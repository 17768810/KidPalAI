import importlib
import importlib.util
import sys
from contextlib import contextmanager
from itertools import count
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

GATEWAY_DIR = Path(__file__).resolve().parents[1]
MAIN_PATH = GATEWAY_DIR / "main.py"
MODULE_COUNTER = count()


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

def load_main_module():
    module_name = f"gateway_main_test_{next(MODULE_COUNTER)}"
    spec = importlib.util.spec_from_file_location(module_name, MAIN_PATH)
    if not spec or not spec.loader:
        raise RuntimeError("could not load gateway main module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        with gateway_import_path():
            spec.loader.exec_module(module)
        return module
    finally:
        sys.modules.pop(module_name, None)


@pytest.fixture
def client():
    main_module = load_main_module()
    with TestClient(main_module.app) as test_client:
        yield test_client


def private_text_event():
    return {
        "t": "C2C_MESSAGE_CREATE",
        "d": {
            "id": "msg-private-1",
            "author": {"id": "user-123"},
            "content": "你好，小书童",
        },
    }


def test_firmware_can_register_websocket_session_by_qq_app_id(client):
    with client.websocket_connect("/qqbot/ws") as websocket:
        websocket.send_json(
            {
                "type": "register",
                "qq_app_id": "102893854",
                "device_id": "mimi-01",
            }
        )

        assert websocket.receive_json() == {
            "type": "registered",
            "qq_app_id": "102893854",
            "device_id": "mimi-01",
        }


def test_webhook_routes_message_to_active_registered_session(client):
    with client.websocket_connect("/qqbot/ws") as websocket:
        websocket.send_json(
            {
                "type": "register",
                "qq_app_id": "102893854",
                "device_id": "mimi-01",
            }
        )
        assert websocket.receive_json() == {
            "type": "registered",
            "qq_app_id": "102893854",
            "device_id": "mimi-01",
        }

        response = client.post("/qqbot/webhook", json=private_text_event())

        assert 200 <= response.status_code < 300
        forwarded = websocket.receive_json()
        assert forwarded["type"] == "inbound_message"
        assert isinstance(forwarded["payload"], dict)
        payload = forwarded["payload"]
        assert payload["channel"] == "qqbot"
        assert payload["chat_id"] == "user:user-123"
        assert payload["sender_id"] == "user-123"
        assert payload["message_id"] == "msg-private-1"
        assert payload["content"] == "你好，小书童"
        assert payload["reply_to"]


def test_webhook_returns_503_when_matching_device_session_is_missing(client):
    response = client.post("/qqbot/webhook", json=private_text_event())

    assert response.status_code == 503
    assert response.headers["content-type"].startswith("application/json")
    assert response.json() == {"detail": "qq device session unavailable"}
