"""속성 영역(1단계): 재료·프로퍼티·공통 정의·스텝 승계·사전 검사·매크로·명령 등록.

케이스 정의: .agent/tests/tc-00-system.md, tc-03-material-property.md, tc-04-load-bc.md, tc-05-analysis.md
형상·메시·솔버가 필요한 부분(덱 출력, 풀이, 요소 전파)은 그 단계에서 더한다.
"""
import math

import pytest

from openfep import App, OfepError

from conftest import history_len, total

NODES = {"type": "nodes", "ids": [1]}


def codes(issues) -> list[str]:
    return sorted(i["code"] for i in issues)


def steel(app: App, name: str = "steel"):
    m = app.model.materials.create(name=name)
    m.set_elastic(data=[[210000.0, 0.3]])
    m.set_density(data=[[7.85e-9]])
    return m


# ================================================================ 재료 (MAT-T01, MAT-T04)
@pytest.mark.feature("MAT-02")
@pytest.mark.feature("MAT-03")
def test_MAT_T01_03_04_elastic_forms(app):
    m = app.model.materials.create(name="m")
    for kind, n in [("iso", 2), ("ortho", 9), ("engineering_constants", 9), ("aniso", 21)]:
        row = [float(i + 1) for i in range(n)]
        if kind == "iso":
            row = [210000.0, 0.3]
        m.set_elastic(type=kind, data=[row])
        assert m.props["behaviors"]["elastic"] == {"type": kind, "data": [row]}
        with pytest.raises(OfepError) as e:  # 상수 개수가 맞지 않으면 거부
            m.set_elastic(type=kind, data=[row + [1.0, 2.0]])
        assert e.value.code == "invalid_param_type"


@pytest.mark.feature("MAT-04")
def test_MAT_T01_05_06_hardening_curve(app):
    m = steel(app)
    curve = [[250.0, 0.0], [300.0, 0.05], [350.0, 0.2]]
    m.set_plastic(data=curve)
    assert app.execute("material.curve", id=m.id, behavior="plastic")["data"] == curve
    before = total(app)
    with pytest.raises(OfepError) as e:  # 소성 변형률이 감소하는 곡선
        m.set_plastic(data=[[250.0, 0.0], [300.0, 0.2], [350.0, 0.1]])
    assert e.value.code == "invalid_order" and total(app) == before
    # 첫 점의 소성 변형률이 0 이 아니면 저장은 되지만 검사에서 지목된다
    m.set_plastic(data=[[250.0, 0.01], [300.0, 0.05]])
    assert "out_of_range" in codes(m.check())


@pytest.mark.feature("MAT-07")
def test_MAT_T01_09_20_temperature_dependence(app):
    m = app.model.materials.create(name="m")
    m.set_elastic(data=[[210000.0, 0.30, 20.0], [190000.0, 0.32, 200.0]])
    r = app.execute("material.curve", id=m.id, behavior="elastic", temperature=110.0)
    assert r["values"] == pytest.approx([200000.0, 0.31])
    assert app.execute("material.curve", id=m.id, behavior="elastic", temperature=-50.0)["values"][0] == 210000.0
    with pytest.raises(OfepError) as e:  # 온도가 오름차순이 아님
        m.set_elastic(data=[[210000.0, 0.3, 200.0], [190000.0, 0.3, 20.0]])
    assert e.value.code == "invalid_order"
    with pytest.raises(OfepError) as e:  # 여러 행인데 온도 열이 없음
        m.set_elastic(data=[[210000.0, 0.3], [190000.0, 0.3]])
    assert e.value.code == "invalid_param_type"


@pytest.mark.feature("MAT-12")
def test_MAT_T01_14_19_physical_range(app):
    m = app.model.materials.create(name="m")
    m.set_elastic(data=[[-1.0, 0.5]])
    m.set_conductivity(data=[[-5.0]])
    m.set_specific_heat(data=[[0.0]])
    fields = sorted(i["field"] for i in m.check() if i["code"] == "out_of_range")
    assert fields == ["conductivity.data", "elastic.data", "elastic.data", "specific_heat.data"]


@pytest.mark.feature("MAT-05")
def test_MAT_T01_18_remove_behavior(app):
    m = steel(app)
    m.set_creep(data=[[1e-10, 5.0, 0.0]])
    m.remove_creep()
    assert "creep" not in m.props["behaviors"]
    app.undo()
    assert "creep" in m.props["behaviors"]
    with pytest.raises(OfepError) as e:
        m.remove_hyperfoam()
    assert e.value.code == "not_found"


@pytest.mark.feature("MAT-28")
def test_MAT_T04_05_prerequisites(app):
    m = app.model.materials.create(name="m")
    m.set_plastic(data=[[250.0, 0.0]])
    assert codes(m.check()) == ["missing_behavior"]
    m.set_elastic(data=[[210000.0, 0.3]])
    assert m.check() == []
    m.set_cyclic_hardening(data=[[250.0, 0.0]])
    m.remove_plastic()
    assert codes(m.check()) == ["missing_behavior"]


@pytest.mark.feature("MAT-14")
def test_MAT_T02_05_hyperelastic_constant_counts(app):
    m = app.model.materials.create(name="rubber")
    expected = {("neo_hooke", 1): 2, ("mooney_rivlin", 1): 3, ("arruda_boyce", 1): 3, ("yeoh", 1): 6,
                ("ogden", 1): 3, ("ogden", 2): 6, ("ogden", 3): 9, ("polynomial", 1): 3, ("polynomial", 2): 7,
                ("polynomial", 3): 12, ("reduced_polynomial", 2): 4}
    for (model, n), count in expected.items():
        m.set_hyperelastic(model=model, n=n, data=[[1.0] * count])
        with pytest.raises(OfepError):
            m.set_hyperelastic(model=model, n=n, data=[[1.0] * (count + 2)])


@pytest.mark.feature("MAT-10")
def test_MAT_T01_12_orientation(app):
    m = steel(app)
    ori = app.model.orientations.create_rectangular(a=[1, 0, 0], b=[0, 1, 0])
    m.set_orientation(orientation=ori.id)
    assert m.props["orientation"] == ori.id
    with pytest.raises(OfepError) as e:
        ori.delete()
    assert e.value.code == "referenced"
    m.set_orientation(orientation=None)
    ori.delete()


@pytest.mark.feature("MAT-11")
def test_MAT_T01_13_24_library(app, tmp_path):
    m = steel(app)
    m.set_plastic(data=[[250.0, 0.0], [300.0, 0.1]])
    app.model.materials.create(name="alu").set_elastic(data=[[70000.0, 0.33]])
    path = str(tmp_path / "lib.json")
    assert app.execute("material.library_export", path=path)["count"] == 2
    assert [x["name"] for x in app.execute("material.library_list", path=path)] == ["steel", "alu"]
    other = App()
    other.execute("material.library_import", path=path)
    assert other.model.materials["steel"].props == m.props
    # 같은 이름이 있으면 번호를 붙여 가져온다
    r = other.execute("material.library_import", path=path, names=["steel"])
    assert [c["name"] for c in r["created"]] == ["steel-2"]
    before = total(other)
    with pytest.raises(OfepError) as e:
        other.execute("material.library_import", path=path, names=["없는 재료"])
    assert e.value.code == "not_found" and total(other) == before


# ================================================================ 프로퍼티 (PRP-T01)
@pytest.mark.feature("PRP-05")
def test_PRP_T01_05_21_section_values(app):
    m = steel(app)
    P = app.model.properties
    rect = P.create_beam(material=m.id, section="rect", dimensions=[20.0, 10.0])
    v = app.execute("property.section_values", id=rect.id)
    assert v["area"] == pytest.approx(200.0)
    assert v["i11"] == pytest.approx(20.0 * 10.0**3 / 12) and v["i22"] == pytest.approx(10.0 * 20.0**3 / 12)
    circ = P.create_beam(material=m.id, section="circ", dimensions=[8.0, 8.0])
    v = app.execute("property.section_values", id=circ.id)
    assert v["area"] == pytest.approx(math.pi * 64 / 4) and v["i11"] == pytest.approx(math.pi * 8**4 / 64)
    pipe = P.create_beam(material=m.id, section="pipe", dimensions=[10.0, 2.0])
    v = app.execute("property.section_values", id=pipe.id)
    assert v["area"] == pytest.approx(math.pi * (100 - 64)) and v["i11"] == pytest.approx(math.pi * (10**4 - 8**4) / 4)
    box = P.create_beam(material=m.id, section="box", dimensions=[20.0, 10.0, 1.0, 1.0, 1.0, 1.0])
    v = app.execute("property.section_values", id=box.id)
    assert v["area"] == pytest.approx(200 - 18 * 8)
    assert v["i11"] == pytest.approx(20 * 10**3 / 12 - 18 * 8**3 / 12)


@pytest.mark.feature("PRP-05")
def test_PRP_T02_beam_dimension_count(app):
    m = steel(app)
    with pytest.raises(OfepError) as e:
        app.model.properties.create_beam(material=m.id, section="box", dimensions=[20.0, 10.0])
    assert e.value.code == "invalid_param_type" and len(app.model.properties) == 0


@pytest.mark.feature("PRP-04")
def test_PRP_T01_04_layup(app):
    m = steel(app)
    p = app.model.properties.create_composite()
    assert codes(p.validate()) == ["missing_field"]
    layers = [{"thickness": 0.2, "material": m.id}, {"thickness": 0.3, "material": m.id}]
    p.set_layup(layers=layers)
    assert p.props["layers"] == layers and p.validate() == []
    with pytest.raises(OfepError) as e:  # 층 안의 참조도 삭제를 막는다
        m.delete()
    assert e.value.code == "referenced"
    with pytest.raises(OfepError) as e:  # 층에 필수 속성이 없음
        p.set_layup(layers=[{"thickness": 0.2}])
    assert e.value.code == "missing_param"
    with pytest.raises(OfepError) as e:  # 적층이 아닌 프로퍼티에는 쓸 수 없다
        app.model.properties.create_solid(material=m.id).set_layup(layers=layers)
    assert e.value.code == "unknown_param"


@pytest.mark.feature("PRP-12")
@pytest.mark.feature("PRP-14")
def test_PRP_T01_12_13_assign(app):
    m = steel(app)
    p = app.model.properties.create_solid(material=m.id)
    assert "unassigned" in codes(app.execute("property.check", id=p.id))
    p.assign(target={"type": "elements", "ids": [1, 2]})
    p.assign(target={"type": "elements", "ids": [2, 3]})
    assert p.props["target"] == {"type": "elements", "ids": [1, 2, 3]}
    with pytest.raises(OfepError) as e:  # 다른 종류의 대상과 섞을 수 없다
        p.assign(target={"type": "set", "ids": [app.model.sets.create_element(name="E", ids=[1]).id]})
    assert e.value.code == "invalid_state"
    p.unassign(target={"type": "elements", "ids": [1, 2, 3]})
    assert "target" not in p.props
    rows = app.execute("property.assignments")["assignments"]
    assert [r["id"] for r in rows] == [p.id] and rows[0]["target"] is None


@pytest.mark.feature("PRP-16")
def test_PRP_T02_nodal_thickness(app):
    p = app.model.properties.create_shell(material=steel(app).id, thickness=2.0)
    p.set_nodal_thickness(values=[[1, 2.0], [2, 1.5]])
    assert p.props["nodal_thickness"] is True and p.props["nodal_thickness_values"] == [[1, 2.0], [2, 1.5]]
    with pytest.raises(OfepError) as e:
        p.set_nodal_thickness(values=[[1, 0.0]])
    assert e.value.code == "out_of_range"


# ================================================================ 공통 정의 (SYS-13, GEO-43)
@pytest.mark.feature("CMN-03")
def test_SYS_13_03_04_05_functions(app, tmp_path):
    t = app.model.functions.create_table(points=[[0.0, 0.0], [1.0, 10.0], [3.0, 30.0]])
    assert app.execute("function.evaluate", id=t.id, x=[0.0, 0.5, 1.0, 2.0, 9.0])["values"] == pytest.approx(
        [0.0, 5.0, 10.0, 20.0, 30.0])
    with pytest.raises(OfepError) as e:  # x 가 오름차순이 아님
        app.model.functions.create_table(points=[[1.0, 0.0], [0.0, 1.0]])
    assert e.value.code == "invalid_order"
    f = app.model.functions.create_expression(expression="2*x^2 + sin(pi/2) - y")
    assert app.execute("function.evaluate", id=f.id, x=[0.0, 3.0], y=1.0)["values"] == pytest.approx([0.0, 18.0])
    bad = app.model.functions.create_expression(expression="2*(x")
    with pytest.raises(OfepError) as e:
        app.execute("function.evaluate", id=bad.id, x=[1.0])
    assert e.value.code == "invalid_expression"
    csv = tmp_path / "curve.csv"
    csv.write_text("time,value\n0,0\n1.5,3\n2,8\n", encoding="utf-8")
    r = app.execute("function.import", path=str(csv), type="amplitude", name="imported")
    assert app.model.functions["imported"].props["points"] == [[0.0, 0.0], [1.5, 3.0], [2.0, 8.0]] and r["id"]


@pytest.mark.feature("CMN-04")
def test_SYS_13_06_07_sets(app):
    a = app.model.sets.create_node(name="A", ids=[1, 2, 3])
    b = app.model.sets.create_node(name="B", ids=[3, 4])
    a.add(members=[3, 9])
    assert app.execute("set.members", id=a.id)["members"] == [1, 2, 3, 9]
    a.remove(members=[9])
    for op, expect in [("union", [1, 2, 3, 4]), ("difference", [1, 2]), ("intersection", [3])]:
        r = app.execute("set.boolean", a=a.id, b=b.id, operation=op, name=f"R-{op}")
        assert app.execute("set.members", id=r["id"])["members"] == expect
    e_set = app.model.sets.create_element(name="E", ids=[1])
    with pytest.raises(OfepError) as e:
        app.execute("set.boolean", a=a.id, b=e_set.id, operation="union")
    assert e.value.code == "invalid_state"
    surf = app.model.sets.create_surface(name="S", faces=[[1, 1]])
    surf.add(members=[[2, 3]])
    assert surf.props["faces"] == [[1, 1], [2, 3]]
    with pytest.raises(OfepError):  # 면 셋에 노드 번호를 넣을 수 없다
        surf.add(members=[5])
    # 셋을 적용 대상으로 쓰면 그 셋은 지울 수 없다
    load = app.model.cases.create().steps.create_static().loads.create_force(
        target={"type": "set", "ids": [a.id]}, components=[1.0, 0.0, 0.0])
    with pytest.raises(OfepError) as e:
        a.delete()
    assert e.value.code == "referenced" and e.value.details["references"][0]["id"] == load.id


@pytest.mark.feature("GEO-43")
def test_GEO_T15_10_11_parameters(app):
    H = app.model.parameters.create(name="H", value=10.0)
    depth = app.model.parameters.create(name="depth")
    app.execute("parameter.set", id=depth.id, expression="H/2")
    assert depth.props["value"] == pytest.approx(5.0)
    app.execute("parameter.set", id=H.id, value=16.0)  # H 를 바꾸면 depth 도 함께 갱신된다
    assert depth.props["value"] == pytest.approx(8.0)
    app.undo()
    assert depth.props["value"] == pytest.approx(5.0) and H.props["value"] == 10.0
    before = total(app)
    with pytest.raises(OfepError) as e:  # 서로를 참조하는 수식
        app.execute("parameter.set", id=H.id, expression="depth + 1")
    assert e.value.code == "cyclic_dependency" and total(app) == before


@pytest.mark.feature("CMN-05")
@pytest.mark.feature("LOD-29")
def test_SYS_12_01_07_unit_and_constants(app):
    assert app.execute("unit.get")["system"] == "mm-t-s"
    app.execute("unit.set", system="m-kg-s")
    assert app.execute("unit.get")["system"] == "m-kg-s"
    with pytest.raises(OfepError) as e:
        app.execute("unit.set", system="furlong-stone-fortnight")
    assert e.value.code == "out_of_range"
    app.undo()
    assert app.execute("unit.get")["system"] == "mm-t-s"
    pc = app.execute("physical_constants.set", absolute_zero=-273.15, stefan_boltzmann=5.669e-8)
    assert pc == {"absolute_zero": -273.15, "stefan_boltzmann": 5.669e-8}


# ================================================================ 스텝 승계·조합 (CAS-T01, LOD-T06, BC-T06)
def two_steps(app: App):
    case = app.model.cases.create(name="c")
    s1, s2 = case.steps.create_static(name="s1"), case.steps.create_static(name="s2")
    f1 = s1.loads.create_force(name="f1", target=NODES, components=[0.0, 0.0, -100.0])
    b1 = s1.bcs.create_displacement(name="fix", target={"type": "nodes", "ids": [9]}, dofs=[1, 2, 3])
    return case, s1, s2, f1, b1


def ids(entries) -> list[int]:
    return [e["id"] for e in entries]


@pytest.mark.feature("CAS-04")
@pytest.mark.feature("LOD-38")
@pytest.mark.feature("BC-36")
def test_CAS_T01_06_LOD_T06_08_09_inheritance(app):
    case, s1, s2, f1, b1 = two_steps(app)
    p2 = s2.loads.create_pressure(name="p2", target={"type": "faces", "ids": [[1, 1]]}, value=2.0)
    eff = app.execute("step.effective", id=s2.id)
    assert ids(eff["loads"]) == [f1.id, p2.id] and ids(eff["bcs"]) == [b1.id]
    assert [e["inherited"] for e in eff["loads"]] == [True, False]
    # 같은 종류·같은 대상의 하중을 다시 주면 앞의 것을 바꾼다
    f2 = s2.loads.create_force(name="f2", target=NODES, components=[0.0, 0.0, -50.0])
    assert ids(app.execute("step.effective", id=s2.id)["loads"]) == [p2.id, f2.id]
    # "모두 새로"
    app.execute("step.set_inheritance", id=s2.id, loads_inheritance="new", bcs_inheritance="new")
    eff = app.execute("step.effective", id=s2.id)
    assert ids(eff["loads"]) == [p2.id, f2.id] and eff["bcs"] == []
    # 억제된 하중은 유효하지 않다
    f2.suppress()
    assert ids(app.execute("step.effective", id=s2.id)["loads"]) == [p2.id]


@pytest.mark.feature("CAS-15")
def test_CAS_T05_02_perturbation_step_loads(app):
    case, s1, s2, f1, b1 = two_steps(app)
    buckle = case.steps.create_buckle(name="buckle", num_modes=3, perturbation=True)
    fb = buckle.loads.create_force(name="fb", target=NODES, components=[-1.0, 0.0, 0.0])
    s4 = case.steps.create_static(name="s4")
    assert ids(app.execute("step.effective", id=buckle.id)["loads"]) == [fb.id]  # 섭동 스텝은 자기 하중만
    assert app.execute("step.effective", id=s4.id)["loads"] == []  # 섭동 스텝 뒤로는 앞의 하중이 이어지지 않는다
    assert ids(app.execute("step.effective", id=s4.id)["bcs"]) == [b1.id]  # 구속은 이어진다


@pytest.mark.feature("CAS-03")
@pytest.mark.feature("WT-03")
def test_CAS_T01_05_11_step_order(app):
    case, s1, s2, f1, b1 = two_steps(app)
    app.execute("step.reorder", id=s2.id, index=0)
    assert [s["name"] for s in case.steps.list()] == ["s2", "s1"]
    assert app.execute("step.effective", id=s2.id)["loads"] == []  # 이제 s2 가 먼저다
    app.undo()
    s1.delete()
    assert app.execute("step.effective", id=s2.id)["loads"] == []


@pytest.mark.feature("CAS-05")
def test_CAS_T01_07_12_load_combination(app):
    case = app.model.cases.create()
    a, b, c = (case.steps.create_static(name=n) for n in "abc")
    a.loads.create_force(name="dead", target=NODES, components=[0.0, 0.0, -10.0])
    b.loads.create_pressure(name="live", target={"type": "faces", "ids": [[1, 1]]}, value=2.0)
    before, hist = total(app), history_len(app)
    r = app.execute("step.load_combination", id=c.id,
                    terms=[{"step": a.id, "factor": 1.2}, {"step": b.id, "factor": 1.6}])
    loads = {l.name: l.props for l in c.loads}
    assert loads["dead"]["components"] == pytest.approx([0.0, 0.0, -12.0])
    assert loads["live"]["value"] == pytest.approx(3.2)
    assert len(r["created"]) == 2 and history_len(app) == hist + 1
    app.undo()
    assert total(app) == before
    # 계수를 곱할 수 없는 하중(각속도는 선형이 아니다)
    b.loads.create_centrifugal(target={"type": "elements", "ids": [1]}, omega=10.0,
                               axis_point=[0, 0, 0], axis_direction=[0, 0, 1])
    with pytest.raises(OfepError) as e:
        app.execute("step.load_combination", id=c.id, terms=[{"step": b.id, "factor": 2.0}])
    assert e.value.code == "not_scalable" and total(app) != before and len(c.loads) == 0


@pytest.mark.feature("CAS-07")
def test_CAS_T01_13_output_inheritance(app):
    case, s1, s2, f1, b1 = two_steps(app)
    o1 = s1.outputs.create_node_file(variables=["U"])
    e1 = s1.outputs.create_element_file(variables=["S"])
    assert sorted(ids(app.execute("step.effective", id=s2.id)["outputs"])) == sorted([o1.id, e1.id])
    o2 = s2.outputs.create_node_file(variables=["U", "RF"])  # 같은 종류를 다시 요청하면 앞 스텝 것은 버린다
    assert sorted(ids(app.execute("step.effective", id=s2.id)["outputs"])) == sorted([e1.id, o2.id])


# ================================================================ 사전 검사 (CAS-T02, BC-T03)
def ready_case(app: App):
    """결함 없는 정적 해석 케이스."""
    m = steel(app)
    prop = app.model.properties.create_solid(material=m.id, target={"type": "elements", "ids": [1]})
    case = app.model.cases.create(name="static")
    step = case.steps.create_static()
    load = step.loads.create_force(target=NODES, components=[0.0, 0.0, -100.0])
    bc = step.bcs.create_displacement(target={"type": "nodes", "ids": [9]}, dofs=[1, 2, 3])
    return m, prop, case, step, load, bc


def check(app: App, case) -> list[dict]:
    return app.execute("case.check", id=case.id)


@pytest.mark.feature("CAS-09")
def test_CAS_T02_01_clean_case(app):
    *_, case, step, load, bc = ready_case(app)
    assert check(app, case) == []


@pytest.mark.feature("CAS-09")
def test_CAS_T02_02_03_04_05_defects(app):
    m, prop, case, step, load, bc = ready_case(app)
    prop.update(material=None)  # 재료 미지정
    found = check(app, case)
    assert [(i["code"], i["object"]) for i in found] == [("missing_field", prop.id)]
    app.undo()
    bc.delete()  # 미구속
    assert [(i["code"], i["object"]) for i in check(app, case)] == [("unconstrained", step.id)]
    app.undo()
    load.delete()  # 하중 없음은 경고
    found = check(app, case)
    assert [(i["code"], i["severity"]) for i in found] == [("no_load", "warning")]
    bc.update(values=[0.0, 0.0, 0.5])  # 0 이 아닌 강제 변위(변위 제어)는 하중이다 → 경고 없음
    assert [i["code"] for i in check(app, case) if i["code"] == "no_load"] == []
    app.undo()
    app.undo()
    load.update(target=None)  # 대상 없는 하중
    assert [(i["code"], i["object"]) for i in check(app, case)] == [("missing_field", load.id)]


@pytest.mark.feature("CAS-43")
def test_CAS_T02_08_09_10_analysis_type_rules(app):
    m, prop, case, step, load, bc = ready_case(app)
    flux = step.loads.create_surface_flux(target={"type": "faces", "ids": [[1, 1]]}, value=5.0)
    assert [(i["code"], i["object"]) for i in check(app, case)] == [("incompatible_load", flux.id)]
    flux.delete()
    m.remove_density()
    freq = case.steps.create_frequency(num_modes=5)  # 밀도 없는 재료로 고유치 해석
    found = check(app, case)
    assert [(i["code"], i["object"], i["field"]) for i in found] == [("missing_behavior", m.id, "density")]
    freq.delete()
    m.set_density(data=[[7.85e-9]])
    thermal = app.model.cases.create(name="thermal", physics="thermal")
    ht = thermal.steps.create_heat_transfer(steady_state=True)
    ht.loads.create_force(target=NODES, components=[1.0, 0.0, 0.0])
    got = {(i["code"], i["field"]) for i in check(app, thermal)}
    assert got == {("incompatible_load", ""), ("missing_behavior", "conductivity")}
    # 섭동 스텝 앞에 정적 스텝이 없음
    lone = app.model.cases.create(name="lone")
    b = lone.steps.create_buckle(num_modes=1, perturbation=True)
    b.loads.create_force(target=NODES, components=[1.0, 0.0, 0.0])
    b.bcs.create_displacement(target=NODES, dofs=[1])
    assert "no_reference_step" in codes(check(app, lone))
    # 모드 기반 동해석인데 앞선 고유치 저장이 없음
    md = app.model.cases.create(name="md")
    step_md = md.steps.create_modal_dynamic()
    step_md.bcs.create_displacement(target=NODES, dofs=[1])
    assert "missing_eigen_data" in codes(check(app, md))
    md.steps.create_frequency(num_modes=5, storage=True).move(index=0)
    assert "missing_eigen_data" not in codes(check(app, md))


@pytest.mark.feature("CAS-09")
def test_CAS_T02_other_case_not_reported(app):
    *_, case, step, load, bc = ready_case(app)
    other = app.model.cases.create(name="other")
    other.steps.create_static().loads.create_force()  # 다른 케이스의 미완성 하중
    assert check(app, case) == []


@pytest.mark.feature("BC-14")
def test_BC_T03_04_05_duplicate_and_conflict(app):
    *_, case, step, load, bc = ready_case(app)
    assert app.execute("bc.check", id=step.id) == []
    dup = step.bcs.create_displacement(name="dup", target={"type": "nodes", "ids": [9, 10]}, dofs=[1])
    found = app.execute("bc.check", id=step.id)
    assert sorted((i["code"], i["object"]) for i in found) == sorted(
        [("duplicate_constraint", bc.id), ("duplicate_constraint", dup.id)])
    dup.update(values=[1.0])  # 같은 자유도에 다른 값
    assert {i["code"] for i in app.execute("bc.check", id=step.id)} == {"conflicting_constraint"}
    dup.delete()
    bc.delete()
    assert codes(app.execute("bc.check", id=step.id)) == ["unconstrained"]


@pytest.mark.feature("BC-14")
def test_BC_T03_06_duplicate_dependent(app):
    C = app.model.constraints
    C.create_equation(terms=[[3, 1, 1.0], [4, 1, -1.0]])
    assert app.execute("constraint.check") == []
    second = C.create_equation(terms=[[3, 1, 1.0], [5, 1, -2.0]])
    assert [(i["code"], i["object"]) for i in app.execute("constraint.check")] == [("duplicate_dependent", second.id)]


@pytest.mark.feature("BC-10")
@pytest.mark.feature("BC-22")
def test_BC_T02_07_swap_and_method(app):
    cp = app.model.contact_properties.create(name="hard", pressure_overclosure="hard")
    slave, master = {"type": "faces", "ids": [[1, 1]]}, {"type": "faces", "ids": [[2, 3]]}
    pair = app.model.contact_pairs.create(slave=slave, master=master, interaction=cp.id)
    pair.swap()
    assert pair.props["slave"] == master and pair.props["master"] == slave
    node_pair = app.model.contact_pairs.create(slave=NODES, master=master, interaction=cp.id)
    before = total(app)
    with pytest.raises(OfepError) as e:  # 주 면은 요소면이어야 한다
        node_pair.swap()
    assert e.value.code == "out_of_range" and total(app) == before
    case = app.model.cases.create()
    app.execute("contact.set_method", id=case.id, method="surface_to_surface")
    assert case.props["contact_method"] == "surface_to_surface"
    app.execute("damping.set_rayleigh", id=case.id, alpha=5.0, beta=1e-4)
    assert (case.props["rayleigh_alpha"], case.props["rayleigh_beta"]) == (5.0, 1e-4)


@pytest.mark.feature("CAS-37")
def test_CAS_T09_05_06_case_links(app):
    a, b, c = (app.model.cases.create(name=n) for n in "abc")
    app.execute("case.link", id=b.id, source=a.id)
    app.execute("case.link", id=c.id, source=b.id)
    dep = app.execute("case.dependencies")
    assert dep["order"] == [a.id, b.id, c.id]
    assert {(e["case"], e["needs"]) for e in dep["edges"]} == {(b.id, a.id), (c.id, b.id)}
    before = total(app)
    with pytest.raises(OfepError) as e:
        app.execute("case.link", id=a.id, source=c.id)
    assert e.value.code == "cyclic_dependency" and total(app) == before
    with pytest.raises(OfepError) as e:  # 연결된 케이스는 지울 수 없다
        a.delete()
    assert e.value.code == "referenced"


@pytest.mark.feature("CAS-31")
def test_CAS_T08_03_04_optimization_definition(app):
    case = app.model.cases.create()
    app.execute("optimization.set_design_variables", id=case.id, design_variable_type="coordinate",
                design_nodes={"type": "nodes", "ids": [1, 2]})
    sens = case.steps.create_sensitivity()
    app.execute("optimization.add_response", id=sens.id, name="mass", type="MASS")
    app.execute("optimization.add_response", id=sens.id, name="disp", type="ALL-DISP", target=NODES)
    assert [r["name"] for r in sens.props["design_responses"]] == ["mass", "disp"]
    with pytest.raises(OfepError) as e:
        app.execute("optimization.add_response", id=sens.id, name="x")
    assert e.value.code == "missing_param"
    fd = case.steps.create_feasible_direction()
    app.execute("optimization.set_objective", id=fd.id, objective="mass", objective_target="min")
    app.execute("optimization.add_constraint", id=fd.id, response="disp", relation="le", relative_value=1.1)
    assert fd.props["constraints"] == [{"response": "disp", "relation": "le", "relative_value": 1.1}]
    with pytest.raises(OfepError) as e:  # 민감도 스텝이 아닌 스텝에는 설계 응답을 둘 수 없다
        app.execute("optimization.add_response", id=fd.id, name="m", type="MASS")
    assert e.value.code == "unknown_param"


# ================================================================ 매크로·스크립트·명령 등록 (SYS-07, SYS-14, SYS-21)
@pytest.mark.feature("API-13")
@pytest.mark.feature("API-14")
def test_SYS_07_01_02_macro(app, tmp_path):
    app.execute("macro.record_start")
    m = steel(app)
    case = app.model.cases.create(name="c")
    case.steps.create_static().loads.create_force(target=NODES, components=[0.0, 0.0, -1.0])
    app.execute("material.list")  # 조회는 기록되지 않는다
    app.undo()
    path = tmp_path / "macro.py"
    code = app.execute("macro.record_stop", path=str(path))["code"]
    assert "material.list" not in code and 'app.execute("app.undo"' in code
    other = App()
    other.execute("script.run", path=str(path))
    assert total(other) == total(app)
    with pytest.raises(OfepError) as e:
        app.execute("macro.record_stop")
    assert e.value.code == "invalid_state"


@pytest.mark.feature("API-14")
def test_SYS_07_03_script_error(app):
    before, hist = total(app), history_len(app)
    code = "app.execute('material.create', name='a')\napp.execute('material.create', name='b')\nraise ValueError('중단')\n"
    with pytest.raises(OfepError) as e:
        app.execute("script.run", code=code)
    assert e.value.code == "script_error" and e.value.details["line"] == 3
    # 오류 전까지의 변경은 남고, 하나씩 되돌릴 수 있다
    assert len(app.model.materials) == 2 and history_len(app) == hist + 2
    app.undo()
    app.undo()
    assert total(app) == before
    # 스크립트에서 묶음을 쓰면 한 단계가 된다
    app.execute("script.run", code="with app.transaction('세 재료'):\n    for n in 'xyz':\n        app.execute('material.create', name=n)\n")
    assert len(app.model.materials) == 3 and history_len(app) == hist + 1
    with pytest.raises(OfepError) as e:
        app.execute("script.run", path="없는 파일.py")
    assert e.value.code == "io_error"


@pytest.mark.feature("API-18")
@pytest.mark.feature("API-24")
def test_SYS_14_03_08_10_registered_command(app):
    def make_pair(p):
        a = app.execute("material.create", name=p["prefix"] + "-a")
        b = app.execute("material.create", name=p["prefix"] + "-b")
        return {"ids": [a["id"], b["id"]]}

    app.register_command("ext.make_pair", make_pair, desc="재료 두 개를 만든다",
                         params=[{"name": "prefix", "type": "string", "desc": "이름 접두사", "must": True}])
    assert "ext.make_pair" in {c["name"] for c in app.commands()}
    before, hist = total(app), history_len(app)
    assert len(app.execute("ext.make_pair", prefix="x")["ids"]) == 2
    assert history_len(app) == hist + 1  # 안의 명령 두 개가 한 단계
    app.undo()
    assert total(app) == before
    with pytest.raises(OfepError) as e:  # 입력 검증은 등록한 명령에도 적용된다
        app.execute("ext.make_pair")
    assert e.value.code == "missing_param"
    # 안에서 실패하면 전부 되돌린다
    app.execute("material.create", name="y-b")
    before = total(app)
    with pytest.raises(OfepError) as e:
        app.execute("ext.make_pair", prefix="y")
    assert e.value.code == "name_conflict" and total(app) == before
    # 이름 충돌과 등록 해제
    with pytest.raises(OfepError) as e:
        app.register_command("material.create", make_pair, desc="x")
    assert e.value.code == "name_conflict"
    app.unregister_command("ext.make_pair")
    assert "ext.make_pair" not in {c["name"] for c in app.commands()}
    with pytest.raises(OfepError):
        app.unregister_command("material.create")  # 내장 명령은 해제할 수 없다


@pytest.mark.feature("WT-18")
def test_SYS_21_06_07_commands_for(app):
    m = steel(app)
    load = app.model.cases.create().steps.create_static().loads.create_force()
    names = {c["name"] for c in app.execute("app.commands_for", id=m.id)}
    assert {"material.set_elastic", "material.delete", "material.rename"} <= names
    assert not any(n.startswith("load.") for n in names)
    common = {c["name"].split(".")[1] for c in app.execute("app.commands_for", ids=[m.id, load.id])}
    assert {"delete", "rename", "copy"} <= common and "set_elastic" not in common and "set_amplitude" not in common


@pytest.mark.feature("CMN-13")
def test_SYS_12_06_09_import_units(app, tmp_path, monkeypatch):
    """가져오기 단위: inch 형상·m 메시를 mm 모델로 가져오면 25.4배·1000배. 다른 단위계의 결과 파일을 단위 지정해 열면 값이 모델 단위계로 환산된다."""
    import test_SOLVER_ccx as S
    from meshutil import block
    # m 메시 → mm 모델: 1000 배
    app.execute("unit.set", system="mm-t-s")
    deck = tmp_path / "m.inp"
    deck.write_text("*NODE\n1, 0, 0, 0\n2, 0.5, 0, 0\n3, 0.5, 0.2, 0\n*ELEMENT, TYPE=CPS3, ELSET=E1\n1, 1, 2, 3\n")
    r = app.execute("mesh.import", path=str(deck), unit_system="m-kg-s")
    assert r["scale"] == pytest.approx(1000.0) and app.execute("mesh.nodes", ids=[2, 3])["coords"] == [[500.0, 0.0, 0.0], [500.0, 200.0, 0.0]]
    app.execute("project.new")
    r = app.execute("mesh.import", path=str(deck), unit_system="m-kg-s", scale=0.5)  # 둘 다 주면 곱한다
    assert r["scale"] == pytest.approx(500.0) and app.execute("mesh.nodes", ids=[2])["coords"] == [[250.0, 0.0, 0.0]]
    with pytest.raises(OfepError):
        app.execute("mesh.import", path=str(deck), unit_system="furlong")
    # inch 형상 → mm: 25.4 배(STEP 왕복)
    if app.execute("app.version").get("geometry"):
        app.execute("project.new")
        part = app.model.parts.create(name="IN")
        part.features.create_box(size=[1.0, 2.0, 3.0])
        step = tmp_path / "inch.step"
        app.execute("geometry.export", id=part.id, path=str(step))
        app.execute("project.new")
        r = app.execute("geometry.import", path=str(step), unit_system="in-lbf-s")
        m = app.execute("geometry.measure", id=r["id"])
        assert m["volume"] == pytest.approx(6.0 * 25.4 ** 3, rel=1e-9) and m["bbox"]["max"] == pytest.approx([25.4, 50.8, 76.2])
    # 결과 파일의 단위(SYS-12-09): m-kg-s 로 푼 결과를 mm-t-s 모델에서 단위 지정해 열면 변위 ×1000, 응력 ×1e-6
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("OFEP_CCX", S.CCX)
    app.execute("project.new")
    app.execute("unit.set", system="m-kg-s")
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210e9, 0.3]])
    mp = app.model.mesh_parts.create(name="MP")
    b = block(app, 4, 1, 1, size=(0.1, 0.02, 0.01), part=mp.id)
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [mp.id]})
    case = app.model.cases.create(name="m")
    case.update(work_directory=str(tmp_path / "work"))
    s = case.steps.create_static()
    root = app.model.sets.create_node(name="ROOT", ids=[b["node"](0, j, k) for j in range(2) for k in range(2)])
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "nodes", "ids": [b["node"](4, 0, 1)]}, components=[0.0, 0.0, -10.0])
    s.outputs.create_node_file(variables=["U"]), s.outputs.create_element_file(variables=["S"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    opened = app.execute("result.open", case=case.id)
    rid, frd = opened["id"], opened["path"]
    assert "unit_system" not in opened
    u_m = app.results.values(rid, 1, "DISP", "magnitude")[1]
    s_m = app.results.values(rid, 1, "STRESS", "mises")[1]
    app.execute("result.close", result=rid)
    app.execute("unit.set", system="mm-t-s")
    rid2 = app.execute("result.open", path=frd, unit_system="m-kg-s")["id"]
    import numpy as np
    assert np.allclose(app.results.values(rid2, 1, "DISP", "magnitude")[1], u_m * 1000.0)
    assert np.allclose(app.results.values(rid2, 1, "STRESS", "mises")[1], s_m * 1e-6)
    assert app.execute("result.open", path=frd, unit_system="m-kg-s")["unit_system"] == "m-kg-s"


@pytest.mark.feature("MAT-11")
@pytest.mark.feature("CMN-12")
def test_MAT_T01_25_builtin_material_db_and_units(app):
    """내장 재료 DB(path 없음): 목록·가져오기가 모델 단위계로 환산된다. unit.symbols 는 입력 상자의 단위 글."""
    rows = {r["name"]: r for r in app.execute("material.library_list")}
    assert len(rows) >= 15 and {"강재", "비철", "토목", "플라스틱"} <= {r["group"] for r in rows.values()}
    s = rows["Steel S235JR"]
    assert s["E"] == pytest.approx(210000.0) and s["nu"] == 0.3 and s["density"] == pytest.approx(7.85e-9) and s["yield"] == pytest.approx(235.0)
    assert s["unit_system"] == "mm-t-s" and "구조용" in s["note"] and "plastic" in s["behaviors"]
    assert "yield" not in rows["Gray cast iron GG25"]  # 취성: 소성 없음
    r = app.execute("material.library_import", names=["Steel S235JR"])
    m = app.execute("material.get", id=r["created"][0]["id"])
    b = m["props"]["behaviors"]
    assert b["elastic"]["data"] == [[210000.0, 0.3]] and b["density"]["data"][0][0] == pytest.approx(7.85e-9)
    assert b["specific_heat"]["data"][0][0] == pytest.approx(460e6) and b["conductivity"]["data"][0][0] == pytest.approx(45.0)  # W/(m·K) = N/(s·K)
    assert b["plastic"]["data"] == [[235.0, 0.0], [360.0, 0.2]] and m["props"]["description"] == s["note"]
    assert app.execute("material.check", id=m["id"]) == []  # 구성 모델 검사: 문제 없음
    sym = app.execute("unit.symbols")
    assert sym["pressure"] == "MPa" and sym["density"] == "t/mm³" and sym["length"] == "mm" and sym["force"] == "N"
    assert app.execute("unit.symbols", system="in-lbf-s")["pressure"] == "psi"
    # SI 모델이면 환산 없음
    app.execute("unit.set", system="m-kg-s")
    rows = {r["name"]: r for r in app.execute("material.library_list")}
    assert rows["Steel S235JR"]["E"] == pytest.approx(210e9) and rows["Steel S235JR"]["density"] == 7850
    assert app.execute("unit.symbols")["pressure"] == "Pa" and app.execute("unit.symbols")["density"] == "kg/m³"
    with pytest.raises(OfepError) as e:
        app.execute("material.library_import", names=["Unobtainium"])
    assert e.value.code == "not_found"
