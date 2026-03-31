# scripts/verify_tts_pcm.py
import asyncio, base64, json, os, uuid
import httpx
from dotenv import load_dotenv

load_dotenv("gateway/.env")

async def main():
    headers = {
        "Authorization": f"Bearer;{os.environ['VOLC_ACCESS_TOKEN']}",
        "Content-Type": "application/json",
    }
    payload = {
        "app": {
            "appid": os.environ["VOLC_APP_ID"],
            "token": os.environ["VOLC_ACCESS_TOKEN"],
            "cluster": "volcano_tts",
        },
        "user": {"uid": "test"},
        "audio": {
            "voice_type": os.getenv("VOLC_VOICE_TYPE", "zh_female_vv_uranus_bigtts"),
            "encoding": "pcm",
            "sample_rate": 16000,
            "bits": 16,
            "channel": 1,
        },
        "request": {
            "reqid": str(uuid.uuid4()),
            "text": "你好，我是小书童",
            "text_type": "plain",
            "operation": "query",
        },
    }
    async with httpx.AsyncClient(timeout=15.0) as client:
        resp = await client.post(
            "https://openspeech.bytedance.com/api/v1/tts",
            headers=headers, json=payload)
        resp.raise_for_status()
        data = resp.json()
        print(f"Response code field: {data.get('code')}")
        print(f"Response message: {data.get('message')}")
        if "data" not in data:
            print(f"ERROR: No 'data' field in response. Full response: {json.dumps(data, ensure_ascii=False)}")
            return
        pcm = base64.b64decode(data["data"])
        out_path = "scripts/test.pcm"
        with open(out_path, "wb") as f:
            f.write(pcm)
        print(f"PCM bytes: {len(pcm)}")
        print(f"Expected duration: {len(pcm)/32000:.2f}s at 16kHz 16bit mono")
        print(f"Saved to: {out_path}")
        print("SUCCESS: TTS PCM output confirmed.")

asyncio.run(main())
