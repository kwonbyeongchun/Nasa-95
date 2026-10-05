"""방향 큐브 UI 컴포넌트: 입력·호버·전환 타이머만 소유한다.

그리기와 영역 판정·카메라 보간은 공용 C++ 명령으로 처리한다.
"""
from PySide6.QtCore import QObject, QPointF, QElapsedTimer, QTimer, Qt, Signal

from ..api import Nasa95Error


class NavigationCube(QObject):
    redraw = Signal()
    DURATION_MS = 320

    def __init__(self, app, viewport):
        super().__init__(viewport)
        self._app = app
        self._viewport = viewport
        self.pressed = False
        self._position = None
        self._elapsed = QElapsedTimer()
        self._timer = QTimer(self)
        self._timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self.advance)
        app.execute("view.hud", navigation_cube=True)

    def _coordinates(self, pos):
        ratio = self._viewport.devicePixelRatioF()
        return dict(x=pos.x() * ratio, y=pos.y() * ratio,
                    width=round(self._viewport.width() * ratio), height=round(self._viewport.height() * ratio))

    def hit_at(self, pos):
        return self._app.execute("view.cube_pick", **self._coordinates(pos))

    def hover(self, pos):
        self._position = QPointF(pos)
        result = self._app.execute("view.cube_hover", **self._coordinates(pos))
        self._viewport.setCursor(Qt.CursorShape.PointingHandCursor if result["hit"] else Qt.CursorShape.ArrowCursor)
        if result["changed"]:
            self.redraw.emit()
        return result["hit"]

    def leave(self):
        self._position = None
        self._viewport.unsetCursor()
        try:
            if self._app.execute("view.cube_hover")["changed"]:
                self.redraw.emit()
        except Nasa95Error:
            pass

    def press(self, pos, button):
        self.stop()
        self.pressed = False
        if self._viewport._attached and button == Qt.MouseButton.LeftButton:
            self.pressed = self.hit_at(pos)["hit"]
            if self.pressed:
                self.hover(pos)

    def release(self, pos, moved):
        consumed, self.pressed = self.pressed, False
        if consumed and not moved:
            hit = self.hit_at(pos)
            self.hover(pos)
            if hit["hit"]:
                self.start(hit["view"])
        return consumed

    def start(self, name):
        self.stop()
        if self._app.execute("view.transition_begin", name=name)["active"]:
            self._elapsed.start()
            self._timer.start()

    def advance(self):
        if not self._viewport._attached or self._viewport._broken:
            self.stop()
            return
        try:
            progress = min(1.0, self._elapsed.elapsed() / self.DURATION_MS)
            active = self._app.execute("view.transition_step", progress=progress)["active"]
            # 회전 중에도 고정된 마우스 위치 아래의 실제 영역을 강조한다.
            if self._position is not None:
                self.hover(self._position)
            self._viewport.present()
            if not active:
                self._timer.stop()
        except Nasa95Error:
            self.stop()

    def stop(self):
        if self._timer.isActive():
            self._timer.stop()
            try:
                self._app.execute("view.transition_cancel")
            except Nasa95Error:
                pass
