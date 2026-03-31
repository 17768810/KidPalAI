import pytest
from fastapi.testclient import TestClient
from unittest.mock import AsyncMock, patch
from main import app

client = TestClient(app)


def test_stt_returns_text():
    with patch("main.transcribe_pcm", new=AsyncMock(return_value="你好世界")):
        resp = client.post(
            "/stt",
            files={"audio": ("audio.pcm", b"\x00" * 100, "application/octet-stream")},
        )
    assert resp.status_code == 200
    assert resp.json() == {"text": "你好世界"}


def test_stt_returns_422_on_empty_transcription():
    with patch("main.transcribe_pcm", new=AsyncMock(return_value="")):
        resp = client.post(
            "/stt",
            files={"audio": ("audio.pcm", b"\x00" * 100, "application/octet-stream")},
        )
    assert resp.status_code == 422


def test_stt_returns_400_on_empty_audio():
    resp = client.post(
        "/stt",
        files={"audio": ("audio.pcm", b"", "application/octet-stream")},
    )
    assert resp.status_code == 400


def test_tts_returns_pcm():
    fake_pcm = b"\x00\x01" * 100
    with patch("main.synthesize_pcm", new=AsyncMock(return_value=fake_pcm)):
        resp = client.post("/tts", json={"text": "你好，书童"})
    assert resp.status_code == 200
    assert resp.headers["content-type"] == "audio/pcm"
    assert resp.content == fake_pcm


def test_tts_returns_400_on_empty_text():
    resp = client.post("/tts", json={"text": "  "})
    assert resp.status_code == 400
