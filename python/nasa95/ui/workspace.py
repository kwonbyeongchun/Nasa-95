"""주 창의 리본·탐색기·테마 구성. 제품 명령은 기존 App을 통해 실행한다."""
from PySide6.QtCore import QSize, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QColor, QKeySequence
from PySide6.QtWidgets import (QDockWidget, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
                               QMenu, QPlainTextEdit, QStackedWidget, QToolButton, QTreeWidgetItemIterator, QVBoxLayout, QWidget)

from ..api import Nasa95Error
from .icons import icon
from .result_workspace import ResultWorkspace
from .ribbon import Ribbon, clean


class Workspace:
    def _setup_workspace(self, view_area):
        editor = QWidget()
        editor.setObjectName("editor")
        editor_column = QVBoxLayout(editor)
        editor_column.setContentsMargins(0, 0, 0, 0)
        editor_column.setSpacing(0)
        header = QWidget()
        header.setObjectName("editorHeader")
        header_row = QHBoxLayout(header)
        header_row.setContentsMargins(0, 0, 6, 0)
        tab = QLabel("3D 뷰")
        tab.setObjectName("editorTab")
        header_row.addWidget(tab)
        header_row.addStretch()
        self._view_buttons = []
        for label, symbol, command, params in (("전체 맞춤", "fit", "view.fit", {}),
                                               ("등각 뷰", "box", "view.standard", {"name": "iso"})):
            button = QToolButton()
            button.setToolTip(label)
            button.setAccessibleName(label)
            button.clicked.connect(lambda _checked=False, c=command, p=params: self._guard(lambda: self._view(c, **p)))
            header_row.addWidget(button)
            self._view_buttons.append((button, symbol))
        # 영역 확대: 누른 뒤 뷰포트에서 끌어 고른 사각형이 화면에 꽉 차게(view.zoom_region). 한 번 쓰면 풀린다
        zoom_region = QToolButton()
        zoom_region.setToolTip("영역 확대 — 누르고 화면에서 끌어 영역을 고른다")
        zoom_region.setAccessibleName("영역 확대")
        zoom_region.setCheckable(True)
        zoom_region.toggled.connect(lambda on: self.viewport.set_zoom_region_mode(on))
        self.viewport.zoom_region_done.connect(lambda: zoom_region.setChecked(False))
        header_row.addWidget(zoom_region)
        self._view_buttons.append((zoom_region, "zoom_region"))
        editor_column.addWidget(header)
        self.breadcrumbs = QLabel("프로젝트  ›  3D 뷰")
        self.breadcrumbs.setObjectName("breadcrumbs")
        editor_column.addWidget(self.breadcrumbs)
        editor_column.addWidget(view_area, 1)
        self.setCentralWidget(editor)
        self.tree.setObjectName("modelTree")
        self.tree.setColumnCount(3)
        self.tree.setHeaderLabels(["모델", "종류", "표시"])
        self.tree.setColumnHidden(1, True)
        self.tree.setHeaderHidden(True)
        self.tree.setIndentation(14)
        self.tree.setIconSize(QSize(16, 16))
        self.tree.setUniformRowHeights(True)
        self.tree.setAnimated(False)
        self.tree.header().setStretchLastSection(False)
        self.tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.tree.header().setSectionResizeMode(2, QHeaderView.ResizeMode.Fixed)
        self.tree.setColumnWidth(2, 26)
        self.tree.itemClicked.connect(self._visibility_clicked)
        self.tree_search = QLineEdit()
        self.tree_search.setPlaceholderText("모델 검색")
        self.tree_search.setClearButtonEnabled(True)
        self.tree_search.setAccessibleName("모델 검색")
        self.tree_search.textChanged.connect(self._filter_tree)
        panel = QWidget()
        panel.setObjectName("treePanel")
        column = QVBoxLayout(panel)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(0)
        search_row = QHBoxLayout()
        search_row.setContentsMargins(12, 8, 12, 6)
        search_row.addWidget(self.tree_search)
        column.addLayout(search_row)
        tools = QHBoxLayout()
        tools.setContentsMargins(12, 0, 8, 2)
        hint = QLabel("프로젝트")
        tools.addWidget(hint)
        tools.addStretch()
        self._tree_tools = []
        for text, symbol, callback in (("모두 펼치기", "expand_all", self.tree.expandAll), ("모두 접기", "collapse_all", self.tree.collapseAll)):
            button = QToolButton()
            button.setToolTip(text)
            button.setAccessibleName(text)
            button.setFixedSize(24, 24)
            button.clicked.connect(callback)
            tools.addWidget(button)
            self._tree_tools.append((button, symbol))
        self.tree_filter_button = QToolButton()
        self.tree_filter_button.setText("빈 항목")
        self.tree_filter_button.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonIconOnly)
        self.tree_filter_button.setCheckable(True)
        self.tree_filter_button.setToolTip("객체가 없는 종류도 표시")
        self.tree_filter_button.setChecked(self.theme.settings.value("show_empty", False, type=bool))
        self.tree_filter_button.toggled.connect(self._filter_tree)
        tools.addWidget(self.tree_filter_button)
        self._tree_tools.append((self.tree_filter_button, "filter"))
        column.addLayout(tools)
        column.addWidget(self.tree, 1)
        self.tree_empty = QLabel("일치하는 항목이 없습니다.")
        self.tree_empty.setProperty("role", "muted")
        self.tree_empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        column.addWidget(self.tree_empty)
        # 작업 공간(D13): 탐색기와 속성 패널은 모델용·결과용을 겹쳐 두고 전환한다(결과 쪽은 result_workspace.py)
        self.result_ws = ResultWorkspace(self.app, self)
        self.result_ws.changed.connect(self._result_view_changed)
        self.explorer_stack = QStackedWidget()
        self.explorer_stack.addWidget(panel)
        self.explorer_stack.addWidget(self.result_ws.tree_panel)
        self.model_dock = QDockWidget("탐색기", self)
        self.model_dock.setObjectName("modelDock")
        self.model_dock.setMinimumWidth(230)
        self.model_dock.setWidget(self.explorer_stack)
        self.addDockWidget(Qt.DockWidgetArea.LeftDockWidgetArea, self.model_dock)
        self.props.setObjectName("propertyTable")
        self.props.setShowGrid(False)
        self.props.setAlternatingRowColors(False)
        self.props.verticalHeader().hide()
        self.props.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.props.verticalHeader().setDefaultSectionSize(24)
        properties = QWidget()
        properties.setObjectName("propertyPanel")
        col = QVBoxLayout(properties)
        col.setContentsMargins(12, 4, 12, 8)
        self.selection_title = QLabel("선택한 객체 없음")
        self.selection_title.setObjectName("selectionTitle")
        self.selection_title.setWordWrap(True)
        self.selection_hint = QLabel("트리 또는 화면에서 객체를 선택하세요.")
        self.selection_hint.setProperty("role", "muted")
        self.selection_hint.setWordWrap(True)
        col.addWidget(self.selection_title)
        col.addWidget(self.selection_hint)
        col.addWidget(self.props, 1)
        self.property_stack = QStackedWidget()
        self.property_stack.addWidget(properties)
        self.property_stack.addWidget(self.result_ws.controls)
        self.property_dock = QDockWidget("속성", self)
        self.property_dock.setObjectName("propertyDock")
        self.property_dock.setMinimumWidth(230)
        self.property_dock.setWidget(self.property_stack)
        self.workspace = "model"
        self._hidden_for_result: list[QDockWidget] = []
        self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, self.property_dock)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(1000)
        self.log.setPlaceholderText("작업 메시지와 오류가 여기에 표시됩니다.")
        self.log_dock = QDockWidget("작업 메시지", self)
        self.log_dock.setObjectName("messageDock")
        self.log_dock.setWidget(self.log)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, self.log_dock)
        self.log_dock.hide()
        self.resizeDocks([self.model_dock, self.property_dock], [256, 256], Qt.Orientation.Horizontal)
        self._tree_expanded = {}
        self._search_expanded = None
        self._closed = False

    def _new_action(self, label, callback, kinds=None):
        action = QAction(label, self)
        action.triggered.connect(lambda: self._guard(callback))
        if kinds:
            self._selection_actions.append((action, kinds))
        self.addAction(action)
        return action

    def _build_ribbon(self):
        self._selection_actions = []
        self.ribbon = Ribbon(self)
        # 기존 메뉴는 확장 API 호환을 위해 보관하고, 리본의 드롭다운에서 접근한다.
        self._legacy_menu = self.menuBar()
        self._legacy_menu.hide()
        menus = {}
        self._menu_actions = self._legacy_menu.actions()
        for action in self._menu_actions:
            menu = action.menu()
            if menu is not None:
                menus[clean(action.text())] = menu
        self._ribbon_menus = menus
        self.ribbon.file_button.setMenu(menus["파일"])
        actions = {clean(a.text()): a for menu in menus.values() for a in menu.actions() if not a.isSeparator()}
        self._ribbon_actions = actions
        for action in actions.values():
            self.addAction(action)  # 숨겨진 메뉴와 무관하게 단축키를 유지한다.
            self.ribbon.search_actions[clean(action.text())] = action
        for name, symbol in (("저장", "save"), ("실행 취소", "undo"), ("다시 실행", "redo")):
            self.ribbon.action(self.ribbon.quick, actions[name], symbol, quick=True)

        groups = [
            ("홈", "프로젝트", [("열기", "open", "열기"), ("저장", "save", "저장")]),
            ("홈", "편집", [("실행 취소", "undo", "실행 취소"), ("다시 실행", "redo", "다시 실행"), ("선택 객체 편집", "edit", "속성 편집")]),
            ("홈", "화면", [("전체 맞춤", "fit", "전체 맞춤"), ("등각 뷰", "box", "등각")]),
            ("형상", "기본 형상", [("상자", "box", "상자"), ("실린더", "box", "실린더"), ("형상 가져오기(STEP·IGES·BREP)", "open", "가져오기")]),
            ("메시", "자동 메싱", [("선택한 파트 자동 메싱", "mesh", "자동 메싱"), ("메시 상태", "check", "메시 상태")]),
            ("해석", "실행", [("선택한 케이스 실행", "run", "해석 실행"), ("실행 상태", "property", "실행 상태"), ("덱 미리 보기", "property", "입력 파일")]),
            ("결과", "결과 표시", [("선택한 케이스의 결과 열기", "open", "결과 열기"), ("컨투어 표시", "result", "컨투어"), ("결과 표시 끄기", "eye_off", "표시 끄기")]),
            ("보기", "카메라", [("전체 맞춤", "fit", "전체 맞춤"), ("등각 뷰", "box", "등각"), ("정면 뷰", "box", "정면"), ("평면 뷰", "box", "평면")]),
        ]
        # 탭 순서는 작업 흐름과 같다.
        for name in ("홈", "형상", "메시", "모델", "해석", "결과", "보기"):
            self.ribbon.page(name)
        deferred = []  # 해석 탭의 흐름(D14): 하중 셋 → 구속 셋 → 해석 케이스(셋을 고른다) → 실행. 실행 패널은 만들기 패널들 뒤에 둔다
        for tab, caption, entries in groups:
            if tab == "해석" and caption == "실행":
                deferred.append((tab, caption, entries))
                continue
            row = self.ribbon.group(tab, caption)
            for name, symbol, short in entries:
                if name in actions:
                    self.ribbon.action(row, actions[name], symbol, short)
        self._label_actions = {}
        numbers = self.ribbon.group("보기", "번호 표시")
        for kind, label, symbol in (("nodes", "절점 번호", "node_labels"), ("elements", "요소 번호", "element_labels")):
            action = QAction(label, self)
            action.setCheckable(True)
            action.triggered.connect(lambda checked, k=kind: self._guard(lambda: self._set_number_labels(k, checked)))
            self._label_actions[kind] = action
            menus["보기"].addAction(action)
            button = self.ribbon.action(numbers, action, symbol)
            button.setToolTip(f"{label} 표시 켜기/끄기 · 화면에 최대 5,000개, 두 종류를 함께 켜면 각각 2,500개")
        self.viewport.presented.connect(self._sync_label_actions)
        self._shrink_ratio = self.theme.settings.value("mesh_shrink_ratio", 0.2, type=float)
        if not 0 < self._shrink_ratio <= 0.9:
            self._shrink_ratio = 0.2
        self._shrink_action = QAction("요소 축소 (Shrink)", self)
        self._shrink_action.setCheckable(True)
        self._shrink_action.triggered.connect(
            lambda checked: self._guard(lambda: self._set_mesh_shrink(self._shrink_ratio if checked else 0.0)))
        shrink_menu = QMenu("축소율", self)
        shrink_group = QActionGroup(shrink_menu)
        self._shrink_presets = {}
        for percent in range(10, 100, 10):
            preset = shrink_menu.addAction(f"{percent}% 축소 · 원래 크기의 {100 - percent}%")
            preset.setCheckable(True)
            shrink_group.addAction(preset)
            preset.triggered.connect(lambda _checked=False, p=percent: self._guard(lambda: self._set_mesh_shrink(p / 100.0)))
            self._shrink_presets[percent] = preset
        self._shrink_action.setMenu(shrink_menu)
        menus["보기"].addAction(self._shrink_action)
        self._shrink_buttons = []
        for tab in ("메시", "보기"):
            button = self.ribbon.action(self.ribbon.group(tab, "메시 표시"), self._shrink_action, "shrink", "요소 축소")
            button.setPopupMode(QToolButton.ToolButtonPopupMode.MenuButtonPopup)
            self._shrink_buttons.append(button)
        self.viewport.presented.connect(self._sync_mesh_shrink)
        # 보·쉘 입체 표시(RND-21·22): 1D 요소는 단면, 2D 요소는 두께가 있는 입체로. 기본 끔(선·면으로 본다)
        self._solid_action = QAction("보·쉘 입체 표시", self)
        self._solid_action.setCheckable(True)
        self._solid_action.triggered.connect(lambda checked: self._guard(lambda: self._set_mesh_solid(checked)))
        menus["보기"].addAction(self._solid_action)
        for tab in ("메시", "보기"):
            button = self.ribbon.action(self.ribbon.group(tab, "메시 표시"), self._solid_action, "solid_section", "보·쉘 입체")
            button.setToolTip("보·쉘 입체 표시 켜기/끄기 · 1D 요소는 프로퍼티 단면, 2D 요소는 두께(오프셋 반영)로 그린다. 끄면 선·면으로 본다")
        self.viewport.presented.connect(self._sync_mesh_solid)
        # 보 1축 방향 표식(PRP-07): 요소마다 단면 1축 쪽 주황 선. 방향을 안 준 보는 솔버 기본(−Z)으로 보인다
        self._axes_action = QAction("보 1축 방향 표시", self)
        self._axes_action.setCheckable(True)
        self._axes_action.triggered.connect(lambda checked: self._guard(lambda: self._set_beam_axes(checked)))
        menus["보기"].addAction(self._axes_action)
        for tab in ("메시", "보기"):
            button = self.ribbon.action(self.ribbon.group(tab, "메시 표시"), self._axes_action, "beam_axis", "보 1축")
            button.setToolTip("보 요소의 단면 1축(형강은 웨브) 방향을 주황 선으로 표시 · 프로퍼티·요소별 방향, 없으면 솔버 기본 −Z")
        self.viewport.presented.connect(self._sync_beam_axes)
        for name, kinds in (("선택한 파트 자동 메싱", ("part",)), ("선택한 케이스 실행", ("case",)),
                            ("선택한 케이스의 결과 열기", ("case",)), ("덱 미리 보기", ("case",))):
            if name in actions:
                self._selection_actions.append((actions[name], kinds))
        self._create_buttons = []
        for tab, caption, kinds in (("모델", "속성 정의", ("material", "property", "csys", "set")),
                                     ("모델", "접촉·연결", ("constraint", "contact_property", "contact_pair")),
                                     ("해석", "하중 셋", ("load_set", "load")),  # 하중은 하중 셋 안에(D14)
                                     ("해석", "구속 셋", ("bc_set", "bc")),       # 구속은 구속 셋 안에
                                     ("해석", "해석 케이스", ("step",)),          # 케이스는 아래 '새 해석 케이스' 창으로(셋을 고른다); 스텝 추가는 여기
                                     ("형상", "피처", ("sketch", "feature"))):
            row = self.ribbon.group(tab, caption)
            if caption == "해석 케이스":
                new_case = self._new_action("새 해석 케이스(해석 종류·메시·하중 셋·구속 셋 선택)", self._new_case)
                new_case.setIconText("해석 케이스")
                self.ribbon.action(row, new_case, "run", "해석 케이스")
                edit_sets = self._new_action("선택한 스텝의 셋·메시 범위 편집", self._edit_step_sets, ("step", "case"))
                edit_sets.setIconText("셋 편집")
                self.ribbon.action(row, edit_sets, "property", "셋 편집")
            for kind in kinds:
                if kind not in self.app.kinds():
                    continue
                spec = self.app.kinds()[kind]
                menu = QMenu(spec["label"], self)
                menu.aboutToShow.connect(lambda m=menu, k=kind: self._populate_create_menu(m, k))
                button = self.ribbon.menu(row, menu, kind, spec["label"])
                self._create_buttons.append((button, spec["parents"]))
        for tab, caption, entries in deferred:
            row = self.ribbon.group(tab, caption)
            for name, symbol, short in entries:
                if name in actions:
                    self.ribbon.action(row, actions[name], symbol, short)
        db_action = self._new_action("재료 DB에서 추가", self._material_db)
        db_action.setIconText("재료 DB")
        self.ribbon.action(self.ribbon.group("모델", "재료 DB"), db_action, "material", "재료 DB")
        # 하중·구속 심볼 토글(해석 탭): 켜 두면 트리에서 고른 스텝(또는 하중·구속의 스텝)의 심볼을 보인다
        self.symbols_action = self._new_action("하중·구속 표시", self._toggle_symbols)
        self.symbols_action.setCheckable(True)
        self.symbols_action.setToolTip("켜면 선택한 스텝(하중·구속을 고르면 그 스텝)의 하중·경계조건 심볼을 화면에 그린다. 결과 공간에서는 꺼지고 돌아오면 복원")
        self.ribbon.action(self.ribbon.group("해석", "표시"), self.symbols_action, "load", "하중·구속")
        runrow = self.ribbon.group("해석", "검사·중지")
        for title, command, symbol in (("사전 검사", "case.check", "check"), ("해석 중지", "case.run_stop", "stop")):
            callback = lambda c=command: self._info("해석", self.app.execute(c, id=self._selected_of(["case"])))
            self.ribbon.action(runrow, self._new_action(title, callback, ("case",)), symbol)
        for tab, menu_name, symbol in (("보기", "보기", "eye"), ("메시", "메시", "mesh"), ("결과", "결과", "result")):
            self.ribbon.menu(self.ribbon.group(tab, "추가 명령"), menus[menu_name], symbol, "모든 명령")
        self.ribbon.menu(self.ribbon.group("홈", "도구"), menus["도구"], "settings", "설정·연동")
        show = self._new_action("모두 표시", lambda: self._view("view.show_all"))
        self.ribbon.action(self.ribbon.group("보기", "표시"), show, "eye")
        self._extension_group = self.ribbon.group("홈", "확장")
        self._extension_button = self.ribbon.menu(self._extension_group, self._make_extension_menu(), "set", "확장 명령")
        self._build_appearance_menu()
        self._build_status_bar()
        self.ribbon.tabs.currentChanged.connect(self._ribbon_tab_changed)  # 결과 탭 ↔ 결과 작업 공간(D13)
        command_search = self._new_action("명령 검색", self.ribbon.focus_search)
        command_search.setShortcut(QKeySequence("Ctrl+Shift+P"))
        self.ribbon.update_search()
        self.ribbon.set_collapsed(self.theme.settings.value("ribbon_collapsed", False, type=bool))
        self.ribbon.collapsedChanged.connect(lambda value: self.theme.settings.setValue("ribbon_collapsed", value))
        self.theme.changed.connect(self._theme_changed)
        self._theme_changed()
        saved = self.theme.settings.value("dock_state_saribbon")
        if saved:
            self.restoreState(saved)

    def _make_extension_menu(self):
        menu = QMenu("확장", self)
        def populate():
            menu.clear()
            self._menu_actions = self._legacy_menu.actions()
            for action in self._menu_actions:
                source = action.menu()
                if source and clean(source.title()) not in ("파일", "편집", "형상", "메시", "해석", "결과", "보기", "도구"):
                    menu.addMenu(source)
            if menu.isEmpty():
                empty = menu.addAction("등록된 확장 메뉴가 없습니다")
                empty.setEnabled(False)
        menu.aboutToShow.connect(populate)
        return menu

    def _populate_create_menu(self, menu, kind):
        menu.clear()
        parents = self.app.kinds()[kind]["parents"]
        parent_id = self._parent_for(kind)
        if parents and parent_id is None:
            labels = " 또는 ".join(self.app.kinds()[k]["label"] for k in parents if k in self.app.kinds())
            menu.addAction(f"트리에서 {labels}을(를) 먼저 선택하세요").setEnabled(False)
            return
        self._add_create_actions(menu, kind, parent_id)

    def _parent_for(self, kind: str):
        """새 객체의 부모: 트리 선택이 부모 종류면 그것, 선택이 그 부모의 자식(예: 하중 셋 안의 하중)이면 그 부모, 하중·구속은 셋이 하나뿐이면 그 셋."""
        parents = self.app.kinds()[kind]["parents"]
        if not parents:
            return None
        if self._selected:
            if self._selected[0] in parents:
                return self._selected[1]
            try:
                obj = self.app.execute(f"{self._selected[0]}.get", id=self._selected[1])
                if obj.get("parent"):
                    for k in self.app.kinds()[self._selected[0]].get("parents", []):  # 선택한 것의 부모가 새 객체의 부모 종류면 그것
                        if k in parents:
                            try:
                                self.app.execute(f"{k}.get", id=obj["parent"])
                                return obj["parent"]
                            except Nasa95Error:
                                continue
            except Nasa95Error:
                pass
        set_kind = {"load": "load_set", "bc": "bc_set"}.get(kind)
        if set_kind and set_kind in parents:
            try:
                sets = self.app.execute(f"{set_kind}.list")
                if len(sets) == 1:
                    return sets[0]["id"]
            except Nasa95Error:
                pass
        return None

    def _build_appearance_menu(self):
        menu = QMenu("화면 설정", self)
        group = QActionGroup(menu)
        group.setExclusive(True)
        for label, mode in (("Light Modern · 라이트", "light"), ("Dark Modern · 다크", "dark"), ("시스템 설정 따르기", "system")):
            action = menu.addAction(label)
            action.setCheckable(True)
            action.setChecked(self.theme.mode == mode)
            group.addAction(action)
            action.triggered.connect(lambda _checked=False, m=mode: self.theme.set_mode(m))
        menu.addSeparator()
        compact = menu.addAction("조밀한 트리")
        compact.setCheckable(True)
        compact.setChecked(self.theme.compact)
        compact.toggled.connect(self.theme.set_compact)
        menu.addSeparator()
        for dock in (self.model_dock, self.property_dock, self.log_dock):
            menu.addAction(dock.toggleViewAction())
        menu.addAction("기본 패널 배치로 복원", self._reset_layout)
        background = menu.addMenu("3D 배경")
        backgrounds = QActionGroup(background)
        for label, value in (("테마 따르기", "theme"), ("흰색", "white"), ("밝은 회색", "gray")):
            action = background.addAction(label)
            action.setCheckable(True)
            action.setChecked(self.theme.settings.value("viewport_background", "theme") == value)
            backgrounds.addAction(action)
            action.triggered.connect(lambda _checked=False, v=value: self._set_view_background(v))
        self.ribbon.options.setMenu(menu)

    def _build_status_bar(self):
        status = self.statusBar()
        status.setSizeGripEnabled(False)
        self.outdated_label = QLabel("")
        self.outdated_label.setObjectName("statusOutdated")
        self.outdated_label.setStyleSheet("color: #D08A00; font-weight: 600; padding: 0 8px;")
        self.outdated_label.hide()
        status.addPermanentWidget(self.outdated_label)
        # 오른쪽: REST ON/OFF(사용자 요청 2026-10-05) — 클릭하면 켜지고 꺼지며, 켜지면 주소·토큰 대화 상자(복사용)
        from .rest_dialog import RestStatusButton
        self.rest_button = RestStatusButton(self.app)
        status.addPermanentWidget(self.rest_button)
        self.workspace_label = QLabel("모델")
        self.workspace_label.setToolTip("작업 공간: 리본의 결과 탭 → 결과, 형상·메시·모델·해석 탭 → 모델")
        status.addPermanentWidget(self.workspace_label)
        mark = QLabel("NASA-95")
        mark.setObjectName("statusMark")
        status.addPermanentWidget(mark)
        status.addPermanentWidget(QLabel("Vulkan"))

    def _set_view_background(self, value):
        self.theme.settings.setValue("viewport_background", value)
        self._apply_view_background()

    def _apply_view_background(self):
        mode = self.theme.settings.value("viewport_background", "theme")
        color = QColor({"white": "#FFFFFF", "gray": "#EEEEEE"}.get(mode, self.theme.colors["viewport"]))
        try:
            self.app.execute("view.overlay", background=[color.red(), color.green(), color.blue()])
            self.viewport.refresh()
        except Nasa95Error:
            pass  # 렌더러 없는 빌드에서도 탐색기·명령을 사용할 수 있다.

    def _reset_layout(self):
        for dock, area in ((self.model_dock, Qt.DockWidgetArea.LeftDockWidgetArea),
                           (self.property_dock, Qt.DockWidgetArea.RightDockWidgetArea),
                           (self.log_dock, Qt.DockWidgetArea.BottomDockWidgetArea)):
            dock.setFloating(False)
            self.addDockWidget(area, dock)
        self.model_dock.show()
        self.property_dock.show()
        self.log_dock.hide()
        self.resizeDocks([self.model_dock, self.property_dock], [256, 256], Qt.Orientation.Horizontal)

    def _theme_changed(self):
        self.ribbon.recolor(self.theme.colors["text"])
        for button, symbol in self._tree_tools + self._view_buttons:
            button.setIcon(icon(symbol, self.theme.colors["muted"]))
        self.tree_search.setStyleSheet("")
        self._decorate_tree()
        self.legend.update()
        self._apply_view_background()
        self.ribbon.native.setWindowTitleBackgroundBrush(QColor(self.theme.colors["panel"]))
        self.ribbon.native.setTabBarBaseLineColor(QColor(self.theme.colors["border"]))
        self.setWindowIcon(icon("box", self.theme.colors["blue"]))

    def _items(self):
        it = QTreeWidgetItemIterator(self.tree)
        while it.value():
            yield it.value()
            it += 1

    def _tree_key(self, item):
        data = item.data(0, Qt.ItemDataRole.UserRole)
        return tuple(data) if data else ("branch", item.text(1))

    def _capture_tree(self):
        if not self.tree_search.text():
            for item in self._items():
                self._tree_expanded[self._tree_key(item)] = item.isExpanded()
        return self.tree.verticalScrollBar().value()

    def _restore_tree(self, scroll):
        for item in self._items():
            key = self._tree_key(item)
            if key in self._tree_expanded:
                item.setExpanded(self._tree_expanded[key])
        self._decorate_tree()
        self._filter_tree()
        self.tree.verticalScrollBar().setValue(scroll)
        self._update_actions()

    def _decorate_tree(self):
        colors = self.theme.colors
        try:
            diagnostics = self.app.execute("view.diagnostics")
            # 실행 중인 이전 빌드와도 호환: 빈 ID 목록은 표시 상태를 바꾸지 않고 현재 목록을 돌려준다.
            hidden = set(diagnostics["hidden"] if "hidden" in diagnostics else self.app.execute("view.show", ids=[])["hidden"])
        except Nasa95Error:
            hidden = set()
        for item in self._items():
            data = item.data(0, Qt.ItemDataRole.UserRole)
            kind = data[0] if data else item.text(1)
            muted = bool(item.data(0, Qt.ItemDataRole.UserRole + 1))
            tone = {"part": "blue", "feature": "blue", "mesh_part": "purple", "material": "orange",
                    "case": "green", "step": "green", "result_file": "purple", "load": "orange",
                    "bc": "blue", "sketch": "orange"}.get(kind, "muted")
            item.setIcon(0, icon(kind, colors["disabled"] if muted else colors[tone]))
            item.setForeground(0, QColor(colors["disabled"] if muted else colors["text"]))
            font = item.font(0)
            font.setBold(False)
            item.setFont(0, font)
            label = self.app.kinds().get(kind, {}).get("label", kind)
            item.setToolTip(0, f"{item.text(0)}\n{label}" + (f" · ID {data[1]}" if data else ""))
            if data and kind in ("part", "mesh_part", "load_set", "bc_set"):
                visible = data[1] not in hidden
                item.setIcon(2, icon("eye" if visible else "eye_off", colors["muted"]))
                item.setData(2, Qt.ItemDataRole.UserRole, visible)
                if kind in ("load_set", "bc_set"):
                    item.setToolTip(2, "이 셋의 하중·구속 심볼을 화면에서 숨기기" if visible else "이 셋의 심볼을 화면에 표시")
                else:
                    item.setToolTip(2, "화면에서 숨기기 (해석 억제와 별개)" if visible else "화면에 표시")

    def _filter_tree(self, *_):
        text = self.tree_search.text().strip().casefold()
        if text and self._search_expanded is None:
            self._search_expanded = {self._tree_key(i): i.isExpanded() for i in self._items()}
        def visit(item, parent_match=False):
            data = item.data(0, Qt.ItemDataRole.UserRole)
            haystack = item.toolTip(0) + " " + item.text(1)
            match = bool(text) and text in haystack.casefold()
            child_match = False
            for index in range(item.childCount()):
                child_match = visit(item.child(index), parent_match or match) or child_match
            visible = parent_match or match or child_match if text else bool(data or item.childCount() or self.tree_filter_button.isChecked())
            item.setHidden(not visible)
            if text and child_match:
                item.setExpanded(True)
            return visible
        shown = 0
        for index in range(self.tree.topLevelItemCount()):
            shown += visit(self.tree.topLevelItem(index))
        self.tree_empty.setVisible(not shown)
        if not text and self._search_expanded is not None:
            for item in self._items():
                if self._tree_key(item) in self._search_expanded:
                    item.setExpanded(self._search_expanded[self._tree_key(item)])
            self._search_expanded = None

    def _visibility_clicked(self, item, column):
        data = item.data(0, Qt.ItemDataRole.UserRole)
        if column != 2 or not data or data[0] not in ("part", "mesh_part", "load_set", "bc_set"):
            return
        visible = item.data(2, Qt.ItemDataRole.UserRole)
        command = "view.hide" if visible else "view.show"

        def toggle():
            self.app.execute(command, ids=[data[1]])
            if data[0] in ("load_set", "bc_set") and not visible:
                # 셋을 켰는데 심볼이 꺼져 있으면 보이는 셋들의 심볼을 바로 그린다(스텝 선택 없이)
                try:
                    shown = self.app.execute("view.diagnostics").get("symbols", False)
                except Nasa95Error:
                    shown = False
                if not shown:
                    hidden = set(self.app.execute("view.diagnostics")["hidden"])
                    sets = [x["id"] for k in ("load_set", "bc_set") for x in self.app.execute(f"{k}.list") if x["id"] not in hidden]
                    self.app.execute("view.symbols", sets=sets)
                    if getattr(self, "symbols_action", None):
                        self.symbols_action.blockSignals(True)
                        self.symbols_action.setChecked(True)
                        self.symbols_action.blockSignals(False)
            self.viewport.present()
        self._guard(toggle)
        self._decorate_tree()

    def _set_number_labels(self, kind, checked):
        try:
            self._view("view.labels", **{"all_" + kind: checked})
        finally:
            self._sync_label_actions()

    def _sync_label_actions(self):
        state = self.app.execute("view.labels_get")
        for kind, action in self._label_actions.items():
            action.setChecked(state[kind])

    def _set_mesh_shrink(self, ratio):
        try:
            self._view("view.mesh_options", shrink=ratio)
        finally:
            self._sync_mesh_shrink()

    def _sync_mesh_shrink(self):
        ratio = self.app.execute("view.mesh_options_get")["shrink"]
        self._shrink_action.setChecked(ratio > 0)
        if ratio > 0 and ratio != self._shrink_ratio:
            self._shrink_ratio = ratio
            self.theme.settings.setValue("mesh_shrink_ratio", ratio)
        percent = self._shrink_ratio * 100
        for value, action in self._shrink_presets.items():
            action.setChecked(abs(value - percent) < 1e-6)
        for button in self._shrink_buttons:
            button.setToolTip(f"Mesh Shrink · {percent:g}% 축소 켜기/끄기\n화살표: 축소율 변경 · 실제 절점 좌표는 유지")

    def _set_mesh_solid(self, on):
        try:
            self._view("view.mesh_options", solid_1d_2d=on)
        finally:
            self._sync_mesh_solid()

    def _sync_mesh_solid(self):
        self._solid_action.setChecked(self.app.execute("view.mesh_options_get")["solid_1d_2d"])

    def _set_beam_axes(self, on):
        try:
            self._view("view.mesh_options", beam_axes=on)
        finally:
            self._sync_beam_axes()

    def _sync_beam_axes(self):
        self._axes_action.setChecked(self.app.execute("view.mesh_options_get").get("beam_axes", False))

    def _update_actions(self):
        if not hasattr(self, "ribbon"):
            return
        self._sync_label_actions()
        self._sync_mesh_shrink()
        self._sync_mesh_solid()
        self._sync_beam_axes()
        for action, kinds in self._selection_actions:
            action.setEnabled(bool(self._selected and self._selected[0] in kinds))
        for button, parents in self._create_buttons:
            kind = next((k for k, spec in self.app.kinds().items() if spec["label"] == button.text()), None)
            enabled = not parents or (kind is not None and self._parent_for(kind) is not None)
            button.setEnabled(enabled)
            button.setToolTip(button.text() + " 만들기" if enabled else "먼저 " + " 또는 ".join(self.app.kinds()[k]["label"] for k in parents if k in self.app.kinds()) + "을(를) 트리에서 선택하세요")
        for name in ("선택 객체 편집", "선택 객체 삭제", "선택 객체 억제/해제"):
            if name in self._ribbon_actions:
                self._ribbon_actions[name].setEnabled(self._selected is not None)
        history = self.app.execute("app.history")
        self._ribbon_actions["실행 취소"].setEnabled(bool(history["undo"]) and not history["in_transaction"])
        self._ribbon_actions["다시 실행"].setEnabled(bool(history["redo"]) and not history["in_transaction"])
        self._ribbon_actions["컨투어 표시"].setEnabled(getattr(self, "_last_result", None) is not None or bool(self.result_ws.results()))

    # ------------------------------------------------------------ 작업 공간(D13): 모델 ↔ 결과. 표시만 바꾸고 명령은 잠그지 않는다
    def set_workspace(self, name: str, keep_symbols: bool = False) -> None:
        """작업 공간 전환. keep_symbols: 모델 공간으로 갈 때 명령이 정해 둔 심볼 상태(셋 모드 등)를 다시 그리지 않고 그대로 둔다."""
        if name not in ("model", "result") or name == self.workspace:
            return
        self.workspace = name
        result = name == "result"
        self.explorer_stack.setCurrentIndex(1 if result else 0)
        self.property_stack.setCurrentIndex(1 if result else 0)
        self.model_dock.setWindowTitle("결과" if result else "탐색기")
        self.property_dock.setWindowTitle("결과 표시" if result else "속성")
        if result:
            # 결과 공간: 하중·구속 심볼 끄기, 확장 패널(스케치 등) 숨기기, 입힌 결과가 없으면 패널 설정으로 입히기
            self._hidden_for_result = [d for d in self.findChildren(QDockWidget)
                                       if d not in (self.model_dock, self.property_dock, self.log_dock) and d.isVisible()]
            for d in self._hidden_for_result:
                d.hide()
            try:
                self.app.execute("view.symbols")
            except Nasa95Error:
                pass
            self.result_ws.refresh()
            self.result_ws.show_last()
        else:
            # 모델 공간: 애니메이션·컨투어 끄기, 숨겼던 패널 복원, 심볼 토글이 켜져 있으면 다시 그린다. 결과는 닫지 않는다(다시 오면 그대로)
            self.result_ws.stop()
            try:
                self.app.execute("view.result_show")
            except Nasa95Error:
                pass
            for d in self._hidden_for_result:
                d.show()
            self._hidden_for_result = []
            if not keep_symbols and getattr(self, "symbols_action", None) and self.symbols_action.isChecked():
                try:
                    self._show_symbols()
                except Nasa95Error:
                    pass
        self.viewport.present()
        self._update_legend()
        self._update_status()

    def _raise_tab(self, name: str) -> None:
        page = self.ribbon.pages.get(name)
        if page is None:
            return
        for method in ("raiseCategory", "showCategory"):
            if hasattr(self.ribbon.native, method):
                getattr(self.ribbon.native, method)(page)
                return

    def _ribbon_tab_changed(self, index: int) -> None:
        text = clean(self.ribbon.tabs.tabText(index)) if index >= 0 else ""
        if text == "결과":
            self.set_workspace("result")
        elif text in ("형상", "메시", "모델", "해석"):
            self.set_workspace("model")
        # 홈·보기 탭은 공간을 바꾸지 않는다

    def _toggle_symbols(self) -> None:
        if self.symbols_action.isChecked():
            try:
                self._show_symbols()
            except Nasa95Error:
                self.symbols_action.setChecked(False)
                raise
        else:
            self._view("view.symbols")

    def _symbols_follow_selection(self) -> None:
        """트리 선택이 스텝·하중·구속이면(토글이 켜져 있을 때) 그 스텝의 심볼로 바꾼다."""
        if getattr(self, "symbols_action", None) and self.symbols_action.isChecked() and self.workspace == "model" \
                and self._selected and self._selected[0] in ("step", "load", "bc"):
            try:
                self._show_symbols()
            except Nasa95Error:
                pass
        # 구속·구속 셋을 고르면 그 구속만 진하게, 축 기호와 함께 그린다(BC-13). 다른 것을 고르면 푼다
        focus = self._selected if self._selected and self._selected[0] in ("bc", "bc_set") else None
        if focus or getattr(self, "_bc_focus", False):
            try:
                if focus:
                    self.app.execute("selection.set", items=[{"kind": "object", "id": focus[1]}])
                else:
                    self.app.execute("selection.clear")
                self._bc_focus = bool(focus)
                self.viewport.present()
            except Nasa95Error:
                pass

    def _after_remote_command(self, name: str) -> None:
        """REST·스크립트로 들어온 명령 뒤: 다시 그리고, 결과를 입혔으면 창도 결과 작업 공간으로 따라간다(어느 입구로 오든 같은 상태)."""
        if name == "view.symbols" and getattr(self, "symbols_action", None):  # 명령으로 켜고 끈 심볼을 토글 버튼에 비춘다
            try:
                shown = self.app.execute("view.diagnostics").get("symbols", None)
            except Nasa95Error:
                shown = None
            if shown is not None:
                self.symbols_action.blockSignals(True)
                self.symbols_action.setChecked(bool(shown))
                self.symbols_action.blockSignals(False)
                if shown and self.workspace == "result":
                    # 심볼을 켜는 것은 모델 작업이다 — 결과 공간에 있었으면 모델 공간으로 따라간다(명령이 정한 심볼 상태는 그대로 둔다)
                    self._raise_tab("해석")
                    self.set_workspace("model", keep_symbols=True)
        if name.startswith("server.") and getattr(self, "rest_button", None):
            self.rest_button.refresh()
        if name == "view.result_show":
            try:
                st = self.app.execute("view.result_state")
            except Nasa95Error:
                st = {"shown": False}
            if st.get("shown"):
                self.result_ws.select_result(st["settings"]["result"])
                if self.workspace != "result":
                    self._raise_tab("결과")
                    self.set_workspace("result")
                else:
                    self.result_ws.refresh()
        self.viewport.present()
        self._update_legend()

    def _result_view_changed(self) -> None:
        self.viewport.present()
        self._update_legend()
        self._update_status()

    def _update_status(self) -> None:
        """상태 표시줄 오른쪽: 작업 공간과 결과의 오래됨(모델이 바뀜 — 재해석 필요)."""
        if not hasattr(self, "workspace_label"):
            return
        self.workspace_label.setText("결과" if self.workspace == "result" else "모델")
        outdated = any(r.get("outdated") for r in self.result_ws.results())
        self.outdated_label.setText("⚠ 모델이 바뀜 — 재해석 필요" if outdated else "")
        self.outdated_label.setVisible(outdated)

    def _reload_results_of(self, case: int) -> None:
        """케이스를 다시 풀고 나면 그 케이스의 열린 결과를 다시 읽는다(outdated 가 풀린다)."""
        for r in self.result_ws.results():
            if r.get("case") == case:
                try:
                    self.app.execute("result.reload", result=r["id"])
                except Nasa95Error:
                    pass
        self.result_ws.refresh()
        if self.workspace == "result":
            self.result_ws.apply()
        self._update_status()

    def _model_event(self, event):
        if self._closed:
            return
        command = event.get("command", "")
        if command:
            self.log.appendPlainText(f"{event.get('source', 'command')} · {command}")
        if event.get("source") == "reset":
            self._tree_expanded.clear()
            self._search_expanded = None
            self._selected = None
            self._apply_view_background()
        QTimer.singleShot(0, self.refresh)

    def _save_workspace(self):
        self.theme.settings.setValue("dock_state_saribbon", self.saveState())
        self.theme.settings.setValue("show_empty", self.tree_filter_button.isChecked())
        self.theme.settings.sync()
