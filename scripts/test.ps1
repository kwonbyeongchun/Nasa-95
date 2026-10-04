# API 테스트를 실행한다. 인자는 pytest 로 그대로 넘어간다.
#   .\scripts\test.ps1
#   .\scripts\test.ps1 -k SYS_08 -v
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$env:PYTHONIOENCODING = "utf-8"
Push-Location $repo
try {
    & "$repo\.venv\Scripts\python.exe" -m pytest @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
