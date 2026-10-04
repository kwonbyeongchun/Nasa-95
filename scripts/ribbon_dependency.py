"""setup_ribbon.ps1용 Qt 개발 파일 준비와 바인딩 설치. 런타임 Qt는 PySide6 것을 쓴다."""
import hashlib
import json
from pathlib import Path
import shutil
import sys
import sysconfig
import urllib.request

QT_ARCHIVE = "qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
QT_URL = ("https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/"
          "qt6_6111/qt6_6111_msvc2022_64/qt.qt6.6111.win64_msvc2022_64/"
          "6.11.1-0-202605090529" + QT_ARCHIVE)
QT_SHA1 = "6f554628540ab947d48294e208cc3caae7f023d2"


def prepare(root):
    qt = root / "qt"
    target = qt / "6.11.1" / "msvc2022_64"
    if (target / "lib/cmake/Qt6/Qt6Config.cmake").is_file():
        print("Qt 6.11.1 development files ready")
        return
    qt.mkdir(parents=True, exist_ok=True)
    archive = qt / QT_ARCHIVE
    if not archive.is_file():
        pending = archive.with_suffix(".download")
        print("Downloading Qt 6.11.1 qtbase", flush=True)
        urllib.request.urlretrieve(QT_URL, pending)
        if hashlib.sha1(pending.read_bytes()).hexdigest() != QT_SHA1:
            raise RuntimeError("Qt archive checksum mismatch")
        pending.replace(archive)
    if hashlib.sha1(archive.read_bytes()).hexdigest() != QT_SHA1:
        raise RuntimeError("Cached Qt archive checksum mismatch")
    import py7zr
    with py7zr.SevenZipFile(archive) as package:
        package.extractall(target)


def install(root):
    target = Path(sysconfig.get_path("purelib")) / "PySideSARibbon"
    target.mkdir(parents=True, exist_ok=True)
    source = root / "saribbon"
    binary = source / "build-qt6.11.1/saribbon.pyd"
    installed = target / "saribbon.pyd"
    # 실행 중인 UI가 같은 DLL을 사용하고 있으면 덮어쓸 필요가 없다.
    if not installed.exists() or hashlib.sha256(binary.read_bytes()).digest() != hashlib.sha256(installed.read_bytes()).digest():
        try:
            shutil.copy2(binary, installed)
        except PermissionError as exc:
            raise RuntimeError("UI가 바인딩을 사용 중입니다. open-fep 창을 닫고 설치를 다시 실행하세요.") from exc
    shutil.copy2(source / "2.9.5/LICENSE", target / "LICENSE")
    (target / "__init__.py").write_text('''"""SARibbon v2.9.5 / PySide6 6.11.1 (MIT)."""
import os
from pathlib import Path
import PySide6
import shiboken6
from PySide6 import QtWidgets
from PySide6.QtCore import qVersion
if PySide6.__version__ != "6.11.1" or shiboken6.__version__ != "6.11.1" or qVersion() != "6.11.1":
    raise ImportError("SARibbon build requires PySide6, Shiboken and Qt 6.11.1")
_dll_handles = []
if os.name == "nt":
    for package in (PySide6, shiboken6):
        _dll_handles.append(os.add_dll_directory(str(Path(package.__file__).parent)))
from . import saribbon
__version__ = "2.9.5"
''', encoding="utf-8")
    (target / "build-info.json").write_text(json.dumps({
        "SARibbon": "2.9.5", "commit": "a21d30c2a8495da92c7db3adcdc124699b0f2a60",
        "Qt": "6.11.1", "python": sys.version, "license": "MIT",
        "frameless": "bundled SAFramelessHelper", "QWindowKit": False,
    }, indent=2), encoding="utf-8")


if __name__ == "__main__":
    {"prepare": prepare, "install": install}[sys.argv[1]](Path(sys.argv[2]).resolve())
