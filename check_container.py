import paramiko, sys

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('8.133.3.7', username='root', password='Asdf@Qwer!23')

stdin, stdout, stderr = client.exec_command('docker ps -a && echo "---" && docker logs voice-gateway --tail 30 2>&1')
out = stdout.read()
sys.stdout.buffer.write(out)
client.close()
