# KidPalAI 小书童 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build an ESP32-S3 AI reading companion for kids that wakes on voice, sends audio to a cloud server, and plays back AI-generated responses.

**Architecture:** ESP32-S3 handles wake word detection (ESP-SR) and audio I/O. A Python FastAPI gateway on the cloud server orchestrates STT (讯飞) → OpenClaw LLM → TTS (火山引擎) and returns MP3 to the device. OpenClaw manages the AI persona and persistent child profile.

**Tech Stack:** ESP-IDF + ESP-SR + ESP-ADF (C), Python FastAPI, Docker Compose, Nginx, 讯飞 STT API, 火山引擎 TTS API, OpenClaw

---

## Phase 1: Cloud Server — Infrastructure Setup

### Task 1: Project Scaffold & Docker Compose

**Files:**
- Create: `docker-compose.yml`
- Create: `nginx/nginx.conf`
- Create: `.env.example`
- Create: `gateway/.env` (gitignored)

**Step 1: Create project directory structure**

```bash
mkdir -p gateway nginx openclaw-data
touch .gitignore .env.example
```

**Step 2: Write `.gitignore`**

```
.env
gateway/.env
openclaw-data/
*.pyc
__pycache__/
```

**Step 3: Write `.env.example`**

```env
# 讯飞 STT
XUNFEI_APP_ID=your_app_id
XUNFEI_API_KEY=your_api_key
XUNFEI_API_SECRET=your_api_secret

# 火山引擎 TTS
VOLC_ACCESS_KEY=your_access_key
VOLC_SECRET_KEY=your_secret_key
VOLC_APP_ID=your_app_id
VOLC_VOICE_TYPE=zh_female_xiaomei_moon_bigtts

# OpenClaw WebChat
OPENCLAW_WEBCHAT_URL=http://openclaw:3000/api/chat
OPENCLAW_API_KEY=your_openclaw_key

# Server
GATEWAY_PORT=8000
DOMAIN=yourdomain.com
```

**Step 4: Write `docker-compose.yml`**

```yaml
version: '3.9'

services:
  openclaw:
    image: ghcr.io/openclaw/openclaw:latest
    container_name: openclaw
    restart: unless-stopped
    volumes:
      - ./openclaw-data:/app/data
    environment:
      - NODE_ENV=production
    networks:
      - kidpal-net

  gateway:
    build: ./gateway
    container_name: voice-gateway
    restart: unless-stopped
    env_file: ./gateway/.env
    ports:
      - "8000:8000"
    depends_on:
      - openclaw
    networks:
      - kidpal-net

  nginx:
    image: nginx:alpine
    container_name: nginx
    restart: unless-stopped
    ports:
      - "80:80"
      - "443:443"
    volumes:
      - ./nginx/nginx.conf:/etc/nginx/nginx.conf:ro
      - /etc/letsencrypt:/etc/letsencrypt:ro
    depends_on:
      - gateway
    networks:
      - kidpal-net

networks:
  kidpal-net:
    driver: bridge
```

**Step 5: Write `nginx/nginx.conf`**

```nginx
events { worker_connections 1024; }

http {
    server {
        listen 80;
        server_name _;
        return 301 https://$host$request_uri;
    }

    server {
        listen 443 ssl;
        server_name yourdomain.com;

        ssl_certificate /etc/letsencrypt/live/yourdomain.com/fullchain.pem;
        ssl_certificate_key /etc/letsencrypt/live/yourdomain.com/privkey.pem;

        location /voice {
            proxy_pass http://gateway:8000;
            proxy_read_timeout 30s;
            client_max_body_size 2M;
        }

        location /health {
            proxy_pass http://gateway:8000;
        }
    }
}
```

**Step 6: Commit**

```bash
git add docker-compose.yml nginx/ .gitignore .env.example
git commit -m "chore: add docker compose infrastructure scaffold"
```

---

### Task 2: Python Gateway — Project Setup

**Files:**
- Create: `gateway/Dockerfile`
- Create: `gateway/requirements.txt`
- Create: `gateway/main.py`

**Step 1: Write `gateway/requirements.txt`**

```
fastapi==0.115.0
uvicorn[standard]==0.30.0
httpx==0.27.0
python-dotenv==1.0.0
websockets==12.0
pydantic==2.7.0
```

**Step 2: Write `gateway/Dockerfile`**

```dockerfile
FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
```

**Step 3: Write `gateway/main.py` skeleton**

```python
from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.responses import Response
import logging

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="KidPalAI Voice Gateway")


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.post("/voice")
async def voice(audio: UploadFile = File(...)):
    """
    Receives raw PCM audio from ESP32-S3.
    Pipeline: PCM → STT → OpenClaw LLM → TTS → MP3 response
    """
    raise NotImplementedError("voice pipeline not implemented yet")
```

**Step 4: Verify it starts**

```bash
cd gateway
pip install -r requirements.txt
uvicorn main:app --reload
# Expected: Uvicorn running on http://127.0.0.1:8000
# curl http://localhost:8000/health → {"status":"ok"}
```

**Step 5: Commit**

```bash
git add gateway/
git commit -m "feat: add voice gateway python project scaffold"
```

---

## Phase 2: Cloud Server — STT Integration (讯飞)

### Task 3: 讯飞 STT Client

讯飞 WebSocket STT API docs: https://www.xfyun.cn/doc/asr/voicedictation/API.html

**Files:**
- Create: `gateway/stt.py`
- Create: `gateway/tests/test_stt.py`

**Step 1: Write `gateway/tests/test_stt.py`**

```python
import pytest
from unittest.mock import AsyncMock, patch
from stt import transcribe_pcm


@pytest.mark.asyncio
async def test_transcribe_returns_string():
    """transcribe_pcm should return a non-empty string for valid audio."""
    fake_pcm = b'\x00' * 3200  # 100ms of 16kHz 16-bit mono silence
    with patch("stt._call_xunfei_api", new_callable=AsyncMock) as mock_api:
        mock_api.return_value = "你好书童"
        result = await transcribe_pcm(fake_pcm)
    assert isinstance(result, str)
    assert result == "你好书童"


@pytest.mark.asyncio
async def test_transcribe_raises_on_empty_audio():
    with pytest.raises(ValueError, match="empty audio"):
        await transcribe_pcm(b"")
```

**Step 2: Run test to verify it fails**

```bash
cd gateway
pytest tests/test_stt.py -v
# Expected: FAIL - ModuleNotFoundError: No module named 'stt'
```

**Step 3: Write `gateway/stt.py`**

```python
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

APP_ID = os.environ["XUNFEI_APP_ID"]
API_KEY = os.environ["XUNFEI_API_KEY"]
API_SECRET = os.environ["XUNFEI_API_SECRET"]
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
        # Send first frame with params
        chunk_size = 1280  # 40ms at 16kHz 16-bit mono
        frames = [pcm_data[i:i + chunk_size] for i in range(0, len(pcm_data), chunk_size)]

        for idx, frame in enumerate(frames):
            status = 0 if idx == 0 else (2 if idx == len(frames) - 1 else 1)
            payload = {
                "common": {"app_id": APP_ID} if idx == 0 else {},
                "business": {
                    "language": "zh_cn",
                    "domain": "iat",
                    "accent": "mandarin",
                    "vad_eos": 5000,
                    "dwa": "wpgs",
                } if idx == 0 else {},
                "data": {
                    "status": status,
                    "format": "audio/L16;rate=16000",
                    "encoding": "raw",
                    "audio": base64.b64encode(frame).decode("utf-8"),
                },
            }
            # Remove empty dicts
            payload = {k: v for k, v in payload.items() if v}
            await ws.send(json.dumps(payload))
            await asyncio.sleep(0.04)  # ~40ms between frames

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
```

**Step 4: Run test to verify it passes**

```bash
pytest tests/test_stt.py -v
# Expected: PASS (mocked API call)
```

**Step 5: Commit**

```bash
git add gateway/stt.py gateway/tests/
git commit -m "feat: add 讯飞 STT client with mock tests"
```

---

## Phase 3: Cloud Server — TTS Integration (火山引擎)

### Task 4: 火山引擎 TTS Client

火山引擎 TTS API docs: https://www.volcengine.com/docs/6561/79817

**Files:**
- Create: `gateway/tts.py`
- Create: `gateway/tests/test_tts.py`

**Step 1: Write `gateway/tests/test_tts.py`**

```python
import pytest
from unittest.mock import AsyncMock, patch
from tts import synthesize_text


@pytest.mark.asyncio
async def test_synthesize_returns_mp3_bytes():
    fake_mp3 = b'\xff\xfb' + b'\x00' * 100  # fake MP3 header
    with patch("tts._call_volc_api", new_callable=AsyncMock) as mock_api:
        mock_api.return_value = fake_mp3
        result = await synthesize_text("你好，我是书童")
    assert isinstance(result, bytes)
    assert len(result) > 0


@pytest.mark.asyncio
async def test_synthesize_raises_on_empty_text():
    with pytest.raises(ValueError, match="empty text"):
        await synthesize_text("")
```

**Step 2: Run test to verify it fails**

```bash
pytest tests/test_tts.py -v
# Expected: FAIL - ModuleNotFoundError: No module named 'tts'
```

**Step 3: Write `gateway/tts.py`**

```python
import base64
import json
import os
import uuid

import httpx
from dotenv import load_dotenv

load_dotenv()

VOLC_ACCESS_KEY = os.environ["VOLC_ACCESS_KEY"]
VOLC_SECRET_KEY = os.environ["VOLC_SECRET_KEY"]
VOLC_APP_ID = os.environ["VOLC_APP_ID"]
VOICE_TYPE = os.getenv("VOLC_VOICE_TYPE", "zh_female_xiaomei_moon_bigtts")

TTS_URL = "https://openspeech.bytedance.com/api/v1/tts"


async def _call_volc_api(text: str) -> bytes:
    headers = {
        "Authorization": f"Bearer;{VOLC_ACCESS_KEY}",
        "Content-Type": "application/json",
    }
    payload = {
        "app": {
            "appid": VOLC_APP_ID,
            "token": VOLC_ACCESS_KEY,
            "cluster": "volcano_tts",
        },
        "user": {"uid": "kidpalai"},
        "audio": {
            "voice_type": VOICE_TYPE,
            "encoding": "mp3",
            "speed_ratio": 1.0,
            "volume_ratio": 1.0,
        },
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
        audio_b64 = data["data"]
        return base64.b64decode(audio_b64)


async def synthesize_text(text: str) -> bytes:
    """Convert text to MP3 audio using 火山引擎 TTS API."""
    if not text:
        raise ValueError("empty text")
    return await _call_volc_api(text)
```

**Step 4: Run test to verify it passes**

```bash
pytest tests/test_tts.py -v
# Expected: PASS
```

**Step 5: Commit**

```bash
git add gateway/tts.py gateway/tests/test_tts.py
git commit -m "feat: add 火山引擎 TTS client with mock tests"
```

---

## Phase 4: Cloud Server — OpenClaw LLM Integration

### Task 5: OpenClaw WebChat Client

**Files:**
- Create: `gateway/llm.py`
- Create: `gateway/tests/test_llm.py`
- Create: `openclaw-data/memory/child_profile.md`

**Step 1: Write `openclaw-data/memory/child_profile.md`**

```markdown
## 小书童设定

- 角色：温柔耐心的AI学习伙伴，名叫"书童"
- 当前用户：小明，8岁，小学二年级
- 主要科目：语文、数学、英语
- 语言风格：简单活泼，多鼓励，避免批评，每次回答不超过3句话
- 每天提醒：18:00 做作业，20:30 准备睡觉
- 禁止话题：暴力、恐怖、成人内容
```

**Step 2: Write `gateway/tests/test_llm.py`**

```python
import pytest
from unittest.mock import AsyncMock, patch
from llm import ask_openclaw


@pytest.mark.asyncio
async def test_ask_returns_string():
    with patch("llm._post_to_openclaw", new_callable=AsyncMock) as mock_post:
        mock_post.return_value = "太棒了！你问了一个很好的问题！"
        result = await ask_openclaw("1加1等于几？")
    assert isinstance(result, str)
    assert len(result) > 0


@pytest.mark.asyncio
async def test_ask_raises_on_empty_question():
    with pytest.raises(ValueError, match="empty question"):
        await ask_openclaw("")
```

**Step 3: Run test to verify it fails**

```bash
pytest tests/test_llm.py -v
# Expected: FAIL - ModuleNotFoundError: No module named 'llm'
```

**Step 4: Write `gateway/llm.py`**

```python
import os

import httpx
from dotenv import load_dotenv

load_dotenv()

OPENCLAW_URL = os.environ["OPENCLAW_WEBCHAT_URL"]
OPENCLAW_API_KEY = os.getenv("OPENCLAW_API_KEY", "")


async def _post_to_openclaw(question: str) -> str:
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_API_KEY}",
    }
    payload = {
        "message": question,
        "stream": False,
    }
    async with httpx.AsyncClient(timeout=20.0) as client:
        response = await client.post(OPENCLAW_URL, headers=headers, json=payload)
        response.raise_for_status()
        data = response.json()
        # OpenClaw WebChat response format
        return data.get("message", data.get("content", str(data)))


async def ask_openclaw(question: str) -> str:
    """Send transcribed text to OpenClaw and get AI response."""
    if not question:
        raise ValueError("empty question")
    return await _post_to_openclaw(question)
```

**Step 5: Run test to verify it passes**

```bash
pytest tests/test_llm.py -v
# Expected: PASS
```

**Step 6: Commit**

```bash
git add gateway/llm.py gateway/tests/test_llm.py openclaw-data/
git commit -m "feat: add OpenClaw LLM client and child profile"
```

---

## Phase 5: Cloud Server — Voice Pipeline Assembly

### Task 6: Wire Up the Full /voice Endpoint

**Files:**
- Modify: `gateway/main.py`
- Create: `gateway/tests/test_voice_endpoint.py`

**Step 1: Write `gateway/tests/test_voice_endpoint.py`**

```python
import pytest
from unittest.mock import AsyncMock, patch
from fastapi.testclient import TestClient
from main import app

client = TestClient(app)


def test_health_endpoint():
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


def test_voice_endpoint_returns_mp3():
    fake_pcm = b'\x00' * 32000  # 1 second of 16kHz 16-bit mono

    with patch("main.transcribe_pcm", new_callable=AsyncMock) as mock_stt, \
         patch("main.ask_openclaw", new_callable=AsyncMock) as mock_llm, \
         patch("main.synthesize_text", new_callable=AsyncMock) as mock_tts:

        mock_stt.return_value = "今天天气怎么样"
        mock_llm.return_value = "今天是个好日子，适合学习！"
        mock_tts.return_value = b'\xff\xfb' + b'\x00' * 100  # fake MP3

        response = client.post(
            "/voice",
            files={"audio": ("audio.pcm", fake_pcm, "application/octet-stream")}
        )

    assert response.status_code == 200
    assert response.headers["content-type"] == "audio/mpeg"
    assert len(response.content) > 0


def test_voice_endpoint_returns_400_on_empty_audio():
    response = client.post(
        "/voice",
        files={"audio": ("audio.pcm", b"", "application/octet-stream")}
    )
    assert response.status_code == 400
```

**Step 2: Run tests to verify they fail**

```bash
pytest tests/test_voice_endpoint.py -v
# Expected: FAIL on test_voice_endpoint_returns_mp3 (NotImplementedError)
```

**Step 3: Update `gateway/main.py`**

```python
from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.responses import Response
import logging

from stt import transcribe_pcm
from llm import ask_openclaw
from tts import synthesize_text

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

app = FastAPI(title="KidPalAI Voice Gateway")


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.post("/voice")
async def voice(audio: UploadFile = File(...)):
    """
    Receives raw 16kHz 16-bit mono PCM audio from ESP32-S3.
    Pipeline: PCM → STT → OpenClaw LLM → TTS → MP3 response
    """
    pcm_data = await audio.read()

    if not pcm_data:
        raise HTTPException(status_code=400, detail="empty audio")

    try:
        # Step 1: Speech to text
        logger.info(f"STT: received {len(pcm_data)} bytes of audio")
        text = await transcribe_pcm(pcm_data)
        logger.info(f"STT result: {text!r}")

        if not text.strip():
            raise HTTPException(status_code=422, detail="could not transcribe audio")

        # Step 2: LLM response
        logger.info(f"LLM: asking OpenClaw: {text!r}")
        reply = await ask_openclaw(text)
        logger.info(f"LLM reply: {reply!r}")

        # Step 3: Text to speech
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

**Step 4: Run tests to verify they pass**

```bash
pytest tests/test_voice_endpoint.py -v
# Expected: PASS (all 3 tests)
```

**Step 5: Run all tests**

```bash
pytest gateway/tests/ -v
# Expected: all tests PASS
```

**Step 6: Commit**

```bash
git add gateway/main.py gateway/tests/test_voice_endpoint.py
git commit -m "feat: wire up full voice pipeline in /voice endpoint"
```

---

## Phase 6: Cloud Server — Deploy

### Task 7: Deploy to VPS

**Prerequisites:** VPS with Docker + Docker Compose installed, domain DNS pointing to VPS.

**Step 1: Install certbot on VPS and get SSL cert**

```bash
# On VPS
sudo apt install certbot
sudo certbot certonly --standalone -d yourdomain.com
# Expected: Certificate saved at /etc/letsencrypt/live/yourdomain.com/
```

**Step 2: Copy project to VPS**

```bash
# On local machine
scp -r . user@yourserver:/opt/kidpalai/
# Or: git clone on VPS
```

**Step 3: Create `.env` from `.env.example`**

```bash
# On VPS
cp .env.example gateway/.env
nano gateway/.env  # fill in real API keys
```

**Step 4: Start all services**

```bash
docker compose up -d
# Expected: 3 containers running (openclaw, voice-gateway, nginx)
docker compose ps
```

**Step 5: Smoke test the gateway**

```bash
curl https://yourdomain.com/health
# Expected: {"status":"ok"}
```

**Step 6: Test with a real PCM file**

```bash
# Record 2 seconds of voice: "你好书童"
# Using ffmpeg: ffmpeg -f pulse -i default -t 2 -ar 16000 -ac 1 -f s16le test.pcm
curl -X POST https://yourdomain.com/voice \
  -F "audio=@test.pcm;type=application/octet-stream" \
  --output reply.mp3
# Expected: reply.mp3 is a valid MP3 with AI response
mpv reply.mp3
```

---

## Phase 7: ESP32-S3 Firmware

### Task 8: ESP-IDF Project Setup

**Prerequisites:** Install ESP-IDF v5.x following https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/

**Files:**
- Create: `firmware/` (ESP-IDF project)

**Step 1: Create ESP-IDF project**

```bash
cd c:/WorkGit/KidPalAI
idf.py create-project firmware
cd firmware
idf.py set-target esp32s3
```

**Step 2: Add ESP-SR and ESP-ADF components**

In `firmware/CMakeLists.txt`, add:
```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    $ENV{ADF_PATH}/components
    $ENV{IDF_PATH}/../esp-sr
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kidpalai)
```

**Step 3: Create `firmware/main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "audio_input.c"
        "audio_output.c"
        "voice_upload.c"
        "led.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_client
        esp_sr
        driver
        nvs_flash
)
```

**Step 4: Commit scaffold**

```bash
git add firmware/
git commit -m "chore: add ESP-IDF firmware project scaffold"
```

---

### Task 9: WiFi Module

**Files:**
- Create: `firmware/main/wifi.h`
- Create: `firmware/main/wifi.c`

**Step 1: Write `firmware/main/wifi.h`**

```c
#pragma once
#include "esp_err.h"

esp_err_t wifi_init_sta(const char *ssid, const char *password);
bool wifi_is_connected(void);
```

**Step 2: Write `firmware/main/wifi.c`**

```c
#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static bool s_connected = false;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_wifi_connect();
        ESP_LOGI(TAG, "retrying wifi connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP, wifi connected");
    }
}

esp_err_t wifi_init_sta(const char *ssid, const char *password) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler, NULL, NULL);

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to SSID: %s", ssid);
        return ESP_OK;
    }
    return ESP_FAIL;
}

bool wifi_is_connected(void) { return s_connected; }
```

**Step 3: Build (verify compile)**

```bash
cd firmware
idf.py build
# Expected: build successful
```

**Step 4: Commit**

```bash
git add firmware/main/wifi.h firmware/main/wifi.c
git commit -m "feat(firmware): add wifi sta module with auto-reconnect"
```

---

### Task 10: LED Status Module

**Files:**
- Create: `firmware/main/led.h`
- Create: `firmware/main/led.c`

**Step 1: Write `firmware/main/led.h`**

```c
#pragma once

typedef enum {
    LED_STATE_IDLE,      // green steady
    LED_STATE_LISTENING, // blue blink
    LED_STATE_WAITING,   // yellow steady
    LED_STATE_PLAYING,   // green blink
    LED_STATE_ERROR,     // red blink
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);
```

**Step 2: Write `firmware/main/led.c`**

```c
#include "led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Adjust GPIO pins to your wiring
#define LED_R_GPIO  GPIO_NUM_1
#define LED_G_GPIO  GPIO_NUM_2
#define LED_B_GPIO  GPIO_NUM_3

static led_state_t s_state = LED_STATE_IDLE;

void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_R_GPIO) |
                        (1ULL << LED_G_GPIO) |
                        (1ULL << LED_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    led_set_state(LED_STATE_IDLE);
}

void led_set_state(led_state_t state) {
    s_state = state;
    gpio_set_level(LED_R_GPIO, 0);
    gpio_set_level(LED_G_GPIO, 0);
    gpio_set_level(LED_B_GPIO, 0);
    switch (state) {
        case LED_STATE_IDLE:    gpio_set_level(LED_G_GPIO, 1); break;
        case LED_STATE_LISTENING: gpio_set_level(LED_B_GPIO, 1); break;
        case LED_STATE_WAITING: gpio_set_level(LED_R_GPIO, 1);
                                gpio_set_level(LED_G_GPIO, 1); break; // yellow
        case LED_STATE_PLAYING: gpio_set_level(LED_G_GPIO, 1); break;
        case LED_STATE_ERROR:   gpio_set_level(LED_R_GPIO, 1); break;
    }
}
```

**Step 3: Build**

```bash
idf.py build
# Expected: build successful
```

**Step 4: Commit**

```bash
git add firmware/main/led.h firmware/main/led.c
git commit -m "feat(firmware): add LED status indicator module"
```

---

### Task 11: I2S Audio Input (INMP441 Microphone)

**Files:**
- Create: `firmware/main/audio_input.h`
- Create: `firmware/main/audio_input.c`

**Step 1: Write `firmware/main/audio_input.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      1

esp_err_t audio_input_init(void);
// Read PCM samples into buf. Returns number of bytes read.
int audio_input_read(int16_t *buf, size_t max_samples);
```

**Step 2: Write `firmware/main/audio_input.c`**

```c
#include "audio_input.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio_input";

// INMP441 wiring
#define I2S_MIC_SCK   GPIO_NUM_12
#define I2S_MIC_WS    GPIO_NUM_11
#define I2S_MIC_SD    GPIO_NUM_10

static i2s_chan_handle_t rx_handle = NULL;

esp_err_t audio_input_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK,
            .ws   = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_MIC_SD,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false,
                             .ws_inv = false},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S mic initialized at %d Hz", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

int audio_input_read(int16_t *buf, size_t max_samples) {
    // INMP441 outputs 32-bit words; MSB is the 18-bit audio data
    int32_t raw[max_samples];
    size_t bytes_read = 0;
    i2s_channel_read(rx_handle, raw, max_samples * sizeof(int32_t),
                     &bytes_read, pdMS_TO_TICKS(100));
    int samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++) {
        buf[i] = (int16_t)(raw[i] >> 14); // shift 32-bit to 16-bit
    }
    return samples;
}
```

**Step 3: Build**

```bash
idf.py build
# Expected: build successful
```

**Step 4: Commit**

```bash
git add firmware/main/audio_input.h firmware/main/audio_input.c
git commit -m "feat(firmware): add I2S audio input for INMP441 microphone"
```

---

### Task 12: I2S Audio Output (MAX98357A Speaker)

**Files:**
- Create: `firmware/main/audio_output.h`
- Create: `firmware/main/audio_output.c`

**Step 1: Write `firmware/main/audio_output.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t audio_output_init(void);
esp_err_t audio_output_play_mp3(const uint8_t *mp3_data, size_t mp3_len);
```

**Step 2: Write `firmware/main/audio_output.c`**

```c
#include "audio_output.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_audio.h"  // from ESP-ADF
#include "mp3_decoder.h"

static const char *TAG = "audio_output";

// MAX98357A wiring
#define I2S_SPK_BCLK  GPIO_NUM_6
#define I2S_SPK_LRC   GPIO_NUM_5
#define I2S_SPK_DIN   GPIO_NUM_4

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t audio_output_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCLK,
            .ws   = I2S_SPK_LRC,
            .dout = I2S_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S speaker initialized");
    return ESP_OK;
}

esp_err_t audio_output_play_mp3(const uint8_t *mp3_data, size_t mp3_len) {
    // Use ESP-ADF mp3 decoder to decode and stream to I2S
    // This is a simplified blocking implementation
    // For production use ESP-ADF pipeline
    ESP_LOGI(TAG, "playing %d bytes of MP3", mp3_len);
    // TODO: implement using esp_audio pipeline
    // See: https://docs.espressif.com/projects/esp-adf/en/latest/
    return ESP_OK;
}
```

**Step 3: Build**

```bash
idf.py build
# Expected: build successful (audio_output_play_mp3 is a stub for now)
```

**Step 4: Commit**

```bash
git add firmware/main/audio_output.h firmware/main/audio_output.c
git commit -m "feat(firmware): add I2S audio output stub for MAX98357A"
```

---

### Task 13: HTTP Voice Upload Module

**Files:**
- Create: `firmware/main/voice_upload.h`
- Create: `firmware/main/voice_upload.c`

**Step 1: Write `firmware/main/voice_upload.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Returns allocated buffer with MP3 data. Caller must free().
// mp3_len is set to the number of bytes returned.
esp_err_t voice_upload(const uint8_t *pcm_data, size_t pcm_len,
                       uint8_t **mp3_out, size_t *mp3_len);
```

**Step 2: Write `firmware/main/voice_upload.c`**

```c
#include "voice_upload.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "voice_upload";

#define GATEWAY_URL "https://yourdomain.com/voice"
#define RESPONSE_BUF_MAX (256 * 1024)  // 256KB max MP3 response

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t capacity;
} response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    response_buf_t *rb = (response_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (rb->len + evt->data_len < rb->capacity) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
        }
    }
    return ESP_OK;
}

esp_err_t voice_upload(const uint8_t *pcm_data, size_t pcm_len,
                       uint8_t **mp3_out, size_t *mp3_len) {
    response_buf_t rb = {
        .buf = malloc(RESPONSE_BUF_MAX),
        .len = 0,
        .capacity = RESPONSE_BUF_MAX,
    };
    if (!rb.buf) return ESP_ERR_NO_MEM;

    // Build multipart/form-data body manually
    const char *boundary = "kidpalai_boundary";
    char header[256];
    snprintf(header, sizeof(header),
             "--%s\r\nContent-Disposition: form-data; name=\"audio\"; "
             "filename=\"audio.pcm\"\r\nContent-Type: application/octet-stream\r\n\r\n",
             boundary);
    char footer[64];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t body_len = strlen(header) + pcm_len + strlen(footer);
    uint8_t *body = malloc(body_len);
    if (!body) { free(rb.buf); return ESP_ERR_NO_MEM; }

    memcpy(body, header, strlen(header));
    memcpy(body + strlen(header), pcm_data, pcm_len);
    memcpy(body + strlen(header) + pcm_len, footer, strlen(footer));

    char content_type[64];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t config = {
        .url = GATEWAY_URL,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .timeout_ms = 20000,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_post_field(client, (char *)body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP error: err=%d status=%d", err, status);
        free(rb.buf);
        return ESP_FAIL;
    }

    *mp3_out = rb.buf;
    *mp3_len = rb.len;
    ESP_LOGI(TAG, "received %d bytes of MP3", rb.len);
    return ESP_OK;
}
```

**Step 3: Build**

```bash
idf.py build
# Expected: build successful
```

**Step 4: Commit**

```bash
git add firmware/main/voice_upload.h firmware/main/voice_upload.c
git commit -m "feat(firmware): add HTTP voice upload to cloud gateway"
```

---

### Task 14: Main Application Loop + Wake Word

**Files:**
- Create: `firmware/main/main.c`
- Create: `firmware/main/Kconfig.projbuild` (WiFi credentials config)

**Step 1: Write `firmware/main/Kconfig.projbuild`**

```kconfig
menu "KidPalAI Configuration"

config KIDPAL_WIFI_SSID
    string "WiFi SSID"
    default "myssid"

config KIDPAL_WIFI_PASS
    string "WiFi Password"
    default "mypassword"

config KIDPAL_GATEWAY_URL
    string "Voice Gateway URL"
    default "https://yourdomain.com/voice"

endmenu
```

**Step 2: Write `firmware/main/main.c`**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_sr/esp_wn_iface.h"
#include "esp_sr/esp_wn_models.h"
#include "esp_sr/esp_afe_sr_iface.h"
#include "esp_sr/esp_afe_sr_models.h"

#include "wifi.h"
#include "led.h"
#include "audio_input.h"
#include "audio_output.h"
#include "voice_upload.h"

static const char *TAG = "main";

// VAD: stop recording after 1.5s of silence
#define VAD_SILENCE_FRAMES  75   // 75 * 20ms = 1.5s
// Max recording: 10 seconds
#define MAX_RECORD_FRAMES   500  // 500 * 20ms = 10s
#define FRAME_SAMPLES       320  // 20ms at 16kHz

void app_main(void) {
    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    led_init();
    led_set_state(LED_STATE_ERROR);  // red until wifi connects

    // Connect WiFi
    ESP_LOGI(TAG, "Connecting to WiFi...");
    if (wifi_init_sta(CONFIG_KIDPAL_WIFI_SSID,
                      CONFIG_KIDPAL_WIFI_PASS) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed");
        return;
    }
    led_set_state(LED_STATE_IDLE);
    ESP_LOGI(TAG, "WiFi connected. Ready.");

    // Init audio I/O
    audio_input_init();
    audio_output_init();

    // Init ESP-SR AFE (Audio Front End) + WakeNet
    esp_afe_sr_iface_t *afe_handle = &ESP_AFE_SR_HANDLE;
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);

    esp_wn_iface_t *wakenet = &WAKENET_MODEL;
    model_iface_data_t *wn_model = wakenet->create(NULL, DET_MODE_90);
    int wn_chunk_size = wakenet->get_num_samples(wn_model);

    ESP_LOGI(TAG, "Listening for wake word...");

    // PCM recording buffer (max 10s)
    int16_t *record_buf = malloc(MAX_RECORD_FRAMES * FRAME_SAMPLES * sizeof(int16_t));
    int16_t frame_buf[FRAME_SAMPLES];

    while (1) {
        // --- IDLE: listen for wake word ---
        int samples = audio_input_read(frame_buf, FRAME_SAMPLES);
        if (samples <= 0) continue;

        // Feed to AFE
        afe_handle->feed(afe_data, frame_buf);
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;

        // Check wake word
        int wn_result = wakenet->detect(wn_model, res->data);
        if (wn_result <= 0) continue;

        // --- WAKE WORD DETECTED ---
        ESP_LOGI(TAG, "Wake word detected! Recording...");
        led_set_state(LED_STATE_LISTENING);

        // Record until silence
        int total_samples = 0;
        int silence_frames = 0;

        while (total_samples < MAX_RECORD_FRAMES * FRAME_SAMPLES) {
            int n = audio_input_read(
                record_buf + total_samples,
                FRAME_SAMPLES);
            if (n <= 0) continue;
            total_samples += n;

            // Simple VAD: check if frame is silent
            int32_t energy = 0;
            for (int i = total_samples - n; i < total_samples; i++) {
                energy += abs(record_buf[i]);
            }
            energy /= n;

            if (energy < 200) {  // silence threshold
                silence_frames++;
            } else {
                silence_frames = 0;
            }

            if (silence_frames >= VAD_SILENCE_FRAMES) {
                ESP_LOGI(TAG, "Silence detected, stopping recording");
                break;
            }
        }

        ESP_LOGI(TAG, "Recorded %d samples, uploading...", total_samples);
        led_set_state(LED_STATE_WAITING);

        // --- UPLOAD TO CLOUD ---
        uint8_t *mp3_data = NULL;
        size_t mp3_len = 0;
        esp_err_t err = voice_upload(
            (uint8_t *)record_buf,
            total_samples * sizeof(int16_t),
            &mp3_data, &mp3_len);

        if (err == ESP_OK && mp3_len > 0) {
            ESP_LOGI(TAG, "Playing response (%d bytes)...", mp3_len);
            led_set_state(LED_STATE_PLAYING);
            audio_output_play_mp3(mp3_data, mp3_len);
            free(mp3_data);
        } else {
            ESP_LOGE(TAG, "Upload failed or empty response");
            led_set_state(LED_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        led_set_state(LED_STATE_IDLE);
        ESP_LOGI(TAG, "Ready, listening again...");
    }

    free(record_buf);
}
```

**Step 3: Build**

```bash
idf.py build
# Expected: build successful
```

**Step 4: Flash to ESP32-S3**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
# Expected: device boots, connects wifi, logs "Listening for wake word..."
```

**Step 5: Commit**

```bash
git add firmware/main/main.c firmware/main/Kconfig.projbuild
git commit -m "feat(firmware): add main loop with wake word detection and voice pipeline"
```

---

## Phase 8: End-to-End Integration Test

### Task 15: Full System Smoke Test

**Step 1: Verify cloud gateway is live**

```bash
curl https://yourdomain.com/health
# Expected: {"status":"ok"}
```

**Step 2: Say wake word near device**

Say: "嘿书童" (or "小书童")
Expected: LED turns blue (recording)

**Step 3: Ask a question**

Say: "今天的作业是什么？"
Expected: LED turns yellow (uploading), then green (playing)
Expected: Speaker plays AI response in Chinese

**Step 4: Test with different age profiles**

- Edit `openclaw-data/memory/child_profile.md` to change age
- Restart OpenClaw: `docker compose restart openclaw`
- Ask the same question and verify the response tone changes

**Step 5: Commit final integration notes**

```bash
git add .
git commit -m "test: end-to-end smoke test passing"
```

---

## Configuration Reference

### WiFi Credentials (firmware)

```bash
cd firmware
idf.py menuconfig
# Navigate to: KidPalAI Configuration
# Set SSID, Password, Gateway URL
```

### Child Profile (OpenClaw)

Edit `openclaw-data/memory/child_profile.md` to customize:
- Child name, age, grade
- Subjects
- Reminder times
- Language style

### API Keys (gateway/.env)

Copy `.env.example` → `gateway/.env` and fill in:
- `XUNFEI_APP_ID` / `XUNFEI_API_KEY` / `XUNFEI_API_SECRET` from https://console.xfyun.cn
- `VOLC_ACCESS_KEY` / `VOLC_SECRET_KEY` / `VOLC_APP_ID` from https://console.volcengine.com
- `OPENCLAW_WEBCHAT_URL` from your OpenClaw server

---

## Quick Reference

| Component | Get API Key |
|-----------|------------|
| 讯飞 STT | https://console.xfyun.cn/app/myapp |
| 火山引擎 TTS | https://console.volcengine.com/speech/service/8 |
| OpenClaw | Self-hosted, see https://docs.openclaw.ai |
| ESP-IDF | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/ |
| ESP-SR (唤醒词) | https://github.com/espressif/esp-sr |
| ESP-ADF (音频) | https://github.com/espressif/esp-adf |
