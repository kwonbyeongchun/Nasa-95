@echo off
rem open-fep UI 실행. 더블 클릭하면 된다. (cmd 가 읽는 파일이므로 CP949 + CRLF 로 저장한다)
setlocal
set ROOT=%~dp0
set PYTHONPATH=%ROOT%python
if not exist "%ROOT%.venv\Scripts\python.exe" (
  echo .venv 가 없습니다. 먼저 빌드하세요: scripts\build.ps1
  pause
  exit /b 1
)
"%ROOT%.venv\Scripts\python.exe" -m openfep.ui %*
if errorlevel 1 pause
