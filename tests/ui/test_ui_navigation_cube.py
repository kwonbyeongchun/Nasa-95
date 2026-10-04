"""방향 큐브의 클릭은 표준 뷰, 드래그는 화면 중앙 회전으로 전달한다."""
import numpy as np
import pytest

pytest.importorskip("PySide6")
from PySide6.QtCore import QPointF, QEvent, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication
from openfep import App
from openfep.ui.viewport import Viewport


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
@pytest.mark.feature("RND-37")
def test_RND_T01_37_cube_click_and_drag_use_shared_commands(monkeypatch):
    qt = QApplication.instance() or QApplication([])
    app = App()
    camera = app.execute("view.standard", name="iso")
    viewport = Viewport(app)
    viewport.resize(400, 300)
    viewport._attached = True
    monkeypatch.setattr(viewport, "present", lambda: None)
    monkeypatch.setattr(viewport, "devicePixelRatioF", lambda: 1.5)
    back = np.subtract(camera["eye"], camera["target"])
    back /= np.linalg.norm(back)
    right = np.cross(-back, camera["up"])
    right /= np.linalg.norm(right)
    up = np.cross(back, right)
    x, y = (88 + 24 * right[0]) / 1.5, (450 - 80 - 24 * up[0]) / 1.5
    calls, picked = [], []
    original = app.execute

    def execute(command, **params):
        calls.append((command, params))
        return original(command, **params)

    monkeypatch.setattr(app, "execute", execute)
    viewport.picked.connect(picked.append)

    def event(kind, px, py, button=Qt.MouseButton.NoButton, buttons=Qt.MouseButton.LeftButton):
        point = QPointF(px, py)
        return QMouseEvent(kind, point, point, button, buttons, Qt.KeyboardModifier.NoModifier)

    try:
        viewport.mousePressEvent(event(QEvent.Type.MouseButtonPress, x, y, Qt.MouseButton.LeftButton))
        viewport.mouseReleaseEvent(event(QEvent.Type.MouseButtonRelease, x, y, Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton))
        assert ("view.transition_begin", {"name": "right"}) in calls
        assert app.execute("view.camera_get") == camera  # 클릭 순간에는 튀지 않고 다음 틱부터 움직인다.
        assert not picked
        cube_calls = [params for command, params in calls if command == "view.cube_pick"]
        assert cube_calls and all(params["width"] == 600 and params["height"] == 450 for params in cube_calls)
        app.execute("view.camera_set", **camera)
        app.execute("view.camera_set", pivot=[2., 3., 4.])
        calls.clear()
        viewport.mousePressEvent(event(QEvent.Type.MouseButtonPress, x, y, Qt.MouseButton.LeftButton))
        viewport.mouseMoveEvent(event(QEvent.Type.MouseMove, x + 12., y + 8.))
        viewport.mouseReleaseEvent(event(QEvent.Type.MouseButtonRelease, x + 12., y + 8., Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton))
        assert any(command == "view.orbit" for command, _ in calls)
        assert not any(command in ("view.orbit_begin", "view.standard", "view.transition_begin") for command, _ in calls)
        assert app.execute("view.camera_get")["pivot"] == camera["target"]
        assert app.execute("view.camera_get")["target"] == camera["target"]
        assert not picked
    finally:
        viewport._attached = False
        viewport.close()
        qt.processEvents()


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
def test_RND_T01_41_cube_animation_timing_and_interruption(monkeypatch):
    from types import SimpleNamespace
    from PySide6.QtGui import QHideEvent
    qt = QApplication.instance() or QApplication([])
    app = App()
    start = app.execute("view.standard", name="iso")
    goal = app.execute("view.standard", name="top")
    app.execute("view.camera_set", **start)
    viewport = Viewport(app)
    viewport._attached = True
    frames, elapsed = [], [0]
    monkeypatch.setattr(viewport, "present", lambda: frames.append(app.execute("view.camera_get")))
    monkeypatch.setattr(viewport.navigation_cube, "_elapsed", SimpleNamespace(start=lambda: None, elapsed=lambda: elapsed[0]))
    try:
        viewport.navigation_cube.start("top")
        assert viewport.navigation_cube._timer.isActive()
        assert app.execute("view.camera_get") == start
        for milliseconds in [80, 160, 320]:
            elapsed[0] = milliseconds
            viewport.navigation_cube.advance()
        assert frames[0] != start and frames[0] != goal
        assert frames[1] != frames[0] and frames[1] != goal
        assert frames[-1] == goal
        assert not viewport.navigation_cube._timer.isActive()
        viewport.navigation_cube.start("front")
        elapsed[0] = 120
        viewport.navigation_cube.advance()
        current = app.execute("view.camera_get")
        pos = QPointF(300., 100.)
        viewport.mousePressEvent(QMouseEvent(QEvent.Type.MouseButtonPress, pos, pos, Qt.MouseButton.LeftButton,
                                             Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier))
        assert not viewport.navigation_cube._timer.isActive()
        assert app.execute("view.transition_step", progress=1.) == {"active": False, "camera": current}
        viewport.navigation_cube.start("right")
        assert viewport.navigation_cube._timer.isActive()
        viewport.hideEvent(QHideEvent())
        assert not viewport.navigation_cube._timer.isActive()
        assert not app.execute("view.transition_step", progress=1.)["active"]
    finally:
        viewport._attached = False
        viewport.close()
        qt.processEvents()


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
def test_RND_T01_41_real_timer_keeps_event_loop_responsive(monkeypatch):
    from PySide6.QtCore import QTimer
    from PySide6.QtTest import QTest
    qt = QApplication.instance() or QApplication([])
    app = App()
    goal = app.execute("view.standard", name="top")
    start = app.execute("view.standard", name="iso")
    viewport = Viewport(app)
    viewport._attached = True
    frames, heartbeats = [], []
    monkeypatch.setattr(viewport, "present", lambda: frames.append(app.execute("view.camera_get")))
    try:
        QTimer.singleShot(80, lambda: heartbeats.append(app.execute("view.camera_get")))
        viewport.navigation_cube.start("top")
        assert viewport.navigation_cube._timer.isActive()
        assert app.execute("view.camera_get") == start
        QTest.qWait(500)
        assert len(frames) >= 3
        assert frames[0] != start and frames[0] != goal
        assert heartbeats and heartbeats[0] != goal  # 전환 중에도 다른 UI 이벤트를 처리한다.
        assert frames[-1] == goal
        assert not viewport.navigation_cube._timer.isActive()
    finally:
        viewport._attached = False
        viewport.close()
        qt.processEvents()


@pytest.mark.feature("RND-10")
@pytest.mark.feature("RND-37")
@pytest.mark.feature("RND-49")
@pytest.mark.parametrize("name,direction", [("front", [0., -1., 0.]), ("front-right", [1., -1., 0.]),
                                            ("front-right-top", [1., -1., 1.])])
def test_RND_T01_44_component_hover_click_and_leave(monkeypatch, name, direction):
    qt = QApplication.instance() or QApplication([])
    app = App()
    app.execute("view.camera_set", eye=(np.array(direction) * 10).tolist(), target=[0., 0., 0.], up=[0., 0., 1.])
    viewport = Viewport(app)
    viewport.resize(400, 300)
    viewport._attached = True
    monkeypatch.setattr(viewport, "devicePixelRatioF", lambda: 1.5)
    monkeypatch.setattr(viewport, "present", lambda: None)
    monkeypatch.setattr(app.view, "hover", lambda x, y: {"hit": False, "changed": False})
    redraws, calls, picked = [], [], []
    viewport.navigation_cube.redraw.connect(lambda: redraws.append(True))
    viewport.picked.connect(picked.append)
    original = app.execute

    def execute(command, **params):
        calls.append((command, params))
        return original(command, **params)

    monkeypatch.setattr(app, "execute", execute)
    point = QPointF(88 / 1.5, (450 - 80) / 1.5)
    try:
        viewport.mouseMoveEvent(QMouseEvent(QEvent.Type.MouseMove, point, point, Qt.MouseButton.NoButton,
                                           Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier))
        assert viewport.cursor().shape() == Qt.CursorShape.PointingHandCursor
        assert redraws
        assert app.execute("view.cube_hover", x=88., y=370., width=600, height=450) == {"hit": True, "view": name, "changed": False}
        viewport.leaveEvent(QEvent(QEvent.Type.Leave))
        assert viewport.cursor().shape() == Qt.CursorShape.ArrowCursor
        assert app.execute("view.cube_hover") == {"hit": False, "changed": False}
        assert len(redraws) == 2
        viewport.mousePressEvent(QMouseEvent(QEvent.Type.MouseButtonPress, point, point, Qt.MouseButton.LeftButton,
                                             Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier))
        viewport.mouseReleaseEvent(QMouseEvent(QEvent.Type.MouseButtonRelease, point, point, Qt.MouseButton.LeftButton,
                                               Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier))
        assert ("view.transition_begin", {"name": name}) in calls
        assert not picked
        assert viewport.navigation_cube._timer.isActive()
    finally:
        viewport._attached = False
        viewport.close()
        qt.processEvents()
