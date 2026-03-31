# KidPalAI 小书童 — 项目总览

## 项目结构

```
KidPalAI/
├── docker-compose.yml          # 一键启动所有服务
├── nginx/nginx.conf            # HTTPS 反向代理
├── .env.example                # API Key 模板
├── gateway/                    # 云端语音网关
│   ├── main.py                 # FastAPI 入口，/health + /voice
│   ├── stt.py                  # 讯飞 STT 客户端
│   ├── tts.py                  # 火山引擎 TTS 客户端
│   ├── llm.py                  # OpenClaw LLM 客户端
│   ├── requirements.txt
│   ├── Dockerfile
│   └── tests/                  # 9 个单元测试，全部通过
├── openclaw-data/
│   └── child_profile.md        # 孩子档案（AI人设配置）
└── firmware/                   # ESP32-S3 固件
    ├── CMakeLists.txt
    ├── README.md               # 安装 & 烧录指南
    └── main/
        ├── main.c              # 主循环：唤醒→录音→上传→播放
        ├── wifi.c/h            # WiFi 连接 + 断线重连
        ├── led.c/h             # 三色 LED 状态指示
        ├── audio_input.c/h     # I2S INMP441 麦克风
        ├── audio_output.c/h    # I2S MAX98357A 功放
        ├── voice_upload.c/h    # HTTP multipart 上传
        └── Kconfig.projbuild   # menuconfig 配置项
```

---

## 下一步行动清单

| # | 步骤 | 说明 |
|---|------|------|
| 1 | **申请 API Key** | [讯飞控制台](https://console.xfyun.cn) 申请语音识别，[火山引擎](https://console.volcengine.com/speech) 申请 TTS |
| 2 | **配置环境变量** | 复制 `.env.example` → `gateway/.env`，填入讯飞、火山引擎、OpenClaw 的 Key |
| 3 | **部署云端服务** | VPS 上执行 `docker compose up -d`，用 `curl https://yourdomain.com/health` 验证 |
| 4 | **安装 ESP-IDF** | 按 [官方文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) 安装 v5.x，执行 `idf.py set-target esp32s3` |
| 5 | **配置固件** | 执行 `idf.py menuconfig`，填写 WiFi SSID/密码 和 Gateway URL |
| 6 | **集成唤醒词** | 安装 [esp-sr](https://github.com/espressif/esp-sr) 组件，取消注释 `main.c` 中的 ESP-SR 代码块 |
| 7 | **集成 MP3 播放** | 安装 [ESP-ADF](https://github.com/espressif/esp-adf)，实现 `audio_output.c` 中的 `audio_output_play_mp3()` |
| 8 | **烧录测试** | 执行 `idf.py -p /dev/ttyUSB0 flash monitor`，说"嘿书童"验证完整链路 |

---

## 各 API 申请入口

| 服务 | 控制台地址 | 免费额度 |
|------|-----------|---------|
| 讯飞语音识别 (STT) | https://console.xfyun.cn/app/myapp | 每天 500 次 |
| 火山引擎 TTS | https://console.volcengine.com/speech/service/8 | 每月 200 万字 |
| OpenClaw | 自建，参考 https://docs.openclaw.ai | 自托管 |

---

## 关键文档参考

| 文档 | 地址 |
|------|------|
| ESP-IDF 入门指南 | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/ |
| ESP-SR 唤醒词 | https://github.com/espressif/esp-sr |
| ESP-ADF 音频框架 | https://docs.espressif.com/projects/esp-adf/en/latest/ |
| 讯飞 WebSocket STT API | https://www.xfyun.cn/doc/asr/voicedictation/API.html |
| 火山引擎 TTS API | https://www.volcengine.com/docs/6561/79817 |
| OpenClaw 文档 | https://docs.openclaw.ai |
