import pytest
import re
from unittest.mock import AsyncMock, MagicMock, patch
from llm import ask_openclaw


@pytest.mark.asyncio
async def test_ask_returns_string():
    with patch("llm._post_to_openclaw", new_callable=AsyncMock) as mock_post:
        mock_post.return_value = "太棒了！你问了一个很好的问题！"
        result = await ask_openclaw("1加1等于几？")
    assert isinstance(result, str)
    assert len(result) > 0


@pytest.mark.asyncio
async def test_ask_raises_on_empty_question():
    with pytest.raises(ValueError, match="empty question"):
        await ask_openclaw("")


@pytest.mark.asyncio
async def test_max_tokens_passed_to_api():
    """max_tokens param must be forwarded to the OpenClaw payload."""
    captured = {}

    async def fake_post(url, **kwargs):
        captured["payload"] = kwargs.get("json", {})
        mock_resp = MagicMock()
        mock_resp.raise_for_status = lambda: None
        mock_resp.json.return_value = {
            "choices": [{"message": {"content": "ok"}}]
        }
        return mock_resp

    with patch("llm.httpx.AsyncClient") as MockClient:
        mock_instance = AsyncMock()
        mock_instance.post = fake_post
        mock_instance.__aenter__ = AsyncMock(return_value=mock_instance)
        mock_instance.__aexit__ = AsyncMock(return_value=None)
        MockClient.return_value = mock_instance

        await ask_openclaw("你好", max_tokens=1)

    assert "max_tokens" in captured["payload"]
    assert captured["payload"]["max_tokens"] == 1


@pytest.mark.asyncio
async def test_stream_openclaw_yields_sentences():
    """stream_openclaw must yield complete sentences split on Chinese punctuation."""
    from llm import stream_openclaw

    sse_lines = [
        'data: {"choices":[{"delta":{"content":"你好"}}]}',
        'data: {"choices":[{"delta":{"content":"，我是"}}]}',
        'data: {"choices":[{"delta":{"content":"小书童。"}}]}',
        'data: {"choices":[{"delta":{"content":"今天天气真好"}}]}',
        'data: {"choices":[{"delta":{"content":"！"}}]}',
        'data: [DONE]',
    ]

    class FakeStreamResponse:
        async def aiter_lines(self):
            for line in sse_lines:
                yield line
        async def __aenter__(self): return self
        async def __aexit__(self, *a): pass
        def raise_for_status(self): pass

    with patch("llm.httpx.AsyncClient") as MockClient:
        mock_instance = AsyncMock()
        mock_instance.stream = MagicMock(return_value=FakeStreamResponse())
        mock_instance.__aenter__ = AsyncMock(return_value=mock_instance)
        mock_instance.__aexit__ = AsyncMock(return_value=None)
        MockClient.return_value = mock_instance

        sentences = []
        async for sentence in stream_openclaw("今天天气怎么样"):
            sentences.append(sentence)

    assert len(sentences) == 2
    assert "小书童" in sentences[0]
    assert "天气" in sentences[1]


@pytest.mark.asyncio
async def test_stream_openclaw_forces_split_at_max_buffer():
    """stream_openclaw must yield when buffer reaches MAX_BUFFER even without punctuation."""
    from llm import stream_openclaw, MAX_BUFFER

    # Generate a long string without punctuation
    long_token = "好" * (MAX_BUFFER + 5)
    sse_lines = [
        f'data: {{"choices":[{{"delta":{{"content":"{long_token}"}}}}]}}',
        'data: [DONE]',
    ]

    class FakeStreamResponse:
        async def aiter_lines(self):
            for line in sse_lines:
                yield line
        async def __aenter__(self): return self
        async def __aexit__(self, *a): pass
        def raise_for_status(self): pass

    with patch("llm.httpx.AsyncClient") as MockClient:
        mock_instance = AsyncMock()
        mock_instance.stream = MagicMock(return_value=FakeStreamResponse())
        mock_instance.__aenter__ = AsyncMock(return_value=mock_instance)
        mock_instance.__aexit__ = AsyncMock(return_value=None)
        MockClient.return_value = mock_instance

        sentences = []
        async for sentence in stream_openclaw("test"):
            sentences.append(sentence)

    # Must yield at least one sentence (forced cut at MAX_BUFFER)
    assert len(sentences) >= 1
    total_chars = sum(len(s) for s in sentences)
    assert total_chars == len(long_token)
