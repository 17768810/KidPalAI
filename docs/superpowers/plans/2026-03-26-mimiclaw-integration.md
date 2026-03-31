# MimiClaw Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate MimiClaw firmware as the ESP32-S3 base, adding KidPalAI's audio pipeline so the device calls MiniMax M2.5-highspeed directly and uses the gateway only for STT/TTS, reducing first-audio latency from 14-19s to ~6-7s.

**Architecture:** MimiClaw handles persona (SOUL.md), memory, cron reminders, Telegram, and LLM calls on the ESP32 itself. A new `audio_task` handles recording → STT → agent → TTS → playback. The gateway becomes a dumb voice service exposing `/stt` (PCM→text) and `/tts` (text→PCM). Agent responses are sentence-split after the full batch LLM response and fed into a `g_sentence_queue`, which `audio_task` consumes one sentence at a time to TTS and play.

> **Spec deviation — voice_stt/voice_tts signatures:** The spec (Section 4.3/4.4) defines `voice_stt(pcm, len, text_out, text_max)` and `voice_tts(text)` with the URL internal to each function. This plan uses `voice_stt(gateway_stt_url, pcm, ...)` and `voice_tts(gateway_tts_url, text)` instead, accepting the URL as a parameter. Reason: the URL comes from `config_store` at runtime, and passing it explicitly makes both functions testable without NVS. The spec pseudocode is superseded by this plan.

> **Spec deviation — LLM streaming:** The spec (Section 4.1) describes per-token callbacks (`on_token`, `on_stream_done`). MimiClaw's `llm_proxy.c` uses batch HTTP (full response buffered). This plan uses post-response sentence splitting in `dispatch_sentences_to_audio()` instead. Latency impact: ~6-7s first audio (vs spec's ~5-6s estimate) — still 2-3× better than the current 14-19s.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, MimiClaw (C), Python FastAPI (gateway), MiniMax M2.5-highspeed API (OpenAI-compatible), Xunfei STT, Volcano Engine TTS

---

## Important Architecture Note

MimiClaw's `llm_proxy.c` uses **batch (non-streaming) HTTP** — the full LLM response is buffered before returning. Token-level callbacks do not exist in the current codebase. The plan therefore uses **post-response sentence splitting**: after `llm_chat_tools()` returns the full text, split it by Chinese punctuation and feed sentences one-by-one to `g_sentence_queue`. With MiniMax M2.5-highspeed (~2-3s total response), this gives first audio at ~6-7s, acceptable for the use case.

---

## File Map

**New firmware: `firmware-mimiclaw/`** (clone of MimiClaw + additions)

| File | Action | Responsibility |
|------|--------|---------------|
| `firmware-mimiclaw/` | Clone | MimiClaw base repo |
| `main/audio/audio_input.c/h` | Port from `firmware/main/audio_input.c` | INMP441 I2S read |
| `main/audio/audio_output.c/h` | Port from `firmware/main/audio_output.c` | MAX98357A I2S write (PCM) |
| `main/audio/vad.c/h` | Extract from `firmware/main/main.c` | Energy VAD, wake detection |
| `main/voice/voice_stt.c/h` | Create | HTTP POST PCM → /stt → JSON text |
| `main/voice/voice_tts.c/h` | Create | HTTP POST text → /tts → PCM → audio_output |
| `main/audio_task/audio_task.c/h` | Create | FreeRTOS task: record→STT→queue→consume sentences→TTS→play |
| `main/bus/message_bus.h` | Modify | Add `MIMI_CHANNEL_VOICE "voice"` constant |
| `main/agent/agent_loop.c` | Modify | Sentence-split response + write g_sentence_queue on voice channel |
| `main/CMakeLists.txt` | Modify | Add new SRCS entries |
| `main/mimi.c` | Modify | Start audio_task in app_main |
| `main/led/led.c/h` | Port from `firmware/main/led.c` | LED_IDLE/LISTENING/WAITING/ERROR states |
| `main/config/config_store.c/h` | Create | NVS: WiFi + gateway_url + llm_url + llm_key + llm_model |
| `main/config/config_server.c/h` | Create | SoftAP HTTP portal (WiFi + MiniMax key form) |
| `main/wifi/wifi_manager.c` | Modify | Add wifi_init_ap(), gpio0_held_long() |
| `sdkconfig.defaults` | Modify | Keep PSRAM; add I2S, main task stack |
| `spiffs_data/config/SOUL.md` | Create | 书童 persona |
| `spiffs_data/config/USER.md` | Create | 叶欣羽 profile |
| `spiffs_data/memory/HEARTBEAT.md` | Create | 18:00/20:30 reminders |
| `spiffs_data/memory/MEMORY.md` | Create | Empty initial memory |

**Gateway: `gateway/`** (existing, modify `main.py` only)

| File | Action | Responsibility |
|------|--------|---------------|
| `gateway/main.py` | Modify | Add `POST /stt` and `POST /tts` endpoints |

---

## Task 1: Gateway — Add /stt and /tts endpoints

**Files:**
- Modify: `gateway/main.py`

This is the first rollback checkpoint. After this task, the existing `/voice/stream` keeps working and new endpoints are live.

- [ ] **Step 1: Add endpoints to main.py**

Open `gateway/main.py`. After the existing imports, add `BaseModel` import and `TTSRequest` class. Then add two new route handlers after the `/health` route:

```python
# At top of file, add to imports:
from pydantic import BaseModel

# Add after the existing imports block:
class TTSRequest(BaseModel):
    text: str
```

Add these two routes after `@app.get("/health")`:

```python
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
    except Exception as e:
        logger.error(f"TTS error: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=str(e))
```

- [ ] **Step 2: Write gateway tests**

Create `gateway/tests/test_stt_tts_endpoints.py`. The `client` fixture is defined in `gateway/tests/conftest.py` (check that it exists; if not, add `@pytest.fixture def client(): from main import app; return TestClient(app)`):

```python
import pytest
from fastapi.testclient import TestClient
from unittest.mock import AsyncMock, patch

def test_stt_returns_text(client):
    with patch("main.transcribe_pcm", new=AsyncMock(return_value="你好世界")):
        boundary = "test_boundary"
        body = (
            f"--{boundary}\r\nContent-Disposition: form-data; name=audio; "
            f"filename=audio.pcm\r\nContent-Type: application/octet-stream\r\n\r\n"
        ).encode() + b"\x00" * 100 + f"\r\n--{boundary}--\r\n".encode()
        resp = client.post(
            "/stt",
            data=body,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        )
    assert resp.status_code == 200
    assert resp.json() == {"text": "你好世界"}

def test_stt_returns_422_on_empty_transcription(client):
    with patch("main.transcribe_pcm", new=AsyncMock(return_value="")):
        boundary = "test_boundary"
        body = (
            f"--{boundary}\r\nContent-Disposition: form-data; name=audio; "
            f"filename=audio.pcm\r\nContent-Type: application/octet-stream\r\n\r\n"
        ).encode() + b"\x00" * 100 + f"\r\n--{boundary}--\r\n".encode()
        resp = client.post(
            "/stt",
            data=body,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        )
    assert resp.status_code == 422

def test_tts_returns_pcm(client):
    fake_pcm = b"\x00\x01" * 100
    with patch("main.synthesize_pcm", new=AsyncMock(return_value=fake_pcm)):
        resp = client.post("/tts", json={"text": "你好，书童"})
    assert resp.status_code == 200
    assert resp.headers["content-type"] == "audio/pcm"
    assert resp.content == fake_pcm

def test_tts_returns_400_on_empty_text(client):
    resp = client.post("/tts", json={"text": "  "})
    assert resp.status_code == 400
```

- [ ] **Step 3: Run tests**

```bash
cd gateway
python -m pytest tests/test_stt_tts_endpoints.py -v
```

Expected: 4 PASS

- [ ] **Step 4: Deploy to server**

```bash
# From Windows, run deploy script
python deploy_gateway.py
# Then wait for container restart
```

Verify live:
```bash
# Test /health still works
curl http://8.133.3.7:8000/health
# Expected: {"status":"ok"}
```

- [ ] **Step 5: Commit**

```bash
git add gateway/main.py gateway/tests/test_stt_tts_endpoints.py
git commit -m "feat: add /stt and /tts endpoints to gateway"
```

---

## Task 2: MimiClaw clone + MiniMax config + SOUL.md + Telegram test

**Files:**
- Create: `firmware-mimiclaw/` (from git clone)
- Create: `firmware-mimiclaw/spiffs_data/config/SOUL.md`
- Create: `firmware-mimiclaw/spiffs_data/config/USER.md`
- Create: `firmware-mimiclaw/spiffs_data/memory/MEMORY.md`
- Create: `firmware-mimiclaw/spiffs_data/memory/HEARTBEAT.md`

Second rollback checkpoint. After this task, MimiClaw text conversation (Telegram) works with MiniMax.

- [ ] **Step 1: Clone MimiClaw into firmware-mimiclaw/**

```bash
cd c:\WorkGit\KidPalAI
git clone https://github.com/memovai/mimiclaw firmware-mimiclaw
```

- [ ] **Step 1b: Check actual SPIFFS directory name in CMakeLists.txt**

```bash
grep -i spiffs firmware-mimiclaw/CMakeLists.txt
```

Find the line like `spiffs_create_partition_image(spiffs <DIR> ...)`. Note the actual directory name (may be `spiffs_image/`, `spiffs_data/`, or other). All subsequent steps use `spiffs_data/` — rename accordingly if different.

- [ ] **Step 2: Verify partition table fits 16MB flash**

```bash
cat firmware-mimiclaw/partitions.csv
```

Calculate total: `ota_0(2MB) + ota_1(2MB) + spiffs(~11.8MB) + nvs+otadata+phy+coredump(~100KB) = ~16MB` ✓

If `ota_0` and `ota_1` are only 2MB each and the final binary exceeds 2MB, adjust:
```
ota_0,  app, ota_0, 0x20000,  0x280000   # 2.5MB
ota_1,  app, ota_1, 0x2A0000, 0x280000   # 2.5MB
spiffs, data, spiffs, 0x520000, 0xAD0000  # ~10.8MB
```
(Only adjust if first `idf.py build` reports binary > 2MB)

- [ ] **Step 3: Write SPIFFS persona files**

```bash
mkdir -p firmware-mimiclaw/spiffs_data/config
mkdir -p firmware-mimiclaw/spiffs_data/memory
mkdir -p firmware-mimiclaw/spiffs_data/sessions
```

Create `firmware-mimiclaw/spiffs_data/config/SOUL.md`:
```markdown
你是温柔耐心的AI学习伙伴，名叫"书童"。
用简单活泼的中文回答，多鼓励，不批评。
每次回答不超过3句话，不使用Markdown格式，不使用星号或特殊符号。
禁止涉及暴力、恐怖、成人内容。
```

Create `firmware-mimiclaw/spiffs_data/config/USER.md`:
```markdown
用户：叶欣羽，10岁，小学二年级
主要科目：语文、数学、英语
语言：中文
```

Create `firmware-mimiclaw/spiffs_data/memory/MEMORY.md`:
```markdown
# 长期记忆
（暂无内容，将在使用中积累）
```

Create `firmware-mimiclaw/spiffs_data/memory/HEARTBEAT.md`:
```markdown
- [ ] 18:00 提醒叶欣羽该做作业了
- [ ] 20:30 提醒叶欣羽准备睡觉
```

- [ ] **Step 4: Update spiffs_create_partition_image path in root CMakeLists.txt**

Open `firmware-mimiclaw/CMakeLists.txt`, change `spiffs_data` to match the actual directory name if different:
```cmake
spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
```
This should already be correct if you named the directory `spiffs_data`. Verify it matches.

- [ ] **Step 5: Configure MiniMax API via serial CLI after first flash**

First build and flash (to verify base MimiClaw works):
```bash
cd firmware-mimiclaw
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

Once running, configure via serial CLI (USB terminal at 115200 baud):
```
set_model_provider openai
set_api_base https://api.minimaxi.com/v1
set_api_key sk-cp-JcZUCFzShseZQYwrDjfZUBV8THQYHHK6nLMsYG2ixz1BvIVRfp2ZWlROeM3iYdd3M5idqAKur9-6hLU91RVS3sjxuVVOOxM7R1-u0Hqfa2Sf74nwtEptOnc
set_model MiniMax-M2.5-highspeed
set_wifi_ssid YOUR_SSID
set_wifi_pass YOUR_PASSWORD
```

Configure Telegram token (optional, for reminder testing later):
```
set_telegram_token YOUR_BOT_TOKEN
```

- [ ] **Step 6: Test Telegram text conversation**

Send a message to the Telegram bot. Verify:
- ESP32 serial log shows `[agent] processing voice channel message` or similar
- Response uses 书童 persona (simple, ≤3 sentences, no Markdown)
- Response latency < 5s for simple questions

- [ ] **Step 7: Commit**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/spiffs_data/ firmware-mimiclaw/partitions.csv
git commit -m "feat: add MimiClaw base with persona SOUL.md and MiniMax config"
```

---

## Task 3: Port audio/ modules (audio_input, audio_output, vad, led)

**Files:**
- Create: `firmware-mimiclaw/main/audio/audio_input.c`
- Create: `firmware-mimiclaw/main/audio/audio_input.h`
- Create: `firmware-mimiclaw/main/audio/audio_output.c`
- Create: `firmware-mimiclaw/main/audio/audio_output.h`
- Create: `firmware-mimiclaw/main/audio/vad.c`
- Create: `firmware-mimiclaw/main/audio/vad.h`
- Create: `firmware-mimiclaw/main/led/led.c`
- Create: `firmware-mimiclaw/main/led/led.h`
- Modify: `firmware-mimiclaw/main/CMakeLists.txt`

- [ ] **Step 1: Create audio_input.h**

Create `firmware-mimiclaw/main/audio/audio_input.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_FRAME_SAMPLES 320   // 20ms at 16kHz

esp_err_t audio_input_init(void);
// Returns number of int16 samples read (always AUDIO_FRAME_SAMPLES on success, ≤0 on error)
int audio_input_read(int16_t *buf, size_t max_samples);
```

- [ ] **Step 2: Create audio_input.c**

Copy `firmware/main/audio_input.c` to `firmware-mimiclaw/main/audio/audio_input.c` exactly (no changes needed — same GPIO pins, same I2S config).

- [ ] **Step 3: Create audio_output.h**

Create `firmware-mimiclaw/main/audio/audio_output.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t audio_output_init(void);
// Write raw PCM (16kHz/16bit/mono) to I2S speaker. Blocks until written.
esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len);
```

- [ ] **Step 4: Create audio_output.c**

Copy `firmware/main/audio_output.c` to `firmware-mimiclaw/main/audio/audio_output.c`. Remove the `audio_output_play_mp3()` stub (not needed). File becomes:

```c
#include "audio_output.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "audio_output";

#define I2S_SPK_BCLK  GPIO_NUM_6
#define I2S_SPK_LRC   GPIO_NUM_5
#define I2S_SPK_DIN   GPIO_NUM_4

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t audio_output_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCLK,
            .ws   = I2S_SPK_LRC,
            .dout = I2S_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S speaker initialized (16kHz MONO PCM)");
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

- [ ] **Step 5: Create vad.h**

Create `firmware-mimiclaw/main/audio/vad.h`:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

// VAD configuration
#define VAD_ENERGY_THRESHOLD  200   // silence threshold (tune per environment)
#define VAD_SILENCE_FRAMES    75    // 1.5s silence → stop recording (75 × 20ms)
#define VAD_WAKE_FRAMES       25    // 0.5s loud audio → wake trigger (25 × 20ms)
#define VAD_WAKE_MULTIPLIER   5     // wake threshold = VAD_ENERGY_THRESHOLD × 5
#define MAX_RECORD_FRAMES     150   // max recording: 3s (150 × 20ms × 320 samples = 96KB)

// Returns mean energy of a 16-bit PCM frame
int32_t vad_frame_energy(const int16_t *frame, int samples);

// Update wake detector state. Returns true when wake is triggered.
// Call once per 20ms frame. Resets after returning true.
bool vad_update_wake(int32_t energy);

// Update silence detector state. Returns true when silence threshold exceeded.
// Reset silence_frames to 0 after a non-silent frame.
bool vad_update_silence(int32_t energy, int *silence_frames);
```

- [ ] **Step 6: Create vad.c**

Create `firmware-mimiclaw/main/audio/vad.c`:
```c
#include "vad.h"
#include <stdlib.h>

static int s_loud_frames = 0;

int32_t vad_frame_energy(const int16_t *frame, int samples)
{
    int32_t energy = 0;
    for (int i = 0; i < samples; i++) energy += abs(frame[i]);
    return energy / samples;
}

bool vad_update_wake(int32_t energy)
{
    if (energy > VAD_ENERGY_THRESHOLD * VAD_WAKE_MULTIPLIER) {
        s_loud_frames++;
    } else {
        s_loud_frames = 0;
    }
    if (s_loud_frames >= VAD_WAKE_FRAMES) {
        s_loud_frames = 0;
        return true;
    }
    return false;
}

bool vad_update_silence(int32_t energy, int *silence_frames)
{
    if (energy < VAD_ENERGY_THRESHOLD) {
        (*silence_frames)++;
    } else {
        *silence_frames = 0;
    }
    return (*silence_frames >= VAD_SILENCE_FRAMES);
}
```

- [ ] **Step 7: Port led.h**

Create `firmware-mimiclaw/main/led/led.h`:
```c
#pragma once

typedef enum {
    LED_STATE_IDLE     = 0,
    LED_STATE_LISTENING,
    LED_STATE_WAITING,
    LED_STATE_ERROR,
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
```

- [ ] **Step 8: Port led.c**

Copy `firmware/main/led.c` to `firmware-mimiclaw/main/led/led.c`. If the original uses `#include "led.h"`, update the include path to just `"led.h"` (same directory).

- [ ] **Step 9: Add audio/ and led/ to CMakeLists.txt**

Open `firmware-mimiclaw/main/CMakeLists.txt`. In the `SRCS` list, add:
```cmake
    "audio/audio_input.c"
    "audio/audio_output.c"
    "audio/vad.c"
    "led/led.c"
```

In the `REQUIRES` list, add `esp_driver_i2s` if not already present.

- [ ] **Step 10: Build to verify no errors**

```bash
cd firmware-mimiclaw
idf.py build 2>&1 | grep -E "(error|warning.*audio)"
```

Expected: 0 errors. Warnings about unused functions are OK.

- [ ] **Step 11: Commit**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/main/audio/
git add firmware-mimiclaw/main/led/
git add firmware-mimiclaw/main/CMakeLists.txt
git commit -m "feat: port audio_input, audio_output, vad, led from KidPalAI"
```

---

## Task 4: New voice/ modules (voice_stt, voice_tts)

**Files:**
- Create: `firmware-mimiclaw/main/voice/voice_stt.c`
- Create: `firmware-mimiclaw/main/voice/voice_stt.h`
- Create: `firmware-mimiclaw/main/voice/voice_tts.c`
- Create: `firmware-mimiclaw/main/voice/voice_tts.h`
- Modify: `firmware-mimiclaw/main/CMakeLists.txt`

- [ ] **Step 1: Create voice_stt.h**

Create `firmware-mimiclaw/main/voice/voice_stt.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// Maximum transcription text length
#define VOICE_STT_MAX_TEXT 512

// POST PCM to gateway/stt, write transcribed text to text_out.
// Returns ESP_OK on success. text_out[0]=='\0' means empty result (silence).
// gateway_stt_url example: "http://8.133.3.7:8000/stt"
esp_err_t voice_stt(const char *gateway_stt_url,
                    const uint8_t *pcm, size_t pcm_len,
                    char *text_out, size_t text_max);
```

- [ ] **Step 2: Create voice_stt.c**

Create `firmware-mimiclaw/main/voice/voice_stt.c`:

```c
#include "voice_stt.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "voice_stt";

// HTTP event handler collects response body
typedef struct { char *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb->len + evt->data_len < rb->cap) {
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
    }
    return ESP_OK;
}

esp_err_t voice_stt(const char *gateway_stt_url,
                    const uint8_t *pcm, size_t pcm_len,
                    char *text_out, size_t text_max)
{
    text_out[0] = '\0';

    // Build multipart/form-data body
    const char *boundary = "kidpalai_stt";
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "--%s\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", boundary);
    char ftr[64];
    snprintf(ftr, sizeof(ftr), "\r\n--%s--\r\n", boundary);

    size_t hlen = strlen(hdr), flen = strlen(ftr);
    size_t body_len = hlen + pcm_len + flen;
    uint8_t *body = malloc(body_len);
    if (!body) return ESP_ERR_NO_MEM;
    memcpy(body,              hdr, hlen);
    memcpy(body + hlen,       pcm, pcm_len);
    memcpy(body + hlen + pcm_len, ftr, flen);

    char ct[80];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);

    // Allocate response buffer: VOICE_STT_MAX_TEXT + JSON wrapper overhead (~20 bytes)
    resp_buf_t rb = { .buf = malloc(VOICE_STT_MAX_TEXT + 32), .len = 0, .cap = VOICE_STT_MAX_TEXT + 32 };
    if (!rb.buf) { free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url           = gateway_stt_url,
        .event_handler = http_event_handler,
        .user_data     = &rb,
        .timeout_ms    = 15000,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", ct);
    esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "STT HTTP error: err=%d status=%d", err, status);
        free(rb.buf);
        return ESP_FAIL;
    }

    rb.buf[rb.len] = '\0';
    cJSON *json = cJSON_Parse(rb.buf);
    free(rb.buf);
    if (!json) { ESP_LOGE(TAG, "STT JSON parse error"); return ESP_FAIL; }

    cJSON *text_item = cJSON_GetObjectItemCaseSensitive(json, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring) {
        strlcpy(text_out, text_item->valuestring, text_max);
        ESP_LOGI(TAG, "STT result: %s", text_out);
    }
    cJSON_Delete(json);
    return ESP_OK;
}
```

- [ ] **Step 3: Create voice_tts.h**

Create `firmware-mimiclaw/main/voice/voice_tts.h`:
```c
#pragma once
#include "esp_err.h"

// POST text to gateway/tts, receive PCM, play via audio_output_write_pcm().
// Blocks until all PCM has been played.
// gateway_tts_url example: "http://8.133.3.7:8000/tts"
esp_err_t voice_tts(const char *gateway_tts_url, const char *text);
```

- [ ] **Step 4: Create voice_tts.c**

Create `firmware-mimiclaw/main/voice/voice_tts.c`:

```c
#include "voice_tts.h"
#include "audio/audio_output.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "voice_tts";

// PCM response can be up to ~500KB for a long sentence.
// Use PSRAM for the receive buffer (8MB available on N16R8).
#define TTS_PCM_BUF_MAX (512 * 1024)

typedef struct { uint8_t *buf; size_t len; size_t cap; } resp_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (rb->len + evt->data_len <= rb->cap) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "PCM buffer full — truncating");
        }
    }
    return ESP_OK;
}

esp_err_t voice_tts(const char *gateway_tts_url, const char *text)
{
    if (!text || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    // Build JSON body using cJSON to safely escape quotes/backslashes in text
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "text", text);
    char *body_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body_str) return ESP_ERR_NO_MEM;
    int body_len = strlen(body_str);

    // Allocate PCM receive buffer from PSRAM
    resp_buf_t rb = {
        .buf = heap_caps_malloc(TTS_PCM_BUF_MAX, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = TTS_PCM_BUF_MAX,
    };
    if (!rb.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url           = gateway_tts_url,
        .event_handler = http_evt,
        .user_data     = &rb,
        .timeout_ms    = 10000,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body_str);  // cJSON_PrintUnformatted allocates with malloc

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "TTS HTTP error: err=%d status=%d", err, status);
        heap_caps_free(rb.buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS: received %d PCM bytes, playing...", (int)rb.len);
    err = audio_output_write_pcm(rb.buf, rb.len);
    heap_caps_free(rb.buf);
    return err;
}
```

- [ ] **Step 5: Add voice/ to CMakeLists.txt**

In `firmware-mimiclaw/main/CMakeLists.txt`, add to SRCS:
```cmake
    "voice/voice_stt.c"
    "voice/voice_tts.c"
```

Add `json` to REQUIRES if not already present (needed by `cJSON` in voice_stt.c).

- [ ] **Step 6: Build to verify**

```bash
cd firmware-mimiclaw
idf.py build 2>&1 | grep -i error
```

Expected: 0 errors.

- [ ] **Step 7: Commit**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/main/voice/
git add firmware-mimiclaw/main/CMakeLists.txt
git commit -m "feat: add voice_stt and voice_tts HTTP clients"
```

---

## Task 5: audio_task basic — record → STT → print → TTS play

**Files:**
- Create: `firmware-mimiclaw/main/audio_task/audio_task.c`
- Create: `firmware-mimiclaw/main/audio_task/audio_task.h`
- Modify: `firmware-mimiclaw/main/CMakeLists.txt`
- Modify: `firmware-mimiclaw/main/mimi.c` (entry point, add audio_task startup)

Third rollback checkpoint: validate the full audio pipeline before wiring the agent. At this stage, speech goes directly STT → TTS (echo back as speech), bypassing the agent.

- [ ] **Step 1: Create audio_task.h**

Create `firmware-mimiclaw/main/audio_task/audio_task.h`:
```c
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// URLs for gateway STT and TTS
#define AUDIO_GATEWAY_STT_URL  "http://8.133.3.7:8000/stt"
#define AUDIO_GATEWAY_TTS_URL  "http://8.133.3.7:8000/tts"

// Sentence queue: agent_loop writes char* sentences, audio_task consumes them.
// NULL pointer = sentinel (end of response).
extern QueueHandle_t g_sentence_queue;

// Start audio_task on Core 0 at priority 10.
// Call once from app_main after WiFi is connected.
void audio_task_start(void);
```

- [ ] **Step 2: Create audio_task.c (basic mode — STT→TTS without agent)**

Create `firmware-mimiclaw/main/audio_task/audio_task.c`:

```c
#include "audio_task.h"
#include "audio/audio_input.h"
#include "audio/audio_output.h"
#include "audio/vad.h"
#include "voice/voice_stt.h"
#include "voice/voice_tts.h"
#include "led/led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio_task";

QueueHandle_t g_sentence_queue = NULL;

// PCM recording buffer: 150 frames × 320 samples × 2 bytes = 96KB
// Allocate from PSRAM (8MB available on N16R8).
#define PCM_BUF_SIZE (MAX_RECORD_FRAMES * AUDIO_FRAME_SAMPLES * sizeof(int16_t))

static void audio_task_fn(void *arg)
{
    // Initialize I2S and LED
    audio_input_init();
    audio_output_init();
    led_init();
    led_set(LED_STATE_IDLE);

    // Create sentence queue (depth 16, stores char* pointers)
    g_sentence_queue = xQueueCreate(16, sizeof(char *));

    int16_t *record_buf = heap_caps_malloc(PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    int16_t frame[AUDIO_FRAME_SAMPLES];

    if (!record_buf) {
        ESP_LOGE(TAG, "FATAL: cannot allocate PCM buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio_task ready — listening for wake");

    while (1) {
        // ── IDLE: poll for wake ──────────────────────────────────────────────
        int n = audio_input_read(frame, AUDIO_FRAME_SAMPLES);
        if (n <= 0) continue;

        int32_t energy = vad_frame_energy(frame, n);
        if (!vad_update_wake(energy)) continue;

        // ── RECORDING ────────────────────────────────────────────────────────
        ESP_LOGI(TAG, "Wake detected — recording");
        led_set(LED_STATE_LISTENING);
        int total  = 0;
        int silence = 0;

        while (total < MAX_RECORD_FRAMES * AUDIO_FRAME_SAMPLES) {
            n = audio_input_read(record_buf + total, AUDIO_FRAME_SAMPLES);
            if (n <= 0) continue;
            total += n;
            energy = vad_frame_energy(record_buf + total - n, n);
            if (vad_update_silence(energy, &silence)) {
                ESP_LOGI(TAG, "Silence — stop recording (%d samples)", total);
                break;
            }
        }

        // ── STT ──────────────────────────────────────────────────────────────
        led_set(LED_STATE_WAITING);
        char text[VOICE_STT_MAX_TEXT] = {0};
        esp_err_t err = voice_stt(AUDIO_GATEWAY_STT_URL,
                                  (uint8_t *)record_buf,
                                  (size_t)(total * sizeof(int16_t)),
                                  text, sizeof(text));
        if (err != ESP_OK || text[0] == '\0') {
            ESP_LOGW(TAG, "STT failed or empty — back to idle");
            continue;
        }
        ESP_LOGI(TAG, "STT: %s", text);

        // ── BASIC MODE: directly TTS the transcription (no agent) ────────────
        // TODO Task 6: replace with g_inbound_queue push + sentence_queue consume
        voice_tts(AUDIO_GATEWAY_TTS_URL, text);

        led_set(LED_STATE_IDLE);
        ESP_LOGI(TAG, "Done — back to idle");
    }
}

void audio_task_start(void)
{
    xTaskCreatePinnedToCore(
        audio_task_fn,
        "audio_task",
        16384,         // 16KB stack — voice_stt/tts call esp_http_client_perform (~6KB stack)
        NULL,
        10,            // priority 10 (higher than telegram/ws tasks at 5)
        NULL,
        0              // Core 0
    );
}
```

- [ ] **Step 3: Wire audio_task_start into mimi.c**

Open `firmware-mimiclaw/main/mimi.c`. Find the `app_main()` function. After WiFi connects and before the main event loop, add:

```c
#include "audio_task/audio_task.h"

// In app_main(), after wifi_manager_start() and wifi connection confirmed:
audio_task_start();
```

- [ ] **Step 4: Add audio_task to CMakeLists.txt**

In `firmware-mimiclaw/main/CMakeLists.txt`, add to SRCS:
```cmake
    "audio_task/audio_task.c"
```

- [ ] **Step 5: Build and flash**

```bash
cd firmware-mimiclaw
idf.py build
idf.py -p COM3 flash monitor
```

- [ ] **Step 6: Test basic audio pipeline**

Speak near the device for 0.5s+ (louder than ambient noise). Observe serial log:
```
I (xxxx) audio_task: Wake detected — recording
I (xxxx) audio_task: Silence — stop recording (NNNN samples)
I (xxxx) voice_stt: STT result: 你好
I (xxxx) voice_tts: TTS: received NNNN PCM bytes, playing...
I (xxxx) audio_task: Done — back to idle
```
Speaker should play back a TTS echo of the transcribed text.

If STT returns empty: check gateway /stt is live (`curl http://8.133.3.7:8000/health`)
If TTS is silent: check PCM bytes > 0 in log; check audio_output GPIO pins match board

- [ ] **Step 7: Commit**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/main/audio_task/
git add firmware-mimiclaw/main/CMakeLists.txt
git add firmware-mimiclaw/main/mimi.c
git commit -m "feat: add audio_task — record→STT→TTS basic pipeline verified"
```

---

## Task 6: Wire agent_loop — g_sentence_queue + sentence splitting

**Files:**
- Modify: `firmware-mimiclaw/main/bus/message_bus.h` (add VOICE channel constant)
- Modify: `firmware-mimiclaw/main/agent/agent_loop.c` (sentence-split, write g_sentence_queue)
- Modify: `firmware-mimiclaw/main/audio_task/audio_task.c` (use inbound_queue + consume sentence_queue)

This task integrates the MimiClaw agent with the audio pipeline. After this, speech triggers the LLM and plays the AI response.

**Key design**: MimiClaw's `llm_chat_tools()` returns the full response text. We split it by Chinese punctuation after the fact (no token streaming needed). The split sentences go into `g_sentence_queue`; `audio_task` consumes them one by one.

- [ ] **Step 1: Add VOICE channel constant to message_bus.h**

Open `firmware-mimiclaw/main/bus/message_bus.h`. Find where existing channel constants are defined (e.g., `#define MIMI_CHANNEL_TELEGRAM "telegram"`). Add:

```c
#define MIMI_CHANNEL_VOICE "voice"
```

- [ ] **Step 2: Add sentence-splitting helper to agent_loop.c**

Open `firmware-mimiclaw/main/agent/agent_loop.c`. Add a helper function before `agent_loop_task`:

```c
// Split text by Chinese/English sentence boundaries.
// Writes each sentence as a heap-allocated char* to g_sentence_queue.
// Writes a NULL sentinel when done.
// Declared extern in audio_task.h — g_sentence_queue must already be created.
extern QueueHandle_t g_sentence_queue;  // defined in audio_task.c

static void dispatch_sentences_to_audio(const char *text)
{
    if (!text || text[0] == '\0' || !g_sentence_queue) return;

    const char *boundaries[] = {"。", "！", "？", "…", "\n", NULL};
    char buf[128];
    int  buf_len = 0;
    const char *p = text;

    while (*p) {
        // Find next boundary starting at p
        const char *next_boundary = NULL;
        int         boundary_len  = 1;
        for (int i = 0; boundaries[i]; i++) {
            const char *found = strstr(p, boundaries[i]);
            if (found && (!next_boundary || found < next_boundary)) {
                next_boundary = found;
                boundary_len  = strlen(boundaries[i]);
            }
        }

        if (next_boundary) {
            // Copy up to and including the boundary
            int chunk = (int)(next_boundary - p) + boundary_len;
            int copy  = chunk < (int)(sizeof(buf) - buf_len - 1)
                      ? chunk : (int)(sizeof(buf) - buf_len - 1);
            memcpy(buf + buf_len, p, copy);
            buf_len += copy;
            buf[buf_len] = '\0';
            p += chunk;
        } else {
            // No more boundaries — copy remainder
            int remaining = strlen(p);
            int copy = remaining < (int)(sizeof(buf) - buf_len - 1)
                     ? remaining : (int)(sizeof(buf) - buf_len - 1);
            memcpy(buf + buf_len, p, copy);
            buf_len += copy;
            buf[buf_len] = '\0';
            p += remaining;
        }

        // Flush if we have a boundary or the buffer is getting full (≥60 bytes)
        bool flush = (next_boundary != NULL) || (buf_len >= 60) || (*p == '\0');
        if (flush && buf_len > 0) {
            char *msg = strdup(buf);
            if (msg) xQueueSend(g_sentence_queue, &msg, pdMS_TO_TICKS(5000));
            buf_len = 0;
            buf[0]  = '\0';
        }
    }

    // Send sentinel: NULL pointer signals end of response
    char *sentinel = NULL;
    xQueueSend(g_sentence_queue, &sentinel, pdMS_TO_TICKS(5000));
}
```

- [ ] **Step 3: Call dispatch_sentences_to_audio in agent_loop_task for voice channel**

In `agent_loop_task`, after the agent produces `resp.text`, find the section that calls `message_bus_push_outbound`. Add a branch for the voice channel:

```c
// After resp.text is populated (after ReAct loop completes):
if (strcmp(msg.channel, MIMI_CHANNEL_VOICE) == 0) {
    // Voice response: split into sentences and play via audio_task
    dispatch_sentences_to_audio(resp.text);
} else {
    // Text channels: push full response to outbound queue (existing behaviour)
    // IMPORTANT: Check MimiClaw's message_bus_push_outbound() ownership contract before
    // calling free() here. If the bus copies the string internally, free() is correct.
    // If the bus takes ownership (stores the pointer), remove free() and let the bus free it.
    // Grep "push_outbound" in firmware-mimiclaw/main/bus/ to confirm before using.
    mimi_msg_t out = {0};
    strlcpy(out.channel, msg.channel, sizeof(out.channel));
    strlcpy(out.chat_id, msg.chat_id, sizeof(out.chat_id));
    out.content = strdup(resp.text);
    message_bus_push_outbound(&out);
    free(out.content);  // safe only if bus copies the string — verify first
}
```

- [ ] **Step 4: Update audio_task.c — replace echo-back with agent dispatch**

In `firmware-mimiclaw/main/audio_task/audio_task.c`, replace the `// BASIC MODE` section:

```c
// OLD (basic mode — echo back):
// voice_tts(AUDIO_GATEWAY_TTS_URL, text);

// NEW — push text to agent, consume sentence queue:
mimi_msg_t inbound = {0};
strlcpy(inbound.channel, MIMI_CHANNEL_VOICE, sizeof(inbound.channel));
strlcpy(inbound.chat_id, "voice_main", sizeof(inbound.chat_id));
inbound.content = strdup(text);   // heap copy — agent_task frees after processing
message_bus_push_inbound(&inbound, pdMS_TO_TICKS(5000));
// Note: message_bus owns inbound.content after push; do not free here

// Consume sentences from g_sentence_queue until sentinel
char *sentence;
while (xQueueReceive(g_sentence_queue, &sentence, pdMS_TO_TICKS(30000)) == pdTRUE) {
    if (sentence == NULL) break;   // sentinel = end of response
    voice_tts(AUDIO_GATEWAY_TTS_URL, sentence);
    free(sentence);
}
```

Add the required includes at the top of audio_task.c:
```c
#include "bus/message_bus.h"
```

- [ ] **Step 5: Build and flash**

```bash
cd firmware-mimiclaw
idf.py build && idf.py -p COM3 flash monitor
```

- [ ] **Step 6: Test voice → agent → speech**

Speak a clear sentence (e.g., "你好，书童"). Observe serial log:
```
I audio_task: Wake detected — recording
I voice_stt: STT result: 你好，书童
I agent_loop: processing voice channel message
I llm_proxy: HTTP POST MiniMax... response received
I audio_task: playing sentence: 你好！我是你的学习伙伴书童。
I audio_task: playing sentence: 今天想学什么？
I audio_task: Done — back to idle
```

Latency target: first sentence plays within 6-7s of speaking.

- [ ] **Step 7: Commit**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/main/bus/message_bus.h
git add firmware-mimiclaw/main/agent/agent_loop.c
git add firmware-mimiclaw/main/audio_task/audio_task.c
git commit -m "feat: wire agent_loop + g_sentence_queue — voice triggers LLM response"
```

---

## Task 7: Port config_server — SoftAP portal with MiniMax API key

**Files:**
- Create: `firmware-mimiclaw/main/config/config_store.c`
- Create: `firmware-mimiclaw/main/config/config_store.h`
- Create: `firmware-mimiclaw/main/config/config_server.c`
- Create: `firmware-mimiclaw/main/config/config_server.h`
- Modify: `firmware-mimiclaw/main/wifi/wifi_manager.c`
- Modify: `firmware-mimiclaw/main/CMakeLists.txt`
- Modify: `firmware-mimiclaw/main/mimi.c`

- [ ] **Step 1: Create config_store.h**

Create `firmware-mimiclaw/main/config/config_store.h`:
```c
#pragma once
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char gateway_url[128];   // e.g., "http://8.133.3.7:8000"
    char llm_url[128];       // e.g., "https://api.minimaxi.com/v1"
    char llm_key[256];
    char llm_model[64];      // e.g., "MiniMax-M2.5-highspeed"
} kidpal_config_t;

esp_err_t config_load(kidpal_config_t *out);
esp_err_t config_save(const kidpal_config_t *cfg);
esp_err_t config_erase(void);
```

- [ ] **Step 2: Create config_store.c**

Create `firmware-mimiclaw/main/config/config_store.c`. Copy from `firmware/main/config_store.c` and extend with LLM fields:

```c
#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define NVS_NAMESPACE "kidpal"
#define KEY_SSID      "wifi_ssid"
#define KEY_PASS      "wifi_pass"
#define KEY_URL       "gateway_url"
#define KEY_LLM_URL   "llm_url"
#define KEY_LLM_KEY   "llm_key"
#define KEY_LLM_MODEL "llm_model"

static const char *TAG = "config_store";

esp_err_t config_load(kidpal_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) return err;

    size_t len;
    bool ok = true;

    len = sizeof(out->wifi_ssid);   ok &= (nvs_get_str(h, KEY_SSID,      out->wifi_ssid,   &len) == ESP_OK);
    len = sizeof(out->wifi_pass);   ok &= (nvs_get_str(h, KEY_PASS,      out->wifi_pass,   &len) == ESP_OK);
    len = sizeof(out->gateway_url); ok &= (nvs_get_str(h, KEY_URL,       out->gateway_url, &len) == ESP_OK);
    len = sizeof(out->llm_url);     nvs_get_str(h, KEY_LLM_URL,   out->llm_url,   &len);  // optional
    len = sizeof(out->llm_key);     nvs_get_str(h, KEY_LLM_KEY,   out->llm_key,   &len);  // optional
    len = sizeof(out->llm_model);   nvs_get_str(h, KEY_LLM_MODEL, out->llm_model, &len);  // optional

    nvs_close(h);
    if (!ok) return ESP_ERR_NVS_NOT_FOUND;
    ESP_LOGI(TAG, "loaded: ssid=%s url=%s", out->wifi_ssid, out->gateway_url);
    return ESP_OK;
}

esp_err_t config_save(const kidpal_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_SSID,      cfg->wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_PASS,      cfg->wifi_pass));
    ESP_ERROR_CHECK(nvs_set_str(h, KEY_URL,       cfg->gateway_url));
    if (cfg->llm_url[0])   nvs_set_str(h, KEY_LLM_URL,   cfg->llm_url);
    if (cfg->llm_key[0])   nvs_set_str(h, KEY_LLM_KEY,   cfg->llm_key);
    if (cfg->llm_model[0]) nvs_set_str(h, KEY_LLM_MODEL, cfg->llm_model);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}
```

- [ ] **Step 3: Create config_server.c/h**

Copy `firmware/main/config_server.c` to `firmware-mimiclaw/main/config/config_server.c`.

In the HTML form, add two new fields after the gateway URL field:

```c
// Add to HTML_FORM after the gateway URL <input>:
"<label>MiniMax API Key</label>"
"<input type='password' name='llm_key'>"
"<label>Gateway URL (\xe4\xb8\x8d\xe7\x9f\xa5\xe9\x81\x93\xe5\x8f\xaf\xe7\xa9\xba\xe7\x9d\x80)</label>"
// gateway_url field already exists
```

In `post_save_handler`, add extraction of `llm_key`:
```c
get_field(body, received, "llm_key", cfg.llm_key, sizeof(cfg.llm_key));
// llm_url and llm_model use defaults if not provided:
if (cfg.llm_url[0] == '\0')
    strlcpy(cfg.llm_url, "https://api.minimaxi.com/v1", sizeof(cfg.llm_url));
if (cfg.llm_model[0] == '\0')
    strlcpy(cfg.llm_model, "MiniMax-M2.5-highspeed", sizeof(cfg.llm_model));
```

Create `firmware-mimiclaw/main/config/config_server.h`:
```c
#pragma once
// Starts SoftAP HTTP portal on port 80. Does not return.
__attribute__((noreturn)) void config_server_start(void);
```

- [ ] **Step 4: Add gpio0_held_long() and config boot check to mimi.c**

In `firmware-mimiclaw/main/mimi.c`, add before `app_main()`:

```c
#include "driver/gpio.h"
#include "config/config_store.h"
#include "config/config_server.h"

#define FACTORY_RESET_HOLD_MS 3000

static bool gpio0_held_long(void)
{
    gpio_config_t io = {
        .pin_bit_mask = BIT64(GPIO_NUM_0),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (gpio_get_level(GPIO_NUM_0) != 0) return false;
    int held = 0;
    while (gpio_get_level(GPIO_NUM_0) == 0 && held < FACTORY_RESET_HOLD_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held += 100;
    }
    return held >= FACTORY_RESET_HOLD_MS;
}
```

In `app_main()`, after NVS init, before WiFi connect:
```c
if (gpio0_held_long()) {
    config_erase();
    config_server_start();  // noreturn
}

kidpal_config_t cfg = {0};
if (config_load(&cfg) == ESP_OK) {
    // Override MimiClaw's LLM settings from config store.
    // MimiClaw reads its LLM credentials from NVS keys set by its own CLI.
    // Write the same keys here so config_server and CLI produce the same result.
    // Check firmware-mimiclaw/main/cli/ for the exact NVS namespace/key names
    // (typically "mimi_cfg" namespace with keys "api_base", "api_key", "model").
    // Example — verify key names against MimiClaw source before using:
    if (cfg.llm_url[0] || cfg.llm_key[0] || cfg.llm_model[0]) {
        nvs_handle_t h;
        if (nvs_open("mimi_cfg", NVS_READWRITE, &h) == ESP_OK) {
            if (cfg.llm_url[0])   nvs_set_str(h, "api_base",  cfg.llm_url);
            if (cfg.llm_key[0])   nvs_set_str(h, "api_key",   cfg.llm_key);
            if (cfg.llm_model[0]) nvs_set_str(h, "model",     cfg.llm_model);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}
// IMPORTANT: Verify "mimi_cfg" namespace and key names against
// firmware-mimiclaw/main/cli/cli.c before flashing. Adjust if they differ.
```

- [ ] **Step 5: Add config/ to CMakeLists.txt**

```cmake
    "config/config_store.c"
    "config/config_server.c"
```

Add `esp_http_server` to REQUIRES if not present.

- [ ] **Step 6: Build, flash, test provisioning**

```bash
idf.py build && idf.py -p COM3 flash monitor
```

Hold GPIO0 (BOOT button) for 3 seconds at boot. Observe:
```
W mimi: GPIO0 held — entering CONFIG mode
```

On phone: connect to WiFi "KidPalAI-Setup". Open `http://192.168.4.1`. Form should show WiFi + MiniMax Key fields. Enter credentials, tap Save. Device restarts and connects.

- [ ] **Step 7: Commit**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/main/config/
git add firmware-mimiclaw/main/mimi.c
git add firmware-mimiclaw/main/CMakeLists.txt
git commit -m "feat: port config_server with MiniMax API key SoftAP form"
```

---

## Task 8: Integration test — full conversation + Telegram + cron reminder

**Files:** (no code changes — test and document only)

- [ ] **Step 1: Full voice conversation test**

Flash latest firmware. Speak a question (use `demo_1.wav` content: "北京今天天气怎么样？"). Measure time from end of speech to first audio output.

Expected:
- STT: ~3s
- MiniMax M2.5-highspeed LLM: ~2-3s
- TTS first sentence: ~1s
- **First audio: ≤7s**

Record actual measurement in notes.

- [ ] **Step 2: Multi-turn conversation test**

Ask 3 follow-up questions. Verify:
- MimiClaw's session history (SPIFFS `/spiffs/sessions/voice_main.jsonl`) accumulates context
- Follow-up questions use prior context (e.g., "那穿什么衣服？" refers to weather)

- [ ] **Step 3: Telegram test**

Send a text message to the Telegram bot. Verify AI responds in Telegram AND ESP32 simultaneously speaks the response aloud (outbound queue dispatches to both channels).

Actually: Telegram responses go to Telegram only; voice responses go to speaker only. The channels are independent. Verify each works independently.

- [ ] **Step 4: Cron reminder test (manual trigger)**

Temporarily edit `spiffs_data/memory/HEARTBEAT.md` to fire in 2 minutes:
```markdown
- [ ] HH:MM 提醒叶欣羽该做作业了
```
Where HH:MM is 2 minutes from now. Reflash SPIFFS only:
```bash
idf.py -p COM3 flash --flash-target spiffs
```
Wait for trigger. Verify:
- ESP32 speaks the reminder aloud
- Telegram sends the reminder to parent (if token configured)

Restore `HEARTBEAT.md` to 18:00/20:30 and reflash.

- [ ] **Step 5: Commit final integration**

```bash
cd c:\WorkGit\KidPalAI
git add firmware-mimiclaw/spiffs_data/memory/HEARTBEAT.md
git commit -m "feat: MimiClaw integration complete — voice+Telegram+cron working"
```

- [ ] **Step 6: Update CLAUDE.md**

Add a new section to `CLAUDE.md` describing the new firmware:

```markdown
## MimiClaw Firmware (firmware-mimiclaw/)

ESP-IDF C firmware based on MimiClaw. Handles AI agent directly on ESP32.
Board: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM).

Pipeline: record → gateway/stt → MiniMax M2.5-highspeed → gateway/tts → play

Build: `cd firmware-mimiclaw && idf.py build`
Flash: `idf.py -p COM3 flash monitor`
Config: Hold GPIO0 3s at boot → SoftAP portal at http://192.168.4.1

Persona: `spiffs_data/config/SOUL.md` (书童, 叶欣羽)
Reminders: `spiffs_data/memory/HEARTBEAT.md`
Gateway STT/TTS: POST http://8.133.3.7:8000/stt and /tts
```

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md with firmware-mimiclaw build/config instructions"
```
