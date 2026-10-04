"""재료 창(사용자 요청 2026-10-04): 값마다 입력 상자 + 끝에 단위, "DB" 로 내장 재료 DB 에서 골라 바로 추가.

모델 변경은 명령으로만 한다(material.create / set_<구성 모델> / remove_<구성 모델> / library_list / library_import).
단위 기호는 unit.symbols 가 준다(모델 단위계). 직교 이방성·곡선 같은 고급 입력은 기존 구성 모델 표 창(ObjectDialog)으로 간다.
"""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QAbstractItemView, QDialog, QDialogButtonBox, QFormLayout, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
                               QPushButton, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from ..api import App, OfepError

# (키, 라벨, 단위 차원, 구성 모델, 설명)
FIELDS = [
    ("E", "탄성계수 E", "pressure", "elastic", "영률"),
    ("nu", "포아송비 ν", "none", "elastic", "−1 < ν < 0.5"),
    ("density", "밀도 ρ", "density", "density", "동해석·자중·열해석에 필요"),
    ("expansion", "열팽창계수 α", "thermal_expansion", "expansion", "열응력 해석"),
    ("conductivity", "열전도율 k", "conductivity", "conductivity", "열해석"),
    ("specific_heat", "비열 c", "specific_heat", "specific_heat", "과도 열해석"),
    ("yield", "항복 응력 σy", "pressure", "plastic", "비우면 탄성만"),
    ("ultimate", "인장 강도 σu", "pressure", "plastic", "항복 뒤 경화 끝점(선택)"),
    ("eps_u", "σu 에서의 소성 변형률", "none", "plastic", "기본 0.1"),
]


def fmt(v) -> str:
    if v is None:
        return ""
    v = float(v)
    return f"{v:.6g}"


class UnitEdit(QWidget):
    """숫자 입력 상자 + 오른쪽 단위 글."""

    def __init__(self, unit: str, tip: str = "", parent=None):
        super().__init__(parent)
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)
        self.edit = QLineEdit()
        self.edit.setAlignment(Qt.AlignmentFlag.AlignRight)
        self.edit.setPlaceholderText("비움")
        self.edit.setToolTip(tip)
        self.unit = QLabel(unit)
        self.unit.setProperty("role", "muted")
        self.unit.setMinimumWidth(72)
        row.addWidget(self.edit, 1)
        row.addWidget(self.unit)

    def value(self) -> float | None:
        text = self.edit.text().strip().replace(",", "")
        if not text:
            return None
        return float(text)

    def set_value(self, v) -> None:
        self.edit.setText(fmt(v))


class MaterialDbDialog(QDialog):
    """내장 재료 DB 목록(모델 단위계로 환산된 대표값). 고른 재료를 바로 추가한다."""

    def __init__(self, app: App, parent=None, path: str | None = None):
        super().__init__(parent)
        self.app, self.path = app, path
        self.setWindowTitle("재료 DB")
        self.resize(900, 460)
        self.chosen: list[str] = []
        try:
            self.rows = app.execute("material.library_list", **({"path": path} if path else {}))
            self.symbols = app.execute("unit.symbols")
        except OfepError as e:
            self.rows, self.symbols = [], {}
            QLabel(str(e), self)
        layout = QVBoxLayout(self)
        self.filter = QLineEdit()
        self.filter.setPlaceholderText("이름·분류·설명으로 거르기")
        self.filter.textChanged.connect(self._fill)
        layout.addWidget(self.filter)
        self.table = QTableWidget(0, 7)
        self.table.setHorizontalHeaderLabels(["재료", "분류", f"E [{self.symbols.get('pressure', '')}]", "ν", f"ρ [{self.symbols.get('density', '')}]",
                                              f"σy [{self.symbols.get('pressure', '')}]", "설명"])
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.verticalHeader().hide()
        self.table.horizontalHeader().setSectionResizeMode(6, QHeaderView.ResizeMode.Stretch)
        self.table.itemDoubleClicked.connect(lambda _i: self._accept())
        layout.addWidget(self.table, 1)
        hint = QLabel(f"값은 모델 단위계({self.symbols.get('system', '')})로 환산된 대표값입니다. 설계 검토에는 규격·시험 성적서 값을 쓰세요.")
        hint.setProperty("role", "muted")
        hint.setWordWrap(True)
        layout.addWidget(hint)
        buttons = QDialogButtonBox()
        self.add_button = buttons.addButton("선택한 재료 추가", QDialogButtonBox.ButtonRole.AcceptRole)
        buttons.addButton(QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._fill()

    def _fill(self):
        text = self.filter.text().strip().casefold()
        self.table.setRowCount(0)
        for r in self.rows:
            hay = f"{r['name']} {r.get('group', '')} {r.get('note', '')}".casefold()
            if text and text not in hay:
                continue
            i = self.table.rowCount()
            self.table.insertRow(i)
            cells = [r["name"], r.get("group", ""), fmt(r.get("E")), fmt(r.get("nu")), fmt(r.get("density")), fmt(r.get("yield")), r.get("note", "")]
            for col, val in enumerate(cells):
                item = QTableWidgetItem(val)
                if 2 <= col <= 5:
                    item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                item.setData(Qt.ItemDataRole.UserRole, r["name"])
                self.table.setItem(i, col, item)
        self.table.resizeColumnsToContents()
        self.table.horizontalHeader().setSectionResizeMode(6, QHeaderView.ResizeMode.Stretch)

    def selected_names(self) -> list[str]:
        return sorted({self.table.item(i.row(), 0).data(Qt.ItemDataRole.UserRole) for i in self.table.selectedIndexes()})

    def _accept(self):
        self.chosen = self.selected_names()
        if self.chosen:
            self.accept()


def add_from_db(app: App, parent=None, path: str | None = None) -> list[int]:
    """DB 창을 띄워 고른 재료를 바로 만든다. 만든 재료 ID 목록."""
    dlg = MaterialDbDialog(app, parent, path)
    if not dlg.exec() or not dlg.chosen:
        return []
    r = app.execute("material.library_import", names=dlg.chosen, **({"path": path} if path else {}))
    return [c["id"] for c in r["created"]]


class MaterialDialog(QDialog):
    """재료 만들기·편집: 이름·설명 + 값 입력 상자(단위 표시) + DB 버튼 + 고급(구성 모델 표)."""

    def __init__(self, app: App, obj: dict | None = None, parent=None):
        super().__init__(parent)
        self.app, self.obj = app, obj
        self.result_id: int | None = obj["id"] if obj else None
        self.setWindowTitle("편집: 재료" if obj else "새 재료")
        try:
            self.symbols = app.execute("unit.symbols")
        except OfepError:
            self.symbols = {}
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.name = QLineEdit(obj["name"] if obj else "")
        self.name.setPlaceholderText("비우면 자동")
        self.description = QLineEdit((obj or {}).get("props", {}).get("description", "") if obj else "")
        form.addRow("이름", self.name)
        form.addRow("설명", self.description)
        self.edits: dict[str, UnitEdit] = {}
        for key, label, dim, _behavior, tip in FIELDS:
            ed = UnitEdit(self.symbols.get(dim, ""), tip)
            ed.edit.setAccessibleName(f"재료 {key}")
            self.edits[key] = ed
            form.addRow(label, ed)
        layout.addLayout(form)
        row = QHBoxLayout()
        db = QPushButton("DB…")
        db.setToolTip("내장 재료 DB 에서 고른다. 고른 값이 상자에 채워지고(여러 개를 고르면 바로 추가)")
        db.clicked.connect(self._from_db)
        advanced = QPushButton("고급(구성 모델 표)…")
        advanced.setToolTip("직교 이방성·온도 의존 표·초탄성 등은 구성 모델 표로 입력")
        advanced.clicked.connect(self._advanced)
        row.addWidget(db)
        row.addWidget(advanced)
        row.addStretch(1)
        layout.addLayout(row)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        from .forms import bind_solver_name
        bind_solver_name(self.name, self.error)  # DB 에서 채운 이름(공백 있음)도 여기서 바뀐다
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(520, 480)
        if obj:
            self._load(obj["props"].get("behaviors", {}))

    # ---- 값 ↔ 구성 모델
    def _load(self, behaviors: dict) -> None:
        el = behaviors.get("elastic")
        if el and el.get("type", "iso") == "iso" and el.get("data"):
            self.edits["E"].set_value(el["data"][0][0])
            self.edits["nu"].set_value(el["data"][0][1])
        for key, name in (("density", "density"), ("specific_heat", "specific_heat")):
            b = behaviors.get(name)
            if b and b.get("data"):
                self.edits[key].set_value(b["data"][0][0])
        for key, name in (("expansion", "expansion"), ("conductivity", "conductivity")):
            b = behaviors.get(name)
            if b and b.get("type", "iso") == "iso" and b.get("data"):
                self.edits[key].set_value(b["data"][0][0])
        pl = behaviors.get("plastic")
        if pl and pl.get("hardening", "isotropic") == "isotropic" and pl.get("data"):
            self.edits["yield"].set_value(pl["data"][0][0])
            if len(pl["data"]) > 1:
                self.edits["ultimate"].set_value(pl["data"][-1][0])
                self.edits["eps_u"].set_value(pl["data"][-1][1])

    def values(self) -> dict[str, float | None]:
        return {k: ed.value() for k, ed in self.edits.items()}

    def _apply(self, close: bool = True):
        try:
            v = self.values()
            if v["E"] is None and v["nu"] is not None or v["E"] is not None and v["nu"] is None:
                raise ValueError("탄성계수와 포아송비는 함께 넣습니다")
            if v["yield"] is not None and v["E"] is None:
                raise ValueError("소성(항복 응력)은 탄성 값이 있어야 합니다")
            if self.obj:
                oid = self.obj["id"]
                name = self.name.text().strip()
                if name and name != self.obj["name"]:
                    self.app.execute("material.rename", id=oid, name=name)
                self.app.execute("material.update", id=oid, description=self.description.text().strip() or None)
            else:
                p = {}
                if self.name.text().strip():
                    p["name"] = self.name.text().strip()
                if self.description.text().strip():
                    p["description"] = self.description.text().strip()
                oid = self.app.execute("material.create", **p)["id"]
            had = set((self.obj or {}).get("props", {}).get("behaviors", {})) if self.obj else set()

            def put(name, present, **params):
                if present:
                    self.app.execute(f"material.set_{name}", id=oid, **params)
                elif name in had:
                    self.app.execute(f"material.remove_{name}", id=oid)

            put("elastic", v["E"] is not None, type="iso", data=[[v["E"], v["nu"]]])
            put("density", v["density"] is not None, data=[[v["density"]]])
            put("expansion", v["expansion"] is not None, type="iso", data=[[v["expansion"]]])
            put("conductivity", v["conductivity"] is not None, type="iso", data=[[v["conductivity"]]])
            put("specific_heat", v["specific_heat"] is not None, data=[[v["specific_heat"]]])
            if v["yield"] is not None:
                curve = [[v["yield"], 0.0]]
                if v["ultimate"] is not None:
                    curve.append([v["ultimate"], v["eps_u"] if v["eps_u"] is not None else 0.1])
                self.app.execute("material.set_plastic", id=oid, hardening="isotropic", data=curve)
            elif "plastic" in had:
                self.app.execute("material.remove_plastic", id=oid)
            self.result_id = oid
            if close:
                self.accept()
        except (OfepError, ValueError) as e:
            self.error.setText(f"[{getattr(e, 'code', 'input')}] {e}")

    # ---- DB·고급
    def _from_db(self):
        dlg = MaterialDbDialog(self.app, self)
        if not dlg.exec() or not dlg.chosen:
            return
        if len(dlg.chosen) == 1 and not self.obj:
            # 하나 골랐으면 값을 상자에 채운다(이름도). 확인을 누르면 그 값으로 만든다
            name = dlg.chosen[0]
            row = next(r for r in dlg.rows if r["name"] == name)
            # 전체 구성 모델은 가져오기로 받는 것이 정확하다: 임시로 가져와 값을 읽고 되돌린다
            r = self.app.execute("material.library_import", names=[name])
            props = self.app.execute("material.get", id=r["created"][0]["id"])["props"]
            self.app.undo()
            self.name.setText(name)
            self.description.setText(row.get("note", ""))
            self._load(props.get("behaviors", {}))
            return
        ids = [c["id"] for c in self.app.execute("material.library_import", names=dlg.chosen)["created"]]
        self.result_id = ids[0]
        self.accept()

    def _advanced(self):
        from .forms import ObjectDialog
        if not self.obj:
            # 먼저 지금 값으로 만들고 표 창으로 간다
            self._apply(close=False)
            if self.result_id is None:
                return
            self.obj = self.app.execute("material.get", id=self.result_id)
        dlg = ObjectDialog(self.app, "material", obj=self.app.execute("material.get", id=self.obj["id"]), parent=self)
        dlg.exec()
        self.obj = self.app.execute("material.get", id=self.obj["id"])
        self._load(self.obj["props"].get("behaviors", {}))
