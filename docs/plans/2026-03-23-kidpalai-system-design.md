# KidPalAI 小书童 - 系统设计文档

**日期：** 2026-03-23
**状态：** 已确认

---

## 1. 项目概述

用 ESP32-S3 + 麦克风 + 喇叭，对接云服务器上的 OpenClaw AI 服务，做一个帮助小孩学习规划、提醒、正面激励的 AI 伴读小书童。

**交互模式：** 语音唤醒 → 自由对话 → 语音回答（类似小爱同学）
**目标用户：** 多年龄段（3–14岁），通过 OpenClaw 档案配置切换人设

---

## 2. 系统架构（方案 A：薄客户端 + 云端全链路）

```
┌─────────────────────────────────┐
│         ESP32-S3 设备            │
│                                 │
│  INMP441(I2S) → 唤醒词检测(ESP-SR)│
│       ↓ 触发                    │
│  录音 PCM → HTTP POST /voice    │
│       ↑                        │
│  收到 MP3 → MAX98357A(I2S) 播放  │
└─────────────────────────────────┘
              ↕ WiFi/Internet
┌─────────────────────────────────┐
│         云服务器（1核 2GB）       │
│                                 │
│  ① 语音网关 (Python FastAPI)     │
│     - 接收 PCM 音频              │
│     - 调用 STT → 得到文字         │
│     - 调用 OpenClaw API → 得到回复│
│     - 调用 TTS → 得到 MP3        │
│     - 返回 MP3 给 ESP32          │
│                                 │
│  ② OpenClaw 服务（Docker）       │
│     - 接入 Claude / DeepSeek LLM│
│     - 持久记忆（孩子档案）         │
│     - Skills：提醒、学习规划等     │
│                                 │
│  ③ STT：讯飞语音识别 API          │
│  ④ TTS：火山引擎 TTS API         │
│  ⑤ Nginx（反向代理 + HTTPS）     │
└─────────────────────────────────┘
```

---

## 3. 硬件方案

| 组件 | 型号 | 用途 | 参考价格 |
|------|------|------|---------|
| 主控 | ESP32-S3 | WiFi + 音频处理 + ESP-SR 唤醒词 | ¥25–40 |
| 麦克风 | INMP441 | I2S 数字麦克风 | ¥10–15 |
| 功放+喇叭 | MAX98357A | I2S 数字功放 | ¥15–25 |
| LED | 三色LED | 状态指示 | ¥2 |
| **合计** | | | **¥52–82** |

### 硬件接线

```
ESP32-S3        INMP441（麦克风）
GPIO 12    →    SCK
GPIO 11    →    WS
GPIO 10    →    SD

ESP32-S3        MAX98357A（功放）
GPIO 6     →    BCLK
GPIO 5     →    LRC
GPIO 4     →    DIN
```

---

## 4. 云端服务栈

### 4.1 语音网关（Python FastAPI）

- `POST /voice`：接收 ESP32 上传的 PCM 音频
- 调用讯飞 STT API → 文字
- ��用 OpenClaw WebChat API → AI 回复文字
- 调用火山引擎 TTS API → MP3
- 返回 MP3 流给 ESP32

**延迟优化：** 流式 STT + 流式 TTS，目标总延迟 1.5–2.5s

### 4.2 API 选型

| 组件 | 选型 | 免费额度 | 理由 |
|------|------|---------|------|
| STT | 讯飞语音识别 | 每天 500 次 | 中文准确率最高 |
| TTS | 火山引擎 TTS | 每月 200 万字 | 中文音色最自然 |
| LLM | Claude / DeepSeek（via OpenClaw） | 按量付费 | OpenClaw 统一管理 |

### 4.3 OpenClaw 孩子档案配置示例

```markdown
## 小书童设定
- 角色：温柔耐心的AI学习伙伴，名叫"书童"
- 当前用户：[孩子姓名]，[年龄]岁，[年级]
- 主要科目：语文、数学、英语
- 语言风格：简单活泼，多鼓励，避免批评
- 每天提醒：18:00 做作业，20:30 准备睡觉
```

### 4.4 部署方式

Docker Compose 管理所有服务：
- `openclaw`：OpenClaw 容器
- `voice-gateway`：Python FastAPI 语音网关
- `nginx`：反向代理 + HTTPS（Let's Encrypt）

---

## 5. ESP32-S3 固件流程

```
上电启动
   ↓
连接 WiFi → 成功：LED绿灯常亮
   ↓
┌─────────────────────────────────┐
│         待机监听循环              │
│  INMP441 持续采集音频            │
│  ESP-SR WakeNet 检测唤醒词       │
│  支持："嘿书童" / "小书童"        │
└────────────┬────────────────────┘
             ↓ 检测到唤醒词
         LED 蓝灯闪烁（录音中）
             ↓
     VAD 检测说话结束（静音 > 1s）
             ↓
     PCM 音频 HTTP POST → /voice
             ↓
         LED 黄灯（等待响应）
             ↓
     接收 MP3 数据流
             ↓
     MAX98357A I2S 播放
         LED 绿灯恢复
             ↓
         回到待机
```

**开发框架：** ESP-IDF（C语言）+ ESP-SR + ESP-ADF

**固件功能清单：**
- WiFi 连接 + 断线自动重连
- ESP-SR WakeNet 唤醒词检测
- I2S 录音 → PCM buffer
- HTTP POST 音频到云端网关
- 接收并 I2S 播放 MP3
- LED 三色状态指示

---

## 6. 功能迭代计划

### MVP 第一期（核心对话）
- 唤醒词触发对话
- 自由问答（作业题目、讲故事、百科知识）
- 孩子基础档案配置（姓名、年级、科目）

### 第二期（学习助手）
- 定时提醒（作业、休息、喝水）—— OpenClaw Cron Job
- 正面激励话术（完成作业后表扬）
- 多年龄段人设切换（通过 OpenClaw 档案配置）

### 第三期（进阶）
- 学习记录（OpenClaw 持久记忆，记住孩子薄弱科目）
- 家长端查看对话记录（OpenClaw Telegram/微信通知）
- 自定义唤醒词训练

---

## 7. 技术栈汇总

| 层级 | 技术 |
|------|------|
| 硬件 | ESP32-S3 + INMP441 + MAX98357A |
| 固件 | ESP-IDF + ESP-SR + ESP-ADF（C语言）|
| 语音网关 | Python FastAPI |
| STT | 讯飞语音识别 API |
| TTS | 火山引擎 TTS API |
| AI 大脑 | OpenClaw + Claude / DeepSeek |
| 部��� | Docker Compose + Nginx + Let's Encrypt |
| 服务器 | 1核 2GB VPS（国内，低延迟）|
