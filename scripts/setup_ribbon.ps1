# SARibbon v2.9.5 공식 PySide6 바인딩을 빌드해 프로젝트 .venv에 설치한다.
# 필요: Windows x64, Python 3.12 .venv, VS 2022 C++ 도구, Git, 인터넷.
param([string]$ThirdParty = "", [switch]$Force)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $ThirdParty) { $ThirdParty = Join-Path (Split-Path -Parent $repo) "third_party" }
$ThirdParty = [IO.Path]::GetFullPath($ThirdParty)
$python = Join-Path $repo ".venv\Scripts\python.exe"
function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "명령 실패: $Program (exit $LASTEXITCODE)" }
}
if (-not (Test-Path -LiteralPath $python)) { throw "먼저 프로젝트 .venv를 만드세요." }
Run-Checked $python @("-c", "import sys,platform,PySide6,shiboken6; from PySide6.QtCore import qVersion; assert sys.version_info[:2]==(3,12) and platform.machine()=='AMD64', 'Python 3.12 x64 required'; assert PySide6.__version__==shiboken6.__version__==qVersion()=='6.11.1', 'Install PySide6==6.11.1 in project .venv first'")
if (-not $Force) {
    & $python -c "from PySideSARibbon import saribbon; assert saribbon.SARibbonBar.versionString()=='2.9.5'" 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "SARibbon 2.9.5 설치 확인 완료"; exit 0 }
}
$root = Join-Path $ThirdParty "saribbon"
$src = Join-Path $root "2.9.5"
$buildPython = Join-Path $root "build-env\Scripts\python.exe"
$build = Join-Path $root "build-qt6.11.1"
if (-not (Test-Path -LiteralPath $src)) {
    Run-Checked "git" @("clone", "--branch", "v2.9.5", "--depth", "1", "https://github.com/czyt1988/SARibbon.git", $src)
}
$commit = & git -C $src rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $commit -ne "a21d30c2a8495da92c7db3adcdc124699b0f2a60") { throw "SARibbon 소스 버전이 고정 커밋과 다릅니다." }
$changes = & git -C $src status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $changes) { throw "SARibbon 소스가 수정되어 있습니다. 별도 ThirdParty 폴더를 지정하세요." }
if (-not (Test-Path -LiteralPath $buildPython)) {
    Run-Checked $python @("-m", "venv", (Join-Path $root "build-env"))
}
Run-Checked $buildPython @("-m", "pip", "install", "PySide6==6.11.1", "shiboken6-generator==6.11.1", "ninja==1.13.2", "py7zr==1.1.3")
Run-Checked $buildPython @((Join-Path $PSScriptRoot "ribbon_dependency.py"), "prepare", $ThirdParty)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio 2022 C++ 빌드 도구가 필요합니다." }
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$oldLanguage = $env:VSLANG
try {
    $env:VSLANG = "1033"
    Run-Checked $cmake @("-S", "$src\pyside6", "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=$ThirdParty\qt\6.11.1\msvc2022_64", "-DPython_EXECUTABLE=$buildPython",
        "-DCMAKE_MAKE_PROGRAM=$root\build-env\Scripts\ninja.exe")
    Run-Checked $cmake @("--build", $build, "--parallel", "4")
    Run-Checked $python @((Join-Path $PSScriptRoot "ribbon_dependency.py"), "install", $ThirdParty)
    Run-Checked $python @("-c", "from PySideSARibbon import saribbon; assert saribbon.SARibbonBar.versionString()=='2.9.5'; print('SARibbon 2.9.5 installed')")
} finally { $env:VSLANG = $oldLanguage }
