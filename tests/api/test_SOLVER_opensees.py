"""케이스의 solver 가 opensees 이면 덱이 OpenSees 의 Tcl 스크립트로 나가고 OpenSees 로 풀린다(D16).

OpenSees 실행 파일: 환경 변수 NASA95_OPENSEES, 없으면 ..\\third_party\\opensees 아래에서 찾는다. 없으면 이 파일의 테스트는 건너뛴다.
값은 보 이론·1자유도계의 닫힌 해와 비교한다(단위: m, N, kg, s).
"""
import math
import os
import pathlib

import pytest

from nasa95 import Nasa95Error


def _find_opensees():
    env = os.environ.get("NASA95_OPENSEES")
    if env and pathlib.Path(env).exists():
        return env
    root = pathlib.Path(__file__).resolve().parents[3] / "third_party" / "opensees"
    hits = sorted(root.rglob("OpenSees.exe")) + sorted(root.rglob("OpenSees")) if root.exists() else []
    return str(hits[0]) if hits else None


OPENSEES = _find_opensees()
pytestmark = pytest.mark.skipif(OPENSEES is None, reason="OpenSees 실행 파일이 없습니다(NASA95_OPENSEES)")

E, NU = 3.0e10, 0.2
B = 0.3                      # 정사각형 단면 0.3 m
AREA, INERTIA = B * B, B ** 4 / 12
L, MASS = 3.0, 1000.0
K = 3 * E * INERTIA / L ** 3  # 외팔 기둥의 끝단 강성


@pytest.fixture
def ops_env(monkeypatch):
    monkeypatch.setenv("NASA95_OPENSEES", OPENSEES)


def column(app, tmp_path, nel=4, axis=(0.0, 1.0, 0.0), dimensions=(B, B), direction=None, density=None, name="col"):
    """뿌리를 고정한 외팔 기둥(선 요소 nel 개)과 끝단의 점 질량. (케이스, 노드 목록, 보 요소 목록)"""
    mat = app.model.materials.create(name="CONC")
    mat.set_elastic(data=[[E, NU]])
    if density:
        mat.set_density(data=[[density]])
    n = app.execute("mesh.nodes_create", coords=[[axis[0] * L * i / nel, axis[1] * L * i / nel, axis[2] * L * i / nel] for i in range(nel + 1)])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", connectivity=[[n[i], n[i + 1]] for i in range(nel)])["ids"]
    point = app.execute("mesh.elements_create", shape="point1", connectivity=[[n[-1]]])["ids"]
    extra = {"direction": list(direction)} if direction else {}
    app.model.properties.create_beam(name="COLUMN", material=mat.id, section="rect", dimensions=list(dimensions), target={"type": "elements", "ids": beams}, **extra)
    app.model.properties.create_mass(name="TIP", mass=MASS, target={"type": "elements", "ids": point})
    case = app.model.cases.create(name=name, solver="opensees")
    case.update(work_directory=str(tmp_path))
    return case, n, beams


def fix_root(step, node):
    step.bcs.create_displacement(target={"type": "nodes", "ids": [node]}, dofs=[1, 2, 3, 4, 5, 6])


def run(app, case):
    r = app.execute("case.run", id=case.id, wait=True)
    assert r["state"] == "completed", (r["errors"], r["log"])
    assert r["skipped"] == [], r["skipped"]
    return r


def value(app, rid, frame, field, component, node):
    return app.execute("result.values", result=rid, frame=frame, field=field, component=component, nodes=[node])["values"][0]


@pytest.mark.feature("CAS-44")
@pytest.mark.feature("CAS-10")
@pytest.mark.feature("CAS-13")
def test_CAS_T10_01_static_cantilever(app, tmp_path, ops_env):
    """평면 외팔 기둥의 끝단 힘: 처짐 F·L³/(3EI), 뿌리 반력 −F. 덱은 Tcl 스크립트(ndm 2)로 나가고 결과는 frd 로 열린다."""
    case, n, beams = column(app, tmp_path)
    s = case.steps.create_static()
    fix_root(s, n[0])
    F = 5000.0
    s.loads.create_force(target={"type": "nodes", "ids": [n[-1]]}, components=[F, 0.0, 0.0])
    deck = app.execute("case.preview_deck", id=case.id)
    assert "model basic -ndm 2 -ndf 3" in deck["text"] and "element elasticBeamColumn" in deck["text"] and deck["skipped"] == []
    assert "*NODE" not in deck["text"]  # CalculiX 덱이 아니다
    r = run(app, case)
    assert r["deck"].endswith("col.tcl") and {"tcl", "frd", "sta"} <= set(r["files"]) and r["increments"][-1]["step"] == 1
    rid = app.execute("result.open", case=case.id)["id"]
    frames = app.execute("result.steps", result=rid)
    assert len(frames) == 1 and frames[0]["type"] == "static"
    assert value(app, rid, 1, "DISP", "D1", n[-1]) == pytest.approx(F / K, rel=1e-4)
    assert value(app, rid, 1, "FORC", "F1", n[0]) == pytest.approx(-F, rel=1e-5)


@pytest.mark.feature("CAS-44")
def test_CAS_T10_02_eigen_period(app, tmp_path, ops_env):
    """끝단 질량만 있는 외팔 기둥의 1차 고유진동수 = sqrt(k/m)/(2π). 모드마다 프레임 하나."""
    case, n, beams = column(app, tmp_path)
    s = case.steps.create_frequency(num_modes=1)
    fix_root(s, n[0])
    run(app, case)
    rid = app.execute("result.open", case=case.id)["id"]
    frames = app.execute("result.steps", result=rid)
    assert len(frames) == 1 and frames[0]["type"] == "frequency"
    assert frames[0]["value"] == pytest.approx(math.sqrt(K / MASS) / (2 * math.pi), rel=1e-4)
    assert abs(value(app, rid, 1, "DISP", "D1", n[-1])) > 0  # 모드 형상


@pytest.mark.feature("CAS-44")
def test_CAS_T10_03_ground_motion(app, tmp_path, ops_env):
    """지반 가속도(계단 a0)를 받는 무감쇠 1자유도계: 상대 변위는 −m·a0/k 둘레로 흔들려 최대 2·m·a0/k 에 이른다."""
    case, n, beams = column(app, tmp_path)
    a0 = 1.0
    quake = app.model.functions.create_amplitude(name="STEP_ACC", points=[[0.0, a0], [10.0, a0]])
    period = 2 * math.pi * math.sqrt(MASS / K)
    s = case.steps.create_dynamic(initial_increment=period / 200, period=1.5 * period)
    fix_root(s, n[0])
    s.bcs.create_base_motion(dof=1, motion="acceleration", amplitude=quake.id)
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "pattern UniformExcitation" in deck and "integrator Newmark 0.5 0.25" in deck
    run(app, case)
    rid = app.execute("result.open", case=case.id)["id"]
    frames = app.execute("result.steps", result=rid)
    assert len(frames) == 300 and frames[0]["type"] == "time" and frames[-1]["value"] == pytest.approx(1.5 * period, rel=1e-5)
    assert {f["name"] for f in app.execute("result.fields", result=rid, frame=1)} >= {"DISP", "VELO", "ACCE"}
    u = [value(app, rid, k + 1, "DISP", "D1", n[-1]) for k in range(len(frames))]
    assert min(u) == pytest.approx(-2 * MASS * a0 / K, rel=2e-3) and max(u) <= 1e-9  # 가진의 반대쪽으로만 간다
    assert u[99] == pytest.approx(min(u), rel=1e-3)  # 반주기(100번째 증분)에 최대


@pytest.mark.feature("CAS-44")
def test_CAS_T10_04_space_frame_axes(app, tmp_path, ops_env):
    """3차원 모델(ndm 3): 단면 1축(direction)에 따라 두 방향의 굽힘 강성이 갈린다. 폭 a(1축)·춤 b 인 직사각형은 1축 둘레 I = a·b³/12."""
    a, b = 0.2, 0.4
    case, n, beams = column(app, tmp_path, axis=(0.0, 0.0, 1.0), dimensions=(a, b), direction=(1.0, 0.0, 0.0))
    s = case.steps.create_static()
    fix_root(s, n[0])
    F = 1000.0
    s.loads.create_force(target={"type": "nodes", "ids": [n[-1]]}, components=[F, F, 0.0])
    assert "model basic -ndm 3 -ndf 6" in app.execute("case.preview_deck", id=case.id)["text"]
    run(app, case)
    rid = app.execute("result.open", case=case.id)["id"]
    # y 로 미는 힘은 1축(x) 둘레로 휜다 → I11 = a·b³/12. x 로 미는 힘은 2축 둘레 → I22 = b·a³/12
    assert value(app, rid, 1, "DISP", "D2", n[-1]) == pytest.approx(F * L ** 3 / (3 * E * a * b ** 3 / 12), rel=1e-4)
    assert value(app, rid, 1, "DISP", "D1", n[-1]) == pytest.approx(F * L ** 3 / (3 * E * b * a ** 3 / 12), rel=1e-4)


@pytest.mark.feature("CAS-44")
def test_CAS_T10_05_gravity_then_eigen(app, tmp_path, ops_env):
    """중력(정적) → 고유치: 자중은 요소 질량에서 절점 하중으로 간다(뿌리 축력 = 전체 무게). 보 등분포 하중은 w·L⁴/(8EI)."""
    rho, g = 2400.0, 9.81
    case, n, beams = column(app, tmp_path, density=rho)
    el = {"type": "elements", "ids": beams}
    s1 = case.steps.create_static(name="gravity")
    fix_root(s1, n[0])
    s1.loads.create_gravity(target={"type": "elements", "ids": beams + [beams[-1] + 1]}, value=g, direction=[0.0, -1.0, 0.0])
    w = 200.0
    s1.loads.create_line_load(target=el, components=[w, 0.0, 0.0])
    s2 = case.steps.create_frequency(name="modes", num_modes=2)
    run(app, case)
    rid = app.execute("result.open", case=case.id)["id"]
    frames = app.execute("result.steps", result=rid)
    assert [f["type"] for f in frames] == ["static", "frequency", "frequency"] and [f["step"] for f in frames] == [1, 2, 2]
    weight = (rho * AREA * L + MASS) * g
    # 절점에 모은 자중: 뿌리 절점의 몫(요소 길이의 절반)은 구속된 자유도라 반력에 그대로 더해진다 → 반력은 전체 무게
    assert value(app, rid, 1, "FORC", "F2", n[0]) == pytest.approx(weight, rel=1e-5)  # frd 는 유효숫자 6자리
    assert value(app, rid, 1, "DISP", "D1", n[-1]) == pytest.approx(w * L ** 4 / (8 * E * INERTIA), rel=1e-4)
    # 분포 질량이 더해져 끝단 질량만 있을 때보다 느리다(레일리 어림: 유효 질량 m + 0.2357·ρAL)
    f1 = math.sqrt(K / (MASS + 0.2357 * rho * AREA * L)) / (2 * math.pi)
    assert frames[1]["value"] == pytest.approx(f1, rel=0.01) and frames[2]["value"] > 5 * frames[1]["value"]


@pytest.mark.feature("CAS-44")
def test_CAS_T10_06_unsupported_is_reported(app, tmp_path, ops_env):
    """쓰지 못하는 것은 조용히 빠뜨리지 않고 skipped 로 알리고, 실행은 막는다. 실행 파일이 없으면 solver_not_found."""
    case, n, beams = column(app, tmp_path)
    s = case.steps.create_static()
    fix_root(s, n[0])
    moved = s.bcs.create_displacement(target={"type": "nodes", "ids": [n[-1]]}, dofs=[1], values=[0.01])
    buckle = case.steps.create_buckle(num_modes=1)
    deck = app.execute("case.preview_deck", id=case.id)
    assert {k["object"] for k in deck["skipped"]} == {moved.id, buckle.id} and all(k["reason"] for k in deck["skipped"])
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=case.id, wait=True)
    assert e.value.code == "deck_incomplete"
    # 입력 검사만: 모델을 만들고 끝낸다
    moved.delete()
    buckle.delete()
    r = app.execute("case.run_check_only", id=case.id, wait=True)
    assert r["state"] == "completed" and "frd" not in r["files"]
    # CalculiX 케이스는 그대로 CalculiX 덱이다
    other = app.model.cases.create(name="ccx")
    other.steps.create_static()
    assert "*NODE" in app.execute("case.preview_deck", id=other.id)["text"]
    case.update(solver_executable=str(tmp_path / "none.exe"))
    with pytest.raises(Nasa95Error) as e:
        app.execute("case.run", id=case.id, wait=True)
    assert e.value.code == "solver_not_found"
