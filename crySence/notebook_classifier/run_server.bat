@echo off
setlocal
cd /d "%~dp0"

if "%CRYSENSE_HOST%"=="" set CRYSENSE_HOST=0.0.0.0
if "%CRYSENSE_PORT%"=="" set CRYSENSE_PORT=8000

python -m uvicorn server:app --host %CRYSENSE_HOST% --port %CRYSENSE_PORT%

