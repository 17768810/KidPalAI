import paramiko, sys, time

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('8.133.3.7', username='root', password='Asdf@Qwer!23')

# Copy wav into container
sftp = client.open_sftp()
sftp.put('c:/WorkGit/KidPalAI/gateway/tests/demo_1.wav', '/tmp/demo_1.wav')
sftp.close()
stdin, stdout, stderr = client.exec_command('docker cp /tmp/demo_1.wav voice-gateway:/tmp/demo_1.wav && echo "copied"')
print(stdout.read().decode())

# Run test script inside container
script = """
import time, subprocess, requests

# convert wav to pcm
r = subprocess.run(['ffmpeg','-y','-i','/tmp/demo_1.wav','-ar','16000','-ac','1','-f','s16le','/tmp/demo.pcm'],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
if r.returncode != 0:
    print('ffmpeg stderr:', r.stderr.decode()[-200:])

with open('/tmp/demo.pcm','rb') as f:
    pcm = f.read()
print('PCM bytes:', len(pcm))

boundary = 'kidpalai_test'
hdr = ('--' + boundary + '\\r\\nContent-Disposition: form-data; name=audio; filename=audio.pcm\\r\\nContent-Type: application/octet-stream\\r\\n\\r\\n').encode()
ftr = ('\\r\\n--' + boundary + '--\\r\\n').encode()
body = hdr + pcm + ftr
hdrs = {'Content-Type': 'multipart/form-data; boundary=' + boundary}

t0 = time.time()
resp = requests.post('http://localhost:8000/voice/stream', data=body, headers=hdrs, stream=True, timeout=90)
print('HTTP', resp.status_code, 'at', round(time.time()-t0,1))
total=0; first=None
for chunk in resp.iter_content(chunk_size=640):
    if chunk:
        if first is None:
            first = time.time()
            print('First chunk at', round(first-t0,1))
        total += len(chunk)
print('Done:', total, 'bytes in', round(time.time()-t0,1), 's')
"""

cmd = f'docker exec voice-gateway python3 -c {repr(script)}'
stdin, stdout, stderr = client.exec_command(cmd, timeout=120)
out = stdout.read()
err = stderr.read()
sys.stdout.buffer.write(out)
if err:
    sys.stderr.buffer.write(err)
client.close()
