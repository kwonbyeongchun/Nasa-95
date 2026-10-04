"""덱 출력 보충: 사용자 서브루틴 BC(BC-37), 부분구조 유지 자유도(BC-38), 열전달 유사 해석 표기(BC-39, LOD-40),
스텝 중 섹션 변경(PRP-26), CalculiX 요소 타입 이름(MSH-27~31, MSH-34, MSH-35).

TC: .agent/tests/tc-04-load-bc.md BC-T06-10·11·12·18·19·20, LOD-T06-10·18 / tc-03 PRP-T02-20·27 / tc-02 MSH-T08-13~21.
"""
import pytest

from openfep import OfepError

from meshutil import block, plate
from test_CASE_deck import _nodes_of, cantilever, deck, find, steel


@pytest.mark.feature("BC-37")
@pytest.mark.feature("CAS-42")
def test_BC_T06_10_18_user_subroutine_bc(app, tmp_path):
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static()
    n = b["node"]
    step.bcs.create_user(target={"type": "nodes", "ids": [n(0, 0, 0), n(0, 1, 0)]}, dofs=[1, 3])
    r, cards = deck(app, case)
    assert r["skipped"] == []
    bnd = [c for c in find(cards, "BOUNDARY") if c[1].get("USER") is True]
    assert len(bnd) == 1 and [l.split(", ")[1:] for l in bnd[0][2]] == [["1", "1"], ["3", "3"]]
    assert _nodes_of(cards, bnd[0][2][0].split(",")[0]) == {n(0, 0, 0), n(0, 1, 0)}
    # 케이스가 서브루틴 필요로 표시된다(BC-T06-18): 전용 실행 파일을 지정하면 경고가 사라진다
    issues = [i for i in app.execute("case.check", id=case.id) if i["code"] == "user_subroutines"]
    assert len(issues) == 1 and [x["what"] for x in issues[0]["needs"]] == ["*BOUNDARY, USER (uboun)"]
    exe = tmp_path / "ccx_user.exe"
    exe.write_bytes(b"")
    case.update(solver_executable=str(exe))
    assert [i for i in app.execute("case.check", id=case.id) if i["code"] == "user_subroutines"] == []
    # 사용자 재료·크리프 LAW=USER 도 같은 경고에 모인다
    case.update(solver_executable=None)  # null 은 속성 제거
    um = steel(app, "UM")
    um.set_user(constants=2, data=[[1.0, 2.0]])
    um.set_creep(law="user")
    app.model.properties.create_solid(name="S2", material=um.id, target={"type": "elements", "ids": [8]})
    issues = [i for i in app.execute("case.check", id=case.id) if i["code"] == "user_subroutines"]
    assert len(issues) == 1 and {x["what"] for x in issues[0]["needs"]} == {"*BOUNDARY, USER (uboun)", "*USER MATERIAL (umat)", "*CREEP, LAW=USER (creep)"}
    case.update(user_subroutines=True)
    assert [i for i in app.execute("case.check", id=case.id) if i["code"] == "user_subroutines"] == []


@pytest.mark.feature("BC-38")
def test_BC_T06_11_19_retained_dofs(app):
    part, b, mat, case = cantilever(app)
    n = b["node"]
    sg = case.steps.create_substructure_generate(stiffness=True, mass=True, output_file="sub")
    assert "missing_retained_dofs" in {i["code"] for i in app.execute("case.check", id=case.id)}  # BC-T06-19
    tip = app.model.sets.create_node(name="TIP", ids=[n(4, 0, 0), n(4, 2, 1)])
    app.execute("substructure.retained_dofs", id=sg.id, retained_dofs=[{"target": {"type": "set", "ids": [tip.id]}, "first_dof": 1, "last_dof": 3},
                                                                        {"target": {"type": "nodes", "ids": [n(0, 0, 0)]}, "first_dof": 2}])
    assert "missing_retained_dofs" not in {i["code"] for i in app.execute("case.check", id=case.id)}
    r, cards = deck(app, case)
    assert r["skipped"] == []
    kinds = [c[0] for c in cards]
    assert kinds.index("SUBSTRUCTURE GENERATE") < kinds.index("RETAINED NODAL DOFS") < kinds.index("SUBSTRUCTURE MATRIX OUTPUT")
    rd, = find(cards, "RETAINED NODAL DOFS")
    assert rd[2][0] == "TIP, 1, 3" and rd[2][1].endswith(", 2") and _nodes_of(cards, rd[2][1].split(",")[0]) == {n(0, 0, 0)}
    assert find(cards, "SUBSTRUCTURE MATRIX OUTPUT")[0][1] == {"STIFFNESS": "YES", "MASS": "YES", "OUTPUT FILE": "sub"}


@pytest.mark.feature("BC-39")
@pytest.mark.feature("LOD-40")
@pytest.mark.feature("CAS-02")
def test_BC_T06_12_20_LOD_T06_10_18_thermal_analogy_labels(app):
    """열전달 유사 해석: 사용자에게는 분야 용어(수두·전위·농도·음압)로 보이고, 덱은 열전달 카드(자유도 11, *DFLUX) 그대로다."""
    part, b, mat, case = cantilever(app)
    mat.set_conductivity(data=[[1e-3]])
    step = case.steps.create_heat_transfer(steady_state=True)
    n = b["node"]
    inlet = app.model.sets.create_node(name="IN", ids=[n(0, j, k) for j in range(3) for k in range(2)])
    step.bcs.create_temperature(target={"type": "set", "ids": [inlet.id]}, value=10.0)
    face = app.model.sets.create_surface(name="OUT", faces=[[4, 4], [8, 4]])
    step.loads.create_surface_flux(target={"type": "set", "ids": [face.id]}, value=2.5)
    expect = {"groundwater": ("총수두 h", "vₙ"), "electrostatic": ("전위 V", "Dₙ / Eₙ = jₙ/σ"), "diffusion": ("농도 ρ_A / C_A", "j_Aₙ / J*_Aₙ"),
              "acoustic": ("압력 p", "ρ0(aₙ−fₙ)")}
    for physics, (temp, flux) in expect.items():
        app.execute("case.set_physics", id=case.id, physics=physics)
        labels = app.execute("case.physics_labels", id=case.id)
        assert labels["thermal_analogy"] is True and labels["temperature"] == temp and labels["normal_flux"] == flux
        assert labels["deck_keywords"] == {"conductivity": "*CONDUCTIVITY", "capacity": "*SPECIFIC HEAT + *DENSITY", "temperature": "*BOUNDARY dof 11",
                                           "normal_flux": "*DFLUX", "heat_source": "*DFLUX BF"}
        r, cards = deck(app, case)
        assert r["skipped"] == []
        assert find(cards, "BOUNDARY")[0][2] == ["IN, 11, 11, 10"]
        df, = find(cards, "DFLUX")
        assert len(df[2]) == 1 and df[2][0].endswith(", S4, 2.5") and find(cards, "HEAT TRANSFER")[0][1] == {"STEADY STATE": True}
        assert find(cards, "ELSET", ELSET=df[2][0].split(",")[0])[0][2] == ["4, 8"]  # 면 셋 → 같은 면 번호의 요소 셋
        assert find(cards, "CONDUCTIVITY")[0][2] == ["0.001"]
    app.execute("case.set_physics", id=case.id, physics="thermal")
    assert app.execute("case.physics_labels", id=case.id)["temperature"] == "온도"
    with pytest.raises(OfepError) as e:
        app.execute("case.set_physics", id=case.id, physics="magic")
    assert e.value.code == "out_of_range" and e.value.details.get("param") == "physics"


@pytest.mark.feature("PRP-26")
def test_PRP_T02_20_27_change_section(app):
    part, b, mat, case = cantilever(app)
    step1 = case.steps.create_static()
    step2 = case.steps.create_static()
    soft = steel(app, "SOFT")
    tip = app.model.sets.create_element(name="TIPE", ids=[4, 8])
    ch = step2.changes.create_change_section(target={"type": "set", "ids": [tip.id]}, material=soft.id)
    r, cards = deck(app, case)
    assert r["skipped"] == []
    steps = [i for i, c in enumerate(cards) if c[0] == "STEP"]
    idx = [i for i, c in enumerate(cards) if c[0] == "CHANGE SOLID SECTION"]
    assert len(idx) == 1 and steps[1] < idx[0]  # 스텝 2 안
    assert cards[idx[0]][1] == {"ELSET": "TIPE", "MATERIAL": "SOFT"} and cards[idx[0]][2] == []
    # 쉘 요소에는 쓸 수 없다(PRP-T02-27): 검사가 지목하고 덱에서 건너뛴다
    pl = plate(app, 1, 1)
    app.model.properties.create_shell(name="SH", material=mat.id, thickness=1.0, target={"type": "elements", "ids": [pl["elements"][0]]})
    ch2 = step2.changes.create_change_section(target={"type": "elements", "ids": [pl["elements"][0]]}, material=soft.id)
    assert [i["object"] for i in app.execute("case.check", id=case.id) if i["code"] == "solid_section_only"] == [ch2.id]
    r, cards = deck(app, case)
    assert [(x["object"], x["reason"]) for x in r["skipped"]] == [(ch2.id, "solid_section_only")]
    assert len(find(cards, "CHANGE SOLID SECTION")) == 1


@pytest.mark.feature("MSH-27")
@pytest.mark.feature("MSH-28")
@pytest.mark.feature("MSH-29")
@pytest.mark.feature("MSH-30")
@pytest.mark.feature("MSH-31")
@pytest.mark.feature("MSH-34")
@pytest.mark.feature("MSH-35")
def test_MSH_T08_13_to_21_element_type_names_in_deck(app):
    """형상별 솔버 타입 목록(mesh.element_types)과 덱의 TYPE 이름. 타입 이름은 CalculiX 매뉴얼 6.2 의 요소 이름 그대로."""
    types = {t["shape"]: t for t in app.execute("mesh.element_types")}
    assert types["hex8"]["types"] == ["C3D8", "C3D8R", "C3D8I", "F3D8"] and types["hex8"]["default_type"] == "C3D8"
    assert types["tet4"]["types"] == ["C3D4", "F3D4"] and types["wedge6"]["types"] == ["C3D6", "F3D6"]
    assert types["hex20"]["types"] == ["C3D20", "C3D20R"] and types["tet10"]["types"] == ["C3D10", "C3D10T"] and types["wedge15"]["types"] == ["C3D15"]
    assert types["quad4"]["types"] == ["S4", "S4R", "M3D4", "M3D4R", "CPS4", "CPS4R", "CPE4", "CPE4R", "CAX4", "CAX4R"]
    assert types["quad8"]["types"] == ["S8", "S8R", "M3D8", "M3D8R", "CPS8", "CPS8R", "CPE8", "CPE8R", "CAX8", "CAX8R"]
    assert types["tri3"]["types"] == ["S3", "M3D3", "CPS3", "CPE3", "CAX3", "US3"] and types["tri6"]["types"] == ["S6", "M3D6", "CPS6", "CPE6", "CAX6"]
    assert types["line2"]["types"] == ["B31", "B31R", "B21", "T3D2", "T2D2", "SPRINGA", "SPRING2", "DASHPOTA", "GAPUNI", "U1"]
    assert types["line3"]["types"] == ["B32", "B32R", "T3D3", "D"] and types["point1"]["types"] == ["MASS", "SPRING1", "DCOUP3D"]
    assert types["pyramid5"]["types"] == [] and types["pyramid5"]["default_type"] == ""
    mat = steel(app)
    case = app.model.cases.create(name="types")
    case.steps.create_static()
    # 3D 솔리드·유체(MSH-T08-13·15·20): 육면체 셋을 타입별로 나눈다
    b = block(app, 4, 1, 1)
    for i, t in enumerate(["C3D8R", "C3D8I", "F3D8"]):
        app.execute("mesh.set_element_type", ids=[2 + i], type=t)
    # 쉘·멤브레인·2D(MSH-T08-16·17·07)
    pl = plate(app, 6, 1)
    for i, t in enumerate(["S4R", "M3D4", "M3D4R", "CPS4", "CPE4R", "CAX4"]):
        app.execute("mesh.set_element_type", ids=[pl["elements"][0] + i], type=t)
    tri = app.execute("mesh.elements_create", shape="tri3", connectivity=[[pl["node"](0, 0), pl["node"](1, 0), pl["node"](0, 1)]], type="US3")["first"]
    # 보·트러스·사용자(MSH-T08-18·14·21)
    n = app.execute("mesh.nodes_create", coords=[[i * 10.0, 50.0, 0.0] for i in range(5)])["first"]
    ln = app.execute("mesh.elements_create", shape="line2", connectivity=[[n + i, n + i + 1] for i in range(4)])["first"]
    for i, t in enumerate(["B31", "B31R", "T3D2", "U1"]):
        app.execute("mesh.set_element_type", ids=[ln + i], type=t)
    with pytest.raises(OfepError) as e:
        app.execute("mesh.set_element_type", ids=[ln], type="C3D8")  # 형상에 맞지 않는 타입
    assert e.value.code == "out_of_range" and "C3D8" in e.value.message
    P = app.model.properties
    P.create_solid(material=mat.id, target={"type": "elements", "ids": [1, 2, 3]})
    P.create_shell(material=mat.id, thickness=1.0, target={"type": "elements", "ids": [pl["elements"][0], tri]})
    P.create_membrane(material=mat.id, thickness=1.0, target={"type": "elements", "ids": [pl["elements"][0] + 1, pl["elements"][0] + 2]})
    P.create_solid(material=mat.id, thickness=1.0, target={"type": "elements", "ids": [pl["elements"][0] + 3, pl["elements"][0] + 4, pl["elements"][0] + 5]})
    P.create_beam(material=mat.id, section="rect", dimensions=[2.0, 1.0], target={"type": "elements", "ids": [ln, ln + 1]})
    P.create_truss(material=mat.id, area=3.0, target={"type": "elements", "ids": [ln + 2]})
    P.create_user(material=mat.id, constants=[1.0], target={"type": "elements", "ids": [ln + 3]})
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    by_type = {c[1]["TYPE"]: [int(l.split(",")[0]) for l in c[2]] for c in find(cards, "ELEMENT")}
    assert by_type["C3D8"] == [1] and by_type["C3D8R"] == [2] and by_type["C3D8I"] == [3] and by_type["F3D8"] == [4]
    e0 = pl["elements"][0]
    assert by_type["S4R"] == [e0] and by_type["M3D4"] == [e0 + 1] and by_type["M3D4R"] == [e0 + 2]
    assert by_type["CPS4"] == [e0 + 3] and by_type["CPE4R"] == [e0 + 4] and by_type["CAX4"] == [e0 + 5] and by_type["US3"] == [tri]
    assert by_type["B31"] == [ln] and by_type["B31R"] == [ln + 1] and by_type["T3D2"] == [ln + 2] and by_type["U1"] == [ln + 3]
    # 사용자 요소: U1 은 *USER ELEMENT 정의가 앞서고, US3 는 매뉴얼에 정의가 없어 경고로 알린다
    assert find(cards, "USER ELEMENT")[0][1]["TYPE"] == "U1"
    assert [w["code"] for w in r["warnings"] if w["code"] == "user_element_definition"] == ["user_element_definition"]
    assert "US3" in [w for w in r["warnings"] if w["code"] == "user_element_definition"][0]["message"]
    # 유체 요소에도 섹션은 솔리드 섹션이다(F3D8 요소 4 는 프로퍼티가 없어 경고)
    assert any(w["code"] == "unassigned_elements" for w in r["warnings"])


@pytest.mark.feature("CAS-43")
def test_CAS_T02_12_increments_exceed_max(app):
    """고정 증분 스텝(모달 동해석·direct)에서 주기 / 증분이 최대 증분 수보다 크면 경고로 그 스텝을 지목한다(솔버는 'max. # of increments reached' 로 멈춘다)."""
    part, b, mat, case = cantilever(app)
    case.steps.create_frequency(num_modes=3, storage=True)
    md = case.steps.create_modal_dynamic(initial_increment=0.01, period=1.2)  # 120 증분 > 기본 100
    issues = [i for i in app.execute("case.check", id=case.id) if i["code"] == "increments_exceed_max"]
    assert len(issues) == 1 and issues[0]["severity"] == "warning" and issues[0]["object"] == md.id
    md.update(max_increments=200)
    assert [i for i in app.execute("case.check", id=case.id) if i["code"] == "increments_exceed_max"] == []
    # 자동 증분(정적, direct 아님)은 증분 수가 바뀌므로 경고하지 않는다
    case.steps.create_static(initial_increment=0.001, period=1.0)
    assert [i for i in app.execute("case.check", id=case.id) if i["code"] == "increments_exceed_max"] == []


@pytest.mark.feature("LOD-08")
def test_LOD_T01_28_pretension_section_bridged(app):
    """프리텐션 단면(매뉴얼 7.106): 단면 노드를 쓰는 요소가 모두 단면에 면을 대야 한다. 사면체 메시의 임의 절단면은 변·꼭짓점으로만 닿는 요소가 있어 경고,
    매핑 육면체의 층 경계 단면은 경고 없음."""
    import numpy as np
    bolt = app.model.parts.create(name="BOLT")  # 두 토막 + 공유 토폴로지: 사면체 메시에 z=20 절단면이 생긴다
    bolt.features.create_box(origin=[-5.0, -5.0, 0.0], size=[10.0, 10.0, 20.0])
    bolt.features.create_box(origin=[-5.0, -5.0, 20.0], size=[10.0, 10.0, 20.0])
    bolt.features.create_share_topology()
    mat = app.model.materials.create(name="M")
    mat.set_elastic(data=[[210000.0, 0.3]])

    def section_faces(table):
        xyz = dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords()))
        lower = app.execute("mesh.find", what="elements", box_min=[-6, -6, -1], box_max=[6, 6, 20.01])["ids"]
        free = app.execute("mesh.free_faces", ids=lower)["faces"]
        elems = {e["id"]: e for e in app.execute("mesh.elements", ids=sorted({f[0] for f in free}))}
        return [[e, f] for e, f in free if all(abs(xyz[elems[e]["nodes"][k]][2] - 20.0) < 1e-6 for k in table[f - 1])]

    def build(faces):
        surf = app.model.sets.create_surface(name="SEC", faces=faces)
        ref = app.execute("mesh.nodes_create", coords=[[0, 0, -10]])["ids"][0]
        case = app.model.cases.create(name="c")
        step = case.steps.create_static()
        step.loads.create_pretension(target={"type": "set", "ids": [surf.id]}, node=ref, direction=[0.0, 0.0, 1.0], force=1000.0)
        return case

    tet_faces = [[0, 1, 2], [0, 3, 1], [1, 3, 2], [2, 3, 0]]
    r = app.execute("mesh.generate", id=bolt.id, size=3.0)
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [r["mesh_part"]]})
    faces = section_faces(tet_faces)
    assert len(faces) > 0
    case = build(faces)
    issues = [i for i in app.execute("case.check", id=case.id) if i["code"] == "pretension_section_bridged"]
    assert len(issues) == 1 and issues[0]["severity"] == "warning" and "개" in issues[0]["message"]
    # 매핑 육면체: 단면이 요소 층 경계 → 모든 접촉 요소가 면을 댄다
    app.execute("project.new")
    bolt = app.model.parts.create(name="BOLT")
    bolt.features.create_box(origin=[-5.0, -5.0, 0.0], size=[10.0, 10.0, 40.0])
    mat = app.model.materials.create(name="M")
    mat.set_elastic(data=[[210000.0, 0.3]])
    hex_faces = [[0, 1, 2, 3], [4, 7, 6, 5], [0, 4, 5, 1], [1, 5, 6, 2], [2, 6, 7, 3], [3, 7, 4, 0]]
    r = app.execute("mesh.generate", id=bolt.id, method="hex_mapped", divisions=[2, 2, 8])
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [r["mesh_part"]]})
    faces = section_faces(hex_faces)
    assert len(faces) == 4
    case = build(faces)
    assert [i for i in app.execute("case.check", id=case.id) if i["code"] == "pretension_section_bridged"] == []
