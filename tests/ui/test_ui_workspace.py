"""리본 공유 명령, 테마 전환, 트리 상태와 표시/억제 분리를 확인한다."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))
pytest.importorskip("PySide6")
from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtGui import QPalette
from PySide6.QtWidgets import QApplication, QToolBar, QToolButton
from openfep import App
from openfep.ui import MainWindow
from openfep.ui.ribbon import saribbon


@pytest.fixture
def window():
    qt = QApplication.instance() or QApplication([])
    w = MainWindow(App())
    yield w, qt
    w.close()
    qt.processEvents()


def branch(w, kind):
    return next(w.tree.topLevelItem(i) for i in range(w.tree.topLevelItemCount()) if w.tree.topLevelItem(i).text(1) == kind)


@pytest.mark.feature("RND-36")
def test_RND_T03_25_ribbon_number_label_toggles(window):
    w, qt = window
    w.show()
    qt.processEvents()
    node = w._label_actions["nodes"]
    element = w._label_actions["elements"]
    assert node.isCheckable() and element.isCheckable()
    assert not node.isChecked() and not element.isChecked()
    buttons = {action: button for button, _, action in w.ribbon.buttons}
    buttons[node].click()
    buttons[element].click()
    assert w.app.execute("view.labels_get")["nodes"] and w.app.execute("view.labels_get")["elements"]
    buttons[node].click()
    assert not node.isChecked() and element.isChecked()
    for mode in ("dark", "light"):
        w.theme.set_mode(mode)
        assert not node.isChecked() and element.isChecked()
    # API/REST로 바꾼 표시도 다음 출력 때 버튼 상태에 반영한다.
    w.app.execute("view.labels", all_nodes=True, all_elements=False)
    w.viewport.present()
    assert node.isChecked() and not element.isChecked()
    w.app.execute("project.new")
    qt.processEvents()
    assert not node.isChecked() and not element.isChecked()


def test_UI_saribbon_titlebar_theme_and_window_controls(window):
    w, qt = window
    w.show()
    # 라이브러리 생성자의 지연된 Office 테마도 처리한 뒤 검사한다.
    qt.processEvents()
    assert isinstance(w, saribbon.SARibbonMainWindow)
    assert w.ribbon.native.isTabOnTitle()
    for mode, background, foreground in (("dark", "#181818", "#cccccc"), ("light", "#c8c8c8", "#3b3b3b")):
        w.theme.set_mode(mode)
        qt.processEvents()
        assert not w.styleSheet()
        assert w.ribbon.stack.palette().color(QPalette.ColorRole.Window).name() == background
        assert w.ribbon.file_button.palette().color(QPalette.ColorRole.ButtonText).name() == foreground
    tabs_top = w.ribbon.tabs.mapTo(w, QPoint()).y()
    assert tabs_top < 36
    for name in ("SAMinimizeWindowButton", "SAMaximizeWindowButton", "SACloseWindowButton"):
        button = w.findChild(QToolButton, name)
        assert button.isVisible() and not button.icon().isNull()
        assert button.mapTo(w, QPoint()).y() < 36
    maximize = w.findChild(QToolButton, "SAMaximizeWindowButton")
    maximize.click()
    qt.processEvents()
    assert w.isMaximized()
    maximize.click()
    qt.processEvents()
    assert not w.isMaximized()


def test_UI_saribbon_compact_search_and_extension_menu(window):
    w, qt = window
    w.resize(1000, 720)
    w.show()
    qt.processEvents()
    assert not w.ribbon.search.isVisible()
    w.ribbon.focus_search()
    qt.processEvents()
    assert w.ribbon.search.isVisible()
    w.app.model.materials.create(name="STEEL")
    w.refresh()
    w.ribbon.search.setText("실행 취소")
    w.ribbon.search.returnPressed.emit()
    assert not w.app.execute("material.list")
    extension = w.menuBar().addMenu("테스트 확장")
    called = []
    command = extension.addAction("확장 실행")
    command.triggered.connect(lambda: called.append(True))
    # SARibbon은 QToolButton.setMenu 대신 공유 QAction에 메뉴를 연결한다.
    menu = w._extension_button.defaultAction().menu()
    opened = []
    def close_popup():
        opened.append(menu.isVisible())
        menu.close()
    QTimer.singleShot(100, close_popup)
    w._extension_button.showMenu()
    assert opened == [True]
    entry = next(action for action in menu.actions() if action.menu() is extension)
    entry.menu().actions()[0].trigger()
    assert called == [True]


@pytest.mark.feature("RND-18")
@pytest.mark.feature("RND-42")
def test_RND_T04_18_drag_preserves_quality(window, monkeypatch):
    w, _ = window
    monkeypatch.setattr(w.viewport, "present", lambda: None)
    settings = w.app.execute("view.quality", antialiasing="ssaa2", transparency="sorted", simplify_during_interaction=True)
    w.app.execute("view.display_mode", mode="shaded_edges")
    w.viewport._begin_interaction()
    assert w.app.execute("view.display_mode")["mode"] == "wireframe"
    assert w.app.execute("view.quality_get") == settings
    w.viewport._end_interaction()
    assert w.app.execute("view.display_mode")["mode"] == "shaded_edges"
    assert w.app.execute("view.quality_get") == settings
    settings = w.app.execute("view.quality", simplify_during_interaction=False)
    w.viewport._begin_interaction()
    assert w.app.execute("view.display_mode")["mode"] == "shaded_edges"
    assert w.app.execute("view.quality_get") == settings


@pytest.mark.feature("WT-01")
@pytest.mark.feature("WT-34")
def test_UI_tree_search_preserves_expansion_and_selection(window):
    w, qt = window
    mat = w.app.model.materials.create(name="STEEL")
    w.app.model.materials.create(name="ALUMINUM")
    w.refresh()
    b = branch(w, "material")
    b.setExpanded(False)
    b.child(0).setSelected(True)
    w.tree_search.setText("STEEL")
    assert b.isExpanded() and not b.child(0).isHidden() and b.child(1).isHidden()
    w.tree_search.clear()
    assert not b.isExpanded() and not b.child(1).isHidden()
    w.app.model.materials.create(name="COPPER")
    w.refresh()
    b = branch(w, "material")
    assert not b.isExpanded() and b.child(0).isSelected()
    assert w._selected == ("material", mat.id)
    assert w.tree.isColumnHidden(1) and not b.child(0).icon(0).isNull()
    w.tree_search.setText("NO_MATCH")
    assert b.isHidden() and not w.tree_empty.isHidden()


@pytest.mark.feature("CMN-08")
@pytest.mark.feature("WT-18")
def test_UI_ribbon_actions_share_undo_and_context(window):
    w, qt = window
    assert [w.ribbon.tabs.tabText(i) for i in range(w.ribbon.tabs.count())] == ["홈", "형상", "메시", "모델", "해석", "결과", "보기"]
    assert not w._ribbon_actions["선택한 케이스 실행"].isEnabled()
    w.app.model.materials.create(name="STEEL")
    w.refresh()
    undo = w._ribbon_actions["실행 취소"]
    assert undo.isEnabled()
    undo.trigger()
    assert not w.app.execute("material.list")
    w._ribbon_actions["다시 실행"].trigger()
    assert len(w.app.execute("material.list")) == 1
    case = w.app.model.cases.create(name="STATIC")
    w.refresh()
    branch(w, "case").child(0).setSelected(True)
    assert w._ribbon_actions["선택한 케이스 실행"].isEnabled()
    assert w._selected == ("case", case.id)
    w.ribbon.set_collapsed(True)
    assert w.ribbon.stack.isHidden()
    w.ribbon.set_collapsed(False)
    assert not w.ribbon.stack.isHidden()
    qt.processEvents()
    assert w.ribbon.file_button.menu().title().startswith("파일")


@pytest.mark.feature("WT-20")
def test_UI_visibility_is_separate_from_suppression(window):
    w, qt = window
    if not w.app.execute("app.version")["geometry"]:
        pytest.skip("형상 커널 없음")
    part = w.app.model.parts.create(name="BRACKET")
    w.refresh()
    item = branch(w, "part").child(0)
    before = w.app.digest()
    w._visibility_clicked(item, 2)
    assert part.id in w.app.execute("view.diagnostics")["hidden"]
    assert w.app.digest() == before and not part.get()["suppressed"]
    w._visibility_clicked(item, 2)
    assert part.id not in w.app.execute("view.diagnostics")["hidden"]


@pytest.mark.feature("CMN-20")
def test_UI_theme_changes_without_model_changes_and_persists(window):
    w, qt = window
    w.app.model.materials.create(name="STEEL")
    before = w.app.digest()
    w.theme.set_mode("dark")
    assert w.palette().color(QPalette.ColorRole.Window).name() == "#1f1f1f"
    w.theme.set_compact(True)
    assert w.app.digest() == before
    assert w.theme.settings.value("theme") == "dark"
    w.theme.set_mode("light")
    assert w.palette().color(QPalette.ColorRole.Window).name() == "#ffffff"
    w.theme.set_mode("system")
    assert w.theme.mode == "system" and w.app.digest() == before


@pytest.mark.feature("CMN-20")
@pytest.mark.feature("WT-01")
def test_UI_panel_visibility_and_view_background(window):
    w, qt = window
    w.show()
    qt.processEvents()
    assert w.findChild(QToolBar, "activityBar") is None
    assert w.model_dock.geometry().left() == w.contentsRect().left()
    menu_actions = w.ribbon.options.menu().actions()
    assert w.model_dock.toggleViewAction() in menu_actions
    assert w.property_dock.toggleViewAction() in menu_actions
    assert w.log_dock.toggleViewAction() in menu_actions
    w.model_dock.toggleViewAction().trigger()
    assert w.model_dock.isHidden()
    w.model_dock.toggleViewAction().trigger()
    assert not w.model_dock.isHidden()
    w.log_dock.toggleViewAction().trigger()
    assert not w.log_dock.isHidden()
    w.theme.set_mode("dark")
    image, _ = w.app.view.render(240, 180)
    assert tuple(image[0, 0, :3]) == (58, 58, 58)
    w._set_view_background("white")
    w.theme.set_mode("light")
    w.theme.set_mode("dark")
    image, _ = w.app.view.render(240, 180)
    assert tuple(image[0, 0, :3]) == (255, 255, 255)
    w._set_view_background("theme")
