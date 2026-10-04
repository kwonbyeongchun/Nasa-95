"""해석 케이스 창(사용자 결정 2026-10-04, D14): 해석 종류·메시·하중 셋(계수)·구속 셋을 골라 케이스 + 스텝을 만든다.

케이스 = 메시 범위(scope) + 스텝(해석 종류, load_sets [{set, factor}], bc_sets). 모든 변경은 명령으로(case.create, step.create_<종류>, step.update,
output_request.create_*). 스텝의 세부(증분·비선형·감쇠 …)는 트리에서 스텝을 더블클릭해 기존 양식으로 고친다.
"""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
                               QListWidget, QListWidgetItem, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from ..api import App, OfepError

PROCEDURES = [  # (스텝 종류, 표시 이름) — 자주 쓰는 순서. 그 밖의 종류는 뒤에 이름 그대로
    ("static", "정적(static)"), ("frequency", "고유진동수(frequency)"), ("buckle", "좌굴(buckle)"), ("dynamic", "동적(dynamic)"),
    ("modal_dynamic", "모드 중첩 동해석(modal_dynamic)"), ("steady_state_dynamics", "정상상태 동해석(steady_state_dynamics)"),
    ("heat_transfer", "열전달(heat_transfer)"), ("coupled_temperature_displacement", "열-구조 연성(coupled)"),
    ("uncoupled_temperature_displacement", "열-구조 순차(uncoupled)"), ("visco", "점탄소성(visco)"),
]
DEFAULT_OUTPUTS = {"static": (["U", "RF"], ["S", "E"]), "frequency": (["U"], []), "buckle": (["U"], []), "dynamic": (["U", "V", "A"], ["S"]),
                   "modal_dynamic": (["U"], ["S"]), "steady_state_dynamics": (["U"], ["S"]), "heat_transfer": (["NT"], ["HFL"]),
                   "coupled_temperature_displacement": (["U", "NT"], ["S"]), "uncoupled_temperature_displacement": (["U", "NT"], ["S"]), "visco": (["U"], ["S", "PEEQ"])}


class CaseDialog(QDialog):
    """새 케이스(또는 스텝 편집: step 을 주면 그 스텝의 셋·메시 범위를 고친다)."""

    def __init__(self, app: App, parent=None, step: dict | None = None):
        super().__init__(parent)
        self.app, self.step = app, step
        self.result_id: int | None = None
        self.setWindowTitle("스텝의 셋·범위 편집" if step else "새 해석 케이스")
        self.resize(560, 620)
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.name = QLineEdit()
        self.name.setPlaceholderText("비우면 자동")
        self.procedure = QComboBox()
        known = {n for n, _ in PROCEDURES}
        for n, label in PROCEDURES:
            self.procedure.addItem(label, n)
        for s in app.kinds()["step"]["subtypes"]:
            if s["name"] not in known:
                self.procedure.addItem(s["name"], s["name"])
        self.nlgeom = QCheckBox("기하 비선형(큰 변형)")
        if not step:
            form.addRow("이름", self.name)
            form.addRow("해석 종류", self.procedure)
            form.addRow("", self.nlgeom)
        layout.addLayout(form)
        # 메시 범위
        mesh_box = QGroupBox("메시(해석에 넣을 메시 파트 — 모두 끄면 전체)")
        mesh_col = QVBoxLayout(mesh_box)
        self.mesh_list = QListWidget()
        self.mesh_list.setMaximumHeight(110)
        try:
            self.mesh_parts = app.execute("mesh_part.list")
        except OfepError:
            self.mesh_parts = []
        for mp in self.mesh_parts:
            item = QListWidgetItem(f"{mp['name']}  (ID {mp['id']})")
            item.setData(Qt.ItemDataRole.UserRole, mp["id"])
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked)
            self.mesh_list.addItem(item)
        if not self.mesh_parts:
            self.mesh_list.addItem("메시가 없습니다 — 메시 탭에서 먼저 메싱하세요")
        mesh_col.addWidget(self.mesh_list)
        layout.addWidget(mesh_box)
        # 하중 셋(계수)
        load_box = QGroupBox("하중 셋 — 체크한 셋이 들어가고, 계수를 곱해 조합한다(예: 1.2·D + 1.6·L)")
        load_col = QVBoxLayout(load_box)
        self.load_table = QTableWidget(0, 3)
        self.load_table.setHorizontalHeaderLabels(["포함", "하중 셋", "계수"])
        self.load_table.verticalHeader().hide()
        self.load_table.horizontalHeader().setStretchLastSection(False)
        self.load_table.setColumnWidth(0, 50)
        self.load_table.setColumnWidth(1, 300)
        self.load_table.setMaximumHeight(160)
        try:
            self.load_sets = app.execute("load_set.list")
            self.bc_sets = app.execute("bc_set.list")
        except OfepError:
            self.load_sets, self.bc_sets = [], []
        for ls in self.load_sets:
            i = self.load_table.rowCount()
            self.load_table.insertRow(i)
            chk = QTableWidgetItem()
            chk.setFlags(Qt.ItemFlag.ItemIsUserCheckable | Qt.ItemFlag.ItemIsEnabled)
            chk.setCheckState(Qt.CheckState.Unchecked)
            chk.setData(Qt.ItemDataRole.UserRole, ls["id"])
            name = QTableWidgetItem(f"{ls['name']}  ({self._count('load', ls['id'])}개 하중)")
            name.setFlags(Qt.ItemFlag.ItemIsEnabled)
            factor = QTableWidgetItem("1")
            factor.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            self.load_table.setItem(i, 0, chk)
            self.load_table.setItem(i, 1, name)
            self.load_table.setItem(i, 2, factor)
        load_col.addWidget(self.load_table)
        if not self.load_sets:
            load_col.addWidget(QLabel("하중 셋이 없습니다 — 해석 탭의 '하중 셋' 으로 만들고 그 안에 하중을 넣으세요"))
        layout.addWidget(load_box)
        # 구속 셋
        bc_box = QGroupBox("구속 셋")
        bc_col = QVBoxLayout(bc_box)
        self.bc_list = QListWidget()
        self.bc_list.setMaximumHeight(110)
        for bs in self.bc_sets:
            item = QListWidgetItem(f"{bs['name']}  ({self._count('bc', bs['id'])}개 구속)")
            item.setData(Qt.ItemDataRole.UserRole, bs["id"])
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Unchecked)
            self.bc_list.addItem(item)
        if not self.bc_sets:
            self.bc_list.addItem("구속 셋이 없습니다 — 해석 탭의 '구속 셋' 으로 만들고 그 안에 구속을 넣으세요")
        bc_col.addWidget(self.bc_list)
        layout.addWidget(bc_box)
        self.outputs = QCheckBox("기본 출력 요청 추가(변위·반력, 응력·변형률)")
        self.outputs.setChecked(True)
        if not step:
            layout.addWidget(self.outputs)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._apply)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        if step:
            self._load(step)

    def _count(self, kind: str, parent: int) -> int:
        try:
            return len(self.app.execute(f"{kind}.list", parent=parent))
        except OfepError:
            return 0

    # ---- 값
    def selected_mesh(self) -> list[int]:
        return [self.mesh_list.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.mesh_list.count())
                if self.mesh_list.item(i).data(Qt.ItemDataRole.UserRole) is not None and self.mesh_list.item(i).checkState() == Qt.CheckState.Checked]

    def selected_load_sets(self) -> list[dict]:
        out = []
        for i in range(self.load_table.rowCount()):
            if self.load_table.item(i, 0).checkState() == Qt.CheckState.Checked:
                f = float(self.load_table.item(i, 2).text().strip() or "1")
                out.append({"set": self.load_table.item(i, 0).data(Qt.ItemDataRole.UserRole), "factor": f})
        return out

    def selected_bc_sets(self) -> list[int]:
        return [self.bc_list.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.bc_list.count())
                if self.bc_list.item(i).data(Qt.ItemDataRole.UserRole) is not None and self.bc_list.item(i).checkState() == Qt.CheckState.Checked]

    def _load(self, step: dict) -> None:
        for ref in step["props"].get("load_sets", []) or []:
            for i in range(self.load_table.rowCount()):
                if self.load_table.item(i, 0).data(Qt.ItemDataRole.UserRole) == ref.get("set"):
                    self.load_table.item(i, 0).setCheckState(Qt.CheckState.Checked)
                    self.load_table.item(i, 2).setText(f"{ref.get('factor', 1.0):g}")
        for sid in step["props"].get("bc_sets", []) or []:
            for i in range(self.bc_list.count()):
                if self.bc_list.item(i).data(Qt.ItemDataRole.UserRole) == sid:
                    self.bc_list.item(i).setCheckState(Qt.CheckState.Checked)
        try:
            case = self.app.execute("case.get", id=step["parent"])
            scope = case["props"].get("scope")
            if scope and scope.get("type") == "parts":
                for i in range(self.mesh_list.count()):
                    item = self.mesh_list.item(i)
                    if item.data(Qt.ItemDataRole.UserRole) is not None:
                        item.setCheckState(Qt.CheckState.Checked if item.data(Qt.ItemDataRole.UserRole) in scope["ids"] else Qt.CheckState.Unchecked)
        except OfepError:
            pass

    def _apply(self):
        try:
            load_sets, bc_sets, mesh = self.selected_load_sets(), self.selected_bc_sets(), self.selected_mesh()
            if not self.step and not load_sets and not bc_sets:
                raise ValueError("하중 셋이나 구속 셋을 하나 이상 고르세요")
            if self.step:
                sid = self.step["id"]
                self.app.execute("step.update", id=sid, load_sets=load_sets or None, bc_sets=bc_sets or None)
                case = self.step["parent"]
                self.app.execute("case.update", id=case, scope={"type": "parts", "ids": mesh} if mesh and len(mesh) < len(self.mesh_parts) else None)
                self.result_id = case
            else:
                p = {}
                if self.name.text().strip():
                    p["name"] = self.name.text().strip()
                if mesh and len(mesh) < len(self.mesh_parts):
                    p["scope"] = {"type": "parts", "ids": mesh}
                case = self.app.execute("case.create", **p)["id"]
                proc = self.procedure.currentData()
                sp = {"parent": case}
                if load_sets:
                    sp["load_sets"] = load_sets
                if bc_sets:
                    sp["bc_sets"] = bc_sets
                if self.nlgeom.isChecked():
                    sp["nlgeom"] = True
                step = self.app.execute(f"step.create_{proc}", **sp)["id"]
                if self.outputs.isChecked():
                    node_vars, elem_vars = DEFAULT_OUTPUTS.get(proc, (["U"], ["S"]))
                    if node_vars:
                        self.app.execute("output_request.create_node_file", parent=step, variables=node_vars)
                    if elem_vars:
                        self.app.execute("output_request.create_element_file", parent=step, variables=elem_vars)
                self.result_id = case
            self.accept()
        except (OfepError, ValueError) as e:
            self.error.setText(f"[{getattr(e, 'code', 'input')}] {e}")
