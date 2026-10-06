"""함수 창(사용자 요청 2026-10-06): 함수(표·진폭·수식)를 더블클릭하면 **그래프**가 보인다.

- 표·진폭(amplitude): 2열 표 편집기 + 선 그래프(QPainter — 패널용 2D 그림이라 규칙 6 의 3D 화면과는 무관) + 통계(점 수·구간·최소·최대·최대 절댓값)
  + 파일에서 가져오기(2열 텍스트·CSV, PEER NGA `.AT2`) 와 값 배율(예: g → mm/s² 9810) + CSV 내보내기
- 수식: t(또는 x) 를 0~범위에서 표본화해 그린다. x, y, z 를 함께 쓰는 공간 분포 수식은 그리지 않는다
모든 변경은 function.create_*/update 명령으로만 간다.
"""
from __future__ import annotations

import csv
import json
import math
import pathlib
import re

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (QComboBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout, QHBoxLayout, QLabel, QLineEdit, QMessageBox,
                               QPushButton, QSizePolicy, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from ..api import App, Nasa95Error


def read_two_columns(path: str) -> list[list[float]]:
    """2열(x, y) 텍스트·CSV 또는 PEER NGA .AT2(헤더 4줄, NPTS=·DT=, 값 나열) → [[x, y], …]."""
    p = pathlib.Path(path)
    text = p.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    if p.suffix.lower() == ".at2" or any("NPTS=" in ln for ln in lines[:6]):
        head = next(ln for ln in lines[:6] if "NPTS=" in ln)
        npts = int(re.search(r"NPTS=\s*(\d+)", head).group(1))
        dt = float(re.search(r"DT=\s*([0-9.Ee+-]+)", head).group(1))
        start = lines.index(head) + 1
        vals = [float(v) for ln in lines[start:] for v in ln.replace(",", " ").split()][:npts]
        return [[i * dt, v] for i, v in enumerate(vals)]
    out = []
    for ln in lines:
        s = ln.strip()
        if not s or s[0] in "#%!;":
            continue
        parts = s.replace(",", " ").replace("\t", " ").split()
        try:
            nums = [float(v) for v in parts[:2]]
        except ValueError:
            continue  # 머리글 줄
        if len(nums) == 2:
            out.append(nums)
    return out


class FunctionPlot(QWidget):
    """점 목록을 선 그래프로. 마우스를 올리면 가장 가까운 점의 값을 보여 준다."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.points: list[list[float]] = []
        self.xlabel, self.ylabel = "x", "y"
        self.hover: int | None = None
        self.setMouseTracking(True)
        self.setMinimumHeight(220)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_points(self, pts: list[list[float]], xlabel="x", ylabel="y"):
        self.points = [[float(a), float(b)] for a, b in pts if a is not None and b is not None]
        self.xlabel, self.ylabel = xlabel, ylabel
        self.hover = None
        self.update()

    # 데이터 → 화면 좌표
    def _frame(self):
        m_l, m_r, m_t, m_b = 64, 16, 14, 30
        return QRectF(m_l, m_t, max(10, self.width() - m_l - m_r), max(10, self.height() - m_t - m_b))

    def _ranges(self):
        xs = [p[0] for p in self.points]
        ys = [p[1] for p in self.points]
        x0, x1 = (min(xs), max(xs)) if xs else (0.0, 1.0)
        y0, y1 = (min(ys), max(ys)) if ys else (0.0, 1.0)
        if x1 - x0 <= 0:
            x1 = x0 + 1.0
        if y1 - y0 <= 0:
            y0, y1 = y0 - 1.0, y1 + 1.0
        pad = 0.05 * (y1 - y0)
        return x0, x1, y0 - pad, y1 + pad

    def _to_screen(self, x, y, fr, rg):
        x0, x1, y0, y1 = rg
        return QPointF(fr.left() + (x - x0) / (x1 - x0) * fr.width(), fr.bottom() - (y - y0) / (y1 - y0) * fr.height())

    @staticmethod
    def _ticks(a, b, n=5):
        span = b - a
        if span <= 0:
            return [a]
        raw = span / n
        mag = 10 ** math.floor(math.log10(raw))
        step = min((s * mag for s in (1, 2, 2.5, 5, 10)), key=lambda s: abs(s - raw))
        t = math.ceil(a / step) * step
        out = []
        while t <= b + 1e-9 * span:
            out.append(round(t, 10))
            t += step
        return out

    def mouseMoveEvent(self, e):
        if not self.points:
            return
        fr, rg = self._frame(), self._ranges()
        x0, x1 = rg[0], rg[1]
        xv = x0 + (e.position().x() - fr.left()) / fr.width() * (x1 - x0)
        # x 오름차순이 보통이지만 보장되지 않으므로 전수 탐색(점 수는 수천 개)
        self.hover = min(range(len(self.points)), key=lambda i: abs(self.points[i][0] - xv))
        self.update()

    def leaveEvent(self, e):
        self.hover = None
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        pal = self.palette()
        text = pal.text().color()
        grid = QColor(text)
        grid.setAlpha(50)
        fr = self._frame()
        p.fillRect(self.rect(), pal.base().color())
        p.setPen(QPen(grid, 1))
        p.drawRect(fr)
        if not self.points:
            p.setPen(text)
            p.drawText(fr, Qt.AlignmentFlag.AlignCenter, "점이 없습니다")
            return
        rg = self._ranges()
        x0, x1, y0, y1 = rg
        font = QFont(self.font())
        font.setPointSizeF(max(7.0, self.font().pointSizeF() - 1))
        p.setFont(font)
        for xt in self._ticks(x0, x1):
            s = self._to_screen(xt, y0, fr, rg)
            p.setPen(QPen(grid, 1, Qt.PenStyle.DotLine))
            p.drawLine(QPointF(s.x(), fr.top()), QPointF(s.x(), fr.bottom()))
            p.setPen(text)
            p.drawText(QRectF(s.x() - 40, fr.bottom() + 2, 80, 16), Qt.AlignmentFlag.AlignHCenter, f"{xt:g}")
        for yt in self._ticks(y0, y1):
            s = self._to_screen(x0, yt, fr, rg)
            p.setPen(QPen(grid, 1, Qt.PenStyle.DotLine))
            p.drawLine(QPointF(fr.left(), s.y()), QPointF(fr.right(), s.y()))
            p.setPen(text)
            p.drawText(QRectF(0, s.y() - 8, fr.left() - 4, 16), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, f"{yt:g}")
        if y0 < 0 < y1:
            z = self._to_screen(x0, 0.0, fr, rg)
            p.setPen(QPen(text, 1))
            p.drawLine(QPointF(fr.left(), z.y()), QPointF(fr.right(), z.y()))
        path = QPainterPath()
        for i, (x, y) in enumerate(self.points):
            s = self._to_screen(x, y, fr, rg)
            if i == 0:
                path.moveTo(s)
            else:
                path.lineTo(s)
        p.setPen(QPen(QColor("#1F6FEB"), 1.4))
        p.drawPath(path)
        if len(self.points) <= 200:  # 점이 적으면 점도 찍는다
            p.setBrush(QColor("#1F6FEB"))
            for x, y in self.points:
                p.drawEllipse(self._to_screen(x, y, fr, rg), 2.2, 2.2)
        p.setPen(text)
        p.drawText(QRectF(fr.left(), fr.bottom() + 14, fr.width(), 16), Qt.AlignmentFlag.AlignHCenter, self.xlabel)
        p.save()
        p.translate(12, fr.center().y())
        p.rotate(-90)
        p.drawText(QRectF(-80, -8, 160, 16), Qt.AlignmentFlag.AlignCenter, self.ylabel)
        p.restore()
        if self.hover is not None and 0 <= self.hover < len(self.points):
            x, y = self.points[self.hover]
            s = self._to_screen(x, y, fr, rg)
            p.setPen(QPen(QColor("#C72E0F"), 1, Qt.PenStyle.DashLine))
            p.drawLine(QPointF(s.x(), fr.top()), QPointF(s.x(), fr.bottom()))
            p.setBrush(QColor("#C72E0F"))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawEllipse(s, 3.5, 3.5)
            label = f"{self.xlabel} = {x:g}, {self.ylabel} = {y:g}"
            w = p.fontMetrics().horizontalAdvance(label) + 10
            bx = min(s.x() + 8, fr.right() - w)
            box = QRectF(bx, fr.top() + 4, w, 18)
            p.setBrush(pal.base().color())
            p.setPen(QPen(grid, 1))
            p.drawRect(box)
            p.setPen(text)
            p.drawText(box, Qt.AlignmentFlag.AlignCenter, label)


class PointsTable(QTableWidget):
    """2열 표 편집기. 값이 바뀌면 changed 콜백."""

    def __init__(self, headers: tuple[str, str], on_change, parent=None):
        super().__init__(0, 2, parent)
        self.setHorizontalHeaderLabels(list(headers))
        self.horizontalHeader().setStretchLastSection(True)
        self.verticalHeader().setDefaultSectionSize(20)
        self._on_change = on_change
        self._loading = False
        self.itemChanged.connect(lambda _item: None if self._loading else on_change())

    def set_points(self, pts: list[list[float]]):
        self._loading = True
        self.setRowCount(len(pts))
        for r, (a, b) in enumerate(pts):
            self.setItem(r, 0, QTableWidgetItem(f"{a:g}"))
            self.setItem(r, 1, QTableWidgetItem(f"{b:g}"))
        self._loading = False

    def points(self) -> list[list[float]]:
        out = []
        for r in range(self.rowCount()):
            a, b = self.item(r, 0), self.item(r, 1)
            if a is None or b is None or not a.text().strip() and not b.text().strip():
                continue
            out.append([float(a.text()), float(b.text())])
        return out

    def add_row(self):
        r = self.rowCount()
        self.insertRow(r)
        self.setItem(r, 0, QTableWidgetItem(""))
        self.setItem(r, 1, QTableWidgetItem(""))
        self.scrollToBottom()

    def remove_selected(self):
        rows = sorted({i.row() for i in self.selectedItems()}, reverse=True)
        for r in rows:
            self.removeRow(r)
        self._on_change()


class FunctionDialog(QDialog):
    """함수 만들기·편집: 표·진폭은 표 + 그래프, 수식은 수식 + 표본 그래프."""

    AXES = {"table": ("x", "y"), "amplitude": ("시간", "값"), "expression": ("t", "값")}

    def __init__(self, app: App, sub: str = "", obj: dict | None = None, parent=None):
        super().__init__(parent)
        self.app, self.obj = app, obj
        self.sub = obj["props"].get("type", "table") if obj else (sub or "table")
        self.result_id: int | None = obj["id"] if obj else None
        labels = {"table": "표 함수", "amplitude": "진폭(시간 이력)", "expression": "수식"}
        self.setWindowTitle(("편집: " if obj else "새 ") + labels.get(self.sub, "함수"))
        props = (obj or {}).get("props", {})
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.name = QLineEdit(obj["name"] if obj else "")
        self.name.setPlaceholderText("비우면 자동")
        form.addRow("이름", self.name)
        self.expression: QLineEdit | None = None
        self.time_ref: QComboBox | None = None
        self.table: PointsTable | None = None
        if self.sub == "expression":
            self.expression = QLineEdit(props.get("expression", ""))
            self.expression.setPlaceholderText("예: 1000*sin(2*pi*t)   변수 x, y, z, t")
            self.expression.textChanged.connect(self._refresh)
            form.addRow("수식", self.expression)
            self.t_max = QLineEdit("10")
            self.t_max.setToolTip("그래프 표본 구간 0 ~ 이 값(t 또는 x 하나만 쓰는 수식만 그린다)")
            self.t_max.textChanged.connect(self._refresh)
            form.addRow("그래프 구간", self.t_max)
        else:
            if self.sub == "amplitude":
                self.time_ref = QComboBox()
                self.time_ref.addItems(["", "step", "total"])
                self.time_ref.setCurrentText(props.get("time", "") or "")
                self.time_ref.setToolTip("기준 시간: step(스텝 시작 기준, 기본) / total(해석 전체 시간)")
                form.addRow("기준 시간", self.time_ref)
        layout.addLayout(form)
        body = QHBoxLayout()
        if self.sub != "expression":
            left = QVBoxLayout()
            self.table = PointsTable(self.AXES[self.sub], self._refresh)
            self.table.set_points(props.get("points", []))
            self.table.setMinimumWidth(220)
            left.addWidget(self.table, 1)
            brow = QHBoxLayout()
            add = QPushButton("행 추가")
            add.clicked.connect(self.table.add_row)
            rem = QPushButton("선택 삭제")
            rem.clicked.connect(self.table.remove_selected)
            imp = QPushButton("파일에서…")
            imp.setToolTip("2열 텍스트·CSV(x y) 또는 PEER NGA .AT2 지진 기록을 읽어 표를 채운다")
            imp.clicked.connect(self._import)
            exp = QPushButton("CSV 저장")
            exp.clicked.connect(self._export)
            for b in (add, rem, imp, exp):
                brow.addWidget(b)
            left.addLayout(brow)
            srow = QHBoxLayout()
            srow.addWidget(QLabel("값 배율"))
            self.scale = QLineEdit("1")
            self.scale.setToolTip("가져올 때 값(2열)에 곱한다. 예: g 단위 기록을 mm/s² 로 → 9810, m/s² 로 → 9.81")
            self.scale.setMaximumWidth(90)
            srow.addWidget(self.scale)
            srow.addStretch(1)
            left.addLayout(srow)
            body.addLayout(left, 2)
        right = QVBoxLayout()
        self.plot = FunctionPlot()
        right.addWidget(self.plot, 1)
        self.stats = QLabel()
        self.stats.setWordWrap(True)
        right.addWidget(self.stats)
        body.addLayout(right, 3)
        layout.addLayout(body, 1)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(900, 560)
        self._refresh()

    # ---- 그래프·통계
    def _current_points(self) -> list[list[float]]:
        if self.table is not None:
            try:
                return self.table.points()
            except ValueError:
                return []
        expr = (self.expression.text() if self.expression else "").strip()
        if not expr:
            return []
        try:
            tmax = float(self.t_max.text() or "10")
        except ValueError:
            tmax = 10.0
        names = set(re.findall(r"[A-Za-z_]\w*", expr)) & {"x", "y", "z", "t"}
        if len(names) > 1:
            return []
        var = next(iter(names), "t")
        self.plot.xlabel = var
        pts = []
        n = 400
        for i in range(n + 1):
            v = tmax * i / n
            try:
                y = _eval_expr(expr, {var: v})
            except Exception:  # noqa: BLE001
                return []
            pts.append([v, y])
        return pts

    def _refresh(self):
        pts = self._current_points()
        xl, yl = self.AXES.get(self.sub, ("x", "y"))
        if self.sub == "expression" and pts:
            xl = self.plot.xlabel
        self.plot.set_points(pts, xl, yl)
        if pts:
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            k = max(range(len(ys)), key=lambda i: abs(ys[i]))
            self.stats.setText(f"점 {len(pts)}개 · {xl} {min(xs):g} ~ {max(xs):g} · {yl} 최소 {min(ys):g}, 최대 {max(ys):g} · 최대 절댓값 {abs(ys[k]):g} ({xl} = {xs[k]:g})")
        else:
            self.stats.setText("")

    # ---- 가져오기·내보내기
    def _import(self):
        path, _ = QFileDialog.getOpenFileName(self, "표 가져오기", "", "기록·표 (*.txt *.dat *.csv *.at2 *.AT2);;모든 파일 (*)")
        if not path or self.table is None:
            return
        try:
            pts = read_two_columns(path)
            scale = float(self.scale.text() or "1")
        except (ValueError, OSError, StopIteration, AttributeError) as e:
            QMessageBox.warning(self, "가져오기", f"읽지 못했습니다: {e}")
            return
        if not pts:
            QMessageBox.warning(self, "가져오기", "숫자 2열을 찾지 못했습니다")
            return
        self.table.set_points([[a, b * scale] for a, b in pts])
        if not self.name.text().strip() and not self.obj:
            self.name.setText(pathlib.Path(path).stem)
        self._refresh()

    def _export(self):
        if self.table is None:
            return
        path, _ = QFileDialog.getSaveFileName(self, "CSV 저장", (self.name.text().strip() or "function") + ".csv", "CSV (*.csv)")
        if not path:
            return
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(self.AXES.get(self.sub, ("x", "y")))
            w.writerows(self._current_points())

    # ---- 적용(명령 계층)
    def _apply(self):
        try:
            p: dict = {}
            if self.sub == "expression":
                p["expression"] = self.expression.text().strip()
            else:
                p["points"] = self.table.points()
                if self.time_ref is not None:
                    p["time"] = self.time_ref.currentText() or None
            name = self.name.text().strip()
            if self.obj:
                oid = self.obj["id"]
                if name and name != self.obj["name"]:
                    self.app.execute("function.rename", id=oid, name=name)
                self.app.execute("function.update", id=oid, **p)
                self.result_id = oid
            else:
                if name:
                    p["name"] = name
                self.result_id = self.app.execute(f"function.create_{self.sub}", **{k: v for k, v in p.items() if v is not None})["id"]
            self.accept()
        except (Nasa95Error, ValueError, json.JSONDecodeError) as e:
            self.error.setText(f"[{getattr(e, 'code', 'input')}] {e}")


def _eval_expr(expr: str, variables: dict[str, float]) -> float:
    """수식 미리보기용 계산(코어의 수식 계산기와 같은 함수 이름: sin cos tan exp log sqrt abs min max pow pi e)."""
    env = {"sin": math.sin, "cos": math.cos, "tan": math.tan, "exp": math.exp, "log": math.log, "sqrt": math.sqrt, "abs": abs,
           "min": min, "max": max, "pow": math.pow, "pi": math.pi, "e": math.e, "__builtins__": {}}
    env.update(variables)
    if re.search(r"[^0-9A-Za-z_+\-*/^().,\s]", expr):
        raise ValueError("허용되지 않는 문자")
    return float(eval(expr.replace("^", "**"), env))  # noqa: S307 — 문자 집합을 제한한 미리보기 계산
