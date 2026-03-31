# QQ Bot Bridge Setup Guide

This guide walks through connecting MimiClaw to Tencent official QQ Bot using the cloud `gateway/` service as webhook ingress and relay.

## Overview

The QQ integration is split across two runtimes:

- `gateway/` receives Tencent QQ webhook events on the public internet
- firmware opens an outbound WebSocket connection to `gateway` and receives normalized inbound messages
- firmware replies over the same bridge, and `gateway` sends the final QQ API response

This keeps the ESP32-S3 in an outbound-only network posture. The board does not need a public HTTPS endpoint.

## What You Need

- a Tencent official QQ Bot app with `app_id` and `app_secret`
- a public `gateway/` deployment that can expose:
  - `POST /qqbot/webhook`
  - `WS /qqbot/ws`
- a MimiClaw firmware build with Wi-Fi working
- either build-time secrets in `main/mimi_secrets.h` or access to the local onboarding/admin portal

## Firmware Configuration

You can configure QQ bridge values at build time in `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_MODEL_PROVIDER   "openai"
#define MIMI_SECRET_BASE_URL         ""
#define MIMI_SECRET_QQ_APP_ID        ""
#define MIMI_SECRET_QQ_APP_SECRET    ""
#define MIMI_SECRET_QQ_GATEWAY_WS_URL "wss://gateway.example.com/qqbot/ws"
```

Or configure them at runtime from the local admin portal at `http://192.168.4.1`:

- `Base URL`
- `QQ App ID`
- `QQ App Secret`
- `QQ Gateway WS URL`

`base_url` is optional. Leave it empty to keep the provider default endpoint.

## Gateway Requirements

The firmware-side `QQ Gateway WS URL` only covers the board-to-gateway bridge. You still need the cloud gateway to be configured for Tencent QQ webhook delivery.

At minimum:

- deploy `gateway/main.py`
- ensure Tencent QQ can reach the gateway's `/qqbot/webhook`
- ensure the board can reach `/qqbot/ws`
- configure the matching QQ app secret in the gateway environment, or let the device send `qq_app_secret` during bridge registration

## Verification

1. Start the cloud `gateway/` service.
2. Power on the board with valid Wi-Fi and QQ bridge configuration.
3. Confirm the gateway logs show the device registered on `/qqbot/ws`.
4. Send a private text message to the QQ bot.
5. Confirm the firmware logs show a `qqbot` inbound message entering the agent loop.
6. Confirm the reply is delivered back to QQ.

For group chats, mention the bot explicitly. The first release only forwards group text that includes an `@bot` mention.

## First Release Scope

Supported:

- private text messages
- group text messages that mention the bot
- outbound text replies routed through the gateway bridge

Not supported yet:

- images, files, voice, or cards
- persistent cloud queueing while the device is offline
- multi-device fanout for one `qq_app_id`

## Security Notes

- QQ credentials should stay in `main/mimi_secrets.h`, the local admin portal, or the gateway environment, not in general documentation.
- Restrict access to the local onboarding/admin page because it can store `qq_app_secret` on the device.
- Protect the public gateway with Tencent webhook validation and any shared-secret checks you enable in `gateway/`.
