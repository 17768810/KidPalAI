# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

KidPalAI ("小书童") is an AI reading companion for kids. An ESP32-S3 device wakes on a voice keyword, streams audio to a cloud server, which runs STT → OpenClaw LLM → TTS and returns MP3 audio for playback.

**Two independent codebases live here:**
- `gateway/` — Python FastAPI cloud service (runs on VPS)
- `firmware/` — ESP-IDF C firmware (runs on ESP32-S3 hardware)

## Gateway (Python)

### Commands

```bash
cd gateway

# Run all tests
python -m pytest tests/ -v

# Run a single test file
python -m pytest tests/test_stt.py -v

# Run a single test
python -m pytest tests/test_voice_endpoint.py::test_voice_endpoint_returns_mp3 -v

# Start dev server (requires gateway/.env with API keys)
python -m uvicorn main:app --reload --port 8000
```

### Architecture

The `/voice` endpoint in `main.py` is the single entry point. It orchestrates three clients in sequence:

```
POST /voice (PCM audio)
  → stt.py::transcribe_pcm()     # 讯飞 WebSocket STT API
  → llm.py::ask_openclaw()       # OpenClaw WebChat HTTP API
  → tts.py::synthesize_text()    # 火山引擎 TTS HTTP API
  → returns MP3 bytes
```

Each client (`stt.py`, `llm.py`, `tts.py`) exposes one async public function and delegates real API calls to a private `_call_*` function. Tests mock the private function, so no real API keys are needed to run tests.

### Environment

Copy `.env.example` → `gateway/.env`. Required vars: `XUNFEI_APP_ID`, `XUNFEI_API_KEY`, `XUNFEI_API_SECRET`, `VOLC_ACCESS_KEY`, `VOLC_APP_ID`, `OPENCLAW_WEBCHAT_URL`.

### Deployment

```bash
# From project root on VPS
docker compose up -d
```

Nginx terminates HTTPS and proxies `/voice` and `/health` to the gateway container. SSL certs are expected at `/etc/letsencrypt/live/<domain>/`.

## Firmware (ESP-IDF / C)

Requires ESP-IDF v5.x. See `firmware/README.md` for full setup.

```bash
cd firmware

# Configure WiFi SSID, password, and gateway URL
idf.py menuconfig   # → KidPalAI Configuration

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

### Architecture

`main.c` runs a single FreeRTOS task with this state machine:

```
IDLE (LED green)
  → wake word detected (ESP-SR WakeNet — not yet wired, see TODOs in main.c)
LISTENING (LED blue)
  → records 16kHz 16-bit mono PCM via audio_input.c (INMP441 on I2S_NUM_0)
  → energy-based VAD stops recording after 1.5s silence
WAITING (LED yellow)
  → voice_upload.c sends PCM as multipart/form-data to CONFIG_KIDPAL_GATEWAY_URL
PLAYING (LED green blink)
  → audio_output.c plays returned MP3 via MAX98357A on I2S_NUM_1
  → audio_output_play_mp3() is a stub pending ESP-ADF integration
```

### Known TODOs in firmware

1. **ESP-SR wake word**: Headers and init code are in `main.c` but commented out. Install `espressif/esp-sr` component and uncomment.
2. **MP3 playback**: `audio_output_play_mp3()` in `audio_output.c` is a stub. Implement using ESP-ADF pipeline, or change gateway TTS output to raw PCM to avoid needing a decoder.

## AI Persona Configuration

`openclaw-data/child_profile.md` is loaded by OpenClaw as persistent memory. Edit this file to change the child's name, age, grade, subjects, and reminder schedule. Restart the OpenClaw container after changes: `docker compose restart openclaw`.

## MimiClaw Firmware (firmware-mimiclaw/)

ESP-IDF C firmware based on MimiClaw. Handles AI agent directly on ESP32-S3, calling MiniMax M2.5-highspeed LLM on-device. Gateway used only for STT/TTS.

**Board**: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)

**Pipeline**: record → gateway/stt → MiniMax M2.5-highspeed → gateway/tts → play

**Build**:
```bash
cd firmware-mimiclaw
idf.py build
idf.py -p COM3 flash monitor
```

**Config**: Hold GPIO0 (BOOT button) for 3s at boot → SoftAP "KidPalAI-Setup" → http://192.168.4.1 → enter WiFi + MiniMax API Key → save → restart

**Persona**: `spiffs_data/config/SOUL.md` (书童, responds to 叶欣羽)
**Reminders**: `spiffs_data/memory/HEARTBEAT.md` (18:00 homework, 20:30 sleep)
**Gateway STT**: POST http://8.133.3.7:8000/stt
**Gateway TTS**: POST http://8.133.3.7:8000/tts

**First-audio latency target**: ~6-7s (STT 3s + MiniMax M2.5-highspeed ~2-3s + TTS 1s)

**Key modules**:
- `main/audio/` — I2S mic (INMP441, I2S_NUM_0) + speaker (MAX98357A, I2S_NUM_1) + energy VAD
- `main/voice/` — HTTP STT and TTS clients
- `main/audio_task/` — FreeRTOS task: wake → record → STT → agent → TTS playback
- `main/led/` — LED states: IDLE/LISTENING/WAITING/ERROR
- `main/config/` — NVS config store + SoftAP provisioning portal

**Integration test checklist** (requires hardware):
- [ ] Voice → STT → MiniMax LLM → TTS → speaker: first audio ≤7s
- [ ] Multi-turn: session history accumulates in SPIFFS
- [ ] Telegram text → AI response in Telegram
- [ ] Cron reminder: HEARTBEAT.md trigger → speaks + Telegram push
- [ ] GPIO0 3s hold → SoftAP portal → config save → WiFi connect
