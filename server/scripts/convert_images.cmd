@echo off
setlocal
cd /d "%~dp0\..\.."
if exist .venv\Scripts\python.exe (
  .venv\Scripts\python.exe server\tools_convert_images.py %*
) else (
  python server\tools_convert_images.py %*
)
