import paramiko, sys

host = '8.133.3.7'
user = 'root'
password = 'Asdf@Qwer!23'

files = [
    ('c:/WorkGit/KidPalAI/gateway/llm.py', '/root/kidpalai/gateway/llm.py'),
    ('c:/WorkGit/KidPalAI/gateway/.env',   '/root/kidpalai/gateway/.env'),
]

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect(host, username=user, password=password)
sftp = client.open_sftp()
for local, remote in files:
    sftp.put(local, remote)
    print('uploaded:', remote)
sftp.close()

stdin, stdout, stderr = client.exec_command(
    'cd /root/kidpalai && docker compose up -d --build gateway 2>&1 | tail -25'
)
out = stdout.read()
err = stderr.read()
sys.stdout.buffer.write(out)
sys.stdout.buffer.write(err)
client.close()
