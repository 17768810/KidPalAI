# IM Integration Guides

Configuration guides for MimiClaw's instant messaging channel integrations.

## Guides

| Guide | Service | Description |
|-------|---------|-------------|
| [Feishu Setup](FEISHU_SETUP.md) | [Feishu / Lark](https://open.feishu.cn/) | Feishu bot channel — receive and send messages via Feishu |
| [QQ Bot Setup](QQBOT_SETUP.md) | [Tencent QQ Bot](https://bot.q.qq.com/) | QQ bot bridge — webhook ingress in `gateway/`, outbound WebSocket bridge to firmware |

## Overview

MimiClaw supports multiple IM channels for interacting with the AI agent. Each guide below walks through obtaining API credentials, configuring MimiClaw (build-time or runtime), and verifying the integration.

Credentials can be set in two ways, depending on the channel and field:

1. **Build-time** — define in `main/mimi_secrets.h` and rebuild
2. **Runtime** — use serial CLI commands or the local onboarding/admin portal (saved to NVS flash, no rebuild needed)

See [mimi_secrets.h.example](../../main/mimi_secrets.h.example) for the full list of configurable secrets.
