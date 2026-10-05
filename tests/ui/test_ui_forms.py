"""생성·편집 양식: 객체 종류 정의에서 양식이 나오고, 입력이 명령 매개변수로 바뀐다(창을 띄우지 않고 위젯만 만든다)."""
import json
import os
import sys

import pytest

_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(_root, "python"))

pytest.importorskip("PySide6")
from nasa95 import App  # noqa: E402


@pytest.fixture(scope="module")
def qt():
    from PySide6.QtWidgets import QApplication
    return QApplication.instance() or QApplication([])


@pytest.mark.feature("WT-11")
@pytest.mark.feature("API-01")
def test_UI_forms_from_schema(qt):
    from nasa95.ui.forms import ObjectDialog
    app = App()
    mat = app.model.materials.create(name="STEEL")
    d = ObjectDialog(app, "property", "shell")
    assert set(d.editors) >= {"material", "thickness", "target", "orientation", "offset"}
    d.name.setText("SH1")
    d.editors["thickness"].widget.setText("2.5")
    mw = d.editors["material"].widget
    mw.setCurrentIndex([mw.itemData(i) for i in range(mw.count())].index(mat.id))
    d.editors["target"].target_type.setCurrentText("elements")
    d.editors["target"].target_ids.setText("1, 2, 3")
    assert d.params() == {"thickness": 2.5, "material": mat.id, "target": {"type": "elements", "ids": [1, 2, 3]}}
    d._apply()
    obj = app.execute("property.get", id=d.result_id)
    assert obj["name"] == "SH1" and obj["props"]["thickness"] == 2.5 and obj["props"]["type"] == "shell"
    # 편집: 값을 바꾸고 비운 속성은 지운다
    e = ObjectDialog(app, "property", obj=obj)
    assert e.editors["thickness"].widget.text() == "2.5"
    e.editors["thickness"].widget.setText("3")
    e.editors["target"].target_ids.setText("")
    e._apply()
    obj = app.execute("property.get", id=d.result_id)
    assert obj["props"]["thickness"] == 3.0 and "target" not in obj["props"]
    # 잘못된 입력은 창 안에 오류로 보이고 모델은 그대로
    bad = ObjectDialog(app, "material")
    bad.name.setText("이름 규칙 위반")
    bad._apply()
    assert bad.error.text().startswith("[invalid_name]") and len(app.execute("material.list")) == 1
    # 하위 객체: 스텝 아래 하중. 벡터·참조 목록·표
    step = app.model.cases.create().steps.create_static()
    f = ObjectDialog(app, "load", "force", parent_id=step.id)
    f.editors["target"].target_ids.setText("5")
    for k, e in enumerate(f.editors["components"].parts):
        e.setText(str([0, 0, -10][k]))
    f._apply()
    assert app.execute("load.get", id=f.result_id)["props"]["components"] == [0.0, 0.0, -10.0]
    g = ObjectDialog(app, "feature", "fuse", parent_id=app.model.parts.create(name="A").id)
    assert g.editors["tools"].kind == "ref_list" and g.params() == {}
    t = ObjectDialog(app, "function", "table")
    t.editors["points"].widget.setText("[[0, 0], [1, 2]]")
    t._apply()
    assert app.execute("function.get", id=t.result_id)["props"]["points"] == [[0.0, 0.0], [1.0, 2.0]]
    # 모든 종류·하위 종류의 양식이 만들어진다
    for kind, spec in app.kinds().items():
        for sub in [s["name"] for s in spec["subtypes"]] or [""]:
            ObjectDialog(app, kind, sub)


@pytest.mark.feature("MAT-01")
def test_UI_solver_name_input(qt):
    """솔버 이름 규칙을 쓰는 종류의 이름 칸: 공백은 _ 로 바뀌고, 그 밖의 안 되는 문자는 입력하는 동안 알린다."""
    from nasa95.ui.forms import ObjectDialog
    from nasa95.ui.material_dialog import MaterialDialog
    app = App()
    m = MaterialDialog(app)
    m.name.setText("Gray cast iron GG25")  # DB 에서 고른 이름이 이렇게 들어온다
    assert m.name.text() == "Gray_cast_iron_GG25" and m.error.text() == ""
    m._apply()
    assert app.execute("material.get", id=m.result_id)["name"] == "Gray_cast_iron_GG25"
    d = ObjectDialog(app, "material")
    d.name.setText("회주철 1")
    assert d.name.text() == "회주철_1" and "쓸 수 없는 문자" in d.error.text()
    d.name.setText("GC250")
    assert d.error.text() == ""
    # 솔버 이름 규칙이 없는 종류의 이름은 그대로 둔다
    c = ObjectDialog(app, "case")
    c.name.setText("해석 1")
    assert c.name.text() == "해석 1"


@pytest.mark.feature("WT-23")
def test_UI_forms_use_selection(qt):
    """화면에서 고른 면·요소면이 대상 입력으로 들어간다."""
    from nasa95.ui.forms import ObjectDialog
    app = App()
    step = app.model.cases.create().steps.create_static()
    picked = [{"hit": True, "kind": "face", "part": 7, "index": 3}, {"hit": True, "kind": "face", "part": 7, "index": 5}]
    d = ObjectDialog(app, "load", "pressure", parent_id=step.id, selection=picked)
    d.editors["target"]._use_selection()
    assert d.params()["target"] == {"type": "geometry", "ids": [[7, "face", 3], [7, "face", 5]]}
    faces = [{"hit": True, "kind": "element_face", "element": 4, "face": 2}]
    e = ObjectDialog(app, "load", "pressure", parent_id=step.id, selection=faces)
    e.editors["target"]._use_selection()
    assert e.params()["target"] == {"type": "faces", "ids": [[4, 2]]}
    b = ObjectDialog(app, "bc", "displacement", parent_id=step.id, selection=faces)  # 경계조건은 노드 대상: 요소면은 못 쓴다
    b.editors["target"]._use_selection()
    assert "target" not in b.params()
    p = ObjectDialog(app, "property", "solid", selection=faces + [{"hit": True, "kind": "element", "element": 9}])
    p.editors["target"]._use_selection()
    assert p.params()["target"] == {"type": "elements", "ids": [4, 9]}


@pytest.mark.feature("MAT-02")
def test_UI_material_behaviors_and_command_dialog(qt):
    from nasa95.ui.forms import BehaviorBox, CommandDialog
    app = App()
    mat = app.model.materials.create(name="STEEL")
    box = BehaviorBox(app, mat.id)
    assert "elastic" in box.names and "density" in box.names and box.list.count() == 0
    d = CommandDialog(app, "material.set_elastic", fixed={"id": mat.id})
    assert set(d.editors) == {"type", "data"}
    d.editors["data"].widget.setText("[[210000, 0.3]]")
    d._apply()
    assert d.result is not None and app.execute("material.get", id=mat.id)["props"]["behaviors"]["elastic"]["data"] == [[210000.0, 0.3]]
    box.refresh()
    assert box.list.count() == 1 and box.list.item(0).data(32) == "elastic"
    # 편집 양식은 현재 값을 보여 준다
    e = CommandDialog(app, "material.set_elastic", fixed={"id": mat.id}, current=box.current()["elastic"])
    assert json.loads(e.editors["data"].widget.text()) == [[210000.0, 0.3]]
    box.list.setCurrentRow(0)
    box.remove()
    assert box.list.count() == 0 and "elastic" not in app.execute("material.get", id=mat.id)["props"].get("behaviors", {})
    # 잘못된 표는 창 안에 오류
    bad = CommandDialog(app, "material.set_elastic", fixed={"id": mat.id})
    bad.editors["data"].widget.setText("[[1, 2, 3, 4]]")
    bad._apply()
    assert bad.error.text().startswith("[invalid_param_type]") and bad.result is None


@pytest.mark.feature("CAS-41")
def test_UI_settings_dialog(qt, tmp_path, monkeypatch):
    from nasa95.ui.forms import SettingsDialog
    monkeypatch.setenv("NASA95_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    d = SettingsDialog(app)
    keys = {"solver_executable", "opensees_executable", "mystran_executable", "threads", "work_directory"}  # 솔버 셋의 실행 파일(D16·D17)
    assert set(d.editors) == keys and d.values() == {k: None for k in keys}
    d.editors["solver_executable"].setText("C:/ccx/ccx.exe")
    d.editors["mystran_executable"].setText("C:/mystran/mystran.exe")
    d.editors["threads"].setText("2")
    d._apply()
    assert app.execute("app.settings_get")["values"] == {"solver_executable": "C:/ccx/ccx.exe", "mystran_executable": "C:/mystran/mystran.exe", "threads": 2}
    assert [s for s in app.execute("solver.list") if s["name"] == "mystran"][0]["setting_key"] == "mystran_executable"
    e = SettingsDialog(app)
    assert e.editors["threads"].text() == "2"
    e.editors["threads"].setText("x")
    e._apply()
    assert e.error.text().startswith("[input]")
    e.editors["threads"].setText("")
    e._apply()
    assert "threads" not in app.execute("app.settings_get")["values"]
