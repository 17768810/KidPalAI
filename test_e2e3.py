import paramiko, sys, time, struct, wave

# Convert WAV to raw PCM locally
with wave.open('c:/WorkGit/KidPalAI/gateway/tests/demo_1.wav', 'rb') as wf:
    # Read all frames
    frames = wf.readframes(wf.getnframes())
    sr = wf.getframerate()
    ch = wf.getnchannels()
    sw = wf.getsampwidth()
    print(f'WAV: {sr}Hz {ch}ch {sw*8}bit, {wf.getnframes()/sr:.2f}s')

# Write raw PCM (already 16kHz 16-bit mono per demo_1.wav)
with open('/tmp/demo.pcm', 'wb') as f:
    f.write(frames)
pcm = frames

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('8.133.3.7', username='root', password='Asdf@Qwer!23')

# Copy PCM into container
sftp = client.open_sftp()
sftp.put('/tmp/demo.pcm', '/tmp/demo.pcm')
sftp.close()
client.exec_command('docker cp /tmp/demo.pcm voice-gateway:/tmp/demo.pcm')
time.sleep(1)

script_content = r"""import time, httpx

with open('/tmp/demo.pcm','rb') as f:
    pcm = f.read()
print('PCM bytes:', len(pcm))

boundary = 'kidpalai_test'
hdr = ('--' + boundary + '\r\nContent-Disposition: form-data; name=audio; filename=audio.pcm\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
ftr = ('\r\n--' + boundary + '--\r\n').encode()
body = hdr + pcm + ftr
hdrs = {'Content-Type': 'multipart/form-data; boundary=' + boundary}

t0 = time.time()
with httpx.stream('POST', 'http://localhost:8000/voice/stream', content=body, headers=hdrs, timeout=90) as resp:
    print('HTTP', resp.status_code, 'at', round(time.time()-t0,1))
    total=0; first=None
    for chunk in resp.iter_bytes(chunk_size=640):
        if chunk:
            if first is None:
                first = time.time()
                print('First chunk at', round(first-t0,1))
            total += len(chunk)
print('Done:', total, 'bytes in', round(time.time()-t0,1), 's')
"""

write_cmd = "docker exec -i voice-gateway tee /tmp/e2e_test.py"
stdin, stdout, stderr = client.exec_command(write_cmd)
stdin.write(script_content.encode())
stdin.channel.shutdown_write()
stdout.read()

stdin, stdout, stderr = client.exec_command('docker exec voice-gateway python3 /tmp/e2e_test.py', timeout=120)
out = stdout.read()
err = stderr.read()
sys.stdout.buffer.write(out)
if err:
    sys.stderr.buffer.write(err)
client.close()
