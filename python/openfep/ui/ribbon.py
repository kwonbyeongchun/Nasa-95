"""SARibbon에 기존 QAction과 명령 검색을 연결하는 얇은 어댑터."""
from PySide6.QtCore import QEvent, QObject, QSize, Qt, Signal
from PySide6.QtGui import QColor, QIcon
from PySide6.QtWidgets import QCompleter, QLineEdit, QStackedWidget, QToolButton
from .icons import icon

try:
    from PySideSARibbon import saribbon
except ImportError as exc:
    raise ImportError("SARibbon 바인딩이 필요합니다. scripts/setup_ribbon.ps1을 실행하세요.") from exc


def clean(text):
    import re
    return re.sub(r"\(&.\)", "", text).replace("&", "").replace("...", "")


class Ribbon(QObject):
    collapsedChanged = Signal(bool)

    def __init__(self, parent):
        super().__init__(parent)
        self.native = parent.ribbonBar()
        self.native.setObjectName("ribbon")
        self.native.setRibbonStyle(saribbon.SARibbonBar.RibbonStyleCompactTwoRow)
        self.native.setTabOnTitle(True)
        self.native.setTitleBarHeight(36)
        self.native.setTabBarHeight(32)
        self.native.setCategoryHeight(88)
        self.native.setPanelTitleHeight(18)
        self.native.setEnableShowPanelTitle(True)
        self.native.setPanelToolButtonIconSize(QSize(16, 16), QSize(24, 24))
        self.native.setEnableWordWrap(False)
        self.native.setLargeButtonMinimumWidthRatio(0.65)
        self.native.showMinimumModeButton(True)
        self.tabs = self.native.ribbonTabBar()
        self.tabs.setObjectName("ribbonTabs")
        self.stack = self.native.findChild(QStackedWidget, "objSARibbonStackedContainerWidget")
        self.quick = self.native.quickAccessBar()
        self.quick.setIconSize(QSize(16, 16))
        self.file_button = self.native.applicationButton()
        self.file_button.setObjectName("fileButton")
        self.file_button.setText("파일")
        self.file_button.setIcon(QIcon())
        self.file_button.setFixedWidth(48)
        self.buttons = []
        self.pages = {}
        self.search_actions = {}
        self._color = "#616161"
        self.search = QLineEdit()
        self.search.setObjectName("commandSearch")
        self.search.setPlaceholderText("명령 검색  Ctrl+Shift+P")
        self.search.setAccessibleName("리본 명령 검색")
        self.search.setFixedWidth(196)
        self.search.setClearButtonEnabled(True)
        self.search.returnPressed.connect(self._run_search)
        right = self.native.rightButtonGroup()
        self._search_widget_action = right.addWidget(self.search)
        self.options = QToolButton()
        self.options.setToolTip("화면 설정 · 테마와 패널")
        self.options.setAccessibleName("화면 설정")
        self.options.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        right.addWidget(self.options)
        # 기본 Windows 프레임 구현은 rightButtonGroup 전체를 드래그 영역에서 제외한다.
        self.native.ribbonModeChanged.connect(lambda _mode: self.collapsedChanged.emit(self._collapsed))
        self.native.installEventFilter(self)

    @property
    def _collapsed(self):
        return self.native.isMinimumMode()

    def height(self):
        return self.native.height()

    def set_collapsed(self, collapsed):
        self.native.setMinimumMode(bool(collapsed))

    def eventFilter(self, obj, event):
        if obj is self.native and event.type() == QEvent.Type.Resize:
            self._search_widget_action.setVisible(self.native.width() >= 1120)
        return False

    def focus_search(self):
        # 좁은 창에서도 단축키로 검색창에 접근한다.
        self._search_widget_action.setVisible(True)
        self.search.setFocus()
        self.search.selectAll()

    def page(self, name):
        if name not in self.pages:
            self.pages[name] = self.native.addCategoryPage(name)
        return self.pages[name]

    def group(self, page, title):
        return self.page(page).addPanel(title)

    def action(self, group, action, name="property", short=None, quick=False):
        if short:
            action.setIconText(short)
        if quick:
            group.addAction(action)
            button = group.widgetForAction(action)
        else:
            group.addLargeAction(action)
            button = group.actionToRibbonToolButton(action)
            button.setObjectName("ribbonCommand")
        button.setAccessibleName(clean(action.text()))
        button.setToolTip(action.text() + ("  " + action.shortcut().toString() if not action.shortcut().isEmpty() else ""))
        self.buttons.append((button, name, action))
        action.setIcon(icon(name, self._color))
        self.search_actions[clean(action.text())] = action
        return button

    def menu(self, group, menu, name="folder", title=None):
        action = menu.menuAction()
        action.setIconText(title or clean(menu.title()))
        action.setIcon(icon(name, self._color))
        group.addLargeMenu(menu)
        button = group.actionToRibbonToolButton(action)
        button.setObjectName("ribbonCommand")
        button.setAccessibleName(title or clean(menu.title()))
        button.setToolTip(title or clean(menu.title()))
        self.buttons.append((button, name, action))
        return button

    def update_search(self):
        completer = QCompleter(sorted(self.search_actions), self.search)
        completer.setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        completer.setFilterMode(Qt.MatchFlag.MatchContains)
        completer.activated.connect(self._run_search)
        previous = self.search.completer()
        self.search.setCompleter(completer)
        if previous:
            previous.deleteLater()

    def _run_search(self, text=None):
        action = self.search_actions.get(text or self.search.text())
        if action and action.isEnabled():
            action.trigger()
            self.search.clear()

    def recolor(self, color):
        self._color = color
        for button, name, action in self.buttons:
            action.setIcon(icon(name, color))
        self.options.setIcon(icon("settings", color))
        self.native.minimumModeAction().setIcon(icon("down" if self._collapsed else "up", color))
        self.native.setWindowTitleTextColor(QColor(color))
        for object_name, name, label in (
            ("SAMinimizeWindowButton", "window_minimize", "최소화"),
            ("SAMaximizeWindowButton", "window_maximize", "최대화 / 복원"),
            ("SACloseWindowButton", "window_close", "닫기"),
        ):
            button = self.parent().findChild(QToolButton, object_name)
            if button:
                glyph = QIcon(icon(name, color))
                if name == "window_maximize":
                    glyph.addPixmap(icon("window_restore", color).pixmap(16, 16), QIcon.Mode.Normal, QIcon.State.On)
                button.setIcon(glyph)
                button.setIconSize(QSize(12, 12))
                button.setAccessibleName(label)
                button.setToolTip(label)
