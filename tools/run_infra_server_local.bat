@echo off
rem Patapon 3 infrastructure server for LOCAL TESTS: no beta gate, no metrics, no logs. The public server is
rem build/beta/servidor_beta.bat. Local tunnel setup: the Cloudflare Tunnel maps
rem https://online.montick.app to http://localhost:27321 (WebSocket, path /infra).
rem Both listeners stay on 127.0.0.1; nothing is opened on the router. Ctrl+C stops it.
cd /d "%~dp0"
python -I infra_server.py --host 127.0.0.1 --port 27320 --ws-port 27321
pause
