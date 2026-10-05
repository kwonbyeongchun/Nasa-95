"""주 창: 워크 트리 · 3D 뷰 · 속성 편집. 모든 동작은 명령 계층(app.execute)으로 간다(아키텍처 규칙 3)."""
from __future__ import annotations

import json
import os
import pathlib
import threading
from typing import Callable

from PySide6.QtCore import QObject, Qt, QThread, QTimer, Signal
from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import (QApplication, QDockWidget, QFileDialog, QHBoxLayout, QInputDialog, QMainWindow, QMenu, QMessageBox, QPlainTextEdit,
                               QPushButton, QMenuBar, QSplitter, QStatusBar, QTableWidget, QTableWidgetItem, QToolBar, QTreeWidget,
                               QTreeWidgetItem, QVBoxLayout, QWidget)

from ..api import App, Nasa95Error
from .forms import CommandDialog, ObjectDialog
from .legend import LegendBar
from .viewport import Viewport
from .theme import Theme
from .workspace import Workspace
from .ribbon import saribbon
from .window_frame import top_resize_hit


class _GuiExecutor(QObject):
    """REST 서버 스레드의 명령 호출을 GUI 스레드로 넘겨 UI 와 같은 순서로 실행한다(API-26 스레드 규칙)."""
    request = Signal(object)

    def __init__(self, app: App):
        super().__init__()
        self.app = app
        self.after: Callable[[str], None] | None = None  # 명령 뒤 창 갱신(뷰 명령은 모델 통지가 없어 여기서 다시 그린다)
        self.request.connect(self._run, Qt.ConnectionType.QueuedConnection)

    def __call__(self, name: str, params: dict):
        if QThread.currentThread() is self.thread():
            return self.app.execute(name, params)
        box = {"done": threading.Event()}
        self.request.emit((name, params, box))
        if not box["done"].wait(timeout=600):
            raise Nasa95Error("timeout", "UI 가 응답하지 않습니다", {"command": name})
        if "error" in box:
            raise box["error"]
        return box["result"]

    def _run(self, item):
        name, params, box = item
        try:
            box["result"] = self.app.execute(name, params)
        except Nasa95Error as e:
            box["error"] = e
        except Exception as e:  # noqa: BLE001
            box["error"] = Nasa95Error("internal", str(e), {})
        finally:
            box["done"].set()
        if self.after:
            self.after(name)


class MainWindow(Workspace, saribbon.SARibbonMainWindow):
    def __init__(self, app: App | None = None):
        # Vulkan 자식 창 때문에 리본·도크까지 HWND로 승격되면 Windows 리사이즈가 느려진다.
        QApplication.setAttribute(Qt.ApplicationAttribute.AA_DontCreateNativeWidgetSiblings)
        super().__init__()
        # 확장 API의 메뉴는 SARibbon의 표시 메뉴와 별도로 보관한다.
        self._legacy_menu = QMenuBar(self)
        self._legacy_menu.hide()
        self.setStyleSheet("")  # SARibbon 기본 Office QSS 대신 공통 테마를 쓴다.
        # SARibbon 생성자의 지연된 테마 적용 뒤에도 앱 테마를 유지한다.
        QTimer.singleShot(0, self._restore_workbench_theme)
        self.theme = Theme(self)
        self.app = app or App()
        if app is None:
            self.app.execute("view.quality", antialiasing="ssaa2")
        self._executor = _GuiExecutor(self.app)
        if hasattr(self.app, "rest"):
            self.app.rest.executor = self._executor
            self._executor.after = self._after_remote_command  # 뷰·선택 명령은 통지가 없으므로 바로 다시 그린다
        self.setWindowTitle("NASA-95")
        self.resize(1280, 800)

        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["이름", "종류"])
        self.tree.setColumnWidth(0, 240)
        self.tree.itemSelectionChanged.connect(self._on_select)
        self.tree.itemDoubleClicked.connect(lambda item, col: self._guard(self._edit_selected))
        self.tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._tree_menu)
        self.viewport = Viewport(self.app)
        self.viewport.picked.connect(self._on_pick)
        self.viewport.presented.connect(self._update_legend)
        self.viewport.hovered.connect(self._on_hover)
        self.viewport.failed.connect(self._renderer_failed)

        self.legend = LegendBar()
        view_area = QWidget()
        row = QHBoxLayout(view_area)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(0)
        row.addWidget(self.viewport, 1)
        row.addWidget(self.legend)

        self.props = QTableWidget(0, 2)
        self.props.setHorizontalHeaderLabels(["속성", "값"])
        self.props.horizontalHeader().setStretchLastSection(True)
        self.props.itemChanged.connect(self._on_prop_edit)
        self._setup_workspace(view_area)
        self.setStatusBar(QStatusBar())

        self._selected: tuple[str, int] | None = None
        self._mesh_job = None  # 진행 중인 배경 메싱의 (진행 창, 타이머)
        self._picked: list[dict] = []  # 화면에서 고른 것(Ctrl 로 여러 개)
        self._editing = False
        self._ext_tree_actions: list[dict] = []  # ext.register_tree_action 으로 더한 상황 메뉴 항목
        self._ext_toolbar = None
        self._ext_menus: dict[str, QMenu] = {}  # ext.register_menu/dialog 가 쓰는 메뉴(이름별)
        self._build_menus()
        if getattr(self.app, "extensions", None):
            self.app.extensions.attach_window(self)  # 켜진 확장의 ui(window): 메뉴·패널 추가(API-19)
        self._build_ribbon()
        self._event_token = self.app.subscribe(self._model_event)
        self.refresh()

    def nativeEvent(self, event_type, message):
        hit = top_resize_hit(self, event_type, message)
        if hit is not None:
            return True, hit
        return super().nativeEvent(event_type, message)

    def _restore_workbench_theme(self):
        self.setStyleSheet("")
        self._theme_changed()

    def menuBar(self):
        # 기존 확장의 menuBar().addMenu() 계약을 리본에서도 유지한다.
        return self._legacy_menu if hasattr(self, "_legacy_menu") else super().menuBar()

    # ------------------------------------------------------------ 메뉴
    def _build_menus(self):
        def act(menu, text, fn, shortcut=None):
            a = QAction(text, self)
            if shortcut:
                a.setShortcut(QKeySequence(shortcut))
            a.triggered.connect(lambda: self._guard(fn))
            menu.addAction(a)
            return a

        m = self.menuBar().addMenu("파일(&F)")
        act(m, "새로 만들기", lambda: self.app.execute("project.new"), "Ctrl+N")
        act(m, "열기...", self._open, "Ctrl+O")
        act(m, "저장", self._save, "Ctrl+S")
        act(m, "다른 이름으로 저장...", lambda: self._save(True))
        m.addSeparator()
        act(m, "형상 가져오기(STEP·IGES·BREP)...", self._import_geometry)
        act(m, "CalculiX 덱 가져오기...", self._import_deck)
        act(m, "이미지로 저장...", self._screenshot)
        m.addSeparator()
        act(m, "끝내기", self.close, "Ctrl+Q")

        m = self.menuBar().addMenu("편집(&E)")
        act(m, "실행 취소", self.app.undo, "Ctrl+Z")
        act(m, "다시 실행", self.app.redo, "Ctrl+Y")
        act(m, "선택 객체 편집...", self._edit_selected, "Return")
        act(m, "선택 객체 삭제", self._delete_selected, "Delete")
        act(m, "선택 객체 억제/해제", self._toggle_suppress)

        m = self.menuBar().addMenu("형상(&G)")
        act(m, "상자...", self._make_box)
        act(m, "실린더...", self._make_cylinder)

        m = self.menuBar().addMenu("메시(&M)")
        act(m, "선택한 파트 자동 메싱...", self._mesh_part)
        act(m, "메시 상태", lambda: self._info("메시 상태", self.app.execute("mesh.status")))

        m = self.menuBar().addMenu("해석(&A)")
        act(m, "선택한 케이스 실행", self._run_case)
        act(m, "실행 상태", self._run_status)
        act(m, "덱 미리 보기", self._preview_deck)

        m = self.menuBar().addMenu("결과(&R)")
        act(m, "선택한 케이스의 결과 열기", self._open_result)
        act(m, "컨투어 표시...", self._show_contour)
        act(m, "결과 표시 끄기", self._hide_result)

        m = self.menuBar().addMenu("보기(&V)")
        for name, label, key in [("iso", "등각", "7"), ("front", "정면", "1"), ("top", "평면", "5"), ("right", "우측면", "4")]:
            act(m, f"{label} 뷰", lambda n=name: self._view("view.standard", name=n), key)
        act(m, "전체 맞춤", lambda: self._view("view.fit"), "F")
        m.addSeparator()
        for mode, label in [("shaded_edges", "음영+경계선"), ("shaded", "음영"), ("wireframe", "와이어프레임")]:
            act(m, label, lambda md=mode: self._view("view.display_mode", mode=md))
        aa = act(m, "안티앨리어싱", lambda: self._view("view.quality", antialiasing="ssaa2" if aa.isChecked() else "none"))
        aa.setCheckable(True)
        aa.setChecked(self.app.execute("view.quality_get")["antialiasing"] != "none")
        self.viewport.presented.connect(lambda: aa.setChecked(self.app.execute("view.quality_get")["antialiasing"] != "none"))
        m.addSeparator()
        act(m, "하중·경계조건 표시(선택한 스텝)", self._show_symbols)
        act(m, "하중·경계조건 숨김", lambda: self._view("view.symbols"))
        m.addSeparator()
        act(m, "클리핑 평면...", lambda: self._command_dialog("view.clip"))
        act(m, "클리핑 끄기", lambda: self._view("view.clip"))
        act(m, "투명...", lambda: self._command_dialog("view.transparency"))
        act(m, "불투명", lambda: self._view("view.transparency"))
        act(m, "화면 요소(좌표축·범례·글자)...", lambda: self._command_dialog("view.hud"))
        act(m, "렌더러 진단", lambda: self._info("렌더러", self.app.execute("view.diagnostics")))

        m = self.menuBar().addMenu("도구(&T)")
        act(m, "설정(솔버·스레드·작업 폴더)...", self._settings)
        m.addSeparator()
        act(m, "REST 서버(주소·포트·토큰, 켜기·끄기)...", self._rest_start)
        act(m, "REST 상태·호출 기록", lambda: self._info("REST", {"status": self.app.execute("server.status"),
                                                                   "log": self.app.execute("server.log", last=20)}))

    # ------------------------------------------------------------ 확장 UI 훅(API-19): ExtensionManager 가 ext.register_* 항목을 넘긴다
    def _ext_run(self, item: dict, **extra):
        """항목의 명령을 실행하고 화면을 새로 그린다."""
        params = dict(item.get("params") or {})
        params.update(extra)
        result = self.app.execute(item["command"], params)
        self.viewport.present()
        self.statusBar().showMessage(f"{item['label']}: {json.dumps(result, ensure_ascii=False)[:200]}", 5000)
        return result

    def _ext_menu(self, name: str) -> QMenu:
        if name not in self._ext_menus:
            self._ext_menus[name] = self.menuBar().addMenu(name)
        return self._ext_menus[name]

    def add_extension_menu(self, item: dict) -> QAction:
        a = QAction(item["label"], self)
        a.triggered.connect(lambda: self._guard(lambda: self._ext_run(item)))
        self._ext_menu(item.get("menu") or "확장").addAction(a)
        return a

    def add_extension_toolbar(self, item: dict) -> QAction:
        if self._ext_toolbar is None:
            self._ext_toolbar = QToolBar("확장", self)
            self.addToolBar(self._ext_toolbar)
        a = QAction(item["label"], self)
        a.triggered.connect(lambda: self._guard(lambda: self._ext_run(item)))
        self._ext_toolbar.addAction(a)
        return a

    def add_extension_panel(self, item: dict) -> QDockWidget:
        """명령의 결과를 보여 주는 도킹 패널. '새로 고침'으로 다시 실행한다."""
        text = QPlainTextEdit()
        text.setReadOnly(True)
        button = QPushButton("새로 고침")

        def refresh():
            text.setPlainText(json.dumps(self._ext_run(item), ensure_ascii=False, indent=1))
        button.clicked.connect(lambda: self._guard(refresh))
        body = QWidget()
        layout = QVBoxLayout(body)
        layout.setContentsMargins(2, 2, 2, 2)
        layout.addWidget(button)
        layout.addWidget(text)
        dock = QDockWidget(item["label"], self)
        dock.setObjectName("ext_panel_" + item["label"])
        dock.setWidget(body)
        self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
        return dock

    def add_extension_dialog(self, item: dict) -> QAction:
        """명령의 매개변수 입력 대화상자를 여는 메뉴 항목(등록부의 매개변수 정의로 만든다)."""
        a = QAction(item["label"] + "...", self)
        a.triggered.connect(lambda: self._guard(lambda: self._command_dialog(item["command"])))
        self._ext_menu(item.get("menu") or "확장").addAction(a)
        return a

    def add_extension_tree_action(self, item: dict) -> None:
        self._ext_tree_actions.append(item)

    # ------------------------------------------------------------ 도우미
    def _renderer_failed(self, msg):
        """렌더러 오류: 모달 대화 상자를 띄우지 않는다(그리기 도중 떠서 창·테스트를 멈춘다). 작업 메시지와 상태 표시줄에 적는다."""
        self.log.appendPlainText(f"[렌더러] {msg}")
        self.log_dock.show()
        self.statusBar().showMessage(f"렌더러 오류: {msg}", 10000)

    def _guard(self, fn):
        try:
            fn()
            self._update_actions()
        except Nasa95Error as e:
            self.log.appendPlainText(f"[{e.code}] {e}")
            QMessageBox.warning(self, "오류", f"[{e.code}] {e}")
            self.statusBar().showMessage(f"오류: {e}", 5000)

    def _view(self, command, **params):
        self.app.execute(command, **params)
        self.viewport.present()
        self._decorate_tree()

    def _info(self, title, data):
        QMessageBox.information(self, title, json.dumps(data, ensure_ascii=False, indent=1)[:4000])

    def _ask_number(self, title, label, default, minimum=0.0):
        value, ok = QInputDialog.getDouble(self, title, label, default, minimum, 1e12, 4)
        return value if ok else None

    # ------------------------------------------------------------ 트리·속성
    def refresh(self):
        if self._closed:
            return
        scroll = self._capture_tree()
        selected = self._selected
        self.tree.blockSignals(True)
        self.tree.clear()
        for branch in self.app.execute("project.tree"):
            label = {"part": "형상", "mesh_part": "메시", "result_file": "결과"}.get(branch["kind"], branch["label"])
            top = QTreeWidgetItem([f"{label} ({branch['count']})", branch["kind"]])
            top.setData(0, Qt.ItemDataRole.UserRole, None)
            self.tree.addTopLevelItem(top)
            self._add_items(top, branch["items"])
            top.setExpanded(branch["count"] > 0)
        self.tree.blockSignals(False)
        self._selected = selected
        self._show_props()
        self._restore_tree(scroll)
        self.result_ws.refresh()  # 결과 트리와 outdated 표시(D13)
        self._update_status()
        self.viewport.refresh()
        self._update_legend()
        info = self.app.execute("project.info")
        self.setWindowTitle((pathlib.Path(info["path"]).name if info.get("path") else "새 프로젝트") +
                            (" *" if info.get("modified") else "") + " — NASA-95")
        project_name = pathlib.Path(info["path"]).name if info.get("path") else "새 프로젝트"
        self.breadcrumbs.setText(project_name + "  ›  3D 뷰")
        self.statusBar().showMessage(f"객체 {info['objects']} · 노드 {info['nodes']} · 요소 {info['elements']}" +
                                     (f" · {info['path']}" if info.get("path") else "") + (" (수정됨)" if info.get("modified") else ""))

    def _add_items(self, parent, items):
        for it in items:
            label = it["name"] + ("  (억제)" if it.get("suppressed") else "")
            node = QTreeWidgetItem([label, it["kind"]])
            node.setData(0, Qt.ItemDataRole.UserRole, (it["kind"], it["id"]))
            node.setData(0, Qt.ItemDataRole.UserRole + 1, bool(it.get("suppressed")))
            if it.get("suppressed"):
                node.setForeground(0, Qt.GlobalColor.gray)
            parent.addChild(node)
            if it.get("children"):
                self._add_items(node, it["children"])
                node.setExpanded(True)
            if self._selected == (it["kind"], it["id"]):
                node.setSelected(True)
                self.tree.scrollToItem(node)

    def _on_select(self):
        items = self.tree.selectedItems()
        self._selected = items[0].data(0, Qt.ItemDataRole.UserRole) if items else None
        self._show_props()
        self._update_actions()
        self._symbols_follow_selection()  # 하중·구속 토글이 켜져 있으면 고른 스텝의 심볼로

    def _show_props(self):
        self._editing = True
        self.props.setRowCount(0)
        self.selection_title.setText("선택한 객체 없음")
        self.selection_hint.setText("트리 또는 화면에서 객체를 선택하세요.")
        if self._selected:
            kind, oid = self._selected
            try:
                obj = self.app.execute(f"{kind}.get", id=oid)
            except Nasa95Error:
                self._selected = None
                obj = None
            if obj:
                self.selection_title.setText(obj["name"])
                self.selection_hint.setText(f"{self.app.kinds()[kind]['label']} · ID {oid}  |  더블클릭으로 상세 편집")
                rows = [("id", obj["id"], False), ("name", obj["name"], True), ("suppressed", obj["suppressed"], False)]
                rows += [(k, v, k != "type") for k, v in sorted(obj["props"].items())]
                self.props.setRowCount(len(rows))
                for r, (k, v, editable) in enumerate(rows):
                    key = QTableWidgetItem(k)
                    key.setFlags(key.flags() & ~Qt.ItemFlag.ItemIsEditable)
                    val = QTableWidgetItem(json.dumps(v, ensure_ascii=False) if not isinstance(v, str) else v)
                    if not editable:
                        val.setFlags(val.flags() & ~Qt.ItemFlag.ItemIsEditable)
                    self.props.setItem(r, 0, key)
                    self.props.setItem(r, 1, val)
        self._editing = False

    def _on_prop_edit(self, item):
        if self._editing or not self._selected or item.column() != 1:
            return
        kind, oid = self._selected
        key = self.props.item(item.row(), 0).text()
        text = item.text()
        if key == "name" and self.app.kinds()[kind].get("solver_name"):
            from .forms import solver_name_text
            text = solver_name_text(text.strip())
        try:
            value = json.loads(text)
        except json.JSONDecodeError:
            value = text
        self._guard(lambda: self.app.execute(f"{kind}.rename" if key == "name" else f"{kind}.update",
                                             **({"id": oid, "name": text} if key == "name" else {"id": oid, key: value})))

    def _selected_of(self, kinds):
        if not self._selected or self._selected[0] not in kinds:
            raise Nasa95Error("invalid_state", f"먼저 트리에서 {' 또는 '.join(kinds)} 를 선택하세요", {})
        return self._selected[1]

    def _update_legend(self):
        try:
            self.legend.set_legend(self.app.execute("view.diagnostics")["legend"])
        except Nasa95Error:
            self.legend.set_legend(None)

    # ------------------------------------------------------------ 생성·편집 양식(객체 종류 정의에서 자동)
    def _tree_menu(self, pos):
        item = self.tree.itemAt(pos)
        if item is None:
            return
        self.tree.setCurrentItem(item)
        menu = QMenu(self)
        data = item.data(0, Qt.ItemDataRole.UserRole)
        kinds = self.app.kinds()
        if data is None:  # 가지: 이 종류의 최상위 객체 만들기
            kind = item.text(1)
            if not kinds[kind]["parents"]:
                self._add_create_actions(menu, kind, None)
        else:
            kind, oid = data
            menu.addAction("편집...", lambda: self._guard(self._edit_selected))
            menu.addAction("삭제", lambda: self._guard(self._delete_selected))
            menu.addAction("억제/해제", lambda: self._guard(self._toggle_suppress))
            children = [k for k, spec in kinds.items() if kind in spec["parents"]]
            if children:
                menu.addSeparator()
                for child in children:
                    sub = menu.addMenu(kinds[child]["label"] + " 만들기")
                    self._add_create_actions(sub, child, oid)
            ext_items = [it for it in self._ext_tree_actions if not it.get("kind") or it["kind"] == kind]
            if ext_items:
                menu.addSeparator()
                for it in ext_items:
                    menu.addAction(it["label"], lambda it=it, oid=oid: self._guard(lambda: self._ext_run(it, id=oid)))
        if not menu.isEmpty():
            menu.exec(self.tree.viewport().mapToGlobal(pos))

    def _add_create_actions(self, menu, kind, parent_id):
        spec = self.app.kinds()[kind]
        subs = [s["name"] for s in spec["subtypes"]] or [""]
        for sub in subs:
            menu.addAction(sub or (spec["label"] + " 만들기"), lambda k=kind, s=sub, p=parent_id: self._guard(lambda: self._create(k, s, p)))

    def _create(self, kind, sub, parent_id):
        if kind == "material":  # 재료 창: 값 입력 상자(단위 표시) + DB(사용자 요청 2026-10-04)
            from .material_dialog import MaterialDialog
            dlg = MaterialDialog(self.app, parent=self)
        elif kind == "property" and sub == "beam":  # 보 프로퍼티 창: 단면 종류·치수·미리보기·1축 방향(D15)
            from .beam_dialog import BeamDialog
            dlg = BeamDialog(self.app, parent=self, selection=self._picked)
        else:
            dlg = ObjectDialog(self.app, kind, sub, parent_id, parent=self, selection=self._picked)
        if dlg.exec() and dlg.result_id:
            self._selected = (kind, dlg.result_id)
            self.refresh()

    def _new_case(self):
        """새 해석 케이스 창(D14): 해석 종류·메시·하중 셋(계수)·구속 셋을 골라 케이스 + 스텝 + 기본 출력을 만든다."""
        from .case_dialog import CaseDialog
        dlg = CaseDialog(self.app, parent=self)
        if dlg.exec() and dlg.result_id:
            self._selected = ("case", dlg.result_id)
            self.refresh()
            self._case_dialog_finished(dlg)

    def _edit_step_sets(self):
        from .case_dialog import CaseDialog
        self._selected_of(("step", "case"))
        if self._selected[0] == "case":
            dlg = CaseDialog(self.app, parent=self, case=self.app.execute("case.get", id=self._selected[1]))
        else:
            step = self.app.execute("step.get", id=self._selected[1])
            dlg = CaseDialog(self.app, parent=self, step=step)
        if dlg.exec():
            self._selected = ("case", dlg.result_id)
            self.refresh()
            self._case_dialog_finished(dlg)

    def _case_dialog_finished(self, dlg):
        for issue in dlg.validation_issues:
            self.log.appendPlainText(f"케이스 검사 [{issue['severity']}] {issue['message']}")
        if dlg.validation_issues:
            self.log_dock.show()
        if dlg.run_requested:
            self._guard(self._run_case)

    def _material_db(self):
        """리본 '재료 DB': 내장 DB 에서 고른 재료를 바로 추가한다."""
        from .material_dialog import add_from_db
        ids = add_from_db(self.app, self)
        if ids:
            self._selected = ("material", ids[-1])
            self.refresh()
            self.statusBar().showMessage(f"재료 {len(ids)}개를 DB 에서 추가했습니다", 5000)

    def _edit_selected(self):
        if not self._selected:
            raise Nasa95Error("invalid_state", "먼저 트리에서 객체를 선택하세요", {})
        kind, oid = self._selected
        if kind == "case":
            return self._edit_step_sets()
        obj = self.app.execute(f"{kind}.get", id=oid)
        if kind == "material":
            from .material_dialog import MaterialDialog
            dlg = MaterialDialog(self.app, obj=obj, parent=self)
        elif kind == "property" and obj["props"].get("type") == "beam":
            from .beam_dialog import BeamDialog
            dlg = BeamDialog(self.app, obj=obj, parent=self, selection=self._picked)
        else:
            dlg = ObjectDialog(self.app, kind, obj=obj, parent=self, selection=self._picked)
        if dlg.exec():
            self.refresh()

    def _on_hover(self, hit):
        if hit.get("hit"):
            text = {"face": lambda: f"형상 면 {hit['index']} (파트 {hit['part']})", "element_face": lambda: f"요소 {hit['element']} 면 {hit['face']}",
                    "element": lambda: f"요소 {hit['element']}"}[hit["kind"]]()
            self.statusBar().showMessage(text)

    def _on_pick(self, hit):
        if not hit.get("hit"):
            self.statusBar().showMessage("빈 곳" + (f"(필터로 제외: {hit['filtered']})" if hit.get("filtered") else ""), 3000)
            self._picked = []
            self.app.execute("selection.clear")  # 선택은 코어의 상태(selection.*)이고 강조 표시가 따라간다
            self.viewport.present()
            return
        item = {k: hit[k] for k in ("kind", "part", "index", "element", "face") if k in hit}
        from PySide6.QtWidgets import QApplication
        add = bool(QApplication.keyboardModifiers() & Qt.KeyboardModifier.ControlModifier)
        self._picked = self.app.execute("selection.set", items=[item], add=add)["items"]
        self.viewport.present()
        if hit["kind"] == "face":
            self.statusBar().showMessage(f"형상 면 {hit['index']} (파트 {hit['part']})", 5000)
            self._selected = ("part", hit["part"])
            self.refresh()
        elif hit["kind"] == "element_face":
            self.statusBar().showMessage(f"요소 {hit['element']} 의 면 {hit['face']}", 5000)
        else:
            self.statusBar().showMessage(f"요소 {hit['element']}", 5000)

    # ------------------------------------------------------------ 동작
    def _open(self):
        path, _ = QFileDialog.getOpenFileName(self, "프로젝트 열기", "", "NASA-95 프로젝트 (*.nasa95 *.ofep)")
        if path:
            self.app.execute("project.open", path=path)
            self.app.execute("view.fit")

    def _save(self, ask=False):
        path = self.app.execute("project.info").get("path")
        if ask or not path:
            path, _ = QFileDialog.getSaveFileName(self, "프로젝트 저장", "", "NASA-95 프로젝트 (*.nasa95)")
        if path:
            self.app.execute("project.save_as", path=path)

    def _import_geometry(self):
        path, _ = QFileDialog.getOpenFileName(self, "형상 가져오기", "", "CAD (*.step *.stp *.iges *.igs *.brep);;모든 파일 (*)")
        if path:
            r = self.app.execute("geometry.import", path=path)
            self._selected = ("part", r["id"])
            self.app.execute("view.fit")

    def _import_deck(self):
        path, _ = QFileDialog.getOpenFileName(self, "CalculiX 덱 가져오기", "", "CalculiX (*.inp);;모든 파일 (*)")
        if path:
            r = self.app.execute("deck.import", path=path)
            self.app.execute("view.fit")
            if r["preserved"]:
                self._info("보존한 카드", r["preserved"])

    def _screenshot(self):
        path, _ = QFileDialog.getSaveFileName(self, "이미지로 저장", "view.png", "PNG (*.png)")
        if path:
            # 창에서는 HUD 범례를 끄고 Qt 범례를 쓰지만, 파일에는 Qt 위젯이 없으니 이 한 장만 HUD 범례를 넣어 찍는다
            self.app.execute("view.hud", legend=True)
            try:
                self.app.execute("view.screenshot", path=path, width=1920, height=1080)
            finally:
                self.app.execute("view.hud", legend=False)

    def _delete_selected(self):
        if self._selected:
            kind, oid = self._selected
            self._selected = None
            self.app.execute(f"{kind}.delete", id=oid)

    def _toggle_suppress(self):
        if self._selected:
            kind, oid = self._selected
            obj = self.app.execute(f"{kind}.get", id=oid)
            self.app.execute(f"{kind}.{'unsuppress' if obj['suppressed'] else 'suppress'}", id=oid)

    def _make_box(self):
        text, ok = QInputDialog.getText(self, "상자", "크기 x, y, z:", text="100, 20, 10")
        if ok:
            size = [float(v) for v in text.split(",")]
            part = self.app.execute("part.create")["id"]
            self.app.execute("feature.create_box", parent=part, size=size)
            self._selected = ("part", part)
            self.app.execute("view.fit")

    def _make_cylinder(self):
        text, ok = QInputDialog.getText(self, "실린더", "반지름, 높이:", text="10, 50")
        if ok:
            r, h = (float(v) for v in text.split(","))
            part = self.app.execute("part.create")["id"]
            self.app.execute("feature.create_cylinder", parent=part, radius=r, height=h)
            self._selected = ("part", part)
            self.app.execute("view.fit")

    def _mesh_part(self):
        part = self._selected_of(("part",))
        # 메셔는 작업 스레드에서 돌고(아키텍처 규칙 5) 창은 진행률을 보여 주다가 끝나면 결과를 모델에 넣는다
        dlg = CommandDialog(self.app, "mesh.generate", fixed={"id": part, "background": True}, parent=self)
        if not (dlg.exec() and dlg.result):
            return
        self.mesh_job_dialog(part)

    def mesh_job_dialog(self, part: int) -> None:
        """돌고 있는 배경 메싱의 진행 창: 진행률·하는 일, 중지. 끝나면 mesh.job_finish 로 결과를 넣는다."""
        from PySide6.QtWidgets import QProgressDialog
        box = QProgressDialog("메싱 준비 중...", "중지", 0, 100, self)
        box.setWindowTitle("자동 메싱")
        box.setMinimumDuration(0)
        box.setAutoClose(False)
        box.setAutoReset(False)
        timer = QTimer(self)
        self._mesh_job = (box, timer)

        def poll():
            st = self.app.execute("mesh.job_status")
            if st["state"] == "running":
                box.setValue(int(st.get("percent") or 0))
                box.setLabelText(f"{st.get('task') or '메싱'} ... {st['elapsed']:.0f}초")
                return
            timer.stop()
            box.close()
            self._mesh_job = None
            if st["state"] == "done":
                try:
                    r = self.app.execute("mesh.job_finish")
                    self.statusBar().showMessage(f"메싱: 노드 {r['nodes']}, 요소 {r['elements']} ({r['shape']}) {st['elapsed']:.1f}초", 8000)
                except Nasa95Error as e:
                    self._info("메싱 결과를 넣지 못했습니다", {"code": e.code, "message": str(e), "details": e.details})
            elif st["state"] == "cancelled":
                self.statusBar().showMessage("메싱을 멈췄습니다", 5000)
            else:
                self._info("메싱 실패", st.get("error", {}))

        def cancel():
            if timer.isActive():
                self.app.execute("mesh.job_cancel")
                box.setLabelText("멈추는 중...")

        box.canceled.connect(cancel)
        timer.timeout.connect(poll)
        timer.start(100)
        poll()

    def _run_case(self):
        case = self._selected_of(("case", "step"))
        if self._selected[0] == "step":
            case = self.app.execute("step.get", id=case)["parent"]
        r = self.app.execute("case.run", id=case)  # 솔버는 별도 프로세스: 기다리지 않고 상태를 주기적으로 본다
        from PySide6.QtWidgets import QProgressDialog
        box = QProgressDialog("솔버 실행 중...", "중지", 0, 0, self)
        box.setWindowTitle("해석")
        box.setMinimumDuration(0)
        timer = QTimer(self)

        def poll():
            st = self.app.execute("case.run_status", id=case, log_lines=3)
            inc = st["increments"]
            box.setLabelText(f"솔버 실행 중... {st['elapsed']:.0f}초" + (f"  스텝 {inc[-1]['step']} 증분 {inc[-1]['increment']}" if inc else "") +
                             ("\n" + "\n".join(st["log"]) if st["log"] else ""))
            if st["state"] != "running":
                timer.stop()
                box.close()
                if st["state"] == "completed":
                    self.statusBar().showMessage(f"풀이 완료 ({st['elapsed']:.1f}초): {st['work_directory']}", 10000)
                    self._reload_results_of(case)  # 열려 있던 그 케이스의 결과를 다시 읽는다(outdated 해제)
                else:
                    self._info(f"풀이 {st['state']}", {"errors": st["errors"], "log": self.app.execute("case.run_status", id=case, log_lines=30)["log"]})

        def cancel():
            if timer.isActive():
                timer.stop()
                self.app.execute("case.run_stop", id=case)
                self.statusBar().showMessage("풀이를 중지했습니다", 5000)

        box.canceled.connect(cancel)
        timer.timeout.connect(poll)
        timer.start(500)
        poll()

    def _run_status(self):
        case = self._selected_of(("case",))
        self._info("실행 상태", self.app.execute("case.run_status", id=case))

    def _preview_deck(self):
        case = self._selected_of(("case",))
        r = self.app.execute("case.preview_deck", id=case, max_lines=200)
        QMessageBox.information(self, "덱 미리 보기(앞 200줄)", r["text"][:6000] + ("\n..." if r["truncated"] else ""))

    def _open_result(self):
        case = self._selected_of(("case",))
        r = self.app.execute("result.open", case=case)
        self._last_result = r["id"]
        self.statusBar().showMessage(f"결과 열림: 프레임 {r['frames']}개", 8000)
        # 결과 작업 공간으로(D13): 트리·패널이 결과용으로 바뀌고 마지막 프레임을 입힌다
        self.result_ws.select_result(r["id"])
        if self.workspace == "result":
            self.result_ws.refresh()
            self.result_ws.show_last()
            self._result_view_changed()
        else:
            self._raise_tab("결과")  # 탭이 바뀌면 set_workspace("result") 가 따라온다
            self.set_workspace("result")

    def _settings(self):
        """프로그램 설정(CAS-41): app.settings_get/set. 모델이 아니라 사용자 환경에 저장된다."""
        from .forms import SettingsDialog
        SettingsDialog(self.app, parent=self).exec()

    def _command_dialog(self, command: str):
        """명령 양식을 띄워 실행하고 화면을 다시 그린다(뷰 명령용)."""
        dlg = CommandDialog(self.app, command, parent=self)
        if dlg.exec():
            self.viewport.refresh()

    def _show_symbols(self):
        step = None
        if self._selected and self._selected[0] in ("step", "load", "bc"):
            step = self._selected[1] if self._selected[0] == "step" else self.app.execute(f"{self._selected[0]}.get", id=self._selected[1])["parent"]
        else:
            steps = self.app.execute("step.list")
            step = steps[-1]["id"] if steps else None
        if step is None:
            raise Nasa95Error("invalid_state", "스텝이 없습니다", {})
        self._view("view.symbols", step=step)

    def _rest_start(self):
        """REST 서버: 상태 표시줄의 REST 대화 상자를 연다(켜기·끄기·주소·포트·토큰 복사가 거기 있다)."""
        self.rest_button.open_dialog()

    def _hide_result(self):
        self.result_ws.hide_result()

    def _show_contour(self):
        """컨투어 표시: 결과 작업 공간으로 가서 패널의 설정(결과 종류·성분·프레임·변형 배율)을 입힌다(D13)."""
        rid = getattr(self, "_last_result", None)
        if rid is None and not self.result_ws.results():
            raise Nasa95Error("invalid_state", "먼저 결과를 여세요", {})
        if rid is not None:
            self.result_ws.select_result(rid)
        self._raise_tab("결과")
        self.set_workspace("result")
        self.result_ws.apply()

    def closeEvent(self, event):
        self._save_workspace()
        self._closed = True
        self.app.unsubscribe(self._event_token)
        self.viewport.closeEvent(event)
        super().closeEvent(event)


def run(argv=None) -> int:
    """UI 실행. 옵션(API-38, 창과 함께 서버): --server [--port N] [--host H] [--token T | --token-file F] [--allow-script] [--project 경로]
    --server 를 주면 창이 뜬 뒤 REST 서버를 켜고 토큰을 표준 출력(과 --token-file)에 적는다. 외부 프로그램(또는 에이전트)이 그 토큰으로
    명령을 보내면 GUI 스레드에서 실행되어 창(트리·뷰포트)에 바로 보인다."""
    import argparse
    import sys
    from PySide6.QtWidgets import QApplication
    parser = argparse.ArgumentParser(prog="nasa95.ui", add_help=True)
    parser.add_argument("--server", action="store_true", help="창과 함께 REST 서버를 켠다")
    parser.add_argument("--host", default=None), parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--token", default=None, help="접속 토큰(없으면 만든다)")
    parser.add_argument("--token-file", default=None, help="토큰을 이 파일에 적는다")
    parser.add_argument("--allow-script", action="store_true", help="REST 로 script.run 허용")
    parser.add_argument("--project", default=None, help="시작할 때 열 프로젝트 파일")
    opts, rest = parser.parse_known_args(list(argv or sys.argv)[1:])
    qt = QApplication.instance() or QApplication([sys.argv[0]] + rest)
    ccx = pathlib.Path(__file__).resolve().parents[4] / "third_party" / "calculix"
    if "NASA95_CCX" not in os.environ and ccx.exists():
        hits = sorted(ccx.rglob("ccx_static.exe"))
        if hits:
            os.environ["NASA95_CCX"] = str(hits[0])
    window = MainWindow()
    window.show()
    if opts.project:
        window.app.execute("project.open", path=opts.project)
        window.refresh()
    if opts.server:
        cfg = {}
        if opts.host:
            cfg["host"] = opts.host
        if opts.port is not None:
            cfg["port"] = opts.port
        if opts.allow_script:
            cfg["allow_script"] = True
        if cfg:
            window.app.execute("server.configure", **cfg)
        if opts.token:  # 정해진 토큰: 서버가 임의 토큰을 만들기 전에 등록한다
            window.app.rest.tokens[opts.token] = "full"
        st = window.app.execute("server.start")
        token = opts.token or next(iter(window.app.rest.tokens))
        if opts.token_file:
            pathlib.Path(opts.token_file).write_text(token, encoding="utf-8")
        print(f"REST {st['url']} token={token}", flush=True)
        window.rest_button.refresh()  # 상태 표시줄 맨 앞 버튼이 주소를 보인다(임시 메시지는 버튼을 가리므로 쓰지 않는다)
    return qt.exec()
