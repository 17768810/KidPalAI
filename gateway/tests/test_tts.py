import base64
import pytest
from unittest.mock import AsyncMock, patch
from tts import synthesize_text, synthesize_pcm


@pytest.mark.asyncio
async def test_synthesize_returns_mp3_bytes():
    fake_mp3 = b'\xff\xfb' + b'\x00' * 100  # fake MP3 header
    with patch("tts._call_volc_api", new_callable=AsyncMock) as mock_api:
        mock_api.return_value = fake_mp3
        result = await synthesize_text("你好，我是书童")
    assert isinstance(result, bytes)
    assert len(result) > 0


@pytest.mark.asyncio
async def test_synthesize_raises_on_empty_text():
    with pytest.raises(ValueError, match="empty text"):
        await synthesize_text("")


@pytest.mark.asyncio
async def test_synthesize_pcm_returns_raw_bytes():
    """synthesize_pcm must decode base64 and return raw bytes (no header)."""
    fake_pcm = b'\x00\x01' * 50  # 100 bytes of fake 16-bit PCM samples
    fake_b64 = base64.b64encode(fake_pcm).decode()

    with patch("tts._call_volc_api_raw", new_callable=AsyncMock) as mock_api:
        mock_api.return_value = fake_b64
        result = await synthesize_pcm("你好")

    assert result == fake_pcm
    # Verify the call included pcm encoding
    mock_api.assert_called_once_with("你好", encoding="pcm")


@pytest.mark.asyncio
async def test_synthesize_pcm_raises_on_empty():
    with pytest.raises(ValueError, match="empty text"):
        await synthesize_pcm("")
