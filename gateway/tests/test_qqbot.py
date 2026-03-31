import importlib
import importlib.util
import sys
from contextlib import contextmanager
from pathlib import Path

import pytest

GATEWAY_DIR = Path(__file__).resolve().parents[1]
QQBOT_PATH = GATEWAY_DIR / "qqbot.py"


@contextmanager
def gateway_import_path():
    sys.path.insert(0, str(GATEWAY_DIR))
    try:
        yield
    finally:
        try:
            sys.path.remove(str(GATEWAY_DIR))
        except ValueError:
            pass


def load_normalizer():
    if not QQBOT_PATH.exists():
        pytest.fail(f"qqbot module is not implemented yet: {QQBOT_PATH} is missing")
    spec = importlib.util.spec_from_file_location("gateway_qqbot_test", QQBOT_PATH)
    if not spec or not spec.loader:
        pytest.fail("qqbot module could not be loaded")
    qqbot = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = qqbot
    try:
        with gateway_import_path():
            spec.loader.exec_module(qqbot)
    except ModuleNotFoundError as exc:
        if exc.name == "qqbot":
            pytest.fail(f"qqbot module is not implemented yet: {exc}")
        raise
    finally:
        sys.modules.pop(spec.name, None)
    for name in ("normalize_qq_event", "normalize_event"):
        fn = getattr(qqbot, name, None)
        if callable(fn):
            return fn
    pytest.fail("qqbot normalization helper is not implemented yet")


def test_normalize_private_text_event():
    normalize_qq_event = load_normalizer()
    event = {
        "t": "C2C_MESSAGE_CREATE",
        "d": {
            "id": "msg-private-1",
            "author": {"id": "user-123"},
            "content": "你好，小书童",
        },
    }

    normalized = normalize_qq_event(event, bot_app_id="102893854")

    assert isinstance(normalized, dict)
    assert normalized["channel"] == "qqbot"
    assert normalized["chat_id"] == "user:user-123"
    assert normalized["sender_id"] == "user-123"
    assert normalized["message_id"] == "msg-private-1"
    assert normalized["content"] == "你好，小书童"
    assert normalized["reply_to"]


def test_normalize_group_mention_text_event():
    normalize_qq_event = load_normalizer()
    event = {
        "t": "GROUP_AT_MESSAGE_CREATE",
        "d": {
            "id": "msg-group-1",
            "group_openid": "group-456",
            "author": {"member_openid": "member-9"},
            "content": "<@!102893854>   讲个故事",
        },
    }

    normalized = normalize_qq_event(event, bot_app_id="102893854")

    assert isinstance(normalized, dict)
    assert normalized["channel"] == "qqbot"
    assert normalized["chat_id"] == "group:group-456"
    assert normalized["sender_id"] == "member-9"
    assert normalized["message_id"] == "msg-group-1"
    assert normalized["content"] == "讲个故事"
    assert normalized["reply_to"]


def test_normalize_unsupported_event_returns_none():
    normalize_qq_event = load_normalizer()
    event = {
        "t": "INTERACTION_CREATE",
        "d": {
            "id": "event-unsupported-1",
            "content": "ignore me",
        },
    }

    assert normalize_qq_event(event, bot_app_id="102893854") is None
