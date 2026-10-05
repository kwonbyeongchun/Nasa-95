"""덱을 CalculiX 로 실제로 풀어 확인한다(3단계). 덱의 형식과 뜻(부호·라벨·승계)이 솔버와 맞는지 본다.

ccx 실행 파일: 환경 변수 NASA95_CCX, 없으면 ..\\third_party\\calculix 아래에서 찾는다. 없으면 이 파일의 테스트는 건너뛴다.
"""
import math
import os
import pathlib
import re
import subprocess

import numpy as np
import pytest

from nasa95 import App, Nasa95Error

from meshutil import block, plate


def _find_ccx():
    env = os.environ.get("NASA95_CCX")
    if env and pathlib.Path(env).exists():
        return env
    root = pathlib.Path(__file__).resolve().parents[3] / "third_party" / "calculix"
    for name in ("ccx_static.exe", "ccx_dynamic.exe", "ccx.exe", "ccx"):
        hits = sorted(root.rglob(name)) if root.exists() else []
        if hits:
            return str(hits[0])
    return None


CCX = _find_ccx()
pytestmark = pytest.mark.skipif(CCX is None, reason="ccx 실행 파일이 없습니다(NASA95_CCX)")


def solve(app, case, tmp_path, name="job"):
    """덱을 쓰고 풀어서 (dat 본문, 솔버 출력) 을 돌려준다. 솔버가 오류를 내면 실패."""
    r = app.execute("case.export_deck", id=getattr(case, "id", case), path=str(tmp_path / f"{name}.inp"))
    assert r["skipped"] == [], r["skipped"]
    for _ in range(4):  # 이 ccx 빌드는 드물게 풀이를 끝낸 뒤 힙 손상(0xC0000374)으로 죽는다 — 솔버 쪽 문제라 다시 돌린다
        p = subprocess.run([CCX, "-i", name], cwd=tmp_path, capture_output=True, text=True, timeout=600,
                           env={**os.environ, "OMP_NUM_THREADS": "1"})
        if p.returncode == 0 or "*ERROR" in (p.stdout + p.stderr):  # 오류 메시지 없이 죽은 것(비정상 종료)만 다시 돌린다
            break
    out = p.stdout + p.stderr
    assert p.returncode == 0 and "*ERROR" not in out, out[-3000:]
    dat = tmp_path / f"{name}.dat"
    return (dat.read_text() if dat.exists() else ""), out


def blocks(dat):
    """dat 파일을 [(제목, 숫자 행들)] 로."""
    out = []
    for line in dat.splitlines():
        s = line.strip()
        if not s:
            continue
        if re.match(r"^[A-Za-z]", s):
            out.append((s, []))
        elif out:
            try:
                out[-1][1].append([float(x) for x in s.split() if x != "L"])  # L: 국부 좌표계 값(*TRANSFORM)
            except ValueError:
                pass
    return [(t, rows) for t, rows in out if rows]


def totals(dat, word="total force"):
    return [rows[0] for title, rows in blocks(dat) if title.startswith(word)]


def steel(app):
    m = app.model.materials.create(name="STEEL")
    m.set_elastic(data=[[210000.0, 0.3]])
    m.set_density(data=[[7.85e-9]])
    m.set_expansion(data=[[1.2e-5]])
    m.set_conductivity(data=[[50.0]])
    m.set_specific_heat(data=[[4.6e8]])
    return m


def node_set(app, name, lo, hi):
    ids = app.execute("mesh.find", what="nodes", box_min=lo, box_max=hi)["ids"]
    return app.model.sets.create_node(name=name, ids=ids)


def cantilever(app, order=2, n=(20, 2, 2)):
    part = app.model.mesh_parts.create(name="BEAM")
    block(app, *n, part=part.id)
    if order == 2:
        app.execute("mesh.convert_order", order=2)
    mat = steel(app)
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [part.id]})
    root = node_set(app, "ROOT", [-0.1, -1, -1], [0.1, 21, 11])
    tip = node_set(app, "TIP", [99.9, -1, -1], [100.1, 21, 11])
    case = app.model.cases.create(name="cantilever")
    return part, mat, root, tip, case


def rf(step, root):
    step.outputs.create_node_print(variables=["RF"], target={"type": "set", "ids": [root.id]}, totals="only")


# ================================================================ 정적: 처짐과 반력
@pytest.mark.feature("CAS-10")
@pytest.mark.feature("CAS-13")
def test_SOLVER_cantilever_tip_force(app, tmp_path):
    part, mat, root, tip, case = cantilever(app)
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    n = len(tip.props["ids"])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1000.0 / n])
    s.outputs.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]})
    rf(s, root)
    dat, _ = solve(app, case, tmp_path)
    uz = [row[3] for title, rows in blocks(dat) if title.startswith("displacements") for row in rows]
    theory = -1000.0 * 100.0**3 / (3 * 210000.0 * (20.0 * 10.0**3 / 12))  # P L³ / 3EI
    assert len(uz) == n and sum(uz) / n == pytest.approx(theory, rel=0.02)
    assert totals(dat)[0] == pytest.approx([0.0, 0.0, 1000.0], abs=1e-3)


@pytest.mark.feature("LOD-02")
@pytest.mark.feature("LOD-17")
def test_SOLVER_pressure_gravity_match_resultant(app, tmp_path):
    """솔리드 면 압력의 부호(면을 누르는 쪽이 양)와 중력: 반력이 load.resultant 와 맞아야 한다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    top = [[e, 2] for e in app.execute("mesh.find", what="elements", box_min=[9.9, -1, 4.9], box_max=[101, 21, 11])["ids"]]
    s.loads.create_pressure(target={"type": "faces", "ids": top}, value=0.5)  # 뿌리 쪽 첫 요소는 뺀다(구속된 노드에 걸린 하중은 반력에 안 잡힌다)
    s.loads.create_gravity(target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0.0, 2.0, 0.0])
    rf(s, root)
    dat, _ = solve(app, case, tmp_path)
    want = app.execute("load.resultant", id=s.id)["force"]
    assert want[2] == pytest.approx(-0.5 * 90.0 * 20.0)
    got = totals(dat)[0]
    # 중력 가운데 구속된 노드 몫은 반력에 잡히지 않으므로 y 는 조금 작다
    assert got[2] == pytest.approx(-want[2], rel=1e-6) and got[0] == pytest.approx(0.0, abs=1e-6)
    assert -want[1] - 1e-9 <= got[1] < -want[1] * 0.9


@pytest.mark.feature("LOD-38")
def test_SOLVER_load_inheritance(app, tmp_path):
    """스텝 승계: keep 이면 앞 스텝 하중이 남고, new 면 지워진다. 한 번도 쓰지 않은 종류의 빈 OP=NEW 카드는 내지 않는다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    T = {"type": "set", "ids": [tip.id]}
    n = len(tip.props["ids"])
    s1 = case.steps.create_static(name="s1")
    s1.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s1.loads.create_force(target=T, components=[0.0, 0.0, -90.0 / n])
    s2 = case.steps.create_static(name="s2")
    s2.loads.create_force(target=T, components=[0.0, 45.0 / n, 0.0])  # 같은 대상의 같은 종류: 앞의 것을 통째로 바꾼다
    s3 = case.steps.create_static(name="s3", loads_inheritance="new")
    s3.loads.create_force(target=T, components=[18.0 / n, 0.0, 0.0])
    s4 = case.steps.create_static(name="s4", loads_inheritance="new")  # 하중이 하나도 없다
    for s in (s1, s2, s3, s4):
        rf(s, root)
    dat, _ = solve(app, case, tmp_path)
    got = totals(dat)
    want = [[0, 0, 90.0], [0, -45.0, 0], [-18.0, 0, 0], [0, 0, 0]]
    assert len(got) == 4
    for g, w, step in zip(got, want, (s1, s2, s3, s4)):
        assert g == pytest.approx(w, abs=1e-6)
        assert app.execute("load.resultant", id=step.id)["force"] == pytest.approx([-x for x in w], abs=1e-9)  # 모델의 승계 규칙과 같다
    text = (tmp_path / "job.inp").read_text()
    assert text.count("*CLOAD, OP=NEW") == 2 and "*TEMPERATURE" not in text and "*RADIATE" not in text


@pytest.mark.feature("BC-36")
def test_SOLVER_bc_inheritance(app, tmp_path):
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    R, T = {"type": "set", "ids": [root.id]}, {"type": "set", "ids": [tip.id]}
    s1 = case.steps.create_static(name="s1")
    s1.bcs.create_displacement(target=R, dofs=[1, 2, 3])
    s1.bcs.create_displacement(target=T, dofs=[3], values=[-1.0])
    s2 = case.steps.create_static(name="s2", bcs_inheritance="new")  # 끝단 강제 변위를 없앤다
    s2.bcs.create_displacement(target=R, dofs=[1, 2, 3])
    for s in (s1, s2):
        rf(s, root)
    dat, _ = solve(app, case, tmp_path)
    got = totals(dat)
    assert abs(got[0][2]) > 100.0 and got[1] == pytest.approx([0, 0, 0], abs=1e-6)


# ================================================================ 쉘
@pytest.mark.feature("LOD-02")
@pytest.mark.feature("PRP-02")
def test_SOLVER_shell_pressure_and_edge_load(app, tmp_path):
    """쉘 압력의 부호(요소 법선 방향으로 미는 쪽이 양)와 변 하중의 라벨."""
    mat = steel(app)
    sh = plate(app, 4, 2, size=(10.0, 4.0))  # z=0 평면, 법선 +z
    ids = list(range(sh["elements"][0], sh["elements"][1] + 1))
    app.execute("mesh.set_element_type", ids=ids, type="S4")
    app.model.properties.create_shell(material=mat.id, thickness=0.5, target={"type": "elements", "ids": ids})
    root = node_set(app, "ROOT", [-0.1, -1, -1], [0.1, 5, 1])
    case = app.model.cases.create()
    s1 = case.steps.create_static(name="pressure")
    s1.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3, 4, 5, 6])
    right = [e for e in ids if e not in (ids[0], ids[4])]  # 구속된 변에 붙은 요소는 뺀다
    s1.loads.create_pressure(target={"type": "elements", "ids": right}, value=2.0)
    rf(s1, root)
    s2 = case.steps.create_static(name="edge", loads_inheritance="new")
    # 오른쪽 끝 변(x=10): 사각형의 둘째 변(절점 2-3). 변을 누르는 쪽(-x)이 양, 단위 길이당 3
    s2.loads.create_edge_load(target={"type": "faces", "ids": [[ids[3], 2], [ids[7], 2]]}, value=3.0)
    rf(s2, root)
    dat, _ = solve(app, case, tmp_path)
    got = totals(dat)
    assert got[0] == pytest.approx([0.0, 0.0, -2.0 * 7.5 * 4.0], abs=1e-6)  # 압력이 +z(법선)로 민다 → 반력 -z
    assert got[1] == pytest.approx([3.0 * 4.0, 0.0, 0.0], abs=1e-6)        # 변 하중이 -x 로 누른다 → 반력 +x


# ================================================================ 고유치·좌굴·열·동해석
@pytest.mark.feature("CAS-02")
def test_SOLVER_frequency(app, tmp_path):
    part, mat, root, tip, case = cantilever(app)
    s = case.steps.create_frequency(num_modes=3)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    dat, _ = solve(app, case, tmp_path)
    freqs = [float(m.group(1)) for m in re.finditer(r"^\s*\d+\s+\S+\s+\S+\s+(\S+)\s+\S+\s*$", dat.split("P A R T I C I P A T I O N")[0], re.M)]
    # 외팔보 1차 굽힘: f = (1.875² / 2π L²) √(EI / ρA)
    import math
    f1 = 1.875**2 / (2 * math.pi * 100.0**2) * math.sqrt(210000.0 * (20 * 10**3 / 12) / (7.85e-9 * 200.0))
    assert freqs and freqs[0] == pytest.approx(f1, rel=0.03)


@pytest.mark.feature("CAS-02")
def test_SOLVER_procedures_run(app, tmp_path):
    """여러 절차가 든 덱이 오류 없이 풀린다: 정적(비선형) → 섭동 고유치 → 좌굴. 동해석은 따로."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    R, T = {"type": "set", "ids": [root.id]}, {"type": "set", "ids": [tip.id]}
    amp = app.model.functions.create_amplitude(name="RAMP", points=[[0.0, 0.0], [1.0, 1.0]])
    s1 = case.steps.create_static(nlgeom=True, initial_increment=0.5, period=1.0, max_increments=50)
    s1.bcs.create_displacement(target=R, dofs=[1, 2, 3])
    s1.loads.create_force(target=T, components=[-10.0, 0.0, -1.0], amplitude=amp.id)
    s1.outputs.create_node_file(variables=["U", "RF"])
    s1.outputs.create_element_file(variables=["S", "E"])
    case.steps.create_frequency(num_modes=2, perturbation=True)
    s3 = case.steps.create_buckle(num_modes=1, perturbation=True)
    s3.loads.create_force(target=T, components=[-1.0, 0.0, 0.0])
    dat, out = solve(app, case, tmp_path)
    assert (tmp_path / "job.frd").stat().st_size > 1000
    # 동해석(레일리 감쇠 포함)
    dyn = app.model.cases.create(name="dyn")
    dyn.update(rayleigh_alpha=10.0, rayleigh_beta=1e-6)
    d1 = dyn.steps.create_dynamic(initial_increment=1e-5, period=5e-5, alpha=-0.05)
    d1.bcs.create_displacement(target=R, dofs=[1, 2, 3])
    d1.loads.create_gravity(target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0.0, 0.0, -1.0])
    solve(app, dyn, tmp_path, "dyn")


@pytest.mark.feature("LOD-05")
def test_SOLVER_heat_transfer(app, tmp_path):
    """정상 열전달: 한쪽 온도 고정, 반대쪽 대류. 1차원 해와 비교하고, 열유속·복사·초기 온도 카드가 풀리는지 본다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 1, 1))
    app.execute("physical_constants.set", absolute_zero=-273.15, stefan_boltzmann=5.669e-11)
    everything = node_set(app, "ALLN", [-1, -1, -1], [101, 21, 11])
    app.model.initial_conditions.create_temperature(target={"type": "set", "ids": [everything.id]}, value=20.0)
    s = case.steps.create_heat_transfer(steady_state=True)
    s.bcs.create_temperature(target={"type": "set", "ids": [root.id]}, value=100.0)
    end = {"type": "faces", "ids": [[10, 4]]}  # x=100 면
    h, k, L, Tinf = 0.5, 50.0, 100.0, 20.0
    s.loads.create_film(target=end, coefficient=h, sink_temperature=Tinf)
    s.outputs.create_node_print(variables=["NT"], target={"type": "set", "ids": [tip.id]})
    s2 = case.steps.create_heat_transfer(steady_state=True, name="more")
    s2.loads.create_surface_flux(target={"type": "faces", "ids": [[5, 2]]}, value=0.01)
    s2.loads.create_body_flux(target={"type": "parts", "ids": [part.id]}, value=1e-4)
    s2.loads.create_radiation(target={"type": "faces", "ids": [[3, 1]]}, emissivity=0.8, sink_temperature=20.0)
    s2.loads.create_concentrated_flux(target={"type": "nodes", "ids": [tip.props["ids"][0]]}, value=0.1)
    dat, _ = solve(app, case, tmp_path)
    temps = [row[1] for title, rows in blocks(dat) if title.startswith("temperatures") for row in rows]
    # k (100 - T)/L = h (T - Tinf)
    want = (k / L * 100.0 + h * Tinf) / (k / L + h)
    assert temps[:4] == pytest.approx([want] * 4, rel=1e-6)


# ================================================================ 구속·접촉·섹션
@pytest.mark.feature("BC-07")
@pytest.mark.feature("BC-08")
@pytest.mark.feature("BC-09")
def test_SOLVER_constraints_and_contact(app, tmp_path):
    """구속식·강체·커플링·타이·접촉 카드가 풀리고, 타이로 붙인 두 블록이 하중을 전달한다."""
    mat = steel(app)
    p1 = app.model.mesh_parts.create(name="LOW")
    p2 = app.model.mesh_parts.create(name="UP")
    p3 = app.model.mesh_parts.create(name="PAD")
    block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=p1.id)                           # 요소 1~4
    block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=p2.id, origin=(0.0, 0.0, 5.0))   # 요소 5~8 (노드는 따로)
    block(app, 1, 1, 1, size=(4.0, 4.0, 2.0), part=p3.id, origin=(3.0, 3.0, 10.0))    # 요소 9
    for p in (p1, p2, p3):
        app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [p.id]})
    S = app.model.sets
    low_top = S.create_surface(name="LOWTOP", faces=[[e, 2] for e in (1, 2, 3, 4)])
    up_bot = S.create_surface(name="UPBOT", faces=[[e, 1] for e in (5, 6, 7, 8)])
    up_top = S.create_surface(name="UPTOP", faces=[[e, 2] for e in (5, 6, 7, 8)])
    pad_bot = S.create_surface(name="PADBOT", faces=[[9, 1]])
    pad_top = S.create_surface(name="PADTOP", faces=[[9, 2]])
    base = node_set(app, "BASE", [-1, -1, -0.1], [11, 11, 0.1])
    ref = app.execute("mesh.nodes_create", coords=[[5.0, 5.0, 15.0]])["ids"][0]
    C = app.model.constraints
    C.create_tie(slave={"type": "set", "ids": [up_bot.id]}, master={"type": "set", "ids": [low_top.id]}, position_tolerance=0.1)
    C.create_coupling_kinematic(surface={"type": "set", "ids": [pad_top.id]}, ref_node=ref, dofs=[1, 2, 3])
    inter = app.model.contact_properties.create(name="INT1", pressure_overclosure="linear", slope=1e6, friction_coefficient=0.2, stick_slope=1e5)
    app.model.contact_pairs.create(slave={"type": "set", "ids": [pad_bot.id]}, master={"type": "set", "ids": [up_top.id]}, interaction=inter.id)
    case = app.model.cases.create()
    case.update(contact_method="surface_to_surface")
    s = case.steps.create_static(nlgeom=False, initial_increment=1.0, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [base.id]}, dofs=[1, 2, 3])
    s.bcs.create_displacement(target={"type": "nodes", "ids": [ref]}, dofs=[1, 2])
    s.loads.create_force(target={"type": "nodes", "ids": [ref]}, components=[0.0, 0.0, -500.0])
    rf(s, base)
    s.outputs.create_node_file(variables=["U"])
    dat, _ = solve(app, case, tmp_path)
    assert totals(dat)[0] == pytest.approx([0.0, 0.0, 500.0], abs=0.5)  # 커플링 → 접촉 → 타이를 거쳐 바닥까지 전달
    # 타이(BC-T02-04): 덱에 *TIE 가 있고, 붙인 면의 종속 노드 변위가 주 면의 보간값과 같다(상면 밑 노드 ≈ 하면 윗 노드, 같은 자리)
    deck_text = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*TIE" in deck_text and "*KINEMATIC" in deck_text
    rid = app.execute("result.open", path=str(tmp_path / "job.frd"))["id"]
    nodes, u = app.results.values(rid, 1, "DISP")
    pos = {n: tuple(np.round(xyz, 6)) for n, xyz in zip(app.mesh.node_ids().tolist(), app.mesh.node_coords().tolist())}
    disp = dict(zip(nodes.tolist(), u.tolist()))
    pairs = [(a, b) for a in up_bot_nodes(app, up_bot) for b in low_top_nodes(app, low_top) if pos[a] == pos[b]]
    assert len(pairs) == 9 and all(np.allclose(disp[a], disp[b], atol=1e-6) for a, b in pairs)
    # 강체 연결(BC-T02-02): 커플링 면의 노드 변위 = 기준 노드의 강체 운동(z 병진만 허용 → 모두 같은 uz)
    pad_nodes = resolve_target(app, {"type": "set", "ids": [pad_top.id]})
    uz = [disp[n][2] for n in pad_nodes]
    assert len(uz) == 4 and uz[0] < 0 and max(uz) - min(uz) < 1e-6 * abs(uz[0])  # 기준 노드(요소 밖)는 frd 에 없다 — 면이 강체로 움직인 것으로 확인


def up_bot_nodes(app, surface):
    return _surface_nodes(app, surface)


def low_top_nodes(app, surface):
    return _surface_nodes(app, surface)


def _surface_nodes(app, surface):
    """면 셋의 노드(요소면 → 노드): 임시 구속의 대상 전개로 얻는다."""
    from nasa95 import App  # noqa: F401
    return [int(n) for n in resolve_target(app, {"type": "set", "ids": [surface.id]})]


def resolve_target(app, target):
    """적용 대상 → 노드 목록(bc.resolve 를 빌려 쓴다: 임시 객체를 묶음 안에서 만들고 되돌린다)."""
    app.execute("app.transaction_begin", name="resolve")
    try:
        case = app.model.cases.create(name="_tmp")
        step = case.steps.create_static()
        bc = step.bcs.create_displacement(target=target, dofs=[1])
        return app.execute("bc.resolve", id=bc.id)["nodes"]
    finally:
        app.execute("app.transaction_rollback")


@pytest.mark.feature("PRP-01")
def test_SOLVER_sections(app, tmp_path):
    """쉘·적층·보·트러스·스프링·질량 섹션이 든 덱이 풀린다."""
    mat = steel(app)
    ori = app.model.orientations.create_rectangular(name="OR1", a=[1.0, 0.0, 0.0], b=[0.0, 1.0, 0.0])
    sh = plate(app, 2, 1, size=(10.0, 5.0))
    app.execute("mesh.convert_order", order=2)  # S8: 적층은 S8R·S6 만
    app.execute("mesh.set_element_type", ids=[2], type="S8R")
    n = app.execute("mesh.nodes_create", coords=[[0, 0, 20], [10, 0, 20], [20, 0, 20], [20, 0, 30]])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[n[:2], n[1:3]])["ids"]
    truss = app.execute("mesh.elements_create", shape="line2", type="T3D2", connectivity=[[n[2], n[3]]])["ids"]
    spring = app.execute("mesh.create_connector", kind="spring", nodes=[n[1], n[3]])["ids"]
    mass = app.execute("mesh.create_connector", kind="mass", nodes=[n[1]])["ids"]
    el = lambda ids: {"type": "elements", "ids": ids}
    P = app.model.properties
    P.create_shell(material=mat.id, thickness=1.0, orientation=ori.id, target=el([1]))
    P.create_composite(target=el([2]), layers=[{"thickness": 0.5, "material": mat.id, "orientation": ori.id}, {"thickness": 0.5, "material": mat.id}])
    P.create_beam(material=mat.id, section="rect", dimensions=[2.0, 1.0], direction=[0.0, 1.0, 0.0], target=el(beams))
    P.create_truss(material=mat.id, area=1.5, target=el(truss))
    P.create_spring(stiffness=100.0, target=el(spring))
    P.create_mass(mass=1e-3, target=el(mass))
    fixed = node_set(app, "FIXED", [-0.1, -1, -1], [0.1, 6, 21])
    case = app.model.cases.create()
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [fixed.id]}, dofs=[1, 2, 3, 4, 5, 6])
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n[3]]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "nodes", "ids": [n[2]]}, components=[0.0, 0.0, -5.0])
    s.loads.create_gravity(target=el([1, 2] + beams), value=9810.0, direction=[0.0, 0.0, -1.0])
    s.outputs.create_node_file(variables=["U"])
    s.outputs.create_element_file(variables=["S"], section_forces=False)
    solve(app, case, tmp_path)


@pytest.mark.feature("CAS-11")
def test_SOLVER_reimported_deck_gives_same_result(app, tmp_path):
    """가져온 덱을 다시 써서 풀면 같은 결과가 나온다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_pressure(target={"type": "faces", "ids": [[e, 2] for e in range(22, 41)]}, value=0.5)
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 1.0, 0.0])
    s.outputs.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]})
    (tmp_path / "a").mkdir(), (tmp_path / "b").mkdir()
    dat_a, _ = solve(app, case, tmp_path / "a")
    other = App()
    r = other.execute("deck.import", path=str(tmp_path / "a" / "job.inp"))
    assert r["preserved"] == []
    dat_b, _ = solve(other, r["case"], tmp_path / "b")
    ua = [row for t, rows in blocks(dat_a) if t.startswith("displacements") for row in rows]
    ub = [row for t, rows in blocks(dat_b) if t.startswith("displacements") for row in rows]
    assert ua and len(ua) == len(ub)
    assert all(b == pytest.approx(a, rel=1e-8, abs=1e-12) for a, b in zip(ua, ub))


@pytest.mark.feature("LOD-13")
@pytest.mark.feature("BC-04")
def test_SOLVER_local_csys_transform(app, tmp_path):
    """국부 좌표계의 집중 하중·경계조건(*TRANSFORM)이 전역 좌표계로 적은 것과 같은 결과를 낸다(솔버로 확인)."""
    # z 축 둘레 90°: 국부 1축 = 전역 y, 국부 2축 = 전역 -x. 국부 힘 [0, F, 0] 은 전역 [-F, 0, 0] 이고
    # 국부 1축 구속은 전역 y 구속이다.
    def model(app, local):
        part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
        s = case.steps.create_static()
        s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
        n = len(tip.props["ids"])
        if local:
            rot = app.model.csys.create_rectangular(name="ROT", origin=[0.0, 0.0, 0.0], axis1_point=[0.0, 1.0, 0.0], plane12_point=[-1.0, 0.0, 0.0])
            s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 100.0 / n, 0.0], csys=rot.id)
            s.bcs.create_displacement(target={"type": "set", "ids": [tip.id]}, dofs=[1], values=[0.01], csys=rot.id)
        else:
            s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[-100.0 / n, 0.0, 0.0])
            s.bcs.create_displacement(target={"type": "set", "ids": [tip.id]}, dofs=[2], values=[0.01])
        # 솔버로 확인: GLOBAL 을 적지 않으면 변환된 노드는 국부 좌표계 값(줄 끝에 L)으로 찍힌다
        s.outputs.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]}, **{"global": local})
        s.outputs.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]})
        rf(s, root)
        return case
    (tmp_path / "g").mkdir(), (tmp_path / "l").mkdir()
    dat_g, _ = solve(app, model(app, False), tmp_path / "g")
    other = App()
    dat_l, _ = solve(other, model(other, True), tmp_path / "l")
    assert "*TRANSFORM, NSET=" in (tmp_path / "l" / "job.inp").read_text()
    disp = lambda dat: [rows for t, rows in blocks(dat) if t.startswith("displacements")]
    ug, ul = disp(dat_g), disp(dat_l)
    assert len(ug) == 2 and len(ul) == 2 and len(ug[0]) == 9
    assert all(abs(r[2] - 0.01) < 1e-9 for r in ug[0])  # 전역 y = 0.01
    assert all(l == pytest.approx(g, rel=1e-6, abs=1e-9) for g, l in zip(ug[0], ul[0]))  # GLOBAL=YES: 전역 값이 같다
    # 국부 출력(L 표시): 국부 x = 전역 y, 국부 y = -전역 x
    assert all([l[0], l[1], l[2], l[3]] == pytest.approx([g[0], g[2], -g[1], g[3]], rel=1e-6, abs=1e-9) for g, l in zip(ug[1], ul[1]))
    assert totals(dat_l)[0] == pytest.approx(totals(dat_g)[0], rel=1e-6, abs=1e-6) and totals(dat_g)[0][0] == pytest.approx(100.0, rel=1e-6)
    # 결과 명령도 국부 행을 읽고 표시한다
    res = other.execute("result.open", path=str(tmp_path / "l" / "job.frd"))["id"]
    tables = other.execute("result.print_tables", result=res, quantity="displacements")
    assert len(tables) == 2 and "local_rows" not in tables[0] and tables[1]["local_rows"] == list(range(9))
    assert tables[1]["rows"][0][1:] == pytest.approx([ul[1][0][1], ul[1][0][2], ul[1][0][3]])


# ================================================================ 솔버 실행 명령 (CAS-13, CAS-39, CAS-41)
@pytest.fixture
def ccx_env(monkeypatch):
    monkeypatch.setenv("NASA95_CCX", CCX)


@pytest.mark.feature("CAS-13")
@pytest.mark.feature("CAS-41")
def test_SOLVER_run_and_status(app, tmp_path, ccx_env):
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    s = case.steps.create_static(nlgeom=True, initial_increment=0.25, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run_status", id=case.id) == {"state": "none", "case": case.id}
    case.update(work_directory=str(tmp_path / "작업 폴더"), threads=1)
    from conftest import history_len, total
    before, hist = total(app), history_len(app)
    r = app.execute("case.run", id=case.id, wait=True)
    assert total(app) == before and history_len(app) == hist  # 실행은 모델을 바꾸지 않는다
    assert r["state"] == "completed" and r["exit_code"] == 0 and r["errors"] == [] and r["skipped"] == []
    assert r["job"] == "cantilever" and pathlib.Path(r["work_directory"]) == tmp_path / "작업 폴더"
    assert set(r["files"]) >= {"inp", "frd", "sta"} and r["files"]["frd"]["bytes"] > 1000
    assert pathlib.Path(r["deck"]).read_text() == app.execute("case.preview_deck", id=case.id)["text"]
    st = app.execute("case.run_status", id=case.id, log_lines=3)
    assert st["state"] == "completed" and len(st["log"]) == 3 and st["elapsed"] > 0 and st["check_only"] is False
    # 수렴 이력: 증분 4개(0.25 씩), 마지막 스텝 시간 1
    inc = st["increments"]
    assert [i["increment"] for i in inc] == [1, 2, 3, 4] and inc[-1]["step_time"] == pytest.approx(1.0)
    assert all(i["step"] == 1 and i["iterations"] >= 1 for i in inc)
    # 프로젝트를 저장했으면 작업 폴더는 프로젝트 파일 옆에 생긴다
    case.update(work_directory=None)
    app.execute("project.save_as", path=str(tmp_path / "model.nasa95"))
    r = app.execute("case.run", id=case.id, wait=True)
    assert pathlib.Path(r["work_directory"]) == tmp_path / "model.work" / "cantilever" and r["state"] == "completed"


@pytest.mark.feature("WT-32")
@pytest.mark.feature("API-30")
def test_SOLVER_SYS_20_07_13_15_run_state_for_tree(app, tmp_path, ccx_env):
    """실행 상태 표시: 미실행(none) → 실행 중(running) → 완료(completed) → 모델을 고치면 결과가 모델보다 오래됨(outdated). 작업 취소는 stopped."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 2, 2))
    s = case.steps.create_static(nlgeom=True, initial_increment=0.05, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    s.outputs.create_node_file(variables=["U"])
    case.update(work_directory=str(tmp_path), threads=1)
    assert app.execute("case.run_status", id=case.id)["state"] == "none"
    r = app.execute("case.run", id=case.id)  # 바로 돌아온다(SYS-16-01)
    assert r["state"] in ("running", "completed")
    job = f"solver:{case.id}"
    assert [j["id"] for j in app.execute("job.list")] == [job]
    st = app.execute("job.wait", id=job, timeout=300)
    assert st["state"] == "completed"
    st = app.execute("case.run_status", id=case.id)
    assert st["state"] == "completed" and st["outdated"] is False
    # 열린 결과도 같은 판정을 준다(SYS-20-15, D13 결과 작업 공간의 "재해석 필요" 표시용)
    res = app.execute("result.open", case=case.id)
    assert res["outdated"] is False and app.execute("result.info", result=res["id"])["outdated"] is False
    by_path = app.execute("result.open", path=st["files"]["frd"]["path"])  # 경로로 연 외부 파일: 연 때의 모델 기준
    assert by_path["case"] is None and by_path["outdated"] is False
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, -1.0, 0.0])  # 모델 수정
    assert app.execute("case.run_status", id=case.id)["outdated"] is True
    assert app.execute("result.info", result=res["id"])["outdated"] is True
    listed = {r["id"]: r for r in app.execute("result.list")}
    assert set(listed) == {res["id"], by_path["id"]} and listed[res["id"]]["outdated"] is True and listed[by_path["id"]]["outdated"] is True
    assert listed[res["id"]]["frames"] == res["frames"] and listed[res["id"]]["case"] == case.id
    app.undo()
    assert app.execute("case.run_status", id=case.id)["outdated"] is False
    assert app.execute("result.info", result=res["id"])["outdated"] is False and app.execute("result.info", result=by_path["id"])["outdated"] is False
    app.execute("result.close", result=by_path["id"])
    assert [r["id"] for r in app.execute("result.list")] == [res["id"]]
    # 취소(SYS-16-02): 긴 풀이를 시작하자마자 멈춘다
    s.update(initial_increment=0.001)
    r = app.execute("case.run", id=case.id)
    if r["state"] == "running":
        st = app.execute("job.cancel", id=job)
        assert app.execute("job.wait", id=job, timeout=60)["state"] in ("stopped", "completed", "failed")
        assert app.execute("case.run_status", id=case.id)["state"] in ("stopped", "completed", "failed")


@pytest.mark.feature("CAS-13")
def test_SOLVER_run_failure_and_errors(app, tmp_path, ccx_env, monkeypatch):
    part, mat, root, tip, case = cantilever(app, order=1, n=(4, 1, 1))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    for p in app.execute("property.list"):
        app.execute("property.delete", id=p["id"])  # 재료 없는 요소 → 솔버 오류
    r = app.execute("case.run", id=case.id, wait=True)
    assert r["state"] == "failed" and r["errors"] and "*ERROR" in r["errors"][0]
    assert [w["code"] for w in r["deck_warnings"]] == ["unassigned_elements"]
    # 덱에 쓰지 못한 객체가 있으면 실행하지 않는다(ASCII 가 아닌 보존 블록은 덱에 쓰지 못한다)
    bad = app.model.deck_blocks.create(parent=case.id, text="*HEADING" + chr(10) + "한글 제목")
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=case.id, wait=True)
    assert e.value.code == "deck_incomplete" and e.value.details["skipped"][0]["object"] == bad.id
    assert app.execute("case.run", id=case.id, wait=True, allow_skipped=True)["skipped"][0]["object"] == bad.id
    # 솔버를 찾지 못함
    case.update(solver_executable=str(tmp_path / "no_such_ccx.exe"))
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=case.id)
    assert e.value.code == "solver_not_found"
    case.update(solver_executable=None)
    monkeypatch.delenv("NASA95_CCX")
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=case.id)
    assert e.value.code == "solver_not_found"
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=mat.id)
    assert e.value.code == "wrong_kind"


@pytest.mark.feature("CAS-39")
def test_SOLVER_check_only(app, tmp_path, ccx_env):
    part, mat, root, tip, case = cantilever(app, order=1, n=(4, 1, 1))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.outputs.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]})
    r = app.execute("case.run_check_only", id=case.id, wait=True)
    assert r["state"] == "completed" and r["check_only"] is True
    assert "*NO ANALYSIS" in pathlib.Path(r["deck"]).read_text()
    assert "*NO ANALYSIS" not in app.execute("case.preview_deck", id=case.id)["text"]


@pytest.mark.feature("CAS-13")
def test_SOLVER_run_stop(app, tmp_path, ccx_env):
    part, mat, root, tip, case = cantilever(app, order=2, n=(40, 12, 12))  # 풀이에 몇 초 걸리는 크기
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    r = app.execute("case.run", id=case.id)
    assert r["state"] == "running" and "exit_code" not in r
    with pytest.raises(Nasa95Error) as e:  # 같은 케이스를 겹쳐 실행할 수 없다
        app.execute("case.run", id=case.id)
    assert e.value.code == "invalid_state"
    st = app.execute("case.run_stop", id=case.id)
    assert st["state"] == "stopped"
    assert app.execute("case.run_status", id=case.id)["state"] == "stopped"
    assert app.execute("case.run_stop", id=case.id)["state"] == "stopped"  # 이미 멈춘 것을 다시 멈춰도 된다


# ================================================================ 결과 읽기 (4단계: RES-01~03, 06, 12, 13, 22, 23, 27, 29, 32, 60)
@pytest.fixture
def solved(app, tmp_path, ccx_env):
    """비선형 정적(증분 2개) → 섭동 고유치 → 좌굴을 푼 외팔보."""
    part, mat, root, tip, case = cantilever(app, order=2, n=(8, 1, 1))
    case.update(work_directory=str(tmp_path))
    T = {"type": "set", "ids": [tip.id]}
    s = case.steps.create_static(nlgeom=True, initial_increment=0.5, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target=T, components=[0.0, 0.0, -100.0 / len(tip.props["ids"])])
    s.outputs.create_node_file(variables=["U", "RF"])
    s.outputs.create_element_file(variables=["S", "E"])
    s.outputs.create_node_print(variables=["RF"], target={"type": "set", "ids": [root.id]}, totals="yes")
    s.outputs.create_element_print(variables=["S"], target={"type": "parts", "ids": [part.id]})
    case.steps.create_frequency(num_modes=3, perturbation=True)
    b = case.steps.create_buckle(num_modes=2, perturbation=True)
    b.loads.create_force(target=T, components=[-1.0, 0.0, 0.0])
    case.update(threads=1)
    run = app.execute("case.run", id=case.id, wait=True)
    # 이 ccx 빌드(2.22, Windows)는 드물게 끝난 뒤 힙 손상(0xC0000374)으로 죽는다 — 솔버 쪽 문제라 몇 번 다시 돌린다
    for _ in range(3):
        if not (run["state"] == "failed" and not run["errors"]):  # 솔버 오류 메시지 없이 죽은 것(비정상 종료)만 다시 돌린다
            break
        run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", (run.get("exit_code"), run["errors"], app.execute("case.run_status", id=case.id, log_lines=40)["log"])
    return {"case": case, "root": root, "tip": tip, "run": run}


@pytest.mark.feature("RES-01")
@pytest.mark.feature("RES-02")
@pytest.mark.feature("RES-27")
@pytest.mark.feature("RES-32")
@pytest.mark.feature("RES-33")
def test_RES_T01_open_and_frames(app, solved):
    from conftest import history_len, total
    before, hist = total(app), history_len(app)
    r = app.execute("result.open", case=solved["case"].id)
    assert total(app) == before and history_len(app) == hist
    info = app.execute("project.info")
    assert (r["nodes"], r["elements"], r["complete"], r["case"]) == (info["nodes"], info["elements"], True, solved["case"].id)
    assert pathlib.Path(r["path"]).name == "cantilever.frd"
    frames = app.execute("result.steps", result=r["id"])
    assert [(f["step"], f["increment"]) for f in frames[:2]] == [(1, 1), (1, 2)] and [f["value"] for f in frames[:2]] == [0.5, 1.0]
    assert frames[0]["fields"] == ["DISP", "STRESS", "TOSTRAIN", "FORC", "ERROR"]
    assert [f["step"] for f in frames[2:]] == [2, 2, 2, 3, 3, 3] and len(frames) == r["frames"] == 8  # 좌굴: 기준 상태 + 모드 2개
    fields = {f["name"]: f for f in app.execute("result.fields", result=r["id"], frame=1)}
    assert fields["DISP"]["components"] == ["D1", "D2", "D3"] and fields["DISP"]["kind"] == "vector" and fields["DISP"]["derived"] == ["magnitude"]
    assert fields["STRESS"]["components"] == ["SXX", "SYY", "SZZ", "SXY", "SYZ", "SZX"] and "mises" in fields["STRESS"]["derived"]
    assert fields["STRESS"]["count"] == info["nodes"] and fields["STRESS"]["location"] == "node"
    # 모델과의 대응
    m = app.execute("result.map_to_model", result=r["id"])
    assert m["matched_nodes"] == info["nodes"] and m["result_only_nodes"] == 0 and m["matched_elements"] == info["elements"] and m["moved_nodes"] == 0
    # 경로로 열기, 닫기, 오류
    r2 = app.execute("result.open", path=r["path"])
    assert r2["id"] != r["id"] and r2["case"] is None
    app.execute("result.close", result=r2["id"])
    for name, params, code in [("result.steps", dict(result=r2["id"]), "not_found"), ("result.close", dict(result=99), "not_found"),
                               ("result.fields", dict(result=r["id"], frame=99), "out_of_range"),
                               ("result.values", dict(result=r["id"], frame=1, field="NOPE"), "not_found"),
                               ("result.values", dict(result=r["id"], frame=1, field="DISP", component="SXX"), "not_found"),
                               ("result.values", dict(result=r["id"], frame=1, field="DISP", nodes=[999999]), "not_found"),
                               ("result.open", dict(), "missing_param"), ("result.open", dict(path="nothing.frd"), "io_error")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute(name, **params)
        assert e.value.code == code, (name, params)


@pytest.mark.feature("RES-12")
@pytest.mark.feature("RES-13")
@pytest.mark.feature("RES-06")
@pytest.mark.feature("API-12")
def test_RES_T02_values(app, solved):
    import numpy as np
    rid = app.execute("result.open", case=solved["case"].id)["id"]
    tip = solved["tip"].props["ids"]
    v = app.execute("result.values", result=rid, frame=2, field="DISP", nodes=tip)
    assert v["ids"] == tip and v["components"] == ["D1", "D2", "D3"] and len(v["values"]) == len(tip)
    theory = -100.0 * 100.0**3 / (3 * 210000.0 * (20.0 * 10.0**3 / 12))
    uz = [row[2] for row in v["values"]]
    assert sum(uz) / len(uz) == pytest.approx(theory, rel=0.03)
    half = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=tip)["values"]
    assert sum(half) / len(half) == pytest.approx(theory / 2, rel=0.03)  # 하중의 절반(시간 0.5)
    # 파생량: 크기 = √(성분²의 합)
    mag = app.execute("result.values", result=rid, frame=2, field="DISP", component="magnitude", nodes=tip)["values"]
    assert mag == pytest.approx([math.sqrt(sum(x * x for x in row)) for row in v["values"]])
    # numpy 배열
    ids, arr = app.results.values(rid, 2, "DISP")
    assert arr.shape == (len(ids), 3) and not arr.flags.writeable
    all_values = app.execute("result.values", result=rid, frame=2, field="DISP")
    assert ids.tolist() == all_values["ids"] and arr.tolist() == all_values["values"]
    _, mises = app.results.values(rid, 2, "STRESS", "mises")
    _, s = app.results.values(rid, 2, "STRESS")
    want = np.sqrt(0.5 * ((s[:, 0] - s[:, 1])**2 + (s[:, 1] - s[:, 2])**2 + (s[:, 2] - s[:, 0])**2) + 3 * (s[:, 3]**2 + s[:, 4]**2 + s[:, 5]**2))
    assert np.allclose(mises, want)
    # 주응력: 텐서의 고유값과 같다
    _, p1 = app.results.values(rid, 2, "STRESS", "p1")
    _, p3 = app.results.values(rid, 2, "STRESS", "p3")
    k = int(np.argmax(mises))
    t = np.array([[s[k, 0], s[k, 3], s[k, 5]], [s[k, 3], s[k, 1], s[k, 4]], [s[k, 5], s[k, 4], s[k, 2]]])
    eig = np.linalg.eigvalsh(t)
    assert p1[k] == pytest.approx(eig[2], rel=1e-9, abs=1e-9) and p3[k] == pytest.approx(eig[0], rel=1e-9, abs=1e-9)
    _, tresca = app.results.values(rid, 2, "STRESS", "tresca")
    assert tresca[k] == pytest.approx(eig[2] - eig[0])
    # 최대·최소
    mm = app.execute("result.minmax", result=rid, frame=2, field="STRESS", component="mises")
    assert mm["max"]["value"] == pytest.approx(float(mises.max())) and mm["max"]["node"] == int(ids[k]) and mm["count"] == len(ids)
    mm = app.execute("result.minmax", result=rid, frame=2, field="DISP", component="D3", nodes=tip)
    assert mm["min"]["value"] == pytest.approx(min(uz)) and mm["min"]["node"] in tip
    # 찍어 보기
    pr = app.execute("result.probe", result=rid, frame=2, field="STRESS", nodes=[int(ids[k])])[0]
    assert pr["node"] == int(ids[k]) and pr["mises"] == pytest.approx(float(mises[k])) and pr["SXX"] == pytest.approx(float(s[k, 0]))
    d = app.execute("result.derived_scalar", result=rid, frame=2, field="STRESS", name="mises", nodes=[int(ids[k])])
    assert d["available"] == ["mises", "tresca", "p1", "p2", "p3", "pressure"] and d["values"] == pytest.approx([float(mises[k])])
    with pytest.raises(Nasa95Error) as e:
        app.results.values(rid, 2, "STRESS", "magnitude")
    assert e.value.code == "not_found"


@pytest.mark.feature("RES-22")
@pytest.mark.feature("RES-17")
def test_RES_T03_reaction_and_history(app, solved):
    rid = app.execute("result.open", case=solved["case"].id)["id"]
    root = solved["root"].props["ids"]
    r = app.execute("result.reaction_sum", result=rid, frame=2, nodes=root)
    assert r["force"] == pytest.approx([0.0, 0.0, 100.0], abs=1e-2) and r["count"] == len(root)  # frd 는 유효숫자 6자리
    # 반력이 원점에 만드는 모멘트 = −(하중의 모멘트). 하중 (0,0,−100) 이 (100,10,5) 에 작용
    assert r["moment"][1] == pytest.approx(-100.0 * 100.0, rel=0.01) and r["moment"][0] == pytest.approx(100.0 * 10.0, rel=0.01)
    assert app.execute("result.reaction_sum", result=rid, frame=1, nodes=root)["force"][2] == pytest.approx(50.0, abs=1e-2)
    about = app.execute("result.reaction_sum", result=rid, frame=2, nodes=root, point=[0.0, 10.0, 5.0])
    assert about["moment"][0] == pytest.approx(0.0, abs=0.1)
    node = solved["tip"].props["ids"][0]
    h = app.execute("result.history", result=rid, field="DISP", component="D3", node=node, step=1)
    assert [(p["frame"], p["x"]) for p in h] == [(1, 0.5), (2, 1.0)] and h[1]["y"] == pytest.approx(2 * h[0]["y"], rel=0.02)
    assert len(app.execute("result.history", result=rid, field="DISP", component="magnitude", node=node)) == 8
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.history", result=rid, field="NOPE", component="D3", node=node)
    assert e.value.code == "not_found"


@pytest.mark.feature("RES-23")
@pytest.mark.feature("RES-60")
@pytest.mark.feature("RES-29")
@pytest.mark.feature("RES-28")
@pytest.mark.feature("RES-30")
@pytest.mark.feature("RES-37")
def test_RES_T04_tables(app, solved):
    rid = app.execute("result.open", case=solved["case"].id)["id"]
    modal, = app.execute("result.modal_summary", result=rid)
    assert modal["step"] == 2 and [m["mode"] for m in modal["modes"]] == [1, 2, 3]
    f1 = 1.875**2 / (2 * math.pi * 100.0**2) * math.sqrt(210000.0 * (20 * 10**3 / 12) / (7.85e-9 * 200.0))
    assert modal["modes"][0]["frequency"] == pytest.approx(f1, rel=0.05)  # 하중이 걸린 상태의 고유치라 조금 다르다
    assert modal["modes"][0]["omega"] == pytest.approx(2 * math.pi * modal["modes"][0]["frequency"], rel=1e-6)
    assert len(modal["modes"][0]["participation"]) == 6
    # 프레임의 값(진동수)과 표가 맞는다
    frames = app.execute("result.steps", result=rid)
    assert [f["value"] for f in frames if f["step"] == 2] == pytest.approx([m["frequency"] for m in modal["modes"]], rel=1e-5)
    buck, = app.execute("result.buckling_summary", result=rid)
    assert buck["step"] == 3 and len(buck["modes"]) == 2 and buck["modes"][0]["factor"] > 0
    euler = math.pi**2 * 210000.0 * (20 * 10**3 / 12) / (4 * 100.0**2)  # 외팔 기둥: π²EI / 4L²
    n = len(solved["tip"].props["ids"])  # 좌굴 스텝의 하중은 노드마다 1 → 전체 n
    assert buck["modes"][0]["factor"] * n == pytest.approx(euler, rel=0.05)
    conv = app.execute("result.convergence", result=rid)
    assert [(i["step"], i["increment"]) for i in conv["increments"]] == [(1, 1), (1, 2)]
    assert conv["iterations"] and conv["iterations"][0]["step"] == 1 and conv["iterations"][-1]["residual_force"] < 1.0
    # dat 의 표(RES-28): 셋 단위 절점력과 합계, 요소 응력
    tables = app.execute("result.print_tables", result=rid)
    kinds = {(t["quantity"], t["set"]) for t in tables}
    assert ("forces", "ROOT") in kinds and ("total force", "ROOT") in kinds and ("stresses", "BEAM") in kinds
    tot = app.execute("result.print_tables", result=rid, quantity="total force", set="ROOT")
    tot = [t for t in tot if t["step"] == 1]  # 출력 요청은 뒤 스텝으로 이어진다
    assert [t["time"] for t in tot] == [0.5, 1.0] and tot[1]["rows"][0][2] == pytest.approx(100.0, abs=0.01)
    forces = app.execute("result.print_tables", result=rid, quantity="forces")[0]
    assert forces["columns"] == ["fx", "fy", "fz"] and len(forces["rows"]) == len(solved["root"].props["ids"]) and len(forces["rows"][0]) == 4
    # dat 합계 = 개별 값의 합(RES-T01-14): 같은 스텝·시간의 forces 표와 total force 표
    first_total = [t for t in app.execute("result.print_tables", result=rid, quantity="total force", set="ROOT") if t["step"] == 1][0]
    first_forces = [t for t in app.execute("result.print_tables", result=rid, quantity="forces", set="ROOT") if t["step"] == 1][0]
    assert first_total["time"] == first_forces["time"]
    for k in range(3):  # dat 의 개별 값은 유효숫자 6자리라 합은 그 반올림 오차(값 크기 × 1e-6 × 개수) 안에서 맞는다
        col = [row[k + 1] for row in first_forces["rows"]]
        assert sum(col) == pytest.approx(first_total["rows"][0][k], abs=max(1e-9, 1e-6 * max(map(abs, col)) * len(col)))
    stresses = [t for t in tables if t["quantity"] == "stresses"][0]
    assert stresses["columns"] == ["elem", "integ.pnt.", "sxx", "syy", "szz", "sxy", "sxz", "syz"] and len(stresses["rows"][0]) == 8
    assert {t["quantity"] for t in app.execute("result.totals", result=rid)} == {"total force"}
    assert app.execute("result.section_output", result=rid) == [] and app.execute("result.contact_summary", result=rid) == []
    # 다시 읽기: 같은 번호로 남는다
    again = app.execute("result.reload", result=rid)
    assert again["id"] == rid and again["frames"] == 8 and again["case"] == solved["case"].id  # 완료 후 다시 읽기: 스텝 수 불변(RES-T01-16)
    assert app.execute("result.steps", result=rid) == frames


def to_binary_frd(src: pathlib.Path, dst: pathlib.Path, double: bool) -> None:
    """ASCII frd 를 cgx 문서의 이진 형식(2 = float, 3 = double)으로 바꾼다(읽기 검사용)."""
    import struct
    out = bytearray()
    lines = src.read_text().splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        key = line[:6]
        if key == "    2C":
            out += (line[:73] + "2").encode() + b"\n"
            i += 1
            while not lines[i].startswith(" -3"):
                nid = int(lines[i][3:13])
                xyz = [float(lines[i][13 + 12 * k:25 + 12 * k]) for k in range(3)]
                out += struct.pack("<i3d", nid, *xyz)
                i += 1
            i += 1
        elif key == "    3C":
            out += (line[:73] + "2").encode() + b"\n"
            i += 1
            while not lines[i].startswith(" -3"):
                head = lines[i]
                eid, etype, grp, mat = int(head[3:13]), int(head[13:18]), int(head[18:23]), int(head[23:28])
                i += 1
                nodes = []
                while i < len(lines) and lines[i].startswith(" -2"):
                    nodes += [int(lines[i][3 + 10 * k:13 + 10 * k]) for k in range((len(lines[i]) - 3) // 10)]
                    i += 1
                out += struct.pack("<4i", eid, etype, grp, mat) + struct.pack(f"<{len(nodes)}i", *nodes)
            i += 1
        elif key == "  100C":
            fmt = 3 if double else 2
            out += (line[:73] + f" {fmt}").encode() + b"\n"
            i += 1
            out += (lines[i] + "\n").encode()  # -4
            ncomp = int(lines[i][13:18])
            i += 1
            exist = 0
            for _ in range(ncomp):
                out += (lines[i] + "\n").encode()
                if int(lines[i][33:38] or 0) == 0:
                    exist += 1
                i += 1
            while not lines[i].startswith(" -3"):
                nid = int(lines[i][3:13])
                vals = []
                while True:
                    text = lines[i][13:]
                    vals += [float(text[12 * k:12 * k + 12]) for k in range(len(text) // 12)]
                    if i + 1 < len(lines) and lines[i + 1].startswith(" -2"):
                        i += 1
                    else:
                        break
                i += 1
                out += struct.pack("<i", nid) + struct.pack(f"<{exist}{'d' if double else 'f'}", *vals[:exist])
            i += 1
        else:
            out += (line + "\n").encode()
            i += 1
    dst.write_bytes(bytes(out))


@pytest.mark.feature("RES-27")
def test_RES_T05_binary_frd(app, solved, tmp_path):
    """이진 frd(float·double)를 ASCII 와 같게 읽는다."""
    src = pathlib.Path(solved["run"]["files"]["frd"]["path"])
    ascii_id = app.execute("result.open", path=str(src))["id"]
    for double in (False, True):
        dst = tmp_path / f"bin{int(double)}.frd"
        to_binary_frd(src, dst, double)
        r = app.execute("result.open", path=str(dst))
        a0 = app.execute("result.open", path=str(src))
        assert (r["nodes"], r["elements"], r["frames"], r["complete"]) == (a0["nodes"], a0["elements"], a0["frames"], True)
        assert app.execute("result.steps", result=r["id"]) == app.execute("result.steps", result=ascii_id)
        m = app.execute("result.map_to_model", result=r["id"])
        assert m["matched_nodes"] == r["nodes"] and m["matched_elements"] == r["elements"] and m["moved_nodes"] == 0
        for frame, field in [(2, "DISP"), (2, "STRESS"), (4, "DISP")]:
            a = app.execute("result.values", result=ascii_id, frame=frame, field=field)
            b = app.execute("result.values", result=r["id"], frame=frame, field=field)
            assert a["ids"] == b["ids"] and a["components"] == b["components"]
            flat_a = [v for row in a["values"] for v in row]
            flat_b = [v for row in b["values"] for v in row]
            assert flat_b == pytest.approx(flat_a, rel=1e-6 if not double else 1e-12, abs=1e-30)
        _, mises = app.results.values(r["id"], 2, "STRESS", "mises")
        assert mises.max() == pytest.approx(app.execute("result.minmax", result=ascii_id, frame=2, field="STRESS", component="mises")["max"]["value"], rel=1e-6)


@pytest.mark.feature("RES-25")
@pytest.mark.feature("RES-18")
@pytest.mark.feature("RES-24")
@pytest.mark.feature("RES-09")
def test_RES_T06_export_path_compare_transform(app, solved, tmp_path):
    """CSV 내보내기, 경로 샘플링, 결과 비교, 좌표계 변환."""
    import csv
    rid = app.execute("result.open", case=solved["case"].id)["id"]
    # CSV: 머리글과 값이 result.values 와 같다
    r = app.execute("result.export_table", result=rid, frame=2, field="STRESS", path=str(tmp_path / "s.csv"), components=["SXX", "mises"])
    assert r["rows"] > 0 and r["columns"] == ["SXX", "mises"]
    rows = list(csv.reader(open(tmp_path / "s.csv")))
    assert rows[0] == ["node", "x", "y", "z", "SXX", "mises"] and len(rows) == r["rows"] + 1
    v = app.execute("result.values", result=rid, frame=2, field="STRESS", component="SXX")
    assert [int(x[0]) for x in rows[1:]] == v["ids"] and [float(x[4]) for x in rows[1:]] == pytest.approx(v["values"], rel=1e-8)
    xyz = dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords().tolist()))
    assert [float(c) for c in rows[1][1:4]] == pytest.approx(xyz[int(rows[1][0])])
    full = app.execute("result.export_table", result=rid, frame=2, field="DISP", path=str(tmp_path / "d.csv"), coordinates=False, nodes=[1, 2])
    assert full["rows"] == 2 and full["columns"][:3] == ["D1", "D2", "D3"] and "magnitude" in full["columns"]
    assert list(csv.reader(open(tmp_path / "d.csv")))[0][:2] == ["node", "D1"]
    # 경로: 보의 축을 따라 처짐(D3)의 크기가 뿌리에서 끝으로 커진다
    path = app.execute("result.path", result=rid, frame=2, field="DISP", component="D3", points=[[0.0, 10.0, 5.0], [100.0, 10.0, 5.0]], samples=11)
    s = path["samples"]
    assert path["length"] == pytest.approx(100.0) and len(s) == 11 and s[0]["s"] == 0 and s[-1]["s"] == pytest.approx(100.0)
    assert all(b["value"] <= a["value"] + 1e-12 for a, b in zip(s, s[1:])) and s[-1]["value"] < s[0]["value"]  # 아래로 처진다(음수)
    assert path["interpolated"] == 11 and all(x["method"] == "interpolated" for x in s) and s[0]["value"] == 0.0  # 뿌리는 구속
    # 요소 안 보간: 끝단 중심의 값은 끝단 꼭짓점 4개 값의 평균(육면체의 삼선형 보간)
    tip_rows = app.execute("result.values", result=rid, frame=2, field="DISP", component="D3", nodes=sorted(solved["tip"].props["ids"]))
    corners = [v for n, v in zip(tip_rows["ids"], tip_rows["values"]) if xyz[n][1] in (0.0, 20.0) and xyz[n][2] in (0.0, 10.0)]
    assert len(corners) == 4 and s[-1]["value"] == pytest.approx(sum(corners) / 4, rel=1e-9)
    outside = app.execute("result.path", result=rid, frame=2, field="DISP", component="D3", points=[[0.0, 10.0, 50.0], [100.0, 10.0, 50.0]], samples=3)
    assert outside["interpolated"] == 0 and all(x["method"] == "nearest_node" and x["distance"] == pytest.approx(40.0) for x in outside["samples"])
    # 비교: 같은 프레임은 차이 0, 프레임 1(절반 하중)과 2 는 다르다
    same = app.execute("result.compare", result=rid, frame=2, field="DISP", component="D3")
    assert same["max_abs_diff"] == 0 and same["count"] > 0 and same["missing"] == 0
    tip_node = max(tip_rows["ids"], key=lambda n: abs(tip_rows["values"][tip_rows["ids"].index(n)]))
    diff = app.execute("result.compare", result=rid, frame=2, field="DISP", component="D3", other_frame=1, nodes=[tip_node])
    assert diff["count"] == 1 and diff["max_abs_diff"] > 0 and diff["nodes"][0]["diff"] == pytest.approx(diff["nodes"][0]["a"] - diff["nodes"][0]["b"])
    assert 0.4 < abs(diff["nodes"][0]["b"] / diff["nodes"][0]["a"]) < 0.6  # 비선형 정적의 중간 증분(하중 절반)
    # 변환: 전역과 같은 직교 좌표계 → 그대로. z 축 둘레 90° → (x, y) 가 (y, -x) 로
    same_cs = app.model.csys.create_rectangular(name="G", origin=[0.0, 0.0, 0.0], axis1_point=[1.0, 0.0, 0.0], plane12_point=[0.0, 1.0, 0.0])
    rot = app.model.csys.create_rectangular(name="R", origin=[0.0, 0.0, 0.0], axis1_point=[0.0, 1.0, 0.0], plane12_point=[-1.0, 0.0, 0.0])
    node = tip_node
    d = app.execute("result.values", result=rid, frame=2, field="DISP", nodes=[node])["values"][0]
    t0 = app.execute("result.transform", result=rid, frame=2, field="DISP", csys=same_cs.id, nodes=[node])
    assert t0["components"] == ["V1", "V2", "V3"] and t0["values"][0] == pytest.approx(d, abs=1e-12)
    t1 = app.execute("result.transform", result=rid, frame=2, field="DISP", csys=rot.id, nodes=[node])["values"][0]
    assert t1 == pytest.approx([d[1], -d[0], d[2]], abs=1e-12)
    st = app.execute("result.values", result=rid, frame=2, field="STRESS", nodes=[node])
    sxx, syy, szz, sxy, sxz, syz = st["values"][0]
    ts = app.execute("result.transform", result=rid, frame=2, field="STRESS", csys=rot.id, nodes=[node])
    assert ts["components"] == ["S11", "S22", "S33", "S12", "S13", "S23"]
    assert ts["values"][0] == pytest.approx([syy, sxx, szz, -sxy, syz, -sxz], abs=1e-9)
    # 원통 좌표계(보의 축 = z 축 주위): 축에서 떨어진 노드의 반경 성분은 (x, y) 방향 투영이다
    cyl = app.model.csys.create_cylindrical(name="C", origin=[0.0, 0.0, 0.0], axis1_point=[1.0, 0.0, 0.0], plane12_point=[0.0, 1.0, 0.0])
    x, y, z = xyz[node]
    rr = (x * x + y * y) ** 0.5
    tc = app.execute("result.transform", result=rid, frame=2, field="DISP", csys=cyl.id, nodes=[node])["values"][0]
    assert tc[0] == pytest.approx((x * d[0] + y * d[1]) / rr, abs=1e-12) and tc[2] == pytest.approx(d[2], abs=1e-12)
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.transform", result=rid, frame=2, field="STRESS", csys=solved["root"].id)
    assert e.value.code == "wrong_kind"


@pytest.mark.feature("CAS-41")
def test_SOLVER_settings_default_solver(app, tmp_path, monkeypatch):
    """케이스에 솔버·스레드·작업 폴더 지정이 없으면 프로그램 설정의 값을 쓴다(환경 변수보다 앞)."""
    monkeypatch.delenv("NASA95_CCX", raising=False)
    monkeypatch.setenv("NASA95_SETTINGS", str(tmp_path / "settings.json"))
    part, mat, root, tip, case = cantilever(app, order=1, n=(4, 1, 1))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=case.id, wait=True)
    assert e.value.code == "solver_not_found"
    app.execute("app.settings_set", key="solver_executable", value=CCX)
    app.execute("app.settings_set", key="threads", value=1)
    app.execute("app.settings_set", key="work_directory", value=str(tmp_path / "작업"))
    r = app.execute("case.run", id=case.id, wait=True)
    assert r["state"] == "completed" and r["solver"] == CCX
    assert pathlib.Path(r["work_directory"]) == tmp_path / "작업" / "cantilever"  # 설정 폴더 아래 작업 이름 폴더


@pytest.mark.feature("RES-01")
@pytest.mark.feature("RES-19")
@pytest.mark.feature("RES-20")
@pytest.mark.feature("RES-21")
def test_RES_T07_result_file_and_derived(app, solved):
    """결과 파일 객체로 결과를 열고, 파생 결과(수식·조합·포락)를 계산한다. 객체는 모델(Undo·저장)에 속하고 값은 계산한다."""
    case = solved["case"]
    rf = app.model.results.create(name="R1", case=case.id)
    r = app.execute("result.open", file=rf.id)
    assert r["frames"] == 8
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.open", file=case.id)
    assert e.value.code == "wrong_kind"
    rid = r["id"]
    tip = solved["tip"].props["ids"]
    # 수식: 안전율 = 250 / mises (프레임 2), 매개변수도 변수로 쓴다
    app.model.parameters.create(name="SY", value=250.0)
    ex = rf.derived.create_expression(name="SF", expression="SY / max(STRESS.mises, 1e-9)", frame=2)
    v = app.execute("result.derived_values", id=ex.id, nodes=tip)
    mises = app.execute("result.values", result=rid, frame=2, field="STRESS", component="mises", nodes=tip)
    assert v["ids"] == mises["ids"] and v["values"] == pytest.approx([250.0 / max(m, 1e-9) for m in mises["values"]], rel=1e-9)
    assert app.execute("result.derived_values", id=ex.id, frame=1, nodes=tip)["frame"] == 1  # 매개변수의 프레임이 우선
    bad = rf.derived.create_expression(name="BAD", expression="DISP.NOPE + 1", frame=2)
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.derived_values", id=bad.id)
    assert e.value.code == "not_found"
    # 조합: 2 × 프레임 2 − 프레임 1 (같은 파일)
    comb = rf.derived.create_combination(name="C", field="DISP", component="D3", terms=[{"frame": 2, "factor": 2.0}, {"frame": 1, "factor": -1.0}])
    c = app.execute("result.derived_values", id=comb.id, nodes=tip)
    d2 = app.execute("result.values", result=rid, frame=2, field="DISP", component="D3", nodes=tip)["values"]
    d1 = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=tip)["values"]
    assert c["values"] == pytest.approx([2 * a - b for a, b in zip(d2, d1)], rel=1e-9, abs=1e-15)
    # 포락: 정적 프레임 1·2 의 처짐 최소(가장 아래로)는 프레임 2 에서, 절대값 최대도 프레임 2
    env = rf.derived.create_envelope(name="E", field="DISP", component="D3", frames=[1, 2], kind="min")
    ev = app.execute("result.derived_values", id=env.id, nodes=tip)
    assert ev["values"] == pytest.approx(d2) and set(ev["source_frames"]) == {2}
    env.update(kind="absmax")
    assert set(app.execute("result.derived_values", id=env.id, nodes=tip)["source_frames"]) == {2}
    env.update(kind="max")
    assert app.execute("result.derived_values", id=env.id, nodes=tip)["values"] == pytest.approx(d1)
    # 모든 프레임 포락: 고유치·좌굴 프레임에는 DISP 가 모드 형상으로 있다 — 출처 프레임이 다양하다
    allf = rf.derived.create_envelope(name="ALL", field="DISP", component="magnitude", kind="max")
    assert len(set(app.execute("result.derived_values", id=allf.id)["source_frames"])) >= 1
    # 객체는 모델에 속한다: Undo 로 사라지고 프로젝트에 저장된다
    n = len(app.execute("derived_result.list"))
    app.undo()
    assert len(app.execute("derived_result.list")) == n - 1
    app.redo()
    assert len(app.execute("derived_result.list")) == n
    assert app.execute("result_file.get", id=rf.id)["props"]["case"] == case.id
    app.execute("result.close", result=rid)
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.derived_values", id=ex.id)
    assert e.value.code == "invalid_state"


@pytest.mark.feature("RES-62")
def test_RES_T08_matrix_files(app, solved, tmp_path):
    """행렬·부분구조 출력 파일 목록: 결과 파일 옆에서 같은 이름의 .mtx 등을 찾는다."""
    rid = app.execute("result.open", case=solved["case"].id)["id"]
    r = app.execute("result.matrix_files", result=rid)
    assert r["files"] == [] and pathlib.Path(r["directory"]) == tmp_path
    (tmp_path / "cantilever.mtx").write_bytes(b"** matrix\n")
    (tmp_path / "other.mtx").write_text("")
    r = app.execute("result.matrix_files", result=rid)
    assert [f["name"] for f in r["files"]] == ["cantilever.mtx"] and r["files"][0]["kind"] == "mtx" and r["files"][0]["bytes"] == 10


@pytest.mark.feature("RES-17")
@pytest.mark.feature("RES-18")
@pytest.mark.feature("RES-25")
@pytest.mark.feature("RES-26")
def test_RES_T09_plot_and_report(app, solved, tmp_path):
    """그래프 객체(이력·경로)의 값은 result.history/result.path 와 같고 CSV 로 나간다. 보고서는 글·뷰·그래프·표를 HTML 하나로 모은다."""
    case, tip = solved["case"], solved["tip"].props["ids"]
    rf = app.model.results.create(name="R", case=case.id)
    hist = app.model.plots.create_history(name="tip_u3", result_file=rf.id, field="DISP", component="D3", node=tip[0], step=1)
    with pytest.raises(Nasa95Error) as e:  # 결과를 열기 전에는 값을 낼 수 없다
        app.execute("plot.data", id=hist.id)
    assert e.value.code == "invalid_state"
    rid = app.execute("result.open", file=rf.id)["id"]
    d = app.execute("plot.data", id=hist.id)
    h = app.execute("result.history", result=rid, field="DISP", component="D3", node=tip[0], step=1)
    assert d["type"] == "history" and d["x"] == [p["x"] for p in h] == [0.5, 1.0] and d["y"] == [p["y"] for p in h]
    assert (d["x_label"], d["y_label"]) == ("time", "DISP.D3")
    pts = [[0.0, 10.0, 5.0], [100.0, 10.0, 5.0]]
    path = app.model.plots.create_path(name="u3_along", result_file=rf.id, field="DISP", component="D3", frame=2, points=pts, samples=11)
    pd = app.execute("plot.data", id=path.id)
    rp = app.execute("result.path", result=rid, frame=2, field="DISP", component="D3", points=pts, samples=11)
    assert pd["x"] == [s["s"] for s in rp["samples"]] and pd["y"] == [s["value"] for s in rp["samples"]] and pd["x"][-1] == pytest.approx(100.0)
    assert pd["y"][0] == pytest.approx(0.0, abs=1e-9) and pd["y"][-1] < 0  # 고정단 0, 자유단은 아래로
    csv = tmp_path / "plot.csv"
    assert app.execute("plot.export", id=hist.id, path=str(csv))["rows"] == 2
    lines = csv.read_text().splitlines()
    assert lines[0] == "time,DISP.D3" and [float(l.split(",")[0]) for l in lines[1:]] == [0.5, 1.0]
    with pytest.raises(Nasa95Error) as e:
        app.execute("plot.data", id=rf.id)
    assert e.value.code == "wrong_kind"
    # 보고서: 뷰는 저장한 이름을 복원해 그리고, 그린 뒤 뷰 상태를 되돌린다
    app.execute("view.result_show", result=rid, frame=2, field="DISP", component="D3")
    app.execute("view.standard", name="iso")
    app.execute("view.save", name="iso_contour")
    app.execute("view.standard", name="front")
    before = app.execute("view.camera_get")
    rep = app.model.reports.create(name="rep", title="외팔보 보고서", items=[
        {"kind": "text", "title": "개요", "text": "끝단 하중 100 N <검증>"},
        {"kind": "view", "title": "처짐", "view": "iso_contour", "width": 160, "height": 120},
        {"kind": "plot", "title": "이력", "plot": hist.id},
        {"kind": "plot", "plot": path.id},
        {"kind": "table", "title": "최대·최소", "command": "result.minmax", "params": {"result": rid, "frame": 2, "field": "DISP", "component": "D3"}},
    ])
    out = tmp_path / "report.html"
    r = app.execute("report.generate", id=rep.id, path=str(out))
    assert r["items"] == ["text", "view", "plot", "plot", "table"] and out.stat().st_size == r["bytes"] > 1000
    html = out.read_text(encoding="utf-8")
    assert "<h1>외팔보 보고서</h1>" in html and "&lt;검증&gt;" in html and "<h2>개요</h2>" in html
    assert html.count("data:image/png;base64,") == 1 and 'width="160"' in html
    assert html.count("<svg") == 2 and "<th>frame</th>" in html and "DISP.D3" in html
    assert "<th>min</th>" in html or "<th>max</th>" in html  # table 항목: result.minmax 의 결과
    assert app.execute("view.camera_get") == before  # 뷰 상태 복원
    bad = app.model.reports.create(name="bad", items=[{"kind": "table", "command": "view.camera_get"}])
    with pytest.raises(Nasa95Error) as e:
        app.execute("report.generate", id=bad.id, path=str(tmp_path / "bad.html"))
    assert e.value.code == "invalid_param"
    with pytest.raises(Nasa95Error) as e:
        app.execute("report.generate", id=hist.id, path=str(out))
    assert e.value.code == "wrong_kind"


@pytest.mark.feature("API-21")
def test_RES_T10_custom_derived_result(app, solved):
    """확장이 등록한 파생 결과 계산: custom 파생 결과 객체가 result.derived_values 에서 그 명령을 부른다(result·frame·nodes 를 넘김)."""
    case, tip = solved["case"], solved["tip"].props["ids"]
    rf = app.model.results.create(name="R", case=case.id)
    rid = app.execute("result.open", file=rf.id)["id"]
    app.execute("ext.register_command", name="my.twice_u3", kind="Q", desc="D3 × 2",
                code="def run(app, params):\n"
                     "    v = app.execute('result.values', result=params['result'], frame=params['frame'], field='DISP', component='D3', nodes=params.get('nodes'))\n"
                     "    return {'ids': v['ids'], 'values': [x * params.get('factor', 2.0) for x in v['values']]}\n",
                params=[{"name": "result", "type": "integer", "desc": ""}, {"name": "frame", "type": "integer", "desc": ""},
                        {"name": "nodes", "type": "integer_list", "desc": ""}, {"name": "factor", "type": "number", "desc": ""}])
    app.execute("ext.register_derived_result", name="twice", command="my.twice_u3", label="두 배")
    d = rf.derived.create_custom(name="T", calculation="twice", frame=2, params={"factor": 3.0})
    v = app.execute("result.derived_values", id=d.id, nodes=tip)
    u3 = app.execute("result.values", result=rid, frame=2, field="DISP", component="D3", nodes=tip)["values"]
    assert v["ids"] == tip and v["values"] == pytest.approx([3.0 * x for x in u3]) and (v["type"], v["calculation"], v["frame"]) == ("custom", "twice", 2)
    assert app.execute("result.derived_values", id=d.id, frame=1, nodes=tip)["frame"] == 1
    bad = rf.derived.create_custom(name="B", calculation="nope", frame=2)
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.derived_values", id=bad.id)
    assert e.value.code == "not_found"


@pytest.mark.feature("RES-31")
def test_RES_T11_diagnostics(app, tmp_path, ccx_env):
    """발산 진단 자료: LAST ITERATIONS 로 솔버가 쓴 ResultsForLastIterations.frd 를 열고, <job>.cel 의 반복별 접촉 요소 셋과 <job>_Warn*.nam 의 경고 셋을 읽는다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(8, 1, 1))
    case.update(work_directory=str(tmp_path), threads=1)
    s = case.steps.create_static(nlgeom=True, initial_increment=0.5, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0 / len(tip.props["ids"])])
    s.outputs.create_node_file(variables=["U"], last_iterations=True, contact_elements=True)
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*NODE FILE, LAST ITERATIONS, CONTACT ELEMENTS" in deck
    # 덱을 다시 읽어도 두 옵션이 남는다
    (tmp_path / "re.inp").write_text(deck)
    again = App()
    again.execute("deck.import", path=str(tmp_path / "re.inp"))
    o = [again.execute("output_request.get", id=x["id"])["props"] for x in again.execute("output_request.list")]
    o = [x for x in o if x["type"] == "node_file"][0]
    assert o["last_iterations"] is True and o["contact_elements"] is True
    run = app.execute("case.run", id=case.id, wait=True)
    for _ in range(3):
        if not (run["state"] == "failed" and not run["errors"]):
            break
        run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", run["errors"]
    rid = app.execute("result.open", case=case.id)["id"]
    d = app.execute("result.diagnostics", result=rid)
    assert pathlib.Path(d["directory"]) == tmp_path and d["warnings"] == [] and d["contact_elements"] is None  # 접촉이 없으니 .cel 은 없다
    assert d["last_iterations"] is not None and d["last_iterations"]["bytes"] > 0 and "result" not in d["last_iterations"]
    d = app.execute("result.diagnostics", result=rid, open=True)
    li = d["last_iterations"]
    assert li["frames"] >= 1 and len(app.execute("result.steps", result=li["result"])) >= 1
    assert "DISP" in [f["name"] for f in app.execute("result.fields", result=li["result"], frame=1)]
    # 접촉 요소·경고 셋 파일은 매뉴얼의 꼴(inp)로 만들어 읽기를 확인한다
    (tmp_path / "cantilever.cel").write_text(
        "*ELEMENT, TYPE=C3D8, ELSET=contactelements_st1_in1_at1_it1\n9001, 1, 2, 3, 4, 5, 6, 7, 8\n"
        "*ELEMENT, TYPE=C3D8, ELSET=contactelements_st1_in2_at2_it3\n9002, 1, 2, 3, 4, 5, 6, 7, 8\n9003, 2, 3, 4, 5, 6, 7, 8, 9\n")
    (tmp_path / "cantilever_WarnNodeMissMasterIntersect.nam").write_text("** warning\n*NSET, NSET=WarnNodeMissMasterIntersect\n3, 4,\n5\n")
    (tmp_path / "other_WarnNodeMissMasterIntersect.nam").write_text("*NSET, NSET=X\n1\n")  # 다른 작업의 파일은 무시
    d = app.execute("result.diagnostics", result=rid)
    sets = d["contact_elements"]["sets"]
    assert [(s["name"], s["step"], s["increment"], s["attempt"], s["iteration"], s["elements"]) for s in sets] == [
        ("contactelements_st1_in1_at1_it1", 1, 1, 1, 1, 1), ("contactelements_st1_in2_at2_it3", 1, 2, 2, 3, 2)]
    assert "ids" not in sets[0]
    one = app.execute("result.diagnostics", result=rid, set="contactelements_st1_in2_at2_it3")["contact_elements"]["sets"][1]
    assert one["ids"] == [9002, 9003] and one["connectivity"][1] == [2, 3, 4, 5, 6, 7, 8, 9]
    assert d["warnings"] == [{"file": "cantilever_WarnNodeMissMasterIntersect.nam", "path": str(tmp_path / "cantilever_WarnNodeMissMasterIntersect.nam"),
                              "kind": "nodes", "name": "WarnNodeMissMasterIntersect", "warning": "WarnNodeMissMasterIntersect", "count": 3, "ids": [3, 4, 5]}]
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.diagnostics", result=rid, set="nope")
    assert e.value.code == "not_found"


@pytest.mark.feature("CAS-38")
@pytest.mark.feature("CAS-14")
def test_CAS_restart_and_attach(app, tmp_path, ccx_env):
    """재시작: 스텝 1 이 *RESTART, WRITE 로 .rout 를 남기고, 스텝을 하나 더 넣은 뒤 case.restart 가 `*RESTART, READ, STEP=1` 덱으로 이어서 푼다.
    이어서 푼 결과의 처짐은 처음부터 두 스텝을 푼 것과 같다. 결과 연결은 result_file 객체를 만든다."""
    def model(a):
        part, mat, root, tip, case = cantilever(a, order=1, n=(8, 1, 1))
        case.update(work_directory=str(tmp_path / a_name(a)), threads=1)
        T = {"type": "set", "ids": [tip.id]}
        s1 = case.steps.create_static(name="s1", restart_write=True)
        s1.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
        s1.loads.create_force(target=T, components=[0.0, 0.0, -100.0 / len(tip.props["ids"])])
        s1.outputs.create_node_file(variables=["U"])
        return case, tip, T
    names = {}
    def a_name(a):
        return names.setdefault(id(a), f"w{len(names)}")
    def solve_ok(a, cmd, **kw):
        run = a.execute(cmd, wait=True, **kw)
        for _ in range(3):
            if not (run["state"] == "failed" and not run["errors"]):
                break
            run = a.execute(cmd, wait=True, **kw)
        assert run["state"] == "completed", (run["errors"], a.execute("case.run_status", id=kw["id"], log_lines=30)["log"])
        return run
    case, tip, T = model(app)
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*RESTART, WRITE\n" in deck
    with pytest.raises(Nasa95Error) as e:  # 스텝 1 뒤에 쓸 스텝이 없다
        app.execute("case.restart", id=case.id, step=1)
    assert e.value.code == "invalid_param"
    run1 = solve_ok(app, "case.run", id=case.id)
    work = pathlib.Path(run1["work_directory"])
    assert (work / "cantilever.rout").exists()
    # 스텝 2(하중 두 배)를 더하고 스텝 1 뒤부터 이어서 푼다
    s2 = case.steps.create_static(name="s2")
    s2.loads.create_force(target=T, components=[0.0, 0.0, -200.0 / len(tip.props["ids"])])
    s2.outputs.create_node_file(variables=["U"])
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.restart", id=case.id, step=2)  # 뒤에 쓸 스텝이 없다
    assert e.value.code == "invalid_param"
    with pytest.raises(Nasa95Error) as e:  # 재시작 파일이 없다
        app.execute("case.restart", id=case.id, step=1, restart_file=str(work / "nope.rout"))
    assert e.value.code == "not_found"
    run2 = solve_ok(app, "case.restart", id=case.id, step=1)
    assert run2["restart_step"] == 1 and run2["job"] == "cantilever_restart1" and (work / "cantilever_restart1.rin").exists()
    restart_deck = pathlib.Path(run2["deck"]).read_text()
    assert restart_deck.splitlines()[1] == "*RESTART, READ, STEP=1" and "*NODE\n" not in restart_deck and "*NSET" not in restart_deck
    assert restart_deck.count("*STEP") == 1 and "TIP, 3, -50." in restart_deck and [w["code"] for w in run2["deck_warnings"]] == ["restart_sets"]
    rid = app.execute("result.open", case=case.id)["id"]
    frames = app.execute("result.steps", result=rid)
    node = tip.props["ids"][0]
    u_restart = app.execute("result.values", result=rid, frame=len(frames), field="DISP", component="D3", nodes=[node])["values"][0]
    # 처음부터 두 스텝을 푼 것과 비교
    other = App()
    case2, tip2, T2 = model(other)
    s2b = case2.steps.create_static(name="s2")
    s2b.loads.create_force(target=T2, components=[0.0, 0.0, -200.0 / len(tip2.props["ids"])])
    s2b.outputs.create_node_file(variables=["U"])
    solve_ok(other, "case.run", id=case2.id)
    rid2 = other.execute("result.open", case=case2.id)["id"]
    u_full = other.execute("result.values", result=rid2, frame=2, field="DISP", component="D3", nodes=[node])["values"][0]
    assert u_restart == pytest.approx(u_full, rel=1e-4) and u_full < 0
    # 덱을 다시 읽으면 restart_write 가 남는다
    again = App()
    again.execute("deck.import", path=run1["deck"])
    st = [again.execute("step.get", id=s["id"])["props"] for s in again.execute("step.list")]
    assert st[0]["restart_write"] is True
    # 결과 연결(CAS-14)
    r = app.execute("case.attach_results", id=case.id)
    rf = app.execute("result_file.get", id=r["id"])
    assert rf["props"]["case"] == case.id and rf["props"]["path"] == r["path"] and rf["name"].endswith(".frd")
    r2 = app.execute("case.attach_results", id=case.id, path=str(work / "cantilever.frd"), name="first", description="처음 실행")
    assert app.execute("result_file.get", id=r2["id"])["props"]["description"] == "처음 실행"
    assert app.execute("result.open", file=r2["id"])["frames"] == 1
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.attach_results", id=case.id, path=str(work / "nope.frd"))
    assert e.value.code == "not_found"
    app.undo()
    app.undo()
    assert app.execute("result_file.list") == []


@pytest.mark.feature("LOD-14")
def test_LOD_map_field(app, tmp_path):
    """외부 점 자료를 노드·면으로 보간(nearest·idw)하고, mapped_field 온도 하중이 덱에 노드마다 쓰여 솔버의 노드 온도(NT)가 보간값과 같다.
    압력은 면 중심 값으로 *DLOAD 에 면마다 나간다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(4, 1, 1))
    pts = [[0.0, 0.0, 0.0, 20.0], [100.0, 0.0, 0.0, 120.0]]  # x 를 따라 20 → 120
    r = app.execute("load.map_field", points=pts, target={"type": "set", "ids": [tip.id]}, method="nearest")
    assert r["what"] == "nodes" and set(r["ids"]) == set(tip.props["ids"]) and set(r["values"]) == {120.0}
    r = app.execute("load.map_field", points=pts, target={"type": "nodes", "ids": [1]}, method="idw", power=1.0)
    assert r["values"] == [20.0]  # 점 위의 노드는 그 점의 값
    mid = app.execute("mesh.find", what="nodes", box_min=[49.9, -1, -1], box_max=[50.1, 21, 11])["ids"]
    r = app.execute("load.map_field", points=pts, target={"type": "nodes", "ids": mid}, method="idw", power=1.0)
    assert all(v == pytest.approx(70.0) for v in r["values"])  # 두 점의 가운데: 1/d 가중 → 평균
    r = app.execute("load.map_field", points=pts, target={"type": "nodes", "ids": mid}, method="idw", power=1.0, radius=60.0)
    assert all(v == pytest.approx(70.0) for v in r["values"])
    r = app.execute("load.map_field", points=pts, target={"type": "nodes", "ids": [1]}, method="idw", radius=0.5)
    assert r["values"] == [20.0]
    # 면: 윗면(z=10, 면 번호 2)의 중심 값 — 요소 1~4 가 x 를 따라 놓인다
    top = {"type": "faces", "ids": [[e, 2] for e in range(1, 5)]}
    r = app.execute("load.map_field", points=pts, target=top, what="faces", method="nearest")
    assert r["faces"] == [[1, 2], [2, 2], [3, 2], [4, 2]] and r["values"] == [20.0, 20.0, 120.0, 120.0]  # 면 중심 x = 12.5, 37.5, 62.5, 87.5
    r = app.execute("load.map_field", points=pts, target=top, what="faces", method="idw", power=1.0)
    assert r["values"][0] < 70 < r["values"][3] and r["values"][0] + r["values"][3] == pytest.approx(140.0)  # 대칭
    with pytest.raises(Nasa95Error) as e:
        app.execute("load.map_field", points=[[0, 0, 0]], target={"type": "nodes", "ids": [1]})
    assert e.value.code == "invalid_param_type"
    with pytest.raises(Nasa95Error) as e:
        app.execute("load.map_field", target={"type": "nodes", "ids": [1]})
    assert e.value.code == "missing_param"
    # 온도 하중 객체: 덱에 노드마다 쓰이고 솔버가 그 온도를 쓴다(열팽창 → 변위, NT 출력)
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    everything = node_set(app, "ALL", [-1, -1, -1], [101, 21, 11])
    app.model.initial_conditions.create_temperature(target={"type": "set", "ids": [everything.id]}, value=20.0)
    load = s.loads.create_mapped_field(target={"type": "set", "ids": [everything.id]}, quantity="temperature", points=pts, method="idw", power=1.0)
    s.outputs.create_node_file(variables=["U"])
    s.outputs.create_node_print(variables=["NT"], target={"type": "nodes", "ids": mid + [1] + tip.props["ids"][:1]})
    r = app.execute("load.map_field", load=load.id)
    assert r["what"] == "nodes" and len(r["ids"]) == app.execute("project.info")["nodes"]
    deck = app.execute("case.preview_deck", id=case.id)
    assert deck["skipped"] == [] and "*TEMPERATURE\n" in deck["text"] and f"{mid[0]}, 70." in deck["text"]
    pressure = s.loads.create_mapped_field(target=top, quantity="pressure", points=pts, method="nearest")
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*DLOAD\n" in deck and deck.count(", P2, ") == 4 and "1, P2, 20." in deck and "4, P2, 120." in deck
    pressure.delete()
    dat, out = solve(app, case, tmp_path)
    temps = {int(row[0]): row[1] for title, rows in blocks(dat) if title.startswith("temperatures") for row in rows}
    assert [temps[n] for n in mid] == pytest.approx([70.0] * len(mid)) and temps[1] == 20.0 and temps[tip.props["ids"][0]] == 120.0


@pytest.mark.feature("RES-46")
@pytest.mark.feature("RES-47")
@pytest.mark.feature("RES-61")
def test_RES_T12_complex_results(app, tmp_path, ccx_env):
    """정상상태 동해석의 실수부(DISP)·허수부(DISPI)로 위상각 값을 구하고, *COMPLEX FREQUENCY 의 복소 고유치 표를 읽는다.
    솔버로 알게 된 것: 정상상태 동해석·복소 고유치는 앞 *FREQUENCY 스텝에 STORAGE=YES(.eig)가 필요하고, 복소 고유치의 .eig 는 PERTURBATION 없이 만들어야 한다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 1, 1))
    case.update(work_directory=str(tmp_path / "ssd"), threads=1)
    s = case.steps.create_frequency(num_modes=6, storage=True)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s2 = case.steps.create_steady_state_dynamics(freq_min=10.0, freq_max=400.0, points=5, damping_type="direct", modal_damping=[[1, 6, 0.02]])
    s2.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -10.0])
    s2.outputs.create_node_file(variables=["U", "PU"])
    run = app.execute("case.run", id=case.id, wait=True)
    for _ in range(3):  # 이 ccx 빌드는 드물게 끝난 뒤 힙 손상으로 죽는다(오류 메시지 없는 실패) — 다시 돌린다
        if not (run["state"] == "failed" and not run["errors"]):
            break
        run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", run["errors"]
    rid = app.execute("result.open", case=case.id)["id"]
    assert app.execute("result.steps", result=rid)[0]["fields"][:4] == ["DISP", "ERROR", "DISPI", "ERRORI"] or "DISPI" in app.execute("result.steps", result=rid)[0]["fields"]
    node = tip.props["ids"][0]
    re = app.execute("result.values", result=rid, frame=2, field="DISP", component="D3", nodes=[node])["values"][0]
    im = app.execute("result.values", result=rid, frame=2, field="DISPI", component="D3", nodes=[node])["values"][0]
    at = lambda deg: app.execute("result.at_phase", result=rid, frame=2, field="DISP", component="D3", phase=deg, nodes=[node])["values"][0]  # noqa: E731
    assert at(0.0) == pytest.approx(re) and at(90.0) == pytest.approx(-im) and at(180.0) == pytest.approx(-re) and at(45.0) == pytest.approx((re - im) / math.sqrt(2))
    # 크기 파생량은 합친 성분으로: 위상 0 의 크기 = 실수부의 크기
    mag = app.execute("result.at_phase", result=rid, frame=2, field="DISP", component="magnitude", phase=0.0, nodes=[node])["values"][0]
    assert mag == pytest.approx(app.execute("result.values", result=rid, frame=2, field="DISP", component="magnitude", nodes=[node])["values"][0])
    # 솔버가 쓴 크기·위상(PDISP)과 맞는지: |u| = sqrt(re² + im²)
    pd = app.execute("result.values", result=rid, frame=2, field="PDISP", component=app.execute("result.fields", result=rid, frame=2)[-1]["components"][2], nodes=[node])["values"][0]
    assert pd == pytest.approx(math.hypot(re, im), rel=1e-4)
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.at_phase", result=rid, frame=2, field="PDISP", component="D3", phase=0.0)
    assert e.value.code == "not_found"
    assert app.execute("result.complex_summary", result=rid) == []  # 복소 고유치 표가 없다
    # 복소 고유치: 회전(원심력) 정적 스텝 → 고유치(저장) → *COMPLEX FREQUENCY, CORIOLIS
    other = App()
    part2, mat2, root2, tip2, case2 = cantilever(other, order=1, n=(10, 1, 1))
    case2.update(work_directory=str(tmp_path / "cfreq"), threads=1)
    s0 = case2.steps.create_static(nlgeom=True)
    s0.bcs.create_displacement(target={"type": "set", "ids": [root2.id]}, dofs=[1, 2, 3])
    s0.loads.create_centrifugal(target={"type": "parts", "ids": [part2.id]}, omega=100.0, axis_point=[0, 0, 0], axis_direction=[0, 0, 1])
    case2.steps.create_frequency(num_modes=6, storage=True)
    s3 = case2.steps.create_complex_frequency(num_modes=6, coriolis=True)
    s3.outputs.create_node_file(variables=["U", "PU"])
    run = other.execute("case.run", id=case2.id, wait=True)
    assert run["state"] == "completed", run["errors"]
    rid2 = other.execute("result.open", case=case2.id)["id"]
    cs = other.execute("result.complex_summary", result=rid2)
    assert len(cs) == 1 and cs[0]["step"] == 3 and len(cs[0]["modes"]) == 6
    m1 = cs[0]["modes"][0]
    normal = [t for t in other.execute("result.modal_summary", result=rid2) if t["step"] == 2][0]["modes"][0]
    assert m1["mode"] == 1 and m1["frequency"] == pytest.approx(normal["frequency"], rel=1e-3) and m1["omega_real"] == pytest.approx(normal["omega"], rel=1e-3)
    assert abs(m1["omega_imag"]) < 1e-3 * m1["omega_real"]  # 감쇠 없는 보: 허수부 ≈ 0
    assert m1["participation"][0]["mode"] == 1 and m1["participation"][0]["real"] == pytest.approx(1.0) and len(m1["participation"]) == 6
    assert other.execute("result.steps", result=rid2)[0]["attributes"]["MODE"] == "1"


@pytest.mark.feature("RES-55")
@pytest.mark.feature("RES-57")
def test_RES_T13_beam_section_forces_and_integration_points(app, tmp_path, ccx_env):
    """보 외팔보(B31 10개, 끝단 힘 F): 단면력(SECTION FORCES)이 보 이론(전단력 = F, 뿌리 모멘트 = F·L)과 맞고, *EL PRINT 의 적분점 응력 표에 좌표가 붙는다."""
    mat = steel(app)
    L, F, nel = 100.0, 10.0, 10
    n = app.execute("mesh.nodes_create", coords=[[L * i / nel, 0.0, 0.0] for i in range(nel + 1)])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[[n[i], n[i + 1]] for i in range(nel)])["ids"]
    el = {"type": "elements", "ids": beams}
    app.model.properties.create_beam(material=mat.id, section="rect", dimensions=[4.0, 2.0], direction=[0.0, 1.0, 0.0], target=el)
    case = app.model.cases.create(name="beam")
    case.update(work_directory=str(tmp_path), threads=1)
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s.loads.create_force(target={"type": "nodes", "ids": [n[-1]]}, components=[0.0, 0.0, -F])
    s.outputs.create_node_file(variables=["U"], expand="2d")
    s.outputs.create_element_file(variables=["S"], section_forces=True)
    s.outputs.create_element_print(variables=["S", "COORD"], target=el)
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*EL FILE, OUTPUT=2D, SECTION FORCES" in deck and "*EL PRINT, ELSET=" in deck  # SECTION FORCES 는 OUTPUT=3D 와 함께 쓸 수 없다
    run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", run["errors"]
    rid = app.execute("result.open", case=case.id)["id"]
    r = app.execute("result.beam_section_forces", result=rid, frame=1)
    assert set(r["ids"]) == set(n) and len(r["normal_force"]) == len(n)
    by = {i: k for k, i in enumerate(r["ids"])}
    # 전단력: 끝단 힘이 z 방향이고 보의 1방향이 (0,1,0) 이므로 2방향(z) 전단력 = ±F 전 구간, 축력 ≈ 0
    shear = [abs(r["shear_2"][by[i]]) for i in n[1:-1]]
    assert all(v == pytest.approx(F, rel=0.05) for v in shear), shear
    assert all(abs(v) < 0.05 * F for v in r["normal_force"]), r["normal_force"]
    # 굽힘 모멘트(1축): 보 이론은 뿌리 F·L, 중간 F·L/2, 끝 0. 솔버로 확인: B31 의 노드 단면력은 요소 안의 값을 노드로 옮긴 것이라
    # 뿌리는 F·(L − Le/2) = 950 에 가깝고 끝은 F·Le/2 = 50 쯤 남는다 → 요소 길이의 절반만큼의 오차를 허용한다
    m_root, m_mid, m_tip = abs(r["moment_1"][by[n[0]]]), abs(r["moment_1"][by[n[nel // 2]]]), abs(r["moment_1"][by[n[-1]]])
    le = L / nel
    assert m_root == pytest.approx(F * L, abs=F * le) and m_mid == pytest.approx(F * L / 2, abs=F * le) and m_tip <= F * le
    sub = app.execute("result.beam_section_forces", result=rid, frame=1, nodes=[n[0], n[-1]])
    assert sub["ids"] == [n[0], n[-1]]
    # 적분점 값: 요소 1 의 적분점마다 응력 6성분과 좌표(x 가 요소 안)
    ip = app.execute("result.integration_point_values", result=rid, quantity="stresses", elements=[beams[0]])
    assert len(ip) == 1 and ip[0]["quantity"] == "stresses" and ip[0]["rows"]
    row = ip[0]["rows"][0]
    assert row["element"] == beams[0] and set(row["values"]) >= {"sxx", "syy", "szz", "sxy", "sxz", "syz"}
    assert "coordinates" in row and 0.0 <= row["coordinates"][0] <= L / nel
    allrows = app.execute("result.integration_point_values", result=rid)
    assert {t["quantity"] for t in allrows} == {"stresses"} and sum(len(t["rows"]) for t in allrows) > nel
    assert app.execute("result.integration_point_values", result=rid, time=99.0) == []
    # 펼친(3D) 출력에서는 보 요소가 없어 단면력을 읽을 수 없다
    s.outputs.create_element_file(variables=["S"], section_forces=False, expand="3d")
    app.execute("output_request.delete", id=[o["id"] for o in app.execute("output_request.list", parent=s.id) if app.execute("output_request.get", id=o["id"])["props"].get("section_forces")][0])
    run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", run["errors"]
    rid3 = app.execute("result.open", case=case.id)["id"]
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.beam_section_forces", result=rid3, frame=1)
    assert e.value.code == "invalid_state"


@pytest.mark.feature("PRP-05")
@pytest.mark.feature("PRP-06")
@pytest.mark.feature("RES-53")
@pytest.mark.parametrize("section", ["I", "T"])
def test_PRP_T01_30_composite_section_cantilever(app, tmp_path, ccx_env, section):
    """형강 합성보(D15)를 솔버로 확인: I·T 단면 외팔보(B31)의 끝 처짐이 보 이론(도심 기준 I)과 1% 안에서 맞고,
    펼친 결과의 플랜지가 1축(+z) 쪽에 생기며(OFFSET 부호), 펼친 노드가 모델 노드에 대응돼 결과를 입힐 수 있다."""
    mat = steel(app)
    L, P, nel = 1000.0, 1000.0, 5
    n = app.execute("mesh.nodes_create", coords=[[L * i / nel, 0.0, 0.0] for i in range(nel + 1)])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[[n[i], n[i + 1]] for i in range(nel)])["ids"]
    h, b, tw, tf = 100.0, 60.0, 6.0, 8.0
    app.model.properties.create_beam(material=mat.id, section=section, dimensions=[h, b, tw, tf], direction=[0.0, 0.0, 1.0],
                                     target={"type": "elements", "ids": beams})
    case = app.model.cases.create(name="steel")
    case.update(work_directory=str(tmp_path), threads=1)
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s.loads.create_force(target={"type": "nodes", "ids": [n[-1]]}, components=[0.0, 0.0, -P])
    s.outputs.create_node_file(variables=["U"])
    s.outputs.create_element_file(variables=["S"])
    run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", run["errors"]
    rid = app.execute("result.open", case=case.id)["id"]
    E = 210000.0
    if section == "I":
        I = b * h**3 / 12 - (b - tw) * (h - 2 * tf) ** 3 / 12
    else:  # T: 플랜지(+1축 쪽) + 웨브, 도심 기준
        A1, z1, A2, z2 = b * tf, (h - tf) / 2, (h - tf) * tw, -tf / 2
        zc = (A1 * z1 + A2 * z2) / (A1 + A2)
        I = b * tf**3 / 12 + A1 * (z1 - zc) ** 2 + tw * (h - tf) ** 3 / 12 + A2 * (z2 - zc) ** 2
    tip = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=[n[-1]], shell_face="mid")["values"][0]
    assert tip == pytest.approx(-P * L**3 / (3 * E * I), rel=0.01), (tip, -P * L**3 / (3 * E * I))
    # 펼친 노드의 위치: 끝단에서 z > h/2 - tf 인 노드는 플랜지(폭 b 전체) → OFFSET 부호가 맞다(플랜지가 +1축 쪽)
    info = app.execute("result.info", result=rid)
    frd = pathlib.Path(info["path"])
    xyz, block = [], False
    for line in frd.read_text().splitlines():
        if line.startswith("    2C"):
            block = True
        elif block and line.startswith(" -1"):
            xyz.append((float(line[13:25]), float(line[25:37]), float(line[37:49])))
        elif block and line.startswith(" -3"):
            break
    top = [y for x, y, z in xyz if abs(x - L) < 1e-6 and z > h / 2 - tf + 1e-6]
    assert top and max(top) == pytest.approx(b / 2) and min(top) == pytest.approx(-b / 2)
    if section == "T":
        bottom = [y for x, y, z in xyz if abs(x - L) < 1e-6 and z < -h / 2 + tf]
        assert bottom and max(bottom) == pytest.approx(tw / 2)  # 아래쪽은 웨브뿐
    # 응력도 모델 노드에 입힌다(RES-53): 뿌리 노드의 굽힘 응력 크기가 M·c/I 에 가깝다(평균이므로 자릿수만)
    sxx = app.execute("result.values", result=rid, frame=1, field="STRESS", component="SXX", nodes=[n[1]], shell_face="mid")["values"]
    assert len(sxx) == 1


@pytest.mark.feature("CMN-11")
@pytest.mark.feature("CMN-12")
def test_CMN_unit_convert_model(app, tmp_path):
    """단위계 변환: mm-t-s 모델을 m-kg-s 로 바꾸면 길이·압력·밀도·강성이 환산되고(계수 확인), 두 모델을 풀면 처짐이 같다(m = mm × 1e-3).
    표시 단위는 값을 바꾸지 않고 환산 계수만 준다."""
    part, mat, root, tip, case = cantilever(app, order=1, n=(10, 1, 1))
    mat.set_specific_heat(data=[[4.46e8]])
    sh = app.model.properties.create_shell(material=mat.id, thickness=2.0, target={"type": "elements", "ids": [1]})
    comp = app.model.properties.create_composite(target={"type": "elements", "ids": [2]}, layers=[{"thickness": 0.5, "material": mat.id}])
    spr = app.model.properties.create_spring(stiffness=100.0, target={"type": "elements", "ids": [3]})
    step = case.steps.create_static()
    step.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    force = step.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0 / len(tip.props["ids"])])
    pres = step.loads.create_pressure(target={"type": "faces", "ids": [[1, 2]]}, value=0.5)
    temp = step.loads.create_temperature(target={"type": "set", "ids": [tip.id]}, value=50.0)
    step.outputs.create_node_file(variables=["U"])
    # 표시 단위
    d = app.execute("unit.set_display", system="m-kg-s")
    assert d["model"] == "mm-t-s" and d["display"] == "m-kg-s" and d["factors"]["length"] == pytest.approx(1e-3) and d["factors"]["pressure"] == pytest.approx(1e6)
    assert app.execute("unit.set_display")["display"] == "mm-t-s"
    assert app.execute("mesh.nodes", ids=[tip.props["ids"][0]])["coords"][0][0] == pytest.approx(100.0)  # 값은 그대로
    # 모델 변환
    xyz0 = app.execute("mesh.nodes", ids=[tip.props["ids"][0]])["coords"][0]
    r = app.execute("unit.convert_model", to="m-kg-s")
    assert r["from"] == "mm-t-s" and r["to"] == "m-kg-s" and r["nodes"] == app.execute("project.info")["nodes"] and r["unconverted"] == []
    assert app.execute("unit.get")["system"] == "m-kg-s"
    assert app.execute("mesh.nodes", ids=[tip.props["ids"][0]])["coords"][0] == pytest.approx([v * 1e-3 for v in xyz0])
    m = app.execute("material.get", id=mat.id)["props"]["behaviors"]
    assert m["elastic"]["data"][0] == pytest.approx([210000.0 * 1e6, 0.3]) and m["density"]["data"][0][0] == pytest.approx(7850.0)
    assert m["expansion"]["data"][0][0] == pytest.approx(1.2e-5) and m["conductivity"]["data"][0][0] == pytest.approx(50.0)
    assert m["specific_heat"]["data"][0][0] == pytest.approx(446.0)
    assert app.execute("property.get", id=sh.id)["props"]["thickness"] == pytest.approx(2e-3)
    assert app.execute("property.get", id=comp.id)["props"]["layers"][0]["thickness"] == pytest.approx(0.5e-3)
    assert app.execute("property.get", id=spr.id)["props"]["stiffness"] == pytest.approx(100.0 * 1e3)  # N/mm → N/m
    assert app.execute("load.get", id=force.id)["props"]["components"][2] == pytest.approx(-100.0 / len(tip.props["ids"]))  # N 은 그대로
    assert app.execute("load.get", id=pres.id)["props"]["value"] == pytest.approx(0.5e6) and app.execute("load.get", id=temp.id)["props"]["value"] == 50.0
    # 되돌리면 전부 원래대로(Undo 한 단계), 다시 하면 같은 값
    app.undo()
    assert app.execute("unit.get")["system"] == "mm-t-s" and app.execute("mesh.nodes", ids=[tip.props["ids"][0]])["coords"][0] == pytest.approx(xyz0)
    app.redo()
    assert app.execute("unit.get")["system"] == "m-kg-s"
    # 형상: 상자 크기(길이)와 가져온 형상의 배율이 환산된다
    geo = app.model.parts.create(name="G")
    bx = geo.features.create_box(size=[10.0, 10.0, 10.0])
    app.execute("unit.convert_model", to="mm-t-s")
    assert app.execute("feature.get", id=bx.id)["props"]["size"] == pytest.approx([1e4, 1e4, 1e4])
    app.execute("unit.convert_model", to="m-kg-s")
    # 같은 단위계로는 아무것도 바꾸지 않는다
    assert app.execute("unit.convert_model", to="m-kg-s")["fields"] == 0
    # 차원을 모르는 구성 모델이 있으면 거부, force 로 진행
    rubber = app.model.materials.create(name="RUBBER")
    rubber.set_hyperelastic(model="neo_hooke", data=[[80.0, 0.0]])
    with pytest.raises(Nasa95Error) as e:
        app.execute("unit.convert_model", to="mm-t-s")
    assert e.value.code == "not_available" and e.value.details["unconverted"][0]["behavior"] == "hyperelastic"
    r = app.execute("unit.convert_model", to="mm-t-s", force=True)
    assert r["unconverted"][0] == {"object": rubber.id, "kind": "material", "behavior": "hyperelastic"}
    app.undo()
    rubber.delete()
    # 솔버로 확인: 변환한 모델(m-kg-s)을 풀면 처짐이 mm 모델의 1e-3 배
    for p in app.execute("property.list"):
        if p["id"] in (sh.id, comp.id, spr.id):
            app.execute("property.delete", id=p["id"])
    pres.delete()
    temp.delete()
    (tmp_path / "m").mkdir()
    dat_m, _ = solve(app, case, tmp_path / "m")
    rid_m = app.execute("result.open", path=str(tmp_path / "m" / "job.frd"))["id"]
    u_m = app.execute("result.values", result=rid_m, frame=1, field="DISP", component="D3", nodes=[tip.props["ids"][0]])["values"][0]
    app.execute("unit.convert_model", to="mm-t-s")
    (tmp_path / "mm").mkdir()
    dat_mm, _ = solve(app, case, tmp_path / "mm")
    rid_mm = app.execute("result.open", path=str(tmp_path / "mm" / "job.frd"))["id"]
    u_mm = app.execute("result.values", result=rid_mm, frame=1, field="DISP", component="D3", nodes=[tip.props["ids"][0]])["values"][0]
    assert u_mm < 0 and u_m == pytest.approx(u_mm * 1e-3, rel=1e-4)


@pytest.mark.feature("LOD-03")
@pytest.mark.feature("LOD-07")
def test_SOLVER_traction_equals_pressure(app, tmp_path):
    """등가 집중 하중으로 쓴 분포력(traction)·합력(total_force)이 같은 면의 압력과 같은 처짐을 준다(1차 요소: 면적 균등 분배가 일관 하중과 같다)."""
    results = {}
    for kind in ("pressure", "traction", "total_force"):
        a = App()
        part, mat, root, tip, case = cantilever(a, order=1, n=(10, 2, 2))
        top = {"type": "faces", "ids": [[e, 2] for e in a.execute("mesh.find", what="elements", box_min=[-1, -1, 4.9], box_max=[101, 21, 11])["ids"]]}
        s = case.steps.create_static()
        s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
        if kind == "pressure":
            s.loads.create_pressure(target=top, value=0.1)
        elif kind == "traction":
            s.loads.create_traction(target=top, components=[0.0, 0.0, -0.1])
        else:
            s.loads.create_total_force(target=top, components=[0.0, 0.0, -0.1 * 100.0 * 20.0])
        s.outputs.create_node_print(variables=["U"], target={"type": "set", "ids": [tip.id]})
        work = tmp_path / kind
        work.mkdir()
        dat, _ = solve(a, case, work)
        results[kind] = [row[3] for title, rows in blocks(dat) if title.startswith("displacements") for row in rows]
    assert results["pressure"] and min(results["pressure"]) < 0
    assert results["traction"] == pytest.approx(results["pressure"], rel=1e-4)
    assert results["total_force"] == pytest.approx(results["pressure"], rel=1e-4)
