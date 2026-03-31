# MimiClaw QQ Bot Bridge Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add LLM `base_url` support, extend the firmware HTTP configuration portal, and implement a Tencent official QQ Bot bridge using the cloud gateway as webhook ingress and relay.

**Architecture:** `gateway/` terminates QQ webhooks and holds QQ platform integration logic. `firmware-mimiclaw` remains the agent runtime and connects outward to the gateway over WebSocket to receive inbound QQ messages and send outbound replies. LLM endpoint selection is moved from a provider-only switch to a configurable URL model with build-time defaults and NVS overrides.

**Tech Stack:** ESP-IDF 5.5 C firmware, ESP HTTP/WebSocket client/server components, NVS, FastAPI, Python async WebSocket handling, Tencent official QQ Bot Open Platform APIs, pytest

---

### Task 1: Extend firmware config schema for LLM base URL and QQ bridge settings

**Files:**
- Modify: `firmware-mimiclaw/main/mimi_config.h`
- Modify: `firmware-mimiclaw/main/mimi_secrets.h.example`
- Modify: `firmware-mimiclaw/main/bus/message_bus.h`

**Step 1: Add failing compile references in downstream code plan**

Target new constants and keys:

- `MIMI_SECRET_BASE_URL`
- `MIMI_SECRET_QQ_APP_ID`
- `MIMI_SECRET_QQ_APP_SECRET`
- `MIMI_SECRET_QQ_GATEWAY_WS_URL`
- `MIMI_NVS_QQ`
- `MIMI_NVS_KEY_BASE_URL`
- `MIMI_NVS_KEY_QQ_APP_ID`
- `MIMI_NVS_KEY_QQ_APP_SECRET`
- `MIMI_NVS_KEY_QQ_GATEWAY_WS_URL`
- `MIMI_CHAN_QQBOT`

**Step 2: Add minimal definitions**

Define the new build-time defaults and NVS keys in `mimi_config.h`, and document the new fields in `mimi_secrets.h.example`.

**Step 3: Run firmware build**

Run: `idf.py build`

Expected: build passes with new config symbols available.

**Step 4: Commit**

```bash
git add firmware-mimiclaw/main/mimi_config.h firmware-mimiclaw/main/mimi_secrets.h.example firmware-mimiclaw/main/bus/message_bus.h
git commit -m "feat(firmware): add qq bridge and llm base url config keys"
```

### Task 2: Extend local onboarding/admin portal schema

**Files:**
- Modify: `firmware-mimiclaw/main/onboard/onboard_html.h`
- Modify: `firmware-mimiclaw/main/onboard/wifi_onboard.c`

**Step 1: Add the new UI fields**

Add:

- `base_url` under LLM
- `qq_app_id`
- `qq_app_secret`
- `qq_gateway_ws_url`

**Step 2: Extend `/config` output**

Expose effective values from NVS/build-time defaults.

**Step 3: Extend `/save` handling**

Persist the new values through the existing `nvs_sync_field()` path.

**Step 4: Rebuild firmware**

Run: `idf.py build`

Expected: build passes and no missing symbols from the new portal fields.

**Step 5: Manual smoke test**

1. Flash firmware
2. Open `http://192.168.4.1`
3. Verify all new fields render
4. Save values and verify they remain visible after reboot

**Step 6: Commit**

```bash
git add firmware-mimiclaw/main/onboard/onboard_html.h firmware-mimiclaw/main/onboard/wifi_onboard.c
git commit -m "feat(firmware): extend onboarding portal for qq bridge and llm base url"
```

### Task 3: Refactor firmware LLM endpoint selection to support configurable base URL

**Files:**
- Modify: `firmware-mimiclaw/main/llm/llm_proxy.c`
- Modify: `firmware-mimiclaw/main/llm/llm_proxy.h`

**Step 1: Write a focused URL resolution helper**

Add a helper that resolves:

- effective full request URL
- host
- path

based on:

- configured `base_url`
- provider fallback when `base_url` is empty

**Step 2: Ensure direct HTTPS code uses the resolved URL**

Update the `esp_http_client` path to use the effective URL.

**Step 3: Ensure proxy-mode code uses resolved host/path**

Update tunnel requests so OpenAI-compatible requests can be sent to non-default hosts.

**Step 4: Add guardrails**

If the configured URL is malformed or too long, log and fall back safely or fail fast with a clear error.

**Step 5: Rebuild firmware**

Run: `idf.py build`

Expected: build passes and generated firmware links successfully.

**Step 6: Manual verification**

Set:

- provider `openai`
- base_url to an OpenAI-compatible endpoint

Expected: request logs show the configured host/path, not hard-coded `api.openai.com`.

**Step 7: Commit**

```bash
git add firmware-mimiclaw/main/llm/llm_proxy.c firmware-mimiclaw/main/llm/llm_proxy.h
git commit -m "feat(firmware): support configurable llm base url"
```

### Task 4: Add gateway-side QQ webhook and device bridge tests

**Files:**
- Create: `gateway/tests/test_qqbot.py`
- Create: `gateway/tests/test_qqbot_bridge.py`

**Step 1: Write failing QQ webhook normalization tests**

Cover:

- valid private text event normalization
- valid group `@bot` text event normalization
- unsupported event ignored

**Step 2: Write failing bridge session tests**

Cover:

- firmware registration by `qq_app_id`
- message routed to active session
- missing session returns a controlled error path

**Step 3: Run tests to confirm failure**

Run:

```bash
cd gateway
python -m pytest tests/test_qqbot.py tests/test_qqbot_bridge.py -v
```

Expected: FAIL due to missing implementation.

**Step 4: Commit**

```bash
git add gateway/tests/test_qqbot.py gateway/tests/test_qqbot_bridge.py
git commit -m "test(gateway): add failing qq bot bridge tests"
```

### Task 5: Implement QQ platform integration in gateway

**Files:**
- Create: `gateway/qqbot.py`
- Modify: `gateway/requirements.txt`

**Step 1: Implement access token management**

Add QQ platform token fetch/cache logic.

**Step 2: Implement webhook parsing helpers**

Normalize incoming QQ events into one internal message envelope.

**Step 3: Implement send-message helpers**

Support private reply and group reply paths needed by the normalized envelope.

**Step 4: Run gateway tests**

Run:

```bash
cd gateway
python -m pytest tests/test_qqbot.py -v
```

Expected: PASS

**Step 5: Commit**

```bash
git add gateway/qqbot.py gateway/requirements.txt
git commit -m "feat(gateway): add qq bot api integration"
```

### Task 6: Implement gateway webhook route and firmware WebSocket bridge

**Files:**
- Modify: `gateway/main.py`

**Step 1: Add firmware WebSocket bridge route**

Accept registration messages and track active sessions by `qq_app_id`.

**Step 2: Add QQ webhook route**

Validate/parse event, resolve destination session, and forward the normalized payload.

**Step 3: Add outbound response handling**

Accept firmware reply envelopes and call QQ send APIs.

**Step 4: Run bridge tests**

Run:

```bash
cd gateway
python -m pytest tests/test_qqbot_bridge.py -v
```

Expected: PASS

**Step 5: Run full gateway test subset**

Run:

```bash
cd gateway
python -m pytest tests/test_qqbot.py tests/test_qqbot_bridge.py -v
```

Expected: PASS

**Step 6: Commit**

```bash
git add gateway/main.py
git commit -m "feat(gateway): add qq webhook relay and firmware bridge"
```

### Task 7: Implement firmware QQ bridge client

**Files:**
- Create: `firmware-mimiclaw/main/channels/qqbot/qqbot_bridge.h`
- Create: `firmware-mimiclaw/main/channels/qqbot/qqbot_bridge.c`
- Modify: `firmware-mimiclaw/main/CMakeLists.txt`

**Step 1: Add failing integration references**

Reference `qqbot_bridge_init()`, `qqbot_bridge_start()`, and `qqbot_bridge_send_message()` from the main app flow so missing symbols surface immediately.

**Step 2: Implement configuration loading**

Load `qq_app_id`, `qq_app_secret`, and `qq_gateway_ws_url` from NVS/build defaults.

**Step 3: Implement WebSocket client lifecycle**

Add:

- connect
- register
- receive message
- reconnect

**Step 4: Implement inbound mapping**

Convert gateway JSON envelopes into `mimi_msg_t` with channel `qqbot`.

**Step 5: Implement outbound bridge send**

Wrap firmware replies into bridge JSON and transmit back to gateway.

**Step 6: Rebuild firmware**

Run: `idf.py build`

Expected: build passes with new bridge module compiled in.

**Step 7: Commit**

```bash
git add firmware-mimiclaw/main/channels/qqbot/qqbot_bridge.h firmware-mimiclaw/main/channels/qqbot/qqbot_bridge.c firmware-mimiclaw/main/CMakeLists.txt
git commit -m "feat(firmware): add qq bot bridge client"
```

### Task 8: Wire QQ channel into runtime startup and outbound dispatch

**Files:**
- Modify: `firmware-mimiclaw/main/mimi.c`

**Step 1: Initialize and start the bridge**

Start it alongside other network channels after WiFi is ready.

**Step 2: Extend outbound dispatch**

Handle `MIMI_CHAN_QQBOT` by forwarding to `qqbot_bridge_send_message()`.

**Step 3: Rebuild firmware**

Run: `idf.py build`

Expected: PASS

**Step 4: Manual end-to-end smoke test**

1. Start `gateway`
2. Power on device with QQ bridge configured
3. Confirm registration in gateway logs
4. Send QQ private message
5. Confirm message reaches firmware logs
6. Confirm reply is sent back to QQ

**Step 5: Commit**

```bash
git add firmware-mimiclaw/main/mimi.c
git commit -m "feat(firmware): wire qq bot bridge into runtime"
```

### Task 9: Update user-facing documentation

**Files:**
- Modify: `firmware-mimiclaw/README.md`
- Modify: `firmware-mimiclaw/README_CN.md`
- Modify: `firmware-mimiclaw/docs/WIFI_ONBOARDING_AP.md`
- Create: `firmware-mimiclaw/docs/im-integration/QQBOT_SETUP.md`

**Step 1: Document new config fields**

Add `base_url`, QQ credentials, and gateway bridge URL.

**Step 2: Document deployment prerequisites**

Explain that QQ inbound messaging requires the cloud gateway webhook.

**Step 3: Document first-release limitations**

Clarify that only text private chat and group mention are supported.

**Step 4: Commit**

```bash
git add firmware-mimiclaw/README.md firmware-mimiclaw/README_CN.md firmware-mimiclaw/docs/WIFI_ONBOARDING_AP.md firmware-mimiclaw/docs/im-integration/QQBOT_SETUP.md
git commit -m "docs: add qq bot bridge setup and llm base url guidance"
```

### Task 10: Final verification

**Files:**
- Test: `gateway/tests/test_qqbot.py`
- Test: `gateway/tests/test_qqbot_bridge.py`

**Step 1: Run gateway tests**

```bash
cd gateway
python -m pytest tests/test_qqbot.py tests/test_qqbot_bridge.py -v
```

Expected: PASS

**Step 2: Run firmware build**

Run: `idf.py build`

Expected: PASS

**Step 3: Run integrated smoke test**

Verify:

- local portal shows `base_url` and QQ settings
- firmware connects to gateway bridge
- QQ private message reaches `agent_loop`
- firmware reply is delivered to QQ

**Step 4: Final commit**

```bash
git add gateway/ firmware-mimiclaw/
git commit -m "feat: add qq bot gateway bridge and configurable llm base url"
```
