"""프로퍼티(섹션)의 덱 출력: .agent/tests/tc-03-material-property.md PRP-T01·PRP-T02.

카드 형식은 CalculiX 2.22 매뉴얼 7장(*SOLID/SHELL/MEMBRANE/BEAM SECTION, *SPRING, *DASHPOT, *MASS, *FLUID SECTION,
*USER SECTION, *MATRIX ASSEMBLE, *ORIENTATION, *DISTRIBUTION, *NODAL THICKNESS)으로 확인한다.
"""
import pytest

from openfep import OfepError

from meshutil import block, plate
from test_CASE_deck import cantilever, deck, find, steel


def _lines(app, nodes):
    """2노드 선 요소들을 만든다. nodes = [[n1, n2], ...]."""
    return app.execute("mesh.elements_create", shape="line2", connectivity=nodes)


@pytest.mark.feature("PRP-02")
@pytest.mark.feature("PRP-03")
@pytest.mark.feature("PRP-16")
@pytest.mark.feature("PRP-17")
@pytest.mark.feature("PRP-18")
@pytest.mark.feature("PRP-23")
def test_PRP_T01_18_19_T02_04_05_06_15_solid_shell_membrane_decks(app):
    """솔리드(방향)·쉘(두께·오프셋·절점 두께)·멤브레인 섹션과 *ORIENTATION 카드."""
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static()
    ori = app.model.orientations.create_rectangular(name="OR1", a=[0.0, 1.0, 0.0], b=[-1.0, 0.0, 0.0])
    cyl = app.model.orientations.create_cylindrical(name="CYL", a=[0.0, 0.0, 0.0], b=[0.0, 0.0, 1.0])
    sec = app.model.properties["SEC"]
    sec.update(orientation=ori.id)
    # 쉘: 두께 2, 오프셋 0.5, 절점 두께
    pl = plate(app, 2, 1)
    shell = app.model.properties.create_shell(name="SH", material=mat.id, thickness=2.0, offset=0.5, orientation=cyl.id,
                                              target={"type": "elements", "ids": [pl["elements"][0]]})
    shell.set_nodal_thickness(values=[[pl["node"](0, 0), 2.0], [pl["node"](1, 0), 1.5]])
    # 멤브레인: 같은 판의 둘째 요소를 M3D4 로
    app.execute("mesh.set_element_type", ids=[pl["elements"][1]], type="M3D4")
    app.model.properties.create_membrane(name="MB", material=mat.id, thickness=0.3, target={"type": "elements", "ids": [pl["elements"][1]]})
    with pytest.raises(OfepError) as e:
        app.model.properties.create_membrane(name="MB0", material=mat.id, thickness=0.0)
    assert e.value.code == "out_of_range" and e.value.details["param"] == "thickness"
    r, cards = deck(app, case)
    assert r["skipped"] == []
    # *ORIENTATION: 직교는 a, b 점. 원통은 SYSTEM=CYLINDRICAL 과 축 위의 두 점(매뉴얼 7.102)
    assert find(cards, "ORIENTATION", NAME="OR1")[0][2] == ["0, 1, 0, -1, 0, 0"]
    assert find(cards, "ORIENTATION", NAME="CYL", SYSTEM="CYLINDRICAL")[0][2] == ["0, 0, 0, 0, 0, 1"]
    solid, = find(cards, "SOLID SECTION")
    assert solid[1] == {"ELSET": "BEAM", "MATERIAL": "STEEL", "ORIENTATION": "OR1"} and solid[2] == []
    sh, = find(cards, "SHELL SECTION")
    assert sh[1]["MATERIAL"] == "STEEL" and sh[1]["OFFSET"] == "0.5" and sh[1]["ORIENTATION"] == "CYL" and sh[1]["NODAL THICKNESS"] is True
    assert sh[2] == ["2"]
    nt, = find(cards, "NODAL THICKNESS")
    assert nt[2] == [f"{pl['node'](0, 0)}, 2", f"{pl['node'](1, 0)}, 1.5"]
    mb, = find(cards, "MEMBRANE SECTION")
    assert mb[1]["MATERIAL"] == "STEEL" and "OFFSET" not in mb[1] and mb[2] == ["0.3"]
    assert find(cards, "ELEMENT", TYPE="M3D4")[0][2][0].startswith(f"{pl['elements'][1]}, ")
    # 방향 분포는 솔리드 섹션에서만 쓸 수 있다(매뉴얼 7.42) — 쉘에 연결하면 경고
    dist = app.model.orientations.create_distribution(name="DI", system="rectangular", default_a=[1.0, 0.0, 0.0], default_b=[0.0, 1.0, 0.0],
                                                      entries=[{"elements": {"type": "elements", "ids": [1, 2]}, "a": [0.0, 0.0, 1.0], "b": [0.0, 1.0, 0.0]}])
    shell.update(orientation=dist.id)
    r, cards = deck(app, case)
    assert {w["code"] for w in r["warnings"]} >= {"distribution_solid_only"}
    shell.update(orientation=cyl.id)
    sec.update(orientation=dist.id)
    r, cards = deck(app, case)
    assert "distribution_solid_only" not in {w["code"] for w in r["warnings"]}
    d, = find(cards, "DISTRIBUTION", NAME="DI_D")
    assert d[2] == [", 1, 0, 0, 0, 1, 0", "1, 0, 0, 1, 0, 1, 0", "2, 0, 0, 1, 0, 1, 0"]  # 첫 줄은 기본 방향(요소 칸 비움)
    assert find(cards, "ORIENTATION", NAME="DI")[0][2] == ["DI_D"] and find(cards, "SOLID SECTION")[0][1]["ORIENTATION"] == "DI"


@pytest.mark.feature("PRP-06")
@pytest.mark.feature("PRP-07")
@pytest.mark.feature("PRP-08")
@pytest.mark.feature("PRP-11")
@pytest.mark.feature("PRP-15")
@pytest.mark.feature("PRP-19")
def test_PRP_T01_23_27_T02_01_03_07_08_09_beam_truss_decks(app):
    """보 단면 종류·방향·오프셋, 요소별 단면 방향, 트러스·평면 요소의 솔리드 섹션, 감차 적분 타입."""
    mat = steel(app)
    case = app.model.cases.create(name="beams")
    case.steps.create_static()
    n = app.execute("mesh.nodes_create", coords=[[i * 10.0, 0.0, 0.0] for i in range(7)])["first"]
    e = _lines(app, [[n + i, n + i + 1] for i in range(6)])["first"]
    P = app.model.properties
    rect = P.create_beam(name="RECT", material=mat.id, section="rect", dimensions=[20.0, 10.0], direction=[0.0, 0.0, 1.0],
                         offset1=0.5, offset2=-0.25, target={"type": "elements", "ids": [e, e + 1]})
    P.create_beam(name="CIRC", material=mat.id, section="circ", dimensions=[12.0, 12.0], target={"type": "elements", "ids": [e + 2]})
    P.create_beam(name="PIPE", material=mat.id, section="pipe", dimensions=[10.0, 2.0], target={"type": "elements", "ids": [e + 3]})
    P.create_beam(name="BOX", material=mat.id, section="box", dimensions=[20.0, 10.0, 1.0, 1.5, 1.0, 1.5], target={"type": "elements", "ids": [e + 4]})
    # GENERAL 단면은 사용자 요소 U1 에서만 쓸 수 있다(매뉴얼 6.3.3) — 그 요소에 *USER ELEMENT 정의가 앞서 나간다(7.136)
    app.execute("mesh.set_element_type", ids=[e + 5], type="U1")
    P.create_beam(name="GEN", material=mat.id, section="general", dimensions=[200.0, 1666.7, 1e-9, 6666.7, 0.8333], direction=[0.0, 0.0, 1.0],
                  target={"type": "elements", "ids": [e + 5]})
    with pytest.raises(OfepError) as ex:
        P.create_beam(name="NEG", material=mat.id, section="general", dimensions=[-1.0, 1.0, 0.0, 1.0, 0.8])
    assert ex.value.code == "out_of_range"
    r, cards = deck(app, case)
    assert r["skipped"] == [] and {w["code"] for w in r["warnings"]} == set()
    bs = {c[1]["SECTION"]: c for c in find(cards, "BEAM SECTION")}
    assert set(bs) == {"RECT", "CIRC", "PIPE", "BOX", "GENERAL"}
    assert bs["RECT"][1]["MATERIAL"] == "STEEL" and bs["RECT"][1]["OFFSET1"] == "0.5" and bs["RECT"][1]["OFFSET2"] == "-0.25"
    assert bs["RECT"][2] == ["20, 10", "0, 0, 1"]  # 둘째 줄 = 1축 방향(매뉴얼 7.3 셋째 줄)
    assert bs["CIRC"][2] == ["12, 12"] and bs["PIPE"][2] == ["10, 2"] and bs["BOX"][2] == ["20, 10, 1, 1.5, 1, 1.5"]
    assert bs["GENERAL"][2] == ["200, 1666.7, 1e-09, 6666.7, 0.8333", "0, 0, 1"]
    kinds = [(c[0], c[1].get("TYPE")) for c in cards]
    assert kinds.index(("USER ELEMENT", "U1")) == kinds.index(("ELEMENT", "U1")) - 1  # 정의가 그 요소 카드 바로 앞
    assert find(cards, "USER ELEMENT")[0][1] == {"TYPE": "U1", "INTEGRATION POINTS": "0", "MAXDOF": "6", "NODES": "2"}
    assert find(cards, "ELEMENT", TYPE="U1")[0][2] == [f"{e + 5}, {n + 5}, {n + 6}"]
    # 요소별 단면 방향(PRP-07): 방향이 다른 요소는 섹션이 갈라진다
    app.execute("mesh.set_beam_direction", ids=[e + 1], direction=[0.0, 1.0, 0.0])
    r, cards = deck(app, case)
    rects = [c for c in find(cards, "BEAM SECTION", SECTION="RECT")]
    assert len(rects) == 2 and sorted(c[2][1] for c in rects) == ["0, 0, 1", "0, 1, 0"]
    assert all(c[1]["ELSET"] != rects[0][1]["ELSET"] or c is rects[0] for c in rects)
    # 트러스·평면 응력: *SOLID SECTION 데이터 줄에 단면적·두께(PRP-15)
    t = _lines(app, [[n, n + 2]])["first"]
    app.execute("mesh.set_element_type", ids=[t], type="T3D2")
    P.create_truss(name="TR", material=mat.id, area=25.0, target={"type": "elements", "ids": [t]})
    pl = plate(app, 1, 1)
    app.execute("mesh.set_element_type", ids=[pl["elements"][0]], type="CPS4R")
    P.create_solid(name="PS", material=mat.id, thickness=1.5, target={"type": "elements", "ids": [pl["elements"][0]]})
    r, cards = deck(app, case)
    secs = {c[1]["ELSET"]: c for c in find(cards, "SOLID SECTION")}
    tr = find(cards, "ELEMENT", TYPE="T3D2")[0][1]["ELSET"] if "ELSET" in find(cards, "ELEMENT", TYPE="T3D2")[0][1] else None
    by_mat = [c for c in find(cards, "SOLID SECTION") if c[2] == ["25"]]
    assert len(by_mat) == 1 and [c for c in find(cards, "SOLID SECTION") if c[2] == ["1.5"]]
    assert find(cards, "ELEMENT", TYPE="CPS4R")[0][2][0].startswith(f"{pl['elements'][0]}, ")
    # 감차 적분(PRP-11): 타입에 R 이 붙고, 빈 문자열로 되돌리면 형상 기본 타입
    app.execute("mesh.set_element_type", ids=[pl["elements"][0]], type="")
    r, cards = deck(app, case)
    assert find(cards, "ELEMENT", TYPE="S4") and not find(cards, "ELEMENT", TYPE="CPS4R")


@pytest.mark.feature("PRP-09")
@pytest.mark.feature("PRP-10")
@pytest.mark.feature("PRP-20")
@pytest.mark.feature("PRP-21")
@pytest.mark.feature("PRP-22")
@pytest.mark.feature("PRP-24")
@pytest.mark.feature("PRP-25")
def test_PRP_T01_25_26_T02_10_14_18_19_spring_dashpot_mass_fluid_user_substructure(app, tmp_path):
    """*SPRING(선형·비선형·자유도·방향), *DASHPOT(주파수 의존), *MASS, *FLUID SECTION, *USER SECTION, *MATRIX ASSEMBLE."""
    mat = steel(app)
    case = app.model.cases.create(name="misc")
    case.steps.create_static()
    n = app.execute("mesh.nodes_create", coords=[[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [2.0, 0.0, 0.0], [3.0, 0.0, 0.0], [4.0, 0.0, 0.0]])["first"]
    e = _lines(app, [[n, n + 1], [n + 1, n + 2], [n + 2, n + 3]])["first"]
    for i, t in enumerate(["SPRINGA", "SPRING2", "DASHPOTA"]):
        app.execute("mesh.set_element_type", ids=[e + i], type=t)
    m = app.execute("mesh.elements_create", shape="point1", connectivity=[[n + 4]], type="MASS")["first"]
    ori = app.model.orientations.create_rectangular(name="OR", a=[0.0, 1.0, 0.0], b=[-1.0, 0.0, 0.0])
    P = app.model.properties
    P.create_spring(name="K", stiffness=1000.0, target={"type": "elements", "ids": [e]})
    P.create_spring(name="KNL", table=[[0.0, 0.0], [10.0, 1.0], [15.0, 2.0]], dof1=1, dof2=1, orientation=ori.id,
                    target={"type": "elements", "ids": [e + 1]})
    P.create_dashpot(name="C", table=[[0.5, 10.0], [0.4, 100.0]], target={"type": "elements", "ids": [e + 2]})
    P.create_mass(name="M", mass=2.5, target={"type": "elements", "ids": [m]})
    with pytest.raises(OfepError) as ex:
        P.create_mass(name="M2", mass=-1.0)
    assert ex.value.code == "out_of_range"
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    k, knl = find(cards, "SPRING")
    # SPRINGA: 자유도 줄은 비운다. SPRING2: 자유도 줄 뒤에 표(비선형, 변위 오름차순). ORIENTATION 은 스프링 카드의 매개변수
    assert "NONLINEAR" not in k[1] and k[2] == ["", "1000"]
    assert knl[1]["NONLINEAR"] is True and knl[1]["ORIENTATION"] == "OR" and knl[2] == ["1, 1", "0, 0", "10, 1", "15, 2"]
    c, = find(cards, "DASHPOT")
    assert c[2] == ["", "0.5, 10", "0.4, 100"]
    assert find(cards, "MASS")[0][2] == ["2.5"]
    # 유체 섹션(PRP-20): 네트워크 요소 D(3노드) 에 종류와 상수. 액체·개수로는 MATERIAL 대신 OIL 등
    f = app.execute("mesh.nodes_create", coords=[[10.0, 0.0, 0.0], [11.0, 0.0, 0.0], [12.0, 0.0, 0.0], [13.0, 0.0, 0.0], [14.0, 0.0, 0.0]])["first"]
    d = app.execute("mesh.elements_create", shape="line3", connectivity=[[f, f + 2, f + 1], [f + 2, f + 4, f + 3]], type="D")["first"]
    air = app.model.materials.create(name="AIR")
    air.set_fluid_constants(data=[[1005.0, 1.8e-5]]), air.set_specific_gas_constant(value=287.0)
    P.create_fluid(name="ORI", material=air.id, section_type="ORIFICE CD1", constants=[20.0, 5.0, 2.0, 0.0, 0.0, 0.0],
                   target={"type": "elements", "ids": [d]})
    P.create_fluid(name="PIPE", material=air.id, section_type="LIQUID PIPE MANNING", constants=[10.0, 50.0, 0.01, 1.0], oil="WATER",
                   target={"type": "elements", "ids": [d + 1]})
    # 사용자 섹션(PRP-24): U1 요소에 상수 3개
    u = _lines(app, [[n, n + 2]])["first"]
    app.execute("mesh.set_element_type", ids=[u], type="U1")
    P.create_user(name="US", material=mat.id, constants=[1.0, 2.0, 3.0], target={"type": "elements", "ids": [u]})
    # 부분구조(PRP-25): *MATRIX ASSEMBLE 은 *ELEMENT 를 대신하므로 대상 요소가 없다. 이름 4자 이내, 행렬 파일 없으면 검사가 지목
    (tmp_path / "part.sti").write_text("1,1,1,1,100.\n")
    sub = P.create_substructure(name="SUB1", stiffness_file="part.sti", mass_file="part.mas")
    case.update(work_directory=str(tmp_path))
    issues = app.execute("case.check", id=case.id)
    assert [i["field"] for i in issues if i["code"] == "missing_file"] == ["mass_file"]
    sub.update(mass_file=str(tmp_path / "part.sti"))
    assert [i for i in app.execute("case.check", id=case.id) if i["code"] in ("missing_file", "name_too_long")] == []
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    fs = {c[1]["TYPE"]: c for c in find(cards, "FLUID SECTION")}
    assert fs["ORIFICE CD1"][1]["MATERIAL"] == "AIR" and fs["ORIFICE CD1"][2] == ["20, 5, 2, 0, 0, 0"]
    assert fs["LIQUID PIPE MANNING"][1]["OIL"] == "WATER" and fs["LIQUID PIPE MANNING"][2] == ["10, 50, 0.01, 1"]
    assert find(cards, "ELEMENT", TYPE="D")[0][2] == [f"{d}, {f}, {f + 2}, {f + 1}", f"{d + 1}, {f + 2}, {f + 4}, {f + 3}"]
    us, = find(cards, "USER SECTION")
    assert us[1]["MATERIAL"] == "STEEL" and us[1]["CONSTANTS"] == "3" and us[2] == ["1, 2, 3"]
    ma, = find(cards, "MATRIX ASSEMBLE")
    assert ma[1]["NAME"] == "SUB1" and ma[1]["STIFFNESS FILE"] == "part.sti" and ma[1]["MASS FILE"].endswith("part.sti") and ma[2] == []
    # 이름이 길면 경고·검사
    sub.rename(name="SUBSTRUCT")
    assert "name_too_long" in {i["code"] for i in app.execute("case.check", id=case.id)}
    assert "name_too_long" in {w["code"] for w in app.execute("case.preview_deck", id=case.id)["warnings"]}
