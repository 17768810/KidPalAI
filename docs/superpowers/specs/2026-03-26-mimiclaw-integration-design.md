# MimiClaw 固件集成设计文档

**日期**：2026-03-26
**状态**：待实现
**目标**：将 MimiClaw 固件作为基础，集成 KidPalAI 音频管道，实现 ESP32 直接调用 MiniMax M2.5-highspeed，首声音延迟降至约 6-7s

---

## 1. 背景与动机

### 当前架构（已优化后）

```
ESP32 → gateway → MiniMax（via packyapi）→ gateway → ESP32
         STT(3s)   LLM(10-15s)     TTS(1s)
首声音：14-19s
```

LLM 调用经过 gateway 中转（packyapi 代理），约 10-15s，仍是主要瓶颈。

### 目标架构

```
ESP32 → gateway/stt(3s) → ESP32 → MiniMax M2.5-highspeed(2-3s) → gateway/tts(1s) → ESP32
首声音：约 6-7s（逐句 TTS，每句约 1s 合成）
```

AI 代理（人设、记忆、定时提醒）运行在 ESP32 上，gateway 降为纯语音中转服务。

### 选择 MimiClaw 的原因

MimiClaw（[github.com/memovai/mimiclaw](https://github.com/memovai/mimiclaw)）已实现：
- ESP32 直连 OpenAI 兼容 API（含 MiniMax），支持流式输出
- SOUL.md / USER.md / MEMORY.md 人设系统
- cron 定时任务（18:00 做作业、20:30 睡觉提醒）
- Telegram Bot + WebSocket 文字输入
- SPIFFS 持久化记忆

缺少（需从 KidPalAI 移植）：
- I2S 麦克风输入（INMP441）
- I2S 扬声器输出（MAX98357A，PCM）
- 能量 VAD
- HTTP STT / TTS 客户端
- SoftAP 配网门户
- LED 状态指示

---

## 2. 硬件要求

| 项目 | 要求 | 当前板子 |
|------|------|---------|
| 芯片 | ESP32-S3 | ESP32-S3 ✓ |
| Flash | 16MB | N16R8（16MB）✓ |
| PSRAM | 8MB | R8（8MB）✓ |
| I2S 麦克风 | INMP441 | GPIO 已连接 ✓ |
| I2S 扬声器 | MAX98357A | GPIO 已连接 ✓ |

---

## 3. 架构设计

### 3.1 固件目录结构

```
firmware-mimiclaw/
  main/
    agent/          ← MimiClaw 原样（agent_loop，增加 sentence_queue 分发）
    llm/            ← MimiClaw 原样（配置 MiniMax M2.5-highspeed）
    telegram/       ← MimiClaw 原样
    gateway/        ← MimiClaw WebSocket（端口 18789）
    cli/            ← MimiClaw 串口配置（兼顾 MiniMax API key 录入）
    storage/        ← MimiClaw SPIFFS（SOUL.md 等）
    audio/          ← 从 KidPalAI 移植
      audio_input.c/h    INMP441，I2S_NUM_0，16kHz/16bit/mono
      audio_output.c/h   MAX98357A，I2S_NUM_1，16kHz/16bit/mono，PCM 模式
      vad.c/h            能量 VAD（阈值 200，1.5s 静音停止）
    voice/          ← 新增
      voice_stt.c/h      HTTP POST /stt，multipart PCM → JSON text
      voice_tts.c/h      HTTP POST /tts，JSON text → 完整 PCM → audio_output
    led/            ← 从 KidPalAI 移植
      led.c/h            LED_STATE_IDLE/LISTENING/WAITING/ERROR
    config_server/  ← 从 KidPalAI 移植（SoftAP HTTP 配网门户，含 API key 配置）
      config_server.c/h
      config_store.c/h
    wifi/           ← 合并两边 WiFi 初始化逻辑
    main.c          ← 新入口，启动 audio_task + MimiClaw 各任务
  spiffs_image/
    SOUL.md
    USER.md
    HEARTBEAT.md
    MEMORY.md
  CMakeLists.txt
  sdkconfig.defaults  ← 含 PSRAM、栈大小配置
  partitions.csv      ← 基于 MimiClaw，验证双 OTA + SPIFFS + 应用分区总和 ≤ 16MB
```

> **注**：使用前需验证 `partitions.csv` 的分区布局：应用程序约 2-3MB，双 OTA 各约 2MB，NVS 24KB，剩余给 SPIFFS。以实际编译大小为准。

### 3.2 任务分配与优先级

| FreeRTOS 任务 | 核心 | 优先级 | 来源 | 说明 |
|--------------|------|--------|------|------|
| audio_task | Core 0 | 10（高） | 新增 | 录音 → STT → 压队列 → 消费 sentence_queue → TTS → 播放 |
| agent_task | Core 1 | 8 | MimiClaw | LLM 调用、工具执行、sentence_queue 写入 |
| telegram_task | Core 0 | 5 | MimiClaw | Telegram Bot |
| ws_server_task | Core 0 | 5 | MimiClaw | WebSocket |
| cron_task | Core 0 | 5 | MimiClaw | 定时提醒 |

audio_task 优先级高于其他 Core 0 任务，保障 I2S DMA 不因 Telegram/WebSocket 阻塞而溢出。

### 3.3 消息总线（扩展 MimiClaw 原有机制）

```
g_inbound_queue   ← audio_task / telegram_task / ws_server_task / cron_task 写入
g_outbound_queue  → telegram_task（Telegram 推送）、ws_server_task（WebSocket 响应）

g_sentence_queue  ← agent_task 在处理语音请求时写入（每句话一条消息）[新增]
                  → audio_task 消费：逐句调用 voice_tts → 播放
```

`g_sentence_queue` 用字符串消息（动态分配，消费方释放），用 sentinel 消息（空字符串或 NULL）标志一次对话结束。

---

## 4. 流式交互核心逻辑

### 4.1 agent_loop 逐句分发（改造要点）

MimiClaw agent_loop 在处理 **语音来源** 的消息时，流式接收 MiniMax token，按句子边界切分后写入 `g_sentence_queue`；agent_task 自身不阻塞于 TTS/I2S。

```c
// agent_loop.c 修改伪代码（仅语音通道）
// 运行在 Core 1，agent_task

char sentence_buf[128];   // 128 字节，约 40 个中文字符，足够一句话
int  buf_len = 0;

void on_token(const char *token, size_t token_len) {
    // 安全追加，防止溢出
    size_t remaining = sizeof(sentence_buf) - buf_len - 1;
    size_t copy_len = token_len < remaining ? token_len : remaining;
    memcpy(sentence_buf + buf_len, token, copy_len);
    buf_len += copy_len;
    sentence_buf[buf_len] = '\0';

    // 句子边界检测（strstr 逐一匹配，正确处理 UTF-8 多字节序列）
    const char *boundaries[] = {"。", "！", "？", "…", "\n", NULL};
    bool boundary = false;
    for (int i = 0; boundaries[i]; i++) {
        if (strstr(sentence_buf, boundaries[i])) { boundary = true; break; }
    }
    // 缓冲区快满时强制切分（60字节 ≈ 20个中文字符，避免单句过长）
    bool overflow = (buf_len >= 60);

    if (boundary || overflow) {
        // 发送到 sentence_queue（深拷贝，agent_task 继续接收下一句 token）
        char *msg = strdup(sentence_buf);
        xQueueSend(g_sentence_queue, &msg, portMAX_DELAY);
        buf_len = 0;
        sentence_buf[0] = '\0';
    }
}

// 所有 token 接收完毕：flush + 发送 sentinel
void on_stream_done() {
    if (buf_len > 0) {
        char *msg = strdup(sentence_buf);
        xQueueSend(g_sentence_queue, &msg, portMAX_DELAY);
    }
    // sentinel：NULL 指针表示本次对话结束
    char *sentinel = NULL;
    xQueueSend(g_sentence_queue, &sentinel, portMAX_DELAY);
}
```

Telegram / WebSocket 输出通道不受影响，仍走全文路径写 g_outbound_queue。

### 4.2 audio_task 状态机

```
IDLE（LED_IDLE）
  → VAD 检测到唤醒（loudfram 连续 25 帧 > 阈值）
  → RECORDING（LED_LISTENING）
      → 录音，VAD 1.5s 静音停止
  → voice_stt(pcm_buf, len, text_out)
      → 成功：继续
      → 失败或空字符串：回到 IDLE
  → push text → g_inbound_queue（消息来源标记为 VOICE_CHANNEL）
  → WAITING（LED_WAITING）
  → 循环消费 g_sentence_queue：
      while true:
          msg = xQueueReceive(g_sentence_queue)
          if msg == NULL:   // sentinel，对话结束
              break
          voice_tts(msg)    // HTTP POST /tts → PCM → audio_output_write_pcm
          free(msg)
  → LED_IDLE，回到 IDLE
```

**并发说明**：audio_task 在 WAITING 状态时不录新音，避免与正在播放的 I2S 冲突。不实现打断（barge-in），作为后续优化留存。

### 4.3 voice_stt 接口

```c
// 输入：PCM 缓冲区（16kHz/16bit/mono）
// 输出：text_out 写入识别文字，ESP_OK 或错误码
// 内部：multipart/form-data POST → gateway/stt → {"text": "..."}
esp_err_t voice_stt(const uint8_t *pcm, size_t len,
                    char *text_out, size_t text_max);
// 返回 ESP_OK 但 text_out[0]=='\0' 表示识别结果为空（静音）
```

### 4.4 voice_tts 接口

```c
// 输入：UTF-8 文字（一句话）
// 内部：POST gateway/tts → 等待完整 PCM 响应 → audio_output_write_pcm()
// HTTP 模式：esp_http_client_open/write/fetch_headers/read 循环（同 voice_upload_stream 模式）
esp_err_t voice_tts(const char *text);
// timeout_ms = 10000（单句 TTS，10s 足够）
```

> **说明**：gateway /tts 使用火山引擎 TTS，返回完整 PCM 后再传输（不支持逐字节流式合成）。每句约 1s，对单句语音是可接受的。

---

## 5. Gateway 变更

### 新增端点

```python
from pydantic import BaseModel

class TTSRequest(BaseModel):
    text: str

# POST /stt
# 输入：multipart/form-data，字段 audio（PCM，16kHz/16bit/mono）
# 输出：{"text": "识别结果"} 或 HTTP 422（空识别结果）
@app.post("/stt")
async def stt(audio: UploadFile = File(...)):
    pcm_data = await audio.read()
    if not pcm_data:
        raise HTTPException(status_code=400, detail="empty audio")
    text = await transcribe_pcm(pcm_data)
    if not text.strip():
        raise HTTPException(status_code=422, detail="could not transcribe audio")
    return {"text": text}

# POST /tts
# 输入：{"text": "一句话"}
# 输出：完整 PCM（16kHz/16bit/mono），Content-Type: audio/pcm
# 注意：synthesize_pcm() 返回完整 PCM 后一次性传输，非逐字节流式
@app.post("/tts")
async def tts(body: TTSRequest):
    if not body.text.strip():
        raise HTTPException(status_code=400, detail="empty text")
    pcm = await synthesize_pcm(body.text)
    return Response(
        content=pcm,
        media_type="audio/pcm",
        headers={"X-Sample-Rate": "16000", "X-Bits": "16", "X-Channels": "1"},
    )
```

保留 `/voice/stream`（备用，回退路径）、`/health`。

---

## 6. SPIFFS 人设文件

### SOUL.md

```markdown
你是温柔耐心的AI学习伙伴，名叫"书童"。
用简单活泼的中文回答，多鼓励，不批评。
每次回答不超过3句话，不使用Markdown格式，不使用星号或特殊符号。
禁止涉及暴力、恐怖、成人内容。
```

### USER.md

```markdown
用户：叶欣羽，10岁，小学二年级
主要科目：语文、数学、英语
语言：中文
```

### HEARTBEAT.md

```markdown
- [ ] 18:00 提醒叶欣羽该做作业了
- [ ] 20:30 提醒叶欣羽准备睡觉
```

cron 触发时：agent_loop 生成提醒话语 → 写入 g_sentence_queue → audio_task 语音播报 + Telegram 推送家长。

---

## 7. 配置方式

| 配置项 | 方式 | 说明 |
|--------|------|------|
| WiFi SSID/密码 | SoftAP 配网门户（config_server） | 首次启动或长按 GPIO0 3s 进入 |
| MiniMax API Key / URL / Model | SoftAP 配网门户（config_server 扩展表单） | 与 WiFi 配置合并，减少操作步骤 |
| Telegram Token | MimiClaw 串口 CLI（`set_telegram_token`） | 家长可选功能，USB 配置一次 |
| 人设内容 | 编辑 spiffs_image/ 后重新烧录 | 或通过 SPIFFS OTA 更新 |

> **说明**：将 MiniMax API Key 加入 SoftAP 门户，避免家长需要串口 CLI 工具，降低安装门槛。

---

## 8. 延迟估算

| 阶段 | 时间 | 说明 |
|------|------|------|
| 说话结束 → gateway/stt 返回 | ~3s | 讯飞 STT，vad_eos=1000ms |
| 文字进 g_inbound_queue → MiniMax 首 token | ~0.5s | M2.5-highspeed，流式 |
| 首句话积累完成（约 15-20 字） | ~0.5s | token 积累到句号 |
| gateway/tts 合成首句 PCM | ~1s | 火山引擎 TTS，完整合成后传输 |
| **首声音延迟** | **约 5-6s** | 比现在 14-19s 快 2-3x |

> **说明**：/tts 不做流式合成（火山引擎 API 本身不支持），每句约 1s 往返。这是已知限制，不影响总体改进效果。

---

## 9. 实现顺序与回退点

1. **Gateway**：添加 `/stt` + `/tts` 端点，部署测试（2-3h）
   - ✅ 回退点：现有 `/voice/stream` 保持可用，固件无需改动

2. **MimiClaw 克隆 + 配置**：配 MiniMax 凭据，SOUL.md，跑通 Telegram 文字对话（1-2h）
   - ✅ 回退点：若 MimiClaw LLM 不可用，仍用 gateway llm.py

3. **移植 audio/**：audio_input、audio_output、vad（2-3h）

4. **新增 voice/**：voice_stt、voice_tts（2-3h）

5. **audio_task 基础版**：录音 → STT → 打印文字 → TTS 播放（无 agent，直接验证音频链路）（2-3h）
   - ✅ 回退点：音频链路验证通过后再接入 agent

6. **接入 agent_loop**：g_inbound_queue + g_sentence_queue + 逐句 TTS（2-3h）

7. **移植 config_server**：扩展 SoftAP 表单含 MiniMax API Key（1-2h）

8. **集成测试**：完整语音对话、Telegram、定时提醒（2-4h）

**预估总工时：15-22h（2-3 天）**

---

## 10. 关键风险与缓解

| 风险 | 可能性 | 缓解 |
|------|--------|------|
| MiniMax M2.5-highspeed 不支持真流式（token-by-token） | 中 | 降级为批量模式：等全文后写一次 g_sentence_queue，延迟约 7-8s，仍优于现状 |
| MimiClaw agent_loop 未暴露 on_token 回调 | 中 | 参考 llm/ 源码添加 token callback；或采用批量模式先跑通 |
| Core 0 Telegram HTTP 阻塞导致音频抖动 | 低 | audio_task 优先级 10 > Telegram 5；I2S DMA 缓冲可抗短暂抢占 |
| partitions.csv 分区越界 | 低 | Step 2 时验证实际编译大小，必要时缩减 SPIFFS（8MB 也够存人设文件） |
| config_server 与 MimiClaw WiFi 初始化冲突 | 低 | 统一到 wifi.c，config_server 调 wifi_init_sta（与现有 KidPalAI 模式相同） |
