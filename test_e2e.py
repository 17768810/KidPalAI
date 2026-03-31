import paramiko, sys

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('8.133.3.7', username='root', password='Asdf@Qwer!23')

sftp = client.open_sftp()
sftp.put('c:/WorkGit/KidPalAI/gateway/tests/demo_1.wav', '/tmp/demo_1.wav')
sftp.close()

# Run e2e inside Docker where Python 3.11+ and requests are available
cmd = r"""docker exec voice-gateway bash -c "
ffmpeg -y -i /tmp/demo_1.wav -ar 16000 -ac 1 -f s16le /tmp/demo.pcm 2>/dev/null && \
python3 -c \"
import time, requests

with open('/tmp/demo.pcm', 'rb') as f:
    pcm = f.read()
print('PCM:', len(pcm), 'bytes')

boundary = 'kidpalai_test'
header = ('--' + boundary + '\r\nContent-Disposition: form-data; name=audio; filename=audio.pcm\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
footer = ('\r\n--' + boundary + '--\r\n').encode()
body = header + pcm + footer
headers = {'Content-Type': 'multipart/form-data; boundary=' + boundary}

t0 = time.time()
resp = requests.post('http://localhost:8000/voice/stream', data=body, headers=headers, stream=True, timeout=60)
print('HTTP', resp.status_code, 'at', round(time.time()-t0,1), 's')
total = 0
first = None
for chunk in resp.iter_content(chunk_size=640):
    if chunk:
        if first is None:
            first = time.time()
            print('First PCM chunk at', round(first-t0,1), 's')
        total += len(chunk)
print('Done:', total, 'bytes in', round(time.time()-t0,1), 's total')
\"
"
"""

# Copy WAV into container first
client.exec_command('docker cp /tmp/demo_1.wav voice-gateway:/tmp/demo_1.wav')
import time; time.sleep(1)

stdin, stdout, stderr = client.exec_command(cmd)
out = stdout.read()
err = stderr.read()
sys.stdout.buffer.write(out)
if err:
    sys.stderr.buffer.write(err)
client.close()
