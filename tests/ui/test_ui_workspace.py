"""리본 공유 명령, 테마 전환, 트리 상태와 표시/억제 분리를 확인한다."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))
pytest.importorskip("PySide6")
from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtGui import QPalette
from PySide6.QtWidgets import QApplication, QToolBar, QToolButton
from nasa95 import App
from nasa95.ui import MainWindow
from nasa95.ui.ribbon import saribbon


@pytest.fixture
def window():
    qt = QApplication.instance() or QApplication([])
    w = MainWindow(App())
    yield w, qt
    w.close()
    qt.processEvents()


def branch(w, kind):
    return next(w.tree.topLevelItem(i) for i in range(w.tree.topLevelItemCount()) if w.tree.topLevelItem(i).text(1) == kind)


@pytest.mark.feature("RND-02")
@pytest.mark.parametrize("mode", ["light", "dark"])
def test_RND_T04_26_result_controls_fit_narrow_dock(window, mode):
    w, qt = window
    w.theme.set_mode(mode)
    w.show()
    w.set_workspace("result")
    r = w.result_ws
    r._loading = True  # 레이아웃 검사에는 솔버 결과가 필요하지 않다.
    r.controls.setEnabled(True)
    r.title.setText("결과 123456 · 케이스 123456")
    r.frame.setMaximum(3118)
    r.frame.setValue(339)
    r.frame_info.setText("스텝 1 증분 237 · 2.37")
    r.field.addItem("VERY_LONG_RESULT_FIELD_WITHOUT_SPACES")
    r.component.addItem("VERY_LONG_COMPONENT_WITHOUT_SPACES")
    r.summary.setText("X:/project_files_git/open-fep/test_file/20_OPENSEES_11_rc_frame_el/"
                      + "long_result_name_" * 12 + ".frd\n절점 20 · 요소 27 · 프레임 3118")
    widgets = (r.title, r.field, r.component, r.frame, r.frame_slider, r.frame_info,
               r.deform, r.colormap, r.levels, r.play, r.hide_button, r.summary)
    # 짧은 창에서 세로 스크롤이 생겨도 오른쪽 버튼·입력창이 잘리면 안 된다.
    w.resize(1280, 540)
    for width in (256, 230, 400, 230):
        w.resizeDocks([w.property_dock], [width], Qt.Orientation.Horizontal)
        qt.processEvents()
        qt.processEvents()
        viewport = r.controls.viewport()
        assert w.property_dock.width() == width
        assert not r.controls.horizontalScrollBar().isVisible()
        assert r.controls.horizontalScrollBar().maximum() == 0
        assert r.controls.widget().width() == viewport.width()
        for widget in widgets:
            left = widget.mapTo(viewport, QPoint()).x()
            assert widget.isVisible() and widget.width() > 0
            assert 0 <= left and left + widget.width() <= viewport.width()
        assert r.summary.height() >= r.summary.heightForWidth(r.summary.width())
    assert r.controls.verticalScrollBar().maximum() > 0
    r.controls.ensureWidgetVisible(r.hide_button)
    qt.processEvents()
    assert viewport.rect().contains(r.hide_button.mapTo(viewport, r.hide_button.rect().center()))


@pytest.mark.feature("CAS-01")
@pytest.mark.parametrize("entry", ["double_click", "edit"])
def test_CAS_T01_23_existing_case_edit_opens_two_trees(window, entry):
    from nasa95.ui.case_dialog import CaseDialog
    from nasa95.ui.case_tree import CaseTree
    w, qt = window
    case = w.app.model.cases.create(name="wind_1.2D_1.0W", threads=3, work_directory="case-work")
    load = w.app.model.load_sets.create(name="Wind")
    bc = w.app.model.bc_sets.create(name="Fixed")
    case.steps.create_static(load_sets=[{"set": load.id, "factor": 1.2}], bc_sets=[bc.id])
    before = w.app.digest()
    w.refresh()
    item = branch(w, "case").child(0)
    w.tree.setCurrentItem(item)
    w._selected = ("case", case.id)
    seen = []
    def inspect_dialog():
        dlg = QApplication.activeModalWidget()
        try:
            if isinstance(dlg, CaseDialog):
                seen.append((len(dlg.findChildren(CaseTree)), dlg.available_tree.isVisible(), dlg.included_tree.isVisible(),
                             dlg._case["id"], dlg.selected_load_sets(), dlg.selected_bc_sets(), dlg.details.isHidden(),
                             dlg.case_editors["threads"].value()))
        finally:
            if dlg:
                dlg.reject()
    QTimer.singleShot(0, inspect_dialog)
    if entry == "double_click":
        w.tree.itemDoubleClicked.emit(item, 0)
    else:
        w._edit_selected()  # 메뉴·우클릭 편집·Enter도 같은 연결을 사용한다.
    assert seen == [(2, True, True, case.id, [{"set": load.id, "factor": 1.2}], [bc.id], True, 3)]
    assert w.app.digest() == before


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


@pytest.mark.feature("RND-21")
def test_RND_T01_48_ribbon_mesh_shrink(window):
    w, qt = window
    w.show()
    qt.processEvents()
    action = w._shrink_action
    assert action.isCheckable() and not action.isChecked()
    w._shrink_presets[20].trigger()
    assert w.app.execute("view.mesh_options_get")["shrink"] == 0.2
    assert all(button.isChecked() for button in w._shrink_buttons)
    w._shrink_presets[40].trigger()
    for button in w._shrink_buttons:
        assert button.popupMode() == QToolButton.ToolButtonPopupMode.MenuButtonPopup
        button.click()
        assert w.app.execute("view.mesh_options_get")["shrink"] == 0.0
        assert not action.isChecked()
        button.click()
        assert w.app.execute("view.mesh_options_get")["shrink"] == 0.4
    w.app.execute("view.mesh_options", edges=False, solid_1d_2d=True, shrink=0.3)
    w.viewport.present()
    assert action.isChecked() and w._shrink_presets[30].isChecked()
    for mode in ("dark", "light"):
        w.theme.set_mode(mode)
        action.trigger()
        action.trigger()
        assert w.app.execute("view.mesh_options_get") == {"edges": False, "shrink": 0.3, "solid_1d_2d": True, "beam_axes": False}
    menu = action.menu()
    opened = []
    def close_popup():
        opened.append(menu.isVisible())
        menu.close()
    QTimer.singleShot(100, close_popup)
    w._shrink_buttons[0].showMenu()
    assert opened == [True]


@pytest.mark.feature("RND-21")
@pytest.mark.feature("RND-22")
def test_RND_T01_49_ribbon_mesh_solid_toggle(window):
    """보·쉘 입체 표시 토글: 기본 끔(선·면), 켜면 solid_1d_2d, 명령으로 바꾼 값도 버튼에 비친다. 다른 옵션은 건드리지 않는다."""
    w, qt = window
    w.show()
    qt.processEvents()
    action = w._solid_action
    assert action.isCheckable() and not action.isChecked()
    assert w.app.execute("view.mesh_options_get")["solid_1d_2d"] is False
    before = w.app.digest()
    w.app.execute("view.mesh_options", shrink=0.3)
    action.trigger()
    assert w.app.execute("view.mesh_options_get") == {"edges": True, "shrink": 0.3, "solid_1d_2d": True, "beam_axes": False} and action.isChecked()
    action.trigger()
    assert w.app.execute("view.mesh_options_get")["solid_1d_2d"] is False and not action.isChecked()
    w.app.execute("view.mesh_options", solid_1d_2d=True)
    w.viewport.present()
    assert action.isChecked()
    assert w.app.digest() == before  # 표시 옵션은 모델을 바꾸지 않는다
    # 보 1축 방향 표식 토글(PRP-07)
    axes = w._axes_action
    assert axes.isCheckable() and not axes.isChecked()
    axes.trigger()
    assert w.app.execute("view.mesh_options_get")["beam_axes"] is True and axes.isChecked()
    w.app.execute("view.mesh_options", beam_axes=False)
    w.viewport.present()
    assert not axes.isChecked() and w.app.digest() == before


def test_UI_saribbon_titlebar_theme_and_window_controls(window):
    w, qt = window
    w.show()
    # 라이브러리 생성자의 지연된 Office 테마도 처리한 뒤 검사한다.
    qt.processEvents()
    assert isinstance(w, saribbon.SARibbonMainWindow)
    assert w.ribbon.native.isTabOnTitle()
    for mode, background, foreground in (("dark", "#181818", "#cccccc"), ("light", "#f3f3f3", "#3b3b3b")):
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


@pytest.mark.feature("API-26")
@pytest.mark.feature("API-35")
def test_UI_rest_status_button_and_dialog(window, monkeypatch):
    """상태 표시줄 오른쪽의 'REST OFF/ON'(모델·NASA-95·Vulkan 과 같은 글자 모양): 클릭하면 켜지며 주소·토큰 대화 상자가 뜨고, 다시 클릭하면 꺼진다.
    대화 상자의 한 줄 복사는 'REST <url> token=<토큰>'. 명령으로 켜고 끄면 글자가 따라간다."""
    from nasa95.ui import rest_dialog
    from nasa95.ui.rest_dialog import RestDialog
    w, qt = window
    btn = w.rest_button
    assert btn.text() == "REST OFF" and btn.property("on") == "false"
    assert btn.parent() is w.statusBar() and btn.x() > w.statusBar().width() // 2  # 오른쪽(영구 위젯)
    w.app.execute("server.configure", port=0)  # 빈 포트
    opened = []
    monkeypatch.setattr(RestDialog, "exec", lambda self: opened.append(self.one_line_text()) or 0)
    btn.toggle()  # 클릭 = 켜기 → 대화 상자
    st = w.app.execute("server.status")
    assert st["running"] and btn.text() == "REST ON" and btn.property("on") == "true"
    assert opened and opened[0] == f"REST {st['url']} token={rest_dialog.full_token(w.app)}" and rest_dialog.full_token(w.app) in w.app.rest.tokens
    dlg = RestDialog(w.app, parent=w)
    assert dlg.state.text() == "켜짐" and dlg.url.text() == st["url"] and dlg.port.value() == st["port"] and dlg.one_line.isEnabled()
    dlg._copy_line()
    assert QApplication.clipboard().text() == dlg.one_line_text()
    btn.toggle()  # 클릭 = 끄기(대화 상자 없음)
    assert not w.app.execute("server.status")["running"] and btn.text() == "REST OFF" and len(opened) == 1
    w.app.execute("server.start")
    w._after_remote_command("server.start")
    assert btn.text() == "REST ON"
    w.app.execute("server.stop")
    w._after_remote_command("server.stop")
    assert btn.text() == "REST OFF"
