# Latency Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce end-to-end perceived latency from ~50s to <5s via LLM session pre-warming and a full streaming PCM pipeline.

**Architecture:** Phase 1 eliminates LLM cold-start (30-40s) by warming the OpenClaw session at startup and sending a `max_tokens=1` keepalive every 5 minutes. Phase 2 replaces the blocking request/response with a chunked streaming pipeline: LLM tokens → sentence-by-sentence TTS (PCM output) → HTTP chunked response → ESP32 I2S write, so the first sound plays ~3s after speech ends.

**Tech Stack:** Python 3.12 / FastAPI / httpx (async streaming) / pytest-asyncio; ESP-IDF v5.x / `esp_http_client` open+read loop / I2S driver

---

## File Map

**Gateway (Phase 1):**
- Modify: `gateway/stt.py` — `vad_eos` 5000 → 1000
- Modify: `gateway/llm.py` — add `max_tokens` param, add `stream_openclaw()` generator (Phase 2)
- Modify: `gateway/main.py` — add `startup_event` warmup/keepalive; add `/voice/stream` (Phase 2)
- Modify: `gateway/tts.py` — add `synthesize_pcm()` (Phase 2)
- Modify: `gateway/Dockerfile` — add `--workers 1` flag (keepalive requires single worker)
- Test: `gateway/tests/test_llm.py`
- Test: `gateway/tests/test_tts.py`
- Test: `gateway/tests/test_voice_endpoint.py`

**Firmware (Phase 2):**
- Modify: `firmware/main/audio_output.c` — change I2S slot to MONO, add `audio_output_write_pcm()`
- Modify: `firmware/main/audio_output.h` — add `audio_output_write_pcm()` declaration
- Modify: `firmware/main/voice_upload.c` — add `voice_upload_stream()` with open/read loop
- Modify: `firmware/main/voice_upload.h` — add `voice_upload_stream()` declaration + callback typedef
- Modify: `firmware/main/main.c` — update state machine to call `voice_upload_stream()` + `pcm_chunk_cb`

---

## Phase 1 — Session Pre-warming & STT Tuning

### Task 1: Reduce STT vad_eos (free 4s win)

**Files:**
- Modify: `gateway/stt.py:66`

- [ ] **Step 1: Write failing test verifying vad_eos value**

In `gateway/tests/test_stt.py`, add:

```python
import json
from unittest.mock import AsyncMock, patch, call
import pytest
from stt import transcribe_pcm


@pytest.mark.asyncio
async def test_vad_eos_is_1000():
    """vad_eos must be 1000ms (not 5000) to avoid 5s extra wait."""
    captured_payloads = []

    async def fake_ws_send(data):
        captured_payloads.append(json.loads(data))

    class FakeWS:
        send = AsyncMock(side_effect=fake_ws_send)
        recv = AsyncMock(return_value=json.dumps({
            "data": {"status": 2, "result": {"ws": []}}
        }))
        async def __aenter__(self): return self
        async def __aexit__(self, *a): pass

    with patch("stt.websockets.connect", return_value=FakeWS()):
        await transcribe_pcm(b'\x00' * 1280)  # single frame

    first_payload = captured_payloads[0]
    assert first_payload["business"]["vad_eos"] == 1000, \
        f"Expected vad_eos=1000, got {first_payload['business']['vad_eos']}"
```

- [ ] **Step 2: Run test to confirm it fails**

```bash
cd gateway && python -m pytest tests/test_stt.py::test_vad_eos_is_1000 -v
```
Expected: `FAILED — AssertionError: Expected vad_eos=1000, got 5000`

- [ ] **Step 3: Change vad_eos in stt.py**

In `gateway/stt.py` line 66, change:
```python
# Before:
"vad_eos": 5000,
# After:
"vad_eos": 1000,
```

- [ ] **Step 4: Run test to confirm pass**

```bash
python -m pytest tests/test_stt.py::test_vad_eos_is_1000 -v
```
Expected: `PASSED`

- [ ] **Step 5: Run full test suite**

```bash
python -m pytest tests/ -v
```
Expected: all existing tests still pass.

- [ ] **Step 6: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add gateway/stt.py gateway/tests/test_stt.py
git commit -m "perf: reduce STT vad_eos 5000→1000ms (saves ~4s per request)"
```

---

### Task 2: Add max_tokens support to LLM client

**Files:**
- Modify: `gateway/llm.py` — add optional `max_tokens` param to `_post_to_openclaw`
- Test: `gateway/tests/test_llm.py`

- [ ] **Step 1: Write failing test**

Add to `gateway/tests/test_llm.py`:

```python
@pytest.mark.asyncio
async def test_max_tokens_passed_to_api():
    """max_tokens param must be forwarded to the OpenClaw payload."""
    import httpx

    captured = {}

    async def fake_post(url, **kwargs):
        captured["payload"] = kwargs.get("json", {})
        mock_resp = AsyncMock()
        mock_resp.raise_for_status = lambda: None
        mock_resp.json.return_value = {
            "choices": [{"message": {"content": "ok"}}]
        }
        return mock_resp

    with patch("llm.httpx.AsyncClient") as MockClient:
        mock_instance = AsyncMock()
        mock_instance.post = fake_post
        mock_instance.__aenter__ = AsyncMock(return_value=mock_instance)
        mock_instance.__aexit__ = AsyncMock(return_value=None)
        MockClient.return_value = mock_instance

        await ask_openclaw("你好", max_tokens=1)

    assert "max_tokens" in captured["payload"]
    assert captured["payload"]["max_tokens"] == 1
```

- [ ] **Step 2: Run test to confirm fails**

```bash
cd gateway && python -m pytest tests/test_llm.py::test_max_tokens_passed_to_api -v
```
Expected: `FAILED` — `ask_openclaw()` doesn't accept `max_tokens`

- [ ] **Step 3: Update llm.py**

Replace `gateway/llm.py` with:

```python
import os
from typing import AsyncGenerator

import httpx
from dotenv import load_dotenv

load_dotenv()

OPENCLAW_URL = os.environ.get("OPENCLAW_WEBCHAT_URL", "http://openclaw:3000/v1/chat/completions")
OPENCLAW_API_KEY = os.getenv("OPENCLAW_API_KEY", "")
OPENCLAW_MODEL = os.getenv("OPENCLAW_MODEL", "agent:main:main")


async def _post_to_openclaw(question: str, max_tokens: int = None) -> str:
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_API_KEY}",
    }
    payload = {
        "model": OPENCLAW_MODEL,
        "messages": [{"role": "user", "content": question}],
        "stream": False,
    }
    if max_tokens is not None:
        payload["max_tokens"] = max_tokens

    async with httpx.AsyncClient(timeout=60.0) as client:
        response = await client.post(OPENCLAW_URL, headers=headers, json=payload)
        response.raise_for_status()
        data = response.json()
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "")
        return data.get("message", data.get("content", str(data)))


async def ask_openclaw(question: str, max_tokens: int = None) -> str:
    """Send transcribed text to OpenClaw and get AI response."""
    if not question:
        raise ValueError("empty question")
    return await _post_to_openclaw(question, max_tokens=max_tokens)
```

- [ ] **Step 4: Run tests**

```bash
python -m pytest tests/test_llm.py -v
```
Expected: all pass

- [ ] **Step 5: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add gateway/llm.py gateway/tests/test_llm.py
git commit -m "feat: add max_tokens param to ask_openclaw (needed for keepalive)"
```

---

### Task 3: Add startup warmup and keepalive to gateway

**Files:**
- Modify: `gateway/main.py`
- Modify: `gateway/Dockerfile` — ensure single worker
- Test: `gateway/tests/test_voice_endpoint.py`

- [ ] **Step 1: Write failing test for startup event**

Add to `gateway/tests/test_voice_endpoint.py`:

```python
import asyncio
from unittest.mock import AsyncMock, patch, MagicMock


def test_startup_event_calls_create_task():
    """startup_event must call asyncio.create_task with a coroutine."""
    # Test only that create_task is called — not the keepalive loop itself
    # (the loop runs forever and cannot be awaited in a unit test).
    from fastapi.testclient import TestClient
    created = []

    def capture_task(coro):
        created.append(getattr(coro, '__name__', repr(coro)))
        coro.close()  # prevent "coroutine was never awaited" RuntimeWarning
        return MagicMock()

    # Patch create_task before importing so the startup handler uses our mock
    with patch("main.asyncio.create_task", side_effect=capture_task):
        from fastapi.testclient import TestClient
        import main as m
        client = TestClient(m.app)
        with client:  # TestClient runs startup events synchronously
            pass

    assert len(created) >= 1, "Expected create_task to be called at startup"
    assert any("warmup" in name or "keepalive" in name for name in created), \
        f"Expected warmup/keepalive task, got: {created}"
```

- [ ] **Step 2: Run to confirm fails**

```bash
cd gateway && python -m pytest tests/test_voice_endpoint.py::test_startup_event_calls_create_task -v
```
Expected: `FAILED — AssertionError: Expected create_task to be called`

- [ ] **Step 3: Update main.py**

Replace `gateway/main.py` with:

```python
import asyncio
import logging

from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.responses import Response, StreamingResponse

from llm import ask_openclaw
from stt import transcribe_pcm
from tts import synthesize_text

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="KidPalAI Voice Gateway")


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
```

- [ ] **Step 4: Run tests**

```bash
python -m pytest tests/ -v
```
Expected: all pass.

- [ ] **Step 5: Add `--workers 1` to Dockerfile CMD**

Edit `gateway/Dockerfile` last line from:
```dockerfile
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
```
to:
```dockerfile
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000", "--workers", "1"]
```

- [ ] **Step 6: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add gateway/main.py gateway/Dockerfile gateway/tests/test_voice_endpoint.py
git commit -m "feat: add LLM session warmup and 5-min keepalive on gateway startup"
```

---

### Task 4: Deploy Phase 1 and measure

- [ ] **Step 1: SSH to server and deploy**

> **Manual step:** Locate the repo on the server first.
> Run `find /home /root /opt -name "docker-compose.yml" 2>/dev/null` to find it.

```bash
ssh root@8.133.3.7
# cd to the repo path found above, e.g.:
cd /root/KidPalAI
git pull
docker compose up -d --build
```

- [ ] **Step 2: Watch warmup logs**

```bash
docker compose logs -f gateway
```
Expected within 3 seconds of startup: `LLM warmup complete`

- [ ] **Step 3: Measure response time from your dev machine**

```bash
# Replace /path/to/test.pcm with any 16kHz mono PCM file (e.g. the demo_1.wav resampled)
time curl -s -o /dev/null -w "%{time_total}" \
  -F "audio=@/path/to/test.pcm;type=application/octet-stream" \
  https://your-domain/voice
```
Expected: **total time < 20s** (down from 50s)

- [ ] **Step 4: Commit timing baseline**

Document measured latency in a comment commit:
```bash
git commit --allow-empty -m "perf: Phase 1 deployed — measured latency ~Xs (was 50s)"
```

---

## Phase 2 — Full Streaming PCM Pipeline

### Task 5: Verify TTS PCM output before writing any firmware

**This is a prerequisite gate — do not proceed to firmware until PCM output is confirmed.**

- [ ] **Step 1: Test fire Volcano Engine PCM output with Python**

Run this script locally (requires `.env` with `VOLC_ACCESS_TOKEN` and `VOLC_APP_ID`):

```python
# scripts/verify_tts_pcm.py
import asyncio, base64, json, os, uuid
import httpx
from dotenv import load_dotenv

load_dotenv("gateway/.env")

async def main():
    headers = {
        "Authorization": f"Bearer;{os.environ['VOLC_ACCESS_TOKEN']}",
        "Content-Type": "application/json",
    }
    payload = {
        "app": {
            "appid": os.environ["VOLC_APP_ID"],
            "token": os.environ["VOLC_ACCESS_TOKEN"],
            "cluster": "volcano_tts",
        },
        "user": {"uid": "test"},
        "audio": {
            "voice_type": os.getenv("VOLC_VOICE_TYPE", "zh_female_vv_uranus_bigtts"),
            "encoding": "pcm",
            "sample_rate": 16000,
            "bits": 16,
            "channel": 1,
        },
        "request": {
            "reqid": str(uuid.uuid4()),
            "text": "你好，我是小书童",
            "text_type": "plain",
            "operation": "query",
        },
    }
    async with httpx.AsyncClient(timeout=15.0) as client:
        resp = await client.post(
            "https://openspeech.bytedance.com/api/v1/tts",
            headers=headers, json=payload)
        resp.raise_for_status()
        data = resp.json()
        pcm = base64.b64decode(data["data"])
        with open("/tmp/test.pcm", "wb") as f:
            f.write(pcm)
        print(f"PCM bytes: {len(pcm)}")
        print(f"Expected duration: {len(pcm)/32000:.2f}s at 16kHz 16bit mono")

asyncio.run(main())
```

```bash
cd c:/WorkGit/KidPalAI
python scripts/verify_tts_pcm.py
```

- [ ] **Step 2: Verify the PCM plays correctly**

On Linux/Mac:
```bash
aplay -r 16000 -f S16_LE -c 1 /tmp/test.pcm
```
On Windows (use Audacity or ffplay):
```bash
ffplay -ar 16000 -ac 1 -f s16le /tmp/test.pcm
```
Expected: clear Chinese speech at normal speed and pitch.

**If audio is distorted (wrong pitch/speed):** the API may be returning a different sample rate. Check `data` response for any `info` field indicating actual format. Adjust `sample_rate` param accordingly and re-test before continuing.

- [ ] **Step 3: Commit the verification script**

```bash
git add scripts/verify_tts_pcm.py
git commit -m "chore: add TTS PCM verification script"
```

---

### Task 6: Add synthesize_pcm() to tts.py

**Files:**
- Modify: `gateway/tts.py`
- Test: `gateway/tests/test_tts.py`

- [ ] **Step 1: Write failing test**

Add to `gateway/tests/test_tts.py`:

```python
import base64


@pytest.mark.asyncio
async def test_synthesize_pcm_returns_raw_bytes():
    """synthesize_pcm must decode base64 and return raw bytes (no header)."""
    fake_pcm = b'\x00\x01' * 50  # 100 bytes of fake 16-bit PCM samples
    fake_b64 = base64.b64encode(fake_pcm).decode()

    with patch("tts._call_volc_api_raw", new_callable=AsyncMock) as mock_api:
        mock_api.return_value = fake_b64
        from tts import synthesize_pcm
        result = await synthesize_pcm("你好")

    assert result == fake_pcm
    # Verify the call included pcm encoding
    call_args = mock_api.call_args
    assert call_args[1].get("encoding") == "pcm" or \
           (len(call_args[0]) > 1 and call_args[0][1] == "pcm")


@pytest.mark.asyncio
async def test_synthesize_pcm_raises_on_empty():
    from tts import synthesize_pcm
    with pytest.raises(ValueError, match="empty text"):
        await synthesize_pcm("")
```

- [ ] **Step 2: Run test to confirm fails**

```bash
cd gateway && python -m pytest tests/test_tts.py::test_synthesize_pcm_returns_raw_bytes -v
```
Expected: `FAILED — ImportError: cannot import name 'synthesize_pcm'`

- [ ] **Step 3: Update tts.py**

Replace `gateway/tts.py` with:

```python
import base64
import json
import os
import uuid

import httpx
from dotenv import load_dotenv

load_dotenv()

VOLC_ACCESS_TOKEN = os.environ["VOLC_ACCESS_TOKEN"]
VOLC_APP_ID = os.environ["VOLC_APP_ID"]
VOICE_TYPE = os.getenv("VOLC_VOICE_TYPE", "zh_female_vv_uranus_bigtts")

TTS_URL = "https://openspeech.bytedance.com/api/v1/tts"


async def _call_volc_api_raw(text: str, encoding: str = "mp3") -> str:
    """Call Volcano TTS API. Returns base64-encoded audio string."""
    headers = {
        "Authorization": f"Bearer;{VOLC_ACCESS_TOKEN}",
        "Content-Type": "application/json",
    }
    audio_cfg = {
        "voice_type": VOICE_TYPE,
        "encoding": encoding,
        "speed_ratio": 1.0,
        "volume_ratio": 1.0,
    }
    if encoding == "pcm":
        audio_cfg["sample_rate"] = 16000
        audio_cfg["bits"] = 16
        audio_cfg["channel"] = 1

    payload = {
        "app": {
            "appid": VOLC_APP_ID,
            "token": VOLC_ACCESS_TOKEN,
            "cluster": "volcano_tts",
        },
        "user": {"uid": "kidpalai"},
        "audio": audio_cfg,
        "request": {
            "reqid": str(uuid.uuid4()),
            "text": text,
            "text_type": "plain",
            "operation": "query",
            "with_frontend": 1,
            "frontend_type": "unitTson",
        },
    }
    async with httpx.AsyncClient(timeout=15.0) as client:
        response = await client.post(TTS_URL, headers=headers, json=payload)
        response.raise_for_status()
        data = response.json()
        return data["data"]


async def _call_volc_api(text: str) -> bytes:
    """Legacy: returns MP3 bytes."""
    b64 = await _call_volc_api_raw(text, encoding="mp3")
    return base64.b64decode(b64)


async def synthesize_text(text: str) -> bytes:
    """Convert text to MP3 audio using 火山引擎 TTS API."""
    if not text:
        raise ValueError("empty text")
    return await _call_volc_api(text)


async def synthesize_pcm(text: str) -> bytes:
    """Convert text to 16kHz 16-bit mono PCM using 火山引擎 TTS API.
    Returns raw PCM bytes, no header. Avoids MP3 decode on ESP32."""
    if not text:
        raise ValueError("empty text")
    b64 = await _call_volc_api_raw(text, encoding="pcm")
    return base64.b64decode(b64)
```

- [ ] **Step 4: Run all TTS tests**

```bash
python -m pytest tests/test_tts.py -v
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add gateway/tts.py gateway/tests/test_tts.py
git commit -m "feat: add synthesize_pcm() — TTS with encoding=pcm, no ffmpeg needed"
```

---

### Task 7: Add stream_openclaw() LLM streaming generator

**Files:**
- Modify: `gateway/llm.py`
- Test: `gateway/tests/test_llm.py`

- [ ] **Step 1: Update imports in test_llm.py**

The new tests use `MagicMock`. Update the import line at the top of `gateway/tests/test_llm.py`:

```python
# Before:
from unittest.mock import AsyncMock, patch
# After:
from unittest.mock import AsyncMock, MagicMock, patch
```

- [ ] **Step 2: Write failing tests**

Add to `gateway/tests/test_llm.py`:

```python
import re


@pytest.mark.asyncio
async def test_stream_openclaw_yields_sentences():
    """stream_openclaw must yield complete sentences split on Chinese punctuation."""
    from llm import stream_openclaw

    sse_lines = [
        'data: {"choices":[{"delta":{"content":"你好"}}]}',
        'data: {"choices":[{"delta":{"content":"，我是"}}]}',
        'data: {"choices":[{"delta":{"content":"小书童。"}}]}',
        'data: {"choices":[{"delta":{"content":"今天天气真好"}}]}',
        'data: {"choices":[{"delta":{"content":"！"}}]}',
        'data: [DONE]',
    ]

    class FakeStreamResponse:
        async def aiter_lines(self):
            for line in sse_lines:
                yield line
        async def __aenter__(self): return self
        async def __aexit__(self, *a): pass
        def raise_for_status(self): pass

    with patch("llm.httpx.AsyncClient") as MockClient:
        mock_instance = AsyncMock()
        mock_instance.stream = MagicMock(return_value=FakeStreamResponse())
        mock_instance.__aenter__ = AsyncMock(return_value=mock_instance)
        mock_instance.__aexit__ = AsyncMock(return_value=None)
        MockClient.return_value = mock_instance

        sentences = []
        async for sentence in stream_openclaw("今天天气怎么样"):
            sentences.append(sentence)

    assert len(sentences) == 2
    assert "小书童" in sentences[0]
    assert "天气" in sentences[1]


@pytest.mark.asyncio
async def test_stream_openclaw_forces_split_at_max_buffer():
    """stream_openclaw must yield when buffer reaches MAX_BUFFER even without punctuation."""
    from llm import stream_openclaw, MAX_BUFFER

    # Generate a long string without punctuation
    long_token = "好" * (MAX_BUFFER + 5)
    sse_lines = [
        f'data: {{"choices":[{{"delta":{{"content":"{long_token}"}}}}]}}',
        'data: [DONE]',
    ]

    class FakeStreamResponse:
        async def aiter_lines(self):
            for line in sse_lines:
                yield line
        async def __aenter__(self): return self
        async def __aexit__(self, *a): pass
        def raise_for_status(self): pass

    with patch("llm.httpx.AsyncClient") as MockClient:
        mock_instance = AsyncMock()
        mock_instance.stream = MagicMock(return_value=FakeStreamResponse())
        mock_instance.__aenter__ = AsyncMock(return_value=mock_instance)
        mock_instance.__aexit__ = AsyncMock(return_value=None)
        MockClient.return_value = mock_instance

        sentences = []
        async for sentence in stream_openclaw("test"):
            sentences.append(sentence)

    # Must yield at least one sentence (forced cut at MAX_BUFFER)
    assert len(sentences) >= 1
    total_chars = sum(len(s) for s in sentences)
    assert total_chars == len(long_token)
```

- [ ] **Step 2: Run tests to confirm they fail**

```bash
cd gateway && python -m pytest tests/test_llm.py::test_stream_openclaw_yields_sentences -v
```
Expected: `FAILED — ImportError: cannot import name 'stream_openclaw'`

- [ ] **Step 3: Add stream_openclaw() to llm.py**

Replace `gateway/llm.py` with the full updated version:

```python
import json
import os
import re
from typing import AsyncGenerator

import httpx
from dotenv import load_dotenv

load_dotenv()

OPENCLAW_URL = os.environ.get("OPENCLAW_WEBCHAT_URL", "http://openclaw:3000/v1/chat/completions")
OPENCLAW_API_KEY = os.getenv("OPENCLAW_API_KEY", "")
OPENCLAW_MODEL = os.getenv("OPENCLAW_MODEL", "agent:main:main")

# Sentence boundary: split on Chinese/English terminal punctuation
SENTENCE_END = re.compile(r"[。！？…\n]")
# Force-split when buffer reaches this many characters (no punctuation in LLM output)
MAX_BUFFER = 60
# Minimum sentence length before dispatching to TTS
MIN_SENTENCE = 5


async def _post_to_openclaw(question: str, max_tokens: int = None) -> str:
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_API_KEY}",
    }
    payload = {
        "model": OPENCLAW_MODEL,
        "messages": [{"role": "user", "content": question}],
        "stream": False,
    }
    if max_tokens is not None:
        payload["max_tokens"] = max_tokens

    async with httpx.AsyncClient(timeout=60.0) as client:
        response = await client.post(OPENCLAW_URL, headers=headers, json=payload)
        response.raise_for_status()
        data = response.json()
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "")
        return data.get("message", data.get("content", str(data)))


async def ask_openclaw(question: str, max_tokens: int = None) -> str:
    """Send transcribed text to OpenClaw and get AI response."""
    if not question:
        raise ValueError("empty question")
    return await _post_to_openclaw(question, max_tokens=max_tokens)


async def stream_openclaw(text: str) -> AsyncGenerator[str, None]:
    """Stream LLM response, yielding one sentence at a time.

    Splits on Chinese/English terminal punctuation (。！？…\\n).
    Forces a split when the buffer reaches MAX_BUFFER characters
    to handle LLM output with no punctuation.
    Skips sentences shorter than MIN_SENTENCE characters.
    """
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_API_KEY}",
    }
    payload = {
        "model": OPENCLAW_MODEL,
        "messages": [{"role": "user", "content": text}],
        "stream": True,
    }

    buffer = ""

    async with httpx.AsyncClient(timeout=None) as client:
        async with client.stream("POST", OPENCLAW_URL, headers=headers, json=payload) as resp:
            resp.raise_for_status()
            async for line in resp.aiter_lines():
                if not line.startswith("data: "):
                    continue
                payload_str = line[6:].strip()
                if payload_str == "[DONE]":
                    break
                try:
                    delta_data = json.loads(payload_str)
                    token = (delta_data.get("choices", [{}])[0]
                             .get("delta", {})
                             .get("content", ""))
                except (json.JSONDecodeError, IndexError):
                    continue

                if not token:
                    continue

                buffer += token

                # Yield on sentence boundary or buffer overflow
                while SENTENCE_END.search(buffer) or len(buffer) >= MAX_BUFFER:
                    match = SENTENCE_END.search(buffer)
                    if match:
                        cut = match.end()
                    else:
                        cut = MAX_BUFFER
                    sentence = buffer[:cut].strip()
                    buffer = buffer[cut:]
                    if len(sentence) >= MIN_SENTENCE:
                        yield sentence

    # Yield any remaining text
    if buffer.strip() and len(buffer.strip()) >= MIN_SENTENCE:
        yield buffer.strip()
```

- [ ] **Step 4: Run all LLM tests**

```bash
python -m pytest tests/test_llm.py -v
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add gateway/llm.py gateway/tests/test_llm.py
git commit -m "feat: add stream_openclaw() — LLM streaming with per-sentence yield"
```

---

### Task 8: Add /voice/stream endpoint to gateway

**Files:**
- Modify: `gateway/main.py`
- Test: `gateway/tests/test_voice_endpoint.py`

- [ ] **Step 1: Write failing test**

Add to `gateway/tests/test_voice_endpoint.py`:

```python
@pytest.mark.asyncio
async def test_voice_stream_returns_chunked_pcm():
    """/voice/stream must return streaming PCM bytes in chunks."""
    from httpx import AsyncClient, ASGITransport
    from main import app

    fake_pcm_chunk1 = b'\x00\x01' * 320  # 640 bytes = 20ms
    fake_pcm_chunk2 = b'\x02\x03' * 320

    async def fake_stream_llm(text):
        yield "你好！"
        yield "今天天气不错。"

    with patch("main.transcribe_pcm", new_callable=AsyncMock) as mock_stt, \
         patch("main.stream_openclaw") as mock_stream, \
         patch("main.synthesize_pcm", new_callable=AsyncMock) as mock_tts:

        mock_stt.return_value = "今天天气怎么样"
        mock_stream.return_value = fake_stream_llm("今天天气怎么样")
        mock_tts.side_effect = [fake_pcm_chunk1, fake_pcm_chunk2]

        fake_pcm = b'\x00' * 32000
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as ac:
            async with ac.stream(
                "POST", "/voice/stream",
                files={"audio": ("audio.pcm", fake_pcm, "application/octet-stream")}
            ) as response:
                assert response.status_code == 200
                assert "audio/pcm" in response.headers.get("content-type", "")
                chunks = []
                async for chunk in response.aiter_bytes():
                    chunks.append(chunk)

        total = b"".join(chunks)
        assert total == fake_pcm_chunk1 + fake_pcm_chunk2
```

- [ ] **Step 2: Run test to confirm fails**

```bash
cd gateway && python -m pytest tests/test_voice_endpoint.py::test_voice_stream_returns_chunked_pcm -v
```
Expected: `FAILED — 404 Not Found`

- [ ] **Step 3: Update imports in main.py**

`main.py` currently imports only `ask_openclaw` and `synthesize_text`. Update both import lines:

```python
# In gateway/main.py, replace:
from llm import ask_openclaw
from tts import synthesize_text
# With:
from llm import ask_openclaw, stream_openclaw
from tts import synthesize_text, synthesize_pcm
```

- [ ] **Step 4: Add /voice/stream endpoint to main.py**

Append the new endpoint after the existing `/voice` endpoint in `gateway/main.py`:

```python
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
```

- [ ] **Step 5: Run all gateway tests**

```bash
python -m pytest tests/ -v
```
Expected: all pass.

- [ ] **Step 6: Deploy updated gateway**

```bash
ssh root@8.133.3.7
cd <repo-path-on-server>   # same path found in Task 4 Step 1
git pull
docker compose up -d --build
```

- [ ] **Step 7: Smoke test /voice/stream from dev machine**

```python
# scripts/test_stream_endpoint.py
import httpx, time, sys

url = "https://your-domain/voice/stream"
pcm_file = sys.argv[1]  # path to a 16kHz PCM file

with open(pcm_file, "rb") as f:
    pcm = f.read()

start = time.time()
first_chunk_time = None
total_bytes = 0

with httpx.stream("POST", url,
                  files={"audio": ("audio.pcm", pcm, "application/octet-stream")},
                  timeout=60) as resp:
    print(f"Status: {resp.status_code}")
    for chunk in resp.iter_bytes(chunk_size=640):
        if first_chunk_time is None:
            first_chunk_time = time.time() - start
            print(f"First PCM chunk received: {first_chunk_time:.2f}s")
        total_bytes += len(chunk)

print(f"Total PCM bytes: {total_bytes}")
print(f"Total time: {time.time()-start:.2f}s")
print(f"Audio duration: {total_bytes/32000:.2f}s at 16kHz 16bit mono")
```

```bash
python scripts/test_stream_endpoint.py /path/to/test.pcm
```
Expected: first chunk within 8-12s, audio plays back correctly.

- [ ] **Step 8: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add gateway/main.py gateway/tests/test_voice_endpoint.py scripts/test_stream_endpoint.py
git commit -m "feat: add /voice/stream endpoint — chunked PCM streaming pipeline"
```

---

### Task 9: ESP32 — fix I2S output to MONO + add audio_output_write_pcm()

**Files:**
- Modify: `firmware/main/audio_output.c`
- Modify: `firmware/main/audio_output.h`

- [ ] **Step 1: Update audio_output.h to add new function signature**

Replace `firmware/main/audio_output.h` with:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t audio_output_init(void);

// Legacy stub — kept for reference, not used in streaming mode
esp_err_t audio_output_play_mp3(const uint8_t *mp3_data, size_t mp3_len);

// Write raw 16kHz 16-bit mono PCM samples directly to I2S output.
// Called repeatedly as PCM chunks arrive from the gateway.
esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len);
```

- [ ] **Step 2: Update audio_output.c**

Replace `firmware/main/audio_output.c` with:

```c
#include "audio_output.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "audio_output";

// MAX98357A wiring (adjust GPIO to match your board)
#define I2S_SPK_BCLK  GPIO_NUM_6
#define I2S_SPK_LRC   GPIO_NUM_5
#define I2S_SPK_DIN   GPIO_NUM_4

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t audio_output_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        // 16kHz matches the PCM contract from the gateway TTS
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        // MONO: gateway sends 16-bit mono PCM; STEREO would halve the pitch
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCLK,
            .ws   = I2S_SPK_LRC,
            .dout = I2S_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S speaker initialized (16kHz MONO)");
    return ESP_OK;
}

esp_err_t audio_output_play_mp3(const uint8_t *mp3_data, size_t mp3_len)
{
    // Not used in streaming mode. Kept as stub.
    ESP_LOGW(TAG, "play_mp3 stub called (%d bytes) — use audio_output_write_pcm instead", mp3_len);
    return ESP_OK;
}

esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    size_t written = 0;
    esp_err_t err = i2s_channel_write(tx_handle, data, len, &written, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write error: %d", err);
    }
    return err;
}
```

- [ ] **Step 3: Build firmware to check for compile errors**

```bash
cd c:/WorkGit/KidPalAI/firmware
idf.py build 2>&1 | tail -20
```
Expected: `Build complete.` (no errors)

- [ ] **Step 4: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add firmware/main/audio_output.c firmware/main/audio_output.h
git commit -m "fix: I2S output STEREO→MONO, add audio_output_write_pcm() for PCM streaming"
```

---

### Task 10: ESP32 — add voice_upload_stream()

**Files:**
- Modify: `firmware/main/voice_upload.h`
- Modify: `firmware/main/voice_upload.c`

- [ ] **Step 1: Update voice_upload.h**

Replace `firmware/main/voice_upload.h` with:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Callback invoked for each PCM chunk received from the gateway.
// data: pointer to chunk bytes (valid only during callback)
// len:  number of bytes in this chunk
typedef void (*voice_pcm_callback_t)(const uint8_t *data, int len);

// Original blocking upload — returns heap-allocated MP3. Caller must free mp3_out.
esp_err_t voice_upload(const char *url,
                       const uint8_t *pcm_data, size_t pcm_len,
                       uint8_t **mp3_out, size_t *mp3_len);

// Streaming upload to /voice/stream endpoint.
// Sends PCM, receives chunked PCM response, calls on_chunk for each received chunk.
// Blocks until the full response is received or an error occurs.
// timeout_ms: watchdog — aborts if no data received for this many ms (0 = disable)
esp_err_t voice_upload_stream(const char *base_url,
                               const uint8_t *pcm_data, size_t pcm_len,
                               voice_pcm_callback_t on_chunk,
                               int watchdog_ms);
```

- [ ] **Step 2: Add #include "esp_timer.h" to the top of voice_upload.c**

In `firmware/main/voice_upload.c`, add to the existing `#include` block at the top of the file (after the other includes):

```c
#include "esp_timer.h"  // for esp_timer_get_time() — used by stream watchdog
```

- [ ] **Step 3: Append voice_upload_stream() to voice_upload.c**

Append the following to the end of `firmware/main/voice_upload.c` (after the existing `voice_upload` function):

```c
esp_err_t voice_upload_stream(const char *base_url,
                               const uint8_t *pcm_data, size_t pcm_len,
                               voice_pcm_callback_t on_chunk,
                               int watchdog_ms)
{
    // Build /voice/stream URL from base_url
    char url[256];
    // Remove trailing slash if present, append /voice/stream
    size_t base_len = strlen(base_url);
    if (base_len > 0 && base_url[base_len - 1] == '/') {
        snprintf(url, sizeof(url), "%.*svoice/stream", (int)(base_len - 1), base_url);
    } else {
        snprintf(url, sizeof(url), "%s/voice/stream", base_url);
    }

    // Build multipart body (same as voice_upload)
    const char *boundary = "kidpalai_boundary_esp32";
    char header[256];
    snprintf(header, sizeof(header),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n",
             boundary);
    char footer[64];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t hlen = strlen(header), flen = strlen(footer);
    size_t body_len = hlen + pcm_len + flen;

    uint8_t *body = malloc(body_len);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body, header, hlen);
    memcpy(body + hlen, pcm_data, pcm_len);
    memcpy(body + hlen + pcm_len, footer, flen);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    // Open connection (no timeout: streaming response can take 15-30s)
    esp_http_client_config_t config = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 0,       // disable read timeout — stream is long-lived
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_open failed: %d", err);
        free(body);
        esp_http_client_cleanup(client);
        return err;
    }

    // Write request body
    int written = esp_http_client_write(client, (const char *)body, (int)body_len);
    free(body);
    if (written < 0) {
        ESP_LOGE(TAG, "http_write failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Read response headers (blocks until server sends them back)
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "stream endpoint returned HTTP %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Stream response: read PCM chunks and pass to callback
#define STREAM_CHUNK 640  // 20ms @ 16kHz 16-bit mono
    uint8_t chunk[STREAM_CHUNK];
    int64_t last_data_us = esp_timer_get_time();
    int total_bytes = 0;
    int bytes_read;

    while (1) {
        bytes_read = esp_http_client_read(client, (char *)chunk, STREAM_CHUNK);

        if (bytes_read > 0) {
            last_data_us = esp_timer_get_time();
            total_bytes += bytes_read;
            on_chunk(chunk, bytes_read);
        } else if (bytes_read == 0) {
            // Connection closed cleanly — stream ended
            break;
        } else {
            // bytes_read < 0 means error
            ESP_LOGE(TAG, "read error: %d", bytes_read);
            err = ESP_FAIL;
            break;
        }

        // Watchdog: abort if no data for watchdog_ms
        if (watchdog_ms > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - last_data_us) / 1000;
            if (elapsed_ms > watchdog_ms) {
                ESP_LOGW(TAG, "stream watchdog timeout (%d ms)", watchdog_ms);
                err = ESP_ERR_TIMEOUT;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "stream complete: %d PCM bytes received", total_bytes);
    esp_http_client_cleanup(client);
    return err;
}
```

- [ ] **Step 4: Build firmware**

```bash
cd c:/WorkGit/KidPalAI/firmware
idf.py build 2>&1 | tail -20
```
Expected: `Build complete.`

- [ ] **Step 5: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add firmware/main/voice_upload.c firmware/main/voice_upload.h
git commit -m "feat: add voice_upload_stream() — open/read loop with PCM chunk callback"
```

---

### Task 11: ESP32 — update main.c state machine

**Files:**
- Modify: `firmware/main/main.c`

- [ ] **Step 1: Add pcm_chunk_cb and update the UPLOAD/PLAY section**

In `firmware/main/main.c`, make these targeted changes:

**After the `#include "voice_upload.h"` line, add:**
```c
// PCM chunk callback: write each received chunk directly to I2S speaker
static void pcm_chunk_cb(const uint8_t *data, int len)
{
    audio_output_write_pcm(data, len);
}
```

**Replace the entire UPLOAD + PLAY block.** Find it by searching for `// ── UPLOAD ─` in `firmware/main/main.c`. Replace from that comment through the closing `led_set_state(LED_STATE_IDLE)` line with:

```c
        // ── UPLOAD + STREAM PLAY ─────────────────────────────────────────────
        led_set_state(LED_STATE_WAITING);
        ESP_LOGI(TAG, "Streaming to gateway...");

        // Build /voice/stream URL: replace /voice suffix if present, or just append
        char stream_url[256];
        snprintf(stream_url, sizeof(stream_url), "%s", cfg.gateway_url);

        esp_err_t err = voice_upload_stream(
            stream_url,
            (uint8_t *)record_buf,
            (size_t)(total_samples * sizeof(int16_t)),
            pcm_chunk_cb,
            30000);  // 30s watchdog: abort if no data for 30s

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Stream complete");
        } else if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Stream watchdog triggered");
            led_set_state(LED_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGE(TAG, "Stream failed: %d", err);
            led_set_state(LED_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
```

Note: Remove the `uint8_t *mp3_data = NULL; size_t mp3_len = 0;` variable declarations that are now unused. The LED state transitions to `LED_STATE_PLAYING` can be set inside `pcm_chunk_cb` on first call if desired, or simply left as `LED_STATE_WAITING` during streaming (acceptable for now).

- [ ] **Step 2: Build firmware**

```bash
cd c:/WorkGit/KidPalAI/firmware
idf.py build 2>&1 | tail -30
```
Expected: `Build complete.` with no errors or new warnings.

- [ ] **Step 3: Flash and test**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Trigger a voice interaction. Expected log sequence:
```
Wake detected! Recording...
Silence detected, stopping recording
Recorded N samples (2.3s), uploading...
Streaming to gateway...
stream complete: XXXXX PCM bytes received
Ready. Listening again...
```
And you should hear the response playing from the speaker during streaming.

- [ ] **Step 4: Commit**

```bash
cd c:/WorkGit/KidPalAI
git add firmware/main/main.c
git commit -m "feat: wire voice_upload_stream into main state machine — streaming PCM playback"
```

---

### Task 12: End-to-end latency measurement

- [ ] **Step 1: Measure time-to-first-sound**

With firmware flashed and gateway deployed:
1. Trigger a voice interaction
2. Note the time from "Silence detected, stopping recording" to first audio from speaker
3. Compare against Phase 1 baseline

Expected: **< 5s** from end of speech to first sound.

- [ ] **Step 2: Document results**

```bash
git commit --allow-empty -m "perf: Phase 2 complete — measured perceived latency ~Xs (target <5s)"
```

- [ ] **Step 3: Verify fallback still works**

Test the original `/voice` endpoint still returns MP3:
```bash
curl -s -o /dev/null -w "%{http_code} %{size_download}b\n" \
  -F "audio=@/path/to/test.pcm;type=application/octet-stream" \
  https://your-domain/voice
```
Expected: `200 NNNNb`

---

## Summary of All Changed Files

| File | Change |
|------|--------|
| `gateway/stt.py` | `vad_eos` 5000 → 1000 |
| `gateway/llm.py` | `max_tokens` param; `stream_openclaw()` with SSE + sentence split |
| `gateway/tts.py` | `synthesize_pcm()` via `encoding=pcm`; `_call_volc_api_raw()` |
| `gateway/main.py` | Startup warmup/keepalive; `/voice/stream` StreamingResponse |
| `gateway/Dockerfile` | `--workers 1` |
| `firmware/main/audio_output.c` | `STEREO → MONO`; `audio_output_write_pcm()` |
| `firmware/main/audio_output.h` | Add `audio_output_write_pcm()` declaration |
| `firmware/main/voice_upload.c` | Add `voice_upload_stream()` open/read loop |
| `firmware/main/voice_upload.h` | Add `voice_pcm_callback_t` typedef; `voice_upload_stream()` |
| `firmware/main/main.c` | `pcm_chunk_cb`; state machine uses `voice_upload_stream` |
| `gateway/tests/test_stt.py` | `test_vad_eos_is_1000` |
| `gateway/tests/test_llm.py` | `test_max_tokens_passed_to_api`, stream tests |
| `gateway/tests/test_tts.py` | `test_synthesize_pcm_*` |
| `gateway/tests/test_voice_endpoint.py` | `test_voice_stream_returns_chunked_pcm` |
| `scripts/verify_tts_pcm.py` | TTS PCM verification (prerequisite gate) |
| `scripts/test_stream_endpoint.py` | Streaming latency measurement |
