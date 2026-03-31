import paramiko, sys

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('8.133.3.7', username='root', password='Asdf@Qwer!23')

# Run inside the gateway Docker container
cmd = "docker exec voice-gateway python3 -c \"\nimport asyncio, sys, time\nsys.path.insert(0, '/app')\nimport os; os.chdir('/app')\n\nasync def main():\n    from llm import ask_openclaw, OPENCLAW_URL, OPENCLAW_MODEL\n    print('URL:', OPENCLAW_URL)\n    print('MODEL:', OPENCLAW_MODEL)\n    t = time.time()\n    resp = await ask_openclaw('你好，请简短介绍下你自己。')\n    print(f'LLM latency: {time.time()-t:.1f}s')\n    print('Response:', resp[:300])\n\nasyncio.run(main())\n\""

stdin, stdout, stderr = client.exec_command(cmd)
out = stdout.read()
err = stderr.read()
sys.stdout.buffer.write(out)
if err:
    sys.stderr.buffer.write(err)
client.close()
