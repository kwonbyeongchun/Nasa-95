"""범례 막대(결과 컨투어의 색 → 값). 3D 뷰 옆의 보통 Qt 위젯이다(3D 장면은 렌더러가 그린다).
색상표·단계 수는 렌더러가 `view.diagnostics` 의 legend 에 돌려주는 값(`view.legend` 설정)을 그대로 따른다."""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QLinearGradient, QPainter, QPalette
from PySide6.QtWidgets import QWidget

# 렌더러(render/src/view.cpp colormap)와 같은 색 정의
_STOPS = {
    "rainbow": [(0, 0, 1), (0, 1, 1), (0, 1, 0), (1, 1, 0), (1, 0, 0)],
    "grayscale": [(0.1, 0.1, 0.1), (0.95, 0.95, 0.95)],
    "blue_red": [(0.2, 0.3, 0.9), (0.95, 0.95, 0.95), (0.9, 0.2, 0.2)],
    "heat": [(0, 0, 0), (1, 0, 0), (1, 1, 0), (1, 1, 1)],
}


def colormap(t: float, name: str = "rainbow", levels: int = 0) -> QColor:
    """0~1 → 색. levels > 0 이면 구간의 가운데 색(단계별 범례)."""
    t = min(max(t, 0.0), 1.0)
    if levels > 0:
        t = (min(int(t * levels), levels - 1) + 0.5) / levels
    stops = _STOPS.get(name, _STOPS["rainbow"])
    x = t * (len(stops) - 1)
    seg = min(len(stops) - 2, int(x))
    f = x - seg
    c = [stops[seg][k] + f * (stops[seg + 1][k] - stops[seg][k]) for k in range(3)]
    return QColor(int(round(255 * c[0])), int(round(255 * c[1])), int(round(255 * c[2])))


class LegendBar(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.legend: dict | None = None
        self.setFixedWidth(110)
        self.hide()

    def set_legend(self, legend: dict | None) -> None:
        self.legend = legend
        self.setVisible(legend is not None)
        self.update()

    def paintEvent(self, event):
        if not self.legend:
            return
        p = QPainter(self)
        p.fillRect(self.rect(), self.palette().color(QPalette.ColorRole.Base))
        top, bottom, x, w = 40, self.height() - 30, 10, 22
        name = self.legend.get("colormap", "rainbow")
        levels = int(self.legend.get("levels", 0) or 0)
        if levels > 0:  # 단계별: 띠를 levels 개로 나눠 칠한다(위가 큰 값)
            for i in range(levels):
                y0 = top + (bottom - top) * i / levels
                y1 = top + (bottom - top) * (i + 1) / levels
                p.fillRect(x, int(y0), w, int(y1) - int(y0) + 1, colormap((levels - i - 0.5) / levels, name, levels))
        else:
            grad = QLinearGradient(0, top, 0, bottom)
            for i in range(33):
                grad.setColorAt(i / 32, colormap(1 - i / 32, name))
            p.fillRect(x, top, w, bottom - top, grad)
        p.setPen(self.palette().color(QPalette.ColorRole.Text))
        p.drawRect(x, top, w, bottom - top)
        lo, hi = self.legend["min"], self.legend["max"]
        p.drawText(4, 16, f"{self.legend['field']}")
        p.drawText(4, 30, f"{self.legend['component']}")
        steps = levels if 0 < levels <= 16 else 8
        for i in range(steps + 1):
            y = top + (bottom - top) * i / steps
            value = hi - (hi - lo) * i / steps
            p.drawLine(x + w, int(y), x + w + 4, int(y))
            p.drawText(x + w + 7, int(y) + 4, f"{value:.3g}")
        p.end()
