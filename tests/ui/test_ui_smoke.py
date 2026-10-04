"""UI 연기 테스트: 주 창이 뜨고, 뷰포트가 Vulkan 으로 창에 프레임을 내고, 트리·속성이 모델을 따라간다.

화면의 모양은 사용자가 직접 확인한다. 여기서는 창이 만들어지고 그려지는지만 본다. 바탕 화면 세션이 없으면 건너뛴다.
"""
import os
import sys

import pytest

_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(_root, "python"))

pytest.importorskip("PySide6")
from openfep import App  # noqa: E402

if os.environ.get("QT_QPA_PLATFORM", "").lower() in ("offscreen", "minimal") or not os.environ.get("SESSIONNAME", "Console"):
    pytest.skip("바탕 화면 세션이 없습니다", allow_module_level=True)


def _renderer_ok():
    try:
        App().execute("view.diagnostics")
        return True
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _renderer_ok(), reason="렌더러 없이 빌드됐거나 Vulkan 을 쓸 수 없음")


@pytest.fixture(scope="module")
def qt():
    from PySide6.QtWidgets import QApplication
    return QApplication.instance() or QApplication([])


def pump(qt, ms=150):
    from PySide6.QtCore import QEventLoop, QTimer
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()


@pytest.mark.feature("RND-02")
@pytest.mark.feature("WT-01")
def test_UI_window_presents_frames(qt):
    from openfep.ui import MainWindow
    app = App()
    part = app.model.parts.create(name="BOX")
    part.features.create_box(size=[100.0, 20.0, 10.0])
    w = MainWindow(app)
    w.show()
    pump(qt, 400)
    from PySide6.QtCore import Qt
    assert w.viewport.testAttribute(Qt.WidgetAttribute.WA_NativeWindow)
    for widget in (w.centralWidget(), w.ribbon.native, w.model_dock, w.property_dock):
        assert not widget.testAttribute(Qt.WidgetAttribute.WA_NativeWindow)
    w.viewport.present()
    d = app.execute("view.diagnostics")
    assert d["last_frame"]["frames"] >= 1 and d["last_frame"]["triangles"] == 12
    assert d["last_frame"]["width"] == w.viewport.width() and d["last_frame"]["height"] == w.viewport.height()
    # 트리에 파트가 있고, 선택하면 속성이 보인다
    top = [w.tree.topLevelItem(i) for i in range(w.tree.topLevelItemCount())]
    parts = [t for t in top if t.text(1) == "part"][0]
    assert parts.childCount() == 1 and parts.child(0).text(0).startswith("BOX")
    parts.child(0).setSelected(True)
    pump(qt, 50)
    keys = [w.props.item(r, 0).text() for r in range(w.props.rowCount())]
    assert "name" in keys and "id" in keys
    # 창 가운데를 찍으면 형상의 면이 잡힌다
    hit = app.view.pick(w.viewport.width() // 2, w.viewport.height() // 2)
    assert hit["hit"] and hit["kind"] == "face" and hit["part"] == part.id
    # 카메라 조작 뒤에도 그려진다
    app.view.orbit(40, 20), app.view.pan(10, -5), app.view.zoom(1.2)
    w.viewport.present()
    assert app.execute("view.diagnostics")["last_frame"]["frames"] >= 2
    # 모델이 바뀌면 트리가 따라온다(통지 → 새로 고침)
    app.execute("mesh.generate", id=part.id, size=10.0) if app.execute("app.version")["mesher"] else None
    app.model.materials.create(name="STEEL")
    pump(qt, 200)
    top = [w.tree.topLevelItem(i) for i in range(w.tree.topLevelItemCount())]
    assert [t for t in top if t.text(1) == "material"][0].childCount() == 1
    # 크기 변경
    w.resize(900, 700)
    pump(qt, 300)
    w.viewport.present()
    d = app.execute("view.diagnostics")["last_frame"]
    assert (d["width"], d["height"]) == (w.viewport.width(), w.viewport.height())
    w.close()
    pump(qt, 100)


@pytest.mark.feature("API-26")
def test_UI_rest_calls_run_on_gui_thread(qt):
    """REST 호출이 서버 스레드에서 들어와도 명령은 GUI 스레드에서 실행된다."""
    import json
    import threading
    import urllib.request
    from openfep.ui import MainWindow
    app = App()
    w = MainWindow(app)
    w.show()
    pump(qt, 200)
    st = app.execute("server.start", port=0)
    token = app.execute("server.token_create")["token"]
    seen = {}

    def worker():
        req = urllib.request.Request(st["url"] + "/commands/material.create", method="POST", data=json.dumps({"name": "FROM_REST"}).encode(),
                                     headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=20) as r:
            seen["status"] = r.status

    t = threading.Thread(target=worker)
    t.start()
    for _ in range(100):  # GUI 이벤트를 돌려야 명령이 실행된다
        pump(qt, 50)
        if not t.is_alive():
            break
    t.join(timeout=1)
    assert seen.get("status") == 200 and [m["name"] for m in app.execute("material.list")] == ["FROM_REST"]
    pump(qt, 200)  # 통지 → 트리 갱신
    top = [w.tree.topLevelItem(i) for i in range(w.tree.topLevelItemCount())]
    assert [x for x in top if x.text(1) == "material"][0].childCount() == 1
    app.execute("server.stop")
    w.close()


@pytest.mark.feature("MSH-05")
@pytest.mark.feature("API-07")
def test_UI_mesh_job_dialog_keeps_ui_responsive(qt):
    """자동 메싱은 작업 스레드에서 돌고, 창은 도는 동안 이벤트를 처리하며 끝나면 결과를 모델에 넣는다."""
    import time
    from openfep.ui import MainWindow
    app = App()
    part = app.model.parts.create(name="BOX")
    part.features.create_box(size=[100.0, 50.0, 20.0])
    w = MainWindow(app)
    w.show()
    pump(qt, 200)
    assert w._mesh_job is None
    app.execute("mesh.generate", id=part.id, size=2.0, background=True)
    w.mesh_job_dialog(part.id)
    ticks = 0
    t0 = time.time()
    while w._mesh_job is not None and time.time() - t0 < 120:
        pump(qt, 50)  # 이벤트 루프가 돈다 = 창이 멈추지 않는다
        ticks += 1
    assert w._mesh_job is None and ticks > 3
    assert app.execute("mesh.job_status")["applied"] is True and app.execute("project.info")["elements"] > 1000
    assert "메싱: 노드" in w.statusBar().currentMessage()
    w.close()


@pytest.mark.feature("API-19")
def test_UI_extension_adds_menu(qt, tmp_path, monkeypatch):
    """켜진 확장의 ui(window) 가 주 창에 메뉴를 더한다."""
    import pathlib
    from openfep.ui import MainWindow
    monkeypatch.setenv("OFEP_EXTENSIONS", str(pathlib.Path(__file__).resolve().parents[2] / "examples" / "extensions"))
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    assert "hello.count_materials" in {c["name"] for c in app.commands()}
    w = MainWindow(app)
    assert [m.title() for m in w.extension_menus] == ["확장(&X)"]
    assert [a.text() for a in w.extension_menus[0].actions()] == ["재료 수 세기"]
    w.close()


@pytest.mark.feature("API-19")
def test_UI_ext_register_ui_items(qt, tmp_path, monkeypatch):
    """ext.register_menu/toolbar/panel/dialog/tree_action 이 주 창에 항목을 더한다 — 창이 뜨기 전에 등록한 것과 뜬 뒤에 등록한 것 모두."""
    from PySide6.QtWidgets import QDockWidget, QToolBar
    from openfep.ui import MainWindow
    monkeypatch.setenv("OFEP_EXTENSIONS", str(tmp_path / "none"))
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    app.execute("ext.register_command", name="my.count", kind="Q", desc="재료 수",
                code="def run(app, params):\n    return {'count': len(app.execute('material.list'))}\n")
    app.execute("ext.register_menu", label="세기", command="my.count", menu="확장")
    app.execute("ext.register_tree_action", label="세기(트리)", command="my.count", kind="material")
    w = MainWindow(app)
    ext_menu = w._ext_menus["확장"]
    assert [a.text() for a in ext_menu.actions()] == ["세기"]
    # 창이 뜬 뒤 등록: 바로 더해진다
    app.execute("ext.register_toolbar", label="도구 세기", command="my.count")
    app.execute("ext.register_panel", label="재료 패널", command="my.count")
    app.execute("ext.register_dialog", label="세기 대화상자", command="my.count", menu="확장")
    assert [a.text() for a in ext_menu.actions()] == ["세기", "세기 대화상자..."]
    toolbars = [t for t in w.findChildren(QToolBar) if t.windowTitle() == "확장"]
    assert len(toolbars) == 1 and [a.text() for a in toolbars[0].actions()] == ["도구 세기"]
    panel = [d for d in w.findChildren(QDockWidget) if d.windowTitle() == "재료 패널"][0]
    app.model.materials.create(name="a")
    panel.widget().findChildren(type(panel.widget().layout().itemAt(0).widget()))[0].click()  # 새로 고침 단추
    pump(qt, 50)
    assert '"count": 1' in panel.widget().layout().itemAt(1).widget().toPlainText()
    ext_menu.actions()[0].trigger()  # 메뉴 항목 실행 → 상태 표시줄에 결과
    assert "세기" in w.statusBar().currentMessage() and "1" in w.statusBar().currentMessage()
    assert [i["label"] for i in w._ext_tree_actions] == ["세기(트리)"]
    assert app.execute("ext.registrations")["ui"][-1]["type"] == "dialog"
    w.close()


@pytest.mark.feature("GEO-32")
@pytest.mark.feature("RND-38")
def test_UI_sketch_mode_extension_draws_with_mouse(qt, tmp_path, monkeypatch):
    """스케치 모드 확장(examples/extensions/sketch_mode): 패널에서 스케치를 시작하고 뷰포트에 마우스 클릭을 넣으면
    sketch.point_from_screen → sketch.snap → sketch.add_* 로 요소가 만들어진다(두 번 클릭 = 사각형, 치수 추가, 완료 → 닫힌 영역)."""
    import pathlib
    from PySide6.QtCore import QEvent, QPointF, Qt
    from PySide6.QtGui import QMouseEvent
    from PySide6.QtWidgets import QApplication
    from openfep.ui import MainWindow
    monkeypatch.setenv("OFEP_EXTENSIONS", str(pathlib.Path(__file__).resolve().parents[2] / "examples" / "extensions"))
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    part = app.model.parts.create(name="P")
    part.features.create_box(size=[40.0, 30.0, 10.0])
    w = MainWindow(app)
    w.show()
    pump(qt, 400)
    mode = w.sketch_mode
    mode.refresh_lists()
    mode.part_box.setCurrentIndex(0)
    mode.plane_box.setCurrentIndex(0)  # XY 평면(z = 0)
    app.execute("view.standard", name="top")
    app.execute("view.fit")
    w.viewport.present()
    mode.start()
    assert mode.active and mode.sketch
    mode.tool_box.setCurrentIndex(mode.TOOLS.index("rectangle"))
    vp = w.viewport
    cx, cy = vp.width() / 2, vp.height() / 2

    def click(x, y):
        for t in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonRelease):
            ev = QMouseEvent(t, QPointF(x, y), QPointF(x, y), Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
            QApplication.sendEvent(vp, ev)
        pump(qt, 50)

    click(cx - 60, cy - 40)
    click(cx + 60, cy + 40)
    ents = app.execute("sketch.get", id=mode.sketch)["props"]["entities"]
    assert len(ents) == 1 and ents[0]["kind"] == "rectangle" and ents[0]["size"][0] > 0 and ents[0]["size"][1] > 0
    # 화면 가운데(전체 맞춤의 중앙 = 상자 중심 (20, 15))를 둘러싼 사각형이다
    u0, v0 = ents[0]["corner"]
    wdt, hgt = ents[0]["size"]
    assert u0 < 20.0 < u0 + wdt and v0 < 15.0 < v0 + hgt
    mode.tool_box.setCurrentIndex(mode.TOOLS.index("circle"))
    click(cx, cy)
    click(cx + 20, cy)
    ents = app.execute("sketch.get", id=mode.sketch)["props"]["entities"]
    assert len(ents) == 2 and ents[1]["kind"] == "circle" and ents[1]["radius"] > 0
    mode.dim_kind.setCurrentText("radius")
    mode.dim_entities.setText("2")
    mode.dim_value.setText("3")
    mode.add_dimension()
    assert app.execute("sketch.get", id=mode.sketch)["props"]["entities"][1]["radius"] == pytest.approx(3.0)
    assert app.execute("sketch.profiles", id=mode.sketch)["count"] == 2  # 사각형 − 원, 원
    assert "u =" in mode.coords.text() or mode.coords.text() == ""
    mode.active = False
    w.close()


@pytest.mark.feature("WT-26")
@pytest.mark.feature("WT-32")
def test_UI_result_workspace_switch_and_outdated(qt, tmp_path, monkeypatch):
    """결과 작업 공간(D13, plans/00008): 리본 '결과' 탭으로 가면 탐색기·속성 패널이 결과용으로 바뀌고 컨투어가 입혀진다.
    '형상' 탭으로 돌아오면 컨투어가 꺼지고 모델 패널로 돌아온다(결과는 닫지 않는다). 모델을 고치면 '모델이 바뀜 — 재해석 필요' 가 뜬다.
    잠금은 없다: 결과 공간에서도 명령이 실행된다."""
    import os as _os
    import pathlib
    import sys as _sys
    _sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "api"))
    import test_SOLVER_ccx as S  # noqa: E402  (ccx 찾기·외팔보 만들기)
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("OFEP_CCX", S.CCX)
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "s.json"))
    from openfep.ui import MainWindow
    app = App()
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(6, 2, 2))
    step = case.steps.create_static()
    step.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    step.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    step.outputs.create_node_file(variables=["U"])
    step.outputs.create_element_file(variables=["S"])
    case.update(work_directory=str(tmp_path), threads=1)
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    w = MainWindow(app)
    w.show()
    pump(qt, 400)
    assert w.workspace == "model" and w.explorer_stack.currentIndex() == 0 and w.property_stack.currentIndex() == 0
    assert w.result_ws.results() == []
    # 결과 열기(리본 동작과 같은 경로) → 결과 공간
    w._selected = ("case", case.id)
    w._open_result()
    pump(qt, 200)
    assert w.workspace == "result" and w.explorer_stack.currentIndex() == 1 and w.property_stack.currentIndex() == 1
    assert w.model_dock.windowTitle() == "결과" and w.workspace_label.text() == "결과"
    shown = app.execute("view.result_state")
    assert shown["shown"] and shown["settings"]["result"] == w.result_ws.result and shown["settings"]["frame"] == w.result_ws.frame.value()
    assert w.result_ws.tree.topLevelItemCount() == 1 and w.result_ws.tree.topLevelItem(0).childCount() >= 1
    assert w.result_ws.field.count() >= 2 and w.result_ws.component.count() >= 1
    # 패널로 성분을 바꾸면 바로 입혀진다
    w.result_ws.field.setCurrentText("DISP")
    pump(qt, 100)
    assert app.execute("view.result_state")["settings"]["field"] == "DISP"
    assert not w.result_ws.is_outdated() and not w.outdated_label.isVisible()
    # 잠금 없음: 결과 공간에서 모델을 고칠 수 있고, 그러면 '재해석 필요'
    step.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, -1.0, 0.0])
    pump(qt, 300)
    assert w.result_ws.is_outdated() and w.outdated_label.isVisible()
    assert "⚠" in w.result_ws.tree.topLevelItem(0).text(0)
    app.undo()
    pump(qt, 300)
    assert not w.result_ws.is_outdated()
    # 형상 탭 → 모델 공간: 컨투어 꺼짐, 패널 복귀, 결과는 열려 있음
    w._ribbon_tab_changed([clean_text for clean_text in range(w.ribbon.tabs.count()) if "형상" in w.ribbon.tabs.tabText(clean_text)][0])
    pump(qt, 200)
    assert w.workspace == "model" and w.explorer_stack.currentIndex() == 0 and w.property_stack.currentIndex() == 0
    assert app.execute("view.result_state")["shown"] is False and len(w.result_ws.results()) == 1
    # 결과 탭으로 다시 → 마지막 설정이 그대로 입혀진다
    w._ribbon_tab_changed([i for i in range(w.ribbon.tabs.count()) if "결과" in w.ribbon.tabs.tabText(i)][0])
    pump(qt, 200)
    assert w.workspace == "result" and app.execute("view.result_state")["settings"]["field"] == "DISP"
    w.close()


@pytest.mark.feature("MAT-01")
@pytest.mark.feature("MAT-11")
@pytest.mark.feature("CMN-12")
def test_UI_material_dialog_units_and_db(qt, tmp_path, monkeypatch):
    """재료 창(사용자 요청): 값마다 입력 상자 + 단위(모델 단위계), 확인하면 구성 모델이 명령으로 들어간다. DB 창에서 고른 재료는 바로 추가되고
    값은 모델 단위계로 환산돼 있다. 편집 창은 저장된 값을 상자에 되돌려 보인다."""
    from openfep.ui.material_dialog import MaterialDbDialog, MaterialDialog, add_from_db
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    dlg = MaterialDialog(app)
    assert dlg.edits["E"].unit.text() == "MPa" and dlg.edits["density"].unit.text() == "t/mm³" and dlg.edits["nu"].unit.text() == ""
    dlg.name.setText("my_steel")
    dlg.edits["E"].edit.setText("210000")
    dlg.edits["nu"].edit.setText("0.3")
    dlg.edits["density"].edit.setText("7.85e-9")
    dlg.edits["yield"].edit.setText("250")
    dlg.edits["ultimate"].edit.setText("400")
    dlg._apply()
    assert dlg.result_id
    m = app.execute("material.get", id=dlg.result_id)
    b = m["props"]["behaviors"]
    assert m["name"] == "my_steel" and b["elastic"]["data"] == [[210000.0, 0.3]] and b["density"]["data"] == [[7.85e-9]]
    assert b["plastic"]["data"] == [[250.0, 0.0], [400.0, 0.1]] and "expansion" not in b
    # 편집: 값이 상자에 돌아오고, 항복을 비우면 소성이 빠진다
    edit = MaterialDialog(app, obj=m)
    assert edit.edits["E"].edit.text() == "210000" and edit.edits["yield"].edit.text() == "250" and edit.edits["eps_u"].edit.text() == "0.1"
    edit.edits["yield"].edit.setText("")
    edit.edits["expansion"].edit.setText("1.2e-5")
    edit._apply()
    b = app.execute("material.get", id=m["id"])["props"]["behaviors"]
    assert "plastic" not in b and b["expansion"]["data"] == [[1.2e-5]]
    # DB: 목록은 모델 단위계로 환산된 값, 고른 것은 바로 추가
    db = MaterialDbDialog(app)
    assert db.table.rowCount() >= 10 and "MPa" in db.table.horizontalHeaderItem(2).text()
    db.filter.setText("6061")
    assert db.table.rowCount() == 1 and db.table.item(0, 2).text() == "68900"
    db.table.selectRow(0)
    assert db.selected_names() == ["Aluminum 6061-T6"]
    r = app.execute("material.library_import", names=["Aluminum 6061-T6", "Steel S355J2"])
    al = app.execute("material.get", id=next(x["id"] for x in r["created"] if x["name"].startswith("Aluminum")))["props"]["behaviors"]
    assert al["elastic"]["data"][0] == pytest.approx([68900.0, 0.33]) and al["density"]["data"][0][0] == pytest.approx(2.7e-9)
    assert al["specific_heat"]["data"][0][0] == pytest.approx(896e6) and al["plastic"]["data"][0][0] == pytest.approx(276.0)
    # 다른 단위계(m-kg-s)의 모델이면 SI 그대로
    app2 = App()
    app2.execute("unit.set", system="m-kg-s")
    rows = {x["name"]: x for x in app2.execute("material.library_list")}
    assert rows["Steel S235JR"]["E"] == pytest.approx(210e9) and rows["Steel S235JR"]["density"] == 7850
    si = MaterialDialog(app2)
    assert si.edits["E"].unit.text() == "Pa" and si.edits["density"].unit.text() == "kg/m³"


@pytest.mark.feature("CAS-05")
@pytest.mark.feature("CAS-01")
def test_UI_case_dialog_picks_sets_and_mesh(qt, tmp_path, monkeypatch):
    """해석 케이스 창(D14): 메시·하중 셋(계수)·구속 셋·해석 종류를 골라 케이스+스텝+기본 출력을 만든다. 하중은 하중 셋 안에,
    구속은 구속 셋 안에 들어가고(리본의 부모 결정 _parent_for), 덱에 셋의 항목이 계수와 함께 나간다."""
    from PySide6.QtCore import Qt
    from openfep.ui import MainWindow
    from openfep.ui.case_dialog import CaseDialog
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    part = app.model.parts.create(name="bar")
    part.features.create_box(size=[20.0, 5.0, 5.0])
    mp = app.execute("mesh.generate", id=part.id, size=5.0)["mesh_part"]
    mat = app.execute("material.library_import", names=["Steel S235JR"])["created"][0]["id"]
    prop = app.model.properties.create_solid(material=mat, target={"type": "parts", "ids": [mp]})
    D = app.model.load_sets.create(name="D")
    S = app.model.bc_sets.create(name="S")
    w = MainWindow(app)
    w.show()
    pump(qt, 300)
    # 리본의 '하중' 버튼: 하중 셋이 하나뿐이면 선택 없이도 그 셋이 부모
    assert w._parent_for("load") == D.id and w._parent_for("bc") == S.id
    w._selected = ("load_set", D.id)
    assert w._parent_for("load") == D.id
    dead = D.loads.create_gravity(target={"type": "parts", "ids": [mp]}, value=9810.0, direction=[0, 0, -1])
    w._selected = ("load", dead.id)  # 셋 안의 하중을 고르고 있어도 새 하중의 부모는 그 셋
    assert w._parent_for("load") == D.id
    S.bcs.create_displacement(target={"type": "nodes", "ids": [1]}, dofs=[1, 2, 3])
    dlg = CaseDialog(app, parent=w)
    assert dlg.mesh_list.count() == 1 and dlg.load_table.rowCount() == 1 and dlg.bc_list.count() == 1
    dlg.name.setText("gravity_case")
    dlg.load_table.item(0, 0).setCheckState(Qt.CheckState.Checked)
    dlg.load_table.item(0, 2).setText("1.35")
    dlg.bc_list.item(0).setCheckState(Qt.CheckState.Checked)
    dlg._apply()
    assert dlg.result_id
    case = app.execute("case.get", id=dlg.result_id)
    step = app.execute("step.list", parent=case["id"])[0]
    sp = app.execute("step.get", id=step["id"])["props"]
    assert case["name"] == "gravity_case" and sp["type"] == "static" and sp["load_sets"] == [{"set": D.id, "factor": 1.35}] and sp["bc_sets"] == [S.id]
    assert "scope" not in case["props"]  # 메시 전부 → 범위 없음
    assert {app.execute("output_request.get", id=o["id"])["props"]["type"] for o in app.execute("output_request.list", parent=step["id"])} == {"node_file", "element_file"}
    deck = app.execute("case.preview_deck", id=case["id"], max_lines=500)["text"]
    assert "GRAV, 13243.5" in deck and "*BOUNDARY" in deck and app.execute("case.check", id=case["id"]) == []
    # 편집 창: 저장된 선택이 돌아오고, 계수를 바꾸면 반영
    edit = CaseDialog(app, parent=w, step=app.execute("step.get", id=step["id"]))
    assert edit.load_table.item(0, 0).checkState() == Qt.CheckState.Checked and edit.load_table.item(0, 2).text() == "1.35"
    edit.load_table.item(0, 2).setText("1")
    edit._apply()
    assert app.execute("step.get", id=step["id"])["props"]["load_sets"] == [{"set": D.id, "factor": 1.0}]
    w.close()
