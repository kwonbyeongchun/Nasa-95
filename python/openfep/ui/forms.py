"""객체 종류 정의(app.kinds)에서 생성·편집 양식을 자동으로 만든다(아키텍처 규칙 9: UI 는 명령 정의에서 나온다).

속성 종류별 입력: number/integer → 스핀, bool → 체크, string(choices) → 콤보, string → 글, vector3 → 글 3개,
ref → 그 종류의 객체 콤보, ref_list → 다중 선택, target → 종류 콤보 + 번호 글, 그 밖(table, object_list, any …) → JSON 글.
"""
from __future__ import annotations

import json
import re
from typing import Any

from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
                               QListWidget, QListWidgetItem, QPushButton, QScrollArea, QVBoxLayout, QWidget)

from ..api import App, OfepError


class FieldEditor:
    """속성 하나의 입력 위젯. value() 는 비어 있으면 None."""

    def __init__(self, app: App, field: dict, current: Any = None, selection: list[dict] | None = None):
        self.field = field
        self.selection = selection or []  # 화면에서 고른 것(형상의 면·요소면·요소)
        self.widget: QWidget
        t = field["type"]
        self.kind = t
        if t == "bool":
            self.widget = QCheckBox()
            self.widget.setChecked(bool(current))
        elif t == "string" and field.get("choices"):
            self.widget = QComboBox()
            self.widget.addItem("")
            for c in field["choices"]:
                self.widget.addItem(c)
            if current:
                self.widget.setCurrentText(str(current))
        elif t == "ref":
            self.widget = QComboBox()
            self.widget.addItem("(없음)", None)
            kind = field.get("ref_kind") or "material"
            for o in _objects(app, kind):
                self.widget.addItem(f"{o['name']} (#{o['id']})", o["id"])
                if current == o["id"]:
                    self.widget.setCurrentIndex(self.widget.count() - 1)
        elif t == "ref_list":
            self.widget = QListWidget()
            self.widget.setSelectionMode(QListWidget.SelectionMode.MultiSelection)
            self.widget.setMaximumHeight(90)
            for o in _objects(app, field.get("ref_kind") or "material"):
                item = QListWidgetItem(f"{o['name']} (#{o['id']})")
                item.setData(32, o["id"])
                self.widget.addItem(item)
                if current and o["id"] in current:
                    item.setSelected(True)
        elif t == "target":
            self.widget = QWidget()
            row = QHBoxLayout(self.widget)
            row.setContentsMargins(0, 0, 0, 0)
            self.target_type = QComboBox()
            for c in field.get("choices") or ["nodes", "elements", "faces", "set", "parts", "geometry"]:
                self.target_type.addItem(c)
            self.target_ids = QLineEdit()
            self.target_ids.setPlaceholderText("번호 목록: 1, 2, 3 / 면: [[1,2],[3,1]] / 형상: [[파트, \"face\", 1]]")
            if current:
                self.target_type.setCurrentText(current.get("type", "nodes"))
                self.target_ids.setText(json.dumps(current.get("ids", []))[1:-1])
            row.addWidget(self.target_type)
            row.addWidget(self.target_ids, 1)
            if self.selection:
                use = QPushButton("선택 사용")
                use.setToolTip("화면에서 고른 면·요소를 대상으로 넣는다")
                use.clicked.connect(self._use_selection)
                row.addWidget(use)
        elif t == "vector3":
            self.widget = QWidget()
            row = QHBoxLayout(self.widget)
            row.setContentsMargins(0, 0, 0, 0)
            self.parts = []
            for k in range(3):
                e = QLineEdit(str(current[k]) if current else "")
                e.setPlaceholderText("xyz"[k])
                self.parts.append(e)
                row.addWidget(e)
        else:
            self.widget = QLineEdit()
            if current is not None:
                self.widget.setText(current if isinstance(current, str) and t == "string" else json.dumps(current, ensure_ascii=False))
            hint = {"number": "실수", "integer": "정수", "string": "글", "number_list": "실수 목록: 1, 2.5", "integer_list": "정수 목록: 1, 2",
                    "string_list": "글 목록: a, b", "table": "표(JSON): [[1, 2], [3, 4]]"}.get(t, "JSON")
            ex = field.get("example")
            self.widget.setPlaceholderText(hint + (f"   예: {json.dumps(ex, ensure_ascii=False)}" if ex is not None else ""))

    def _use_selection(self):
        """화면 선택 → 대상. 형상의 면이면 geometry, 요소면이면 faces, 요소면 elements."""
        kinds = {h["kind"] for h in self.selection}
        allowed = self.field.get("choices") or ["nodes", "elements", "faces", "set", "parts", "geometry"]
        if kinds == {"face"} and "geometry" in allowed:
            self.target_type.setCurrentText("geometry")
            ids = [[h["part"], "face", h["index"]] for h in self.selection]
        elif kinds == {"element_face"} and "faces" in allowed:
            self.target_type.setCurrentText("faces")
            ids = [[h["element"], h["face"]] for h in self.selection]
        elif kinds <= {"element", "element_face"} and "elements" in allowed:
            self.target_type.setCurrentText("elements")
            ids = sorted({h["element"] for h in self.selection})
        else:
            self.target_ids.setPlaceholderText("이 대상 종류에는 선택한 것을 쓸 수 없습니다")
            return
        self.target_ids.setText(json.dumps(ids)[1:-1])

    def value(self):
        t = self.kind
        w = self.widget
        if t == "bool":
            return True if w.isChecked() else None  # 끄면 속성을 지운다(= 기본값 false)
        if t == "string" and self.field.get("choices"):
            return w.currentText() or None
        if t == "ref":
            return w.currentData()
        if t == "ref_list":
            ids = [i.data(32) for i in w.selectedItems()]
            return ids or None
        if t == "target":
            text = self.target_ids.text().strip()
            if not text:
                return None
            return {"type": self.target_type.currentText(), "ids": json.loads(f"[{text}]")}
        if t == "vector3":
            texts = [e.text().strip() for e in self.parts]
            if not any(texts):
                return None
            return [float(x or 0) for x in texts]
        text = w.text().strip()
        if not text:
            return None
        if t == "string":
            return text
        if t in ("number", "integer"):
            return int(text) if t == "integer" else float(text)
        if t in ("number_list", "integer_list", "string_list"):
            items = [s.strip() for s in text.strip("[]").split(",") if s.strip()]
            return [str(s.strip("\"'")) if t == "string_list" else (int(s) if t == "integer_list" else float(s)) for s in items]
        return json.loads(text)


_SOLVER_NAME_BAD = re.compile(r"[^A-Za-z0-9_.\-]")


def solver_name_text(text: str) -> str:
    """솔버 이름 규칙을 쓰는 종류의 이름: 공백을 _ 로 바꾼다."""
    return re.sub(r"\s+", "_", text)


def bind_solver_name(edit: QLineEdit, error: QLabel) -> None:
    """이름 칸에서 공백은 바로 _ 로 바꾸고, 그 밖에 솔버 이름에 쓸 수 없는 문자는 입력하는 동안 알린다."""

    def on_change(text: str) -> None:
        fixed = solver_name_text(text)
        if fixed != text:
            pos = edit.cursorPosition()
            edit.setText(fixed)  # 다시 on_change 가 불린다
            edit.setCursorPosition(min(pos, len(fixed)))
            return
        bad = sorted(set(_SOLVER_NAME_BAD.findall(text)))
        error.setText(f"이름에 쓸 수 없는 문자: {' '.join(bad)}  (영문·숫자·_ - . 만)" if bad else "")

    edit.textChanged.connect(on_change)


def _objects(app: App, kind: str) -> list[dict]:
    try:
        return app.execute(f"{kind}.list")
    except OfepError:
        return []


class ObjectDialog(QDialog):
    """객체 생성(kind, sub) 또는 편집(obj) 양식."""

    def __init__(self, app: App, kind: str, sub: str = "", parent_id: int | None = None, obj: dict | None = None, parent=None,
                 selection: list[dict] | None = None):
        super().__init__(parent)
        self.app, self.kind, self.sub, self.parent_id, self.obj = app, kind, sub, parent_id, obj
        spec = app.kinds()[kind]
        title = f"{spec['label']}" + (f" ({sub})" if sub else "")
        self.setWindowTitle(("편집: " if obj else "새 ") + title)
        fields = list(spec["fields"])
        for s in spec["subtypes"]:
            if s["name"] == (obj["props"].get("type", "") if obj else sub):
                fields += s["fields"]
        self.editors: dict[str, FieldEditor] = {}
        form = QFormLayout()
        self.name = QLineEdit(obj["name"] if obj else "")
        self.name.setPlaceholderText("비우면 자동")
        form.addRow("이름", self.name)
        current = obj["props"] if obj else {}
        for f in fields:
            if f["name"] == "type":
                continue
            ed = FieldEditor(app, f, current.get(f["name"]), selection)
            self.editors[f["name"]] = ed
            label = f["name"] + (" *" if f.get("must") or f.get("required") else "")
            lab = QLabel(label)
            lab.setToolTip(f.get("desc", ""))
            form.addRow(lab, ed.widget)
        body = QWidget()
        body.setLayout(form)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(body)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        if spec.get("solver_name"):
            bind_solver_name(self.name, self.error)
        layout = QVBoxLayout(self)
        layout.addWidget(scroll, 1)
        if kind == "material" and obj:
            layout.addWidget(BehaviorBox(app, obj["id"], self), 2)
        layout.addWidget(self.error)
        layout.addWidget(buttons)
        self.resize(560, min(160 + 34 * (len(fields) + 1) + (260 if kind == "material" and obj else 0), 800))
        self.result_id: int | None = None

    def params(self) -> dict:
        p = {}
        for name, ed in self.editors.items():
            v = ed.value()
            if v is not None:
                p[name] = v
        return p

    def _apply(self):
        try:
            p = self.params()
            if self.obj:
                oid = self.obj["id"]
                if self.name.text().strip() and self.name.text().strip() != self.obj["name"]:
                    self.app.execute(f"{self.kind}.rename", id=oid, name=self.name.text().strip())
                # 비운 속성은 지운다
                for name in self.editors:
                    if name not in p and name in self.obj["props"]:
                        p[name] = None
                if p:
                    self.app.execute(f"{self.kind}.update", id=oid, **p)
                self.result_id = oid
            else:
                if self.name.text().strip():
                    p["name"] = self.name.text().strip()
                if self.parent_id:
                    p["parent"] = self.parent_id
                command = f"{self.kind}.create" + (f"_{self.sub}" if self.sub else "")
                self.result_id = self.app.execute(command, **p)["id"]
            self.accept()
        except (OfepError, ValueError, json.JSONDecodeError) as e:
            code = getattr(e, "code", "input")
            self.error.setText(f"[{code}] {e}")


class CommandDialog(QDialog):
    """명령 하나의 매개변수 양식(명령 정의에서 자동). fixed 는 숨겨서 그대로 넘긴다."""

    def __init__(self, app: App, command: str, fixed: dict | None = None, current: dict | None = None, parent=None,
                 selection: list[dict] | None = None):
        super().__init__(parent)
        self.app, self.command, self.fixed = app, command, dict(fixed or {})
        spec = [c for c in app.commands() if c["name"] == command][0]
        self.setWindowTitle(f"{command} — {spec['desc']}")
        self.editors: dict[str, FieldEditor] = {}
        form = QFormLayout()
        current = current or {}
        for f in spec["params"]:
            if f["name"] in self.fixed:
                continue
            ed = FieldEditor(app, f, current.get(f["name"]), selection)
            self.editors[f["name"]] = ed
            lab = QLabel(f["name"] + (" *" if f.get("must") else ""))
            lab.setToolTip(f.get("desc", ""))
            form.addRow(lab, ed.widget)
        body = QWidget()
        body.setLayout(form)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(body)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(spec["desc"]))
        layout.addWidget(scroll, 1)
        layout.addWidget(self.error)
        layout.addWidget(buttons)
        self.resize(560, min(160 + 34 * (len(self.editors) + 2), 720))
        self.result = None

    def params(self) -> dict:
        p = dict(self.fixed)
        for name, ed in self.editors.items():
            v = ed.value()
            if v is not None:
                p[name] = v
        return p

    def _apply(self):
        try:
            self.result = self.app.execute(self.command, **self.params())
            self.accept()
        except (OfepError, ValueError, json.JSONDecodeError) as e:
            self.error.setText(f"[{getattr(e, 'code', 'input')}] {e}")


class BehaviorBox(QGroupBox):
    """재료의 구성 모델(탄성·밀도·소성 …): 목록 + 추가·편집·제거. material.set_<이름>/remove_<이름> 명령을 쓴다."""

    def __init__(self, app: App, material_id: int, parent=None):
        super().__init__("구성 모델", parent)
        self.app, self.material_id = app, material_id
        self.names = sorted(c["name"][len("material.set_"):] for c in app.commands()
                            if c["name"].startswith("material.set_") and c["name"] != "material.set_orientation")
        self.list = QListWidget()
        self.list.itemDoubleClicked.connect(lambda item: self.edit(item.data(32)))
        self.choice = QComboBox()
        for n in self.names:
            self.choice.addItem(n)
        add = QPushButton("추가/편집")
        add.clicked.connect(lambda: self.edit(self.choice.currentText()))
        remove = QPushButton("제거")
        remove.clicked.connect(self.remove)
        row = QHBoxLayout()
        row.addWidget(self.choice, 1)
        row.addWidget(add)
        row.addWidget(remove)
        layout = QVBoxLayout(self)
        layout.addWidget(self.list)
        layout.addLayout(row)
        self.refresh()

    def current(self) -> dict:
        return self.app.execute("material.get", id=self.material_id)["props"].get("behaviors", {})

    def refresh(self):
        self.list.clear()
        for name, value in self.current().items():
            item = QListWidgetItem(f"{name}: {json.dumps(value, ensure_ascii=False)[:120]}")
            item.setData(32, name)
            self.list.addItem(item)

    def edit(self, name: str):
        dlg = CommandDialog(self.app, f"material.set_{name}", fixed={"id": self.material_id}, current=self.current().get(name), parent=self)
        if dlg.exec():
            self.refresh()

    def remove(self):
        item = self.list.currentItem()
        if item:
            try:
                self.app.execute(f"material.remove_{item.data(32)}", id=self.material_id)
            except OfepError:
                pass
            self.refresh()

class SettingsDialog(QDialog):
    """프로그램 설정(CAS-41): 솔버 실행 파일·스레드 수·작업 폴더. app.settings_get/set 으로 읽고 쓴다(모델이 아니다)."""

    KEYS = [("solver_executable", "솔버 실행 파일(ccx)", "케이스에 지정이 없을 때 쓴다. 비우면 환경 변수 OFEP_CCX"),
            ("threads", "병렬 스레드 수", "OMP_NUM_THREADS. 비우면 솔버 기본값"),
            ("work_directory", "작업 폴더", "비우면 프로젝트 파일 옆 <프로젝트>.work")]

    def __init__(self, app: App, parent=None):
        super().__init__(parent)
        self.app = app
        current = app.execute("app.settings_get")
        self.setWindowTitle("설정")
        form = QFormLayout()
        self.editors: dict[str, QLineEdit] = {}
        for key, label, tip in self.KEYS:
            ed = QLineEdit()
            value = current["values"].get(key)
            ed.setText("" if value is None else str(value))
            ed.setToolTip(tip)
            ed.setPlaceholderText(tip)
            self.editors[key] = ed
            form.addRow(label, ed)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(f"설정 파일: {current['path']}"))
        layout.addLayout(form)
        layout.addWidget(self.error)
        layout.addWidget(buttons)
        self.resize(560, 200)

    def values(self) -> dict:
        out = {}
        for key, ed in self.editors.items():
            text = ed.text().strip()
            if key == "threads":
                out[key] = int(text) if text else None
            else:
                out[key] = text or None
        return out

    def _apply(self):
        try:
            for key, value in self.values().items():
                self.app.execute("app.settings_set", key=key, value=value)
            self.accept()
        except (OfepError, ValueError) as e:
            self.error.setText(f"[{getattr(e, 'code', 'input')}] {e}")
