import base64
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
