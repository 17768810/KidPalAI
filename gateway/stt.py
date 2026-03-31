import asyncio
import base64
import hashlib
import hmac
import json
import os
import time
from datetime import datetime
from urllib.parse import urlencode
from wsgiref.handlers import format_date_time

import websockets
from dotenv import load_dotenv

load_dotenv()

APP_ID = os.environ.get("XUNFEI_APP_ID", "f6303fce")
API_KEY = os.environ.get("XUNFEI_API_KEY", "40dc57ea0613ac141932102f4850dd58")
API_SECRET = os.environ.get("XUNFEI_API_SECRET", "YmY4ZjJlZTEyMjM0ZTA3MWIwNjAzZDNj")
STT_URL = "wss://iat-api.xfyun.cn/v2/iat"


def _build_auth_url() -> str:
    now = datetime.now()
    date = format_date_time(time.mktime(now.timetuple()))
    signature_origin = f"host: iat-api.xfyun.cn\ndate: {date}\nGET /v2/iat HTTP/1.1"
    signature_sha = hmac.new(
        API_SECRET.encode("utf-8"),
        signature_origin.encode("utf-8"),
        digestmod=hashlib.sha256,
    ).digest()
    signature = base64.b64encode(signature_sha).decode("utf-8")
    auth_origin = (
        f'api_key="{API_KEY}", algorithm="hmac-sha256", '
        f'headers="host date request-line", signature="{signature}"'
    )
    auth = base64.b64encode(auth_origin.encode("utf-8")).decode("utf-8")
    params = {"authorization": auth, "date": date, "host": "iat-api.xfyun.cn"}
    return STT_URL + "?" + urlencode(params)


async def _call_xunfei_api(pcm_data: bytes) -> str:
    url = _build_auth_url()
    result_text = []

    async with websockets.connect(url) as ws:
        chunk_size = 1280  # 40ms at 16kHz 16-bit mono
        frames = [pcm_data[i:i + chunk_size] for i in range(0, len(pcm_data), chunk_size)]

        for idx, frame in enumerate(frames):
            status = 0 if idx == 0 else (2 if idx == len(frames) - 1 else 1)
            payload: dict = {
                "data": {
                    "status": status,
                    "format": "audio/L16;rate=16000",
                    "encoding": "raw",
                    "audio": base64.b64encode(frame).decode("utf-8"),
                },
            }
            if idx == 0:
                payload["common"] = {"app_id": APP_ID}
                payload["business"] = {
                    "language": "zh_cn",
                    "domain": "iat",
                    "accent": "mandarin",
                    "vad_eos": 1000,
                    "dwa": "wpgs",
                }
            await ws.send(json.dumps(payload))
            await asyncio.sleep(0.04)

        # Receive results
        while True:
            response = json.loads(await ws.recv())
            if response.get("data", {}).get("result"):
                for ws_item in response["data"]["result"].get("ws", []):
                    for cw in ws_item.get("cw", []):
                        result_text.append(cw.get("w", ""))
            if response.get("data", {}).get("status") == 2:
                break

    return "".join(result_text)


async def transcribe_pcm(pcm_data: bytes) -> str:
    """Transcribe raw 16kHz 16-bit mono PCM audio to text using 讯飞 STT API."""
    if not pcm_data:
        raise ValueError("empty audio")
    return await _call_xunfei_api(pcm_data)
