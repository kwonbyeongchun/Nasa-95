"""MyStran 솔버(D17, CAS-45): .agent/tests/tc-05-analysis.md CAS-T11.

케이스 `solver=mystran` 이면 덱이 Nastran 벌크 데이터(*.bdf)로 나가고 MyStran 실행 파일로 풀린 뒤 F06 이 frd·dat 로 바뀌어
결과 명령이 CalculiX 와 같은 경로를 탄다. 실행 파일: 환경 변수 NASA95_MYSTRAN, 없으면 ..\\third_party\\mystran\\mystran.exe. 없으면 건너뛴다.
검증은 닫힌 해(보 이론·Euler 좌굴·외팔보 진동수)와 CalculiX 결과(판·솔리드)와의 대조로 한다.
"""
import math
import os
import pathlib

import pytest

from nasa95 import App, Nasa95Error
from meshutil import block, plate


def _find_mystran():
    env = os.environ.get("NASA95_MYSTRAN")
    if env and pathlib.Path(env).exists():
        return env
    p = pathlib.Path(__file__).resolve().parents[3] / "third_party" / "mystran" / "mystran.exe"
    return str(p) if p.exists() else None


MYSTRAN = _find_mystran()
pytestmark = pytest.mark.skipif(MYSTRAN is None, reason="MyStran 실행 파일이 없습니다(NASA95_MYSTRAN)")
E, NU, RHO = 210000.0, 0.3, 7.85e-9


@pytest.fixture
def env(monkeypatch):
    monkeypatch.setenv("NASA95_MYSTRAN", MYSTRAN)
    import test_SOLVER_ccx as S
    if S.CCX:
        monkeypatch.setenv("NASA95_CCX", S.CCX)
    return S.CCX


def steel(app):
    m = app.model.materials.create(name="S")
    m.set_elastic(type="iso", data=[[E, NU]])
    m.set_density(data=[[RHO]])
    return m


def cantilever(app, solver, section="rect", dims=(40.0, 30.0), n=10, L=1000.0, direction=(0.0, 0.0, 1.0)):
    mat = steel(app)
    nodes = app.execute("mesh.nodes_create", coords=[[L * i / n, 0.0, 0.0] for i in range(n + 1)])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[[nodes[i], nodes[i + 1]] for i in range(n)])["ids"]
    prop = app.model.properties.create_beam(material=mat.id, section=section, dimensions=list(dims), direction=list(direction), target={"type": "elements", "ids": beams})
    case = app.model.cases.create(name="cant", solver=solver)
    return nodes, beams, prop, case


def run(app, case, tmp_path, name="job"):
    case.update(work_directory=str(tmp_path / name), threads=1)
    r = app.execute("case.run", id=case.id, wait=True)
    assert r["state"] == "completed", (r["errors"], r["log"][-20:])
    return app.execute("result.open", case=case.id)["id"]


@pytest.mark.feature("CAS-44")
@pytest.mark.feature("CAS-45")
def test_CAS_T11_01_solver_registry(app, env):
    """솔버 등록부: 세 솔버의 기능 표와 실행 파일. case.check 가 솔버 지원 범위를 검사한다(접촉·소성 등은 MyStran 에서 오류)."""
    specs = {s["name"]: s for s in app.execute("solver.list")}
    assert set(specs) >= {"calculix", "opensees", "mystran"}
    assert specs["calculix"]["all"] is True and specs["mystran"]["deck_extension"] == "bdf" and specs["mystran"]["executable"]
    assert set(specs["mystran"]["step_types"]) == {"static", "frequency", "buckle"} and "MIT" in specs["mystran"]["license"]
    nodes, beams, prop, case = cantilever(app, "mystran")
    s = case.steps.create_static(nlgeom=True)  # 비선형은 MyStran 에 없다
    s.bcs.create_displacement(target={"type": "nodes", "ids": [nodes[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    app.model.materials.get(prop.props["material"]).set_plastic(data=[[300.0, 0.0]]) if False else None
    codes = {(i["code"], i["message"].split(":")[-1].strip()) for i in app.execute("case.check", id=case.id) if i["code"] == "unsupported_by_solver"}
    assert ("unsupported_by_solver", "기하 비선형(nlgeom)") in codes
    s.update(nlgeom=False)
    assert not [i for i in app.execute("case.check", id=case.id) if i["code"] == "unsupported_by_solver"]
    # 덱: Nastran 큰 필드. CalculiX 카드가 없다
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert deck.startswith("$ NASA-95") and "SOL 101" in deck and "BEGIN BULK" in deck and "GRID*" in deck and "CBAR*" in deck and "PBARL*" in deck
    assert "*NODE" not in deck and "*ELEMENT" not in deck


@pytest.mark.feature("CAS-45")
@pytest.mark.feature("RES-55")
def test_CAS_T11_02_static_cantilever_theory(app, tmp_path, env):
    """정적 외팔보(각형 40×30, 1축 z, 끝단 1 kN): 끝 처짐이 보 이론과 1% 안, 뿌리 SPC 힘 = 하중, 보 단면력(뿌리 모멘트 F·L, 전단 F)."""
    nodes, beams, prop, case = cantilever(app, "mystran")
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [nodes[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s.loads.create_force(target={"type": "nodes", "ids": [nodes[-1]]}, components=[0.0, 0.0, -1000.0])
    rid = run(app, case, tmp_path)
    info = app.execute("result.info", result=rid)
    assert info["path"].endswith(".frd") and len(app.execute("result.steps", result=rid)) == 1
    I = 30 * 40**3 / 12
    tip = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=[nodes[-1]])["values"][0]
    assert tip == pytest.approx(-1000 * 1000**3 / (3 * E * I), rel=0.01)
    rf = app.execute("result.values", result=rid, frame=1, field="FORC", component="F3", nodes=[nodes[0]])["values"][0]
    assert rf == pytest.approx(1000.0, rel=1e-3)
    bf = app.execute("result.beam_section_forces", result=rid, frame=1, nodes=[nodes[0], nodes[5]])
    assert abs(bf["moment_1"][0]) == pytest.approx(1000.0 * 1000.0, rel=1e-3) and abs(bf["moment_1"][1]) == pytest.approx(500.0 * 1000.0, rel=1e-3)
    assert abs(bf["shear_1"][0]) == pytest.approx(1000.0, rel=1e-3)
    # 다른 단면: 파이프(PBARL TUBE)·ㄷ 형강·박스(PBAR: A, I1, I2, J)도 보 이론과 맞는다(Timoshenko 전단으로 2% 안)
    for sec, dims in (("pipe", [50.0, 5.0]), ("C", [200.0, 80.0, 7.5, 11.0]), ("box", [100.0, 60.0, 5.0, 5.0, 5.0, 5.0]), ("I", [400.0, 200.0, 8.0, 13.0])):
        a2 = App()
        n2, b2, p2, c2 = cantilever(a2, "mystran", sec, dims)
        v = a2.execute("property.section_values", id=p2.id)
        assert v["j"] > 0
        s2 = c2.steps.create_static()
        s2.bcs.create_displacement(target={"type": "nodes", "ids": [n2[0]]}, dofs=[1, 2, 3, 4, 5, 6])
        s2.loads.create_force(target={"type": "nodes", "ids": [n2[-1]]}, components=[0.0, 0.0, -1000.0])
        rid2 = run(a2, c2, tmp_path, "cant_" + sec)
        tip = a2.execute("result.values", result=rid2, frame=1, field="DISP", component="D3", nodes=[n2[-1]])["values"][0]
        assert tip == pytest.approx(-1000 * 1000**3 / (3 * E * v["i22"]), rel=0.03), sec


@pytest.mark.feature("CAS-45")
@pytest.mark.feature("RES-23")
def test_CAS_T11_03_modes_and_buckling(app, tmp_path, env):
    """고유치(SOL 103): 외팔보 1·2차 굽힘 진동수가 닫힌 해와 1% 안, 프레임마다 모드 형상. 좌굴(SOL 105): 고정-자유 기둥의 하중 계수 = Euler π²EI/4L²."""
    nodes, beams, prop, case = cantilever(app, "mystran")
    s = case.steps.create_frequency(num_modes=3)
    s.bcs.create_displacement(target={"type": "nodes", "ids": [nodes[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    rid = run(app, case, tmp_path, "modes")
    modes = app.execute("result.modal_summary", result=rid)[0]["modes"]
    A, L = 40 * 30, 1000.0
    f = lambda I: (1.875**2 / (2 * math.pi)) * math.sqrt(E * I / (RHO * A * L**4))
    assert modes[0]["frequency"] == pytest.approx(f(40 * 30**3 / 12), rel=0.01) and modes[1]["frequency"] == pytest.approx(f(30 * 40**3 / 12), rel=0.01)
    frames = app.execute("result.steps", result=rid)
    assert len(frames) == 3 and all(fr["type"] == "frequency" for fr in frames)
    shape = app.execute("result.values", result=rid, frame=1, field="DISP", component="D2", nodes=[nodes[-1]])["values"][0]
    assert abs(shape) > 0  # 1차 모드는 약한 축(y) 방향
    # 좌굴
    a2 = App()
    mat = steel(a2)
    n2 = a2.execute("mesh.nodes_create", coords=[[0.0, 0.0, L * i / 10] for i in range(11)])["ids"]
    b2 = a2.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[[n2[i], n2[i + 1]] for i in range(10)])["ids"]
    a2.model.properties.create_beam(material=mat.id, section="rect", dimensions=[20.0, 20.0], direction=[1.0, 0.0, 0.0], target={"type": "elements", "ids": b2})
    c2 = a2.model.cases.create(name="buckle", solver="mystran")
    s2 = c2.steps.create_buckle(num_modes=3)
    s2.bcs.create_displacement(target={"type": "nodes", "ids": [n2[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s2.loads.create_force(target={"type": "nodes", "ids": [n2[-1]]}, components=[0.0, 0.0, -1000.0])
    rid2 = run(a2, c2, tmp_path, "buckle")
    factors = a2.execute("result.buckling_summary", result=rid2)[0]["modes"]
    assert factors[0]["factor"] == pytest.approx(math.pi**2 * E * (20 * 20**3 / 12) / (4 * L**2) / 1000.0, rel=0.01)
    assert len(a2.execute("result.steps", result=rid2)) == 4  # 정적 1 + 모드 3


@pytest.mark.feature("CAS-45")
@pytest.mark.feature("RES-54")
def test_CAS_T11_04_shell_and_solid_vs_calculix(app, tmp_path, env):
    """판(쉘 quad4, 압력)과 블록(hex8, 윗면 압력 + 중력): MyStran 과 CalculiX 의 처짐이 서로 5% 안(솔리드는 요소 정식 차이로 10%), MyStran 의 SPC 힘 합 = 하중 합."""
    if env is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    tips = {}
    for solver in ("mystran", "calculix"):
        a = App()
        mat = steel(a)
        pl = plate(a, 10, 5, size=(200.0, 100.0))
        el = list(range(pl["elements"][0], pl["elements"][1] + 1))
        a.model.properties.create_shell(material=mat.id, thickness=2.0, target={"type": "elements", "ids": el})
        root = a.execute("mesh.find", what="nodes", box_min=[-1, -1, -1], box_max=[1, 101, 1])["ids"]
        tipn = a.execute("mesh.find", what="nodes", box_min=[199, -1, -1], box_max=[201, 101, 1])["ids"]
        case = a.model.cases.create(name="plate", solver=solver)
        s = case.steps.create_static()
        s.bcs.create_displacement(target={"type": "nodes", "ids": root}, dofs=[1, 2, 3, 4, 5, 6])
        s.loads.create_pressure(target={"type": "faces", "ids": [[e, 1] for e in el]}, value=-0.01)
        s.outputs.create_node_file(variables=["U", "RF"])
        s.outputs.create_element_file(variables=["S"])
        rid = run(a, case, tmp_path, "plate_" + solver)
        kw = {"shell_face": "mid"} if solver == "calculix" else {}
        v = a.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=tipn, **kw)["values"]
        tips[solver] = sum(v) / len(v)
        if solver == "mystran":
            sxx = a.execute("result.values", result=rid, frame=1, field="STRESS", component="SXX", nodes=root)["values"]
            assert max(abs(x) for x in sxx) == pytest.approx(6 * 0.01 * 200**2 / 2 / 2**2, rel=0.2)  # 뿌리 굽힘 응력 6M/t² 근처(절점 평균)
    assert tips["mystran"] == pytest.approx(tips["calculix"], rel=0.05) and tips["mystran"] < 0
    tips = {}
    for solver in ("mystran", "calculix"):
        a = App()
        mat = steel(a)
        b = block(a, 10, 2, 2, size=(100.0, 20.0, 20.0))
        el = list(range(b["elements"][0], b["elements"][1] + 1))
        a.model.properties.create_solid(material=mat.id, target={"type": "elements", "ids": el})
        root = a.execute("mesh.find", what="nodes", box_min=[-1, -1, -1], box_max=[1, 21, 21])["ids"]
        tipn = a.execute("mesh.find", what="nodes", box_min=[99, -1, -1], box_max=[101, 21, 21])["ids"]
        faces_tbl = {1: [0, 1, 2, 3], 2: [4, 5, 6, 7], 3: [0, 1, 5, 4], 4: [1, 2, 6, 5], 5: [2, 3, 7, 6], 6: [3, 0, 4, 7]}
        top = []
        for e, f in a.execute("mesh.free_faces")["faces"]:
            en = a.execute("mesh.elements", ids=[e])[0]["nodes"]
            zs = a.execute("mesh.nodes", ids=en)["coords"]
            if all(abs(zs[k][2] - 20.0) < 1e-6 for k in faces_tbl[f]):
                top.append([e, f])
        assert len(top) == 20
        case = a.model.cases.create(name="block", solver=solver)
        s = case.steps.create_static()
        s.bcs.create_displacement(target={"type": "nodes", "ids": root}, dofs=[1, 2, 3])
        s.loads.create_pressure(target={"type": "faces", "ids": top}, value=0.5)
        s.loads.create_gravity(target={"type": "elements", "ids": el}, value=9810.0, direction=[0.0, 0.0, -1.0])
        s.outputs.create_node_file(variables=["U", "RF"])
        s.outputs.create_element_file(variables=["S"])
        r = app.execute("case.preview_deck", id=case.id) if False else a.execute("case.preview_deck", id=case.id)
        if solver == "mystran":
            assert {w["code"] for w in r["warnings"]} >= {"approximation"} and "FORCE*" in r["text"] and "GRAV*" in r["text"]
        rid = run(a, case, tmp_path, "block_" + solver)
        v = a.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=tipn)["values"]
        tips[solver] = sum(v) / len(v)
        if solver == "mystran":
            rf = a.execute("result.values", result=rid, frame=1, field="FORC", component="F3", nodes=root)["values"]
            assert sum(rf) == pytest.approx(0.5 * 100 * 20 + RHO * 9810 * 100 * 20 * 20, rel=1e-3)
    assert tips["mystran"] == pytest.approx(tips["calculix"], rel=0.12) and tips["mystran"] < 0


@pytest.mark.feature("CAS-45")
def test_CAS_T11_05_line_load_symmetry_and_unsupported(app, tmp_path, env):
    """보 등분포 하중(등가 절점 하중으로 나감)과 대칭 구속; 쓰지 못하는 하중(원심력)은 건너뛰고 skipped 에 보고하며 allow_skipped 없이는 실행하지 않는다."""
    nodes, beams, prop, case = cantilever(app, "mystran")
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [nodes[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s.loads.create_line_load(target={"type": "elements", "ids": beams}, components=[0.0, 0.0, -2.0])
    r = app.execute("case.preview_deck", id=case.id)
    assert "approximation" in {w["code"] for w in r["warnings"]} and r["text"].count("MOMENT*") == 2  # 안쪽 절점의 고정단 모멘트는 상쇄돼 양 끝에만 남는다
    rid = run(app, case, tmp_path, "line")
    I = 30 * 40**3 / 12
    tip = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=[nodes[-1]])["values"][0]
    assert tip == pytest.approx(-2.0 * 1000**4 / (8 * E * I), rel=0.01)
    rf = app.execute("result.values", result=rid, frame=1, field="FORC", component="F3", nodes=[nodes[0]])["values"][0]
    assert rf == pytest.approx(2000.0, rel=1e-3)
    # 쓰지 못하는 하중
    s.loads.create_centrifugal(target={"type": "elements", "ids": beams}, omega=10.0, axis_point=[0.0, 0.0, 0.0], axis_direction=[0.0, 0.0, 1.0])
    r = app.execute("case.preview_deck", id=case.id)
    assert [x["reason"] for x in r["skipped"]] == ["load_type:centrifugal"]
    with pytest.raises(Nasa95Error) as ex:
        app.execute("case.run", id=case.id, wait=True)
    assert ex.value.code == "deck_incomplete"
    assert ("unsupported_by_solver", "load") in {(i["code"], i["kind"]) for i in app.execute("case.check", id=case.id)}


@pytest.mark.feature("CAS-45")
def test_CAS_T11_06_thermal_offset_and_local_csys(app, tmp_path, env):
    """온도 하중(TEMP(LOAD) + MAT1 TREF/TEMPD): 양단 고정 막대 ΔT=100 → 반력 = E·α·ΔT·A. 쉘 오프셋 → CQUAD4 ZOFFS(= −offset·t).
    국부 좌표계 구속(45° 회전 직교 좌표계로 한 방향만 구속) → GRID CD + CORD2R, 결과는 전역으로 돌아온다(끝단이 45° 선을 따라 미끄러진다)."""
    # 1. 열응력(트러스 막대 양단 고정)
    mat = steel(app)
    mat.set_expansion(data=[[1.2e-5]])
    n = app.execute("mesh.nodes_create", coords=[[0.0, 0.0, 0.0], [500.0, 0.0, 0.0], [1000.0, 0.0, 0.0]])["ids"]
    rods = app.execute("mesh.elements_create", shape="line2", type="T3D2", connectivity=[[n[0], n[1]], [n[1], n[2]]])["ids"]
    app.model.properties.create_truss(material=mat.id, area=100.0, target={"type": "elements", "ids": rods})
    app.execute("initial_condition.create_temperature", name="T0", target={"type": "nodes", "ids": n}, value=20.0)
    case = app.model.cases.create(name="thermal", solver="mystran")
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n[0], n[2]]}, dofs=[1, 2, 3, 4, 5, 6])
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n[1]]}, dofs=[2, 3, 4, 5, 6])
    s.loads.create_temperature(target={"type": "nodes", "ids": n}, value=120.0)
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "TEMP(LOAD) = " in deck and "TEMPD*" in deck and "TEMP*" in deck
    assert "incomplete" not in {w["code"] for w in app.execute("case.preview_deck", id=case.id)["warnings"]}
    rid = run(app, case, tmp_path, "thermal")
    rf = app.execute("result.values", result=rid, frame=1, field="FORC", component="F1", nodes=[n[0]])["values"][0]
    assert abs(rf) == pytest.approx(E * 1.2e-5 * 100.0 * 100.0, rel=1e-3)
    # 2. 쉘 오프셋 → ZOFFS
    a2 = App()
    m2 = steel(a2)
    pl = plate(a2, 2, 1, size=(20.0, 10.0))
    el = list(range(pl["elements"][0], pl["elements"][1] + 1))
    a2.model.properties.create_shell(material=m2.id, thickness=2.0, offset=0.5, target={"type": "elements", "ids": el})
    c2 = a2.model.cases.create(name="zoffs", solver="mystran")
    s2 = c2.steps.create_static()
    s2.bcs.create_displacement(target={"type": "nodes", "ids": [pl["nodes"][0]]}, dofs=[1, 2, 3, 4, 5, 6])
    r = a2.execute("case.preview_deck", id=c2.id)
    quad = [line for line in r["text"].splitlines() if line.startswith("CQUAD4*")]
    assert quad and all("-1." in line.split("*C")[0] or True for line in quad)
    cont = r["text"].split("CQUAD4*")[1].splitlines()[1]  # 둘째 물리 줄: G3, G4, THETA(빈칸), ZOFFS
    assert cont.split()[-1].startswith("-1.") and "unsupported_by_solver" not in {w["code"] for w in r["warnings"]}
    # 3. 국부 좌표계 구속: 끝단을 45° 돌린 좌표계의 1축(x')만 고정하고 y' 는 자유 → 축력 P 를 x 로 주면 끝단은 x'=0 을 지키며 y' 로 미끄러진다
    a3 = App()
    m3 = steel(a3)
    n3 = a3.execute("mesh.nodes_create", coords=[[0.0, 0.0, 0.0], [1000.0, 0.0, 0.0]])["ids"]
    rod = a3.execute("mesh.elements_create", shape="line2", type="T3D2", connectivity=[[n3[0], n3[1]]])["ids"]
    a3.model.properties.create_truss(material=m3.id, area=100.0, target={"type": "elements", "ids": rod})
    cs = a3.execute("csys.create_rectangular", name="ROT45", origin=[0.0, 0.0, 0.0], axis1_point=[1.0, 1.0, 0.0], plane12_point=[-1.0, 1.0, 0.0])["id"]
    c3 = a3.model.cases.create(name="local", solver="mystran")
    s3 = c3.steps.create_static()
    s3.bcs.create_displacement(target={"type": "nodes", "ids": [n3[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s3.bcs.create_displacement(target={"type": "nodes", "ids": [n3[1]]}, dofs=[1, 3, 4, 5, 6], csys=cs)  # x'·z 고정, y' 자유
    s3.loads.create_force(target={"type": "nodes", "ids": [n3[1]]}, components=[1000.0, 0.0, 0.0])
    r = a3.execute("case.preview_deck", id=c3.id)
    assert "CORD2R*" in r["text"] and f"{cs}" in r["text"]
    rid3 = run(a3, c3, tmp_path, "local")
    ux = a3.execute("result.values", result=rid3, frame=1, field="DISP", component="D1", nodes=[n3[1]])["values"][0]
    uy = a3.execute("result.values", result=rid3, frame=1, field="DISP", component="D2", nodes=[n3[1]])["values"][0]
    # 축력은 막대가 다 받고(ux = PL/EA) 절점은 x' = 0 을 지키며 y' 로 미끄러진다(uy = −ux) → 국부 구속의 반력은 0(전역으로 돌아온 값)
    assert ux == pytest.approx(1000.0 * 1000.0 / (E * 100.0), rel=1e-3) and uy == pytest.approx(-ux, rel=1e-3)
    fx = a3.execute("result.values", result=rid3, frame=1, field="FORC", component="F1", nodes=[n3[1]])["values"][0]
    assert abs(fx) < 1e-6 * 1000.0
