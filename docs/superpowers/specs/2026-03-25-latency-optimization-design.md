# KidPalAI 延迟优化设计文档

**日期**：2026-03-25
**状态**：已批准（经审阅修订）
**目标**：端到端感知延迟从 50s 降至 < 5s

---

## 1. 问题分析

### 当前延迟分解

注：当前 `audio_output_play_mp3()` 是存根（函数体为空），实际播放时间为 0ms，下表中播放时间待实现后才成立。

| 阶段 | 耗时 | 说明 |
|------|------|------|
| ESP32 录音 + VAD | 2–3s | 正常，VAD 1.5s 静音触发 |
| PCM 上传 | 0.5–1s | 网络传输 |
| STT（讯飞 WebSocket） | 3–5s | `vad_eos=5000`（5s 等待）可优化 |
| **LLM（MiniMax-M2.5 冷启动）** | **30–40s** | **主要瓶颈** |
| TTS（火山引擎） | 2–5s | 等完整文本才调用 |
| MP3 下载（当前存根，实为 0） | 0–1s | 播放未实现 |
| **合计** | **~40–50s** | 与实测吻合 |

### 根本原因

1. **LLM 冷启动**：OpenClaw/MiniMax-M2.5 session 首次初始化耗时 30-40s，热请求 3-8s。
2. **串行等待**：LLM 生成完整回复后才调 TTS，TTS 完成后才传给 ESP32，没有任何流水线。
3. **MP3 解码未实现**：ESP32 端 `audio_output_play_mp3()` 是存根；本次优化通过服务端直接输出 PCM 绕过此问题。
4. **STT vad_eos 过长**：`stt.py` 发送 `vad_eos=5000`（5 秒），可降至 1000ms。

---

## 2. 优化目标

- **感知延迟**（说完话到听到第一个字）：< 5s
- **实际总处理时间**：< 15s（包括完整回答播放完毕）
- **改动风险**：分阶段，每阶段独立可验证

---

## 3. 方案设计

### 阶段一：Session 预热 + STT 调优（快速止血）

**改动文件**：`gateway/main.py`、`gateway/llm.py`、`gateway/stt.py`

**原理**：
1. FastAPI 启动时向 OpenClaw 发心跳请求（`max_tokens=1`，极低消耗），预热 session
2. 每 5 分钟心跳防止 session 过期
3. STT `vad_eos` 从 5000 降至 1000ms

**预期效果**：总延迟 50s → ~15s

**实现要点**：

```python
# gateway/main.py
@app.on_event("startup")
async def startup_event():
    asyncio.create_task(_warmup_and_keepalive())

async def _warmup_and_keepalive():
    await asyncio.sleep(2)  # 等服务完全就绪
    try:
        await _call_openclaw("你好", max_tokens=1)  # 极低消耗
    except Exception as e:
        logger.warning(f"LLM warmup failed: {e}")
    while True:
        await asyncio.sleep(300)
        try:
            await _call_openclaw("你好", max_tokens=1)
        except Exception:
            pass
```

```python
# gateway/stt.py — 修改 vad_eos
"vad_eos": 1000,   # 从 5000 改为 1000ms
```

> **注意**：心跳只调 LLM，不触发 TTS，避免额外费用。`max_tokens=1` 确保 OpenClaw 只生成一个 token。

**部署**：`docker compose up -d --build`，单 worker：`CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000", "--workers", "1"]`（心跳任务只在单 worker 中才可靠运行）

---

### 阶段二：全链路流式管道（大幅降低感知延迟）

**改动文件**：`gateway/main.py`、`gateway/llm.py`、`gateway/tts.py`、`gateway/requirements.txt`、`gateway/Dockerfile`、`firmware/main/voice_upload.c`、`firmware/main/audio_output.c`、`firmware/main/main.c`

#### 2a. TTS 直接输出 PCM（关键优化）

火山引擎 TTS API 支持直接返回 PCM（`encoding: "pcm"`），**无需 pydub/ffmpeg**：

```python
# gateway/tts.py — 修改 encoding
payload = {
    ...
    "audio": {
        "voice_type": "zh_female_maomao_story_moon_bigtts",
        "encoding": "pcm",        # 从 "mp3" 改为 "pcm"
        "sample_rate": 16000,     # 必须与 ESP32 I2S_NUM_1 配置一致
        "bits": 16,
        "channel": 1,
    }
}
# 返回值：base64 PCM（无需解码，直接 base64.b64decode 即可）
```

**PCM 参数合约**（网关 → ESP32 全链路统一）：
- 采样率：16000 Hz
- 位深：16-bit signed little-endian
- 声道：1（mono）
- ESP32 I2S_NUM_1 必须按此配置初始化（见 2d 节）

> **验证步骤**：阶段二开始前，先用 Python 脚本调火山引擎 TTS，`encoding=pcm`，确认返回数据用 `aplay -r 16000 -f S16_LE -c 1` 可正常播放。

这样 `tts_to_pcm()` 函数简化为：

```python
async def synthesize_pcm(text: str) -> bytes:
    """调 TTS API 直接返回 16kHz 16bit mono PCM"""
    raw = await _call_tts(text, encoding="pcm")
    return base64.b64decode(raw["data"]["audio"])
```

不需要新增 `pydub` 或 `ffmpeg` 依赖。

#### 2b. LLM 流式输出

```python
# gateway/llm.py — 新增 stream_openclaw()
async def stream_openclaw(text: str) -> AsyncGenerator[str, None]:
    """流式调用 LLM，按中文句子边界 yield"""
    payload = {
        "model": MODEL,
        "messages": [...],
        "stream": True,          # 关键：开启流式
        "max_tokens": 500,
    }
    buffer = ""
    SENTENCE_END = re.compile(r"[。！？…\n]")  # 句尾标点
    MAX_BUFFER = 60  # 最多积累 60 字符强制切句（防止无标点超长句）

    async with httpx.AsyncClient(timeout=None) as client:  # streaming 不设 full-response timeout
        async with client.stream("POST", OPENCLAW_URL, json=payload, ...) as resp:
            async for line in resp.aiter_lines():
                if not line.startswith("data: "):
                    continue
                delta = parse_sse_delta(line)
                if delta:
                    buffer += delta
                    if SENTENCE_END.search(buffer) or len(buffer) >= MAX_BUFFER:
                        yield buffer.strip()
                        buffer = ""
    if buffer.strip():
        yield buffer.strip()
```

**句子切割规则**：
- 遇到 `。！？…\n` 立即切句
- 超过 60 字符强制切句（防止无标点长句）
- 每句最少 5 字符（防止标点连续造成空句）

#### 2c. Gateway 流式端点

```python
# gateway/main.py — 新增 /voice/stream
@app.post("/voice/stream")
async def voice_stream(audio: UploadFile = File(...)):
    pcm = await audio.read()
    text = await transcribe_pcm(pcm)       # STT，同旧

    async def generate():
        async for sentence in stream_openclaw(text):
            if len(sentence) < 5:
                continue
            pcm_bytes = await synthesize_pcm(sentence)  # TTS → PCM 直接
            yield pcm_bytes

    return StreamingResponse(
        generate(),
        media_type="audio/pcm",
        headers={"X-Sample-Rate": "16000", "X-Bits": "16", "X-Channels": "1"}
    )
```

原 `/voice` 端点保留，作为 fallback。

#### 2d. ESP32 固件：流式接收 PCM 写 I2S

**关键变更**：使用 `esp_http_client_open` + `esp_http_client_read` 循环（非 `perform`），实现真正流式接收。

**`firmware/main/voice_upload.c`**（新函数 `voice_upload_stream`）：

```c
#define PCM_CHUNK_BYTES 640   // 20ms @ 16kHz 16bit mono

esp_err_t voice_upload_stream(const uint8_t *pcm_data, int pcm_len,
                               voice_pcm_callback_t on_pcm_chunk) {
    esp_http_client_config_t cfg = {
        .url            = get_config_gateway_url_stream(), // /voice/stream
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 0,   // 0 = 不超时，流式响应需要长连接
        .buffer_size    = PCM_CHUNK_BYTES * 4,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    // 设置 multipart 请求头和 body
    // ... (multipart 编码同旧 voice_upload.c)

    ESP_ERROR_CHECK(esp_http_client_open(client, total_body_len));
    esp_http_client_write(client, body, total_body_len);

    // 读响应状态码
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) { ... }

    // 流式读响应，逐 chunk 回调
    uint8_t chunk[PCM_CHUNK_BYTES];
    int bytes_read;
    while ((bytes_read = esp_http_client_read(client, (char*)chunk, PCM_CHUNK_BYTES)) > 0) {
        on_pcm_chunk(chunk, bytes_read);  // 回调写 I2S
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}
```

**回调函数（在 `main.c` 中定义）**：

```c
static void pcm_chunk_cb(const uint8_t *data, int len) {
    audio_output_write_pcm(data, len);
}
```

**`firmware/main/audio_output.c`**：

```c
// I2S_NUM_1 初始化配置（新增，确保与 PCM 参数合约一致）
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),   // 16kHz，与 TTS 输出一致
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_MONO),               // MONO，修复原 STEREO 错误
    .gpio_cfg = { /* 同原配置 */ },
};

esp_err_t audio_output_write_pcm(const uint8_t *data, size_t len) {
    size_t written = 0;
    return i2s_channel_write(tx_handle, data, len, &written, portMAX_DELAY);
}
```

> **重要**：
> - 槽模式从 `STEREO` 改为 `MONO`：原配置写入单声道 PCM 到立体声通道会导致音调降低 50%
> - 采样率 16000 Hz 与 TTS PCM 参数合约严格对齐
> - ESP32 I2S 写入是连续字节流，不需要识别句子边界——多句 PCM 拼接后直接逐字节写入，I2S 硬件连续播放，无需帧头或分隔符

**`firmware/main/main.c`** 状态机更新：

```c
case STATE_WAITING:
    set_led(LED_YELLOW);
    ret = voice_upload_stream(record_buf, record_len, pcm_chunk_cb);
    // voice_upload_stream 内部流式写 I2S，此函数返回时播放已结束
    state = (ret == ESP_OK) ? STATE_IDLE : STATE_IDLE;
    break;
```

---

## 4. 数据流总图（优化后）

```
ESP32                          Gateway (8.133.3.7)            外部 API
─────                          ──────────────────            ──────────
IDLE
  │ 检测到音频能量
  ▼
RECORDING (VAD 1.5s)
  │
  ▼
WAITING
  │ POST /voice/stream (PCM)
  │ ─────────────────────────────────────────────►
  │                            STT (讯飞, vad_eos=1s)──►
  │                            ◄──────────────────────
  │                            LLM stream (stream=True)►
  │                            ◄── token by token ─────
  │                            按句切割 (。！？, max 60字)
  │                            每句 TTS (PCM 输出) ──────►
  │                            ◄── PCM bytes ──────────
  │ ◄── PCM chunk (640B) ─────
  │ I2S_NUM_1 MONO write
  ▼ → 喇叭出声 (~3-5s 后)
PLAYING（通过回调，边收边放）
  │ ◄── 更多 PCM chunks ──────
  │ I2S write 继续
  │ HTTP 流结束
  ▼
IDLE
```

---

## 5. 预期延迟（优化后）

| 阶段 | 当前 | 阶段一后 | 阶段二后 |
|------|------|---------|---------|
| 录音 + VAD | 2–3s | 2–3s | 2–3s |
| STT | 3–5s | 2–3s（vad_eos=1s） | 2–3s |
| LLM 首句 | 30–40s（冷）| 3–8s（热）| 2–3s（流式+热）|
| TTS 首句 | 3s | 3s | 0.5–1s（单句） |
| **首声播出（感知延迟）** | **~50s** | **~10–15s** | **~3–5s** |
| 完整回答结束 | N/A（存根）| ~20s | ~12–15s |

---

## 6. 实施计划

### 阶段一（1–2 小时）

1. `gateway/stt.py`：`vad_eos` 从 5000 → 1000
2. `gateway/main.py`：加 `startup_event` + `_warmup_and_keepalive`（`max_tokens=1`）
3. `gateway/llm.py`：`_call_openclaw` 支持 `max_tokens` 参数
4. 部署（单 worker）：`docker compose up -d --build`
5. 测试：Python 脚本发音频，测量 LLM 响应时间对比

### 阶段二（1–2 天）

1. `gateway/tts.py`：加 `synthesize_pcm()` 函数（`encoding=pcm`）
2. `gateway/llm.py`：实现 `stream_openclaw()`（`stream=True`，SSE 解析）
3. `gateway/main.py`：加 `/voice/stream` 端点
4. `firmware/main/audio_output.c`：
   - I2S 槽模式从 STEREO → MONO
   - 加 `audio_output_write_pcm()`
5. `firmware/main/voice_upload.c`：实现 `voice_upload_stream()`（`open/read` 循环，`timeout_ms=0`）
6. `firmware/main/main.c`：更新状态机，加 `pcm_chunk_cb`
7. `idf.py build && idf.py flash`
8. 端到端测试

---

## 7. 风险与规避

| 风险 | 概率 | 规避措施 |
|------|------|---------|
| LLM streaming SSE 格式不标准 | 中 | 先用 curl 测试 OpenClaw stream 响应格式 |
| I2S MONO 改动影响录音端 | 低 | 录音用 I2S_NUM_0，播放用 I2S_NUM_1，互不影响 |
| `timeout_ms=0` 导致 ESP32 永久阻塞 | 中 | 在回调层计时，超过 30s 没有新 chunk 则主动关闭 |
| TTS API `encoding=pcm` 实际不支持 | 低 | 先用 Python 脚本测试火山引擎 PCM 输出 |
| 句子切割导致 TTS 停顿 | 中 | 调整 MAX_BUFFER（60字），不够可降至 30 |
| LLM streaming API 不稳定 | 中 | 原 `/voice` 端点保留作 fallback |

---

## 8. 不在范围内

- STT 换供应商（讯飞已足够）
- LLM 换模型（用户明确保留 MiniMax）
- 唤醒词集成（ESP-SR，独立 TODO）
- 多 worker / 分布式 keepalive
- 完整对话历史管理优化
