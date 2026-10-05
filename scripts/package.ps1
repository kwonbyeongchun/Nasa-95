# 배포본을 만든다: 임베디드 Python + PySide6 + SARibbon 바인딩 + nasa95 패키지(_nasa95.pyd) + 필요한 DLL(OpenCASCADE·Netgen)
#   .\scripts\package.ps1                   # dist\NASA-95\ (포터블) + dist\NASA-95-<버전>-setup.exe (Inno Setup 이 있으면)
#   .\scripts\package.ps1 -Version 0.2.0    # 버전을 지정(없으면 CMakeLists 의 project VERSION)
#   .\scripts\package.ps1 -NoInstaller      # 포터블 폴더와 zip 만
# 먼저 build.ps1(+ setup_ribbon.ps1) 로 빌드가 끝나 있어야 한다. 솔버(CalculiX·OpenSees·MyStran)는 넣지 않는다(별도 실행 파일, 설정에서 경로 지정).
param(
    [string]$Version = "",
    [switch]$NoInstaller,
    [string]$PythonEmbedVersion = "3.12.10"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$venv = Join-Path $repo ".venv\Scripts\python.exe"
$pkg = Join-Path $repo "python\nasa95"
$dist = Join-Path $repo "dist"
$app = Join-Path $dist "NASA-95"
$thirdParty = Join-Path (Split-Path -Parent $repo) "third_party"
function Run-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "명령 실패: $Program $Arguments (exit $LASTEXITCODE)" }
}

if (-not $Version) {
    $m = Select-String -Path (Join-Path $repo "CMakeLists.txt") -Pattern 'project\(\S+ VERSION ([0-9.]+)'
    $Version = $m.Matches[0].Groups[1].Value
}
$pyd = Get-ChildItem $pkg -Filter "_nasa95*.pyd" | Select-Object -First 1
if (-not $pyd) { throw "빌드된 모듈이 없습니다: $pkg\_nasa95*.pyd — 먼저 scripts\build.ps1" }
Write-Host "NASA-95 $Version 배포본 — $pyd"

if (Test-Path $app) { Remove-Item $app -Recurse -Force }
New-Item -ItemType Directory -Force $app | Out-Null

# 1) 임베디드 Python
$embedZip = Join-Path $thirdParty "python-embed\python-$PythonEmbedVersion-embed-amd64.zip"
if (-not (Test-Path $embedZip)) {
    New-Item -ItemType Directory -Force (Split-Path $embedZip) | Out-Null
    $url = "https://www.python.org/ftp/python/$PythonEmbedVersion/python-$PythonEmbedVersion-embed-amd64.zip"
    Write-Host "내려받기 $url"
    Invoke-WebRequest -Uri $url -OutFile $embedZip
}
$py = Join-Path $app "python"
Expand-Archive -Path $embedZip -DestinationPath $py -Force
$pth = Get-ChildItem $py -Filter "python3*._pth" | Select-Object -First 1
@("python312.zip", ".", "Lib\site-packages", "", "# NASA-95: 임베디드 Python. import site 는 켜지 않는다(사용자 site-packages 와 섞이지 않게)") |
    Set-Content -Path $pth.FullName -Encoding ascii
$site = Join-Path $py "Lib\site-packages"
New-Item -ItemType Directory -Force $site | Out-Null

# 2) Python 의존성: PySide6(LGPL, 동적) · numpy · SARibbon 바인딩(MIT, .venv 에 설치된 것)
Run-Checked $venv @("-m", "pip", "install", "--quiet", "--target", $site, "--only-binary=:all:", "PySide6==6.11.1", "shiboken6==6.11.1", "numpy")
$ribbon = & $venv -c "import PySideSARibbon, pathlib; print(pathlib.Path(PySideSARibbon.__file__).parent)"
if ($LASTEXITCODE -ne 0) { throw "SARibbon 바인딩이 .venv 에 없습니다: scripts\setup_ribbon.ps1" }
Copy-Item $ribbon (Join-Path $site "PySideSARibbon") -Recurse -Force
Get-ChildItem $site -Recurse -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force

# 3) nasa95 패키지(소스 + pyd). 작업 파일(pyd.old 등)·캐시는 뺀다
$dest = Join-Path $site "nasa95"
# robocopy 의 "*.pyd.*" 는 ".pyd" 자체도 걸러내므로 pyd 는 뺀 채 복사하고 모듈만 따로 넣는다
robocopy $pkg $dest /E /XD __pycache__ /XF "*.pyd*" "_dll_dirs.txt" "*.pdb" /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "nasa95 패키지 복사 실패" }
Copy-Item $pyd.FullName $dest -Force

# 4) 코어가 쓰는 DLL: _nasa95.pyd 에서 시작해 의존 DLL 을 빌드 때의 DLL 폴더들에서 찾아 bin\ 에 모은다(시스템 DLL 은 제외)
$bin = Join-Path $app "bin"
New-Item -ItemType Directory -Force $bin | Out-Null
$dllDirs = @()
$dirsFile = Join-Path $pkg "_dll_dirs.txt"
if (Test-Path $dirsFile) { $dllDirs = Get-Content $dirsFile | Where-Object { $_.Trim() -and (Test-Path $_.Trim()) } | ForEach-Object { $_.Trim() } }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -version "[17.0,18.0)" -property installationPath
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe 를 찾지 못했습니다(Visual Studio 2022 C++ 도구)" }
$seen = @{}
$queue = New-Object System.Collections.Queue
$queue.Enqueue($pyd.FullName)
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    $out = & $dumpbin.FullName /NOLOGO /DEPENDENTS $file 2>$null
    foreach ($line in $out) {
        if ($line -match '^\s+(\S+\.dll)\s*$') {
            $name = $Matches[1]
            if ($seen.ContainsKey($name.ToLower())) { continue }
            $found = $null
            foreach ($d in $dllDirs) { $c = Join-Path $d $name; if (Test-Path $c) { $found = $c; break } }
            $seen[$name.ToLower()] = $found
            if ($found) { Copy-Item $found $bin -Force; $queue.Enqueue($found) }
        }
    }
}
$copied = ($seen.Values | Where-Object { $_ }).Count
Write-Host "DLL $copied 개 → bin\ (시스템 DLL $(($seen.Values | Where-Object { -not $_ }).Count) 개 제외)"
"../../../../bin" | Set-Content -Path (Join-Path $dest "_dll_dirs.txt") -Encoding ascii

# 5) 실행 파일(시작 스크립트)·라이선스·README
@'
@echo off
rem NASA-95 실행(콘솔 없음). 명령줄 옵션은 그대로 넘긴다: --project <파일>, --server --port 8765 --token <T>
start "" "%~dp0python\pythonw.exe" -m nasa95.ui %*
'@ | Set-Content -Path (Join-Path $app "NASA-95.cmd") -Encoding ascii
@'
@echo off
rem NASA-95 실행(콘솔 로그 보기)
"%~dp0python\pythonw.exe" --version >nul 2>&1
"%~dp0python\python.exe" -m nasa95.ui %*
'@ | Set-Content -Path (Join-Path $app "NASA-95-console.cmd") -Encoding ascii
$lic = Join-Path $app "licenses"
New-Item -ItemType Directory -Force $lic | Out-Null
$occtRoot = Get-ChildItem (Join-Path $thirdParty "occt") -Recurse -Depth 2 -Filter "LICENSE_LGPL_21.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($occtRoot) {
    Copy-Item $occtRoot.FullName (Join-Path $lic "OpenCASCADE-LICENSE_LGPL_21.txt")
    $exc = Join-Path $occtRoot.DirectoryName "OCCT_LGPL_EXCEPTION.txt"
    if (Test-Path $exc) { Copy-Item $exc (Join-Path $lic "OpenCASCADE-OCCT_LGPL_EXCEPTION.txt") }
}
$ngLic = Join-Path $thirdParty "netgen\src\LICENSE"
if (Test-Path $ngLic) { Copy-Item $ngLic (Join-Path $lic "Netgen-LICENSE(LGPL-2.1).txt") }
if (Test-Path (Join-Path $ribbon "LICENSE")) { Copy-Item (Join-Path $ribbon "LICENSE") (Join-Path $lic "SARibbon-LICENSE(MIT).txt") }
$psLic = Get-ChildItem $site -Filter "LICENSE*" -Recurse -Depth 2 | Where-Object { $_.FullName -match "PySide6" } | Select-Object -First 1
if ($psLic) { Copy-Item $psLic.FullName (Join-Path $lic "PySide6-LICENSE(LGPL-3.0).txt") }
Copy-Item (Join-Path $py "LICENSE.txt") (Join-Path $lic "Python-LICENSE.txt") -ErrorAction SilentlyContinue
@"
NASA-95 $Version — CAE Pre/Post 프로세서 (https://github.com/kwonbyeongchun/Nasa-95)

실행: NASA-95.cmd (또는 시작 메뉴의 NASA-95). 콘솔 로그가 필요하면 NASA-95-console.cmd.
프로젝트 파일: *.nasa95 (예전 *.ofep 도 열린다).
솔버: CalculiX(ccx), OpenSees, MyStran 은 이 배포본에 들어 있지 않다(각각 별도 프로그램, 라이선스가 다르다).
  설치한 뒤 설정(리본 → 설정·연동 → 설정)에서 실행 파일 경로를 지정하거나 환경 변수 NASA95_CCX / NASA95_OPENSEES / NASA95_MYSTRAN 을 둔다.
3D 화면은 Vulkan 을 쓴다(그래픽 드라이버의 vulkan-1.dll). 구성: 임베디드 Python $PythonEmbedVersion, PySide6 6.11.1(LGPL), SARibbon 2.9.5(MIT),
OpenCASCADE 8.0.1(LGPL 2.1 + 예외, 동적 링크), Netgen(LGPL 2.1, 동적 링크). 라이선스 문서는 licenses\ 에 있다.
"@ | Set-Content -Path (Join-Path $app "README.txt") -Encoding utf8

# 6) 자가 검사: 배포본의 Python 으로 코어를 불러 명령 하나를 실행한다
$check = & (Join-Path $py "python.exe") -c "import nasa95, PySideSARibbon; a = nasa95.App(); v = a.execute('app.version'); print(v)"
if ($LASTEXITCODE -ne 0) { throw "배포본 자가 검사 실패(DLL 누락?)" }
Write-Host "자가 검사: $check"

# 7) zip + 설치 파일
$zip = Join-Path $dist "NASA-95-$Version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $app -DestinationPath $zip -CompressionLevel Optimal
Write-Host "zip: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
if (-not $NoInstaller) {
    $iscc = Get-Command iscc -ErrorAction SilentlyContinue
    $isccPath = if ($iscc) { $iscc.Source } else { "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
    if (Test-Path $isccPath) {
        Run-Checked $isccPath @("/Q", "/DAppVersion=$Version", "/DAppDir=$app", "/DOutDir=$dist", (Join-Path $repo "installer\NASA-95.iss"))
        Write-Host "설치 파일: $dist\NASA-95-$Version-setup.exe ($([math]::Round((Get-Item "$dist\NASA-95-$Version-setup.exe").Length / 1MB)) MB)"
    } else {
        Write-Warning "Inno Setup(ISCC.exe) 이 없어 설치 파일은 만들지 않았습니다. zip 만 만들었습니다."
    }
}
