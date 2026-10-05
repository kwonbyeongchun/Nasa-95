"""Windows가 실제 창 위 테두리를 크기 조절 영역으로 판정하는지 검사한다."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
pytest.importorskip("PySide6")
from PySide6.QtCore import QPoint
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QToolButton
from nasa95 import App
from nasa95.ui import MainWindow

pytestmark = [pytest.mark.skipif(sys.platform != "win32", reason="Windows native frame"),
              pytest.mark.feature("RND-02")]


@pytest.fixture
def frame():
    qt = QApplication.instance() or QApplication([])
    w = MainWindow(App())
    w.resize(1360, 800)
    w.show()
    QTest.qWait(80)
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.SendMessageW.restype = ctypes.c_ssize_t
    user.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.GetWindowRect.restype = wintypes.BOOL
    user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    user.ClientToScreen.restype = wintypes.BOOL
    hwnd = int(w.winId())
    def hit(x, y):
        return user.SendMessageW(hwnd, 0x84, 0, (x & 65535) | ((y & 65535) << 16))
    def bounds():
        rect = wintypes.RECT()
        assert user.GetWindowRect(hwnd, ctypes.byref(rect))
        return rect
    def local_hit(point):
        ratio = w.devicePixelRatioF()
        point = wintypes.POINT(round(point.x() * ratio), round(point.y() * ratio))
        assert user.ClientToScreen(hwnd, ctypes.byref(point))
        return hit(point.x, point.y)
    yield w, hit, bounds, local_hit
    w.close()
    qt.processEvents()


def test_RND_T04_23_top_edge_and_corners(frame):
    w, hit, bounds, _ = frame
    r = bounds()
    for y in (r.top + 1, r.top + round(4 * w.devicePixelRatioF())):
        assert hit(r.left + 1, y) == 13  # HTTOPLEFT
        assert hit(r.right - 2, y) == 14  # HTTOPRIGHT
        # 아이콘·리본 탭·제목·창 제어 위도 같은 위쪽 경계다.
        for fraction in (0.02, 0.15, 0.5, 0.9):
            assert hit(r.left + round((r.right - r.left) * fraction), y) == 12
    w.move(-200, -80)
    QTest.qWait(40)
    r = bounds()
    assert r.left < 0
    assert hit(r.left + 1, r.top + 1) == 13
    assert hit((r.left + r.right) // 2, r.top + 1) == 12


def test_RND_T04_24_title_and_controls_keep_their_behavior(frame):
    w, _, _, hit = frame
    ribbon = w.ribbon
    left = ribbon.tabs.mapTo(w, ribbon.tabs.rect().topRight()).x()
    right = ribbon.quick.mapTo(w, QPoint()).x()
    assert hit(QPoint((left + right) // 2, 18)) == 2  # HTCAPTION
    for widget in (ribbon.tabs, ribbon.file_button, ribbon.search, ribbon.options,
                   w.findChild(QToolButton, "SACloseWindowButton")):
        assert hit(widget.mapTo(w, widget.rect().center())) == 1  # HTCLIENT


def test_RND_T04_25_non_resizable_states(frame):
    w, hit, bounds, _ = frame
    for state in (w.showMaximized, w.showFullScreen):
        state()
        QTest.qWait(40)
        r = bounds()
        assert hit((r.left + r.right) // 2, r.top + 1) not in (12, 13, 14)
    w.showNormal()
    QTest.qWait(40)
    w.setFixedWidth(w.width())
    r = bounds()
    assert hit(r.left + 1, r.top + 1) == 12  # 폭 고정이면 세로만 조절
    w.setFixedHeight(w.height())
    assert hit((r.left + r.right) // 2, r.top + 1) not in (12, 13, 14)
