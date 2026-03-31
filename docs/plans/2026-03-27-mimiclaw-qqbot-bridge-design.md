# MimiClaw QQ Bot Bridge Design

**Date:** 2026-03-27
**Status:** Approved

---

## 1. Goal

Extend `firmware-mimiclaw` so the local HTTP configuration service supports:

- LLM `BASE_URL` configuration
- new build-time default LLM settings for OpenAI Codex
- new QQ Bot credentials and gateway bridge settings

Add a new QQ Bot channel using Tencent official QQ Bot Open Platform, with the cloud `gateway/` service acting as the public webhook ingress and protocol bridge.

---

## 2. Problem Statement

The current `firmware-mimiclaw` architecture already supports:

- local onboarding/admin portal over HTTP
- runtime config persistence in NVS with build-time defaults from `main/mimi_secrets.h`
- inbound/outbound message routing through `message_bus`
- network channels for Telegram, Feishu, and local WebSocket

However, it does not support:

- overriding the LLM API endpoint with a configurable `BASE_URL`
- QQ Bot credentials or QQ-specific bridge settings in the HTTP config flow
- Tencent official QQ Bot as a first-class inbound/outbound channel
- a cloud-side webhook relay for QQ events

Tencent official QQ Bot is not a good direct fit for ESP32-S3 because the platform-facing ingress should be a public HTTPS webhook. The board should not be exposed directly to the public internet.

---

## 3. Chosen Architecture

### 3.1 Recommended Split

Use `gateway/` as the public-facing QQ ingress and relay:

1. Tencent QQ Bot platform sends webhook events to `gateway`
2. `gateway` validates the request and normalizes the message
3. `firmware-mimiclaw` opens an outbound WebSocket connection to `gateway`
4. `gateway` pushes normalized QQ messages to the correct device session
5. firmware converts the message into existing `message_bus` input
6. `agent_loop` generates the response
7. firmware sends the outbound reply back to `gateway` over the same bridge
8. `gateway` calls Tencent QQ send APIs

### 3.2 Why This Approach

- Keeps public webhook and QQ platform complexity in Python on the server
- Keeps ESP32-S3 in an outbound-only network posture
- Reuses the existing firmware message bus and channel dispatch model
- Avoids redesigning the agent architecture around a cloud-only channel manager

---

## 4. Configuration Model

### 4.1 LLM Defaults

The firmware must support a configurable `base_url` for the LLM provider.

Requested defaults:

- provider: `openai`
- api_key: set via `mimi_secrets.h`
- base_url: `https://www.packyapi.com/v1`
- model: `gpt-5.3-codex-xhigh`

Implementation rule:

- runtime NVS value wins
- if NVS is empty, fall back to `mimi_secrets.h`
- if both are empty, fall back to provider-derived built-in URL

### 4.2 QQ Bot Defaults

QQ defaults will be supported through the existing build-time secrets layer and runtime NVS override path.

Requested defaults:

- `qq_app_id`
- `qq_app_secret`

For safety and consistency with the existing architecture, the actual secret values should live in:

- `firmware-mimiclaw/main/mimi_secrets.h`
- `gateway/.env`

They should not be duplicated into general-purpose documentation beyond naming the fields.

### 4.3 New Config Fields

Firmware HTTP config portal and NVS should expose:

- `base_url`
- `qq_app_id`
- `qq_app_secret`
- `qq_gateway_ws_url`

The `qq_gateway_ws_url` is required because the board must know where to open the bridge connection.

---

## 5. Firmware Design

### 5.1 New Channel

Add a first-class channel constant:

- `qqbot`

This channel behaves like Telegram and Feishu from the perspective of the message bus and outbound dispatcher.

### 5.2 QQ Bridge Client

Add a firmware-side QQ bridge client module that:

- loads QQ and bridge config from NVS/build-time defaults
- opens an outbound WebSocket connection to `qq_gateway_ws_url`
- registers the device and its `qq_app_id`
- receives normalized QQ inbound messages
- converts them into `mimi_msg_t`
- sends outbound `qqbot` replies back to the gateway

The bridge client should own:

- reconnect logic
- device registration handshake
- parsing minimal bridge protocol messages
- sending outbound reply envelopes

### 5.3 Message Mapping

Inbound gateway payload fields:

- `type`
- `channel`
- `chat_id`
- `message_id`
- `sender_id`
- `reply_to`
- `content`

Mapped into firmware:

- `msg.channel = "qqbot"`
- `msg.chat_id = "user:<id>"` or `group:<id>`
- `msg.content = text body`

QQ metadata that does not fit in `mimi_msg_t` should be retained in the bridge module long enough to support reply routing.

### 5.4 LLM Base URL

The existing `llm_proxy.c` derives URLs from provider constants only. This must change to:

1. use configured `base_url` when available
2. derive host and path from the URL for proxy-mode requests
3. fall back to provider defaults when no configured base URL exists

This requires refactoring URL handling so both direct HTTPS calls and proxy tunnel calls share the same resolved endpoint.

---

## 6. Gateway Design

### 6.1 Public HTTP Webhook

Add a QQ webhook route to `gateway/main.py`.

Responsibilities:

- receive Tencent official QQ Bot platform events
- validate webhook signature / request authenticity
- ignore unsupported event types
- normalize supported text events into a compact internal payload

### 6.2 Device Session Bridge

Add a firmware-facing WebSocket route to `gateway`.

Responsibilities:

- accept firmware bridge connections
- authenticate/register by `qq_app_id`
- keep an in-memory mapping from `qq_app_id` to active device session
- forward inbound normalized QQ events to the correct device
- accept outbound reply envelopes from firmware
- call QQ official send APIs

### 6.3 First Release Scope

Support:

- direct/private text messages
- group text messages that mention the bot

Ignore for first release:

- attachments
- voice
- image handling
- advanced card messages
- channel/forum-specific message variants unless they map cleanly to the same API shape

---

## 7. HTTP Portal UX Changes

The local onboarding/admin UI should add:

- `Base URL` under LLM Configuration
- `QQ Bot` section with `App ID`, `App Secret`, `Gateway WS URL`

The UI should continue to use the current `/config` and `/save` JSON model.

No major visual redesign is required; this is a schema extension only.

---

## 8. File-Level Impact

Firmware files expected to change:

- `firmware-mimiclaw/main/mimi_config.h`
- `firmware-mimiclaw/main/mimi_secrets.h.example`
- `firmware-mimiclaw/main/onboard/onboard_html.h`
- `firmware-mimiclaw/main/onboard/wifi_onboard.c`
- `firmware-mimiclaw/main/llm/llm_proxy.c`
- `firmware-mimiclaw/main/llm/llm_proxy.h`
- `firmware-mimiclaw/main/bus/message_bus.h`
- `firmware-mimiclaw/main/mimi.c`
- `firmware-mimiclaw/main/CMakeLists.txt`
- new `firmware-mimiclaw/main/channels/qqbot/*`

Gateway files expected to change:

- `gateway/main.py`
- new `gateway/qqbot.py`
- `gateway/requirements.txt`
- new `gateway/tests/test_qqbot.py`
- possibly new `gateway/tests/test_qqbot_bridge.py`

---

## 9. Risks

### 9.1 Secret Placement

The requested defaults include real credential values. Keeping those values only in the secrets/config layer reduces accidental spread through unrelated source files and docs.

### 9.2 QQ API Complexity

Tencent official QQ Bot has more event variants than Telegram. The first release should stay narrow and support only text conversations.

### 9.3 Firmware WebSocket Reliability

The bridge client needs reconnection and idempotent registration handling. Without that, QQ delivery will be fragile.

### 9.4 URL Handling

Supporting configurable `base_url` means the firmware can no longer assume a fixed host/path pair for OpenAI-compatible requests. URL parsing must be explicit and tested.

---

## 10. Validation Strategy

Firmware:

- verify `/config` returns the new fields
- verify `/save` persists and reboots cleanly
- verify `llm_proxy` uses `base_url` when configured
- verify QQ bridge connection registers and can enqueue inbound messages

Gateway:

- verify QQ webhook parsing and validation
- verify session registration by `qq_app_id`
- verify routing inbound QQ text to the registered firmware session
- verify firmware outbound response triggers the QQ send API call

End-to-end:

1. QQ user sends text to bot
2. `gateway` receives webhook
3. firmware receives bridged message
4. `agent_loop` produces a reply
5. `gateway` sends reply back to QQ

---

## 11. Non-Goals

The first release does not include:

- media upload/download
- voice message transcription from QQ
- multiple-device fanout for a single `qq_app_id`
- persistent cloud queueing when the board is offline
- migration of all channel logic into `gateway`

