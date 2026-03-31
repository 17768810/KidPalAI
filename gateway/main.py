import asyncio
import logging
import os
from dataclasses import dataclass, field
from typing import Any

import httpx

from fastapi import FastAPI, File, HTTPException, Request, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import Response, StreamingResponse
from pydantic import BaseModel

from llm import ask_openclaw, stream_openclaw
from qqbot import QQBotClient, normalize_qq_event
from stt import transcribe_pcm
from tts import synthesize_text, synthesize_pcm

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="KidPalAI Voice Gateway")


class TTSRequest(BaseModel):
    text: str


@dataclass
class QQBridgeSession:
    websocket: WebSocket
    qq_app_id: str
    device_id: str = ""
    qq_app_secret: str = ""
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)


_qq_sessions_lock = asyncio.Lock()
_qq_sessions_by_app_id: dict[str, QQBridgeSession] = {}
_qq_clients_lock = asyncio.Lock()
_qq_clients_by_app_id: dict[str, QQBotClient] = {}


@app.on_event("startup")
async def startup_event():
    asyncio.create_task(_warmup_and_keepalive())


async def _warmup_and_keepalive():
    """Pre-warm LLM session at startup, then keep it warm every 5 minutes.
    Uses max_tokens=1 to minimise token cost."""
    await asyncio.sleep(2)  # let the server finish starting up
    try:
        await ask_openclaw("你好", max_tokens=1)
        logger.info("LLM warmup complete")
    except Exception as e:
        logger.warning(f"LLM warmup failed (non-fatal): {e}")

    while True:
        await asyncio.sleep(300)  # 5 minutes
        try:
            await ask_openclaw("你好", max_tokens=1)
            logger.debug("LLM keepalive OK")
        except Exception as e:
            logger.warning(f"LLM keepalive failed: {e}")


@app.get("/health")
async def health():
    return {"status": "ok"}


def _as_clean_text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.strip()
    return str(value).strip()


def _as_mapping(value: Any) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    return {}


def _configured_bridge_secret() -> str:
    return _as_clean_text(os.getenv("QQ_BRIDGE_SHARED_SECRET"))


def _configured_qq_app_secret(qq_app_id: str) -> str:
    secret = _as_clean_text(os.getenv(f"QQ_APP_SECRET_{qq_app_id}"))
    if secret:
        return secret
    return _as_clean_text(os.getenv("QQ_APP_SECRET"))


def _bridge_registration_allowed(*, qq_app_id: str, qq_app_secret: str) -> bool:
    configured_bridge_secret = _configured_bridge_secret()
    if configured_bridge_secret:
        return qq_app_secret == configured_bridge_secret

    configured_app_secret = _configured_qq_app_secret(qq_app_id)
    if configured_app_secret:
        return qq_app_secret == configured_app_secret

    return True


async def _register_qq_session(
    *,
    websocket: WebSocket,
    qq_app_id: str,
    device_id: str,
    qq_app_secret: str,
) -> None:
    session = QQBridgeSession(
        websocket=websocket,
        qq_app_id=qq_app_id,
        device_id=device_id,
        qq_app_secret=qq_app_secret,
    )
    async with _qq_sessions_lock:
        _qq_sessions_by_app_id[qq_app_id] = session
        if qq_app_id in _qq_clients_by_app_id:
            _qq_clients_by_app_id.pop(qq_app_id, None)


async def _send_bridge_json(*, websocket: WebSocket, payload: dict[str, Any], session: QQBridgeSession | None = None) -> None:
    if session is None:
        await websocket.send_json(payload)
        return
    async with session.send_lock:
        await websocket.send_json(payload)


async def _wait_for_session_release(qq_app_id: str, websocket: WebSocket) -> None:
    existing = await _get_registered_qq_session(qq_app_id)
    if existing is None or existing.websocket is websocket:
        return
    async with existing.send_lock:
        return


async def _run_with_current_session(
    *,
    qq_app_id: str,
    websocket: WebSocket | None,
    action,
    cleanup_current_session_on_error: bool = False,
):
    session = await _get_registered_qq_session(qq_app_id)
    if session is None:
        raise ValueError("qq device session unavailable")
    if websocket is not None and session.websocket is not websocket:
        raise ValueError("qq device session unavailable")

    async with session.send_lock:
        async with _qq_sessions_lock:
            current = _qq_sessions_by_app_id.get(qq_app_id)
            if current is not session:
                raise ValueError("qq device session unavailable")
            if websocket is not None and current.websocket is not websocket:
                raise ValueError("qq device session unavailable")
        try:
            return await action(session)
        except Exception:
            if cleanup_current_session_on_error:
                async with _qq_sessions_lock:
                    current = _qq_sessions_by_app_id.get(qq_app_id)
                    if current is session:
                        _qq_sessions_by_app_id.pop(qq_app_id, None)
                        _qq_clients_by_app_id.pop(qq_app_id, None)
            raise


async def _unregister_qq_session(*, qq_app_id: str, websocket: WebSocket) -> None:
    async with _qq_sessions_lock:
        existing = _qq_sessions_by_app_id.get(qq_app_id)
        if existing and existing.websocket is websocket:
            _qq_sessions_by_app_id.pop(qq_app_id, None)
            _qq_clients_by_app_id.pop(qq_app_id, None)


async def _get_registered_qq_session(qq_app_id: str) -> QQBridgeSession | None:
    async with _qq_sessions_lock:
        return _qq_sessions_by_app_id.get(qq_app_id)


async def _get_current_session_for_websocket(*, qq_app_id: str, websocket: WebSocket) -> QQBridgeSession | None:
    session = await _get_registered_qq_session(qq_app_id)
    if session is None or session.websocket is not websocket:
        return None
    return session


async def _get_single_registered_qq_app_id() -> str:
    async with _qq_sessions_lock:
        if len(_qq_sessions_by_app_id) == 1:
            return next(iter(_qq_sessions_by_app_id.keys()))
    return ""


def _webhook_authorized(request: Request, event: dict[str, Any]) -> bool:
    expected_token = _as_clean_text(os.getenv("QQ_WEBHOOK_TOKEN"))
    if expected_token:
        actual_token = _as_clean_text(
            request.headers.get("authorization")
            or request.headers.get("x-qq-webhook-token")
            or request.headers.get("x-qqbot-webhook-token")
        )
        if actual_token.startswith("Bearer "):
            actual_token = actual_token[7:].strip()
        if actual_token != expected_token:
            return False

    expected_secret = _as_clean_text(os.getenv("QQ_WEBHOOK_SECRET"))
    if expected_secret:
        actual_secret = _as_clean_text(
            request.headers.get("x-signature-ed25519")
            or request.headers.get("x-qq-signature")
            or request.headers.get("x-qqbot-signature")
            or event.get("signature")
        )
        if actual_secret != expected_secret:
            return False

    return True


def _resolve_qq_app_id_from_webhook(event: dict[str, Any], request: Request) -> str:
    header_app_id = (
        _as_clean_text(request.headers.get("x-bot-appid"))
        or _as_clean_text(request.headers.get("x-union-appid"))
        or _as_clean_text(request.headers.get("x-qqbot-appid"))
    )

    payload_app_id = _as_clean_text(event.get("app_id") or event.get("appid"))
    if not payload_app_id:
        data = _as_mapping(event.get("d"))
        payload_app_id = _as_clean_text(data.get("app_id") or data.get("appid"))

    if header_app_id and payload_app_id and header_app_id != payload_app_id:
        raise HTTPException(status_code=401, detail="mismatched qq app id")

    return payload_app_id or header_app_id


def _resolve_qq_app_secret(*, qq_app_id: str, payload: dict[str, Any], session: QQBridgeSession) -> str:
    if session.qq_app_secret:
        return session.qq_app_secret
    return _configured_qq_app_secret(qq_app_id)


async def _get_or_create_qq_client(*, qq_app_id: str, qq_app_secret: str) -> QQBotClient:
    async with _qq_clients_lock:
        client = _qq_clients_by_app_id.get(qq_app_id)
        if client is not None:
            return client
        client = QQBotClient(
            app_id=qq_app_id,
            app_secret=qq_app_secret,
            api_base_url=os.getenv("QQ_API_BASE_URL", "").strip() or "https://api.sgroup.qq.com",
            token_url=os.getenv("QQ_TOKEN_URL", "").strip() or "https://bots.qq.com/app/getAppAccessToken",
        )
        _qq_clients_by_app_id[qq_app_id] = client
        return client


def _extract_outbound_payload(message: dict[str, Any]) -> dict[str, Any]:
    payload = _as_mapping(message.get("payload"))
    if payload:
        return payload
    return message


def _is_outbound_reply_message(message: dict[str, Any]) -> bool:
    msg_type = _as_clean_text(message.get("type")).lower()
    if msg_type in {"outbound_reply", "outbound_message", "reply", "send_reply"}:
        return True
    payload = _extract_outbound_payload(message)
    return "reply_to" in payload and ("content" in payload or "text" in payload)


async def _handle_outbound_reply(*, session: QQBridgeSession, message: dict[str, Any]) -> dict[str, Any]:
    payload = _extract_outbound_payload(message)

    content = _as_clean_text(payload.get("content") or payload.get("text"))
    if not content:
        raise ValueError("outbound content is required")

    reply_to = _as_mapping(payload.get("reply_to"))
    if not reply_to:
        raise ValueError("outbound reply_to is required")

    qq_app_id = session.qq_app_id
    if not qq_app_id:
        raise ValueError("qq_app_id is required")

    qq_app_secret = _resolve_qq_app_secret(qq_app_id=qq_app_id, payload=payload, session=session)
    if not qq_app_secret:
        raise ValueError("qq app secret unavailable")

    client = await _get_or_create_qq_client(qq_app_id=qq_app_id, qq_app_secret=qq_app_secret)
    return await client.send_reply(reply_to=reply_to, content=content)


@app.websocket("/qqbot/ws")
async def qqbot_ws_bridge(websocket: WebSocket):
    await websocket.accept()
    registered_app_id = ""
    device_id = ""
    try:
        while True:
            raw_message = await websocket.receive_json()
            message = _as_mapping(raw_message)
            if not message:
                if registered_app_id:
                    session = await _get_current_session_for_websocket(
                        qq_app_id=registered_app_id,
                        websocket=websocket,
                    )
                    if session is not None:
                        await _send_bridge_json(
                            websocket=websocket,
                            session=session,
                            payload={"type": "error", "detail": "invalid payload"},
                        )
                        continue
                await websocket.send_json({"type": "error", "detail": "invalid payload"})
                continue

            msg_type = _as_clean_text(message.get("type"))

            if msg_type == "register":
                qq_app_id = _as_clean_text(message.get("qq_app_id"))
                if not qq_app_id:
                    if registered_app_id:
                        session = await _get_current_session_for_websocket(
                            qq_app_id=registered_app_id,
                            websocket=websocket,
                        )
                        if session is not None:
                            await _send_bridge_json(
                                websocket=websocket,
                                session=session,
                                payload={"type": "error", "detail": "qq_app_id is required"},
                            )
                            continue
                    await websocket.send_json({"type": "error", "detail": "qq_app_id is required"})
                    continue
                device_id = _as_clean_text(message.get("device_id"))
                qq_app_secret = _as_clean_text(message.get("qq_app_secret"))
                if not _bridge_registration_allowed(qq_app_id=qq_app_id, qq_app_secret=qq_app_secret):
                    if registered_app_id:
                        session = await _get_current_session_for_websocket(
                            qq_app_id=registered_app_id,
                            websocket=websocket,
                        )
                        if session is not None:
                            await _send_bridge_json(
                                websocket=websocket,
                                session=session,
                                payload={"type": "error", "detail": "bridge authentication failed"},
                            )
                            continue
                    await websocket.send_json({"type": "error", "detail": "bridge authentication failed"})
                    continue
                session_app_secret = qq_app_secret
                if _configured_bridge_secret() and qq_app_secret == _configured_bridge_secret():
                    session_app_secret = ""
                if registered_app_id and registered_app_id != qq_app_id:
                    await _unregister_qq_session(qq_app_id=registered_app_id, websocket=websocket)
                await _wait_for_session_release(qq_app_id, websocket)
                await _register_qq_session(
                    websocket=websocket,
                    qq_app_id=qq_app_id,
                    device_id=device_id,
                    qq_app_secret=session_app_secret,
                )
                registered_app_id = qq_app_id
                session = await _get_registered_qq_session(registered_app_id)
                await _send_bridge_json(
                    websocket=websocket,
                    session=session,
                    payload={
                        "type": "registered",
                        "qq_app_id": qq_app_id,
                        "device_id": device_id,
                    },
                )
                continue

            if _is_outbound_reply_message(message):
                if not registered_app_id:
                    await websocket.send_json({"type": "error", "detail": "register first"})
                    continue
                try:
                    async def send_current(session: QQBridgeSession) -> dict[str, Any]:
                        return await _handle_outbound_reply(session=session, message=message)

                    send_result = await _run_with_current_session(
                        qq_app_id=registered_app_id,
                        websocket=websocket,
                        action=send_current,
                    )
                except ValueError as exc:
                    if str(exc) == "qq device session unavailable":
                        registered_app_id = ""
                    active_session = None
                    if registered_app_id:
                        active_session = await _get_current_session_for_websocket(
                            qq_app_id=registered_app_id,
                            websocket=websocket,
                        )
                    if active_session is not None:
                        await _send_bridge_json(
                            websocket=websocket,
                            session=active_session,
                            payload={"type": "error", "detail": str(exc)},
                        )
                    else:
                        await websocket.send_json({"type": "error", "detail": str(exc)})
                    continue
                except httpx.HTTPError as exc:
                    logger.warning("qq outbound http error: %s", exc)
                    session = await _get_current_session_for_websocket(
                        qq_app_id=registered_app_id,
                        websocket=websocket,
                    )
                    if session is None:
                        await websocket.send_json({"type": "error", "detail": "qq outbound send failed"})
                        continue
                    await _send_bridge_json(
                        websocket=websocket,
                        session=session,
                        payload={"type": "error", "detail": "qq outbound send failed"},
                    )
                    continue
                except Exception as exc:
                    logger.error("qq outbound relay error: %s", exc, exc_info=True)
                    session = await _get_current_session_for_websocket(
                        qq_app_id=registered_app_id,
                        websocket=websocket,
                    )
                    if session is None:
                        await websocket.send_json({"type": "error", "detail": "qq outbound send failed"})
                        continue
                    await _send_bridge_json(
                        websocket=websocket,
                        session=session,
                        payload={"type": "error", "detail": "qq outbound send failed"},
                    )
                    continue

                await _send_bridge_json(
                    websocket=websocket,
                    session=session,
                    payload={
                        "type": "outbound_sent",
                        "qq_app_id": registered_app_id,
                        "result": send_result,
                    },
                )
                continue

            if registered_app_id:
                session = await _get_current_session_for_websocket(
                    qq_app_id=registered_app_id,
                    websocket=websocket,
                )
                if session is not None:
                    await _send_bridge_json(
                        websocket=websocket,
                        session=session,
                        payload={"type": "error", "detail": "unsupported message type"},
                    )
                    continue
            await websocket.send_json({"type": "error", "detail": "unsupported message type"})
    except WebSocketDisconnect:
        pass
    finally:
        if registered_app_id:
            await _unregister_qq_session(qq_app_id=registered_app_id, websocket=websocket)


@app.post("/qqbot/webhook")
async def qqbot_webhook(request: Request):
    try:
        event_raw = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="invalid qq webhook payload")

    event = _as_mapping(event_raw)
    if not event:
        raise HTTPException(status_code=400, detail="invalid qq webhook payload")
    if not _webhook_authorized(request, event):
        raise HTTPException(status_code=401, detail="invalid qq webhook signature")

    qq_app_id = _resolve_qq_app_id_from_webhook(event, request)
    if not qq_app_id:
        qq_app_id = await _get_single_registered_qq_app_id()

    if not qq_app_id:
        raise HTTPException(status_code=503, detail="qq device session unavailable")

    normalized = normalize_qq_event(event, bot_app_id=qq_app_id)
    if normalized is None:
        return {"status": "ignored"}

    try:
        async def send_inbound(session: QQBridgeSession) -> None:
            await session.websocket.send_json({"type": "inbound_message", "payload": normalized})

        await _run_with_current_session(
            qq_app_id=qq_app_id,
            websocket=None,
            action=send_inbound,
            cleanup_current_session_on_error=True,
        )
    except Exception:
        raise HTTPException(status_code=503, detail="qq device session unavailable")

    return {"status": "queued"}


@app.post("/stt")
async def stt_endpoint(audio: UploadFile = File(...)):
    """PCM audio → transcribed text. Used by MimiClaw firmware."""
    pcm_data = await audio.read()
    if not pcm_data:
        raise HTTPException(status_code=400, detail="empty audio")
    try:
        text = await transcribe_pcm(pcm_data)
        logger.info(f"STT /stt result: {text!r}")
        if not text.strip():
            raise HTTPException(status_code=422, detail="could not transcribe audio")
        return {"text": text}
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"STT error: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/tts")
async def tts_endpoint(body: TTSRequest):
    """Text → 16kHz/16bit/mono PCM. Used by MimiClaw firmware."""
    if not body.text.strip():
        raise HTTPException(status_code=400, detail="empty text")
    try:
        pcm = await synthesize_pcm(body.text)
        logger.info(f"TTS /tts: {len(body.text)} chars → {len(pcm)} PCM bytes")
        return Response(
            content=pcm,
            media_type="audio/pcm",
            headers={"X-Sample-Rate": "16000", "X-Bits": "16", "X-Channels": "1"},
        )
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"TTS error: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/voice")
async def voice(audio: UploadFile = File(...)):
    """
    Receives raw 16kHz 16-bit mono PCM audio from ESP32-S3.
    Pipeline: PCM -> STT -> OpenClaw LLM -> TTS -> MP3 response
    """
    pcm_data = await audio.read()

    if not pcm_data:
        raise HTTPException(status_code=400, detail="empty audio")

    try:
        logger.info(f"STT: received {len(pcm_data)} bytes of audio")
        text = await transcribe_pcm(pcm_data)
        logger.info(f"STT result: {text!r}")

        if not text.strip():
            raise HTTPException(status_code=422, detail="could not transcribe audio")

        logger.info(f"LLM: asking OpenClaw: {text!r}")
        reply = await ask_openclaw(text)
        logger.info(f"LLM reply: {reply!r}")

        logger.info(f"TTS: synthesizing {len(reply)} chars")
        mp3_data = await synthesize_text(reply)
        logger.info(f"TTS: got {len(mp3_data)} bytes of MP3")

        return Response(content=mp3_data, media_type="audio/mpeg")

    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Pipeline error: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/voice/stream")
async def voice_stream(audio: UploadFile = File(...)):
    """
    Streaming pipeline: PCM -> STT -> LLM (stream) -> per-sentence TTS (PCM) -> chunked PCM
    ESP32 reads chunks directly into I2S, first sound plays ~3s after speech ends.
    PCM contract: 16kHz, 16-bit signed little-endian, mono.
    """
    pcm_data = await audio.read()

    if not pcm_data:
        raise HTTPException(status_code=400, detail="empty audio")

    try:
        logger.info(f"STREAM STT: {len(pcm_data)} bytes")
        text = await transcribe_pcm(pcm_data)
        logger.info(f"STREAM STT result: {text!r}")

        if not text.strip():
            raise HTTPException(status_code=422, detail="could not transcribe audio")

        async def generate():
            sentence_num = 0
            async for sentence in stream_openclaw(text):
                logger.info(f"STREAM TTS sentence {sentence_num}: {sentence!r}")
                try:
                    pcm_bytes = await synthesize_pcm(sentence)
                    logger.info(f"STREAM TTS sentence {sentence_num}: {len(pcm_bytes)} PCM bytes")
                    yield pcm_bytes
                    sentence_num += 1
                except Exception as e:
                    logger.error(f"TTS error for sentence {sentence_num}: {e}")
                    # Skip failed sentence, continue with next

        return StreamingResponse(
            generate(),
            media_type="audio/pcm",
            headers={
                "X-Sample-Rate": "16000",
                "X-Bits": "16",
                "X-Channels": "1",
            },
        )

    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Stream pipeline error: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=str(e))
