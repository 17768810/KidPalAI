# Wi-Fi AP Onboarding Guide

This guide documents the Wi-Fi onboarding flow for firmware builds that include the MimiClaw onboarding portal.

## What It Does

When onboarding is enabled, MimiClaw can expose a local Wi-Fi access point such as `MimiClaw-XXXX` and serve a configuration page at `http://192.168.4.1`.

Typical uses:

- first-time Wi-Fi setup without editing firmware
- updating saved Wi-Fi, model, `base_url`, proxy, or bot credentials later
- recovering a device that can no longer join the previous router

## Requirements

- a firmware build with the Wi-Fi onboarding portal enabled
- an ESP32-S3 board powered on and booted normally
- a phone or laptop that can join the temporary `MimiClaw-XXXX` network

## First-Time Setup

1. Power on the device.
2. Wait for the onboarding hotspot `MimiClaw-XXXX` to appear.
3. Join that hotspot from your phone or laptop.
4. Open `http://192.168.4.1` if the captive page does not open automatically.
5. Fill in at least:
   - `SSID`
   - `Password`
6. Add optional settings if needed:
   - `API Key`
   - `Model`
   - `Provider`
   - `Base URL`
   - `Telegram`
   - `Feishu`
   - `QQ App ID`
   - `QQ App Secret`
   - `QQ Gateway WS URL`
   - `Proxy`
   - `Search`
7. Click `Save & Restart`.
8. Wait for the device to reboot and join your normal Wi-Fi network.

If you plan to use QQ, the device still needs a reachable cloud gateway. The portal only stores firmware-side settings; QQ inbound traffic is received by `gateway/qqbot/webhook` and relayed to the board over `qq_gateway_ws_url`.

## Updating Settings Later

If the firmware keeps the admin AP online after normal Wi-Fi connection, you can reconnect to `MimiClaw-XXXX` later and open `http://192.168.4.1` again.

The page should prefill the currently effective configuration so you can edit only the fields you want to change.

## Config Priority

The onboarding page follows the same config priority as the firmware:

1. saved NVS values
2. build-time defaults from `main/mimi_secrets.h`

That means:

- if a field exists in NVS, it overrides the build-time default
- if you clear a saved field and reboot, the device falls back to `main/mimi_secrets.h`
- if both NVS and build-time values are empty, the field stays empty

For LLM endpoints specifically:

- `base_url` lets you point OpenAI-compatible traffic at a non-default host
- leaving `base_url` empty falls back to the provider-derived default URL

## Clearing Saved Values

On firmware versions that support clearing fields from the portal:

- leaving a field blank and saving removes the corresponding NVS key
- after reboot, the page shows the build-time fallback if one exists

If you need to wipe all saved runtime settings, use:

```text
mimi> config_reset
```

## Troubleshooting

### No `MimiClaw-XXXX` hotspot appears

- verify that the running firmware actually includes the onboarding portal
- if the device already connects successfully and does not keep the admin AP online, clear Wi-Fi config and reboot
- confirm the board has finished booting before scanning for Wi-Fi

### The page still shows old values

- refresh the page manually
- reconnect to the AP and open `http://192.168.4.1` again
- if needed, restart the browser once to clear stale captive portal state

### The build-time provider/model do not show up

If the device already has `model` or `provider` saved in NVS, the saved values win over `main/mimi_secrets.h`.

To return to build-time defaults:

- clear those fields in the portal and save, or
- run `config_reset`

### QQ is configured but messages do not arrive

- verify that `QQ Gateway WS URL` points to the gateway's `/qqbot/ws` endpoint
- verify that the cloud gateway is also exposing `/qqbot/webhook` to Tencent QQ
- confirm the board can reach the gateway over the configured Wi-Fi network
- note that the first release only supports private text messages and group `@bot` text messages

## Notes

- The onboarding AP is typically local-only and intended for nearby configuration.
- Current onboarding implementations may use an open AP for simplicity, so avoid leaving it exposed longer than necessary.
- If your deployment needs stronger local protection, add an AP password before using the flow in production.
- QQ bot credentials entered here are stored on-device for bridge registration, so protect access to the local admin page.
