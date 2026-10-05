# 코어와 Python 모듈을 빌드한다. (Visual Studio 2022, x64)
#   .\scripts\build.ps1                 # Release 빌드
#   .\scripts\build.ps1 -Config Debug
#   .\scripts\build.ps1 -Fresh          # CMake 캐시를 새로 만든다
param(
    [string]$Config = "Release",
    [switch]$Fresh
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$python = Join-Path $repo ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) {
    throw "가상 환경이 없습니다. 먼저 실행: python -m venv --system-site-packages .venv ; .venv\Scripts\python -m pip install pybind11 pytest"
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)?.Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -version "[17.0,18.0)" -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
$pybind = & $python -c "import pybind11; print(pybind11.get_cmake_dir())"

$configure = @("-S", $repo, "-B", "$repo\build", "-G", "Visual Studio 17 2022", "-A", "x64",
               "-DPython_EXECUTABLE=$python", "-Dpybind11_DIR=$pybind")
# 외부 의존성 폴더를 환경 변수로 바꿀 수 있다(CI: scripts\ci-deps.ps1 이 ..\third_party 를 채우므로 보통 필요 없다)
if ($env:NASA95_THIRD_PARTY) {
    $tp = $env:NASA95_THIRD_PARTY
    $configure += "-DNASA95_OCCT_DIR=$tp\occt\8.0.1\opencascade-8.0.1-vc14-64", "-DNASA95_NETGEN_DIR=$tp\netgen\install", "-DNASA95_VULKAN_SDK=$tp\vulkan\sdk"
} elseif ($env:VULKAN_SDK -and -not (Test-Path "$repo\..\third_party\vulkan\sdk\Include\vulkan\vulkan.h")) {
    $configure += "-DNASA95_VULKAN_SDK=$env:VULKAN_SDK"
}
if ($Fresh) { $configure = @("--fresh") + $configure }
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw "CMake 구성 실패" }
& $cmake --build "$repo\build" --config $Config
if ($LASTEXITCODE -ne 0) { throw "빌드 실패" }
