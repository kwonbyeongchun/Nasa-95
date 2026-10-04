"""연속 크기/paint 요청을 합치고 숨김·종료 후 대기 중 출력을 취소한다."""
from types import SimpleNamespace
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
pytest.importorskip("PySide6")
from PySide6.QtCore import QEventLoop, QTimer
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from openfep.ui.viewport import Viewport


@pytest.fixture
def viewport():
    qt = QApplication.instance() or QApplication([])
    calls = []
    detached = []
    # 창 수명 이벤트는 큐브 호버도 해제한다. 실제 명령의 반환 계약을 유지한다.
    def execute(name, **params):
        if name == "view.cube_hover":
            return {"hit": False, "changed": False}
        if name == "view.hud":
            return {}
        raise AssertionError(f"Unexpected command: {name}")
    app = SimpleNamespace(execute=execute)
    app.view = SimpleNamespace(attach_window=lambda hwnd: None,
                               detach_window=lambda: detached.append(True))
    v = Viewport(app)
    app.view.present = lambda: calls.append((v.width(), v.height()))
    v.resize(400, 300)
    v.show()
    QTest.qWait(100)
    calls.clear()
    yield v, calls, detached
    v.close()
    qt.processEvents()


@pytest.mark.feature("RND-02")
@pytest.mark.feature("RND-05")
def test_RND_T04_20_resize_coalesces_and_draws_latest_size(viewport):
    v, calls, _ = viewport
    for i in range(50):
        v.resize(401 + i, 301 + i)
        v.paintEvent(None)
    assert calls == []  # resize/paint 이벤트 처리 중 GPU를 기다리지 않는다.
    QTest.qWait(100)
    assert calls == [(450, 350)]
    QTest.qWait(80)
    assert len(calls) == 1  # 화면이 그대로면 추가 출력하지 않는다.


@pytest.mark.feature("RND-02")
@pytest.mark.feature("RND-05")
def test_RND_T04_21_continuous_resize_has_live_frames(viewport):
    v, calls, _ = viewport
    loop = QEventLoop()
    timer = QTimer()
    steps = []
    def step():
        steps.append(True)
        v.resize(400 + len(steps), 300 + len(steps))
        if len(steps) == 40:
            timer.stop()
            loop.quit()
    timer.timeout.connect(step)
    timer.start(8)
    loop.exec()
    assert len(calls) >= 2  # 드래그가 끝나기 전에도 중간 화면을 출력한다.
    QTest.qWait(100)
    assert calls[-1] == (440, 340)
    assert len(calls) < 40  # 모든 크기 변경마다 렌더링하지 않는다.


@pytest.mark.feature("RND-02")
@pytest.mark.feature("RND-05")
def test_RND_T04_22_pending_frame_cancelled_on_hide_and_close(viewport):
    v, calls, detached = viewport
    v.resize(440, 330)
    v.hide()
    QTest.qWait(80)
    assert not calls
    v.show()
    QTest.qWait(100)
    assert calls[-1] == (440, 330)
    calls.clear()
    v.resize(450, 340)
    v.close()
    QTest.qWait(80)
    assert not calls and detached == [True]
