import paramiko, sys

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('8.133.3.7', username='root', password='Asdf@Qwer!23')

script = """
import time, httpx, json

url = 'https://www.packyapi.com/v1/chat/completions'
headers = {
    'Content-Type': 'application/json',
    'Authorization': 'Bearer sk-fzQ6O5nD4Dbr1YS5dcQ2nVI44NNhru01nSa8sy7VzE4yxRqY',
}
payload = {
    'model': 'gpt-5.3-codex-xhigh',
    'messages': [{'role': 'user', 'content': '用中文说三句话，每句话以句号结尾。'}],
    'stream': True,
}

t0 = time.time()
chunks = []
with httpx.stream('POST', url, headers=headers, json=payload, timeout=60) as resp:
    for line in resp.iter_lines():
        if not line.startswith('data: '):
            continue
        data = line[6:].strip()
        if data == '[DONE]':
            break
        try:
            d = json.loads(data)
            token = d.get('choices',[{}])[0].get('delta',{}).get('content','')
            if token:
                t = round(time.time()-t0, 2)
                chunks.append((t, token))
        except:
            pass

print(f'Total chunks: {len(chunks)}')
print(f'First chunk: {chunks[0] if chunks else None}')
print(f'Last chunk: {chunks[-1] if chunks else None}')
if len(chunks) > 1:
    gaps = [round(chunks[i+1][0]-chunks[i][0],3) for i in range(min(5, len(chunks)-1))]
    print(f'First 5 inter-chunk gaps: {gaps}')
print('Full text:', ''.join(t for _,t in chunks)[:200])
"""

write_cmd = "docker exec -i voice-gateway tee /tmp/stream_test.py"
stdin, stdout, stderr = client.exec_command(write_cmd)
stdin.write(script.encode())
stdin.channel.shutdown_write()
stdout.read()

stdin, stdout, stderr = client.exec_command('docker exec voice-gateway python3 /tmp/stream_test.py', timeout=60)
out = stdout.read()
err = stderr.read()
sys.stdout.buffer.write(out)
if err:
    sys.stderr.buffer.write(err)
client.close()
