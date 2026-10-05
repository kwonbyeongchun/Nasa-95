"""NASA-95 Python API.

UI 와 사용자 스크립트가 함께 쓰는 얇은 래퍼다. 모든 호출은 C++ 코어의 명령 계층으로 간다.
"""
import os as _os
import pathlib as _pathlib

# 코어가 쓰는 DLL(OpenCASCADE 등)의 폴더를 검색 경로에 더한다. 목록은 빌드할 때 만들어진다.
_dirs = _pathlib.Path(__file__).with_name("_dll_dirs.txt")
if hasattr(_os, "add_dll_directory") and _dirs.exists():
    for _line in _dirs.read_text(encoding="utf-8").splitlines():
        if _line.strip() and _os.path.isdir(_line.strip()):
            _os.add_dll_directory(_line.strip())

from .api import App, Collection, Handle, Nasa95Error

__all__ = ["App", "Collection", "Handle", "Nasa95Error"]
