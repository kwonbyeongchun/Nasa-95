"""결과 작업 공간(D13, plans/00008): 결과 트리(결과 → 프레임 → 필드·성분)와 결과 조작 패널.

모델 작업 공간과 같은 창·같은 모델을 쓴다. 전환은 표시만 바꾸고 명령은 잠그지 않는다. 모든 동작은 명령 계층(view.result_show·
view.legend·view.animate·result.*)을 부른다 — 이 모듈은 그 호출자일 뿐이다(아키텍처 규칙 3·9).
"""
from __future__ import annotations

from PySide6.QtCore import QObject, Qt, QTimer, Signal
from PySide6.QtWidgets import (QComboBox, QDoubleSpinBox, QFormLayout, QFrame, QHBoxLayout, QLabel, QPushButton, QScrollArea, QSlider,
                               QSizePolicy, QSpinBox, QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget)

from ..api import App, Nasa95Error

COLORMAPS = ("rainbow", "blue_red", "heat", "grayscale")
OUTDATED_TEXT = "모델이 바뀜 — 재해석 필요"


class ResultWorkspace(QObject):
    changed = Signal()  # 화면을 다시 그려야 한다(창이 present·범례 갱신)

    def __init__(self, app: App, parent: QWidget | None = None):
        super().__init__(parent)
        self._app = app
        self._loading = False
        self._result: int | None = None  # 패널이 다루는 결과(result.open 번호)
        # ---- 트리: 결과 → 프레임 → 필드:성분
        self.tree = QTreeWidget()
        self.tree.setObjectName("resultTree")
        self.tree.setHeaderHidden(True)
        self.tree.setIndentation(14)
        self.tree.itemClicked.connect(self._tree_clicked)
        self.tree_panel = QWidget()
        column = QVBoxLayout(self.tree_panel)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(0)
        head = QLabel("열린 결과")
        head.setContentsMargins(12, 8, 12, 4)
        column.addWidget(head)
        column.addWidget(self.tree, 1)
        self.tree_empty = QLabel("열린 결과가 없습니다.\n해석 탭에서 케이스를 실행하거나 결과 탭에서 결과를 여세요.")
        self.tree_empty.setProperty("role", "muted")
        self.tree_empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.tree_empty.setWordWrap(True)
        column.addWidget(self.tree_empty)
        # ---- 조작 패널(스크롤 영역에 넣어 최소 높이가 창을 키우지 않게 한다 — 도크는 세로로 쌓인다)
        inner = QWidget()
        inner.setObjectName("resultControls")
        self.controls = QScrollArea()
        self.controls.setWidgetResizable(True)
        self.controls.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.controls.setFrameShape(QFrame.Shape.NoFrame)
        self.controls.setWidget(inner)
        self.controls.setMinimumHeight(0)
        col = QVBoxLayout(inner)
        col.setContentsMargins(12, 4, 12, 8)
        self.title = QLabel("결과")
        self.title.setObjectName("selectionTitle")
        self.outdated = QLabel("")
        self.outdated.setObjectName("resultOutdated")
        self.outdated.setWordWrap(True)
        self.outdated.setStyleSheet("color: #D08A00; font-weight: 600;")
        col.addWidget(self.title)
        col.addWidget(self.outdated)
        form = QFormLayout()
        form.setContentsMargins(0, 4, 0, 0)
        form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.field = QComboBox()
        self.component = QComboBox()
        self.frame = QSpinBox()
        self.frame.setMinimum(1)
        self.frame_slider = QSlider(Qt.Orientation.Horizontal)
        self.frame_slider.setMinimum(1)
        self.frame_info = QLabel("")
        self.frame_info.setProperty("role", "muted")
        self.deform = QDoubleSpinBox()
        self.deform.setRange(0.0, 1e9)
        self.deform.setDecimals(2)
        self.deform.setValue(0.0)
        self.deform.setSpecialValueText("변형 없이")
        self.colormap = QComboBox()
        self.colormap.addItems(COLORMAPS)
        self.levels = QSpinBox()
        self.levels.setRange(0, 64)
        self.levels.setValue(0)
        self.levels.setSpecialValueText("연속")
        frame_row = QWidget()
        fr = QHBoxLayout(frame_row)
        fr.setContentsMargins(0, 0, 0, 0)
        fr.addWidget(self.frame)
        fr.addWidget(self.frame_slider, 1)
        form.addRow("결과 종류", self.field)
        form.addRow("성분", self.component)
        form.addRow("프레임", frame_row)
        form.addRow("", self.frame_info)
        form.addRow("변형 배율", self.deform)
        form.addRow("색상표", self.colormap)
        form.addRow("단계 수", self.levels)
        col.addLayout(form)
        buttons = QHBoxLayout()
        self.play = QPushButton("애니메이션")
        self.play.setCheckable(True)
        self.hide_button = QPushButton("표시 끄기")
        buttons.addWidget(self.play)
        buttons.addWidget(self.hide_button)
        col.addLayout(buttons)
        self.summary = QLabel("")
        self.summary.setProperty("role", "muted")
        self.summary.setWordWrap(True)
        col.addWidget(self.summary)
        col.addStretch(1)
        # 긴 결과 이름·경로·프레임 설명이 도크 폭을 늘리지 않도록 높이로 확장한다.
        for label in (self.title, self.outdated, self.frame_info, self.summary):
            label.setWordWrap(True)
            label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        for name in ("field", "component", "colormap"):
            combo = getattr(self, name)
            combo.setAccessibleName(f"결과 {name}")
            combo.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
            combo.setMinimumContentsLength(8)
            combo.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.field.currentIndexChanged.connect(self._field_changed)
        self.component.currentIndexChanged.connect(lambda _i: self.apply())
        self.frame.valueChanged.connect(self._frame_changed)
        self.frame_slider.valueChanged.connect(self.frame.setValue)
        self.deform.valueChanged.connect(lambda _v: self.apply())
        self.colormap.currentIndexChanged.connect(lambda _i: self._legend_changed())
        self.levels.valueChanged.connect(lambda _v: self._legend_changed())
        self.play.toggled.connect(self._toggle_animation)
        self.hide_button.clicked.connect(self.hide_result)
        self._anim = QTimer(self)
        self._anim.timeout.connect(self._advance)

    # ------------------------------------------------------------ 상태 조회
    def results(self) -> list[dict]:
        try:
            return self._app.execute("result.list")
        except Nasa95Error:
            return []

    @property
    def result(self) -> int | None:
        return self._result

    def select_result(self, result: int | None) -> None:
        """패널이 다룰 결과를 바꾼다(필드·프레임 목록을 다시 채운다)."""
        if result == self._result and result is not None:
            return
        self._result = result
        self._fill()

    def refresh(self) -> None:
        """모델·결과가 바뀐 뒤: 트리와 outdated 표시를 다시 그린다. 패널이 다루던 결과가 닫혔으면 다른 것으로."""
        rows = self.results()
        ids = [r["id"] for r in rows]
        if self._result not in ids:
            self._result = ids[-1] if ids else None
            self._fill()
        self._fill_tree(rows)
        self._update_outdated(rows)

    # ------------------------------------------------------------ 트리
    def _fill_tree(self, rows: list[dict]) -> None:
        self.tree.blockSignals(True)
        self.tree.clear()
        for r in rows:
            name = f"결과 {r['id']}" + (f" · 케이스 {r['case']}" if r.get("case") else "") + ("  ⚠" if r.get("outdated") else "")
            top = QTreeWidgetItem([name])
            top.setData(0, Qt.ItemDataRole.UserRole, ("result", r["id"]))
            top.setToolTip(0, r["path"] + ("\n" + OUTDATED_TEXT if r.get("outdated") else ""))
            self.tree.addTopLevelItem(top)
            try:
                frames = self._app.execute("result.steps", result=r["id"])
            except Nasa95Error:
                frames = []
            for fr in frames:
                label = f"프레임 {fr['frame']} · 스텝 {fr.get('step', '')} 증분 {fr.get('increment', '')}"
                if fr.get("value") is not None:
                    label += f" · {fr['value']:.4g}"
                node = QTreeWidgetItem([label])
                node.setData(0, Qt.ItemDataRole.UserRole, ("frame", r["id"], fr["frame"]))
                top.addChild(node)
                if fr["frame"] == frames[-1]["frame"]:  # 마지막 프레임만 필드를 펼쳐 둔다(프레임마다 조회하지 않게)
                    try:
                        fields = self._app.execute("result.fields", result=r["id"], frame=fr["frame"])
                    except Nasa95Error:
                        fields = []
                    for f in fields:
                        for comp in f["components"] + f["derived"]:
                            leaf = QTreeWidgetItem([f"{f['name']} : {comp}"])
                            leaf.setData(0, Qt.ItemDataRole.UserRole, ("field", r["id"], fr["frame"], f["name"], comp))
                            node.addChild(leaf)
                    node.setExpanded(True)
            top.setExpanded(r["id"] == self._result)
        self.tree.blockSignals(False)
        self.tree_empty.setVisible(not rows)

    def _tree_clicked(self, item, _column):
        data = item.data(0, Qt.ItemDataRole.UserRole)
        if not data:
            return
        if data[0] == "result":
            self.select_result(data[1])
        elif data[0] == "frame":
            self.select_result(data[1])
            self.frame.setValue(data[2])
        elif data[0] == "field":
            self.select_result(data[1])
            self._loading = True
            self.frame.setValue(data[2])
            self.field.setCurrentText(data[3])
            self._fill_components()
            self.component.setCurrentText(data[4])
            self._loading = False
            self.apply()

    # ------------------------------------------------------------ 패널 채우기
    def _fill(self) -> None:
        self._loading = True
        self.field.clear()
        self.component.clear()
        self.frame_info.setText("")
        if self._result is None:
            self.title.setText("결과 없음")
            self.summary.setText("")
            self.controls.setEnabled(False)
            self._loading = False
            return
        self.controls.setEnabled(True)
        try:
            info = self._app.execute("result.info", result=self._result)
            frames = self._app.execute("result.steps", result=self._result)
        except Nasa95Error:
            self._loading = False
            return
        self.title.setText(f"결과 {self._result}" + (f" · 케이스 {info['case']}" if info.get("case") else ""))
        self.summary.setText(f"{info['path']}\n절점 {info['nodes']} · 요소 {info['elements']} · 프레임 {info['frames']}")
        n = max(len(frames), 1)
        self.frame.setMaximum(n)
        self.frame_slider.setMaximum(n)
        # 화면에 이미 입힌 결과가 이 결과면 그 설정을 따르고, 아니면 마지막 프레임
        shown = self._shown()
        mine = shown if shown and shown["result"] == self._result else None
        self.frame.setValue(mine["frame"] if mine else n)
        self.frame_slider.setValue(self.frame.value())
        if mine:
            self.deform.setValue(float(mine.get("deform_scale", 0.0) or 0.0))
        self._fill_fields(shown)
        self._loading = False

    def _shown(self) -> dict | None:
        try:
            st = self._app.execute("view.result_state")
        except Nasa95Error:
            return None
        return st["settings"] if st.get("shown") else None

    def _fill_fields(self, shown: dict | None = None) -> None:
        was = self._loading
        self._loading = True
        current = self.field.currentText()
        self.field.clear()
        try:
            fields = self._app.execute("result.fields", result=self._result, frame=self.frame.value())
        except Nasa95Error:
            fields = []
        self._fields = {f["name"]: f["components"] + f["derived"] for f in fields}
        self.field.addItems(list(self._fields))
        want = shown["field"] if shown and shown.get("result") == self._result else current
        if want in self._fields:
            self.field.setCurrentText(want)
        elif "STRESS" in self._fields:
            self.field.setCurrentText("STRESS")
        self._fill_components(shown)
        self._loading = was

    def _fill_components(self, shown: dict | None = None) -> None:
        was = self._loading
        self._loading = True
        current = self.component.currentText()
        self.component.clear()
        comps = self._fields.get(self.field.currentText(), [])
        self.component.addItems(comps)
        want = shown["component"] if shown and shown.get("field") == self.field.currentText() else current
        if want in comps:
            self.component.setCurrentText(want)
        elif "mises" in comps:
            self.component.setCurrentText("mises")
        elif "magnitude" in comps:
            self.component.setCurrentText("magnitude")
        self._loading = was

    def _field_changed(self, _index):
        if self._loading:
            return
        self._fill_components()
        self.apply()

    def _frame_changed(self, value):
        self.frame_slider.blockSignals(True)
        self.frame_slider.setValue(value)
        self.frame_slider.blockSignals(False)
        if self._loading:
            return
        self._fill_fields()
        self.apply()

    # ------------------------------------------------------------ 화면에 적용
    def settings(self) -> dict | None:
        if self._result is None or not self.field.currentText() or not self.component.currentText():
            return None
        params = {"result": self._result, "frame": self.frame.value(), "field": self.field.currentText(), "component": self.component.currentText()}
        if self.deform.value() > 0:
            params["deform_scale"] = self.deform.value()
        return params

    def apply(self) -> None:
        if self._loading:
            return
        params = self.settings()
        if not params:
            return
        try:
            self._app.execute("view.result_show", **params)
            st = self._app.execute("view.result_state")
            self.frame_info.setText(f"스텝 {st.get('step', '')} 증분 {st.get('increment', '')}" + (f" · {st['value']:.4g}" if st.get("value") is not None else ""))
        except Nasa95Error as e:
            self.frame_info.setText(str(e))
            return
        self.changed.emit()

    def _legend_changed(self) -> None:
        if self._loading:
            return
        try:
            self._app.execute("view.legend", colormap=self.colormap.currentText(), levels=self.levels.value())
        except Nasa95Error:
            return
        self.changed.emit()

    def hide_result(self) -> None:
        self.play.setChecked(False)
        try:
            self._app.execute("view.result_show")
        except Nasa95Error:
            return
        self.changed.emit()

    def show_last(self) -> None:
        """결과 공간에 들어올 때: 화면에 입힌 결과가 없으면 패널의 설정(마지막 프레임·STRESS mises 등)을 입힌다."""
        if self._shown() is None and self._result is not None:
            self.apply()

    # ------------------------------------------------------------ 애니메이션(view.animate 를 타이머로 넘긴다)
    def _toggle_animation(self, on: bool) -> None:
        try:
            if on:
                params = self.settings()
                if not params:
                    self.play.setChecked(False)
                    return
                self._app.execute("view.result_show", **params)
                self._app.execute("view.animate", interval_ms=200, loop=True, **({"deform_scale": self.deform.value()} if self.deform.value() > 0 else {}))
                self._anim.start(200)
            else:
                self._anim.stop()
                self._app.execute("view.animate", stop=True)
        except Nasa95Error:
            self._anim.stop()
            self.play.setChecked(False)
            return
        self.changed.emit()

    def _advance(self) -> None:
        try:
            st = self._app.execute("view.animate", advance=True)
        except Nasa95Error:
            self.play.setChecked(False)
            return
        if not st.get("playing", True):
            self.play.setChecked(False)
            return
        shown = self._shown()
        if shown:
            self._loading = True
            self.frame.setValue(shown["frame"])
            self._loading = False
        self.changed.emit()

    def stop(self) -> None:
        if self.play.isChecked():
            self.play.setChecked(False)

    # ------------------------------------------------------------ outdated
    def _update_outdated(self, rows: list[dict]) -> None:
        mine = next((r for r in rows if r["id"] == self._result), None)
        self.outdated.setText(("⚠ " + OUTDATED_TEXT) if mine and mine.get("outdated") else "")

    def is_outdated(self) -> bool:
        return bool(self.outdated.text())
