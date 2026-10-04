"""회전은 클릭 위치와 무관하게 현재 화면 중앙을 고정한다."""
import numpy as np
import pytest

pytest.importorskip("PySide6")
from PySide6.QtCore import QPointF, QEvent, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication
from openfep import App
from openfep.ui.viewport import Viewport


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-42")
@pytest.mark.parametrize("projection", ["orthographic", "perspective"])
@pytest.mark.parametrize("press", [(430.25, 255.5), (20., 20.), (770., 570.)])
def test_RND_T01_32_drag_uses_screen_center(monkeypatch, projection, press):
    qt = QApplication.instance() or QApplication([])
    app = App()
    if not app.execute("app.version")["geometry"]:
        pytest.skip("형상 커널 없음")
    part = app.model.parts.create(name="BOX")
    part.features.create_box(size=[10., 20., 30.])
    # 이동한 뷰: 모델 중심과 현재 화면 중심을 구분하고 이전 표면 pivot도 남겨 둔다.
    center = [8., 12., 15.]
    start = app.execute("view.camera_set", eye=[8., 12., 70.], target=center, up=[0., 1., 0.],
                        pivot=[7., 13., 30.], height=40., projection=projection)
    before = app.digest()
    app.execute("view.quality", antialiasing="ssaa2", simplify_during_interaction=True)
    viewport = Viewport(app)
    viewport.resize(800, 600)
    viewport._attached = True  # 이벤트 전달만 검사, 실제 창 표시는 하지 않는다.
    monkeypatch.setattr(viewport, "present", lambda: None)
    original = app.execute
    calls = []

    def execute(command, **params):
        if command in ("view.orbit_begin", "view.camera_set", "view.orbit"):
            calls.append((command, params, original("view.display_mode")["mode"]))
        return original(command, **params)

    monkeypatch.setattr(app, "execute", execute)

    def event(kind, x, y, button=Qt.MouseButton.NoButton, buttons=Qt.MouseButton.LeftButton):
        point = QPointF(x, y)
        return QMouseEvent(kind, point, point, button, buttons, Qt.KeyboardModifier.NoModifier)

    try:
        x, y = press
        viewport.mousePressEvent(event(QEvent.Type.MouseButtonPress, x, y, Qt.MouseButton.LeftButton))
        assert app.execute("view.camera_get") == start  # 버튼 누르기만으로 화면이 이동하지 않는다.
        viewport.mouseMoveEvent(event(QEvent.Type.MouseMove, x + 15.25, y - 15.25))
        pivot = app.execute("view.camera_get")["pivot"]
        viewport.mouseMoveEvent(event(QEvent.Type.MouseMove, x + 29.75, y - 30.5))
        assert [call[0] for call in calls] == ["view.camera_set", "view.orbit", "view.orbit"]
        assert calls[0][1] == {"pivot": center}
        assert calls[0][2] == "shaded_edges"
        assert calls[1][2] == calls[2][2] == "wireframe"
        assert calls[1][1] == {"dx": 15.25, "dy": -15.25, "height": 600}
        assert pivot == center
        camera = app.execute("view.camera_get")
        assert camera["pivot"] == camera["target"] == center
        assert np.linalg.norm(np.subtract(camera["eye"], center)) == pytest.approx(55.)
        assert camera["eye"] != start["eye"]
        assert camera["projection"] == projection and camera["height"] == 40.
        assert app.digest() == before
        viewport.mouseReleaseEvent(event(QEvent.Type.MouseButtonRelease, x + 29.75, y - 30.5, Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton))
        assert app.execute("view.display_mode")["mode"] == "shaded_edges"
        assert app.execute("view.quality_get")["antialiasing"] == "ssaa2"
        # 클릭 선택만 한 경우 회전을 시작하지 않는다.
        monkeypatch.setattr(app.view, "pick", lambda x, y: {"hit": False})
        viewport.mousePressEvent(event(QEvent.Type.MouseButtonPress, 400., 300., Qt.MouseButton.LeftButton))
        viewport.mouseReleaseEvent(event(QEvent.Type.MouseButtonRelease, 400., 300., Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton))
        assert len(calls) == 3
    finally:
        viewport._attached = False
        viewport.close()
        qt.processEvents()
