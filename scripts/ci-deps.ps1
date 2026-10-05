# 외부 의존성을 ..\third_party\ 에 준비한다(CI 와 새 PC 공용). 이미 있으면 건너뛴다.
#   .\scripts\ci-deps.ps1                      # OpenCASCADE 8.0.1(공식 빌드 내려받기) + Netgen v6.2.2608(소스 빌드) + Vulkan SDK 연결
#   .\scripts\ci-deps.ps1 -ThirdParty D:\tp    # 다른 폴더
# Vulkan SDK: 환경 변수 VULKAN_SDK(LunarG 설치)가 있으면 third_party\vulkan\sdk 에 정션으로 잇는다. 없으면 렌더러 없이 빌드된다.
# SARibbon 바인딩은 .venv 가 필요하므로 따로 scripts\setup_ribbon.ps1 을 돌린다. 솔버는 준비하지 않는다(별도 실행 파일).
param(
    [string]$ThirdParty = "",
    [string]$OcctVersion = "8.0.1",
    [string]$NetgenTag = "v6.2.2608",
    [switch]$SkipNetgen
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $ThirdParty) { $ThirdParty = Join-Path (Split-Path -Parent $repo) "third_party" }
$ThirdParty = [IO.Path]::GetFullPath($ThirdParty)
New-Item -ItemType Directory -Force $ThirdParty | Out-Null
function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "명령 실패: $Program $Arguments (exit $LASTEXITCODE)" }
}

# ---- OpenCASCADE: 공식 릴리스의 combined(3rdparty 포함) zip. 바깥 zip 안에 zip 이 한 번 더 들어 있다
$occtRoot = Join-Path $ThirdParty "occt\$OcctVersion"
$occtDir = Join-Path $occtRoot "opencascade-$OcctVersion-vc14-64"
if (-not (Test-Path (Join-Path $occtDir "cmake\OpenCASCADEConfig.cmake"))) {
    New-Item -ItemType Directory -Force $occtRoot | Out-Null
    $outer = Join-Path $occtRoot "occt-combined-release-no-pch.zip"
    $url = "https://github.com/Open-Cascade-SAS/OCCT/releases/download/V$OcctVersion/occt-combined-release-no-pch.zip"
    Write-Host "OpenCASCADE $OcctVersion 내려받기: $url"
    Invoke-WebRequest -Uri $url -OutFile $outer
    $tmp = Join-Path $occtRoot "_unzip"
    Expand-Archive -Path $outer -DestinationPath $tmp -Force
    $inner = Get-ChildItem $tmp -Filter "*.zip" | Select-Object -First 1
    if ($inner) { Expand-Archive -Path $inner.FullName -DestinationPath $occtRoot -Force; Remove-Item $tmp -Recurse -Force }
    else { Get-ChildItem $tmp | Move-Item -Destination $occtRoot -Force; Remove-Item $tmp -Recurse -Force }
    Remove-Item $outer -Force
    if (-not (Test-Path (Join-Path $occtDir "cmake\OpenCASCADEConfig.cmake"))) { throw "OpenCASCADE 압축 구조가 예상과 다릅니다: $occtRoot" }
}
Write-Host "OpenCASCADE: $occtDir"
$zlib = Get-ChildItem (Join-Path $occtRoot "3rdparty-vc14-64") -Directory -Filter "zlib-*" | Select-Object -First 1

# ---- Netgen: 소스 빌드(OCCT 연결, GUI·Python 없음). 라이선스 LGPL 2.1
$ngRoot = Join-Path $ThirdParty "netgen"
$ngInstall = Join-Path $ngRoot "install"
if (-not $SkipNetgen -and -not (Test-Path (Join-Path $ngInstall "cmake\NetgenConfig.cmake"))) {
    $ngSrc = Join-Path $ngRoot "src"
    if (-not (Test-Path (Join-Path $ngSrc "CMakeLists.txt"))) {
        Run-Checked "git" @("clone", "--branch", $NetgenTag, "--depth", "1", "https://github.com/NGSolve/netgen.git", $ngSrc)
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -version "[17.0,18.0)" -property installationPath
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue)?.Source
    if (-not $cmake) { $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }
    $ngBuild = Join-Path $ngRoot "build"
    Write-Host "Netgen $NetgenTag 빌드 → $ngInstall"
    Run-Checked $cmake @("-S", $ngSrc, "-B", $ngBuild, "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_INSTALL_PREFIX=$ngInstall", "-DUSE_SUPERBUILD=OFF", "-DUSE_GUI=OFF", "-DUSE_PYTHON=OFF", "-DUSE_OCC=ON",
        "-DUSE_NATIVE_ARCH=OFF", "-DUSE_MPI=OFF", "-DUSE_JPEG=OFF", "-DUSE_MPEG=OFF", "-DUSE_CGNS=OFF",
        "-DOpenCascade_DIR=$occtDir\cmake", "-DOpenCASCADE_DIR=$occtDir\cmake",
        "-DZLIB_INCLUDE_DIR=$($zlib.FullName)\include", "-DZLIB_LIBRARY_RELEASE=$($zlib.FullName)\lib\zlib.lib")
    Run-Checked $cmake @("--build", $ngBuild, "--config", "Release", "--target", "install", "--parallel")
}
if (Test-Path (Join-Path $ngInstall "cmake\NetgenConfig.cmake")) { Write-Host "Netgen: $ngInstall" } else { Write-Host "Netgen 없음(자동 메싱 없이 빌드된다)" }

# ---- Vulkan SDK
$vkDir = Join-Path $ThirdParty "vulkan\sdk"
if (-not (Test-Path (Join-Path $vkDir "Include\vulkan\vulkan.h"))) {
    if ($env:VULKAN_SDK -and (Test-Path (Join-Path $env:VULKAN_SDK "Include\vulkan\vulkan.h"))) {
        New-Item -ItemType Directory -Force (Split-Path $vkDir) | Out-Null
        New-Item -ItemType Junction -Path $vkDir -Target $env:VULKAN_SDK | Out-Null
        Write-Host "Vulkan SDK: $vkDir → $env:VULKAN_SDK"
    } else {
        Write-Warning "Vulkan SDK 가 없습니다(VULKAN_SDK 환경 변수). 렌더러 없이 빌드됩니다."
    }
} else { Write-Host "Vulkan SDK: $vkDir" }
