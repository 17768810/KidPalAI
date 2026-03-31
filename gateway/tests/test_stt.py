import json

import pytest
from unittest.mock import AsyncMock, patch
from stt import transcribe_pcm


@pytest.mark.asyncio
async def test_transcribe_returns_string():
    fake_pcm = b'\x00' * 3200  # 100ms of 16kHz 16-bit mono silence
    with patch("stt._call_xunfei_api", new_callable=AsyncMock) as mock_api:
        mock_api.return_value = "你好书童"
        result = await transcribe_pcm(fake_pcm)
    assert isinstance(result, str)
    assert result == "你好书童"


@pytest.mark.asyncio
async def test_transcribe_raises_on_empty_audio():
    with pytest.raises(ValueError, match="empty audio"):
        await transcribe_pcm(b"")



@pytest.mark.asyncio
async def test_vad_eos_is_1000():
    """vad_eos must be 1000ms (not 5000) to avoid 5s extra wait."""
    captured_payloads = []

    async def fake_ws_send(data):
        captured_payloads.append(json.loads(data))

    class FakeWS:
        send = AsyncMock(side_effect=fake_ws_send)
        recv = AsyncMock(return_value=json.dumps({
            "data": {"status": 2, "result": {"ws": []}}
        }))
        async def __aenter__(self): return self
        async def __aexit__(self, *a): pass

    with patch("stt.websockets.connect", return_value=FakeWS()):
        await transcribe_pcm(b'\x00' * 1280)  # single frame

    first_payload = captured_payloads[0]
    assert first_payload["business"]["vad_eos"] == 1000, \
        f"Expected vad_eos=1000, got {first_payload['business']['vad_eos']}"
