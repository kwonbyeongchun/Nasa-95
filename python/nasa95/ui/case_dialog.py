"""좌우 트리로 케이스 공통 메시와 스텝별 하중·구속 참조를 구성한다."""
from __future__ import annotations
import json
import math
from PySide6.QtCore import Qt
from PySide6.QtGui import QPalette
from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFormLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QScrollArea, QSplitter, QTreeWidgetItem, QVBoxLayout, QWidget)
from ..api import App, Nasa95Error
from .case_tree import CaseTree, KEY, STEP
from .icons import icon
from .forms import FieldEditor

PROCEDURES = [  # (스텝 종류, 표시 이름) — 자주 쓰는 순서. 그 밖의 종류는 뒤에 이름 그대로
    ("static", "정적(static)"), ("frequency", "고유진동수(frequency)"), ("buckle", "좌굴(buckle)"), ("dynamic", "동적(dynamic)"),
    ("modal_dynamic", "모드 중첩 동해석(modal_dynamic)"), ("steady_state_dynamics", "정상상태 동해석(steady_state_dynamics)"),
    ("heat_transfer", "열전달(heat_transfer)"), ("coupled_temperature_displacement", "열-구조 연성(coupled)"),
    ("uncoupled_temperature_displacement", "열-구조 순차(uncoupled)"), ("visco", "점탄소성(visco)"),
]
DEFAULT_OUTPUTS = {"static": (["U", "RF"], ["S", "E"]), "frequency": (["U"], []), "buckle": (["U"], []), "dynamic": (["U", "V", "A"], ["S"]),
                   "modal_dynamic": (["U"], ["S"]), "steady_state_dynamics": (["U"], ["S"]), "heat_transfer": (["NT"], ["HFL"]),
                   "coupled_temperature_displacement": (["U", "NT"], ["S"]), "uncoupled_temperature_displacement": (["U", "NT"], ["S"]), "visco": (["U"], ["S", "PEEQ"])}

LABELS = {"mesh_part": "메시", "load_set": "하중 셋", "bc_set": "구속 셋"}
SYMBOLS = {"mesh_part": "mesh", "load_set": "load", "bc_set": "bc"}


class CaseDialog(QDialog):
    """추가·제외는 임시 트리만 변경하고 저장할 때 하나의 트랜잭션으로 적용한다."""

    def __init__(self, app: App, parent=None, step: dict | None = None, case: dict | None = None):
        super().__init__(parent)
        self.app, self.step = app, step
        self.result_id = None
        self.run_requested = False
        self.validation_issues = []
        self._entries, self._source_items, self._selected_items = {}, {}, {}
        self._custom_scope = None
        self._case = app.execute("case.get", id=step["parent"]) if step else case
        self._steps = ([app.execute("step.get", id=s["id"]) for s in app.execute("step.list", parent=self._case["id"])]
                       if self._case else [])
        self._adding_step = not self._steps
        if self._adding_step:
            self._steps = [{"id": 0, "name": "스텝 1", "props": {"type": "static"}}]
        self.setWindowTitle(f"편집: 해석 케이스 — {self._case['name']}" if self._case else "새 해석 케이스")
        self.resize(1040, 700)
        self.setMinimumSize(820, 540)
        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        form = QFormLayout()
        self.name = QLineEdit(self._case["name"] if self._case else "")
        self.name.setPlaceholderText("케이스 이름 (비우면 자동)")
        form.addRow("케이스 이름", self.name)
        self.procedure = QComboBox()
        for n, label in PROCEDURES:
            self.procedure.addItem(label, n)
        known = {n for n, _ in PROCEDURES}
        for spec in app.kinds()["step"]["subtypes"]:
            if spec["name"] not in known:
                self.procedure.addItem(spec["name"], spec["name"])
        self.nlgeom = QCheckBox("기하 비선형 (큰 변형)")
        if self._adding_step:
            row = QHBoxLayout()
            row.addWidget(self.procedure, 1)
            row.addWidget(self.nlgeom)
            form.addRow("해석 종류", row)
        self.step_selector = QComboBox()
        for s in self._steps:
            self.step_selector.addItem(f"{s['name']} · {s['props']['type']}", s["id"])
        if step:
            self.step_selector.setCurrentIndex(self.step_selector.findData(step["id"]))
        form.addRow("하중·구속 추가 대상", self.step_selector)
        layout.addLayout(form)
        hint = QLabel("왼쪽 항목을 오른쪽에 추가하세요. 메시 범위는 케이스 공통이며 하중·구속은 선택한 스텝에 포함됩니다.")
        hint.setWordWrap(True)
        layout.addWidget(hint)
        self.available_tree, self.included_tree = CaseTree(), CaseTree(included=True)
        self.available_tree.setAccessibleName("사용 가능한 메시·하중·구속")
        self.included_tree.setAccessibleName("이번 해석에 포함할 메시·하중·구속")
        self.available_tree.peer, self.included_tree.peer = self.included_tree, self.available_tree
        self.available_search, self.included_search = QLineEdit(), QLineEdit()
        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(self._pane("현재 프로젝트 · 사용 가능한 항목", self.available_search, self.available_tree))
        arrows = QWidget()
        arrow_layout = QVBoxLayout(arrows)
        arrow_layout.setContentsMargins(4, 0, 4, 0)
        arrow_layout.addStretch()
        self.add_button, self.remove_button = QPushButton("추가 →"), QPushButton("← 제외")
        for button in (self.add_button, self.remove_button):
            button.setAutoDefault(False)
            arrow_layout.addWidget(button)
        arrow_layout.addStretch()
        arrows.setFixedWidth(92)
        split.addWidget(arrows)
        split.addWidget(self._pane("이번 해석에 포함", self.included_search, self.included_tree))
        split.setChildrenCollapsible(False)
        split.setStretchFactor(0, 1)
        split.setStretchFactor(2, 1)
        layout.addWidget(split, 1)
        self.summary = QLabel()
        layout.addWidget(self.summary)
        self.outputs = QCheckBox("해석 종류에 맞는 기본 결과 출력 추가")
        self.outputs.setChecked(True)
        if self._adding_step:
            layout.addWidget(self.outputs)
        # 이전 케이스 편집 창의 솔버·작업 폴더 등은 접힌 세부 설정에서 계속 편집한다.
        self.details_button = QPushButton("세부 설정 · 솔버 / 작업 폴더 / 해석 분야")
        self.details_button.setCheckable(True)
        self.details_button.setAutoDefault(False)
        layout.addWidget(self.details_button)
        details_body = QWidget()
        details_form = QFormLayout(details_body)
        self.case_editors = {}
        current = self._case["props"] if self._case else {}
        for field in app.kinds()["case"]["fields"]:
            key = field["name"]
            if key in ("scope", "type"):
                continue
            editor = FieldEditor(app, field, current.get(key))
            self.case_editors[key] = editor
            label = QLabel(key)
            label.setToolTip(field.get("desc", ""))
            details_form.addRow(label, editor.widget)
        self._initial_case_values = {key: ed.value() for key, ed in self.case_editors.items()}
        self.details = QScrollArea()
        self.details.setWidgetResizable(True)
        self.details.setWidget(details_body)
        self.details.setMaximumHeight(190)
        self.details.hide()
        self.details_button.toggled.connect(self.details.setVisible)
        layout.addWidget(self.details)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        buttons = QDialogButtonBox()
        self.save_button = buttons.addButton("저장", QDialogButtonBox.ButtonRole.AcceptRole)
        self.run_button = buttons.addButton("저장 후 해석", QDialogButtonBox.ButtonRole.ActionRole)
        buttons.addButton("취소", QDialogButtonBox.ButtonRole.RejectRole)
        self.save_button.setDefault(True)
        self.run_button.setAutoDefault(False)
        self.save_button.clicked.connect(lambda: self._apply())
        self.run_button.clicked.connect(lambda: self._apply(run=True))
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._source_groups = {kind: self._branch(self.available_tree, label, SYMBOLS[kind]) for kind, label in LABELS.items()}
        self._mesh_group = self._branch(self.included_tree, "메시 · 케이스 공통", "mesh")
        self._step_groups = {}
        for s in self._steps:
            sid = s["id"]
            branch = self._branch(self.included_tree, s["name"], "property", sid)
            if self._case:
                inheritance = [f"이전 {label} {'유지' if s['props'].get(field, 'keep') == 'keep' else '교체'}"
                               for label, field in (("하중", "loads_inheritance"), ("구속", "bcs_inheritance"))]
                branch.setToolTip(0, f"{s['props']['type']} · " + " · ".join(inheritance))
            self._step_groups[sid] = {kind: self._branch(branch, label, SYMBOLS[kind], sid)
                                      for kind, label in LABELS.items() if kind != "mesh_part"}
        self._load_catalog()
        if self._case:
            self._load_existing()
        self.add_button.clicked.connect(lambda: self._add(self.available_tree.selectedItems()))
        self.remove_button.clicked.connect(lambda: self._remove(self.included_tree.selectedItems()))
        self.available_tree.transferRequested.connect(self._add)
        self.included_tree.transferRequested.connect(self._remove)
        self.included_tree.dropRequested.connect(self._drop_add)
        self.available_tree.dropRequested.connect(lambda items, target: self._remove(items))
        self.included_tree.itemSelectionChanged.connect(self._right_selection)
        self.available_tree.itemSelectionChanged.connect(self._buttons)
        self.included_tree.itemChanged.connect(lambda *_: self.error.clear())
        self.step_selector.currentIndexChanged.connect(self._refresh)
        self.available_search.textChanged.connect(self._refresh)
        self.included_search.textChanged.connect(self._refresh)
        self.procedure.currentIndexChanged.connect(self._procedure_changed)
        self.name.textChanged.connect(lambda *_: self.error.clear())
        self.available_tree.expandAll()
        self.included_tree.expandAll()
        self._refresh()

    def _pane(self, title, search, tree):
        panel = QWidget()
        column = QVBoxLayout(panel)
        column.setContentsMargins(0, 0, 0, 0)
        heading = QLabel(title)
        font = heading.font()
        font.setBold(True)
        heading.setFont(font)
        column.addWidget(heading)
        search.setPlaceholderText("이름 또는 ID 검색")
        search.setClearButtonEnabled(True)
        search.setAccessibleName(title + " 검색")
        column.addWidget(search)
        column.addWidget(tree, 1)
        return panel

    def _branch(self, parent, label, symbol, sid=None):
        item = QTreeWidgetItem(parent, [label])
        item.setIcon(0, icon(symbol, self.palette().color(QPalette.ColorRole.Text).name()))
        item.setData(0, STEP, sid)
        item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsDropEnabled)
        return item

    def _load_catalog(self):
        branches = self.app.execute("project.tree")
        self.mesh_parts = self.app.execute("mesh_part.list")
        meshes = next(b["items"] for b in branches if b["kind"] == "mesh_part")
        count = self.app.execute("mesh.statistics")["elements"]
        assigned = sum(m.get("summary", {}).get("elements", 0) for m in meshes)
        for m in meshes:
            n = m.get("summary", {}).get("elements", 0)
            self._catalog_entry("mesh_part", m, f"요소 {n:,}개", count=n)
        if count > assigned:
            self._catalog_entry("mesh_part", {"id": 0, "name": "파트 미지정 메시"}, f"요소 {count - assigned:,}개", count=count - assigned)
        for kind, child_kind in (("load_set", "load"), ("bc_set", "bc")):
            for obj in self.app.execute(f"{kind}.list"):
                children = [self.app.execute(f"{child_kind}.get", id=c["id"]) for c in self.app.execute(f"{child_kind}.list", parent=obj["id"])]
                self._catalog_entry(kind, obj, f"{len(children)}개 항목", children=children)
        if self._case:
            scope = self._case["props"].get("scope")
            # 요소·셋·형상 범위를 파트 전체로 넓히지 않고 그대로 왕복한다.
            if scope and (scope["type"] != "parts" or any(("mesh_part", i) not in self._entries for i in scope["ids"])):
                self._custom_scope = scope
                self._catalog_entry("mesh_part", {"id": -1, "name": "기존 지정 범위"}, scope["type"])

    def _catalog_entry(self, kind, obj, detail, children=(), count=None):
        key = (kind, obj["id"])
        self._entries[key] = dict(obj=obj, detail=detail, children=children, count=count)
        self._source_items[key] = self._item(self._source_groups[kind], key)

    def _item(self, parent, key, sid=None, factor=1.0):
        data = self._entries[key]
        obj, kind = data["obj"], key[0]
        included = parent.treeWidget() is self.included_tree
        suffix = " · 억제됨" if obj.get("suppressed") else ""
        item = QTreeWidgetItem(parent, [obj["name"] + suffix, f"{factor:g}" if included and kind == "load_set" else ("" if included else data["detail"])])
        item.setData(0, KEY, key)
        item.setData(0, STEP, sid)
        item.setIcon(0, icon(SYMBOLS[kind], self.palette().color(QPalette.ColorRole.Text).name()))
        item.setToolTip(0, f"{obj['name']} · ID {obj['id']} · {data['detail']}")
        flags = Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsDragEnabled
        if included and kind == "load_set":
            flags |= Qt.ItemFlag.ItemIsEditable
            item.setToolTip(1, "하중 계수 · 더블클릭 또는 F2로 편집")
        item.setFlags(flags)
        for child in data["children"]:
            q = child["props"]
            detail = QTreeWidgetItem(item, [child["name"] + (" · 억제됨" if child.get("suppressed") else ""), q.get("type", "")])
            detail.setFlags(Qt.ItemFlag.ItemIsEnabled)
            detail.setIcon(0, icon(SYMBOLS[kind], self.palette().color(QPalette.ColorRole.Text).name()))
            detail.setToolTip(0, json.dumps(q.get("target", {}), ensure_ascii=False))
        return item

    def _insert(self, key, sid=None, factor=1.0):
        if key[0] == "mesh_part":
            sid = None
        token = (sid, *key)
        if token in self._selected_items:
            return
        if key not in self._entries:
            raise ValueError(f"참조 항목을 찾을 수 없습니다: {key}")
        parent = self._mesh_group if sid is None else self._step_groups[sid][key[0]]
        self._selected_items[token] = self._item(parent, key, sid, factor)
        parent.setExpanded(True)

    def _load_existing(self):
        scope = self._case["props"].get("scope")
        mesh = [-1] if self._custom_scope else (scope["ids"] if scope else [key[1] for key in self._entries if key[0] == "mesh_part"])
        for oid in mesh:
            self._insert(("mesh_part", oid))
        for s in self._steps:
            for ref in s["props"].get("load_sets", []) or []:
                self._insert(("load_set", ref["set"]), s["id"], ref.get("factor", 1.0))
            for oid in s["props"].get("bc_sets", []) or []:
                self._insert(("bc_set", oid), s["id"])

    def _transferable(self, items):
        found = {}
        def visit(item):
            if item.isHidden():
                return
            key = item.data(0, KEY)
            if key:
                found[(item.data(0, STEP), *key)] = item
            elif item.flags() & Qt.ItemFlag.ItemIsSelectable:
                for i in range(item.childCount()):
                    visit(item.child(i))
        for item in items:
            visit(item)
        return list(found.values())

    def _add(self, items):
        sid = self.step_selector.currentData()
        for item in self._transferable(items):
            self._insert(tuple(item.data(0, KEY)), sid)
        self.error.clear()
        self._refresh()

    def _remove(self, items):
        for item in self._transferable(items):
            token = (item.data(0, STEP), *item.data(0, KEY))
            self._selected_items.pop(token, None)
            item.parent().removeChild(item)
        self.error.clear()
        self._refresh()

    def _drop_add(self, items, target):
        if target and target.data(0, STEP) is not None:
            self.step_selector.setCurrentIndex(self.step_selector.findData(target.data(0, STEP)))
        self._add(items)

    def _right_selection(self):
        item = self.included_tree.currentItem()
        if item and item.data(0, STEP) is not None:
            self.step_selector.setCurrentIndex(self.step_selector.findData(item.data(0, STEP)))
        self._buttons()

    def _buttons(self):
        self.add_button.setEnabled(bool(self._transferable(self.available_tree.selectedItems())))
        self.remove_button.setEnabled(bool(self._transferable(self.included_tree.selectedItems())))

    def _refresh(self, *_):
        sid = self.step_selector.currentData()
        for key, item in self._source_items.items():
            token = (None if key[0] == "mesh_part" else sid, *key)
            query = self.available_search.text().strip().casefold()
            data = self._entries[key]
            text = " ".join([data["obj"]["name"], str(key[1]), *(c["name"] for c in data["children"])]).casefold()
            item.setHidden(token in self._selected_items or bool(query and query not in text))
        query = self.included_search.text().strip().casefold()
        for token, item in self._selected_items.items():
            data = self._entries[token[1:]]
            text = " ".join([data["obj"]["name"], str(token[2]), *(c["name"] for c in data["children"])]).casefold()
            item.setHidden(bool(query and query not in text))
        mesh_count = len(self.selected_mesh())
        load_count = sum(k[1] == "load_set" for k in self._selected_items)
        bc_count = sum(k[1] == "bc_set" for k in self._selected_items)
        self.summary.setText(f"포함: 메시 {mesh_count}개 · 하중 셋 {load_count}개 · 구속 셋 {bc_count}개   |   하중 계수는 오른쪽에서 편집")
        self._buttons()

    def _procedure_changed(self):
        self.step_selector.setItemText(0, "스텝 1 · " + self.procedure.currentText())

    def selected_mesh(self):
        return [oid for sid, kind, oid in self._selected_items if kind == "mesh_part"]

    def selected_load_sets(self, sid=None):
        sid = self.step_selector.currentData() if sid is None else sid
        out = []
        for (owner, kind, oid), item in self._selected_items.items():
            if owner == sid and kind == "load_set":
                factor = float(item.text(1).strip())
                if not math.isfinite(factor):
                    raise ValueError("하중 계수는 유한한 숫자로 입력하세요")
                out.append({"set": oid, "factor": factor})
        return out

    def selected_bc_sets(self, sid=None):
        sid = self.step_selector.currentData() if sid is None else sid
        return [oid for owner, kind, oid in self._selected_items if owner == sid and kind == "bc_set"]

    def _mesh_scope(self):
        mesh = self.selected_mesh()
        if not mesh:
            raise ValueError("오른쪽에 해석할 메시를 하나 이상 추가하세요")
        if -1 in mesh:
            if len(mesh) > 1:
                raise ValueError("기존 지정 범위를 제외한 뒤 새 메시 범위를 선택하세요")
            return self._custom_scope
        if not any(self._entries[("mesh_part", oid)]["count"] for oid in mesh):
            raise ValueError("선택한 메시에 요소가 없습니다")
        if 0 not in mesh:
            return {"type": "parts", "ids": mesh}
        # 파트 미지정 요소와 선택한 파트만 포함한다. 좌표·연결 배열은 UI로 가져오지 않는다.
        ids = set(self.app.execute("mesh.find", what="elements")["ids"])
        for p in self.mesh_parts:
            if p["id"] not in mesh:
                ids.difference_update(self.app.execute("mesh.find", what="elements", part=p["id"])["ids"])
        return {"type": "elements", "ids": sorted(ids)}

    def _apply(self, run=False):
        try:
            scope = self._mesh_scope()
            selections = {s["id"]: dict(load_sets=self.selected_load_sets(s["id"]), bc_sets=self.selected_bc_sets(s["id"])) for s in self._steps}
            options = {}
            for key, editor in self.case_editors.items():
                value = editor.value()
                if value != self._initial_case_values[key]:
                    options[key] = value
            with self.app.transaction("해석 케이스 구성"):
                if self._case:
                    case = self._case["id"]
                    params = {"id": case, "scope": scope, **options}
                    self.app.execute("case.update", **params)
                    if self.name.text().strip() and self.name.text().strip() != self._case["name"]:
                        self.app.execute("case.rename", id=case, name=self.name.text().strip())
                else:
                    params = {"scope": scope, **options}
                    if self.name.text().strip():
                        params["name"] = self.name.text().strip()
                    case = self.app.execute("case.create", **params)["id"]
                for sid, selection in selections.items():
                    if sid:
                        self.app.execute("step.update", id=sid, **selection)
                if self._adding_step:
                    proc = self.procedure.currentData()
                    sp = dict(parent=case, **selections[0])
                    if self.nlgeom.isChecked():
                        sp["nlgeom"] = True
                    sid = self.app.execute(f"step.create_{proc}", **sp)["id"]
                    if self.outputs.isChecked():
                        nodes, elements = DEFAULT_OUTPUTS.get(proc, (["U"], ["S"]))
                        for kind, variables in (("node_file", nodes), ("element_file", elements)):
                            if variables:
                                self.app.execute(f"output_request.create_{kind}", parent=sid, variables=variables)
                self.validation_issues = self.app.execute("case.check", id=case)
                errors = [i["message"] for i in self.validation_issues if i["severity"] == "error"]
                if run and errors:
                    raise ValueError("해석 전 확인이 필요합니다:\n" + "\n".join(errors[:6]))
            self.result_id, self.run_requested = case, run
            self.accept()
        except (Nasa95Error, ValueError) as e:
            self.error.setText(f"[{getattr(e, 'code', 'input')}] {e}")
