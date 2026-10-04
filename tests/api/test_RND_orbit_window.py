"""RND-T01-33: 숨긴 네이티브 창의 실제 크기·SSAA에서 회전 중심을 검사한다."""
import ctypes
import sys
from ctypes import wintypes

import pytest

from test_VIEW_render import _available, box, needs_geometry
from test_RND_orbit import project, top_camera

pytestmark = pytest.mark.skipif(sys.platform != "win32" or not _available(), reason="Windows Vulkan 필요")


@needs_geometry
@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-18")
def test_RND_T01_33_window_pivot_with_ssaa_and_resize(app):
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.CreateWindowExW.restype = wintypes.HWND
    user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
    user.DestroyWindow.argtypes = [wintypes.HWND]
    user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    hwnd = user.CreateWindowExW(0, "STATIC", "open-fep orbit test", 0x80000000, 0, 0, 400, 300, None, None, None, None)
    assert hwnd, ctypes.get_last_error()
    try:
        part = box(app)
        app.view.attach_window(hwnd)
        point = [7., 13., 30.]
        for width, height in [(400, 300), (620, 420)]:
            assert user.SetWindowPos(hwnd, None, 0, 0, width, height, 0x0004 | 0x0010)
            for projection in ["orthographic", "perspective"]:
                for aa in ["none", "ssaa2"]:
                    camera = top_camera(app, projection)
                    app.execute("view.quality", antialiasing=aa)
                    assert tuple(app.view.present()) == (width, height)
                    x, y = project(camera, point, width, height)
                    hit = app.view.orbit_begin(x, y)  # 실제 창 크기를 기본값으로 사용
                    assert hit["hit"] and hit["pivot"] == pytest.approx(point, abs=1e-10)
                    assert app.view.pick(round(x), round(y))["part"] == part.id
                    actual = app.view.orbit(0.05 * height, -0.04 * height)
                    assert project(actual, point, width, height) == pytest.approx([x, y], abs=1e-9)
                    app.view.present()
                    assert app.view.pick(round(x), round(y))["part"] == part.id
                    # 명시 크기와 기본 창 크기의 회전 감도도 같다.
                    app.execute("view.camera_set", **camera)
                    app.view.orbit_begin(x, y, width=width, height=height)
                    explicit = app.view.orbit(0.05 * height, -0.04 * height, height=height)
                    for key in ("eye", "target", "up"):
                        assert actual[key] == pytest.approx(explicit[key])
    finally:
        app.view.detach_window()
        user.DestroyWindow(hwnd)
