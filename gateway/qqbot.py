from __future__ import annotations

import asyncio
import re
import time
from collections.abc import Mapping
from typing import Any

import httpx

DEFAULT_QQ_API_BASE_URL = "https://api.sgroup.qq.com"
DEFAULT_QQ_TOKEN_URL = "https://bots.qq.com/app/getAppAccessToken"
TOKEN_REFRESH_SKEW_SECONDS = 60

EVENT_PRIVATE_TEXT = "C2C_MESSAGE_CREATE"
EVENT_GROUP_AT_TEXT = "GROUP_AT_MESSAGE_CREATE"


def _as_clean_text(value: Any) -> str:
    if value is None:
        return ""
    if not isinstance(value, str):
        return ""
    text = value
    return re.sub(r"\s+", " ", text).strip()


def _strip_bot_mention(content: str, bot_app_id: str) -> str:
    mention_pattern = re.compile(rf"<@!?{re.escape(str(bot_app_id))}>")
    without_mention = mention_pattern.sub("", content)
    return _as_clean_text(without_mention)


def _extract_mapping(obj: Any) -> Mapping[str, Any]:
    if isinstance(obj, Mapping):
        return obj
    return {}


class QQAccessTokenManager:
    def __init__(
        self,
        *,
        app_id: str,
        app_secret: str,
        token_url: str = DEFAULT_QQ_TOKEN_URL,
        timeout: float = 10.0,
    ) -> None:
        self.app_id = str(app_id)
        self.app_secret = str(app_secret)
        self.token_url = token_url
        self.timeout = timeout
        self._access_token = ""
        self._expires_at = 0.0
        self._lock = asyncio.Lock()

    def _token_valid(self) -> bool:
        return bool(self._access_token) and time.time() < self._expires_at - TOKEN_REFRESH_SKEW_SECONDS

    async def get_access_token(self, *, force_refresh: bool = False) -> str:
        if not force_refresh and self._token_valid():
            return self._access_token

        async with self._lock:
            if not force_refresh and self._token_valid():
                return self._access_token

            payload = {"appId": self.app_id, "clientSecret": self.app_secret}
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                response = await client.post(self.token_url, json=payload)
            response.raise_for_status()
            data = response.json()

            token = str(data.get("access_token", "")).strip()
            if not token:
                raise RuntimeError("qqbot token response missing access_token")

            expires_in = int(data.get("expires_in") or 7200)
            expires_in = max(expires_in, TOKEN_REFRESH_SKEW_SECONDS + 1)
            self._access_token = token
            self._expires_at = time.time() + expires_in
            return self._access_token


def normalize_qq_event(event: Mapping[str, Any], *, bot_app_id: str) -> dict[str, Any] | None:
    envelope = _extract_mapping(event)
    event_type = str(envelope.get("t", "")).strip()
    data = _extract_mapping(envelope.get("d"))

    if event_type == EVENT_PRIVATE_TEXT:
        message_id = _as_clean_text(data.get("id"))
        author = _extract_mapping(data.get("author"))
        sender_id = _as_clean_text(author.get("id"))
        content = _as_clean_text(data.get("content"))
        if not (message_id and sender_id and content):
            return None
        return {
            "channel": "qqbot",
            "chat_id": f"user:{sender_id}",
            "sender_id": sender_id,
            "message_id": message_id,
            "content": content,
            "reply_to": {
                "chat_type": "private",
                "user_openid": sender_id,
                "msg_id": message_id,
            },
        }

    if event_type == EVENT_GROUP_AT_TEXT:
        message_id = _as_clean_text(data.get("id"))
        group_openid = _as_clean_text(data.get("group_openid"))
        author = _extract_mapping(data.get("author"))
        sender_id = _as_clean_text(author.get("member_openid") or author.get("id"))
        content = _strip_bot_mention(_as_clean_text(data.get("content")), bot_app_id)
        if not (message_id and group_openid and sender_id and content):
            return None
        return {
            "channel": "qqbot",
            "chat_id": f"group:{group_openid}",
            "sender_id": sender_id,
            "message_id": message_id,
            "content": content,
            "reply_to": {
                "chat_type": "group",
                "group_openid": group_openid,
                "msg_id": message_id,
            },
        }

    return None


def normalize_event(event: Mapping[str, Any], *, bot_app_id: str) -> dict[str, Any] | None:
    return normalize_qq_event(event, bot_app_id=bot_app_id)


def build_private_reply_payload(content: str, *, msg_id: str | None = None) -> dict[str, Any]:
    payload: dict[str, Any] = {"content": _as_clean_text(content)}
    if msg_id:
        payload["msg_id"] = str(msg_id)
    return payload


def build_group_reply_payload(content: str, *, msg_id: str | None = None) -> dict[str, Any]:
    payload: dict[str, Any] = {"content": _as_clean_text(content)}
    if msg_id:
        payload["msg_id"] = str(msg_id)
    return payload


def _auth_headers(*, app_id: str, access_token: str) -> dict[str, str]:
    return {
        "Authorization": f"QQBot {access_token}",
        "X-Union-Appid": str(app_id),
        "Content-Type": "application/json",
    }


def _parse_success_response(response: httpx.Response) -> dict[str, Any]:
    if response.status_code == 204 or not response.content:
        return {}

    content_type = response.headers.get("content-type", "").lower()
    if "json" in content_type:
        return response.json()

    text = response.text.strip()
    if not text:
        return {}
    return {"text": text}


async def send_private_text(
    *,
    api_base_url: str,
    app_id: str,
    access_token: str,
    user_openid: str,
    content: str,
    msg_id: str | None = None,
    timeout: float = 10.0,
) -> dict[str, Any]:
    url = f"{api_base_url.rstrip('/')}/v2/users/{user_openid}/messages"
    payload = build_private_reply_payload(content, msg_id=msg_id)
    headers = _auth_headers(app_id=app_id, access_token=access_token)
    async with httpx.AsyncClient(timeout=timeout) as client:
        response = await client.post(url, headers=headers, json=payload)
    response.raise_for_status()
    return _parse_success_response(response)


async def send_group_text(
    *,
    api_base_url: str,
    app_id: str,
    access_token: str,
    group_openid: str,
    content: str,
    msg_id: str | None = None,
    timeout: float = 10.0,
) -> dict[str, Any]:
    url = f"{api_base_url.rstrip('/')}/v2/groups/{group_openid}/messages"
    payload = build_group_reply_payload(content, msg_id=msg_id)
    headers = _auth_headers(app_id=app_id, access_token=access_token)
    async with httpx.AsyncClient(timeout=timeout) as client:
        response = await client.post(url, headers=headers, json=payload)
    response.raise_for_status()
    return _parse_success_response(response)


class QQBotClient:
    def __init__(
        self,
        *,
        app_id: str,
        app_secret: str,
        api_base_url: str = DEFAULT_QQ_API_BASE_URL,
        token_url: str = DEFAULT_QQ_TOKEN_URL,
        timeout: float = 10.0,
    ) -> None:
        self.app_id = str(app_id)
        self.api_base_url = api_base_url
        self.timeout = timeout
        self.token_manager = QQAccessTokenManager(
            app_id=app_id,
            app_secret=app_secret,
            token_url=token_url,
            timeout=timeout,
        )

    async def _send_with_token_retry(self, sender) -> dict[str, Any]:
        token = await self.token_manager.get_access_token()
        try:
            return await sender(token)
        except httpx.HTTPStatusError as exc:
            if exc.response.status_code not in (401, 403):
                raise
        refreshed_token = await self.token_manager.get_access_token(force_refresh=True)
        return await sender(refreshed_token)

    async def send_private_reply(self, *, user_openid: str, content: str, msg_id: str | None = None) -> dict[str, Any]:
        async def sender(token: str) -> dict[str, Any]:
            return await send_private_text(
                api_base_url=self.api_base_url,
                app_id=self.app_id,
                access_token=token,
                user_openid=user_openid,
                content=content,
                msg_id=msg_id,
                timeout=self.timeout,
            )

        return await self._send_with_token_retry(sender)

    async def send_group_reply(self, *, group_openid: str, content: str, msg_id: str | None = None) -> dict[str, Any]:
        async def sender(token: str) -> dict[str, Any]:
            return await send_group_text(
                api_base_url=self.api_base_url,
                app_id=self.app_id,
                access_token=token,
                group_openid=group_openid,
                content=content,
                msg_id=msg_id,
                timeout=self.timeout,
            )

        return await self._send_with_token_retry(sender)

    async def send_reply(self, *, reply_to: Mapping[str, Any], content: str) -> dict[str, Any]:
        chat_type = str(reply_to.get("chat_type", "")).strip()
        msg_id = _as_clean_text(reply_to.get("msg_id"))
        if chat_type == "private":
            user_openid = _as_clean_text(reply_to.get("user_openid"))
            if not user_openid:
                raise ValueError("reply_to.user_openid is required for private reply")
            return await self.send_private_reply(user_openid=user_openid, content=content, msg_id=msg_id or None)
        if chat_type == "group":
            group_openid = _as_clean_text(reply_to.get("group_openid"))
            if not group_openid:
                raise ValueError("reply_to.group_openid is required for group reply")
            return await self.send_group_reply(group_openid=group_openid, content=content, msg_id=msg_id or None)
        raise ValueError(f"unsupported reply chat_type: {chat_type!r}")
