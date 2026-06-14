@echo off
setlocal
cd /d "%~dp0\..\.."
if exist .venv\Scripts\python.exe (
  .venv\Scripts\python.exe server\tools_generate_indexes.py %*
) else (
  python server\tools_generate_indexes.py %*
)
