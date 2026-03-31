import json
import os
import re
from typing import AsyncGenerator

import httpx
from dotenv import load_dotenv

load_dotenv()

SYSTEM_PROMPT = (
    '你是温柔耐心的AI学习伙伴，名叫"书童"。'
    '当前用户：叶欣羽，10岁，小学二年级。'
    '回答要求：简单活泼，多鼓励，每次回答不超过3句话，不使用Markdown格式，不使用星号或特殊符号。'
    '禁止话题：暴力、恐怖、成人内容。'
)

# OpenAI-compatible endpoint — takes priority if LLM_BASE_URL is set
_base_url = os.getenv("LLM_BASE_URL", "")
if _base_url:
    OPENCLAW_URL = _base_url.rstrip("/") + "/chat/completions"
    OPENCLAW_API_KEY = os.getenv("LLM_API_KEY", "")
    OPENCLAW_MODEL = os.getenv("LLM_MODEL", "gpt-4o")
else:
    OPENCLAW_URL = os.environ.get("OPENCLAW_WEBCHAT_URL", "http://openclaw:3000/v1/chat/completions")
    OPENCLAW_API_KEY = os.getenv("OPENCLAW_API_KEY", "")
    OPENCLAW_MODEL = os.getenv("OPENCLAW_MODEL", "agent:main:main")

# Sentence boundary: split on Chinese/English terminal punctuation
SENTENCE_END = re.compile(r"[。！？…\n]")
# Force-split when buffer reaches this many characters (no punctuation in LLM output)
MAX_BUFFER = 60
# Minimum sentence length before dispatching to TTS
MIN_SENTENCE = 5


def _wrap_question(question: str) -> str:
    """Prepend instruction to keep response short and plain-text."""
    return f"[请用2句话简短回答，不用markdown，不用星号]\n{question}"


async def _post_to_openclaw(question: str, max_tokens: int = None) -> str:
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_API_KEY}",
    }
    payload = {
        "model": OPENCLAW_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": _wrap_question(question)},
        ],
        "stream": False,
    }
    if max_tokens is not None:
        payload["max_tokens"] = max_tokens

    async with httpx.AsyncClient(timeout=60.0) as client:
        response = await client.post(OPENCLAW_URL, headers=headers, json=payload)
        response.raise_for_status()
        data = response.json()
        choices = data.get("choices", [])
        if choices:
            return choices[0].get("message", {}).get("content", "")
        return data.get("message", data.get("content", str(data)))


async def ask_openclaw(question: str, max_tokens: int = None) -> str:
    """Send transcribed text to OpenClaw and get AI response."""
    if not question:
        raise ValueError("empty question")
    return await _post_to_openclaw(question, max_tokens=max_tokens)


async def stream_openclaw(text: str) -> AsyncGenerator[str, None]:
    """Stream LLM response, yielding one sentence at a time.

    Splits on Chinese/English terminal punctuation (。！？…\\n).
    Forces a split when the buffer reaches MAX_BUFFER characters
    to handle LLM output with no punctuation.
    Skips sentences shorter than MIN_SENTENCE characters.
    """
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {OPENCLAW_API_KEY}",
    }
    payload = {
        "model": OPENCLAW_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": _wrap_question(text)},
        ],
        "stream": True,
        "max_tokens": 80,
    }

    buffer = ""

    async with httpx.AsyncClient(timeout=None) as client:
        async with client.stream("POST", OPENCLAW_URL, headers=headers, json=payload) as resp:
            resp.raise_for_status()
            async for line in resp.aiter_lines():
                if not line.startswith("data: "):
                    continue
                payload_str = line[6:].strip()
                if payload_str == "[DONE]":
                    break
                try:
                    delta_data = json.loads(payload_str)
                    token = (delta_data.get("choices", [{}])[0]
                             .get("delta", {})
                             .get("content", ""))
                except (json.JSONDecodeError, IndexError):
                    continue

                if not token:
                    continue

                buffer += token

                # Yield on sentence boundary or buffer overflow
                while SENTENCE_END.search(buffer) or len(buffer) >= MAX_BUFFER:
                    match = SENTENCE_END.search(buffer)
                    if match:
                        cut = match.end()
                    else:
                        cut = MAX_BUFFER
                    sentence = buffer[:cut].strip()
                    buffer = buffer[cut:]
                    if len(sentence) >= MIN_SENTENCE:
                        yield sentence

    # Yield any remaining text
    if buffer.strip() and len(buffer.strip()) >= MIN_SENTENCE:
        yield buffer.strip()
