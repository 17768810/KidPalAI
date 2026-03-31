# KidPalAI 小书童

> 基于 ESP32-S3 + 云端 AI 的儿童语音伴读助手

小书童是一款运行在 ESP32-S3 开发板上的 AI 语音伴读设备，小孩说出唤醒词后即可自由对话，设备将语音上传至云端，经过语音识别 → AI 大脑 → 语音合成的全链路处理后，将回答播放给孩子。支持学习规划、作业提醒、正面激励、知识问答等场景，适合 3–14 岁多年龄段，通过配置文件灵活切换 AI 人设。

---

## 功能特性

- **语音唤醒**：说"嘿书童"或"小书童"触发对话，无需按键
- **自由对话**：支持课程问答、故事讲述、知识百科等任意对话
- **学习提醒**：可配置定时提醒（做作业、休息、喝水）
- **正面激励**：AI 以温柔、鼓励的语气回应，避免批评
- **多年龄适配**：通过修改孩子档案，切换不同年龄段的对话风格
- **低延迟**：STT/TTS 均调用 API，配合流式处理，响应延迟约 1.5–2.5 秒
- **LED 状态指示**：绿色（待机）/ 蓝色（录音中）/ 黄色（等待响应）/ 红色（错误）

---

## 系统架构

```
┌─────────────────────────────────┐
│         ESP32-S3 设备            │
│                                 │
│  INMP441 麦克风 (I2S)            │
│    ↓ 唤醒词检测 (ESP-SR)          │
│  录音 PCM → HTTP POST /voice    │
│    ↑ 接收 MP3                   │
│  MAX98357A 功放 (I2S) → 喇叭    │
└──────────────┬──────────────────┘
               ↕ WiFi / HTTPS
┌──────────────┴──────────────────┐
│         云服务器 (VPS)            │
│                                 │
│  Nginx (HTTPS 反向代理)          │
│    ↓                            │
│  语音网关 FastAPI                │
│    ├── 讯飞 STT API → 文字       │
│    ├── OpenClaw → AI 回复        │
│    └── 火山引擎 TTS API → MP3    │
│                                 │
│  OpenClaw (Docker)              │
│    └── Claude / DeepSeek LLM   │
└─────────────────────────────────┘
```

### 调用流程

```
1. 小孩说"嘿书童"
2. ESP32-S3 本地检测到唤醒词（ESP-SR WakeNet）
3. 开始录音，能量 VAD 检测到 1.5s 静音后停止
4. 将 PCM 音频 HTTP POST 到云端 /voice 接口
5. 云端：讯飞 STT 将音频转为文字
6. 云端：文字发送给 OpenClaw，获得 AI 回复
7. 云端：火山引擎 TTS 将回复合成为 MP3
8. MP3 返回给 ESP32-S3，通过喇叭播放
```

---

## 项目结构

```
KidPalAI/
├── CLAUDE.md                   # Claude Code 操作指引
├── docker-compose.yml          # 一键启动云端所有服务
├── .env.example                # 环境变量模板
├── nginx/
│   └── nginx.conf              # HTTPS 反向代理配置
├── gateway/                    # 云端语音网关（Python）
│   ├── main.py                 # FastAPI 入口：/health、/voice
│   ├── stt.py                  # 讯飞语音识别客户端
│   ├── tts.py                  # 火山引擎语音合成客户端
│   ├── llm.py                  # OpenClaw LLM 客户端
│   ├── requirements.txt
│   ├── Dockerfile
│   └── tests/                  # 单元测试（9 个，全部通过）
├── openclaw-data/
│   └── child_profile.md        # 孩子档案 & AI 人设配置
├── firmware/                   # ESP32-S3 固件（C / ESP-IDF）
│   ├── CMakeLists.txt
│   ├── README.md               # 固件安装与烧录指南
│   └── main/
│       ├── main.c              # 主循环：唤醒→录音→上传→播放
│       ├── wifi.c/h            # WiFi 连接与断线重连
│       ├── led.c/h             # 三色 LED 状态指示
│       ├── audio_input.c/h     # I2S 麦克风驱动（INMP441）
│       ├── audio_output.c/h    # I2S 功放驱动（MAX98357A）
│       ├── voice_upload.c/h    # HTTP multipart 音频上传
│       └── Kconfig.projbuild   # menuconfig 配置项
└── docs/
    ├── OVERVIEW.md             # 项目总览与行动清单
    └── plans/                  # 设计文档与实施计划
```

---

## 硬件清单

| 组件 | 型号 | 用途 | 参考价格 |
|------|------|------|---------|
| 主控 | ESP32-S3 | WiFi + 音频处理 + 唤醒词检测 | ¥25–40 |
| 麦克风 | INMP441 | I2S 数字麦克风 | ¥10–15 |
| 功放 | MAX98357A | I2S 数字功放 | ¥10–15 |
| 喇叭 | 3W 8Ω | 音频输出 | ¥5–10 |
| LED | 三色 LED | 状态指示 | ¥2 |
| **合计** | | | **约 ¥52–82** |

### 接线方式

| ESP32-S3 | INMP441（麦克风）| MAX98357A（功放）|
|----------|----------------|----------------|
| GPIO 12  | SCK            |                |
| GPIO 11  | WS             |                |
| GPIO 10  | SD             |                |
| GPIO 6   |                | BCLK           |
| GPIO 5   |                | LRC            |
| GPIO 4   |                | DIN            |
| GPIO 1   | LED 红         |                |
| GPIO 2   | LED 绿         |                |
| GPIO 3   | LED 蓝         |                |

---

## 部署指南

### 前置准备

1. **申请 API Key**

   | 服务 | 地址 | 免费额度 |
   |------|------|---------|
   | 讯飞语音识别 | https://console.xfyun.cn/app/myapp | 每天 500 次 |
   | 火山引擎 TTS | https://console.volcengine.com/speech/service/8 | 每月 200 万字 |

2. **准备 VPS**：1 核 2 GB 即可，需安装 Docker 和 Docker Compose，并绑定域名

### 云端部署（VPS）

```bash
# 1. 克隆
git clone <repo-url>
cd KidPalAI

# 2. 申请 SSL 证书
sudo certbot certonly --standalone -d yourdomain.com

# 3. 配置环境变量
cp .env.example gateway/.env
# 编辑 gateway/.env，填入以下内容：
#   XUNFEI_APP_ID / XUNFEI_API_KEY / XUNFEI_API_SECRET
#   VOLC_ACCESS_KEY / VOLC_APP_ID
#   OPENCLAW_WEBCHAT_URL

# 4. 替换域名（nginx.conf 和 .env 中的 yourdomain.com）
sed -i 's/yourdomain.com/你的域名/g' nginx/nginx.conf

# 5. 启动所有服务
docker compose up -d

# 6. 验证
curl https://你的域名/health
# 期望输出：{"status":"ok"}
```

### OpenClaw 配置（独立部署）

本项目使用独立安装的 OpenClaw 作为 AI 大脑（非 Docker 内置），通过 HTTP `/v1/chat/completions` 接口对接。

#### 1. 开启 HTTP 接口 & 远程访问

编辑服务器上的 `~/.openclaw/openclaw.json`：

```json
{
  "gateway": {
    "bind": "custom",
    "customBindHost": "0.0.0.0",
    "auth": {
      "token": "your-gateway-token"
    },
    "http": {
      "endpoints": {
        "chatCompletions": {
          "enabled": true
        }
      }
    }
  }
}
```

安装为系统服务并启动：

```bash
openclaw gateway install
openclaw gateway start
```

验证接口：

```bash
curl -X POST http://<服务器IP>:<端口>/v1/chat/completions \
  -H "Authorization: Bearer <your-gateway-token>" \
  -H "Content-Type: application/json" \
  -d '{"model":"agent:main:main","messages":[{"role":"user","content":"你好"}],"stream":false}'
```

#### 2. 配置环境变量

在 `gateway/.env` 中填写：

```env
OPENCLAW_WEBCHAT_URL=http://<服务器IP>:<端口>/v1/chat/completions
OPENCLAW_API_KEY=<your-gateway-token>
OPENCLAW_MODEL=agent:main:main
```

#### 3. AI 人设配置（Workspace 文件）

OpenClaw 通过 `~/.openclaw/workspace/` 目录下的 Markdown 文件加载 AI 人设和用户信息：

| 文件 | 用途 |
|------|------|
| `IDENTITY.md` | AI 名字、角色类型、风格定义 |
| `SOUL.md` | 语言风格、回答规则、禁止话题、每日提醒 |
| `USER.md` | 孩子姓名、年龄、年级、科目、时区 |

**IDENTITY.md 示例：**

```markdown
- **Name:** 书童
- **Creature:** AI 学习伙伴
- **Vibe:** 温柔耐心、活泼鼓励、简单易懂
- **Emoji:** 📚
```

**SOUL.md 示例：**

```markdown
你是"书童"——叶欣羽的 AI 学习伙伴。温柔耐心，用简单活泼的语言和她交流。

**说话风格：**
- 语言简单活泼，适合10岁小学生
- 多鼓励，避免批评
- 每次回答不超过3句话

**禁止话题：** 暴力、恐怖、成人内容

**每日提醒：**
- 18:00 提醒做作业
- 21:30 提醒准备睡觉
```

**USER.md 示例：**

```markdown
- **Name:** 叶欣羽
- **What to call them:** 欣羽
- **Timezone:** Asia/Shanghai
- **Notes:** 10岁，小学二年级，主要科目：语文、数学、英语
```

修改文件后无需重启，OpenClaw 下次对话时自动读取。若需立即生效：

```bash
openclaw gateway restart
```

### 孩子档案配置

编辑 `openclaw-data/child_profile.md`，根据孩子的实际情况修改：

```markdown
## 小书童设定
- 角色：温柔耐心的AI学习伙伴，名叫"书童"
- 当前用户：小明，8岁，小学二年级
- 主要科目：语文、数学、英语
- 语言风格：简单活泼，多鼓励，避免批评，每次回答不超过3句话
- 每天提醒：18:00 做作业，20:30 准备睡觉
```

修改后重启 OpenClaw 使配置生效：

```bash
docker compose restart openclaw
```

### 固件烧录（ESP32-S3）

```bash
# 1. 安装 ESP-IDF v5.x
# 参考：https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/

# 2. 安装 esp-sr 唤醒词组件
cd firmware
idf.py add-dependency "espressif/esp-sr^1.0.0"

# 3. 配置 WiFi 和服务器地址
idf.py set-target esp32s3
idf.py menuconfig
# 进入 "KidPalAI Configuration"，填写：
#   - WiFi SSID
#   - WiFi Password
#   - Voice Gateway URL（https://你的域名/voice）

# 4. 编译并烧录
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

烧录成功后，LED 绿灯常亮表示 WiFi 已连接，设备已就绪。

---

## 本地开发（网关）

```bash
cd gateway

# 安装依赖
pip install -r requirements.txt

# 运行测试（无需真实 API Key）
python -m pytest tests/ -v

# 启动开发服务器（需要 gateway/.env）
python -m uvicorn main:app --reload --port 8000
```

---

## 技术栈

| 层级 | 技术 |
|------|------|
| 硬件 | ESP32-S3 + INMP441 + MAX98357A |
| 固件 | ESP-IDF v5 + ESP-SR + ESP-ADF（C 语言）|
| 语音网关 | Python 3.12 + FastAPI + asyncio |
| 语音识别 | 讯飞实时语音转写 WebSocket API |
| 语音合成 | 火山引擎大模型语音合成 API |
| AI 大脑 | OpenClaw（自托管）+ Claude / DeepSeek |
| 部署 | Docker Compose + Nginx + Let's Encrypt |

---

## 后续计划

- [ ] 接入 ESP-SR WakeNet 实现真正的离线唤醒词检测
- [ ] 实现 ESP-ADF MP3 播放管道（当前为 stub）
- [ ] 第二期：OpenClaw Cron Job 定时提醒（作业、休息）
- [ ] 第三期：学习记录持久化，家长端通知（微信/Telegram）
