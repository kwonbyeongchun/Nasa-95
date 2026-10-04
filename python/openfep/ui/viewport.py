"""3D 뷰포트: Qt 창의 네이티브 핸들에 C++ Vulkan 렌더러가 직접 그린다(아키텍처 규칙 4, 6).

이 위젯은 그리지 않는다 — 마우스 입력을 렌더러의 카메라 조작으로 넘기고, 그릴 때가 되면 present 를 부를 뿐이다.
"""
from __future__ import annotations

from PySide6.QtCore import QEvent, QPointF, QRect, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QWidget

from ..api import App, OfepError
from .navigation_cube import NavigationCube


class _RegionBand(QWidget):
    """영역 확대의 선택 사각형. 뷰포트는 Vulkan 네이티브 창이라 자식 위젯(QRubberBand)은 그 위에 불투명하게 뜬다 —
    대신 DWM 이 합성하는 반투명 최상위 창(레이어드 창)을 화면 좌표에 띄워 안을 비치게 그린다. 마우스는 통과시킨다."""

    def __init__(self, viewport: QWidget):
        super().__init__(None, Qt.WindowType.ToolTip | Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint
                         | Qt.WindowType.WindowTransparentForInput)
        self._viewport = viewport
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)

    def set_rect(self, rect: QRect) -> None:  # 뷰포트 좌표. 뷰포트 밖으로 끌어도 상자는 뷰포트 안까지만
        rect = rect.intersected(self._viewport.rect())
        top_left = self._viewport.mapToGlobal(rect.topLeft())
        self.setGeometry(QRect(top_left, rect.size()))
        if not self.isVisible():
            self.show()
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(0, 120, 215, 50))
        p.setPen(QPen(QColor(0, 120, 215), 1, Qt.PenStyle.DashLine))
        p.drawRect(self.rect().adjusted(0, 0, -1, -1))


class Viewport(QWidget):
    picked = Signal(dict)       # 클릭한 자리의 객체
    failed = Signal(str)        # 렌더러 오류(한 번만)
    presented = Signal()        # 프레임을 낸 뒤(범례 등 갱신용)
    hovered = Signal(dict)      # 마우스 아래 객체가 바뀜
    zoom_region_done = Signal()  # 영역 확대를 마쳤거나 취소함(버튼의 눌림을 풀기 위해)

    def __init__(self, app: App, parent=None):
        super().__init__(parent)
        self._app = app
        self._attached = False
        self._broken = False
        self._needs_frame = True
        self._last = QPointF()
        self._press = QPointF()
        self._button = Qt.MouseButton.NoButton
        self._moved = False
        self._interaction_mode = None  # 조작 중 간소화 전의 표시 모드
        # 영역 확대(view.zoom_region): 모드가 켜지면 왼쪽 끌기가 회전 대신 사각형을 고른다. 사각형은 반투명 최상위 창으로 보인다
        self._zoom_region = False
        self._band = _RegionBand(self)
        # 크기/노출 이벤트에서 GPU 완료를 기다리지 않는다. 연속 요청은 최신 크기로 합친다.
        self._render_timer = QTimer(self)
        self._render_timer.setSingleShot(True)
        self._render_timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._render_timer.timeout.connect(self._present_pending)
        self.navigation_cube = NavigationCube(app, self)
        self.navigation_cube.redraw.connect(self.refresh)
        # 창에서는 결과 범례를 Qt 위젯(legend.py)이 그린다 — 렌더러 HUD 범례까지 두면 같은 막대가 둘 보인다(사용자 결정 2026-10-04).
        # HUD 범례는 창 없는 스크린샷·PNG 내보내기용으로만 남는다(view.hud 설정은 project.new 에도 유지된다)
        try:
            app.execute("view.hud", legend=False)
        except OfepError:
            pass
        # Vulkan에 필요한 HWND는 이 위젯뿐이다. 부모/도크까지 네이티브 창으로 만들지 않는다.
        self.setAttribute(Qt.WidgetAttribute.WA_DontCreateNativeAncestors)
        self.setAttribute(Qt.WidgetAttribute.WA_PaintOnScreen)
        self.setAttribute(Qt.WidgetAttribute.WA_NativeWindow)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self.setMinimumSize(200, 150)

    # Qt 가 이 위젯에 그리지 않게 한다
    def paintEngine(self):
        return None

    def attach(self) -> None:
        if self._attached or self._broken:
            return
        try:
            self._app.view.attach_window(int(self.winId()))
            self._attached = True
        except OfepError as e:
            self._broken = True
            self.failed.emit(str(e))

    def present(self) -> None:
        self._render_timer.stop()  # 명시적 출력이 이미 대기 중인 갱신도 처리한다.
        if not self._attached:
            self.attach()
        if not self._attached:
            return
        try:
            self._app.view.present()
            self._needs_frame = False
            self.presented.emit()
        except OfepError as e:
            self._broken = True
            self._attached = False
            self.failed.emit(str(e))

    def refresh(self) -> None:
        """모델이 바뀌었을 때(장면은 렌더러가 캐시하고 바뀐 것만 다시 만든다)."""
        self._needs_frame = True
        self.update()

    # --- Qt 이벤트
    def _schedule_present(self, delay=16):
        if self.isVisible() and not self._broken and not self._render_timer.isActive():
            self._render_timer.start(delay)

    def _present_pending(self):
        if self.isVisible() and not self.window().isMinimized():
            self.present()

    def showEvent(self, event):
        super().showEvent(event)
        self._needs_frame = True
        self.attach()
        self._schedule_present()

    def paintEvent(self, event):
        # 이미 출력한 크기의 뒤늦은 WM_PAINT는 마지막 Vulkan 화면으로 충족된다.
        if self._needs_frame:
            self._schedule_present()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._needs_frame = True
        self._schedule_present(33)

    def event(self, event):
        if event.type() == QEvent.Type.DevicePixelRatioChange:
            self.refresh()
        return super().event(event)

    def hideEvent(self, event):
        self.navigation_cube.stop()
        self.navigation_cube.leave()
        self._render_timer.stop()
        super().hideEvent(event)

    def closeEvent(self, event):
        self.navigation_cube.stop()
        self.navigation_cube.leave()
        self._render_timer.stop()
        if self._attached:
            self._app.view.detach_window()
            self._attached = False
        super().closeEvent(event)

    # ---- 영역 확대 모드(3D 뷰 탭의 "영역 확대" 버튼)
    def set_zoom_region_mode(self, on: bool) -> None:
        self._zoom_region = bool(on)
        self.setCursor(Qt.CursorShape.CrossCursor if on else Qt.CursorShape.ArrowCursor)
        if not on:
            self._band.hide()

    def _finish_zoom_region(self, end: QPointF) -> None:
        self._band.hide()
        rect = QRect(self._press.toPoint(), end.toPoint()).normalized().intersected(self.rect())
        self.set_zoom_region_mode(False)
        self.zoom_region_done.emit()
        if rect.width() < 3 or rect.height() < 3 or not self._attached:  # 끌지 않고 누르기만 하면 취소
            return
        try:
            self._app.execute("view.zoom_region", x0=rect.left(), y0=rect.top(), x1=rect.right(), y1=rect.bottom(),
                              width=self.width(), height=self.height())
        except OfepError:
            return
        self.present()

    def mousePressEvent(self, event):
        self._last = event.position()
        self._press = event.position()
        self._button = event.button()
        self._moved = False
        if self._zoom_region and event.button() == Qt.MouseButton.LeftButton:
            self._band.set_rect(QRect(event.position().toPoint(), event.position().toPoint()))
            self.setFocus()
            return
        try:
            self.navigation_cube.press(event.position(), event.button())
        except OfepError:
            pass
        self.setFocus()

    def leaveEvent(self, event):
        self.navigation_cube.leave()
        super().leaveEvent(event)

    def mouseMoveEvent(self, event):
        pos = event.position()
        if self._zoom_region and self._button == Qt.MouseButton.LeftButton:
            self._band.set_rect(QRect(self._press.toPoint(), pos.toPoint()).normalized())
            self._moved = True
            return
        dx, dy = pos.x() - self._last.x(), pos.y() - self._last.y()
        self._last = pos
        if dx == 0 and dy == 0 or not self._attached:
            return
        try:
            if self._button in (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton, Qt.MouseButton.MiddleButton) and not self._moved:
                if self._button == Qt.MouseButton.LeftButton:
                    # 현재 화면 중앙(target)을 고정 중심으로 쓴다. 클릭 위치나 모델 표면에 의존하지 않는다.
                    camera = self._app.execute("view.camera_get")
                    self._app.execute("view.camera_set", pivot=camera["target"])
                self._begin_interaction()
            if self._button == Qt.MouseButton.LeftButton:
                self._app.view.orbit(dx, dy, height=self.height())
            elif self._button in (Qt.MouseButton.RightButton, Qt.MouseButton.MiddleButton):
                self._app.view.pan(dx, dy)
            else:  # 버튼 없이 움직이면 마우스 오버 강조(RND-49)
                over_cube = self.navigation_cube.hover(pos)
                hover = self._app.view.hover(-1, -1) if over_cube else self._app.view.hover(pos.x(), pos.y())
                if hover.get("changed"):
                    self.hovered.emit(hover)
                    self.present()
                return
        except OfepError:
            return
        self._moved = True
        self.present()

    def mouseReleaseEvent(self, event):
        if self._zoom_region and event.button() == Qt.MouseButton.LeftButton:
            self._button = Qt.MouseButton.NoButton
            self._finish_zoom_region(event.position())
            return
        if event.button() == Qt.MouseButton.LeftButton and self._attached:
            try:
                consumed = self.navigation_cube.release(event.position(), self._moved)
                if not consumed and not self._moved:
                    pos = event.position().toPoint()
                    self.picked.emit(self._app.view.pick(pos.x(), pos.y()))
            except OfepError:
                pass
        self._button = Qt.MouseButton.NoButton
        self._end_interaction()

    # 조작 중 간소화(RND-42, view.quality simplify_during_interaction): 끌기 시작하면 와이어프레임으로, 놓으면 원래 표시 모드로
    def _begin_interaction(self) -> None:
        if self._interaction_mode is not None:
            return
        try:
            if not self._app.execute("view.quality_get")["simplify_during_interaction"]:
                return
            mode = self._app.execute("view.display_mode")["mode"]
        except OfepError:
            return
        if mode == "wireframe":
            return
        self._interaction_mode = mode
        try:
            self._app.execute("view.display_mode", mode="wireframe")
        except OfepError:
            self._interaction_mode = None

    def _end_interaction(self) -> None:
        if self._interaction_mode is None:
            return
        mode, self._interaction_mode = self._interaction_mode, None
        try:
            self._app.execute("view.display_mode", mode=mode)
        except OfepError:
            return
        self.present()

    def wheelEvent(self, event):
        self.navigation_cube.stop()
        if not self._attached:
            return
        steps = event.angleDelta().y() / 120.0
        pos = event.position()
        try:
            self._app.view.zoom(1.15 ** steps, pos.x(), pos.y())
        except OfepError:
            return
        self.present()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape and self._zoom_region:  # 영역 확대 취소
            self.set_zoom_region_mode(False)
            self.zoom_region_done.emit()
            return
        key = event.text().lower()
        views = {"f": None, "1": "front", "2": "back", "3": "left", "4": "right", "5": "top", "6": "bottom", "7": "iso"}
        if key in views:
            self.navigation_cube.stop()
            try:
                if views[key] is None:
                    self._app.execute("view.fit")
                else:
                    self._app.execute("view.standard", name=views[key])
            except OfepError:
                return
            self.present()
        else:
            super().keyPressEvent(event)
