@echo off
setlocal
cd /d "%~dp0\..\.."
if not exist .venv (
  python -m venv .venv
)
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install -r server\requirements.txt
if not exist server\config.ini copy server\config.ini.sample server\config.ini
 echo.
echo Setup complete. Edit server\config.ini if needed, then run server\scripts\run_server.cmd
