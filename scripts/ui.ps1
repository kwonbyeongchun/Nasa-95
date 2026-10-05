# UI 를 띄운다(소스 저장소에서 실행). 솔버는 third_party\calculix 의 것을 쓴다.
$root = Split-Path $PSScriptRoot -Parent
$env:PYTHONPATH = Join-Path $root "python"
& (Join-Path $root ".venv\Scripts\python.exe") -m nasa95.ui @args
