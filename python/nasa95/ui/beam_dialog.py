"""보 프로퍼티 창(D15, 2026-10-05): 단면 종류(직사각형·타원·파이프·박스·형강 I·T·L·C·일반) + 치수 입력 상자(단위 표시) + 단면 미리보기 +
1축 방향(전역 축 고르기 또는 벡터) + 오프셋 + 재료·대상.

모델 변경은 명령으로만 한다(property.create_beam / property.update). 단면 모양은 property.section_shape 가 준다(형강은 합성보 분해).
"""
from __future__ import annotations

import json

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QBrush, QColor, QPainter, QPen
from PySide6.QtWidgets import (QAbstractItemView, QComboBox, QDialog, QDialogButtonBox, QFormLayout, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
                               QPushButton, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from ..api import App, Nasa95Error
from .material_dialog import UnitEdit, fmt

# 단면 종류 → (표시 이름, 치수 (키, 라벨, 설명) 목록)
SECTIONS = {
    "rect": ("직사각형", [("a", "1축 두께 a", ""), ("b", "2축 두께 b", "")]),
    "circ": ("원형·타원", [("d1", "1축 지름", "원이면 둘을 같게"), ("d2", "2축 지름", "")]),
    "pipe": ("파이프", [("r", "바깥 반지름", ""), ("t", "두께", "")]),
    "box": ("박스", [("a", "1축 바깥 치수 a", ""), ("b", "2축 바깥 치수 b", ""), ("t1", "+1축 벽 두께 t1", ""), ("t2", "+2축 벽 두께 t2", ""),
                    ("t3", "−1축 벽 두께 t3", ""), ("t4", "−2축 벽 두께 t4", "")]),
    "I": ("I·H 형강", [("h", "높이 h (1축)", "플랜지 바깥 사이"), ("b", "폭 b (2축)", ""), ("tw", "웨브 두께 tw", ""), ("tf", "플랜지 두께 tf", "")]),
    "T": ("T 형강", [("h", "높이 h (1축)", "플랜지가 +1축 쪽"), ("b", "폭 b (2축)", ""), ("tw", "웨브 두께 tw", ""), ("tf", "플랜지 두께 tf", "")]),
    "L": ("L 형강(앵글)", [("h", "세로 다리 h (1축)", "세로 다리는 −2축 쪽"), ("b", "가로 다리 b (2축)", "가로 다리는 −1축 쪽"), ("tw", "세로 다리 두께", ""), ("tf", "가로 다리 두께", "")]),
    "C": ("C·ㄷ 형강(채널)", [("h", "높이 h (1축)", "웨브는 −2축 쪽"), ("b", "폭 b (2축)", ""), ("tw", "웨브 두께 tw", ""), ("tf", "플랜지 두께 tf", "")]),
    "general": ("일반(A·I)", [("A", "단면적 A", "사용자 요소 U1 전용"), ("I11", "I11", ""), ("I12", "I12", "U1 은 0"), ("I22", "I22", ""), ("k", "전단 계수 k", "Timoshenko")]),
}
AXES = [("+Z", [0.0, 0.0, 1.0]), ("−Z", [0.0, 0.0, -1.0]), ("+X", [1.0, 0.0, 0.0]), ("−X", [-1.0, 0.0, 0.0]), ("+Y", [0.0, 1.0, 0.0]), ("−Y", [0.0, -1.0, 0.0])]


class SectionPreview(QWidget):
    """단면 그림: 부분 직사각형을 1축(위)·2축(오른쪽) 좌표로 그린다. 기준선(+)과 1축 화살표 표시."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.shape: dict | None = None
        self.offsets = (0.0, 0.0)
        self.setMinimumSize(200, 200)

    def set_shape(self, shape: dict | None, offsets=(0.0, 0.0)) -> None:
        self.shape, self.offsets = shape, offsets
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        p.fillRect(0, 0, w, h, self.palette().base())
        if not self.shape or not self.shape.get("rects"):
            p.setPen(self.palette().text().color())
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "치수를 입력하면 단면이 보입니다")
            return
        H, B = self.shape["extent"]
        if H <= 0 or B <= 0:
            return
        # 오프셋: 축 = 기준선 − o × 크기 → 단면 중심이 기준선에서 (−o1·H, −o2·B) 에 있다. 기준선을 화면 가운데에 둔다
        o1, o2 = self.offsets
        shift1, shift2 = -o1 * H, -o2 * B
        span1, span2 = H + 2 * abs(shift1), B + 2 * abs(shift2)
        scale = 0.8 * min(w / span2, h / span1)
        cx, cy = w / 2, h / 2
        p.setPen(QPen(QColor(90, 140, 200), 1))
        p.setBrush(QBrush(QColor(122, 162, 204)))
        for r in self.shape["rects"]:
            c1, c2 = r["c1"] + shift1, r["c2"] + shift2
            x = cx + (c2 - r["t2"] / 2) * scale
            y = cy - (c1 + r["t1"] / 2) * scale
            p.drawRect(QRectF(x, y, r["t2"] * scale, r["t1"] * scale))
        # 기준선(보 요소 선이 지나는 곳)
        p.setPen(QPen(QColor(220, 80, 60), 2))
        p.drawLine(int(cx - 6), int(cy), int(cx + 6), int(cy))
        p.drawLine(int(cx), int(cy - 6), int(cx), int(cy + 6))
        # 1축 화살표(위), 2축(오른쪽)
        p.setPen(QPen(QColor(60, 160, 60), 2))
        p.drawLine(int(cx), int(cy), int(cx), int(cy - 0.45 * h))
        p.drawText(int(cx + 4), int(cy - 0.45 * h + 12), "1")
        p.setPen(QPen(QColor(60, 100, 200), 2))
        p.drawLine(int(cx), int(cy), int(cx + 0.45 * w), int(cy))
        p.drawText(int(cx + 0.45 * w - 10), int(cy - 4), "2")


class SectionDbDialog(QDialog):
    """표준 형강 목록(KS·EN): 규격·계열·이름으로 거르고 하나를 고른다. 치수는 모델 단위계."""

    def __init__(self, app: App, parent=None):
        super().__init__(parent)
        self.app = app
        self.setWindowTitle("표준 형강 목록")
        self.resize(760, 460)
        self.chosen: dict | None = None
        try:
            self.rows = app.execute("property.section_library_list")
            unit = app.execute("unit.symbols").get("length", "")
        except Nasa95Error as e:
            self.rows, unit = [], ""
            QLabel(str(e), self)
        layout = QVBoxLayout(self)
        top = QHBoxLayout()
        self.standard = QComboBox()
        self.standard.addItems(["전체", "KS", "EN"])
        self.series = QComboBox()
        self.series.addItem("전체 계열")
        for key in sorted({r["series"] for r in self.rows}, key=lambda k: ("HCLSRIUX".find(k[0]), k)):
            self.series.addItem(key)
        self.filter = QLineEdit()
        self.filter.setPlaceholderText("이름으로 거르기 (예: 400x200, IPE 2)")
        for w in (self.standard, self.series):
            w.currentIndexChanged.connect(self._fill)
        self.filter.textChanged.connect(self._fill)
        top.addWidget(self.standard)
        top.addWidget(self.series)
        top.addWidget(self.filter, 1)
        layout.addLayout(top)
        self.table = QTableWidget(0, 6)
        self.table.setHorizontalHeaderLabels(["이름", "규격", "계열", "종류", f"치수 ({unit})", "비고"])
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.horizontalHeader().setSectionResizeMode(4, QHeaderView.ResizeMode.Stretch)
        self.table.doubleClicked.connect(lambda _i: self._accept())
        layout.addWidget(self.table, 1)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._fill()

    def _fill(self):
        std, series, text = self.standard.currentText(), self.series.currentText(), self.filter.text().strip().lower()
        self.table.setRowCount(0)
        for r in self.rows:
            if std != "전체" and r["standard"] != std:
                continue
            if series != "전체 계열" and r["series"] != series:
                continue
            if text and text not in r["name"].lower():
                continue
            row = self.table.rowCount()
            self.table.insertRow(row)
            sec = r["section"]
            if sec in ("I", "C", "L"):
                dims = f"h {fmt(r['h'])} · b {fmt(r['b'])} · tw {fmt(r['tw'])} · tf {fmt(r['tf'])}"
            elif sec == "box":
                dims = f"a {fmt(r['a'])} · b {fmt(r['b'])} · t {fmt(r['t'])}"
            else:
                dims = f"D {fmt(r['D'])} · t {fmt(r['t'])}"
            note = {"I": "합성보(직사각형 3개)", "C": "합성보(직사각형 3개)", "L": "합성보(직사각형 2개)", "box": "CalculiX BOX(얇은 벽)", "pipe": "CalculiX PIPE(얇은 벽)"}[sec]
            for col, cell in enumerate((r["name"], r["standard"], r["series"], SECTIONS[sec][0], dims, note)):
                item = QTableWidgetItem(cell)
                if col == 0:
                    item.setData(Qt.ItemDataRole.UserRole, r)
                self.table.setItem(row, col, item)

    def selected(self) -> dict | None:
        items = self.table.selectedItems()
        if not items:
            return None
        return self.table.item(items[0].row(), 0).data(Qt.ItemDataRole.UserRole)

    def _accept(self):
        self.chosen = self.selected()
        if self.chosen is None:
            return
        self.accept()


class BeamDialog(QDialog):
    """보 프로퍼티 만들기·편집."""

    def __init__(self, app: App, obj: dict | None = None, parent=None, selection: list[dict] | None = None):
        super().__init__(parent)
        self.app, self.obj = app, obj
        self.result_id: int | None = obj["id"] if obj else None
        self.setWindowTitle("편집: 보 프로퍼티" if obj else "새 보 프로퍼티")
        try:
            self.symbols = app.execute("unit.symbols")
        except Nasa95Error:
            self.symbols = {}
        props = (obj or {}).get("props", {})
        layout = QVBoxLayout(self)
        body = QHBoxLayout()
        form = QFormLayout()
        self.name = QLineEdit(obj["name"] if obj else "")
        self.name.setPlaceholderText("비우면 자동")
        form.addRow("이름", self.name)
        self.material = QComboBox()
        self.material.addItem("(없음)", None)
        for m in app.execute("material.list"):
            self.material.addItem(f"{m['name']} (#{m['id']})", m["id"])
            if props.get("material") == m["id"]:
                self.material.setCurrentIndex(self.material.count() - 1)
        form.addRow("재료", self.material)
        self.section = QComboBox()
        for key, (label, _) in SECTIONS.items():
            self.section.addItem(label, key)
        self.section.setCurrentIndex(max(0, list(SECTIONS).index(props.get("section", "rect"))))
        self.section.currentIndexChanged.connect(self._section_changed)
        srow = QHBoxLayout()
        srow.addWidget(self.section, 1)
        db = QPushButton("표준 형강 DB…")
        db.setToolTip("KS·EN 표준 형강(H·ㄷ·ㄱ·각형강관·강관, IPE·HEA·HEB·UPN·CHS·SHS·RHS)에서 골라 종류·치수·이름을 채운다")
        db.clicked.connect(self._from_db)
        srow.addWidget(db)
        form.addRow("단면 종류", srow)
        self.dim_rows: list[tuple[QLabel, UnitEdit]] = []
        length = self.symbols.get("length", "")
        for _ in range(6):
            label = QLabel()
            ed = UnitEdit(length)
            ed.edit.textChanged.connect(self._refresh_preview)
            self.dim_rows.append((label, ed))
            form.addRow(label, ed)
        # 1축 방향: 전역 축 고르기 또는 벡터
        self.direction = QComboBox()
        for label, _vec in AXES:
            self.direction.addItem(label)
        self.direction.addItem("직접 입력")
        self.direction_edit = QLineEdit()
        self.direction_edit.setPlaceholderText("x, y, z")
        self.direction.currentIndexChanged.connect(lambda i: self.direction_edit.setEnabled(i == len(AXES)))
        drow = QHBoxLayout()
        drow.addWidget(self.direction)
        drow.addWidget(self.direction_edit, 1)
        form.addRow("1축 방향", drow)
        self.direction.setToolTip("단면 1축(형강은 웨브·높이 방향)이 가리키는 전역 방향. 보 축에 수직인 성분만 쓴다. 솔버 기본은 −Z")
        self.offset1 = UnitEdit("", "기준선에서 단면 축까지(두께 단위, 형강은 h 단위). 0.5 = 기준선이 +1축 쪽 표면")
        self.offset2 = UnitEdit("", "기준선에서 단면 축까지(두께 단위, 형강은 b 단위)")
        self.offset1.edit.textChanged.connect(self._refresh_preview)
        self.offset2.edit.textChanged.connect(self._refresh_preview)
        form.addRow("오프셋 1축", self.offset1)
        form.addRow("오프셋 2축", self.offset2)
        from .forms import FieldEditor
        spec = app.kinds()["property"]
        target_field = next(f for f in spec["fields"] if f["name"] == "target")
        self.target = FieldEditor(app, target_field, props.get("target"), selection)
        form.addRow("대상(요소)", self.target.widget)
        body.addLayout(form, 1)
        self.preview = SectionPreview()
        side = QVBoxLayout()
        side.addWidget(QLabel("단면 미리보기 (위 = 1축, 오른쪽 = 2축, + = 기준선)"))
        side.addWidget(self.preview, 1)
        self.values_label = QLabel()
        self.values_label.setWordWrap(True)
        self.values_label.setProperty("role", "muted")
        side.addWidget(self.values_label)
        body.addLayout(side, 1)
        layout.addLayout(body)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        from .forms import bind_solver_name
        bind_solver_name(self.name, self.error)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(820, 520)
        self._section_changed()
        if props:
            self._load(props)
        self._refresh_preview()

    # ---- 단면 종류에 따라 치수 상자 보이기
    def _section_changed(self):
        key = self.section.currentData()
        fields = SECTIONS[key][1]
        unit = self.symbols.get("length", "")
        for i, (label, ed) in enumerate(self.dim_rows):
            if i < len(fields):
                k, text, tip = fields[i]
                label.setText(text)
                ed.edit.setToolTip(tip)
                ed.unit.setText("" if key == "general" else unit)
                label.show(), ed.show()
            else:
                label.hide(), ed.hide()
        self._refresh_preview()

    def _load(self, props: dict) -> None:
        dims = props.get("dimensions", [])
        for i, (_label, ed) in enumerate(self.dim_rows):
            if i < len(dims):
                ed.set_value(dims[i])
        d = props.get("direction")
        if d is not None:
            for i, (_label, vec) in enumerate(AXES):
                if [float(x) for x in d] == vec:
                    self.direction.setCurrentIndex(i)
                    break
            else:
                self.direction.setCurrentIndex(len(AXES))
                self.direction_edit.setText(", ".join(fmt(x) for x in d))
        if props.get("offset1") is not None:
            self.offset1.set_value(props["offset1"])
        if props.get("offset2") is not None:
            self.offset2.set_value(props["offset2"])

    def _from_db(self):
        dlg = SectionDbDialog(self.app, parent=self)
        if dlg.exec() and dlg.chosen:
            self.apply_library_row(dlg.chosen)

    def apply_library_row(self, row: dict) -> None:
        """표준 형강 한 줄로 종류·치수·이름을 채운다."""
        self.section.setCurrentIndex(list(SECTIONS).index(row["section"]))
        for i, (_label, ed) in enumerate(self.dim_rows):
            if i < len(row["dimensions"]):
                ed.set_value(row["dimensions"][i])
        if not self.name.text().strip() or self.name.text().strip() == getattr(self, "_db_name", None):
            self._db_name = row["name"].replace(" ", "_")
            self.name.setText(self._db_name)
        self._refresh_preview()

    def dimensions(self) -> list[float] | None:
        key = self.section.currentData()
        out = []
        for i in range(len(SECTIONS[key][1])):
            v = self.dim_rows[i][1].value()
            if v is None:
                return None
            out.append(v)
        return out

    def direction_vector(self) -> list[float] | None:
        i = self.direction.currentIndex()
        if i < len(AXES):
            return AXES[i][1]
        text = self.direction_edit.text().strip()
        if not text:
            return None
        return [float(x) for x in text.replace(";", ",").split(",")]

    def _refresh_preview(self):
        key = self.section.currentData()
        dims = self.dimensions()
        if dims is None or key == "general":
            self.preview.set_shape(None)
            self.values_label.setText("")
            return
        try:
            shape = self.app.execute("property.section_shape", section=key, dimensions=dims)
        except Nasa95Error as e:
            self.preview.set_shape(None)
            self.values_label.setText(str(e))
            return
        self.preview.set_shape(shape, (self.offset1.value() or 0.0, self.offset2.value() or 0.0))
        area = sum(r["t1"] * r["t2"] for r in shape["rects"])
        note = "합성보(직사각형 부분 %d개)로 덱에 나간다" % len(shape["rects"]) if shape["composite"] else ""
        self.values_label.setText(f"외접 상자 {fmt(shape['extent'][0])} × {fmt(shape['extent'][1])}, 면적 ≈ {fmt(area)}. {note}")

    def _apply(self):
        key = self.section.currentData()
        dims = self.dimensions()
        if dims is None:
            self.error.setText("치수를 모두 입력하세요")
            return
        params = {"section": key, "dimensions": dims}
        name = self.name.text().strip()
        if name and not self.obj:
            params["name"] = name
        if self.material.currentData() is not None:
            params["material"] = self.material.currentData()
        try:
            d = self.direction_vector()
            if d is not None:
                params["direction"] = d
            for k, ed in (("offset1", self.offset1), ("offset2", self.offset2)):
                if ed.value() is not None:
                    params[k] = ed.value()
            t = self.target.value()
            if t is not None:
                params["target"] = t
            if self.obj:
                self.app.execute("property.update", id=self.obj["id"], **params)
                if name and name != self.obj["name"]:
                    self.app.execute("property.rename", id=self.obj["id"], name=name)
            else:
                self.result_id = self.app.execute("property.create_beam", **params)["id"]
        except (Nasa95Error, ValueError, json.JSONDecodeError) as e:
            self.error.setText(str(e))
            return
        self.accept()
