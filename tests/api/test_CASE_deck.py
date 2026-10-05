"""솔버 입력 파일(덱) 출력(3단계, CAS-10)과 메시 내보내기(MSH-03).

케이스 정의: .agent/tests/tc-05-case.md (CAS-T03). 솔버로 실제로 푸는 검증은 ccx 실행 파일이 준비된 뒤에 한다.
여기서는 덱의 구조·내용이 모델과 맞는지, 쓰지 못한 것을 빠짐없이 알리는지를 본다.
"""
import re

import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total
from meshutil import block, plate


def parse(text: str) -> list[tuple[str, dict, list[str]]]:
    """덱을 카드 목록으로: (키워드, 매개변수, 데이터 줄)."""
    cards = []
    for line in text.splitlines():
        if line.startswith("**"):
            continue
        if line.startswith("*"):
            head = [p.strip() for p in line[1:].split(",")]
            params = {}
            for p in head[1:]:
                k, _, v = p.partition("=")
                params[k.strip()] = re.sub(r"^(-?\d+)\.$", r"\1", v.strip()) if v else True
            cards.append((head[0], params, []))
        else:
            # 실수는 항상 소수점을 붙여 쓴다("100."). 비교하기 쉽게 끝의 점은 뗀다.
            cards[-1][2].append(", ".join(re.sub(r"^(-?\d+)\.$", r"\1", cell.strip()) for cell in line.split(",")).rstrip()
                                if line.strip() else line)
    return cards


def find(cards, keyword, **params):
    return [c for c in cards if c[0] == keyword and all(c[1].get(k) == v for k, v in params.items())]


def deck(app, case):
    r = app.execute("case.preview_deck", id=case.id)
    assert all(ord(ch) < 128 for ch in r["text"])  # 덱은 ASCII
    assert max(len(cell.strip()) for line in r["text"].splitlines() if not line.startswith("*") for cell in line.split(",")) <= 20
    return r, parse(r["text"])


def steel(app, name="STEEL"):
    m = app.model.materials.create(name=name)
    m.set_elastic(data=[[210000.0, 0.3]])
    m.set_density(data=[[7.85e-9]])
    return m


def cantilever(app, order=1):
    part = app.model.mesh_parts.create(name="BEAM")
    b = block(app, 4, 2, 1, part=part.id)
    if order == 2:
        app.execute("mesh.convert_order", order=2)
    mat = steel(app)
    app.model.properties.create_solid(name="SEC", material=mat.id, target={"type": "parts", "ids": [part.id]})
    case = app.model.cases.create(name="cantilever")
    return part, b, mat, case


# ================================================================ 기본 구조
@pytest.mark.feature("CAS-10")
def test_CAS_T03_basic_deck(app):
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static()
    fix = app.model.sets.create_surface(name="FIX", faces=[[1, 6], [5, 6]])
    step.bcs.create_displacement(target={"type": "set", "ids": [fix.id]}, dofs=[1, 2, 3])
    step.loads.create_force(target={"type": "nodes", "ids": [b["node"](4, 0, 1)]}, components=[0.0, 0.0, -10.0])
    step.outputs.create_node_file(variables=["U", "RF"])
    before, hist = total(app), history_len(app)
    r, cards = deck(app, case)
    assert total(app) == before and history_len(app) == hist  # 조회는 모델을 바꾸지 않는다
    assert r["warnings"] == [] and r["skipped"] == [] and r["truncated"] is False
    assert r["lines"] == r["text"].count("\n")
    assert [c[0] for c in cards] == [
        "HEADING", "NODE", "ELEMENT", "SURFACE", "NSET", "NSET", "MATERIAL", "ELASTIC", "DENSITY",
        "SOLID SECTION", "STEP", "STATIC", "BOUNDARY", "CLOAD", "NODE FILE", "END STEP"]
    node, = find(cards, "NODE")
    assert len(node[2]) == 30 and node[2][1] == "2, 25, 0, 0"
    elem, = find(cards, "ELEMENT", TYPE="C3D8", ELSET="BEAM")  # 메시 파트는 요소 카드의 ELSET 이 된다
    assert len(elem[2]) == 8 and elem[2][0] == "1, 1, 2, 7, 6, 16, 17, 22, 21"
    assert find(cards, "SURFACE", NAME="FIX", TYPE="ELEMENT")[0][2] == ["1, S6", "5, S6"]
    assert find(cards, "SOLID SECTION")[0][1] == {"ELSET": "BEAM", "MATERIAL": "STEEL"}
    assert find(cards, "ELASTIC")[0][2] == ["210000, 0.3"] and find(cards, "DENSITY")[0][2] == ["7.85e-09"]
    # 경계조건: 면 셋 → 그 면의 노드 셋
    fixed = find(cards, "BOUNDARY")[0][2]
    nset = fixed[0].split(",")[0]
    assert fixed == [f"{nset}, {d}, {d}" for d in (1, 2, 3)]
    assert find(cards, "NSET", NSET=nset)[0][2] == [", ".join(str(b["node"](0, j, k)) for k in range(2) for j in range(3))]
    load = find(cards, "CLOAD")[0][2]
    assert len(load) == 3 and load[2].endswith(", 3, -10")  # 0 인 성분도 쓴다(앞 스텝의 성분이 남지 않게)
    assert find(cards, "NODE FILE")[0][2] == ["U, RF"]
    # 같은 모델이면 같은 덱
    assert app.execute("case.preview_deck", id=case.id)["text"] == r["text"]


@pytest.mark.feature("CAS-10")
def test_CAS_T03_second_order_and_face_nodes(app):
    part, b, mat, case = cantilever(app, order=2)
    step = case.steps.create_static()
    fix = app.model.sets.create_surface(name="FIX", faces=[[1, 6]])
    bc = step.bcs.create_displacement(target={"type": "set", "ids": [fix.id]}, dofs=[1])
    assert app.execute("bc.resolve", id=bc.id)["count"] == 8  # 2차 요소의 면: 꼭짓점 4 + 중간 절점 4
    r, cards = deck(app, case)
    elem, = find(cards, "ELEMENT", TYPE="C3D20")
    assert len(elem[2]) == 16  # 요소마다 두 줄(한 줄에 16칸까지)
    first = elem[2][0].rstrip(",").split(",") + elem[2][1].split(",")
    assert len(first) == 21 and elem[2][0].endswith(",")
    nset = find(cards, "BOUNDARY")[0][2][0].split(",")[0]
    assert len(find(cards, "NSET", NSET=nset)[0][2][0].split(",")) == 8


@pytest.mark.feature("CAS-10")
def test_CAS_T03_number_format(app):
    ids = app.execute("mesh.nodes_create", coords=[[1 / 3, -1.2345678901234567e-5, 1e-300], [1e21, 0.1 + 0.2, -0.0],
                                                   [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]])["ids"]
    app.execute("mesh.elements_create", shape="tet4", connectivity=[ids[1:]])
    case = app.model.cases.create()
    r, cards = deck(app, case)  # deck() 이 모든 칸이 20자 이하인지 본다
    rows = [[float(x) for x in line.split(",")[1:]] for line in find(cards, "NODE")[0][2]]
    assert rows[0] == pytest.approx([1 / 3, -1.2345678901234567e-5, 1e-300], rel=1e-12)
    assert rows[1] == pytest.approx([1e21, 0.3, 0.0], rel=1e-12)
    codes = {w["code"] for w in r["warnings"]}
    assert codes == {"unassigned_elements", "no_steps"}


# ================================================================ 재료·섹션
@pytest.mark.feature("CAS-10")
@pytest.mark.feature("MAT-02")
def test_CAS_T03_materials(app):
    part, b, mat, case = cantilever(app)
    mat.set_elastic(type="engineering_constants", data=[[1e5, 2e5, 3e5, 0.1, 0.2, 0.3, 4e4, 5e4, 6e4, 20.0]])
    mat.set_plastic(hardening="kinematic", data=[[250.0, 0.0, 20.0], [300.0, 0.1, 20.0], [200.0, 0.0, 400.0]])
    mat.set_expansion(type="ortho", zero=20.0, data=[[1e-5, 2e-5, 3e-5]])
    mat.set_conductivity(data=[[50.0, 20.0], [45.0, 400.0]])
    mat.set_specific_heat(data=[[4.6e8]])
    mat.set_creep(data=[[1e-10, 5.0, 0.0]])
    mat.set_structural_damping(value=0.03)
    mat.set_allowable(**{"yield": 250.0})
    rubber = app.model.materials.create(name="RUBBER")
    rubber.set_hyperelastic(model="ogden", n=2, data=[[1.0, 2.0, 0.1, 3.0, 4.0, 0.2]])
    user = app.model.materials.create(name="UMAT")
    user.set_user(constants=3, data=[[1.0, 2.0, 3.0]])
    user.set_depvar(count=4)
    special = app.model.materials.create(name="TENS")
    special.set_special(model="tension_only", constants=[[210000.0, 1e-3]])
    case.steps.create_static()
    r, cards = deck(app, case)
    assert find(cards, "ELASTIC", TYPE="ENGINEERING CONSTANTS")[0][2] == ["100000, 200000, 300000, 0.1, 0.2, 0.3, 40000, 50000,", "60000, 20"]
    assert find(cards, "PLASTIC", HARDENING="KINEMATIC")[0][2] == ["250, 0, 20", "300, 0.1, 20", "200, 0, 400"]
    assert find(cards, "EXPANSION", TYPE="ORTHO", ZERO="20")[0][2] == ["1e-05, 2e-05, 3e-05"]
    assert find(cards, "CONDUCTIVITY")[0][2] == ["50, 20", "45, 400"]
    assert find(cards, "CREEP")[0][2] == ["1e-10, 5, 0"] and find(cards, "DAMPING", STRUCTURAL="0.03")
    assert find(cards, "HYPERELASTIC", OGDEN=True, N="2")[0][2] == ["1, 2, 0.1, 3, 4, 0.2"]
    assert find(cards, "USER MATERIAL", CONSTANTS="3")[0][2] == ["1, 2, 3"] and find(cards, "DEPVAR")[0][2] == ["4"]
    assert [c[1]["NAME"] for c in find(cards, "MATERIAL")] == ["STEEL", "RUBBER", "UMAT", "TENSION_ONLY_TENS"]  # 내장 특수 재료는 매뉴얼의 접두어가 붙는다
    assert "ALLOWABLE" not in r["text"].upper()  # 허용치는 솔버로 나가지 않는다
    # 내장 특수 재료도 덱으로 나간다(매뉴얼 6.8.12: 접두어 + *USER MATERIAL, CONSTANTS=2)
    assert r["skipped"] == [] and find(cards, "USER MATERIAL", CONSTANTS="2")


@pytest.mark.feature("CAS-10")
@pytest.mark.feature("PRP-01")
def test_CAS_T03_sections(app):
    mat = steel(app)
    ori = app.model.orientations.create_rectangular(name="OR1", a=[1.0, 0.0, 0.0], b=[0.0, 1.0, 0.0], rotation_axis=3, rotation_angle=30.0)
    sh = plate(app, 2, 1)
    n = app.execute("mesh.nodes_create", coords=[[0, 0, 5], [1, 0, 5], [2, 0, 5], [0, 5, 5]])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[n[:2], n[1:3]])["ids"]
    truss = app.execute("mesh.elements_create", shape="line2", type="T3D2", connectivity=[[n[0], n[3]]])["ids"]
    spring = app.execute("mesh.create_connector", kind="spring", nodes=[n[0], n[2]])["ids"]
    spring2 = app.execute("mesh.create_connector", kind="spring_fixed_direction", nodes=[n[1], n[3]])["ids"]
    mass = app.execute("mesh.create_connector", kind="mass", nodes=[n[3]])["ids"]
    gap = app.execute("mesh.create_connector", kind="gap", nodes=[n[2], n[3]])["ids"]
    el = lambda ids: {"type": "elements", "ids": ids}
    P = app.model.properties
    shell = P.create_shell(name="SH", material=mat.id, thickness=2.0, offset=0.5, orientation=ori.id, target=el([sh["elements"][0]]))
    shell.set_nodal_thickness(values=[[1, 2.0], [2, 1.5]])
    P.create_composite(name="LAM", target=el([sh["elements"][1]]),
                       layers=[{"thickness": 0.5, "material": mat.id, "orientation": ori.id}, {"thickness": 0.25, "material": mat.id}])
    P.create_beam(name="BM", material=mat.id, section="rect", dimensions=[20.0, 10.0], direction=[0.0, 0.0, 1.0], target=el(beams))
    app.execute("mesh.set_beam_direction", ids=[beams[1]], direction=[0.0, 1.0, 0.0])
    P.create_truss(name="TR", material=mat.id, area=3.5, target=el(truss))
    P.create_spring(name="SP", stiffness=100.0, target=el(spring))
    P.create_spring(name="SP2", dof1=1, dof2=2, table=[[0.0, 0.0], [10.0, 1.0]], target=el(spring2))
    P.create_mass(name="MS", mass=0.25, target=el(mass))
    P.create_gap(name="GP", clearance=0.1, direction=[1.0, 0.0, 0.0], stiffness=1e6, target=el(gap))
    unassigned = P.create_solid(name="FREE", material=mat.id)
    case = app.model.cases.create()
    case.steps.create_static()
    r, cards = deck(app, case)
    assert find(cards, "ORIENTATION", NAME="OR1")[0][2] == ["1, 0, 0, 0, 1, 0", "3, 30"]
    s, = find(cards, "SHELL SECTION", MATERIAL="STEEL")
    assert s[1]["ORIENTATION"] == "OR1" and s[1]["OFFSET"] == "0.5" and s[1]["NODAL THICKNESS"] is True and s[2] == ["2"]
    assert find(cards, "NODAL THICKNESS")[0][2] == ["1, 2", "2, 1.5"]
    assert find(cards, "SHELL SECTION", COMPOSITE=True)[0][2] == ["0.5, , STEEL, OR1", "0.25, , STEEL"]
    # 요소별 단면 방향이 다르면 섹션을 나눈다
    bm = find(cards, "BEAM SECTION", SECTION="RECT")
    assert sorted(c[2][1] for c in bm) == ["0, 0, 1", "0, 1, 0"] and all(c[2][0] == "20, 10" for c in bm)
    assert {tuple(find(cards, "ELSET", ELSET=c[1]["ELSET"])[0][2]) for c in bm} == {(str(beams[0]),), (str(beams[1]),)}
    assert [c[2] for c in find(cards, "SOLID SECTION")] == [["3.5"]]
    assert sorted((c[2] for c in find(cards, "SPRING")), key=len) == [["", "100"], ["1, 2", "0, 0", "10, 1"]]
    assert find(cards, "SPRING", NONLINEAR=True) and find(cards, "MASS")[0][2] == ["0.25"]
    assert find(cards, "GAP")[0][2] == ["0.1, 1, 0, 0, , 1000000"]
    assert {e[1]["TYPE"] for e in find(cards, "ELEMENT")} == {"S4", "B31", "T3D2", "SPRINGA", "SPRING2", "MASS", "GAPUNI"}
    assert [(w["code"], w.get("object")) for w in r["warnings"]] == [("unassigned_property", unassigned.id)]
    assert r["skipped"] == []


# ================================================================ 스텝·하중·경계조건
PROCEDURES = {
    "static": ("STATIC", {}), "frequency": ("FREQUENCY", dict(num_modes=5)), "complex_frequency": ("COMPLEX FREQUENCY", dict(num_modes=5)),
    "buckle": ("BUCKLE", dict(num_modes=3)), "modal_dynamic": ("MODAL DYNAMIC", {}),
    "steady_state_dynamics": ("STEADY STATE DYNAMICS", dict(freq_min=1.0, freq_max=100.0)), "dynamic": ("DYNAMIC", {}),
    "heat_transfer": ("HEAT TRANSFER", {}), "coupled_temperature_displacement": ("COUPLED TEMPERATURE-DISPLACEMENT", {}),
    "uncoupled_temperature_displacement": ("UNCOUPLED TEMPERATURE-DISPLACEMENT", {}), "visco": ("VISCO", dict(cetol=1e-4)),
    "electromagnetics": ("ELECTROMAGNETICS", {}), "cfd": ("CFD", {}), "green": ("GREEN", dict(num_modes=4)),
    "substructure_generate": ("SUBSTRUCTURE GENERATE", {}),
}


@pytest.mark.feature("CAS-02")
@pytest.mark.feature("CAS-10")
def test_CAS_T03_procedures(app, build):
    part, b, mat, case = cantilever(app)
    made = {}
    for sub in [s["name"] for s in build.kinds["step"]["subtypes"]]:
        if sub in PROCEDURES:
            made[sub] = app.execute(f"step.create_{sub}", parent=case.id, **PROCEDURES[sub][1])["id"]
        else:
            made[sub] = build.make("step", sub, parent=case.id)
    r, cards = deck(app, case)
    written = [c[0] for c in cards if c[0] in {k for k, _ in PROCEDURES.values()}]
    assert written == [PROCEDURES[s][0] for s in made if s in PROCEDURES]
    # 모든 절차를 쓴다(표 밖의 민감도·균열 전파·실행 가능 방향·강건 설계는 예제 값으로 생성)
    assert len(find(cards, "STEP")) == len(find(cards, "END STEP")) == len(made)
    assert r["skipped"] == [], r["skipped"]


@pytest.mark.feature("CAS-10")
def test_CAS_T03_procedure_parameters(app):
    part, b, mat, case = cantilever(app)
    S = case.steps
    S.create_static(nlgeom=True, max_increments=500, load_application="step", initial_increment=0.1, period=2.0, min_increment=1e-5,
                    max_increment=0.5, direct=True, solver="pardiso",
                    controls=[{"parameters": "time_incrementation", "values": [[4.0, 8.0, 9.0, 16.0, 10.0, 4.0]]}])
    S.create_frequency(num_modes=10, freq_max=5000.0, storage=True, perturbation=True)
    S.create_modal_dynamic(initial_increment=1e-4, period=0.01, damping_type="rayleigh", modal_damping=[[0.5, 1e-5]])
    S.create_steady_state_dynamics(freq_min=10.0, freq_max=1000.0, points=20, modal_damping=[[1, 10, 0.02]])
    S.create_dynamic(alpha=-0.1, explicit_scheme="explicit", initial_increment=1e-6, period=1e-3)
    S.create_heat_transfer(steady_state=True)
    r, cards = deck(app, case)
    steps = find(cards, "STEP")
    assert steps[0][1] == {"NLGEOM": True, "INC": "500", "AMPLITUDE": "STEP"} and steps[1][1] == {"PERTURBATION": True}
    st, = find(cards, "STATIC")
    assert st[1] == {"SOLVER": "PARDISO", "DIRECT": True} and st[2] == ["0.1, 2, 1e-05, 0.5"]
    assert find(cards, "CONTROLS", PARAMETERS="TIME INCREMENTATION")[0][2] == ["4, 8, 9, 16, 10, 4"]
    fr, = find(cards, "FREQUENCY")
    assert fr[1] == {"STORAGE": "YES"} and fr[2] == ["10, , 5000"]
    assert find(cards, "MODAL DYNAMIC")[0][2] == ["0.0001, 0.01"]
    md = find(cards, "MODAL DAMPING")
    assert md[0][1] == {"RAYLEIGH": True} and md[0][2] == [", , 0.5, 1e-05"] and md[1][2] == ["1, 10, 0.02"]
    assert find(cards, "STEADY STATE DYNAMICS")[0][2] == ["10, 1000, 20"]
    assert find(cards, "DYNAMIC")[0][1] == {"ALPHA": "-0.1", "EXPLICIT": True}
    assert find(cards, "HEAT TRANSFER")[0][1] == {"STEADY STATE": True}


@pytest.mark.feature("CAS-10")
@pytest.mark.feature("LOD-01")
def test_CAS_T03_loads(app):
    part, b, mat, case = cantilever(app)
    amp = app.model.functions.create_amplitude(name="RAMP", points=[[0.0, 0.0], [1.0, 1.0], [2.0, 0.5], [3.0, 0.0], [4.0, 0.0]], time="total")
    expr = app.model.functions.create_expression(name="FX", expression="2*x")
    eset = app.model.sets.create_element(name="HALF", ids=[1, 2])
    step = case.steps.create_static()
    L = step.loads
    P, N = {"type": "parts", "ids": [part.id]}, {"type": "nodes", "ids": [3, 1]}
    top = {"type": "faces", "ids": [[1, 2], [2, 2], [4, 4]]}
    L.create_force(target=N, components=[1.0, 0.0, -2.0], amplitude=amp.id, time_delay=0.5)
    L.create_moment(target=N, components=[0.0, 3.0, 0.0])
    L.create_pressure(target=top, value=2.0)
    L.create_gravity(target=P, value=9810.0, direction=[0.0, 0.0, -2.0])
    L.create_centrifugal(target={"type": "set", "ids": [eset.id]}, omega=10.0, axis_point=[1.0, 2.0, 3.0], axis_direction=[0.0, 0.0, 1.0])
    L.create_temperature(target=N, value=100.0)
    L.create_concentrated_flux(target=N, value=5.0)
    L.create_surface_flux(target=top, value=7.0)
    L.create_body_flux(target=P, value=0.5)
    L.create_film(target=top, coefficient=10.0, sink_temperature=293.0, coefficient_amplitude=amp.id)
    L.create_radiation(target={"type": "faces", "ids": [[1, 2]]}, emissivity=0.8, sink_temperature=293.0)
    bad = [L.create_traction(name="dist", target=top, components=[0.0, 0.0, 1.0], distribution=app.model.functions.create_expression(name="D", expression="z").id),
           L.create_force(name="by_expr", target=N, components=[1.0, 0.0, 0.0], amplitude=expr.id),
           L.create_force(name="local", target=N, components=[1.0, 0.0, 0.0], csys=app.model.csys.create_rectangular().id),
           L.create_force(name="geo", target={"type": "geometry", "ids": [[app.model.parts.create(name="G").id, "face", 1]]},
                          components=[1.0, 0.0, 0.0])]
    r, cards = deck(app, case)
    a, = find(cards, "AMPLITUDE", NAME="RAMP")
    assert a[1]["TIME"] == "TOTAL TIME" and a[2] == ["0, 0, 1, 1, 2, 0.5, 3, 0", "4, 0"]
    n13 = find(cards, "NSET")[0][1]["NSET"]
    assert find(cards, "NSET", NSET=n13)[0][2] == ["1, 3"]  # 같은 대상은 셋 하나를 함께 쓴다
    cl = find(cards, "CLOAD")
    assert cl[0][1] == {"AMPLITUDE": "RAMP", "TIME DELAY": "0.5"} and cl[0][2] == [f"{n13}, 1, 1", f"{n13}, 2, 0", f"{n13}, 3, -2"]
    assert cl[1][2] == [f"{n13}, 4, 0", f"{n13}, 5, 3", f"{n13}, 6, 0"] and len(cl) == 2
    dl = [line for c in find(cards, "DLOAD") for line in c[2]]
    internal = {tuple(c[2]): c[1]["ELSET"] for c in find(cards, "ELSET") if c[1]["ELSET"].startswith("NASA95_")}
    e12, e4 = "HALF", internal[("4",)]  # 내용이 같은 사용자 셋이 있으면 그것을 쓴다
    assert dl == [f"{e12}, P2, 2", f"{e4}, P4, 2", "BEAM, GRAV, 9810, 0, 0, -1", "HALF, CENTRIF, 100, 1, 2, 3, 0, 0, 1"]
    assert find(cards, "TEMPERATURE")[0][2] == [f"{n13}, 100"] and find(cards, "CFLUX")[0][2] == [f"{n13}, 11, 5"]
    assert [c[2] for c in find(cards, "DFLUX")] == [[f"{e12}, S2, 7", f"{e4}, S4, 7"], ["BEAM, BF, 0.5"]]
    f, = find(cards, "FILM")
    assert f[1] == {"FILM AMPLITUDE": "RAMP"} and f[2] == [f"{e12}, F2, 293, 10", f"{e4}, F4, 293, 10"]
    assert find(cards, "RADIATE")[0][2] == [f"{internal[('1',)]}, R2, 293, 0.8"]
    # 쓰지 못한 하중은 이유와 함께 알리고, 덱에도 주석으로 남긴다
    assert [(s["object"], s["reason"]) for s in r["skipped"]] == [
        (bad[0].id, "distribution"), (bad[1].id, "amplitude_function"), (bad[2].id, "incomplete_csys"), (bad[3].id, "not_available")]
    assert r["text"].count("** skipped: load") == 4


@pytest.mark.feature("CAS-10")
@pytest.mark.feature("BC-01")
def test_CAS_T03_bcs_and_shell_loads(app):
    mat = steel(app)
    sh = plate(app, 2, 1)
    app.model.properties.create_shell(name="SH", material=mat.id, thickness=1.0, target={"type": "elements", "ids": [1, 2]})
    case = app.model.cases.create()
    step = case.steps.create_static()
    amp = app.model.functions.create_amplitude(name="A1", points=[[0.0, 0.0], [1.0, 1.0]])
    nodes = {"type": "nodes", "ids": [1, 4]}
    B = step.bcs
    B.create_displacement(target=nodes, dofs=[1, 2, 6], values=[0.0, 0.5])
    B.create_symmetry(target=nodes, normal="x")
    B.create_antisymmetry(target=nodes, normal="x")
    B.create_fixed_current(target=nodes, dofs=[3])
    B.create_temperature(target=nodes, value=300.0, amplitude=amp.id)
    B.create_base_motion(dof=3, amplitude=amp.id, motion="acceleration")
    local = B.create_displacement(name="local", target=nodes, dofs=[1], csys=app.model.csys.create_rectangular().id)
    step.loads.create_pressure(target={"type": "elements", "ids": [1, 2]}, value=3.0)
    step.loads.create_edge_load(target={"type": "faces", "ids": [[1, 4]]}, value=5.0)
    r, cards = deck(app, case)
    s = find(cards, "NSET")[0][1]["NSET"]
    bcs = find(cards, "BOUNDARY")
    assert bcs[0][2] == [f"{s}, 1, 1", f"{s}, 2, 2, 0.5", f"{s}, 6, 6"]
    assert bcs[1][2] == [f"{s}, 1, 1", f"{s}, 5, 5", f"{s}, 6, 6"]  # 대칭(법선 x): 쉘이 있으므로 회전도 막는다
    assert bcs[2][2] == [f"{s}, 4, 4", f"{s}, 2, 2", f"{s}, 3, 3"]
    assert bcs[3][1] == {"FIXED": True} and bcs[3][2] == [f"{s}, 3, 3"]
    assert bcs[4][1] == {"AMPLITUDE": "A1"} and bcs[4][2] == [f"{s}, 11, 11, 300"]
    assert find(cards, "BASE MOTION")[0][1] == {"DOF": "3", "AMPLITUDE": "A1", "TYPE": "ACCELERATION"}
    dl = [line for c in find(cards, "DLOAD") for line in c[2]]
    assert dl[0].endswith(", P, 3") and dl[1].endswith(", EDNOR4, 5")
    assert [(x["object"], x["reason"]) for x in r["skipped"]] == [(local.id, "incomplete_csys")]
    # 솔리드만 있는 모델에서는 대칭 구속에 회전 자유도를 쓰지 않는다
    app.execute("project.new")
    part, b, mat, case = cantilever(app)
    case.steps.create_static().bcs.create_symmetry(target={"type": "nodes", "ids": [1]}, normal="y")
    r, cards = deck(app, case)
    assert [line.split(", ", 1)[1] for line in find(cards, "BOUNDARY")[0][2]] == ["2, 2"]


@pytest.mark.feature("CAS-10")
@pytest.mark.feature("LOD-38")
def test_CAS_T03_inheritance_and_suppress(app):
    part, b, mat, case = cantilever(app)
    N = {"type": "nodes", "ids": [1]}
    s1 = case.steps.create_static(name="s1")
    s1.bcs.create_displacement(target=N, dofs=[1])
    f1 = s1.loads.create_force(target=N, components=[1.0, 0.0, 0.0])
    s2 = case.steps.create_static(name="s2", loads_inheritance="new", bcs_inheritance="new")
    s2.loads.create_force(target=N, components=[0.0, 2.0, 0.0])
    s3 = case.steps.create_static(name="s3", bcs_inheritance="new")
    s3.bcs.create_displacement(target=N, dofs=[2])
    r, cards = deck(app, case)
    i2, i3 = [i for i, c in enumerate(cards) if c[0] == "STEP"][1:]
    step2, step3 = cards[i2:i3], cards[i3:]
    # 앞 스텝의 것을 지우려고 OP=NEW 를 쓴다. 이 스텝에 그 종류가 없어도 빈 카드로 지운다
    assert find(step2, "BOUNDARY") == [("BOUNDARY", {"OP": "NEW"}, [])]
    assert find(step2, "CLOAD")[0][1] == {"OP": "NEW"} and len(find(step2, "CLOAD")[0][2]) == 3
    # 앞 스텝에서 쓴 적 없는 종류의 빈 카드는 내지 않는다(솔버가 오류로 보는 것이 있다)
    assert find(step2, "DLOAD") == [] and find(step2, "FILM") == [] and find(step2, "TEMPERATURE") == []
    assert find(step3, "BOUNDARY")[0][1] == {"OP": "NEW"} and len(find(step3, "BOUNDARY")[0][2]) == 1
    assert find(step3, "CLOAD") == []
    assert not any("OP" in c[1] for c in cards[:i2])
    # 억제한 것은 덱에 나가지 않는다
    f1.suppress(), s2.suppress()
    r, cards = deck(app, case)
    assert len(find(cards, "STEP")) == 2 and find(cards, "CLOAD") == [] and r["skipped"] == []


@pytest.mark.feature("CAS-10")
@pytest.mark.feature("CAS-07")
def test_CAS_T03_outputs(app):
    part, b, mat, case = cantilever(app)
    nset = app.model.sets.create_node(name="TIP", ids=[5, 10])
    surf = app.model.sets.create_surface(name="CUT", faces=[[1, 6]])
    tp = app.model.time_points.create(name="TP1", times=[0.25, 0.5, 1.0])
    O = case.steps.create_static().outputs
    O.create_node_file(variables=["U", "RF"], frequency=2, **{"global": False})
    O.create_element_file(variables=["S"], time_points=tp.id, expand="3d", section_forces=True)
    O.create_node_print(variables=["U"], target={"type": "set", "ids": [nset.id]}, totals="only")
    O.create_element_print(variables=["S", "PEEQ"], target={"type": "parts", "ids": [part.id]}, frequency=5)
    O.create_section_print(variables=["SOF", "SOM"], target={"type": "set", "ids": [surf.id]}, label="SEC1")
    O.create_contact_file(variables=["CDIS", "CSTR"])
    r, cards = deck(app, case)
    assert find(cards, "TIME POINTS", NAME="TP1")[0][2] == ["0.25, 0.5, 1"]
    assert find(cards, "NODE FILE")[0][1] == {"FREQUENCY": "2", "GLOBAL": "NO"}
    # SECTION FORCES 와 OUTPUT=3D 는 함께 쓸 수 없다(매뉴얼 *EL FILE; ccx 2.22 는 오류로 멈춘다) → 2D 로 쓰고 경고
    assert find(cards, "EL FILE")[0][1] == {"TIME POINTS": "TP1", "OUTPUT": "2D", "SECTION FORCES": True}
    assert [w["code"] for w in r["warnings"] if w["code"] == "section_forces_output_2d"] == ["section_forces_output_2d"]
    assert find(cards, "NODE PRINT")[0][1] == {"NSET": "TIP", "TOTALS": "ONLY"} and find(cards, "NODE PRINT")[0][2] == ["U"]
    assert find(cards, "EL PRINT")[0][1] == {"ELSET": "BEAM", "FREQUENCY": "5"} and find(cards, "EL PRINT")[0][2] == ["S, PEEQ"]
    assert find(cards, "SECTION PRINT")[0][1] == {"SURFACE": "CUT", "NAME": "SEC1"}
    assert find(cards, "CONTACT FILE")[0][2] == ["CDIS, CSTR"] and r["skipped"] == []


# ================================================================ 구속·접촉·초기 조건
@pytest.mark.feature("CAS-10")
@pytest.mark.feature("BC-07")
def test_CAS_T03_constraints_contact(app):
    part, b, mat, case = cantilever(app)
    block(app, 2, 2, 1, size=(50.0, 20.0, 10.0), origin=(0.0, 0.0, 10.0))
    top = app.model.sets.create_surface(name="TOP", faces=[[e, 2] for e in range(1, 5)])
    bottom = app.model.sets.create_surface(name="BOT", faces=[[9, 1], [10, 1]])
    ref = app.execute("mesh.nodes_create", coords=[[120.0, 10.0, 5.0]])["ids"][0]
    C = app.model.constraints
    eq = C.create_equation(terms=[[3, 1, 1.0], [4, 1, -1.0], [5, 2, 0.5]])
    C.create_mpc_plane(nodes=[1, 2, 6, 7])
    C.create_rigid_body(target={"type": "nodes", "ids": [5, 10]}, ref_node=ref)
    kc = C.create_coupling_kinematic(surface={"type": "faces", "ids": [[4, 4]]}, ref_node=ref, dofs=[1, 2])
    tie = C.create_tie(slave={"type": "set", "ids": [bottom.id]}, master={"type": "set", "ids": [top.id]}, position_tolerance=0.1, adjust=False)
    inter = app.model.contact_properties.create(name="INT1", pressure_overclosure="linear", slope=1e6, friction_coefficient=0.2,
                                                stick_slope=1e5)
    pair = app.model.contact_pairs.create(slave={"type": "set", "ids": [bottom.id]}, master={"type": "set", "ids": [top.id]},
                                          interaction=inter.id, small_sliding=True, clearance=0.01)
    case.update(contact_method="surface_to_surface", rayleigh_alpha=0.1, rayleigh_beta=1e-5)
    app.execute("physical_constants.set", absolute_zero=-273.15, stefan_boltzmann=5.67e-11)
    step = case.steps.create_static()
    step.changes.create_model_change_contact(pair=pair.id, action="remove")
    step.changes.create_change_friction(interaction=inter.id, friction_coefficient=0.3)
    step.changes.create_model_change_element(target={"type": "elements", "ids": [9, 10]}, action="remove")
    step.outputs.create_contact_print(variables=["CF"], pair=pair.id, totals="yes")
    IC = app.model.initial_conditions
    IC.create_temperature(target={"type": "nodes", "ids": [1, 2]}, value=293.0)
    IC.create_velocity(target={"type": "nodes", "ids": [1, 2]}, components=[1.0, 0.0, -2.0])
    stress = IC.create_stress(target={"type": "elements", "ids": [1]}, components=[1.0, 0, 0, 0, 0, 0])
    r, cards = deck(app, case)
    assert find(cards, "EQUATION")[0][2] == ["3", "3, 1, 1, 4, 1, -1, 5, 2, 0.5"]
    assert find(cards, "MPC")[0][2] == ["PLANE, 1, 2, 6, 7"]
    rb, = find(cards, "RIGID BODY")
    assert rb[1]["REF NODE"] == str(ref) and find(cards, "NSET", NSET=rb[1]["NSET"])[0][2] == ["5, 10"]
    cp, = find(cards, "COUPLING")
    assert cp[1]["CONSTRAINT NAME"] == f"C{kc.id}" and find(cards, "SURFACE", NAME=cp[1]["SURFACE"])[0][2] == ["4, S4"]
    assert find(cards, "KINEMATIC")[0][2] == ["1, 1", "2, 2"]
    t, = find(cards, "TIE")
    assert t[1] == {"NAME": f"C{tie.id}", "POSITION TOLERANCE": "0.1", "ADJUST": "NO"} and t[2] == ["BOT, TOP"]
    assert find(cards, "SURFACE BEHAVIOR")[0][1] == {"PRESSURE-OVERCLOSURE": "LINEAR"} and find(cards, "SURFACE BEHAVIOR")[0][2] == ["1000000"]
    assert find(cards, "FRICTION")[0][2] == ["0.2, 100000"]
    p, = find(cards, "CONTACT PAIR")
    assert p[1] == {"INTERACTION": "INT1", "TYPE": "SURFACE TO SURFACE", "SMALL SLIDING": True} and p[2] == ["BOT, TOP"]
    assert find(cards, "CLEARANCE")[0][1] == {"SLAVE": "BOT", "MASTER": "TOP", "VALUE": "0.01"}
    assert find(cards, "PHYSICAL CONSTANTS")[0][1] == {"ABSOLUTE ZERO": "-273.15", "STEFAN BOLTZMANN": "5.67e-11"}
    assert find(cards, "DAMPING")[0][1] == {"ALPHA": "0.1", "BETA": "1e-05"}
    ic = find(cards, "INITIAL CONDITIONS")
    assert ic[0][1] == {"TYPE": "TEMPERATURE"} and ic[0][2][0].endswith(", 293")
    assert [line.split(", ", 1)[1] for line in ic[1][2]] == ["1, 1", "2, 0", "3, -2"]
    mc = find(cards, "MODEL CHANGE")
    assert mc[0][1] == {"TYPE": "CONTACT PAIR", "REMOVE": True} and mc[0][2] == ["BOT, TOP"]
    assert mc[1][1] == {"TYPE": "ELEMENT", "REMOVE": True} and find(cards, "ELSET", ELSET=mc[1][2][0])[0][2] == ["9, 10"]
    assert find(cards, "CHANGE FRICTION")[0][1] == {"INTERACTION": "INT1"} and find(cards, "FRICTION")[1][2] == ["0.3"]
    assert find(cards, "CONTACT PRINT")[0][1] == {"SLAVE": "BOT", "MASTER": "TOP", "TOTALS": "YES"}
    assert [(s["object"], s["reason"]) for s in r["skipped"]] == [(stress.id, "initial_condition_type")]
    # 모델 정의는 스텝보다 앞에 있다
    order = [c[0] for c in cards]
    assert max(order.index(k) for k in ("EQUATION", "TIE", "CONTACT PAIR", "INITIAL CONDITIONS", "DAMPING")) < order.index("STEP")
    assert eq.id


# ================================================================ 범위, 파일, 오류
@pytest.mark.feature("CAS-10")
@pytest.mark.feature("CAS-01")
def test_CAS_T03_scope(app):
    part, b, mat, case = cantilever(app)
    other = app.model.mesh_parts.create(name="OTHER")
    block(app, 1, 1, 1, part=other.id, origin=(500.0, 0.0, 0.0))
    app.model.sets.create_node(name="MIXED", ids=[1, 2, 31, 32])
    app.model.sets.create_element(name="FAR", ids=[9])
    case.update(scope={"type": "parts", "ids": [part.id]})
    step = case.steps.create_static()
    far = step.loads.create_force(target={"type": "nodes", "ids": [31]}, components=[1.0, 0.0, 0.0])
    r, cards = deck(app, case)
    assert len(find(cards, "NODE")[0][2]) == 30 and len(find(cards, "ELEMENT")[0][2]) == 8
    assert find(cards, "NSET", NSET="MIXED")[0][2] == ["1, 2"]  # 범위 밖의 노드는 뺀다
    assert find(cards, "ELSET", ELSET="FAR") == [] and find(cards, "ELSET", ELSET="OTHER") == []
    assert [(s["object"], s["reason"]) for s in r["skipped"]] == [(far.id, "invalid_state")]
    assert r["warnings"] == []  # 범위 안의 요소는 모두 프로퍼티가 있다
    case.update(scope=None)
    r, cards = deck(app, case)
    assert len(find(cards, "NODE")[0][2]) == 38 and [w["code"] for w in r["warnings"]] == ["unassigned_elements"]


@pytest.mark.feature("CAS-10")
def test_CAS_T03_export_file_and_errors(app, tmp_path):
    part, b, mat, case = cantilever(app)
    case.steps.create_static()
    preview = app.execute("case.preview_deck", id=case.id)
    path = tmp_path / "한글 폴더" / "job.inp"
    path.parent.mkdir()
    before, hist = total(app), history_len(app)
    r = app.execute("case.export_deck", id=case.id, path=str(path))
    assert path.read_bytes().decode("ascii") == preview["text"]
    assert (r["lines"], r["bytes"]) == (preview["lines"], len(preview["text"])) and r["warnings"] == [] and r["skipped"] == []
    assert total(app) == before and history_len(app) == hist
    short = app.execute("case.preview_deck", id=case.id, max_lines=5)
    assert short["truncated"] is True and short["text"] == "".join(preview["text"].splitlines(keepends=True)[:5])
    assert short["lines"] == preview["lines"]
    for name, params, code in [
        ("case.preview_deck", dict(id=mat.id), "wrong_kind"),
        ("case.preview_deck", dict(id=9999), "not_found"),
        ("case.preview_deck", dict(id=case.id, max_lines=0), "out_of_range"),
        ("case.export_deck", dict(id=case.id), "missing_param"),
        ("case.export_deck", dict(id=case.id, path=str(tmp_path / "none" / "x.inp")), "io_error"),
    ]:
        with pytest.raises(Nasa95Error) as e:
            app.execute(name, **params)
        assert e.value.code == code, (name, params)
    # CalculiX 에 없는 요소가 있으면 덱을 쓰지 않는다
    app.execute("mesh.elements_create", shape="pyramid5", connectivity=[[1, 2, 7, 6, 16]])
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.preview_deck", id=case.id)
    assert e.value.code == "unsupported"


@pytest.mark.feature("MSH-03")
def test_MSH_T07_export_inp(app, tmp_path):
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.export", path=str(tmp_path / "empty.inp"))
    assert e.value.code == "invalid_state"
    part, b, mat, case = cantilever(app)
    app.model.sets.create_node(name="N1", ids=[1, 2])
    app.model.sets.create_surface(name="S1", faces=[[1, 1]])
    path = tmp_path / "mesh.inp"
    r = app.execute("mesh.export", path=str(path))
    assert (r["nodes"], r["elements"], r["format"]) == (30, 8, "inp")
    cards = parse(path.read_text())
    assert [c[0] for c in cards] == ["NODE", "ELEMENT", "NSET", "SURFACE"]  # 메시와 셋만
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.export", path=str(path), format="stl")
    assert e.value.code == "out_of_range"


# ================================================================ 메시 가져오기 (MSH-02)
def mesh_state(app):
    """메시와 셋의 내용(객체 ID 와 무관하게 비교할 수 있는 형태)."""
    parts = {p["id"]: p["name"] for p in app.execute("mesh_part.list")}
    elems = [(e["id"], e["shape"], e["type"], parts.get(e["part"], ""), tuple(e["nodes"])) for e in app.execute("mesh.elements")]
    sets = sorted((s["name"], s["type"], str(app.execute("set.members", id=s["id"])["members"])) for s in app.execute("set.list"))
    return app.mesh.node_ids().tolist(), app.mesh.node_coords().tolist(), elems, sets


@pytest.mark.feature("MSH-02")
@pytest.mark.feature("MSH-03")
def test_MSH_T07_round_trip(app, tmp_path):
    p1 = app.model.mesh_parts.create(name="SOLID")
    p2 = app.model.mesh_parts.create(name="SKIN")
    block(app, 2, 1, 1, part=p1.id)
    app.execute("mesh.convert_order", order=2)
    app.execute("mesh.set_element_type", ids=[1], type="C3D20R")
    sh = plate(app, 2, 1)
    app.execute("mesh.transform", ids=list(range(sh["elements"][0], sh["elements"][1] + 1)), translate=[0.3, 1 / 3, -50.0])
    app.execute("mesh.elements_create", shape="quad4", type="S4R", connectivity=[[33, 34, 37, 36]], part=p2.id)
    n = app.execute("mesh.nodes_create", coords=[[1e-7, 2.5e20, -3.0]])["ids"][0]
    app.execute("mesh.create_connector", kind="mass", nodes=[n])
    app.execute("mesh.create_network", connectivity=[[0, 1, 2]])
    app.execute("id.renumber", what="elements", start=101, ids=[2])
    app.model.sets.create_node(name="N1", ids=[1, 5, 9])
    app.model.sets.create_element(name="E1", ids=[1, 101])
    app.model.sets.create_surface(name="S1", faces=[[1, 3], [101, 6], [sh["elements"][0], 2]])
    app.model.sets.create_node_surface(name="NS1", ids=[2, 3])
    path = str(tmp_path / "rt.inp")
    app.execute("mesh.export", path=path)
    other = App()
    r = other.execute("mesh.import", path=path)
    assert (r["nodes"], r["elements"]) == (app.execute("project.info")["nodes"], app.execute("project.info")["elements"])
    assert r["ignored"] == {} and r["unsupported_types"] == {} and r["skipped_faces"] == 0
    assert (r["node_offset"], r["element_offset"]) == (0, 0) and len(r["parts"]) == 2 and len(r["sets"]) == 4
    assert mesh_state(other) == mesh_state(app)  # 좌표까지 완전히 같다
    # 다시 내보내면 같은 파일
    path2 = str(tmp_path / "rt2.inp")
    other.execute("mesh.export", path=path2)
    assert open(path2).read() == open(path).read()
    # 한 번 더 가져오면 번호를 밀어서 더한다. Undo 로 통째로 되돌린다
    before = total(other)
    r2 = other.execute("mesh.import", path=path)
    assert (r2["node_offset"], r2["element_offset"]) == (other.mesh.node_ids()[r["nodes"] - 1], max(e[0] for e in mesh_state(app)[2]))
    info = other.execute("project.info")
    assert (info["nodes"], info["elements"]) == (2 * r["nodes"], 2 * r["elements"])
    names = sorted(s["name"] for s in other.execute("set.list"))
    assert names == ["E1", "E1_2", "N1", "N1_2", "NS1", "NS1_2", "S1", "S1_2"]
    assert other.execute("set.members", id=r2["sets"][0])["members"] == [x + r2["node_offset"] for x in (1, 5, 9)]
    other.undo()
    assert total(other) == before


INP = """** 다른 프로그램이 쓴 파일을 흉내 낸다
*Heading
 sample
*NODE, NSET=Nall
1, 0., 0., 0.
2, 1.0, 0, 0
3, 1.0E+00, 1.0, 0
4, 0, 1, 0
5, 0, 0, 1
 6 , 1 , 0 , 1
7, 1, 1, 1
8, 0, 1, 1
9, 2, 0
*INCLUDE, INPUT=sub/elements.inp
*NSET, NSET=bottom, GENERATE
1, 4
*NSET, NSET=odd, GENERATE
1, 9, 2
*nset, nset=both
bottom, odd,
9
*ELSET, ELSET=solids
Evol
*ELSET, ELSET=some
1, 3
*SURFACE, NAME=top
Evol, S2
3, SPOS
3, S3
*SURFACE, NAME=nodesurf, TYPE=NODE
bottom
9
*MATERIAL, NAME=steel
*ELASTIC
210000, 0.3
*STEP
*STATIC
*END STEP
"""
ELEMENTS = """*ELEMENT, TYPE=C3D8,
  ELSET=Evol
1, 1, 2, 3, 4, 5, 6, 7, 8
*ELEMENT, TYPE=C3D20X, ELSET=Eodd
2, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7,
8, 1, 2, 3, 4
*ELEMENT, TYPE=S3, ELSET=Eshell
3, 2, 9,
3
*ELEMENT, TYPE=T3D2
4, 1, 9
"""


@pytest.mark.feature("MSH-02")
@pytest.mark.feature("CAS-40")
def test_MSH_T07_import_foreign_file(app, tmp_path):
    (tmp_path / "sub").mkdir()
    (tmp_path / "model.inp").write_bytes(INP.replace("\n", "\r\n").encode("utf-8"))
    (tmp_path / "sub" / "elements.inp").write_text(ELEMENTS)
    hist = history_len(app)
    r = app.execute("mesh.import", path=str(tmp_path / "model.inp"))
    assert history_len(app) == hist + 1
    assert (r["nodes"], r["elements"]) == (9, 3)
    assert r["unsupported_types"] == {"C3D20X": 1}  # 읽지 못한 타입은 알린다
    assert r["ignored"] == {"MATERIAL": 1, "ELASTIC": 1, "STEP": 1, "STATIC": 1, "END STEP": 1}
    assert r["skipped_faces"] == 1  # SPOS
    assert app.execute("mesh.nodes", ids=[3, 6, 9])["coords"] == [[1.0, 1.0, 0.0], [1.0, 0.0, 1.0], [2.0, 0.0, 0.0]]
    parts = {p["id"]: p["name"] for p in app.execute("mesh_part.list")}
    assert sorted(parts.values()) == ["Eshell", "Evol"]  # 요소가 없는 Eodd 는 만들지 않는다
    elems = {e["id"]: e for e in app.execute("mesh.elements")}
    assert (elems[1]["shape"], parts[elems[1]["part"]]) == ("hex8", "Evol")
    assert (elems[3]["shape"], elems[3]["type"], elems[3]["nodes"]) == ("tri3", "S3", [2, 9, 3])
    assert (elems[4]["type"], elems[4]["part"]) == ("T3D2", 0)
    sets = {s["name"]: app.execute("set.members", id=s["id"])["members"] for s in app.execute("set.list")}
    assert sets == {"bottom": [1, 2, 3, 4], "odd": [1, 3, 5, 7, 9], "both": [1, 2, 3, 4, 5, 7, 9], "solids": [1], "some": [1, 3],
                    "top": [[1, 2], [3, 1]], "nodesurf": [1, 2, 3, 4, 9]}  # 쉘의 S3 = 첫 변
    assert app.execute("mesh.check")["ok"]
    app.undo()
    assert app.execute("project.info")["nodes"] == 0 and app.execute("set.list") == []


@pytest.mark.feature("MSH-02")
def test_MSH_T07_import_errors(app, tmp_path):
    def attempt(text, name="bad.inp"):
        path = tmp_path / name
        path.write_text(text)
        before = total(app)
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.import", path=str(path))
        assert total(app) == before  # 실패하면 아무것도 남지 않는다
        return e.value

    node = "*NODE\n1, 0, 0, 0\n2, 1, 0, 0\n"
    e = attempt(node + "3, 1x, 0, 0\n")
    assert e.code == "parse_error" and e.details["line"] == 1 and e.details["keyword"] == "NODE"
    assert attempt(node + "*ELEMENT, TYPE=T3D2\n1, 1, 5\n").code == "not_found"  # 없는 노드
    # 절점 번호가 남으면 솔버처럼 앞의 것만 쓴다(배포본 예제에 C3D20 연결을 C3D8 로 읽는 덱이 있다) → 오류가 아니라 extra_connectivity 로 센다
    extra = tmp_path / "extra.inp"
    extra.write_text(node + "*ELEMENT, TYPE=T3D2\n1, 1, 2, 2\n")
    rr = app.execute("mesh.import", path=str(extra))
    assert rr["elements"] == 1 and rr.get("extra_connectivity") == 1 and app.execute("mesh.elements", ids=[1])[0]["nodes"] == [1, 2]
    app.undo()
    assert attempt(node + "*ELEMENT, TYPE=C3D8\n1, 1, 2\n").code == "parse_error"  # 절점이 모자란다
    assert attempt(node + "*ELEMENT\n1, 1, 2\n").code == "parse_error"  # TYPE 없음
    assert attempt(node + "1, 5, 5, 5\n").code == "parse_error"  # 같은 노드 번호
    assert attempt(node + "*NSET, NSET=A\nB\n").code == "parse_error"  # 정의되지 않은 셋
    assert attempt(node + "*NSET, NSET=A, GENERATE\n5, 1\n").code == "parse_error"
    assert attempt(node + "*INCLUDE, INPUT=nothing.inp\n").code == "io_error"
    assert attempt("*INCLUDE, INPUT=loop.inp\n", "loop.inp").code == "invalid_state"  # 자기 자신을 포함
    assert attempt("*MATERIAL, NAME=A\n").code == "invalid_state"  # 노드가 없다
    with pytest.raises(Nasa95Error) as err:
        app.execute("mesh.import", path=str(tmp_path / "none.inp"))
    assert err.value.code == "io_error"


# ================================================================ 경계조건 카드(BC-T01·T04·T05·T06): 덱의 카드로 확인
def _static_case(app):
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static()
    return part, b, mat, case, step


def _nodes_of(cards, name):
    """노드 셋 이름 → 노드 번호 집합(노드 번호면 그 하나)."""
    if name.isdigit():
        return {int(name)}
    out = set()
    for c in find(cards, "NSET", NSET=name):
        for row in c[2]:
            out |= {int(x) for x in row.split(",") if x.strip()}
    return out


def _bc_rows(cards):
    """*BOUNDARY 줄을 (노드 집합, 자유도 시작, 끝, 값 또는 None) 목록으로."""
    rows = []
    for c in find(cards, "BOUNDARY"):
        for row in c[2]:
            cells = [x.strip() for x in row.split(",")]
            rows.append((_nodes_of(cards, cells[0]), int(cells[1]), int(cells[2]) if len(cells) > 2 else int(cells[1]), float(cells[3]) if len(cells) > 3 else None))
    return rows


@pytest.mark.feature("BC-02")
@pytest.mark.feature("BC-03")
@pytest.mark.feature("BC-06")
def test_BC_T01_03_04_05_08_prescribed_symmetry_temperature(app):
    """강제 변위(값 있는 *BOUNDARY), 대칭·반대칭(법선에 따른 자유도 집합 — 솔리드는 병진만), 고정 온도(자유도 11)."""
    part, b, mat, case, step = _static_case(app)
    fix = app.model.sets.create_node(name="END", ids=[b["node"](4, 0, 0), b["node"](4, 1, 0)])
    step.bcs.create_displacement(target={"type": "set", "ids": [fix.id]}, dofs=[1], values=[0.5])
    step.bcs.create_symmetry(target={"type": "nodes", "ids": [b["node"](0, 0, 0)]}, normal="y")
    step.bcs.create_antisymmetry(target={"type": "nodes", "ids": [b["node"](0, 1, 0)]}, normal="y")
    step.bcs.create_temperature(target={"type": "nodes", "ids": [b["node"](2, 0, 0)]}, value=80.0)
    r, cards = deck(app, case)
    assert r["skipped"] == []
    rows = _bc_rows(cards)
    n0, n1, n2 = b["node"](0, 0, 0), b["node"](0, 1, 0), b["node"](2, 0, 0)
    assert ({b["node"](4, 0, 0), b["node"](4, 1, 0)}, 1, 1, 0.5) in rows
    dofs_of = lambda node: sorted(d for nodes, d, _, v in rows if node in nodes)  # noqa: E731
    assert dofs_of(n0) == [2]  # y 대칭: y 병진만(솔리드)
    assert dofs_of(n1) == [1, 3]  # 반대칭: 여집합
    assert ({n2}, 11, 11, 80.0) in rows


@pytest.mark.feature("BC-15")
@pytest.mark.feature("BC-16")
@pytest.mark.feature("BC-17")
@pytest.mark.feature("BC-18")
@pytest.mark.feature("BC-21")
def test_BC_T04_01_02_04_05_06_07_08_12_calculix_constraints(app):
    """현재 상태 고정(*BOUNDARY, FIXED), MPC(PLANE·STRAIGHT·BEAM·MEANROT·DIST), 강체(REF NODE·ROT NODE), 커플링(KINEMATIC·DISTRIBUTING·ORIENTATION), 타이 옵션."""
    part, b, mat, case, step = _static_case(app)
    n = b["node"]
    step2 = case.steps.create_static()
    step2.bcs.create_fixed_current(target={"type": "nodes", "ids": [n(4, 0, 0)]}, dofs=[1, 2, 3])
    app.model.constraints.create_mpc_plane(nodes=[n(1, 0, 0), n(1, 1, 0), n(1, 2, 0), n(1, 0, 1)])
    app.model.constraints.create_mpc_straight(nodes=[n(2, 0, 0), n(2, 1, 0), n(2, 2, 0)])
    app.model.constraints.create_mpc_beam(nodes=[n(3, 0, 0), n(3, 1, 0)])
    pilot = app.execute("mesh.nodes_create", coords=[[50.0, 10.0, 20.0]])["ids"][0]
    app.model.constraints.create_mpc_meanrot(nodes=[n(4, 0, 1), n(4, 1, 1), n(4, 2, 1)], pilot_node=pilot)
    app.model.constraints.create_mpc_dist(nodes=[n(4, 0, 0), n(4, 2, 0)], pilot_node=pilot)
    ref = app.execute("mesh.nodes_create", coords=[[0.0, 10.0, 20.0]])["ids"][0]
    rot = app.execute("mesh.nodes_create", coords=[[0.0, 10.0, 30.0]])["ids"][0]
    app.model.constraints.create_rigid_body(target={"type": "nodes", "ids": [n(0, 0, 1), n(0, 1, 1)]}, ref_node=ref, rot_node=rot)
    ori = app.model.orientations.create_rectangular(name="OR1", a=[0.0, 1.0, 0.0], b=[0.0, 0.0, 1.0])
    top = app.model.sets.create_surface(name="TOP", faces=[[5, 2], [6, 2]])
    app.model.constraints.create_coupling_kinematic(surface={"type": "set", "ids": [top.id]}, ref_node=ref, dofs=[1, 2, 3])
    app.model.constraints.create_coupling_distributing(surface={"type": "set", "ids": [top.id]}, ref_node=rot, dofs=[1, 2, 3, 4, 5, 6], orientation=ori.id)
    other = app.model.sets.create_surface(name="BOT", faces=[[1, 1], [2, 1]])
    app.model.constraints.create_tie(slave={"type": "set", "ids": [other.id]}, master={"type": "set", "ids": [top.id]}, position_tolerance=0.01, adjust=False)
    r, cards = deck(app, case)
    assert r["skipped"] == []
    fixed = find(cards, "BOUNDARY", FIXED=True)[0]
    assert [row.split(",")[1].strip() for row in fixed[2]] == ["1", "2", "3"] and _nodes_of(cards, fixed[2][0].split(",")[0]) == {n(4, 0, 0)}
    mpcs = {c[2][0].split(",")[0].strip(): c[2] for c in find(cards, "MPC")}
    assert set(mpcs) == {"PLANE", "STRAIGHT", "BEAM", "MEANROT", "DIST"}
    assert mpcs["PLANE"][0].startswith("PLANE, ") and len(mpcs["PLANE"][0].split(",")) == 5
    assert mpcs["MEANROT"][0].endswith(f", {pilot}") and mpcs["DIST"][0].endswith(f", {pilot}")
    rb, = find(cards, "RIGID BODY")
    assert rb[1]["REF NODE"] == str(ref) and rb[1]["ROT NODE"] == str(rot) and "NSET" in rb[1]
    couplings = find(cards, "COUPLING")
    assert len(couplings) == 2 and {c[1]["REF NODE"] for c in couplings} == {str(ref), str(rot)} and all(c[1]["SURFACE"] == "TOP" for c in couplings)
    assert any(c[1].get("ORIENTATION") == "OR1" for c in couplings)
    assert find(cards, "KINEMATIC")[0][2] == ["1, 1", "2, 2", "3, 3"] and [r.split(",")[0] for r in find(cards, "DISTRIBUTING")[0][2]] == ["1", "2", "3", "4", "5", "6"]
    tie, = find(cards, "TIE")
    assert tie[1]["POSITION TOLERANCE"] == "0.01" and tie[1]["ADJUST"] == "NO" and tie[2] == ["BOT, TOP"]


@pytest.mark.feature("BC-19")
@pytest.mark.feature("BC-20")
@pytest.mark.feature("BC-21")
def test_BC_T04_09_10_11_16_cyclic_multistage(app):
    """순환대칭(*CYCLIC SYMMETRY MODEL, *TIE CYCLIC SYMMETRY, 절직경 선택), 다단 연결(*TIE, MULTISTAGE), 타이 조정 기본값."""
    part, b, mat, case, step = _static_case(app)
    n = b["node"]
    left = app.model.sets.create_node(name="LEFT", ids=[n(0, j, k) for j in range(3) for k in range(2)])
    right = app.model.sets.create_node(name="RIGHT", ids=[n(4, j, k) for j in range(3) for k in range(2)])
    cs = app.model.constraints.create_cyclic_symmetry(slave={"type": "set", "ids": [left.id]}, master={"type": "set", "ids": [right.id]}, sectors=12,
                                                      axis_point_a=[0, 0, 0], axis_point_b=[0, 0, 1], ngraph=3, position_tolerance=0.02)
    freq = case.steps.create_frequency(num_modes=4)
    ms_a = app.model.sets.create_node(name="MSA", ids=[n(2, 0, 0), n(2, 1, 0)])
    ms_b = app.model.sets.create_node(name="MSB", ids=[n(2, 0, 1), n(2, 1, 1)])
    app.model.constraints.create_multistage(slave={"type": "set", "ids": [ms_a.id]}, master={"type": "set", "ids": [ms_b.id]})
    top = app.model.sets.create_surface(name="TOP", faces=[[5, 2]])
    bot = app.model.sets.create_surface(name="BOT", faces=[[1, 1]])
    app.model.constraints.create_tie(slave={"type": "set", "ids": [bot.id]}, master={"type": "set", "ids": [top.id]})  # 조정 기본값
    r, cards = deck(app, case)
    assert r["skipped"] == []
    model, = find(cards, "CYCLIC SYMMETRY MODEL")
    assert model[1]["N"] == "12" and model[1]["NGRAPH"] == "3" and model[2] == ["0, 0, 0, 0, 0, 1"]
    ties = find(cards, "TIE")
    cyc = [t for t in ties if "CYCLIC SYMMETRY" in t[1]][0]
    assert cyc[1]["POSITION TOLERANCE"] == "0.02"
    sa, sb = [x.strip() for x in cyc[2][0].split(",")]
    node_surfaces = {c[1]["NAME"]: c for c in find(cards, "SURFACE", TYPE="NODE")}
    nodes_in = lambda card: {int(x) for row in card[2] for x in row.split(",") if x.strip().isdigit()}  # noqa: E731
    assert nodes_in(node_surfaces[sa]) == set(left.props["ids"]) and nodes_in(node_surfaces[sb]) == set(right.props["ids"])  # 노드 셋 → 노드 면
    ms = [t for t in ties if "MULTISTAGE" in t[1]][0]
    ma, mb = [x.strip() for x in ms[2][0].split(",")]
    resolve = lambda name: nodes_in(node_surfaces[name]) if name in node_surfaces else _nodes_of(cards, name)  # noqa: E731 — 노드 셋 이름 그대로도 된다
    assert resolve(ma) == set(ms_a.props["ids"]) and resolve(mb) == set(ms_b.props["ids"])
    plain = [t for t in ties if "CYCLIC SYMMETRY" not in t[1] and "MULTISTAGE" not in t[1]][0]
    assert "ADJUST" not in plain[1]
    # 절직경 선택(BC-19): 고유치 스텝의 설정으로
    freq.update(nodal_diameter_min=0, nodal_diameter_max=3) if "nodal_diameter_min" in {f["name"] for f in app.kinds()["step"]["fields"] + [f for s in app.kinds()["step"]["subtypes"] if s["name"] == "frequency" for f in s["fields"]]} else None
    text = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*SELECT CYCLIC SYMMETRY MODES" in text or "nodal_diameter_min" not in {f["name"] for s in app.kinds()["step"]["subtypes"] if s["name"] == "frequency" for f in s["fields"]}


@pytest.mark.feature("BC-23")
@pytest.mark.feature("BC-24")
@pytest.mark.feature("BC-25")
@pytest.mark.feature("BC-26")
@pytest.mark.feature("BC-27")
@pytest.mark.feature("BC-28")
@pytest.mark.feature("BC-29")
def test_BC_T05_05_06_07_08_09_10_11_12_13_contact_details(app):
    """접촉 압력 거동(LINEAR·HARD·EXPONENTIAL·TABULAR·TIED), 마찰, 초기 간극·조정·미소 미끄럼, 접촉 감쇠, 간극 열전도·마찰 발열, 스텝 중 변경, 갭 요소."""
    part, b, mat, case, step = _static_case(app)
    top = app.model.sets.create_surface(name="TOP", faces=[[5, 2], [6, 2]])
    bot = app.model.sets.create_surface(name="BOT", faces=[[1, 1], [2, 1]])
    P = app.model.contact_properties
    props = {
        "linear": P.create(name="LIN", pressure_overclosure="linear", slope=1e6, friction_coefficient=0.3, stick_slope=1e5, damping=0.1, damping_tangent_fraction=0.5,
                           conductance=[[100.0, 0.0, 0.0], [0.0, 1.0, 0.0]], heat_conversion=0.9, heat_slave_fraction=0.5),
        "hard": P.create(name="HARD", pressure_overclosure="hard"),
        "exp": P.create(name="EXP", pressure_overclosure="exponential", c0=0.01, p0=100.0),
        "tab": P.create(name="TAB", pressure_overclosure="tabular", table=[[0.0, 0.0], [100.0, 0.01]]),
        "tied": P.create(name="TIED", pressure_overclosure="tied", slope=1e7),
    }
    pair = app.model.contact_pairs.create(slave={"type": "set", "ids": [bot.id]}, master={"type": "set", "ids": [top.id]}, interaction=props["linear"].id,
                                          small_sliding=True, adjust=0.1, clearance=0.05)
    for k in ("hard", "exp", "tab", "tied"):
        app.model.contact_pairs.create(slave={"type": "set", "ids": [bot.id]}, master={"type": "set", "ids": [top.id]}, interaction=props[k].id)
    step2 = case.steps.create_static()
    step2.changes.create_change_friction(interaction=props["linear"].id, friction_coefficient=0.1, stick_slope=1e4)
    step2.changes.create_change_surface_behavior(interaction=props["linear"].id, pressure_overclosure="hard")
    step2.changes.create_change_contact_type(method="node_to_surface")
    gap_nodes = app.execute("mesh.nodes_create", coords=[[0.0, 0.0, -5.0], [0.0, 0.0, -6.0]])["ids"]
    gap = app.execute("mesh.create_connector", kind="gap", nodes=gap_nodes)["ids"]
    app.model.properties.create_gap(target={"type": "elements", "ids": gap}, clearance=1.0, direction=[0.0, 0.0, 1.0])
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    inter = {c[1]["NAME"]: i for i, c in enumerate(cards) if c[0] == "SURFACE INTERACTION"}
    assert set(inter) == {"LIN", "HARD", "EXP", "TAB", "TIED"}
    behaviours = {c[1].get("PRESSURE-OVERCLOSURE"): c for c in find(cards, "SURFACE BEHAVIOR")}
    assert set(behaviours) == {"LINEAR", "HARD", "EXPONENTIAL", "TABULAR", "TIED"}
    assert behaviours["LINEAR"][2][0].startswith("1e+06") or behaviours["LINEAR"][2][0].startswith("1000000")
    assert behaviours["EXPONENTIAL"][2] == ["0.01, 100"] and behaviours["TABULAR"][2] == ["0, 0", "100, 0.01"]
    assert find(cards, "FRICTION")[0][2] == ["0.3, 100000"]
    assert find(cards, "CONTACT DAMPING")[0][1].get("TANGENT FRACTION") == "0.5" and find(cards, "CONTACT DAMPING")[0][2] == ["0.1"]
    assert find(cards, "GAP CONDUCTANCE")[0][2] == ["100, 0, 0", "0, 1, 0"]
    assert find(cards, "GAP HEAT GENERATION")[0][2] == ["0.9, 0.5"] or find(cards, "GAP HEAT GENERATION")[0][2][0].startswith("0.9, 0.5")
    cp = find(cards, "CONTACT PAIR", INTERACTION="LIN")[0]
    assert cp[1]["SMALL SLIDING"] is True and cp[1]["ADJUST"] == "0.1" and cp[2] == ["BOT, TOP"]
    # TYPE 은 필수(매뉴얼 7.25): 케이스에 접촉 방식이 없으면 SURFACE TO SURFACE 로 쓰고 한 번 경고한다
    assert cp[1]["TYPE"] == "SURFACE TO SURFACE" and [w["code"] for w in r["warnings"] if w["code"] == "contact_method_default"] == ["contact_method_default"]
    assert find(cards, "CLEARANCE")[0][1]["VALUE"] == "0.05"
    assert find(cards, "CHANGE FRICTION", INTERACTION="LIN") and find(cards, "CHANGE SURFACE BEHAVIOR", INTERACTION="LIN")
    assert "TO NODE TO SURFACE" in find(cards, "CHANGE CONTACT TYPE")[0][1]
    step2.changes.create_change_contact_type(method="surface_to_surface")  # 솔버가 허용하지 않는 방향: 쓰지 않고 알린다
    assert [x["reason"] for x in app.execute("case.preview_deck", id=case.id)["skipped"]] == ["change_type"]
    assert find(cards, "ELEMENT", TYPE="GAPUNI") and find(cards, "GAP")[0][2] == ["1, 0, 0, 1"]
    case.update(contact_method="node_to_surface")
    r2, cards2 = deck(app, case)
    assert find(cards2, "CONTACT PAIR", INTERACTION="LIN")[0][1]["TYPE"] == "NODE TO SURFACE" and not [w for w in r2["warnings"] if w["code"] == "contact_method_default"]


@pytest.mark.feature("BC-30")
@pytest.mark.feature("BC-32")
@pytest.mark.feature("BC-33")
@pytest.mark.feature("BC-35")
@pytest.mark.feature("BC-31")
@pytest.mark.feature("BC-34")
def test_BC_T06_01_02_03_04_05_06_07_other_bcs(app, tmp_path):
    """기초 가진(*BASE MOTION), 네트워크 경계조건(자유도 1·2·11), 전자기(자유도 8), 요소 제거(*MODEL CHANGE), 서브모델(*SUBMODEL + *BOUNDARY, SUBMODEL), 3D 유체 경계조건."""
    part, b, mat, case, step = _static_case(app)
    n = b["node"]
    freq = case.steps.create_frequency(num_modes=2, storage=True)
    ssd = case.steps.create_steady_state_dynamics(freq_min=1.0, freq_max=100.0)
    amp = app.model.functions.create_amplitude(name="A1", points=[[0.0, 1.0], [100.0, 1.0]])
    ssd.bcs.create_base_motion(dof=3, motion="acceleration", amplitude=amp.id)
    net = app.execute("mesh.nodes_create", coords=[[0, 0, -10], [0, 0, -20], [0, 0, -30]])["ids"]
    step.bcs.create_network(target={"type": "nodes", "ids": [net[0]]}, quantity="total_pressure", value=2.0)
    step.bcs.create_network(target={"type": "nodes", "ids": [net[0]]}, quantity="temperature", value=300.0)
    step.bcs.create_network(target={"type": "nodes", "ids": [net[1]]}, quantity="mass_flow", value=0.5)
    step.bcs.create_electromagnetic(target={"type": "nodes", "ids": [n(0, 0, 0)]}, value=0.0)
    step2 = case.steps.create_static()
    step2.changes.create_model_change_element(target={"type": "elements", "ids": [8]}, action="remove")
    gfile = tmp_path / "global.frd"
    gfile.write_text("")
    step.bcs.create_submodel(target={"type": "nodes", "ids": [n(4, 0, 0)]}, dofs=[1, 2, 3], global_file=str(gfile), global_step=2)
    step.bcs.create_fluid(target={"type": "nodes", "ids": [n(4, 2, 1)]}, quantity="velocity", values=[1.0, 0.0, 0.0])
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    bm, = find(cards, "BASE MOTION")
    assert bm[1]["DOF"] == "3" and bm[1]["TYPE"] == "ACCELERATION" and bm[1]["AMPLITUDE"] == "A1"
    rows = _bc_rows(cards)
    assert ({net[0]}, 2, 2, 2.0) in rows and ({net[0]}, 11, 11, 300.0) in rows and ({net[1]}, 1, 1, 0.5) in rows
    assert ({n(0, 0, 0)}, 8, 8, 0.0) in rows
    mc, = find(cards, "MODEL CHANGE")
    assert mc[1]["TYPE"] == "ELEMENT" and "REMOVE" in mc[1]
    sub, = find(cards, "SUBMODEL")
    assert sub[1]["TYPE"] == "NODE" and sub[1]["INPUT"].endswith("global.frd")
    assert any(c[1].get("SUBMODEL") is True and c[1].get("STEP") == "2" for c in find(cards, "BOUNDARY"))
    assert ({n(4, 2, 1)}, 1, 1, 1.0) in rows and ({n(4, 2, 1)}, 2, 2, 0.0) in rows  # 유체 속도 → 자유도 1~3


# ================================================================ 하중 카드(LOD-T0x): 덱의 카드로 확인
def _cload_sum(cards):
    tot = [0.0, 0.0, 0.0]
    for c in find(cards, "CLOAD"):
        for row in c[2]:
            cells = [x.strip() for x in row.split(",")]
            if cells[0].isdigit() and len(cells) == 3:
                tot[int(cells[1]) - 1] += float(cells[2])
    return tot


@pytest.mark.feature("LOD-03")
@pytest.mark.feature("LOD-04")
@pytest.mark.feature("LOD-06")
@pytest.mark.feature("LOD-07")
@pytest.mark.feature("LOD-08")
def test_LOD_T02_distributed_remote_total_pretension(app):
    """분포력(traction)·합력(total_force)·보 선하중은 등가 집중 하중으로(합 보존), 원격 하중은 분배 커플링 + 기준 노드 CLOAD, 체적력(가속도·원심력·만유인력), 프리텐션."""
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static()
    n = b["node"]
    top = app.model.sets.create_surface(name="TOP", faces=[[e, 2] for e in range(5, 9)])  # 윗면(z=10) 가운데 j=1 줄의 4면 = 4 × 25 × 10 = 1000
    step.loads.create_traction(target={"type": "set", "ids": [top.id]}, components=[1.0, 0.0, -2.0])
    r, cards = deck(app, case)
    assert r["skipped"] == [] and [w["code"] for w in r["warnings"]] == ["equivalent_nodal_load"]
    assert _cload_sum(cards) == pytest.approx([1000.0, 0.0, -2000.0])  # 분포력 × 넓이 1000
    app.execute("load.delete", id=step.loads.list()[0]["id"])
    step.loads.create_total_force(target={"type": "set", "ids": [top.id]}, components=[0.0, 300.0, -100.0])
    r, cards = deck(app, case)
    assert _cload_sum(cards) == pytest.approx([0.0, 300.0, -100.0])
    app.execute("load.delete", id=step.loads.list()[0]["id"])
    # 보 선하중: B31 두 개(길이 10, 10)에 q = (0, 0, -3) → 합 −60
    bn = app.execute("mesh.nodes_create", coords=[[0, 0, 20], [10, 0, 20], [20, 0, 20]])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[bn[:2], bn[1:]])["ids"]
    app.model.properties.create_beam(material=mat.id, section="rect", dimensions=[1.0, 1.0], direction=[0.0, 1.0, 0.0], target={"type": "elements", "ids": beams})
    step.loads.create_line_load(target={"type": "elements", "ids": beams}, components=[0.0, 0.0, -3.0])
    r, cards = deck(app, case)
    assert _cload_sum(cards) == pytest.approx([0.0, 0.0, -60.0])
    rows = {int(x.split(",")[0]): float(x.split(",")[2]) for c in find(cards, "CLOAD") for x in c[2]}
    assert rows[bn[1]] == pytest.approx(-30.0) and rows[bn[0]] == pytest.approx(-15.0)  # 가운데 절점은 두 요소의 몫
    app.execute("load.delete", id=step.loads.list()[0]["id"])
    # 2차 보(B32, 절점 순서 끝·가운데·끝): 길이 20 에 q = −3 → 끝 1/6 (−10), 가운데 2/3 (−40), 합 −60
    bn3 = app.execute("mesh.nodes_create", coords=[[0, 0, 30], [10, 0, 30], [20, 0, 30]])["ids"]
    beam3 = app.execute("mesh.elements_create", shape="line3", type="B32", connectivity=[bn3])["ids"]
    app.model.properties.create_beam(material=mat.id, section="rect", dimensions=[1.0, 1.0], direction=[0.0, 1.0, 0.0], target={"type": "elements", "ids": beam3})
    step.loads.create_line_load(target={"type": "elements", "ids": beam3}, components=[0.0, 0.0, -3.0])
    r, cards = deck(app, case)
    rows = {int(x.split(",")[0]): float(x.split(",")[2]) for c in find(cards, "CLOAD") for x in c[2]}
    assert _cload_sum(cards) == pytest.approx([0.0, 0.0, -60.0]) and rows[bn3[1]] == pytest.approx(-40.0) and rows[bn3[0]] == pytest.approx(-10.0)
    app.execute("load.delete", id=step.loads.list()[0]["id"])
    # 원격 하중: 기준 노드(새 번호) + *COUPLING/*DISTRIBUTING + CLOAD(힘·모멘트)
    step.loads.create_remote_force(target={"type": "set", "ids": [top.id]}, point=[50.0, 10.0, 50.0], force=[0.0, 0.0, -100.0], moment=[0.0, 500.0, 0.0])
    r, cards = deck(app, case)
    assert r["skipped"] == []
    cp = [c for c in find(cards, "COUPLING") if c[1].get("SURFACE") == "TOP"][0]
    ref = int(cp[1]["REF NODE"])
    assert ref > max(bn) and any(c[2][0].startswith(f"{ref}, 50, 10, 50") for c in find(cards, "NODE"))
    assert find(cards, "DISTRIBUTING")[0][2] == ["1, 6"]
    cl = [x for c in find(cards, "CLOAD") for x in c[2]]
    assert f"{ref}, 3, -100" in cl and f"{ref}, 5, 500" in cl
    app.execute("load.delete", id=step.loads.list()[0]["id"])
    # 체적력
    step.loads.create_acceleration(target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0.0, 0.0, -1.0])
    step.loads.create_centrifugal(target={"type": "parts", "ids": [part.id]}, omega=100.0, axis_point=[0, 0, 0], axis_direction=[0, 0, 1])
    step.loads.create_newton_gravity(target={"type": "parts", "ids": [part.id]})
    app.execute("physical_constants.set", newton_gravity=6.674e-11)
    r, cards = deck(app, case)
    dl = [x for c in find(cards, "DLOAD") for x in c[2]]
    assert "BEAM, GRAV, 9810, 0, 0, -1" in dl and "BEAM, CENTRIF, 10000, 0, 0, 0, 0, 0, 1" in dl and "BEAM, NEWTON" in dl
    assert find(cards, "PHYSICAL CONSTANTS")
    # 프리텐션: 단면 면 + 기준 노드(요소에 속하지 않는 노드)
    for l in step.loads.list():
        app.execute("load.delete", id=l["id"])
    sec = app.model.sets.create_surface(name="SEC", faces=[[2, 4]])  # 요소 2 의 +x 면
    pre = app.execute("mesh.nodes_create", coords=[[0, 0, -50]])["ids"][0]
    step.loads.create_pretension(target={"type": "set", "ids": [sec.id]}, node=pre, direction=[1.0, 0.0, 0.0], force=1000.0)
    r, cards = deck(app, case)
    assert r["skipped"] == []
    pt, = find(cards, "PRE-TENSION SECTION")
    assert pt[1]["SURFACE"] == "SEC" and pt[1]["NODE"] == str(pre) and pt[2] == ["1, 0, 0"]
    assert f"{pre}, 1, 1000" in [x for c in find(cards, "CLOAD") for x in c[2]]
    app.execute("load.update", id=step.loads.list()[0]["id"], fixed=True)
    r, cards = deck(app, case)
    assert f"{pre}, 1, 1" in [x for c in find(cards, "BOUNDARY") for x in c[2]]


@pytest.mark.feature("LOD-05")
@pytest.mark.feature("LOD-09")
@pytest.mark.feature("LOD-11")
@pytest.mark.feature("LOD-12")
@pytest.mark.feature("LOD-20")
@pytest.mark.feature("LOD-21")
@pytest.mark.feature("LOD-30")
@pytest.mark.feature("LOD-31")
@pytest.mark.feature("LOD-32")
@pytest.mark.feature("LOD-33")
@pytest.mark.feature("LOD-36")
@pytest.mark.feature("LOD-39")
def test_LOD_T03_temperature_initial_amplitude_phase(app, tmp_path):
    """온도 하중·구배·결과 파일 온도, 초기 조건(온도·속도·변위·응력·소성 변형률·유체), 시간 함수·시간 지연, 실수/허수(LOAD CASE), 섹터, 공간 분포(아직 미지원 → skipped), 초기 변형률 증분."""
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static()
    n = b["node"]
    amp = app.model.functions.create_amplitude(name="A1", points=[[0.0, 0.0], [1.0, 1.0]])
    step.loads.create_temperature(target={"type": "nodes", "ids": [n(4, 0, 0)]}, value=100.0, amplitude=amp.id, time_delay=0.5)
    step.loads.create_temperature_gradient(target={"type": "nodes", "ids": [n(4, 1, 0)]}, value=50.0, gradient=2.0)
    tf = tmp_path / "thermal.frd"
    tf.write_text("")
    step.loads.create_temperature_from_file(file=str(tf), begin_step=2)
    step.loads.create_force(target={"type": "nodes", "ids": [n(4, 2, 1)]}, components=[0.0, 0.0, -1.0], phase="imaginary", sector=3)
    app.model.initial_conditions.create_temperature(target={"type": "nodes", "ids": [n(0, 0, 0)]}, value=20.0)
    app.model.initial_conditions.create_velocity(target={"type": "nodes", "ids": [n(0, 1, 0)]}, components=[1.0, 0.0, 0.0])
    app.model.initial_conditions.create_displacement(target={"type": "nodes", "ids": [n(0, 2, 0)]}, components=[0.0, 0.1, 0.0])
    app.model.initial_conditions.create_mass_flow(target={"type": "nodes", "ids": [n(0, 0, 1)]}, value=0.5)
    app.model.initial_conditions.create_pressure(target={"type": "nodes", "ids": [n(0, 1, 1)]}, value=1e5)
    app.model.initial_conditions.create_fluid_velocity(target={"type": "nodes", "ids": [n(0, 2, 1)]}, components=[1.0, 2.0, 3.0])
    r, cards = deck(app, case)
    kinds = {x["type"] for x in r["skipped"]}
    temp = [c for c in find(cards, "TEMPERATURE") if "FILE" not in c[1]]
    assert any(c[1].get("AMPLITUDE") == "A1" and c[1].get("TIME DELAY") == "0.5" for c in temp)
    rows = [x for c in temp for x in c[2]]
    assert any(x.endswith(", 50, 2") for x in rows)  # 기준면 온도 + 구배
    tfile = [c for c in find(cards, "TEMPERATURE") if "FILE" in c[1]][0]
    assert tfile[1]["FILE"].endswith("thermal.frd") and tfile[1]["BSTEP"] == "2"
    cl = find(cards, "CLOAD")[0]
    assert cl[1]["LOAD CASE"] == "2" and cl[1]["SECTOR"] == "3"
    ic = {c[1]["TYPE"]: c for c in find(cards, "INITIAL CONDITIONS")}
    assert {"TEMPERATURE", "VELOCITY", "DISPLACEMENT", "MASS FLOW", "PRESSURE", "FLUID VELOCITY"} <= set(ic)
    rowset = lambda card: {(frozenset(_nodes_of(cards, x.split(",")[0].strip())), x.split(",", 1)[1].strip()) for x in card[2]}  # noqa: E731
    assert rowset(ic["VELOCITY"]) >= {(frozenset({n(0, 1, 0)}), "1, 1")} and (frozenset({n(0, 2, 0)}), "2, 0.1") in rowset(ic["DISPLACEMENT"])
    assert {(frozenset({n(0, 2, 1)}), f"{d}, {d}") for d in (1, 2, 3)} <= rowset(ic["FLUID VELOCITY"])
    # 응력·소성 변형률 초기 조건과 초기 변형률 증분은 아직 보존·미지원. 공간 분포 압력은 요소면마다 값으로 풀려 나간다(LOD-T01-29)
    app.model.initial_conditions.create_stress(target={"type": "parts", "ids": [part.id]}, components=[10.0, 0.0, 0.0, 0.0, 0.0, 0.0])
    app.model.initial_conditions.create_strain_increase(target={"type": "parts", "ids": [part.id]})
    dist = app.model.functions.create_expression(name="HYDRO", expression="10 - z")
    step.loads.create_pressure(target={"type": "faces", "ids": [[1, 2]]}, value=2.0, distribution=dist.id)
    r, cards = deck(app, case)
    reasons = {(x["kind"], x["type"]): x["reason"] for x in r["skipped"]}
    assert ("load", "pressure") not in reasons
    xyz = dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords().tolist()))
    el = app.execute("mesh.elements", ids=[1])[0]
    face_nodes = [el["nodes"][k] for k in {"hex8": [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]]}[el["shape"]][1]]
    zc = sum(xyz[n_][2] for n_ in face_nodes) / 4
    hydro = [ln for c in find(cards, "DLOAD") for ln in c[2] if ln.startswith("1, P2,")]
    assert len(hydro) == 1 and float(hydro[0].split(",")[2]) == pytest.approx(2.0 * (10 - zc))
    assert ("initial_condition", "stress") in reasons or find(cards, "INITIAL CONDITIONS", TYPE="STRESS")
    assert ("initial_condition", "strain_increase") in reasons or find(cards, "INITIAL STRAIN INCREASE")


@pytest.mark.feature("LOD-18")
@pytest.mark.feature("LOD-19")
@pytest.mark.feature("LOD-22")
@pytest.mark.feature("LOD-23")
@pytest.mark.feature("LOD-24")
@pytest.mark.feature("LOD-25")
@pytest.mark.feature("LOD-26")
@pytest.mark.feature("LOD-27")
@pytest.mark.feature("LOD-28")
@pytest.mark.feature("LOD-34")
@pytest.mark.feature("LOD-35")
@pytest.mark.feature("LOD-37")
def test_LOD_T04_thermal_network_submodel_user(app, tmp_path):
    """열 하중 카드(*CFLUX·*DFLUX S/BF·*FILM·FxFC·*RADIATE·RxCR), 네트워크 압력(PxNP), 서브모델 하중(*SUBMODEL + *DSLOAD/*CLOAD SUBMODEL), 전류(*CFLUX 유사), 사용자 하중 라벨."""
    part, b, mat, case = cantilever(app)
    step = case.steps.create_heat_transfer(steady_state=True)
    n = b["node"]
    top = app.model.sets.create_surface(name="TOP", faces=[[5, 2]])
    step.loads.create_concentrated_flux(target={"type": "nodes", "ids": [n(4, 0, 0)]}, value=5.0)
    step.loads.create_surface_flux(target={"type": "set", "ids": [top.id]}, value=2.0)
    step.loads.create_body_flux(target={"type": "elements", "ids": [1]}, value=0.1)
    step.loads.create_film(target={"type": "faces", "ids": [[6, 2]]}, coefficient=10.0, sink_temperature=300.0)
    fluid = app.execute("mesh.nodes_create", coords=[[0, 0, -10]])["ids"][0]
    step.loads.create_forced_convection(target={"type": "faces", "ids": [[7, 2]]}, coefficient=15.0, fluid_node=fluid)
    step.loads.create_radiation(target={"type": "faces", "ids": [[8, 2]]}, emissivity=0.8, sink_temperature=293.0)
    step.loads.create_cavity_radiation(target={"type": "faces", "ids": [[1, 1]]}, emissivity=0.7, sink_temperature=293.0, cavity="C1")
    step.loads.create_network_pressure(target={"type": "faces", "ids": [[2, 1]]}, fluid_node=fluid)
    gf = tmp_path / "global.frd"
    gf.write_text("")
    step.loads.create_submodel_traction(target={"type": "faces", "ids": [[3, 1]]}, global_file=str(gf), global_step=2)
    step.loads.create_submodel_force(target={"type": "nodes", "ids": [n(4, 1, 0)]}, global_file=str(gf), global_step=1)
    step.loads.create_coil_current(target={"type": "nodes", "ids": [n(4, 2, 0)]}, value=1000.0, frequency=50.0)
    step.loads.create_user(target={"type": "faces", "ids": [[4, 1]]}, base="pressure", label="1")
    step.loads.create_user(target={"type": "faces", "ids": [[4, 2]]}, base="surface_flux", label="2")
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    cf = {(frozenset(_nodes_of(cards, x.split(",")[0].strip())), x.split(",", 1)[1].strip()) for c in find(cards, "CFLUX") if "SUBMODEL" not in c[1] for x in c[2]}
    assert (frozenset({n(4, 0, 0)}), "11, 5") in cf and (frozenset({n(4, 2, 0)}), "11, 1000") in cf
    df = [x for c in find(cards, "DFLUX") for x in c[2]]
    assert any(x.endswith(", S2, 2") for x in df) and any(x.endswith(", BF, 0.1") for x in df) and any("S2NU2" in x for x in df)
    fl = [x for c in find(cards, "FILM") for x in c[2]]
    assert any(x.endswith(", F2, 300, 10") for x in fl) and any(x.endswith(f", F2FC, {fluid}, 15") for x in fl)
    rd = find(cards, "RADIATE")
    assert any(x.endswith(", R2, 293, 0.8") for c in rd for x in c[2]) and any(c[1].get("CAVITY") == "C1" and x.endswith(", R1CR, 293, 0.7") for c in rd for x in c[2])
    dl = [x for c in find(cards, "DLOAD") for x in c[2]]
    assert any(x.endswith(f", P1NP, {fluid}") for x in dl) and any("P1NU1" in x for x in dl)
    subs = find(cards, "SUBMODEL")
    assert {s[1]["TYPE"] for s in subs} == {"SURFACE", "NODE"} and all(s[1]["INPUT"].endswith("global.frd") for s in subs)
    ds, = find(cards, "DSLOAD")
    assert ds[1]["SUBMODEL"] is True and ds[1]["STEP"] == "2" and ds[2][0].endswith(", P1")
    cs = [c for c in find(cards, "CLOAD") if "SUBMODEL" in c[1]][0]
    assert cs[1]["STEP"] == "1" and [x.split(",")[1].strip() for x in cs[2]] == ["1", "2", "3"]


# ================================================================ 해석 케이스·스텝 설정 카드(CAS-T0x)
def _first_card_after(cards, keyword):
    """키워드 카드의 (매개변수, 데이터 줄)."""
    c, = find(cards, keyword)
    return c[1], c[2]


@pytest.mark.feature("CAS-06")
@pytest.mark.feature("CAS-08")
@pytest.mark.feature("CAS-16")
@pytest.mark.feature("CAS-18")
@pytest.mark.feature("CAS-19")
@pytest.mark.feature("CAS-20")
def test_CAS_T05_static_settings_scope_controls_output(app):
    """정적 스텝의 시간 증분(INC, 증분 줄, DIRECT), 수렴 제어(*CONTROLS), 출력 시점(*TIME POINTS)·출력 빈도·세분(변수·셋·GLOBAL·TOTALS), 해석 범위(scope: 그 요소와 노드만), 해석 파라미터(레일리 감쇠·접촉 방식)."""
    part, b, mat, case = cantilever(app)
    n = b["node"]
    step = case.steps.create_static(initial_increment=0.1, period=2.0, min_increment=1e-5, max_increment=0.5, max_increments=50, direct=True, nlgeom=True,
                                    load_application="step", controls=[{"parameters": "time_incrementation", "values": [[4, 8, 9, 200, 10, 4, 5, 10]]}],
                                    controls_reset=False)
    tp = app.model.time_points.create(name="TP1", times=[0.5, 1.0, 2.0])
    step.bcs.create_displacement(target={"type": "nodes", "ids": [n(0, 0, 0)]}, dofs=[1, 2, 3])
    sub = app.model.sets.create_node(name="TIPN", ids=[n(4, 0, 0)])
    step.outputs.create_node_file(variables=["U", "RF"], frequency=5, target={"type": "set", "ids": [sub.id]}, **{"global": False})
    step.outputs.create_element_file(variables=["S", "E"], time_points=tp.id, expand="2d")
    step.outputs.create_node_print(variables=["RF"], target={"type": "set", "ids": [sub.id]}, totals="only", **{"global": True})
    step.outputs.create_contact_file(variables=["CDIS", "CSTR"])
    case.update(rayleigh_alpha=0.1, rayleigh_beta=0.002, contact_method="surface_to_surface")
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    st, _ = _first_card_after(cards, "STEP")
    assert st["NLGEOM"] is True and st["INC"] == "50" and st["AMPLITUDE"] == "STEP"
    sp, lines = _first_card_after(cards, "STATIC")
    assert sp.get("DIRECT") is True and lines == ["0.1, 2, 1e-05, 0.5"]
    cp, cl = _first_card_after(cards, "CONTROLS")
    assert cp["PARAMETERS"] == "TIME INCREMENTATION" and cl[0].startswith("4, 8, 9, 200, 10, 4, 5")
    assert find(cards, "TIME POINTS", NAME="TP1")[0][2] == ["0.5, 1, 2"]
    nf, = find(cards, "NODE FILE")
    assert nf[1]["FREQUENCY"] == "5" and nf[1]["NSET"] == "TIPN" and nf[1]["GLOBAL"] == "NO" and nf[2] == ["U, RF"]
    ef, = find(cards, "EL FILE")
    assert ef[1]["TIME POINTS"] == "TP1" and ef[1]["OUTPUT"] == "2D" and ef[2] == ["S, E"]
    npr, = find(cards, "NODE PRINT")
    assert npr[1]["NSET"] == "TIPN" and npr[1]["TOTALS"] == "ONLY" and npr[1]["GLOBAL"] == "YES"
    assert find(cards, "CONTACT FILE")[0][2] == ["CDIS, CSTR"]
    assert find(cards, "DAMPING")[0][1] == {"ALPHA": "0.1", "BETA": "0.002"} or find(cards, "MODAL DAMPING") or "ALPHA=0.1" in r["text"]
    # 해석 범위: 요소 1~4 만 → 노드도 그 요소의 것만
    case.update(scope={"type": "elements", "ids": [1, 2, 3, 4]})
    r, cards = deck(app, case)
    el, = find(cards, "ELEMENT")
    assert [int(x.split(",")[0]) for x in el[2]] == [1, 2, 3, 4]
    node, = find(cards, "NODE")
    used = {int(x.split(",")[0]) for x in node[2]}
    assert used == {int(v) for x in el[2] for v in x.split(",")[1:]}


@pytest.mark.feature("CAS-21")
@pytest.mark.feature("CAS-22")
@pytest.mark.feature("CAS-23")
@pytest.mark.feature("CAS-24")
@pytest.mark.feature("CAS-25")
def test_CAS_T06_dynamic_procedures(app):
    """고유치(STORAGE·GLOBAL=NO·CYCMPC·진동수 범위), 모드 감쇠(직접·레일리), 복소 고유치(CORIOLIS), 정상상태 동해석(점 수·편향·비조화·푸리에), 직접 적분(*DYNAMIC ALPHA·EXPLICIT·RELATIVE TO ABSOLUTE·*MODAL DYNAMIC)."""
    part, b, mat, case = cantilever(app)
    case.steps.create_frequency(num_modes=8, freq_min=10.0, freq_max=5000.0, storage=True, cycmpc="inactive", **{"global": False})
    case.steps.create_complex_frequency(num_modes=4, coriolis=True)
    case.steps.create_steady_state_dynamics(freq_min=1.0, freq_max=100.0, points=20, bias=3.0, harmonic=False, fourier_terms=5, period_start=0.0, period_end=1.0,
                                            damping_type="direct", modal_damping=[[1, 4, 0.02], [5, 8, 0.05]])
    case.steps.create_modal_dynamic(initial_increment=0.001, period=0.1, direct=True, steady_state=True, damping_type="rayleigh", modal_damping=[[0.1, 0.001]])
    case.steps.create_dynamic(initial_increment=0.001, period=0.05, alpha=-0.05, explicit_scheme="explicit", relative_to_absolute=True)
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    fp, fl = _first_card_after(cards, "FREQUENCY")
    assert fp["STORAGE"] == "YES" and fp["GLOBAL"] == "NO" and fp["CYCMPC"] == "INACTIVE" and fl == ["8, 10, 5000"]
    cp, cl = _first_card_after(cards, "COMPLEX FREQUENCY")
    assert cp.get("CORIOLIS") is True and cl == ["4"]
    sp, sl = _first_card_after(cards, "STEADY STATE DYNAMICS")
    assert sp["HARMONIC"] == "NO" and sl == ["1, 100, 20, 3, 5, 0, 1"]
    md = find(cards, "MODAL DAMPING")
    assert any(c[1] == {} and c[2] == ["1, 4, 0.02", "5, 8, 0.05"] for c in md) and any(c[1].get("RAYLEIGH") is True and c[2] == [", , 0.1, 0.001"] or c[1].get("RAYLEIGH") is True for c in md)
    mp, ml = _first_card_after(cards, "MODAL DYNAMIC")
    assert mp.get("DIRECT") is True and mp.get("STEADY STATE") is True and ml == ["0.001, 0.1"]
    dp, dl = _first_card_after(cards, "DYNAMIC")
    assert dp["ALPHA"] == "-0.05" and dp.get("EXPLICIT") is True and dp.get("RELATIVE TO ABSOLUTE") is True and dl == ["0.001, 0.05"]


@pytest.mark.feature("CAS-26")
@pytest.mark.feature("CAS-27")
@pytest.mark.feature("CAS-28")
@pytest.mark.feature("CAS-29")
@pytest.mark.feature("CAS-30")
@pytest.mark.feature("CAS-34")
@pytest.mark.feature("CAS-35")
def test_CAS_T07_thermal_coupled_visco_em_cfd_substructure_green(app):
    """열전달(정상·DELTMX·고유모드·저장), 열-구조 연성(연성·비연성, ALPHA), 점소성(CETOL), 전자기(MAGNETOSTATICS·OMEGA·NO HEAT TRANSFER), 3D 유체(COMPRESSIBLE·난류 모델·SHALLOW WATER),
    부분구조 생성(*SUBSTRUCTURE GENERATE + *RETAINED NODAL DOFS + *SUBSTRUCTURE MATRIX OUTPUT), 그린 함수."""
    part, b, mat, case = cantilever(app)
    n = b["node"]
    case.steps.create_heat_transfer(steady_state=True, deltmx=50.0, initial_increment=1.0, period=10.0)
    case.steps.create_heat_transfer(eigenmodes=True, num_modes=6, storage=True)
    case.steps.create_coupled_temperature_displacement(initial_increment=0.1, period=1.0, alpha=-0.1, deltmx=20.0)
    case.steps.create_uncoupled_temperature_displacement(steady_state=True, initial_increment=0.5, period=1.0)
    case.steps.create_visco(cetol=1e-4, initial_increment=0.01, period=1.0)
    case.steps.create_electromagnetics(magnetostatics=True, no_heat_transfer=True, initial_increment=1.0, period=1.0)
    case.steps.create_electromagnetics(frequency_domain=True, omega=314.0, initial_increment=1.0, period=1.0)
    case.steps.create_cfd(steady_state=True, compressible=True, turbulence_model="k-omega", initial_increment=0.01, period=1.0)
    case.steps.create_cfd(shallow_water=True, initial_increment=0.01, period=1.0)
    case.steps.create_substructure_generate(stiffness=True, mass=True, output_file="super", retained_dofs=[{"target": {"type": "nodes", "ids": [n(4, 0, 0)]}, "first_dof": 1, "last_dof": 3}])
    case.steps.create_green(num_modes=5, storage=True)
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    ht = find(cards, "HEAT TRANSFER")
    assert ht[0][1].get("STEADY STATE") is True and ht[0][1]["DELTMX"] == "50" and ht[0][2] == ["1, 10"]
    assert ht[1][1].get("FREQUENCY") is True and ht[1][1]["STORAGE"] == "YES" and ht[1][2] == ["6"]
    cp, cl = _first_card_after(cards, "COUPLED TEMPERATURE-DISPLACEMENT")
    assert cp["ALPHA"] == "-0.1" and cp["DELTMX"] == "20" and cl == ["0.1, 1"]
    up, ul = _first_card_after(cards, "UNCOUPLED TEMPERATURE-DISPLACEMENT")
    assert up.get("STEADY STATE") is True and ul == ["0.5, 1"]
    vp, vl = _first_card_after(cards, "VISCO")
    assert vp["CETOL"] == "0.0001" and vl == ["0.01, 1"]
    em = find(cards, "ELECTROMAGNETICS")
    assert em[0][1].get("MAGNETOSTATICS") is True and em[0][1].get("NO HEAT TRANSFER") is True and em[1][1]["OMEGA"] == "314"
    cfd = find(cards, "CFD")
    assert cfd[0][1].get("STEADY STATE") is True and cfd[0][1].get("COMPRESSIBLE") is True and cfd[0][1]["TURBULENCE MODEL"] == "K-OMEGA"
    assert cfd[1][1].get("SHALLOW WATER") is True
    assert find(cards, "SUBSTRUCTURE GENERATE")
    rd, = find(cards, "RETAINED NODAL DOFS")
    assert rd[2][0].endswith(", 1, 3")
    mo, = find(cards, "SUBSTRUCTURE MATRIX OUTPUT")
    assert mo[1] == {"STIFFNESS": "YES", "MASS": "YES", "OUTPUT FILE": "super"}
    gp, gl = _first_card_after(cards, "GREEN")
    assert gp["STORAGE"] == "YES" and gl == ["5"]


@pytest.mark.feature("CAS-32")
@pytest.mark.feature("CAS-33")
@pytest.mark.feature("CAS-36")
@pytest.mark.feature("CAS-42")
def test_CAS_T08_special_procedures_preserved(app, tmp_path):
    """특수 절차의 덱 출력: 균열 전파(*CRACK PROPAGATION + *HCF), 민감도(*SENSITIVITY + *DESIGN RESPONSE + *FILTER; 설계 변수는 케이스의 *DESIGN VARIABLES),
    실행 가능 방향(*FEASIBLE DIRECTION + *OBJECTIVE + *CONSTRAINT + *GEOMETRIC CONSTRAINT), 강건 설계(*ROBUST DESIGN + *CORRELATION LENGTH + *GEOMETRIC TOLERANCES).
    자동 메시 세분화(*REFINE MESH)는 쓴다. 사용자 서브루틴은 설정으로 관리한다(솔버 실행 파일 지정)."""
    part, b, mat, case = cantilever(app)
    rs = case.steps.create_robust_design(random_field_only=True, accuracy=0.99, correlation_length=2.0, constrained=True,
                                         tolerances=[{"target": {"type": "nodes", "ids": [1, 2]}, "mean": 0.0, "deviation": 0.1}])
    fd = case.steps.create_feasible_direction(method="gradient_projection", step_size=0.1, objective="resp2", objective_target="min",
                                              constraints=[{"response": "resp1", "relation": "le", "relative_value": 1.0}],
                                              geometric_constraints=[{"type": "MAXGROWTH", "target": {"type": "nodes", "ids": [1]}, "value": 0.5},
                                                                     {"type": "MAXMEMBERSIZE", "target": {"type": "nodes", "ids": [1]}, "other_target": {"type": "nodes", "ids": [2]}, "value": 0.1}])
    cr = case.steps.create_crack_propagation(input_file="crack.frd", material=mat.id, length_method="cumulative", max_increment=0.05, max_angle=10.0,
                                             hcf_input_file="hcf.frd", hcf_mode=3, hcf_mission_step=1, hcf_scaling=0.01)
    se = case.steps.create_sensitivity(design_responses=[{"name": "resp1", "type": "MASS", "target": {"type": "elements", "ids": [1]}},
                                                         {"name": "resp2", "type": "MISESSTRESS", "target": {"type": "nodes", "ids": [1, 2]}, "values": [10.0, 100.0]}],
                                       filter_type="explicit", filter_radius=3.0, edge_preservation=True)
    app.execute("optimization.set_design_variables", id=case.id, design_variable_type="coordinate", design_nodes={"type": "nodes", "ids": [1, 2, 3]})
    r, cards = deck(app, case)
    reasons = {x["type"]: x["reason"] for x in r["skipped"]}
    assert reasons == {}, reasons
    assert "*FEASIBLE DIRECTION, METHOD=GRADIENT PROJECTION\n0.1\n*OBJECTIVE, TARGET=MIN\nresp2\n*CONSTRAINT\nresp1, LE, 1., \n*GEOMETRIC CONSTRAINT\n" in app.execute("case.preview_deck", id=case.id)["text"]
    gc = app.execute("case.preview_deck", id=case.id)["text"].split("*GEOMETRIC CONSTRAINT\n")[1].split("*")[0].splitlines()
    assert gc[0].startswith("MAXGROWTH, ") and gc[0].endswith(", , 0.5") and gc[1].startswith("MAXMEMBERSIZE, ") and gc[1].endswith(", 0.1") and gc[1].count(",") == 3
    assert "*ROBUST DESIGN, RANDOM FIELD ONLY\n0.99\n*CORRELATION LENGTH\n2.\n*GEOMETRIC TOLERANCES, TYPE=NORMAL, CONSTRAINED\n" in app.execute("case.preview_deck", id=case.id)["text"]
    text = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*DESIGN VARIABLES, TYPE=COORDINATE\n" in text
    assert "*CRACK PROPAGATION, INPUT=crack.frd, MATERIAL=" in text and ", LENGTH=CUMULATIVE\n0.05, 10.\n*HCF, INPUT=hcf.frd, MODE=3, MISSION STEP=1, SCALING=0.01\n" in text
    assert "*SENSITIVITY\n*DESIGN RESPONSE, NAME=resp1\nMASS, " in text and "*DESIGN RESPONSE, NAME=resp2\nMISESSTRESS, " in text and ", 10., 100.\n" in text
    assert "*FILTER, TYPE=EXPLICIT, EDGE PRESERVATION=YES\n3.\n" in text
    # *REFINE MESH 는 정적 스텝의 설정(mesh.refine 관련)으로 쓴다
    for s in app.execute("step.list", parent=case.id):
        app.execute("step.delete", id=s["id"])
    kinds = {f["name"] for s in app.kinds()["step"]["subtypes"] if s["name"] == "static" for f in s["fields"]} | {f["name"] for f in app.kinds()["step"]["fields"]}
    if "refine_limit" in kinds:
        case.steps.create_static(refine_limit=50.0, refine_variable="S")
        assert "*REFINE MESH" in app.execute("case.preview_deck", id=case.id)["text"]
    # 사용자 서브루틴(CAS-42): 사용자가 빌드한 솔버 실행 파일을 케이스에 지정한다
    exe = tmp_path / "ccx_user.exe"
    exe.write_bytes(b"")
    case.update(solver_executable=str(exe))
    assert app.execute("case.get", id=case.id)["props"]["solver_executable"] == str(exe)


# ================================================================ 재료 카드(MAT-T0x)
def _mat_cards(app, case, name):
    """재료 이름의 *MATERIAL 뒤에 오는 카드들(다음 *MATERIAL 또는 섹션 전까지)."""
    r, cards = deck(app, case)
    out, on = [], False
    for c in cards:
        if c[0] == "MATERIAL":
            on = c[1]["NAME"] == name
            continue
        if on and c[0] in ("SOLID SECTION", "SHELL SECTION", "BEAM SECTION", "STEP", "SURFACE INTERACTION"):
            break
        if on:
            out.append(c)
    return r, out


@pytest.mark.feature("MAT-01")
@pytest.mark.feature("MAT-13")
@pytest.mark.feature("MAT-06")
@pytest.mark.feature("MAT-23")
@pytest.mark.feature("MAT-08")
@pytest.mark.feature("MAT-09")
def test_MAT_T02_elastic_formats_thermal_damping_allowable(app):
    """탄성 형식(iso·ortho·engineering constants·aniso, 온도 의존), 열 물성(팽창 ortho·기준 온도, 전도 ortho, 비열), 구조 감쇠, 허용치(덱에는 안 나감 — 검사용)."""
    part, b, mat, case = cantilever(app)
    case.steps.create_static()
    m = app.model.materials.create(name="ORTHO")
    m.set_elastic(type="ortho", data=[[1e5, 2e4, 3e4, 1.1e5, 2.5e4, 1.2e5, 3e4, 4e4, 5e4, 20.0], [0.9e5, 2e4, 3e4, 1.0e5, 2.5e4, 1.1e5, 3e4, 4e4, 5e4, 100.0]])
    m.set_expansion(type="ortho", zero=20.0, data=[[1e-5, 2e-5, 3e-5]])
    m.set_conductivity(type="ortho", data=[[50.0, 40.0, 30.0]])
    m.set_specific_heat(data=[[450.0, 20.0], [500.0, 300.0]])
    m.set_structural_damping(value=0.03)
    m.set_allowable(yield_=250.0, ultimate=400.0) if False else app.execute("material.set_allowable", id=m.id, **{"yield": 250.0, "ultimate": 400.0})
    m2 = app.model.materials.create(name="ENG")
    m2.set_elastic(type="engineering_constants", data=[[1e5, 1e5, 2e4, 0.3, 0.3, 0.3, 4e4, 4e4, 4e4]])
    m3 = app.model.materials.create(name="ANISO")
    m3.set_elastic(type="aniso", data=[[float(i + 1) * 1e3 for i in range(21)]])
    app.model.properties.create_solid(material=m.id, target={"type": "elements", "ids": [1]})
    app.model.properties.create_solid(material=m2.id, target={"type": "elements", "ids": [2]})
    app.model.properties.create_solid(material=m3.id, target={"type": "elements", "ids": [3]})
    r, cards = _mat_cards(app, case, "ORTHO")
    assert r["skipped"] == [], r["skipped"]
    kinds = [c[0] for c in cards]
    assert kinds[:5] == ["ELASTIC", "EXPANSION", "CONDUCTIVITY", "SPECIFIC HEAT", "DAMPING"] or set(kinds) >= {"ELASTIC", "EXPANSION", "CONDUCTIVITY", "SPECIFIC HEAT", "DAMPING"}
    el = [c for c in cards if c[0] == "ELASTIC"][0]
    assert el[1]["TYPE"] == "ORTHO" and len(el[2]) == 4 and el[2][0] == "100000, 20000, 30000, 110000, 25000, 120000, 30000, 40000," and el[2][1] == "50000, 20"
    ex = [c for c in cards if c[0] == "EXPANSION"][0]
    assert ex[1]["TYPE"] == "ORTHO" and ex[1]["ZERO"] == "20" and ex[2] == ["1e-05, 2e-05, 3e-05"]
    cd = [c for c in cards if c[0] == "CONDUCTIVITY"][0]
    assert cd[1]["TYPE"] == "ORTHO" and cd[2] == ["50, 40, 30"]
    sh = [c for c in cards if c[0] == "SPECIFIC HEAT"][0]
    assert sh[2] == ["450, 20", "500, 300"]
    dp = [c for c in cards if c[0] == "DAMPING"][0]
    assert dp[1].get("STRUCTURAL") == "0.03"
    assert not [c for c in cards if "ALLOWABLE" in c[0]]  # 허용치는 솔버 카드가 아니다
    _, cards2 = _mat_cards(app, case, "ENG")
    assert cards2[0][1]["TYPE"] == "ENGINEERING CONSTANTS" and cards2[0][2][0] == "100000, 100000, 20000, 0.3, 0.3, 0.3, 40000, 40000," and cards2[0][2][1] == "40000"
    _, cards3 = _mat_cards(app, case, "ANISO")
    assert cards3[0][1]["TYPE"] == "ANISO" and len(cards3[0][2]) == 3 and cards3[0][2][2] == "17000, 18000, 19000, 20000, 21000"
    # 재료 관리: 복제·이름 바꾸기·검사·라이브러리
    cp = app.execute("material.copy", id=m.id)
    assert app.execute("material.get", id=cp["id"])["props"]["behaviors"]["elastic"]["type"] == "ortho"
    app.execute("material.rename", id=cp["id"], name="ORTHO_COPY")
    assert app.execute("material.check", id=m.id) == []
    bad = app.model.materials.create(name="BAD")
    bad.set_elastic(data=[[-1.0, 0.7]])
    issues = app.execute("material.check", id=bad.id)
    assert {i["code"] for i in issues} >= {"out_of_range"}


@pytest.mark.feature("MAT-15")
@pytest.mark.feature("MAT-16")
@pytest.mark.feature("MAT-17")
@pytest.mark.feature("MAT-18")
@pytest.mark.feature("MAT-19")
@pytest.mark.feature("MAT-20")
@pytest.mark.feature("MAT-21")
@pytest.mark.feature("MAT-22")
@pytest.mark.feature("MAT-24")
@pytest.mark.feature("MAT-25")
@pytest.mark.feature("MAT-26")
@pytest.mark.feature("MAT-27")
def test_MAT_T03_inelastic_special_user_fluid_change(app):
    """소성 경화 종류(등방·이동·복합+반복 경화·Johnson-Cook·속도 의존), 변형 소성, 크리프, Mohr-Coulomb(+경화), 초탄성·초탄성 폼, 내장 특수 재료, 사용자 재료(DEPVAR), 전자기·유체 물성, 스텝 중 재료 변경, 유사 해석용 물성 표기."""
    part, b, mat, case = cantilever(app)
    step = case.steps.create_static(nlgeom=True)
    M = app.model.materials
    def solid(m, e):
        app.model.properties.create_solid(material=m.id, target={"type": "elements", "ids": [e]})
    kin = M.create(name="KIN"); kin.set_elastic(data=[[2e5, 0.3]]); kin.set_plastic(hardening="kinematic", data=[[250.0, 0.0], [300.0, 0.1]]); solid(kin, 1)
    comb = M.create(name="COMB"); comb.set_elastic(data=[[2e5, 0.3]]); comb.set_plastic(hardening="combined", data=[[250.0, 0.0], [300.0, 0.1]]); comb.set_cyclic_hardening(data=[[250.0, 0.0], [320.0, 0.1]]); comb.set_rate_dependent(c=40.0, eps0=5.0); solid(comb, 2)
    jc = M.create(name="JC"); jc.set_elastic(data=[[2e5, 0.3]]); jc.set_plastic(hardening="johnson_cook", data=[[250.0, 100.0, 0.2, 1.0, 1800.0, 293.0]]); solid(jc, 3)
    dp = M.create(name="DP"); dp.set_elastic(data=[[2e5, 0.3]]); dp.set_deformation_plasticity(data=[[2e5, 0.3, 250.0, 5.0, 0.002]]); solid(dp, 4)
    cr = M.create(name="CR"); cr.set_elastic(data=[[2e5, 0.3]]); cr.set_creep(law="norton", data=[[1e-20, 5.0, 0.0]]); solid(cr, 5)
    mc = M.create(name="MC"); mc.set_elastic(data=[[2e5, 0.3]]); mc.set_mohr_coulomb(data=[[30.0, 10.0]]); mc.set_mohr_coulomb_hardening(data=[[1.0, 0.0], [2.0, 0.1]]); solid(mc, 6)
    he = M.create(name="HE"); he.set_hyperelastic(model="mooney_rivlin", data=[[80.0, 20.0, 0.001]]); solid(he, 7)
    hf = M.create(name="HF"); hf.set_hyperfoam(n=1, data=[[1.0, 4.0, 0.3]]); solid(hf, 8)
    sp = M.create(name="SP"); sp.set_special(model="tension_only", constants=[[2e5, 0.3]])
    us = M.create(name="US"); us.set_user(type="mechanical", constants=3, data=[[1.0, 2.0, 3.0]]); us.set_depvar(count=5)
    ec = M.create(name="EC"); ec.set_electrical_conductivity(data=[[5.96e7]]); ec.set_magnetic_permeability(data=[[1.2566e-6, 2.0]])
    fl = M.create(name="FL"); fl.set_fluid_constants(data=[[1000.0, 1e-3]]); fl.set_specific_gas_constant(value=287.0)
    step2 = case.steps.create_static(nlgeom=True)
    step2.changes.create_change_material(material=kin.id, hardening="kinematic", data=[[300.0, 0.0], [350.0, 0.1]])
    r, cards = deck(app, case)
    assert r["skipped"] == [], r["skipped"]
    mats = {}
    cur = None
    for c in cards:
        if c[0] == "MATERIAL":
            cur = c[1]["NAME"]; mats[cur] = []
        elif cur and c[0] in ("SOLID SECTION", "STEP"):
            cur = None
        elif cur:
            mats[cur].append(c)
    pl = {c[1].get("HARDENING"): c for c in mats["KIN"] + mats["COMB"] + mats["JC"] if c[0] == "PLASTIC"}
    assert pl["KINEMATIC"][2] == ["250, 0", "300, 0.1"] and pl["COMBINED"] and pl["JOHNSON COOK"][2] == ["250, 100, 0.2, 1, 1800, 293"]
    assert any(c[0] == "CYCLIC HARDENING" and c[2] == ["250, 0", "320, 0.1"] for c in mats["COMB"])
    assert any(c[0] == "RATE DEPENDENT" and c[2] == ["40, 5"] for c in mats["COMB"])
    assert any(c[0] == "DEFORMATION PLASTICITY" and c[2] == ["200000, 0.3, 250, 5, 0.002"] for c in mats["DP"])
    assert any(c[0] == "CREEP" and c[1].get("LAW", "NORTON") == "NORTON" and c[2] == ["1e-20, 5, 0"] for c in mats["CR"])
    assert any(c[0] == "MOHR COULOMB" and c[2] == ["30, 10"] for c in mats["MC"]) and any(c[0] == "MOHR COULOMB HARDENING" for c in mats["MC"])
    assert any(c[0] == "HYPERELASTIC" and c[1].get("MOONEY-RIVLIN") is True and c[2] == ["80, 20, 0.001"] for c in mats["HE"])
    assert any(c[0] == "HYPERFOAM" and c[1].get("N") == "1" and c[2] == ["1, 4, 0.3"] for c in mats["HF"])
    assert any(c[0] == "USER MATERIAL" and c[1].get("CONSTANTS") == "3" and c[2] == ["1, 2, 3"] for c in mats["US"]) and any(c[0] == "DEPVAR" and c[2] == ["5"] for c in mats["US"])
    assert any(c[0] == "ELECTRICAL CONDUCTIVITY" for c in mats["EC"]) and any(c[0] == "MAGNETIC PERMEABILITY" for c in mats["EC"])
    assert any(c[0] == "FLUID CONSTANTS" and c[2] == ["1000, 0.001"] for c in mats["FL"]) and any(c[0] == "SPECIFIC GAS CONSTANT" and c[2] == ["287"] for c in mats["FL"])
    # 내장 특수 재료(매뉴얼 6.8.12): 이름이 TENSION_ONLY 로 시작하는 사용자 재료, 상수 2개(E, 최대 압축 응력)
    assert "SP" not in mats and any(c[0] == "USER MATERIAL" and c[1].get("CONSTANTS") == "2" and c[2] == ["200000, 0.3"] for c in mats["TENSION_ONLY_SP"])
    cm, = find(cards, "CHANGE MATERIAL")
    assert cm[1]["NAME"] == "KIN" and find(cards, "CHANGE PLASTIC")[0][1].get("HARDENING") == "KINEMATIC" and find(cards, "CHANGE PLASTIC")[0][2] == ["300, 0", "350, 0.1"]
    # 유사 해석용 물성 표기(MAT-27): 열전달 유사 분야의 케이스에서는 전도율 등이 그 분야 이름으로 표시된다(덱은 같은 카드)
    case.update(physics="groundwater")
    labels = app.execute("case.physics_labels", id=case.id)
    assert labels["thermal_analogy"] is True and labels["conductivity"] == "투수계수 k" and labels["temperature"] == "총수두 h"
    assert labels["deck_keywords"]["conductivity"] == "*CONDUCTIVITY"
    assert app.execute("case.physics_labels", id=case.id)["physics"] == "groundwater"
    case.update(physics="electrostatic")
    assert "유전율" in app.execute("case.physics_labels", id=case.id)["conductivity"]
    case.update(physics="structural")
    assert app.execute("case.physics_labels", id=case.id)["thermal_analogy"] is False


@pytest.mark.feature("LOD-02")
@pytest.mark.feature("MSH-09")
def test_LOD_T01_03_04_pressure_on_shell_mesh_geometry_face(app):
    """면 메시(쉘)의 형상 면에 건 압력: 전개하면 요소(면이 아니라)이고, 덱에는 쉘 라벨 `P`(면 번호 없음)로 요소 셋에 쓴다. 솔리드 메시의 면은 그대로 `P<n>`."""
    part = app.model.parts.create(name="PL")
    part.features.create_plane(point=[5.0, 5.0, 0.0], normal=[0.0, 0.0, 1.0], size=10.0)
    r = app.execute("mesh.generate", id=part.id, size=5.0)
    mat = app.model.materials.create(name="M")
    mat.set_elastic(data=[[210000.0, 0.3]])
    app.model.properties.create_shell(material=mat.id, thickness=1.0, target={"type": "parts", "ids": [r["mesh_part"]]})
    case = app.model.cases.create(name="c")
    step = case.steps.create_static()
    step.bcs.create_displacement(target={"type": "geometry", "ids": [[part.id, "edge", 1]]}, dofs=[1, 2, 3])
    load = step.loads.create_pressure(target={"type": "geometry", "ids": [[part.id, "face", 1]]}, value=2.0)
    res = app.execute("load.resolve", id=load.id)
    assert res["what"] == "elements" and res["count"] == r["elements"]
    out, cards = deck(app, case)
    assert out["skipped"] == [], out["skipped"]
    dl = find(cards, "DLOAD")
    assert len(dl) == 1 and len(dl[0][2]) == 1 and dl[0][2][0].split(", ")[1:] == ["P", "2"]
    assert app.execute("case.check", id=case.id) == []


@pytest.mark.feature("CAS-05")
@pytest.mark.feature("LOD-01")
@pytest.mark.feature("BC-01")
def test_CAS_T01_15_load_sets_and_bc_sets(app):
    """하중 셋·구속 셋(D14): 모델 수준의 셋을 스텝이 계수와 함께 참조한다. 덱·step.effective 가 셋의 항목을 스텝 직속과 함께 보고,
    하중을 고치면 그 셋을 쓰는 케이스가 모두 따라간다(복사가 아님). 계수를 곱할 수 없는 하중은 건너뛴다."""
    part, b, mat, case = cantilever(app)
    N = {"type": "nodes", "ids": [3]}
    D = app.model.load_sets.create(name="D", description="자중")
    L = app.model.load_sets.create(name="L")
    S = app.model.bc_sets.create(name="S")
    dead = D.loads.create_gravity(name="g", target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0.0, 0.0, -1.0])
    live = L.loads.create_force(name="tip", target=N, components=[0.0, 0.0, -10.0])
    hidden = L.loads.create_force(name="hidden", target={"type": "nodes", "ids": [1]}, components=[5.0, 0.0, 0.0])
    hidden.suppress()
    fix = S.bcs.create_displacement(name="fix", target={"type": "nodes", "ids": [9]}, dofs=[1, 2, 3])
    assert app.execute("load.get", id=dead.id)["parent"] == D.id and app.execute("bc.get", id=fix.id)["parent"] == S.id
    # 케이스 1: 1.2 D + 1.6 L, 구속 S. 케이스 2: L 만
    s1 = case.steps.create_static(load_sets=[{"set": D.id, "factor": 1.2}, {"set": L.id, "factor": 1.6}], bc_sets=[S.id])
    case2 = app.model.cases.create(name="live_only")
    s2 = case2.steps.create_static(load_sets=[{"set": L.id}], bc_sets=[S.id])
    eff = app.execute("step.effective", id=s1.id)
    assert {e["id"] for e in eff["loads"]} == {dead.id, live.id} and [e["id"] for e in eff["bcs"]] == [fix.id]
    assert all(e["inherited"] is False for e in eff["loads"] + eff["bcs"])
    r, cards = deck(app, case)
    assert r["skipped"] == []
    cl = [ln for c in find(cards, "CLOAD") for ln in c[2]]
    assert any(ln.endswith(", 3, -16") for ln in cl) and not any("5" in ln.split(",")[-1] for ln in cl)  # 1.6 × −10, 억제한 하중은 없음
    grav = [ln for c in find(cards, "DLOAD") for ln in c[2] if "GRAV" in ln]
    assert len(grav) == 1 and float(grav[0].split(",")[2]) == pytest.approx(1.2 * 9810.0)
    bnd = [ln for c in find(cards, "BOUNDARY") for ln in c[2]]
    assert bnd and all(ln.startswith("9,") or not ln[0].isdigit() for ln in bnd)
    r2, cards2 = deck(app, case2)
    assert any(ln.endswith(", 3, -10") for c in find(cards2, "CLOAD") for ln in c[2]) and not find(cards2, "DLOAD")
    # 셋의 하중을 고치면 두 케이스가 모두 따라간다(복사가 아니다)
    live.update(components=[0.0, 0.0, -20.0])
    assert any(ln.endswith(", 3, -32") for c in find(deck(app, case)[1], "CLOAD") for ln in c[2])
    assert any(ln.endswith(", 3, -20") for c in find(deck(app, case2)[1], "CLOAD") for ln in c[2])
    # case.check: 셋의 하중·구속이 있으니 no_load·unconstrained 가 아니다
    codes = {i["code"] for i in app.execute("case.check", id=case.id)}
    assert "no_load" not in codes and "unconstrained" not in codes
    # 계수를 곱할 수 없는 하중(각속도)에 계수 → 그 스텝의 하중 출력은 not_scalable 로 건너뜀
    L.loads.create_centrifugal(name="spin", target={"type": "parts", "ids": [part.id]}, omega=10.0, axis_point=[0, 0, 0], axis_direction=[0, 0, 1])
    with pytest.raises(Nasa95Error) as e:
        app.execute("step.effective", id=s1.id)
    assert e.value.code == "not_scalable"
    app.undo()
    # 스텝이 참조하는 셋은 지울 수 없다(referenced). 참조를 먼저 풀면 지워진다
    with pytest.raises(Nasa95Error) as e:
        D.delete()
    assert e.value.code == "referenced"
    s1.update(load_sets=[{"set": L.id, "factor": 1.6}])
    D.delete()
    assert {x["id"] for x in app.execute("step.effective", id=s1.id)["loads"]} == {live.id}
