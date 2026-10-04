"""덱 가져오기(3단계, CAS-11)와 해석하지 못한 카드의 보존(CAS-12).

케이스 정의: .agent/tests/tc-05-case.md (CAS-T04).
"""
import pytest

from openfep import App, OfepError

from conftest import total
from meshutil import block, plate
from test_CASE_deck import find, parse


def objs(app, kind, **kw):
    """목록의 객체를 속성까지 조회한다."""
    return [app.execute(f"{kind}.get", id=o["id"]) | {"type": o.get("type", "")} for o in app.execute(f"{kind}.list", **kw)]


def export(app, case_id, path):
    r = app.execute("case.export_deck", id=case_id, path=str(path))
    return r, open(path).read()


def normalized(text):
    """셋 카드는 적힌 순서와 무관하게, 나머지는 순서대로."""
    cards = parse(text)
    sets = sorted((c[0], tuple(sorted(c[1].items(), key=str)), tuple(c[2])) for c in cards if c[0] in ("NSET", "ELSET", "SURFACE"))
    rest = [c for c in cards if c[0] not in ("NSET", "ELSET", "SURFACE")]
    return sets, rest


def build(app):
    """가져오기가 읽을 수 있는 범위를 두루 쓰는 모델."""
    part = app.model.mesh_parts.create(name="BODY")
    b = block(app, 4, 2, 1, part=part.id)
    skin = app.model.mesh_parts.create(name="SKIN")
    sh = plate(app, 2, 1)
    shells = list(range(sh["elements"][0], sh["elements"][1] + 1))
    app.execute("mesh.transform", ids=shells, translate=[0.0, 0.0, -30.0])
    for e in app.execute("mesh.elements", ids=shells):
        app.execute("mesh.elements_delete", ids=[e["id"]])
        app.execute("mesh.elements_create", shape="quad4", connectivity=[e["nodes"]], ids=[e["id"]], part=skin.id)
    n = app.execute("mesh.nodes_create", coords=[[0, 0, 50], [10, 0, 50], [20, 0, 50]])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[n[:2], n[1:]])["ids"]
    spring = app.execute("mesh.create_connector", kind="spring", nodes=[n[0], n[2]])["ids"]
    spring2 = app.execute("mesh.create_connector", kind="spring_fixed_direction", nodes=[n[0], n[1]])["ids"]
    mass = app.execute("mesh.create_connector", kind="mass", nodes=[n[2]])["ids"]

    steel = app.model.materials.create(name="STEEL")
    steel.set_elastic(data=[[210000.0, 0.3, 20.0], [190000.0, 0.31, 400.0]])
    steel.set_density(data=[[7.85e-9]])
    steel.set_plastic(hardening="kinematic", data=[[250.0, 0.0], [300.0, 0.1]])
    steel.set_expansion(zero=20.0, data=[[1.2e-5]])
    steel.set_conductivity(data=[[50.0]])
    steel.set_specific_heat(data=[[4.6e8]])
    lam = app.model.materials.create(name="LAMINA")
    lam.set_elastic(type="engineering_constants", data=[[1e5, 8e3, 8e3, 0.3, 0.3, 0.4, 4e3, 4e3, 3e3, 20.0]])
    rubber = app.model.materials.create(name="RUBBER")
    rubber.set_hyperelastic(model="ogden", n=2, data=[[1.0, 2.0, 0.1, 3.0, 4.0, 0.2]])
    ori = app.model.orientations.create_rectangular(name="OR1", a=[1.0, 0.0, 0.0], b=[0.0, 1.0, 0.0], rotation_axis=3, rotation_angle=30.0)
    amp = app.model.functions.create_amplitude(name="RAMP", points=[[0.0, 0.0], [0.5, 1.0], [1.0, 0.25]], time="total")
    tp = app.model.time_points.create(name="TP1", times=[0.25, 0.5, 1.0])

    el = lambda ids: {"type": "elements", "ids": ids}
    P = app.model.properties
    P.create_solid(material=steel.id, orientation=ori.id, target={"type": "parts", "ids": [part.id]})
    P.create_shell(material=steel.id, thickness=2.0, offset=0.5, target=el(shells[:1]))
    P.create_composite(target=el(shells[1:]), layers=[{"thickness": 0.5, "material": lam.id, "orientation": ori.id},
                                                      {"thickness": 0.25, "material": steel.id}])
    P.create_beam(material=steel.id, section="rect", dimensions=[4.0, 2.0], direction=[0.0, 0.0, 1.0], target=el(beams))
    P.create_spring(stiffness=100.0, target=el(spring))
    P.create_spring(dof1=1, dof2=2, table=[[0.0, 0.0], [10.0, 1.0]], target=el(spring2))
    P.create_mass(mass=0.25, target=el(mass))
    app.model.initial_conditions.create_temperature(target={"type": "nodes", "ids": [1, 2, 3]}, value=20.0)

    fix = app.model.sets.create_surface(name="FIX", faces=[[1, 6], [5, 6]])
    tip = app.model.sets.create_node(name="TIP", ids=sorted(b["node"](4, j, k) for j in range(3) for k in range(2)))
    case = app.model.cases.create(name="job")
    s1 = case.steps.create_static(nlgeom=True, max_increments=200, initial_increment=0.1, period=1.0, min_increment=1e-5, max_increment=0.5,
                                  solver="pardiso")
    s1.bcs.create_displacement(target={"type": "set", "ids": [fix.id]}, dofs=[1, 2, 3])
    s1.bcs.create_displacement(target={"type": "set", "ids": [tip.id]}, dofs=[3], values=[-0.5], amplitude=amp.id)
    s1.bcs.create_temperature(target={"type": "nodes", "ids": [7]}, value=300.0)
    L = s1.loads
    top = {"type": "faces", "ids": [[1, 2], [2, 2], [4, 4]]}
    L.create_force(target={"type": "nodes", "ids": [3, 8]}, components=[1.0, 0.0, -2.0], amplitude=amp.id, time_delay=0.25)
    L.create_moment(target={"type": "set", "ids": [tip.id]}, components=[0.0, 3.0, 0.0])
    L.create_pressure(target=top, value=2.0)
    L.create_pressure(target=el(shells), value=0.5)
    L.create_gravity(target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0.0, 0.0, -1.0])
    L.create_centrifugal(target={"type": "parts", "ids": [part.id]}, omega=10.0, axis_point=[1.0, 2.0, 3.0], axis_direction=[0.0, 0.0, 1.0])
    L.create_temperature(target={"type": "set", "ids": [tip.id]}, value=100.0)
    L.create_concentrated_flux(target={"type": "nodes", "ids": [4]}, value=5.0)
    L.create_surface_flux(target=top, value=7.0)
    L.create_body_flux(target={"type": "parts", "ids": [part.id]}, value=0.5)
    L.create_film(target=top, coefficient=10.0, sink_temperature=293.0, coefficient_amplitude=amp.id)
    L.create_radiation(target={"type": "faces", "ids": [[1, 2]]}, emissivity=0.8, sink_temperature=293.0)
    O = s1.outputs
    O.create_node_file(variables=["U", "RF"], frequency=2)
    O.create_element_file(variables=["S", "E"], time_points=tp.id, section_forces=True)
    O.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]}, totals="only")
    O.create_element_print(variables=["S"], target={"type": "parts", "ids": [part.id]})
    s2 = case.steps.create_frequency(num_modes=8, freq_max=5000.0, storage=True, perturbation=True, loads_inheritance="new")
    s2.outputs.create_node_file(variables=["U"])
    s3 = case.steps.create_dynamic(alpha=-0.1, initial_increment=1e-4, period=0.01, bcs_inheritance="new")
    s3.bcs.create_fixed_current(target={"type": "set", "ids": [tip.id]}, dofs=[1, 2])
    case.steps.create_heat_transfer(steady_state=True)
    case.steps.create_buckle(num_modes=3, accuracy=0.01)
    case.steps.create_steady_state_dynamics(freq_min=10.0, freq_max=1000.0, points=20)
    return case


# ================================================================ 왕복
@pytest.mark.feature("CAS-11")
@pytest.mark.feature("CAS-10")
def test_CAS_T04_round_trip(app, tmp_path):
    case = build(app)
    paths = []
    for name in "abc":  # 케이스 이름은 파일 이름에서 오므로 폴더만 달리한다
        (tmp_path / name).mkdir()
        paths.append(tmp_path / name / "job.inp")
    ra, text_a = export(app, case.id, paths[0])
    assert ra["skipped"] == [] and ra["warnings"] == []
    other = App()
    r = other.execute("deck.import", path=str(paths[0]))
    assert r["preserved"] == [] and r["unsupported_types"] == {} and r["skipped_faces"] == 0  # 전부 해석했다
    assert (r["nodes"], r["elements"]) == (app.execute("project.info")["nodes"], app.execute("project.info")["elements"])
    assert r["created"] == {"material": 3, "orientation": 1, "function": 1, "time_points": 1, "property": 7,
                            "initial_condition": 1, "step": 6, "bc": 4, "load": 12, "output_request": 5}
    rb, text_b = export(other, r["case"], paths[1])
    assert rb["skipped"] == [] and rb["warnings"] == []
    assert normalized(text_b) == normalized(text_a)  # 다시 쓴 덱이 같은 내용이다(셋 카드의 순서만 다를 수 있다)
    # 한 번 가져온 모델은 다시 가져와도 글자 그대로 같은 덱이 된다
    third = App()
    r3 = third.execute("deck.import", path=str(paths[1]))
    assert export(third, r3["case"], paths[2])[1] == text_b


@pytest.mark.feature("CAS-11")
def test_CAS_T04_imported_model(app, tmp_path):
    """가져온 모델이 원래 모델과 같은 값을 갖는지(덱 글자가 아니라 객체로) 확인한다."""
    src = App()
    case = build(src)
    export(src, case.id, tmp_path / "a.inp")
    r = app.execute("deck.import", path=str(tmp_path / "a.inp"))
    assert app.execute("case.get", id=r["case"])["name"] == "a"
    mats = {m["name"]: m["id"] for m in app.execute("material.list")}
    steel = app.execute("material.get", id=mats["STEEL"])["props"]["behaviors"]
    assert steel["elastic"] == {"type": "iso", "data": [[210000.0, 0.3, 20.0], [190000.0, 0.31, 400.0]]}
    assert steel["plastic"] == {"hardening": "kinematic", "data": [[250.0, 0.0], [300.0, 0.1]]}
    assert steel["expansion"] == {"type": "iso", "zero": 20.0, "data": [[1.2e-5]]}
    lam = app.execute("material.get", id=mats["LAMINA"])["props"]["behaviors"]["elastic"]
    assert lam["type"] == "engineering_constants" and lam["data"] == [[1e5, 8e3, 8e3, 0.3, 0.3, 0.4, 4e3, 4e3, 3e3, 20.0]]
    assert app.execute("material.get", id=mats["RUBBER"])["props"]["behaviors"]["hyperelastic"] == {
        "model": "ogden", "n": 2, "data": [[1.0, 2.0, 0.1, 3.0, 4.0, 0.2]]}
    props = {p["type"]: p["props"] for p in objs(app, "property")}
    assert props["beam"]["dimensions"] == [4.0, 2.0] and props["beam"]["direction"] == [0.0, 0.0, 1.0]
    assert [layer["thickness"] for layer in props["composite"]["layers"]] == [0.5, 0.25]
    assert props["shell"]["thickness"] == 2.0 and props["shell"]["offset"] == 0.5 and props["mass"]["mass"] == 0.25
    springs = sorted((p["props"] for p in objs(app, "property") if p["type"] == "spring"), key=lambda q: "table" in q)
    assert springs[0]["stiffness"] == 100.0 and "dof1" not in springs[0]
    assert (springs[1]["dof1"], springs[1]["dof2"], springs[1]["table"]) == (1, 2, [[0.0, 0.0], [10.0, 1.0]])
    steps = objs(app, "step", parent=r["case"])
    assert [s["type"] for s in steps] == ["static", "frequency", "dynamic", "heat_transfer", "buckle", "steady_state_dynamics"]
    s1 = steps[0]["props"]
    assert (s1["nlgeom"], s1["max_increments"], s1["initial_increment"], s1["period"], s1["min_increment"], s1["max_increment"],
            s1["solver"]) == (True, 200, 0.1, 1.0, 1e-5, 0.5, "pardiso")
    assert steps[1]["props"]["loads_inheritance"] == "new" and steps[1]["props"]["perturbation"] is True
    assert steps[2]["props"]["bcs_inheritance"] == "new" and steps[2]["props"]["alpha"] == -0.1
    # 합력이 원래 모델과 같다(하중이 같은 곳에 같은 크기로 걸렸다)
    a = src.execute("load.resultant", id=src.execute("step.list", parent=case.id)[0]["id"])
    b = app.execute("load.resultant", id=steps[0]["id"])
    assert b["force"] == pytest.approx(a["force"]) and b["moment"] == pytest.approx(a["moment"])
    assert len(b["unsupported"]) == len(a["unsupported"])
    # 구속되는 노드도 같다
    def constrained(ap, step):
        out = set()
        for bc in objs(ap, "bc", parent=step):
            if bc["type"] == "displacement":
                out |= {(n, d) for n in ap.execute("bc.resolve", id=bc["id"])["nodes"] for d in bc["props"]["dofs"]}
        return out
    assert constrained(app, steps[0]["id"]) == constrained(src, src.execute("step.list", parent=case.id)[0]["id"])
    assert app.execute("case.check", id=r["case"]) is not None


# ================================================================ 다른 프로그램이 쓴 덱, 보존
FOREIGN = """*HEADING
Bracket model - written by another tool
*NODE
1, 0, 0, 0
2, 1, 0, 0
3, 1, 1, 0
4, 0, 1, 0
5, 0, 0, 1
6, 1, 0, 1
7, 1, 1, 1
8, 0, 1, 1
*ELEMENT, TYPE=C3D8R, ELSET=Solid
1, 1, 2, 3, 4, 5, 6, 7, 8
*ELEMENT, TYPE=DASHPOTA, ELSET=Damper
2, 5, 7
*NSET, NSET=Base, GENERATE
1, 4
*NSET, NSET=Top
5, 6, 7, 8
*MATERIAL, NAME=Steel
*ELASTIC
210000., .3
*DENSITY
7.85E-9
*MATERIAL, NAME=Odd
*ELASTIC, TYPE=ISO
1000., .3
*FLUID CONSTANTS
1., 2.
*SOLID SECTION, ELSET=Solid, MATERIAL=Steel
*DASHPOT, ELSET=Damper

0.5
*BOUNDARY
Base, 1, 3
*RESTART, WRITE, FREQUENCY=5
*STEP, NLGEOM=YES, INC=100
*STATIC
0.1, 1.
*CONTROLS, PARAMETERS=FIELD
0.005, 0.01
*BOUNDARY
5, 3, 3, -0.1
6, 3, 3, -0.1
7, 1
*CLOAD
Top, 3, -25.
8, 1, 2.
8, 2, 3.
*DLOAD
Solid, P2, 1.5
Solid, P1NU, 2.
*NODE FILE, WEIRD=1
U
*EL FILE
S, E
*END STEP
*STEP
*STATIC, WEIRD OPTION
*CLOAD
8, 1, 2.
*END STEP
*STEP
*SENSITIVITY
*END STEP
*STEP, PERTURBATION
*FREQUENCY
5
*END STEP
"""


@pytest.mark.feature("CAS-11")
@pytest.mark.feature("CAS-12")
def test_CAS_T04_foreign_deck_preserves_unknown(app, tmp_path):
    path = tmp_path / "bracket.inp"
    path.write_text(FOREIGN)
    r = app.execute("deck.import", path=str(path))
    assert app.execute("case.get", id=r["case"])["props"]["description"] == "Bracket model - written by another tool"
    assert r["created"]["material"] == 2 and r["created"]["property"] == 2 and r["created"]["step"] == 2  # *FLUID CONSTANTS 도 읽는다
    kept = [(p["keyword"], p["reason"]) for p in r["preserved"]]
    assert kept == [
        ("RESTART", "unknown_keyword"),
        ("DLOAD", "unknown_label"),          # 한 줄이라도 못 읽으면 카드 전체를 보존한다
        ("NODE FILE", "unknown_param"),      # 모르는 매개변수가 붙은 카드
        ("STEP", "unknown_param"),           # 절차를 못 읽으면 스텝 전체
        ("STEP", "missing_param"),           # *SENSITIVITY 에 *DESIGN RESPONSE 가 없다
    ]
    steps = objs(app, "step", parent=r["case"])
    assert [s["type"] for s in steps] == ["static", "frequency"]
    s1 = steps[0]["id"]
    # 모델 수준의 경계조건은 첫 스텝으로 간다. 노드 번호로 적힌 줄은 내용이 같은 것끼리 묶인다
    own_bc, own_load = (app.execute(f"step.own_{k}_set", id=s1)["id"] for k in ("bc", "load"))  # 가져온 하중·구속은 스텝 전용 셋에
    bcs = [(b["props"]["target"], b["props"]["dofs"], b["props"].get("values")) for b in objs(app, "bc", parent=own_bc)]
    base = [s["id"] for s in app.execute("set.list") if s["name"] == "Base"][0]
    assert bcs == [({"type": "set", "ids": [base]}, [1, 2, 3], None), ({"type": "nodes", "ids": [5, 6]}, [3], [-0.1]),
                   ({"type": "nodes", "ids": [7]}, [1], None)]
    loads = [(ld["type"], ld["props"]["components"]) for ld in objs(app, "load", parent=own_load)]
    assert loads == [("force", [0.0, 0.0, -25.0]), ("force", [2.0, 3.0, 0.0])]
    # 조회
    listed = app.execute("deck.unsupported", id=r["case"])
    assert [(b["keyword"], b["step"]) for b in listed] == [
        ("*RESTART", None), ("*STEP", None), ("*STEP", None), ("*DLOAD", s1), ("*NODE FILE", s1)]
    assert listed[0]["source"] == "bracket.inp:36" and listed[0]["lines"] == 1
    # 다시 쓰면 보존한 내용이 그대로, 제자리에 나간다
    out = app.execute("case.preview_deck", id=r["case"])
    text = out["text"]
    assert out["skipped"] == []
    assert "*MATERIAL, NAME=Odd\n*ELASTIC\n1000., 0.3\n*FLUID CONSTANTS\n1., 2.\n" in text  # 읽은 재료(*FLUID CONSTANTS 포함)가 우리 꼴로 나간다
    assert "*DASHPOT, ELSET=Damper\n\n0.5\n" in text  # 읽은 대시포트가 같은 꼴로 나간다(빈 자유도 줄)
    assert steps[0]["props"]["controls"] == [{"parameters": "field", "values": [[0.005, 0.01]]}]
    assert "*STEP\n*STATIC, WEIRD OPTION\n*CLOAD\n8, 1, 2.\n*END STEP\n" in text and "*STEP\n*SENSITIVITY\n*END STEP\n" in text
    first_step = text.index("*STEP, NLGEOM")
    assert text.index("*RESTART, WRITE, FREQUENCY=5") < first_step < text.index("*CONTROLS, PARAMETERS=FIELD") < text.index("*END STEP")
    assert text.index("Solid, P1NU, 2.") < text.index("*END STEP")
    # 통째로 보존한 스텝도 원래 순서 그대로: 정적 → (보존) → (보존) → 고유치
    assert first_step < text.index("WEIRD OPTION") < text.index("*SENSITIVITY") < text.index("*FREQUENCY")
    cards = parse(text)
    assert find(cards, "ELEMENT", TYPE="DASHPOTA")[0][1]["ELSET"] == "Damper"
    # 보존한 내용을 억제하면 나가지 않는다
    app.execute("deck_block.suppress", id=listed[0]["id"])  # *RESTART
    assert "*RESTART" not in app.execute("case.preview_deck", id=r["case"])["text"]
    # 한 번에 되돌린다
    while app.execute("project.info")["nodes"]:
        app.undo()
    assert app.execute("material.list") == [] and app.execute("deck.unsupported") == []


@pytest.mark.feature("CAS-11")
def test_CAS_T04_import_rules(app, tmp_path):
    path = tmp_path / "m.inp"
    path.write_text("*NODE\n1, 0, 0, 0\n2, 1, 0, 0\n*ELEMENT, TYPE=T3D2, ELSET=E1\n1, 1, 2\n*BOUNDARY\n1, 1, 3\n")
    before = total(app)
    r = app.execute("deck.import", path=str(path))
    # 스텝이 없으면 모델 수준의 경계조건은 보존한다
    assert [(p["keyword"], p["reason"]) for p in r["preserved"]] == [("BOUNDARY", "no_step")] and "step" not in r["created"]
    with pytest.raises(OfepError) as e:  # 메시가 있는 모델에는 가져오지 않는다
        app.execute("deck.import", path=str(path))
    assert e.value.code == "invalid_state"
    app.undo()
    assert total(app) == before
    with pytest.raises(OfepError) as e:
        app.execute("deck.import", path=str(tmp_path / "none.inp"))
    assert e.value.code == "io_error"
    path.write_text("*NODE\n1, 0, 0, 0\n*STEP\n*STATIC\n")  # *END STEP 이 없다
    r = app.execute("deck.import", path=str(path))
    assert [(p["keyword"], p["reason"]) for p in r["preserved"]] == [("STEP", "parse_error")]
    with pytest.raises(OfepError) as e:
        app.execute("deck_block.create", parent=r["case"])  # 내용이 없는 보존 블록은 만들 수 없다
    assert e.value.code == "missing_param"


@pytest.mark.feature("CAS-11")
@pytest.mark.feature("BC-07")
@pytest.mark.feature("BC-10")
def test_CAS_T04_round_trip_constraints_contact(app, tmp_path):
    """구속(식·MPC·강체·커플링·타이·순환대칭)과 접촉(속성·쌍·간극)이 가져오기로 복원된다."""
    import re
    from test_CASE_deck import find, parse
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    p1, p2, p3 = (app.model.mesh_parts.create(name=n) for n in ("LOW", "UP", "PAD"))
    block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=p1.id)
    block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=p2.id, origin=(0.0, 0.0, 5.0))
    block(app, 1, 1, 1, size=(4.0, 4.0, 2.0), part=p3.id, origin=(3.0, 3.0, 10.0))
    for p in (p1, p2, p3):
        app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [p.id]})
    S = app.model.sets
    low_top = S.create_surface(name="LOWTOP", faces=[[e, 2] for e in (1, 2, 3, 4)])
    up_bot = S.create_surface(name="UPBOT", faces=[[e, 1] for e in (5, 6, 7, 8)])
    up_top = S.create_surface(name="UPTOP", faces=[[e, 2] for e in (5, 6, 7, 8)])
    pad_bot = S.create_surface(name="PADBOT", faces=[[9, 1]])
    pad_top = S.create_surface(name="PADTOP", faces=[[9, 2]])
    ref = app.execute("mesh.nodes_create", coords=[[5.0, 5.0, 15.0]])["ids"][0]
    C = app.model.constraints
    C.create_equation(terms=[[3, 1, 1.0], [4, 1, -1.0], [5, 2, 0.5]])
    C.create_equation(terms=[[6, 3, 1.0], [7, 3, -1.0]])
    C.create_mpc_plane(nodes=[1, 2, 6, 7])
    C.create_mpc_meanrot(nodes=[1, 2, 3], pilot_node=ref)
    C.create_rigid_body(target={"type": "nodes", "ids": [5, 10]}, ref_node=ref)
    C.create_coupling_kinematic(surface={"type": "set", "ids": [pad_top.id]}, ref_node=ref, dofs=[1, 2, 3])
    C.create_coupling_distributing(surface={"type": "set", "ids": [pad_top.id]}, ref_node=ref, dofs=[3])
    C.create_tie(slave={"type": "set", "ids": [up_bot.id]}, master={"type": "set", "ids": [low_top.id]}, position_tolerance=0.1, adjust=False)
    inter = app.model.contact_properties.create(name="INT1", pressure_overclosure="linear", slope=1e6, sigma_inf=10.0, friction_coefficient=0.2,
                                                stick_slope=1e5, damping=0.01, conductance=[[100.0, 0.0], [200.0, 10.0]])
    app.model.contact_properties.create(name="INT2", pressure_overclosure="exponential", c0=0.01, p0=100.0)
    pair = app.model.contact_pairs.create(slave={"type": "set", "ids": [pad_bot.id]}, master={"type": "set", "ids": [up_top.id]},
                                          interaction=inter.id, small_sliding=True, adjust=0.05, clearance=0.01)
    case = app.model.cases.create(name="job")
    case.update(contact_method="surface_to_surface")
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [1, 2]}, dofs=[1, 2, 3])
    (tmp_path / "a").mkdir(), (tmp_path / "b").mkdir()
    ra, text_a = export(app, case.id, tmp_path / "a" / "job.inp")
    assert ra["skipped"] == []
    other = App()
    r = other.execute("deck.import", path=str(tmp_path / "a" / "job.inp"))
    assert r["preserved"] == []
    assert r["created"]["constraint"] == 8 and r["created"]["contact_property"] == 2 and r["created"]["contact_pair"] == 1
    kinds = sorted(c["type"] for c in objs(other, "constraint"))
    assert kinds == ["coupling_distributing", "coupling_kinematic", "equation", "equation", "mpc_meanrot", "mpc_plane", "rigid_body", "tie"]
    eq = [c["props"]["terms"] for c in objs(other, "constraint") if c["type"] == "equation"]
    assert eq == [[[3, 1, 1.0], [4, 1, -1.0], [5, 2, 0.5]], [[6, 3, 1.0], [7, 3, -1.0]]]
    props = {c["name"]: c["props"] for c in objs(other, "contact_property")}
    assert props["INT1"]["slope"] == 1e6 and props["INT1"]["friction_coefficient"] == 0.2 and props["INT1"]["conductance"] == [[100.0, 0.0], [200.0, 10.0]]
    assert props["INT2"] == {"pressure_overclosure": "exponential", "c0": 0.01, "p0": 100.0}
    cp = objs(other, "contact_pair")[0]["props"]
    assert cp["small_sliding"] is True and cp["adjust"] == 0.05 and cp["clearance"] == 0.01
    assert other.execute("case.get", id=r["case"])["props"]["contact_method"] == "surface_to_surface"
    rb, text_b = export(other, r["case"], tmp_path / "b" / "job.inp")
    strip = lambda t: re.sub(r"\bC\d+\b", "C#", t)  # 구속 이름은 객체 번호에서 나온다
    assert rb["skipped"] == [] and normalized(strip(text_b)) == normalized(strip(text_a))


@pytest.mark.feature("CAS-11")
@pytest.mark.feature("BC-04")
@pytest.mark.feature("LOD-13")
@pytest.mark.feature("CAS-17")
def test_CAS_T04_round_trip_transform_changes(app, tmp_path):
    """국부 좌표계(*TRANSFORM)·초기 속도·대시포트·간극·스텝 중 변경·접촉/단면 출력·셋 구속식이 왕복한다."""
    import re
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    mat.set_plastic(data=[[250.0, 0.0], [300.0, 0.1]])
    soft = app.model.materials.create(name="SOFT")
    soft.set_elastic(data=[[1000.0, 0.45]])
    p1, p2 = (app.model.mesh_parts.create(name=n) for n in ("LOW", "UP"))
    b = block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=p1.id)
    block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=p2.id, origin=(0.0, 0.0, 5.0))
    n = app.execute("mesh.nodes_create", coords=[[20.0, 0.0, 0.0], [30.0, 0.0, 0.0], [40.0, 0.0, 0.0]])["ids"]
    dash = app.execute("mesh.create_connector", kind="dashpot", nodes=[n[0], n[1]])["ids"]
    gap = app.execute("mesh.create_connector", kind="gap", nodes=[n[1], n[2]])["ids"]
    P = app.model.properties
    for p in (p1, p2):
        P.create_solid(material=mat.id, target={"type": "parts", "ids": [p.id]})
    P.create_dashpot(coefficient=0.5, target={"type": "elements", "ids": dash})
    P.create_gap(clearance=0.1, direction=[1.0, 0.0, 0.0], target={"type": "elements", "ids": gap})
    S = app.model.sets
    low_top = S.create_surface(name="LOWTOP", faces=[[e, 2] for e in (1, 2, 3, 4)])
    up_bot = S.create_surface(name="UPBOT", faces=[[e, 1] for e in (5, 6, 7, 8)])
    bottom = S.create_node(name="BOT", ids=sorted(b["node"](i, j, 0) for i in range(3) for j in range(3)))
    side = S.create_node(name="SIDE", ids=sorted(b["node"](2, j, 1) for j in range(3)))
    # 좌표계: z 축 둘레로 90° 돌린 직교 좌표계(국부 1축 = 전역 y), 그리고 z 축의 원통 좌표계
    rot = app.model.csys.create_rectangular(name="ROT", origin=[0.0, 0.0, 0.0], axis1_point=[0.0, 1.0, 0.0], plane12_point=[-1.0, 0.0, 0.0])
    cyl = app.model.csys.create_cylindrical(name="CYL", origin=[5.0, 5.0, 0.0], axis1_point=[6.0, 5.0, 0.0], plane12_point=[5.0, 6.0, 0.0])
    inter = app.model.contact_properties.create(name="INT1", pressure_overclosure="linear", slope=1e6, friction_coefficient=0.2)
    pair = app.model.contact_pairs.create(slave={"type": "set", "ids": [up_bot.id]}, master={"type": "set", "ids": [low_top.id]}, interaction=inter.id)
    C = app.model.constraints
    app.model.initial_conditions.create_velocity(target={"type": "set", "ids": [side.id]}, components=[0.0, 0.0, -1.5])
    app.model.initial_conditions.create_displacement(target={"type": "nodes", "ids": [n[2]]}, components=[0.1, 0.0, 0.0])
    app.execute("physical_constants.set", absolute_zero=-273.15, stefan_boltzmann=5.67e-11)
    case = app.model.cases.create(name="job", contact_method="surface_to_surface")  # *CONTACT PAIR 의 TYPE 은 필수(없으면 기본값 경고)
    s1 = case.steps.create_static(nlgeom=True)
    s1.bcs.create_displacement(target={"type": "set", "ids": [bottom.id]}, dofs=[1, 2, 3], csys=rot.id)
    s1.bcs.create_displacement(target={"type": "set", "ids": [side.id]}, dofs=[1], values=[0.2], csys=cyl.id)
    s1.bcs.create_displacement(target={"type": "nodes", "ids": [n[0]]}, dofs=[1, 2, 3])
    s1.loads.create_force(target={"type": "set", "ids": [side.id]}, components=[10.0, 0.0, 0.0], csys=cyl.id)
    s1.loads.create_force(target={"type": "nodes", "ids": [n[2]]}, components=[0.0, 0.0, -1.0])
    s1.set_controls(controls=[{"parameters": "time_incrementation", "values": [[4.0, 8.0, 9.0, 16.0, 10.0, 4.0, 0.0, 5.0, 10.0, 1.0]]}])
    s1.outputs.create_contact_print(variables=["CF"], pair=pair.id, totals="yes")
    s1.outputs.create_section_print(variables=["SOF", "SOM"], target={"type": "set", "ids": [low_top.id]}, label="SEC1")
    s2 = case.steps.create_static()
    ch = s2.changes
    ch.create_model_change_element(target={"type": "parts", "ids": [p2.id]}, action="remove")
    ch.create_model_change_contact(pair=pair.id, action="remove")
    ch.create_change_friction(interaction=inter.id, friction_coefficient=0.3)
    ch.create_change_surface_behavior(interaction=inter.id, pressure_overclosure="exponential", c0=0.01, p0=100.0)
    ch.create_change_material(material=mat.id, hardening="isotropic", data=[[260.0, 0.0], [320.0, 0.2]])
    ch.create_change_section(target={"type": "parts", "ids": [p1.id]}, material=soft.id)
    (tmp_path / "a").mkdir(), (tmp_path / "b").mkdir()
    ra, text_a = export(app, case.id, tmp_path / "a" / "job.inp")
    assert ra["skipped"] == [] and ra["warnings"] == []
    cards = parse(text_a)
    tr = [c for c in cards if c[0] == "TRANSFORM"]
    assert [c[1]["TYPE"] for c in tr] == ["R", "C"]
    assert tr[0][2] == ["0, 1, 0, -1, 0, 0"] and tr[1][2] == ["5, 5, 0, 5, 5, 1"]
    other = App()
    r = other.execute("deck.import", path=str(tmp_path / "a" / "job.inp"))
    assert r["preserved"] == []
    assert r["created"]["csys"] == 2 and r["created"]["step_change"] == 6 and r["created"]["initial_condition"] == 2
    cs = {c["name"]: c for c in objs(other, "csys")}
    assert cs["T_BOT"]["type"] == "rectangular" and cs["T_BOT"]["props"]["axis1_point"] == [0.0, 1.0, 0.0]
    assert cs["T_SIDE"]["type"] == "cylindrical" and cs["T_SIDE"]["props"]["origin"] == [5.0, 5.0, 0.0]
    bcs = objs(other, "bc")
    assert sorted(bc["props"].get("csys", 0) for bc in bcs) == [0, cs["T_BOT"]["id"], cs["T_SIDE"]["id"]]
    assert sorted(l["props"].get("csys", 0) for l in objs(other, "load")) == [0, cs["T_SIDE"]["id"]]
    ic = {c["type"]: c["props"] for c in objs(other, "initial_condition")}
    assert ic["velocity"]["components"] == [0.0, 0.0, -1.5] and ic["displacement"]["components"] == [0.1, 0.0, 0.0]
    kinds = sorted(c["type"] for c in objs(other, "step_change"))
    assert kinds == ["change_friction", "change_material", "change_section", "change_surface_behavior", "model_change_contact", "model_change_element"]
    cm = next(c["props"] for c in objs(other, "step_change") if c["type"] == "change_material")
    assert cm["data"] == [[260.0, 0.0], [320.0, 0.2]] and cm["hardening"] == "isotropic"
    outs = {o["type"]: o["props"] for o in objs(other, "output_request")}
    assert outs["contact_print"]["totals"] == "yes" and "pair" in outs["contact_print"] and outs["section_print"]["label"] == "SEC1"
    props = {p["type"]: p["props"] for p in objs(other, "property")}
    assert props["dashpot"]["coefficient"] == 0.5 and (props["gap"]["clearance"], props["gap"]["direction"]) == (0.1, [1.0, 0.0, 0.0])
    steps = objs(other, "step")
    assert steps[0]["props"]["controls"] == [{"parameters": "time_incrementation", "values": [[4.0, 8.0, 9.0, 16.0, 10.0, 4.0, 0.0, 5.0, 10.0, 1.0]]}]
    assert other.execute("physical_constants.set") == {"absolute_zero": -273.15, "stefan_boltzmann": 5.67e-11}  # 빈 호출은 현재 값
    rb, text_b = export(other, r["case"], tmp_path / "b" / "job.inp")
    strip = lambda t: re.sub(r"\bC\d+\b", "C#", t)
    assert rb["skipped"] == [] and normalized(strip(text_b)) == normalized(strip(text_a))


@pytest.mark.feature("CAS-11")
@pytest.mark.feature("CAS-10")
def test_CAS_T04_transform_rules(app, tmp_path):
    """좌표 변환의 규칙: 한 노드에 두 좌표계나 전역·국부가 섞이면 쓰지 못하고 알린다. 셋으로 적은 구속식은 노드마다 하나가 된다."""
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    part = app.model.mesh_parts.create(name="BODY")
    block(app, 1, 1, 1, part=part.id)
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [part.id]})
    rot = app.model.csys.create_rectangular(name="ROT", origin=[0.0, 0.0, 0.0], axis1_point=[0.0, 1.0, 0.0], plane12_point=[-1.0, 0.0, 0.0])
    cyl = app.model.csys.create_cylindrical(name="CYL", origin=[0.0, 0.0, 0.0], axis1_point=[1.0, 0.0, 0.0], plane12_point=[0.0, 1.0, 0.0])
    sph = app.model.csys.create_spherical(name="SPH", origin=[0.0, 0.0, 0.0], axis1_point=[1.0, 0.0, 0.0], plane12_point=[0.0, 1.0, 0.0])
    case = app.model.cases.create(name="job")
    s = case.steps.create_static()
    a = s.bcs.create_displacement(target={"type": "nodes", "ids": [1, 2]}, dofs=[1], csys=rot.id)
    b = s.bcs.create_displacement(target={"type": "nodes", "ids": [2]}, dofs=[2], csys=cyl.id)       # 노드 2 에 두 좌표계
    c = s.loads.create_force(target={"type": "nodes", "ids": [4]}, components=[1.0, 0.0, 0.0])         # 변환된 노드 4 에 전역 하중
    d = s.bcs.create_displacement(target={"type": "nodes", "ids": [3]}, dofs=[3], csys=sph.id)       # 구면 좌표계는 솔버에 없다
    e = s.bcs.create_displacement(target={"type": "nodes", "ids": [4]}, dofs=[1, 2], csys=rot.id)
    r = app.execute("case.preview_deck", id=case.id)
    reasons = {s["object"]: s["reason"] for s in r["skipped"]}
    assert reasons == {a.id: "transform_conflict", b.id: "transform_conflict", c.id: "transform_conflict", d.id: "transform_type"}
    cards = parse(r["text"])
    assert [c[2] for c in cards if c[0] == "TRANSFORM"] == [["0, 1, 0, -1, 0, 0"]]  # 남은 객체(e)의 노드 4 만 변환한다
    assert [c[2] for c in cards if c[0] == "NSET" and c[1]["NSET"] == "OFEP_N1"] == [["4"]]
    # 셋으로 적은 구속식
    deck = tmp_path / "eq.inp"
    deck.write_text("*NODE, NSET=NALL\n1, 0., 0., 0.\n2, 1., 0., 0.\n3, 2., 0., 0.\n4, 3., 0., 0.\n*ELEMENT, TYPE=T3D2, ELSET=E1\n1, 1, 2\n2, 2, 3\n3, 3, 4\n"
                    "*NSET, NSET=TOP\n2, 3\n*EQUATION\n2\nTOP, 1, 1.\n4, 1, -1.\n*EQUATION\n2\nTOP, 2, 1.\nTOP, 3, -1.\n")
    other = App()
    r = other.execute("deck.import", path=str(deck))
    eq = [c["props"]["terms"] for c in objs(other, "constraint")]
    assert eq == [[[2, 1, 1.0], [4, 1, -1.0]], [[3, 1, 1.0], [4, 1, -1.0]]]
    assert [p["reason"] for p in r["preserved"]] == ["unknown_param"] and r["preserved"][0]["keyword"] == "EQUATION"


@pytest.mark.feature("CAS-40")
def test_CAS_T04_split_deck(app, tmp_path):
    """덱 분할 출력: 메시(노드·요소·셋)는 <이름>_mesh.inp 로, 본 파일은 *INCLUDE 로 읽는다. 가져오기가 포함 파일을 따라 읽어 같은 모델이 된다."""
    case = build(app)
    (tmp_path / "a").mkdir(), (tmp_path / "b").mkdir()
    whole = export(app, case.id, tmp_path / "a" / "job.inp")[1]
    r = app.execute("case.export_deck", id=case.id, path=str(tmp_path / "b" / "job.inp"), split=True)
    assert r["files"]["mesh"].endswith("job_mesh.inp") and r["skipped"] == []
    main = (tmp_path / "b" / "job.inp").read_text()
    mesh = (tmp_path / "b" / "job_mesh.inp").read_text()
    assert "*INCLUDE, INPUT=job_mesh.inp\n" in main and "*NODE,\n" not in main and "*NODE\n" not in main and "*ELEMENT," not in main
    assert mesh.startswith("*NODE") and "*NSET" in mesh and "*STEP" not in mesh
    assert main.replace("*INCLUDE, INPUT=job_mesh.inp\n", mesh) == whole  # 합치면 분할하지 않은 덱과 글자 그대로 같다
    other = App()
    imported = other.execute("deck.import", path=str(tmp_path / "b" / "job.inp"))
    assert imported["preserved"] == [] and (imported["nodes"], imported["elements"]) == (app.execute("project.info")["nodes"], app.execute("project.info")["elements"])
    with pytest.raises(OfepError) as e:
        app.execute("case.export_deck", id=case.id, path=str(tmp_path / "b" / "한글 이름.inp"), split=True)
    assert e.value.code == "invalid_param"
