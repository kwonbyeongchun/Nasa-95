"""CAS-T01-17~22: 선택한 해석 구성과 저장·덱의 일치, 편집·취소·실패 복원."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from PySide6.QtCore import QMimeData, QPointF, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QDialog, QLineEdit
from nasa95 import App, Nasa95Error
from nasa95.ui.case_dialog import CaseDialog
from nasa95.ui.case_tree import MIME


@pytest.fixture
def scene():
    qt = QApplication.instance() or QApplication([])
    app = App()
    parts = [app.model.mesh_parts.create(name=n).id for n in ("Body", "Bracket")]
    n = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [10, 0, 0], [20, 0, 0]])["first"]
    for i, p in enumerate(parts):
        app.execute("mesh.elements_create", shape="line2", connectivity=[[n + i, n + i + 1]], part=p)
    mat = app.model.materials.create(name="Steel")
    mat.set_elastic(data=[[210000.0, 0.3]])
    app.model.properties.create_truss(material=mat.id, area=10.0, target={"type": "parts", "ids": parts})
    loads = [app.model.load_sets.create(name=n).id for n in ("Dead", "Live")]
    app.execute("load.create_force", parent=loads[1], target={"type": "nodes", "ids": [n + 1]}, components=[10, 0, 0], name="Tip_Force")
    bc = app.model.bc_sets.create(name="Fixed").id
    app.execute("bc.create_displacement", parent=bc, target={"type": "nodes", "ids": [n]}, dofs=[1, 2, 3])
    dialogs = []
    def dialog(**kw):
        d = CaseDialog(app, **kw)
        dialogs.append(d)
        return d
    yield app, parts, loads, bc, dialog, qt
    for d in dialogs:
        d.close()
        d.deleteLater()
    qt.processEvents()


def add(d, *keys):
    d._add([d._source_items[key] for key in keys])


@pytest.mark.feature("CAS-01")
@pytest.mark.feature("CAS-05")
@pytest.mark.feature("CAS-08")
def test_CAS_T01_17_transfer_save_scope_and_undo(scene):
    app, parts, loads, bc, dialog, qt = scene
    d = dialog()
    d.show()
    qt.processEvents()
    before, history = app.digest(), app.execute("app.history")
    assert not d.selected_mesh()
    for key in (("mesh_part", parts[0]), ("load_set", loads[1]), ("bc_set", bc)):
        d._source_items[key].setSelected(True)
    d.add_button.click()
    assert d.selected_mesh() == [parts[0]]
    assert d.selected_bc_sets() == [bc]
    # 같은 항목을 재추가해도 참조는 하나다.
    add(d, ("load_set", loads[1]))
    factor = d._selected_items[(0, "load_set", loads[1])]
    d.included_tree.setCurrentItem(factor, 1)
    QTest.keyClick(d.included_tree, Qt.Key.Key_F2)
    editor = d.included_tree.findChild(QLineEdit)
    assert editor is not None
    editor.selectAll()
    QTest.keyClicks(editor, "1.5")
    QTest.keyClick(editor, Qt.Key.Key_Return)
    qt.processEvents()  # delegate의 queued commitData/closeEditor 처리
    assert d.selected_load_sets() == [{"set": loads[1], "factor": 1.5}]
    assert app.digest() == before and app.execute("app.history") == history
    d.save_button.click()
    assert d.result() == QDialog.DialogCode.Accepted
    case = app.execute("case.get", id=d.result_id)
    assert case["props"]["scope"] == {"type": "parts", "ids": [parts[0]]}
    step = app.execute("step.list", parent=d.result_id)[0]
    props = app.execute("step.get", id=step["id"])["props"]
    assert props["load_sets"] == [{"set": loads[1], "factor": 1.5}] and props["bc_sets"] == [bc]
    deck = app.execute("case.preview_deck", id=d.result_id, max_lines=500)["text"]
    assert "2, 1, 15" in deck  # 선택한 셋의 집중력 × 1.5
    element_lines = deck.split("*ELEMENT", 1)[1].split("\n", 1)[1].split("*", 1)[0].strip().splitlines()
    assert len(element_lines) == 1 and element_lines[0].startswith("1,")
    # 원본 셋은 계속 존재하며 한 번의 Undo로 케이스 전체가 되돌아간다.
    assert len(app.execute("load_set.list")) == 2
    app.execute("app.undo")
    assert app.digest() == before


@pytest.mark.feature("CAS-05")
def test_CAS_T01_18_search_double_click_remove_cancel(scene):
    app, parts, loads, bc, dialog, qt = scene
    d = dialog()
    d.show()
    qt.processEvents()
    before = app.digest()
    d.available_search.setText("Tip_Force")
    assert not d._source_items[("load_set", loads[1])].isHidden()
    assert d._source_items[("load_set", loads[0])].isHidden()
    item = d._source_items[("load_set", loads[1])]
    qt.processEvents()
    position = d.available_tree.visualItemRect(item).center()
    QTest.mouseClick(d.available_tree.viewport(), Qt.MouseButton.LeftButton, pos=position)
    QTest.mouseDClick(d.available_tree.viewport(), Qt.MouseButton.LeftButton, pos=position)
    assert len(d.selected_load_sets()) == 1
    d.included_search.setText("없는 이름")
    assert d.selected_load_sets()  # 검색은 해석 구성에 영향 없음
    d.included_search.clear()
    right = d._selected_items[(0, "load_set", loads[1])]
    d.included_tree.setCurrentItem(right)
    QTest.keyClick(d.included_tree, Qt.Key.Key_Delete)
    assert not d.selected_load_sets() and not item.isHidden()
    d.reject()
    assert app.digest() == before


@pytest.mark.feature("CAS-08")
@pytest.mark.parametrize("factor", ["nan", "inf", "잘못된 값"])
def test_CAS_T01_19_empty_mesh_and_bad_factor_do_not_mutate(scene, factor):
    app, parts, loads, bc, dialog, qt = scene
    d = dialog()
    before = app.digest()
    d._apply()
    assert "메시" in d.error.text() and not d.result_id and app.digest() == before
    add(d, ("mesh_part", parts[0]), ("load_set", loads[1]))
    d._selected_items[(0, "load_set", loads[1])].setText(1, factor)
    d._apply()
    assert d.error.text() and not d.result_id and app.digest() == before


@pytest.mark.feature("CAS-03")
@pytest.mark.feature("CAS-05")
def test_CAS_T01_20_multistep_edit_drop_and_preserve_options(scene):
    app, parts, loads, bc, dialog, qt = scene
    case = app.model.cases.create(scope={"type": "parts", "ids": parts})
    first = case.steps.create_static(load_sets=[{"set": loads[0], "factor": 1.2}], bc_sets=[bc], nlgeom=True)
    second = case.steps.create_static(load_sets=[{"set": loads[1], "factor": 2.0}], loads_inheritance="new", period=2.0)
    d = dialog(step=second.get())
    assert d.selected_load_sets() == [{"set": loads[1], "factor": 2.0}]
    assert set(d.selected_mesh()) == set(parts) and len(d._step_groups) == 2
    d._selected_items[(first.id, "load_set", loads[0])].setText(1, "1.4")
    d._remove([d._selected_items[(None, "mesh_part", parts[1])]])
    # 같은 창의 실제 dropEvent 경로로 두 번째 스텝에 구속을 추가한다.
    d.available_tree.clearSelection()
    d._source_items[("bc_set", bc)].setSelected(True)
    target = d._step_groups[second.id]["bc_set"]
    d.show()
    qt.processEvents()
    mime = QMimeData()
    mime.setData(MIME, b"references")
    class Drop:
        accepted = False
        def source(self): return d.available_tree
        def mimeData(self): return mime
        def position(self): return QPointF(d.included_tree.visualItemRect(target).center())
        def acceptProposedAction(self): self.accepted = True
        def ignore(self): self.accepted = False
    event = Drop()
    d.included_tree.dropEvent(event)
    assert event.accepted and d.selected_bc_sets(second.id) == [bc]
    d._apply()
    assert d.result_id == case.id, d.error.text()
    p1, p2 = first.get()["props"], second.get()["props"]
    assert p1["load_sets"] == [{"set": loads[0], "factor": 1.4}] and p1["nlgeom"] is True
    assert p2["load_sets"] == [{"set": loads[1], "factor": 2.0}] and p2["period"] == 2.0 and p2["loads_inheritance"] == "new"
    assert case.get()["props"]["scope"] == {"type": "parts", "ids": [parts[0]]}


@pytest.mark.feature("API-04")
@pytest.mark.feature("CAS-09")
def test_CAS_T01_21_failed_save_rolls_back_and_run_validation(scene, monkeypatch):
    app, parts, loads, bc, dialog, qt = scene
    d = dialog()
    add(d, ("mesh_part", parts[0]), ("bc_set", bc))
    before = app.digest()
    execute = app.execute
    def fail(command, params=None, **kw):
        if command == "output_request.create_node_file":
            raise Nasa95Error("test_failure", "출력 요청 생성 실패", {})
        return execute(command, params, **kw)
    monkeypatch.setattr(app, "execute", fail)
    d._apply()
    assert "출력 요청" in d.error.text() and app.digest() == before and not d.result_id
    monkeypatch.setattr(app, "execute", execute)
    def check_errors(command, params=None, **kw):
        if command == "case.check":
            return [{"severity": "error", "message": "재료 누락"}]
        return execute(command, params, **kw)
    monkeypatch.setattr(app, "execute", check_errors)
    d._apply(run=True)
    assert "재료 누락" in d.error.text() and app.digest() == before and not d.run_requested
    monkeypatch.setattr(app, "execute", execute)
    # 저장은 미완성 케이스도 허용한다. 실행 버튼만 검사 오류를 차단한다.
    d._apply()
    assert d.result_id and not d.run_requested


@pytest.mark.feature("CAS-08")
def test_CAS_T01_22_unassigned_and_existing_element_scope(scene):
    app, parts, loads, bc, dialog, qt = scene
    raw = app.execute("mesh.elements_create", shape="line2", connectivity=[[1, 3]])["first"]
    d = dialog()
    add(d, ("mesh_part", 0))
    d._apply()
    assert d.result_id, d.error.text()
    case = app.execute("case.get", id=d.result_id)
    assert case["props"]["scope"] == {"type": "elements", "ids": [raw]}
    step = app.execute("step.list", parent=d.result_id)[0]
    edit = dialog(step=app.execute("step.get", id=step["id"]))
    assert edit.selected_mesh() == [-1]
    edit._apply()
    assert app.execute("case.get", id=d.result_id)["props"]["scope"] == case["props"]["scope"]


@pytest.mark.feature("CAS-01")
@pytest.mark.feature("CAS-03")
def test_CAS_T01_24_empty_case_and_details_are_saved_atomically(scene):
    app, parts, loads, bc, dialog, qt = scene
    case = app.model.cases.create(name="Empty", threads=2, work_directory="original", rayleigh_alpha=0.1)
    before = app.digest()
    d = dialog(case=case.get())
    assert d._adding_step and len(d._step_groups) == 1
    assert not app.execute("step.list", parent=case.id)  # 창을 열기만 하면 스텝을 만들지 않는다.
    d.case_editors["threads"].widget.setText("4")
    d.case_editors["work_directory"].widget.clear()
    add(d, ("load_set", loads[1]), ("bc_set", bc))
    d._apply()
    assert d.result_id == case.id, d.error.text()
    assert len(app.execute("case.list")) == 1 and len(app.execute("step.list", parent=case.id)) == 1
    props = case.get()["props"]
    assert props["threads"] == 4 and "work_directory" not in props and props["rayleigh_alpha"] == 0.1
    app.execute("app.undo")
    assert app.digest() == before
