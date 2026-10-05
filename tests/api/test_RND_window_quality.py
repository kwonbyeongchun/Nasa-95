"""숨긴 Win32 테스트 창에서 Vulkan SSAA·리사이즈·ID 픽킹을 검사한다."""
import ctypes
import sys
from ctypes import wintypes

import pytest

from test_VIEW_render import _available, box, needs_geometry

pytestmark = pytest.mark.skipif(sys.platform != "win32" or not _available(), reason="Windows Vulkan 필요")


@needs_geometry
@pytest.mark.feature("RND-02")
@pytest.mark.feature("RND-18")
@pytest.mark.feature("RND-24")
@pytest.mark.feature("RND-44")
def test_RND_T04_17_window_ssaa_resize_pick(app):
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.CreateWindowExW.restype = wintypes.HWND
    user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
    user.DestroyWindow.argtypes = [wintypes.HWND]
    user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    # WS_POPUP, WS_VISIBLE 없음: 사용자의 창과 바탕 화면을 건드리지 않는다.
    hwnd = user.CreateWindowExW(0, "STATIC", "NASA-95 render test", 0x80000000, 0, 0, 400, 300, None, None, None, None)
    assert hwnd, ctypes.get_last_error()
    try:
        part = box(app)
        app.execute("view.hud", triad=False, legend=False)
        app.execute("view.standard", name="top")
        app.view.attach_window(hwnd)
        frames = 0
        for width, height, mode in [(400, 300, "none"), (400, 300, "ssaa2"), (620, 420, "ssaa2"), (620, 420, "none"), (400, 300, "ssaa2")]:
            assert user.SetWindowPos(hwnd, None, 0, 0, width, height, 0x0004 | 0x0010)
            app.execute("view.quality", antialiasing=mode)
            app.view.present()
            d = app.execute("view.diagnostics")
            frame = d["last_frame"]
            # 현재 크기로 준비한 뒤 한 번만 그린다(이전 크기의 예비 프레임 없음).
            assert frame["frames"] == frames + 1
            frames = frame["frames"]
            scale = 2 if mode == "ssaa2" else 1
            assert frame["antialiasing"] == mode
            assert (frame["width"], frame["height"]) == (width, height)
            assert (frame["render_width"], frame["render_height"]) == (width * scale, height * scale)
            hit = app.view.pick(width // 2, height // 2)
            assert hit["hit"] and hit["part"] == part.id
            face = app.execute("geometry.entity_info", id=part.id, type="face", index=hit["index"])
            assert face["center"] == pytest.approx([5., 10., 30.])
            assert app.view.pick(2, 2) == {"hit": False}
        app.view.detach_window()
        assert app.execute("view.diagnostics")["gpu_memory"]["persistent"] == 0
    finally:
        app.view.detach_window()
        user.DestroyWindow(hwnd)
