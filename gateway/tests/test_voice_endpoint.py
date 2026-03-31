import pytest
from unittest.mock import AsyncMock, patch
from fastapi.testclient import TestClient
from main import app
import httpx

client = TestClient(app)


def test_health_endpoint():
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


def test_voice_endpoint_returns_mp3():
    fake_pcm = b'\x00' * 32000  # 1 second of 16kHz 16-bit mono

    with patch("main.transcribe_pcm", new_callable=AsyncMock) as mock_stt, \
         patch("main.ask_openclaw", new_callable=AsyncMock) as mock_llm, \
         patch("main.synthesize_text", new_callable=AsyncMock) as mock_tts:

        mock_stt.return_value = "今天天气怎么样"
        mock_llm.return_value = "今天是个好日子，适合学习！"
        mock_tts.return_value = b'\xff\xfb' + b'\x00' * 100  # fake MP3

        response = client.post(
            "/voice",
            files={"audio": ("audio.pcm", fake_pcm, "application/octet-stream")}
        )

    assert response.status_code == 200
    assert response.headers["content-type"] == "audio/mpeg"
    assert len(response.content) > 0


def test_voice_endpoint_returns_400_on_empty_audio():
    response = client.post(
        "/voice",
        files={"audio": ("audio.pcm", b"", "application/octet-stream")}
    )
    assert response.status_code == 400


def test_startup_event_calls_create_task():
    """startup_event must call asyncio.create_task with a coroutine."""
    import asyncio
    from unittest.mock import MagicMock, patch
    created = []

    def capture_task(coro):
        created.append(getattr(coro, '__name__', repr(coro)))
        coro.close()  # prevent "coroutine was never awaited" RuntimeWarning
        return MagicMock()

    with patch("main.asyncio.create_task", side_effect=capture_task):
        from fastapi.testclient import TestClient
        import main as m
        client = TestClient(m.app)
        with client:  # TestClient runs startup events synchronously
            pass

    assert len(created) >= 1, "Expected create_task to be called at startup"
    assert any("warmup" in name or "keepalive" in name for name in created), \
        f"Expected warmup/keepalive task, got: {created}"


@pytest.mark.asyncio
async def test_voice_stream_returns_chunked_pcm():
    """/voice/stream must return streaming PCM bytes in chunks."""
    from httpx import AsyncClient, ASGITransport
    from main import app

    fake_pcm_chunk1 = b'\x00\x01' * 320  # 640 bytes = 20ms
    fake_pcm_chunk2 = b'\x02\x03' * 320

    async def fake_stream_llm(text):
        yield "你好！"
        yield "今天天气不错。"

    with patch("main.transcribe_pcm", new_callable=AsyncMock) as mock_stt, \
         patch("main.stream_openclaw") as mock_stream, \
         patch("main.synthesize_pcm", new_callable=AsyncMock) as mock_tts:

        mock_stt.return_value = "今天天气怎么样"
        mock_stream.return_value = fake_stream_llm("今天天气怎么样")
        mock_tts.side_effect = [fake_pcm_chunk1, fake_pcm_chunk2]

        fake_pcm = b'\x00' * 32000
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as ac:
            async with ac.stream(
                "POST", "/voice/stream",
                files={"audio": ("audio.pcm", fake_pcm, "application/octet-stream")}
            ) as response:
                assert response.status_code == 200
                assert "audio/pcm" in response.headers.get("content-type", "")
                chunks = []
                async for chunk in response.aiter_bytes():
                    chunks.append(chunk)

        total = b"".join(chunks)
        assert total == fake_pcm_chunk1 + fake_pcm_chunk2
