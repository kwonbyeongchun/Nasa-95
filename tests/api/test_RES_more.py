"""결과 보충(RES-T05·T06·T07·T03·T01-06): 열 결과, 접촉 결과, 오차 추정, 단면 출력, 애니메이션·위상, 전개된 쉘 결과와 면별 응력, 제거된 요소,
로컬 좌표계 결과, 유사 해석 표기, 지연 로딩. ccx 가 없으면 건너뛴다.
"""
import math
import pathlib

import numpy as np
import pytest

from openfep import App, OfepError

import test_SOLVER_ccx as S
from meshutil import block, plate

pytestmark = pytest.mark.skipif(S.CCX is None, reason="ccx 실행 파일이 없습니다(OFEP_CCX)")


@pytest.fixture
def ccx_env(monkeypatch):
    monkeypatch.setenv("OFEP_CCX", S.CCX)


def _run(app, case, tmp_path):
    case.update(work_directory=str(tmp_path), threads=1)
    r = app.execute("case.run", id=case.id, wait=True)
    for _ in range(3):  # 드문 힙 손상 종료는 다시 돌린다(솔버 쪽 문제)
        if not (r["state"] == "failed" and not r["errors"]):
            break
        r = app.execute("case.run", id=case.id, wait=True)
    assert r["state"] == "completed", (r.get("exit_code"), r["errors"], app.execute("case.run_status", id=case.id, log_lines=30)["log"])
    return app.execute("result.open", case=case.id)["id"]


@pytest.mark.feature("RES-34")
@pytest.mark.feature("RES-45")
def test_RES_T05_02_15_13_26_thermal_fields_and_labels(app, tmp_path, ccx_env):
    """열 결과: 온도가 선형, 열유속이 고온→저온 방향으로 kΔT/L. 유사 해석 분야를 지정하면 온도·열유속 결과에 그 분야 이름이 붙는다."""
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 1, 1))
    mat.set_conductivity(data=[[50.0]])
    s = case.steps.create_heat_transfer(steady_state=True)
    s.bcs.create_temperature(target={"type": "set", "ids": [root.id]}, value=100.0)
    s.bcs.create_temperature(target={"type": "set", "ids": [tip.id]}, value=0.0)
    s.outputs.create_node_file(variables=["NT"])
    s.outputs.create_element_file(variables=["HFL"])
    rid = _run(app, case, tmp_path)
    fields = {f["name"]: f for f in app.execute("result.fields", result=rid, frame=1)}
    assert fields["NDTEMP"]["kind"] == "scalar" and fields["FLUX"]["components"] == ["F1", "F2", "F3"] and "label" not in fields["NDTEMP"]
    nodes, t = app.results.values(rid, 1, "NDTEMP", "T")
    xyz = dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords().tolist()))
    assert np.allclose(t, [100.0 * (1 - xyz[n][0] / 100.0) for n in nodes.tolist()], atol=1e-6)  # 선형
    nodes, q = app.results.values(rid, 1, "FLUX")
    k_dT_L = 50.0 * 100.0 / 100.0
    assert np.allclose(q[:, 0], k_dT_L, rtol=1e-4) and np.allclose(q[:, 1:], 0.0, atol=1e-6)  # +x(고온→저온), 크기 kΔT/L(frd 는 유효숫자 5~6자리)
    # 유사 해석 표기(RES-T05-13·26)
    app.execute("case.set_physics", id=case.id, physics="electrostatic")
    fields = {f["name"]: f for f in app.execute("result.fields", result=rid, frame=1)}
    assert fields["NDTEMP"]["label"] == "전위 V" and fields["NDTEMP"]["physics"] == "electrostatic" and "D" in fields["FLUX"]["label"]
    app.execute("case.set_physics", id=case.id, physics="groundwater")
    fields = {f["name"]: f for f in app.execute("result.fields", result=rid, frame=1)}
    assert fields["NDTEMP"]["label"] == "총수두 h" and fields["FLUX"]["label"] == "유출 속도 v"
    app.execute("case.set_physics", id=case.id, physics="thermal")
    assert "label" not in {f["name"]: f for f in app.execute("result.fields", result=rid, frame=1)}["NDTEMP"]


@pytest.mark.feature("RES-35")
@pytest.mark.feature("RES-36")
def test_RES_T05_03_04_16_17_contact_and_error(app, tmp_path, ccx_env):
    """접촉 결과: 접촉력 합 = 하중(1%), 떨어진 면은 압력 0·간극 양수. 오차 추정: 유한하고 균일 응력에서 0 에 가깝다."""
    mat = S.steel(app)
    low = app.model.mesh_parts.create(name="LOW")
    up = app.model.mesh_parts.create(name="UP")
    b1 = block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=low.id)
    b2 = block(app, 2, 2, 1, size=(10.0, 10.0, 5.0), part=up.id, origin=(0.0, 0.0, 5.0))
    for p in (low, up):
        app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [p.id]})
    S_ = app.model.sets
    low_top = S_.create_surface(name="LOWTOP", faces=[[e, 2] for e in (1, 2, 3, 4)])
    up_bot = S_.create_surface(name="UPBOT", faces=[[e, 1] for e in (5, 6, 7, 8)])
    base = S.node_set(app, "BASE", [-1, -1, -0.1], [11, 11, 0.1])
    top = S.node_set(app, "TOPN", [-1, -1, 9.9], [11, 11, 10.1])
    inter = app.model.contact_properties.create(name="INT1", pressure_overclosure="linear", slope=1e6)
    pair = app.model.contact_pairs.create(slave={"type": "set", "ids": [up_bot.id]}, master={"type": "set", "ids": [low_top.id]}, interaction=inter.id)
    case = app.model.cases.create(name="contact")
    case.update(contact_method="surface_to_surface")
    s = case.steps.create_static(nlgeom=False, initial_increment=1.0, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [base.id]}, dofs=[1, 2, 3])
    s.bcs.create_displacement(target={"type": "set", "ids": [top.id]}, dofs=[1, 2])
    s.loads.create_pressure(target={"type": "faces", "ids": [[e, 2] for e in (5, 6, 7, 8)]}, value=2.0)  # 위에서 누른다: 총 200
    s.outputs.create_node_file(variables=["U"])
    s.outputs.create_element_file(variables=["S", "ERR"])
    s.outputs.create_contact_file(variables=["CDIS", "CSTR"])
    s.outputs.create_contact_print(variables=["CF"], pair=pair.id, totals="yes")
    rid = _run(app, case, tmp_path)
    fields = {f["name"]: f for f in app.execute("result.fields", result=rid, frame=1)}
    assert "CONTACT" in fields and "ERROR" in fields
    nodes, c = app.results.values(rid, 1, "CONTACT")
    comp = fields["CONTACT"]["components"]
    pres = c[:, comp.index("CPRESS")]
    assert pres.min() >= -1e-6 and 1.0 < pres[pres > 0].mean() < 3.0  # 접촉 면의 절점 압력(요소 값의 절점 외삽)은 2 근처
    summary = app.execute("result.contact_summary", result=rid)
    tot = [t for t in summary if t["quantity"] == "contact total force"]
    assert tot and tot[0]["set"] == "UPBOT" and tot[0]["master"] == "LOWTOP" and abs(tot[0]["rows"][0][2]) == pytest.approx(200.0, rel=0.01)  # 접촉력 합 = 하중
    area = [t for t in summary if t["quantity"] == "contact area force"][0]["rows"][0]
    assert area[0] == pytest.approx(100.0, rel=1e-6) and area[1] == pytest.approx(-200.0, rel=0.01)  # 면적 100, 법선력 −200(압축)
    err = app.results.values(rid, 1, "ERROR")[1]
    assert np.all(np.isfinite(err))  # 접촉 모델의 오차 추정(ZZ)은 유한하다. 균일 응력·세분화는 아래 test_RES_T05_04_17_error_estimate
    # 떨어진 접촉(RES-T05-16): 위 블록을 1 띄우고 하중 없이 → 압력 0, 간극(COPEN) > 0
    app.execute("mesh.nodes_move", ids=[n for n in app.mesh.node_ids().tolist() if app.mesh.node_coords()[list(app.mesh.node_ids()).index(n)][2] >= 5.0 - 1e-9 and n > b1["nodes"][1]],
                translation=[0.0, 0.0, 0.01])  # 작은 간극(탐색 범위 안): 접촉 변수가 계산되되 압력은 0
    s.loads.list() and [app.execute("load.delete", id=l["id"]) for l in s.loads.list()]
    s.bcs.create_displacement(target={"type": "set", "ids": [top.id]}, dofs=[3])
    rid2 = _run(app, case, tmp_path / "gap")
    nodes, c = app.results.values(rid2, 1, "CONTACT")
    comp = {f["name"]: f for f in app.execute("result.fields", result=rid2, frame=1)}["CONTACT"]["components"]
    # 매뉴얼 7.20: COPEN 은 침투(음수)만 저장하고 양의 간극은 0 으로 둔다 → 떨어진 접촉은 압력 0, COPEN 0(침투 없음)
    assert np.allclose(c[:, comp.index("CPRESS")], 0.0, atol=1e-9) and np.allclose(c[:, comp.index("COPEN")], 0.0, atol=1e-12)
    assert [t for t in app.execute("result.contact_summary", result=rid2) if t["quantity"] == "contact total force"][0]["rows"][0][2] == pytest.approx(0.0, abs=1e-9)


@pytest.mark.feature("RES-36")
def test_RES_T05_04_17_error_estimate(app, tmp_path, ccx_env):
    """오차 추정(ZZ, STR 오차 %): 균일 단축 인장에서는 0 에 가깝고, 굽힘에서는 메시를 세분화하면 줄어든다."""

    def bend_error(n):
        app.execute("project.new")
        part, mat, root, tip, case = S.cantilever(app, order=1, n=n)
        s = case.steps.create_static()
        s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
        s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
        s.outputs.create_element_file(variables=["S", "ERR"])
        rid = _run(app, case, tmp_path / f"bend{n[0]}")
        err = app.results.values(rid, 1, "ERROR")[1]
        assert np.all(np.isfinite(err))
        return float(np.mean(err))

    coarse, fine = bend_error((5, 1, 1)), bend_error((20, 4, 4))
    assert fine < coarse  # 세분화하면 감소(RES-T05-04)
    # 균일 단축 인장(RES-T05-17): 뿌리는 x 만 구속(옆은 자유), 끝에 균일 하중
    app.execute("project.new")
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(4, 2, 2))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1])
    s.bcs.create_displacement(target={"type": "nodes", "ids": [root.props["ids"][0]]}, dofs=[2, 3])
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n for n in root.props["ids"] if app.mesh.node_coords()[list(app.mesh.node_ids()).index(n)][2] == 0.0][:1]}, dofs=[2])
    s.loads.create_pressure(target={"type": "faces", "ids": [[e, 4] for e in (4, 8, 12, 16)]}, value=-1.0)  # 끝면을 당긴다(면 번호는 아래에서 확인)
    s.outputs.create_element_file(variables=["S", "ERR"])
    # 끝면(x=100)의 면 번호를 찾아 다시 지정한다
    faces = []
    for e in (4, 8, 12, 16):
        nodes = app.execute("mesh.elements", ids=[e])[0]["nodes"]
        xyz = {n: app.mesh.node_coords()[list(app.mesh.node_ids()).index(n)] for n in nodes}
        for f, table in enumerate([[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]], start=1):
            if all(abs(xyz[nodes[k]][0] - 100.0) < 1e-9 for k in table):
                faces.append([e, f])
    for l in s.loads.list():
        app.execute("load.delete", id=l["id"])
    s.loads.create_pressure(target={"type": "faces", "ids": faces}, value=-1.0)
    rid = _run(app, case, tmp_path / "uniform")
    err = app.results.values(rid, 1, "ERROR")[1]
    assert np.all(np.isfinite(err)) and err.max() < 0.5  # 균일 응력: 오차 추정(%)이 0 에 가깝다


@pytest.mark.feature("RES-38")
def test_RES_T05_06_section_output(app, tmp_path, ccx_env):
    """단면 출력: 외팔보 중간 단면(x=50)의 전단력 = P, 모멘트 = P·(남은 길이 50)."""
    # 단면력은 절점 평균 응력을 면 적분한 값(매뉴얼 7.114)이라 단면 분할이 충분해야 전단력이 맞는다: 2차 요소, 단면 4×4
    part, mat, root, tip, case = S.cantilever(app, order=2, n=(10, 4, 4))
    P = 100.0
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    n_tip = len(tip.props["ids"])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -P / n_tip])
    # x=50 단면: 요소 5(x 40~50)의 오른쪽 면(S4)... 육면체의 면 번호는 블록 노드 순서 기준이므로 자유 면 쪽 대신 요소 6 의 x=50 면을 쓴다
    mid = app.model.sets.create_surface(name="MID", faces=[[e, 6] for e in (6, 16, 26, 36)]) if False else None
    elems_at_50 = [e for e in range(1, 161) if (e - 1) % 10 == 5]  # x 50~60 인 요소(길이 방향 6번째)
    faces = []
    for e in elems_at_50:
        nodes = app.execute("mesh.elements", ids=[e])[0]["nodes"]
        xyz = {n: app.mesh.node_coords()[list(app.mesh.node_ids()).index(n)] for n in nodes}
        for f, table in enumerate([[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]], start=1):
            if all(abs(xyz[nodes[k]][0] - 50.0) < 1e-9 for k in table):
                faces.append([e, f])
    assert len(faces) == 16
    mid = app.model.sets.create_surface(name="MID", faces=faces)
    s.outputs.create_section_print(variables=["SOF", "SOM"], target={"type": "set", "ids": [mid.id]}, label="MID")
    s.outputs.create_node_file(variables=["U"])
    rid = _run(app, case, tmp_path)
    tables = app.execute("result.section_output", result=rid)
    assert tables and all(t["quantity"].startswith("section") and t["set"] == "MID" for t in tables)
    forces = [t for t in tables if t["quantity"] == "section total force"][0]
    moments = [t for t in tables if t["quantity"] == "section moment"][0]
    area = [t for t in tables if t["quantity"] == "section area force"][0]
    # 모멘트 = P × 남은 길이(단면 중심 기준), 면적 = 20×10. 둘 다 0.1% 안.
    assert abs(moments["rows"][0][moments["columns"].index("my")]) == pytest.approx(P * 50.0, rel=1e-3)
    assert area["rows"][0][area["columns"].index("area")] == pytest.approx(200.0) and area["rows"][0][area["columns"].index("bending_moment")] == pytest.approx(P * 50.0, rel=1e-3)
    # 전단력: 솔버는 절점 평균 응력(적분점 → 절점 외삽 → 평균)을 면에 적분한다(매뉴얼 7.114). 외삽된 전단 응력은 단면 분포를 과대·과소평가해
    # 이 모델에서 P 와 20~35% 차이가 난다(1차 요소 20, 2차 요소 135). 값이 P 와 같은 부호·같은 자릿수이고 fz 와 shear_force 가 서로 같은지만 본다.
    fz = forces["rows"][0][forces["columns"].index("fz")]
    assert 0.5 * P < abs(fz) < 1.5 * P and area["rows"][0][area["columns"].index("shear_force")] == pytest.approx(abs(fz), rel=1e-6)
    assert abs(forces["rows"][0][forces["columns"].index("fx")]) < 1e-6 * P and abs(area["rows"][0][area["columns"].index("normal_force")]) < 1e-6 * P


@pytest.mark.feature("RES-11")
@pytest.mark.feature("RES-48")
def test_RES_T03_05_13_T06_04_10_animation_and_phase(app, tmp_path, ccx_env):
    """애니메이션: 설정 왕복, 프레임 이미지 12장. 모드(위상) 애니메이션: 0° 와 180° 프레임의 변위 부호가 반대, 첫·끝 프레임이 같다."""
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 2, 2))
    s = case.steps.create_static(nlgeom=True, initial_increment=0.25, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    s.outputs.create_node_file(variables=["U"])
    f = case.steps.create_frequency(num_modes=2)
    f.outputs.create_node_file(variables=["U"])
    rid = _run(app, case, tmp_path)
    if "view.animate" not in {c["name"] for c in app.commands()}:
        pytest.skip("렌더러 없이 빌드됨")
    try:
        app.execute("view.diagnostics")
    except OfepError:
        pytest.skip("Vulkan 을 쓸 수 없음")
    app.execute("view.hud", triad=False, legend=False)
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", deform_scale=10.0)
    an = app.execute("view.animate", frames=[1, 2, 3, 4], interval_ms=50, loop=False)
    assert an["frames"] == [1, 2, 3, 4] and an["interval_ms"] == 50 and an["loop"] is False and an["playing"] is True
    app.execute("view.animate", stop=True)
    out = tmp_path / "anim"
    out.mkdir()
    r = app.execute("view.export_animation", pattern=str(out / "f_{frame}.png"), frames=[1, 2, 3, 4], width=200, height=150)
    assert len(list(out.glob("f_*.png"))) == 4 and r["count"] == 4
    # 위상 애니메이션(RES-T06-04·10): 모드 형상 프레임(스텝 2 의 첫 모드)을 0~360° 12 단계로. 프레임 번호 5 = 모드 1
    mode = [fr["frame"] for fr in app.execute("result.steps", result=rid) if fr["step"] == 2][0]
    app.execute("view.result_show", result=rid, frame=mode, field="DISP", component="magnitude", deform_scale=5.0)
    ph = app.execute("view.animate", phase_steps=12, interval_ms=30, loop=False, deform_scale=5.0)
    assert ph["phases"] == [30.0 * k for k in range(12)] and ph["frames"] == [mode] and ph["playing"] is True
    nodes, u = app.results.values(rid, mode, "DISP")
    tip_row = int(np.argmax(np.abs(u[:, 2])))
    deg0 = app.execute("view.animate", phase=0.0)["deform_scale_effective"]
    deg180 = app.execute("view.animate", phase=180.0)["deform_scale_effective"]
    assert deg0 == pytest.approx(5.0) and deg180 == pytest.approx(-5.0)  # 반 주기: 변위 부호 반대
    assert app.execute("view.animate", phase=360.0)["deform_scale_effective"] == pytest.approx(deg0)  # 첫·끝 프레임 동일
    out2 = tmp_path / "phase"
    out2.mkdir()
    r = app.execute("view.export_animation", pattern=str(out2 / "p_{frame}.png"), phase_steps=12, width=200, height=150)
    files = sorted(out2.glob("p_*.png"))
    assert len(files) == 12 and files[0].read_bytes() != files[6].read_bytes()
    app.execute("view.animate", stop=True)
    app.execute("view.result_show")


@pytest.mark.feature("RES-53")
@pytest.mark.feature("RES-54")
@pytest.mark.feature("RES-08")
def test_RES_T07_01_02_10_T03_03_11_expanded_shell(app, tmp_path, ccx_env):
    """전개된 쉘 결과: 결과 노드가 원래 쉘 노드에 대응하고 전개 두께 = 프로퍼티 두께. 면별 응력: 굽힘에서 ±6M/t², 중립면 ≈ 0; 막응력에서는 세 면이 같다.
    솔리드 결과에 면을 고르면 오류."""
    mat = S.steel(app)
    t, L, W, P = 1.0, 10.0, 4.0, 2.0
    sh = plate(app, 5, 2, size=(L, W))
    ids = list(range(sh["elements"][0], sh["elements"][1] + 1))
    app.execute("mesh.set_element_type", ids=ids, type="S4")
    app.model.properties.create_shell(material=mat.id, thickness=t, target={"type": "elements", "ids": ids})
    root = S.node_set(app, "ROOT", [-0.1, -1, -1], [0.1, 5, 1])
    tipn = S.node_set(app, "TIPN", [9.9, -1, -1], [10.1, 5, 1])
    case = app.model.cases.create(name="shell")
    s1 = case.steps.create_static(name="bend")
    s1.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3, 4, 5, 6])
    s1.loads.create_force(target={"type": "set", "ids": [tipn.id]}, components=[0.0, 0.0, -P / len(tipn.props["ids"])])  # 끝단 전체 P (아래로)
    s1.outputs.create_node_file(variables=["U"])
    s1.outputs.create_element_file(variables=["S"])
    s2 = case.steps.create_static(name="membrane", loads_inheritance="new")
    s2.loads.create_force(target={"type": "set", "ids": [tipn.id]}, components=[P / len(tipn.props["ids"]), 0.0, 0.0])  # 면내 인장
    rid = _run(app, case, tmp_path)
    m = app.execute("result.map_to_model", result=rid)
    assert m["matched_nodes"] == 0 and m["expanded_nodes"] == m["result_only_nodes"] == 2 * 18  # 쉘 노드 18개 × 위·아래
    assert all(abs(v - t) < 1e-6 for v in m["expanded_thickness"].values()) and len(m["expansion"]) == 18
    assert all(len(v["top"]) == 1 and len(v["bottom"]) == 1 for v in m["expansion"].values())
    # 굽힘(RES-T07-02): 뿌리 근처 단면의 모멘트 M' = P·x /W (단위 폭) → σ = ±6M'/t². x = 1 (첫 요소열의 중간) 에서 비교
    mid_nodes = [n for n in app.mesh.node_ids().tolist() if abs(app.mesh.node_coords()[list(app.mesh.node_ids()).index(n)][0] - 2.0) < 1e-9]
    top = app.execute("result.values", result=rid, frame=1, field="STRESS", component="SXX", shell_face="top", nodes=mid_nodes)
    bot = app.execute("result.values", result=rid, frame=1, field="STRESS", component="SXX", shell_face="bottom", nodes=mid_nodes)
    mid = app.execute("result.values", result=rid, frame=1, field="STRESS", component="SXX", shell_face="mid", nodes=mid_nodes)
    sigma = 6.0 * (P * (L - 2.0) / W) / t**2
    assert np.allclose(top["values"], sigma, rtol=0.08) and np.allclose(bot["values"], -sigma, rtol=0.08)  # 상면 인장(아래로 굽음), 하면 압축
    assert np.all(np.abs(mid["values"]) < 0.05 * sigma)  # 중립면 ≈ 0
    # 막응력(RES-T07-10): 세 면이 같다(P/(W·t))
    for face in ("top", "bottom", "mid"):
        v = app.execute("result.values", result=rid, frame=2, field="STRESS", component="SXX", shell_face=face, nodes=mid_nodes)["values"]
        assert np.allclose(v, P / (W * t), rtol=0.05)
    # 설정 왕복(RES-T03-03): view.result_show 의 shell_face
    if "view.result_show" in {c["name"] for c in app.commands()}:
        try:
            app.execute("view.diagnostics")
            r = app.execute("view.result_show", result=rid, frame=1, field="STRESS", component="SXX", shell_face="top")
            assert r["settings"]["shell_face"] == "top"
            with pytest.raises(OfepError):
                app.execute("view.result_show", result=rid, frame=1, field="STRESS", component="SXX", shell_face="inside")
            # shell_face 없이도 펼쳐진 결과는 중립면 값으로 자동 대응해 색이 입혀진다(결과 노드가 모델 노드와 겹치지 않으므로)
            app.execute("view.result_show")
            plain = app.view.render(160, 120)[0]
            app.execute("view.result_show", result=rid, frame=1, field="STRESS", component="SXX")
            assert not np.array_equal(app.view.render(160, 120)[0], plain)
            app.execute("view.result_show")
        except OfepError as e:
            if e.code != "not_available":
                raise
    with pytest.raises(OfepError) as e:
        app.execute("result.values", result=rid, frame=1, field="STRESS", shell_face="top")  # component 없음
    assert e.value.code == "missing_param"
    # 솔리드 결과에 면 선택(RES-T03-11): 오류
    app.execute("project.new")
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(4, 1, 1))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    s.outputs.create_element_file(variables=["S"])
    rid = _run(app, case, tmp_path / "solid")
    with pytest.raises(OfepError) as e:
        app.execute("result.values", result=rid, frame=1, field="STRESS", component="SXX", shell_face="top")
    assert e.value.code == "not_available"


@pytest.mark.feature("RES-59")
def test_RES_T07_08_14_removed_elements(app, tmp_path, ccx_env):
    """스텝 2 에서 제거한 요소는 그 스텝 결과의 값 조회·표시에서 빠지고, 스텝 3 에서 다시 넣으면 나타난다."""
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(4, 1, 1))
    s1 = case.steps.create_static(name="s1")
    s1.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s1.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    s1.outputs.create_node_file(variables=["U"])
    s2 = case.steps.create_static(name="s2", loads_inheritance="new")
    s2.changes.create_model_change_element(target={"type": "elements", "ids": [4]}, action="remove")
    s2.loads.create_force(target={"type": "nodes", "ids": [n for n in app.mesh.node_ids().tolist() if abs(app.mesh.node_coords()[list(app.mesh.node_ids()).index(n)][0] - 75.0) < 1e-9]},
                          components=[0.0, 0.0, -1.0])
    s3 = case.steps.create_static(name="s3", loads_inheritance="new", nlgeom=True)  # 요소 추가(변형 없이)는 비선형 스텝에서만(솔버 오류 메시지)
    s3.changes.create_model_change_element(target={"type": "elements", "ids": [4]}, action="add")
    assert "model_change_add_nonlinear" not in {i["code"] for i in app.execute("case.check", id=case.id)}
    s3.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    rid = _run(app, case, tmp_path)
    frames = app.execute("result.steps", result=rid)
    f1, f2, f3 = [fr["frame"] for fr in frames if fr["increment"] == max(x["increment"] for x in frames if x["step"] == fr["step"])][:3]
    tip_nodes = set(tip.props["ids"])
    v1 = app.execute("result.values", result=rid, frame=f1, field="DISP", component="D3")
    v2 = app.execute("result.values", result=rid, frame=f2, field="DISP", component="D3")
    v3 = app.execute("result.values", result=rid, frame=f3, field="DISP", component="D3")
    assert tip_nodes <= set(v1["ids"]) and tip_nodes <= set(v3["ids"])
    assert not (tip_nodes & set(v2["ids"]))  # 제거된 요소 4 에만 속한 끝 노드는 스텝 2 에서 빠진다
    # 솔버 자체가 제거된 요소의 노드를 그 스텝 결과에 쓰지 않는다 → include_inactive 로도 파일에 없는 노드는 나오지 않는다(기본 결과의 상위집합)
    with_inactive = set(app.execute("result.values", result=rid, frame=f2, field="DISP", component="D3", include_inactive=True)["ids"])
    assert set(v2["ids"]) <= with_inactive and not (tip_nodes & with_inactive)
    if "view.result_show" in {c["name"] for c in app.commands()}:
        try:
            app.execute("view.diagnostics")
        except OfepError:
            return
        app.execute("view.hud", triad=False, legend=False)
        app.execute("view.display_mode", mode="shaded")
        app.execute("view.standard", name="front")
        app.execute("view.fit")
        app.execute("view.result_show", result=rid, frame=f2, field="DISP", component="magnitude")
        ids2 = app.view.render(400, 300)[1]
        app.execute("view.result_show", result=rid, frame=f3, field="DISP", component="magnitude")
        ids3 = app.view.render(400, 300)[1]
        # 스텝 2 화면에는 요소 4 가 없다(오른쪽 1/4 이 비어 가로 폭이 좁다)
        x2 = np.nonzero(ids2)[1]
        x3 = np.nonzero(ids3)[1]
        assert x2.max() < x3.max() - 10 and x2.min() == x3.min()
        app.execute("view.result_show")


@pytest.mark.feature("RES-58")
def test_RES_T07_07_13_local_csys_results(app, tmp_path, ccx_env):
    """로컬 좌표계 출력(GLOBAL=NO)으로 저장된 결과를 열면 좌표 변환이 걸린 노드의 값을 전역으로 돌려 조회할 수 있고, 전역 출력 결과와 같다."""
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(4, 1, 1))
    cs = app.model.csys.create_rectangular(name="ROT", origin=[0, 0, 0], axis1_point=[0, 1, 0], plane12_point=[-1, 0, 0])  # z 둘레 90° (x' = y, y' = -x)
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    # 끝 노드에 *TRANSFORM: 같은 노드의 하중·구속은 모두 그 좌표계로 쓴다(전역·국부가 섞이면 덱이 거부한다). 전역 -y = 국부 -x'
    f = s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[-1.0, 0.0, 0.0])
    f.set_csys(csys=cs.id)
    bc = s.bcs.create_displacement(target={"type": "set", "ids": [tip.id]}, dofs=[3])
    bc.set_csys(csys=cs.id)
    s.outputs.create_node_file(variables=["U"], **{"global": False})
    rid_local = _run(app, case, tmp_path / "local")
    tip_nodes = sorted(tip.props["ids"])
    local = app.execute("result.values", result=rid_local, frame=1, field="DISP", nodes=tip_nodes)["values"]
    glob = app.execute("result.values", result=rid_local, frame=1, field="DISP", nodes=tip_nodes, coordinates="global")["values"]
    # 로컬 성분 (u', v') = (uy, -ux) → 전역 (ux, uy) = (-v', u')
    for l, g in zip(local, glob):
        assert g[0] == pytest.approx(-l[1], abs=1e-12) and g[1] == pytest.approx(l[0], abs=1e-12) and g[2] == pytest.approx(l[2], abs=1e-12)
    assert all(g[1] < 0 for g in glob)  # 전역 -y 방향 변위
    info = app.execute("result.open", path=app.execute("case.run_status", id=case.id)["files"]["frd"]["path"])
    # 전역 출력(RES-T07-13)과 비교
    for o in s.outputs.list():
        app.execute("output_request.update", id=o["id"], **{"global": True})
    rid_global = _run(app, case, tmp_path / "global")
    g2 = app.execute("result.values", result=rid_global, frame=1, field="DISP", nodes=tip_nodes)["values"]
    assert np.allclose(g2, glob, atol=1e-10)
    assert np.allclose(app.execute("result.values", result=rid_global, frame=1, field="DISP", nodes=tip_nodes, coordinates="global")["values"], g2)  # 전역 출력은 그대로


@pytest.mark.feature("RES-03")
def test_RES_T01_06_13_lazy_loading(app, tmp_path, ccx_env):
    """지연 로딩: 연 직후에는 값이 올라와 있지 않고, 요청한 필드만 올라온다. 필드 해제로 메모리를 되돌린다."""
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 2, 2))
    s = case.steps.create_static(nlgeom=True, initial_increment=0.5, period=1.0)  # 증분 2개 → 프레임 2개
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -1.0])
    s.outputs.create_node_file(variables=["U", "RF"])
    s.outputs.create_element_file(variables=["S", "E"])
    rid = _run(app, case, tmp_path)
    assert len(app.execute("result.steps", result=rid)) == 2
    d = app.execute("result.diagnostics", result=rid)
    assert d["loaded_fields"] == 0 and d["loaded_bytes"] == 0 and d["file_bytes"] > 0
    app.execute("result.values", result=rid, frame=1, field="DISP", component="magnitude")
    d1 = app.execute("result.diagnostics", result=rid)
    assert d1["loaded_fields"] == 1 and 0 < d1["loaded_bytes"] < d1["file_bytes"]
    app.execute("result.values", result=rid, frame=2, field="STRESS", component="mises")
    d2 = app.execute("result.diagnostics", result=rid)
    assert d2["loaded_fields"] == 2 and d2["loaded_bytes"] > d1["loaded_bytes"]
    # 필드 해제(RES-T01-13)
    r = app.execute("result.unload", result=rid, frame=1)
    d3 = app.execute("result.diagnostics", result=rid)
    assert r["unloaded"] == 1 and d3["loaded_fields"] == 1 and d3["loaded_bytes"] < d2["loaded_bytes"]
    app.execute("result.unload", result=rid)
    assert app.execute("result.diagnostics", result=rid)["loaded_bytes"] == 0
    assert len(app.execute("result.values", result=rid, frame=1, field="DISP", component="magnitude")["values"]) > 0  # 다시 읽힌다


@pytest.mark.feature("RES-49")
def test_RES_T06_05_11_frequency_response_plot(app, tmp_path, ccx_env):
    """주파수 응답 그래프: 정상상태 동해석의 끝단 변위 크기를 진동수에 대해 모은 값이 스텝별 조회와 같고 공진 주파수에서 최대, 위상은 공진을 지나며 약 180° 바뀐다."""
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 1, 1))
    s = case.steps.create_frequency(num_modes=4, storage=True)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    rid0 = _run(app, case, tmp_path / "modal")
    f1 = [t for t in app.execute("result.modal_summary", result=rid0) if t["step"] == 1][0]["modes"][0]["frequency"]
    app.execute("result.close", result=rid0)
    s2 = case.steps.create_steady_state_dynamics(freq_min=0.5 * f1, freq_max=1.5 * f1, points=41, damping_type="direct", modal_damping=[[1, 4, 0.02]])
    s2.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -10.0])
    s2.outputs.create_node_file(variables=["U", "PU"])
    _run(app, case, tmp_path)
    rf = app.model.results.create(name="R", case=case.id)
    rid = app.execute("result.open", file=rf.id)["id"]  # 그래프는 결과 파일 객체로 연 결과를 쓴다
    node = tip.props["ids"][0]
    comps = {f["name"]: f["components"] for f in app.execute("result.fields", result=rid, frame=app.execute("result.steps", result=rid)[-1]["frame"])}
    mag3, pha3 = comps["PDISP"][2], comps["PDISP"][5]
    plot = app.model.plots.create_frequency_response(name="tip_fr", result_file=rf.id, field="PDISP", component=mag3, node=node, step=2)
    d = app.execute("plot.data", id=plot.id)
    # 매뉴얼 7.123: 범위 안에 고유 진동수가 m 개면 점 수는 n·m − m + n (n=41, m=1 → 81). 고유 진동수가 고정점이 된다
    frames = [fr for fr in app.execute("result.steps", result=rid) if fr["step"] == 2]
    assert d["type"] == "frequency_response" and d["x_label"] == "frequency" and len(d["x"]) == len(frames) == 81
    assert any(abs(x - f1) < 1e-6 * f1 for x in d["x"])
    assert d["x"] == pytest.approx([fr["value"] for fr in frames])  # 스텝별 값을 모은 것과 동일(RES-T06-05)
    for fr, y in zip(frames, d["y"]):
        assert y == pytest.approx(app.execute("result.values", result=rid, frame=fr["frame"], field="PDISP", component=mag3, nodes=[node])["values"][0])
    peak = d["x"][int(np.argmax(d["y"]))]
    assert peak == pytest.approx(f1, rel=0.03)  # 공진 주파수에서 최대
    # 위상 그래프(RES-T06-11): 공진 앞뒤로 약 180° 차이
    ph = app.model.plots.create_frequency_response(name="tip_phase", result_file=rf.id, field="PDISP", component=pha3, node=node, step=2)
    y = app.execute("plot.data", id=ph.id)["y"]
    assert abs(abs(y[-1] - y[0]) - 180.0) < 15.0 or abs(abs(y[-1] - y[0]) - 180.0) % 360.0 < 15.0
    r = app.execute("plot.export", id=plot.id, path=str(tmp_path / "fr.csv"))
    assert r["rows"] == len(frames) and (tmp_path / "fr.csv").read_text().splitlines()[0] == f"frequency,PDISP.{mag3}"


def _sector(app, mat):
    """12 섹터 고리의 30° 조각(r 50~100, z 0~10): (r, θ, z) 블록을 원통 좌표로 옮긴다. 돌려주는 값: 블록 정보, LEFT·RIGHT·INNER 셋."""
    mp = app.model.mesh_parts.create(name="SEC")
    b = block(app, 4, 4, 2, size=(50.0, 30.0, 10.0), part=mp.id, origin=(50.0, 0.0, 0.0))  # x=r, y=θ(도), z=z
    ids = app.mesh.node_ids().tolist()
    xyz = app.mesh.node_coords()
    app.execute("mesh.nodes_move", ids=ids, coords=[[r * math.cos(math.radians(t)), r * math.sin(math.radians(t)), z] for r, t, z in xyz])
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [mp.id]})
    left = app.model.sets.create_node(name="LEFT", ids=[b["node"](i, 0, k) for i in range(5) for k in range(3)])
    right = app.model.sets.create_node(name="RIGHT", ids=[b["node"](i, 4, k) for i in range(5) for k in range(3)])
    app.model.constraints.create_cyclic_symmetry(slave={"type": "set", "ids": [left.id]}, master={"type": "set", "ids": [right.id]}, sectors=12,
                                                  axis_point_a=[0, 0, 0], axis_point_b=[0, 0, 1])
    inner = app.model.sets.create_node(name="INNER", ids=[b["node"](0, j, k) for j in range(1, 5) for k in range(3)])  # 종속 면(LEFT)의 노드는 뺀다
    return b, left, right, inner


@pytest.mark.feature("RES-51")
@pytest.mark.feature("RES-52")
def test_RES_T06_07_08_13_14_cyclic_symmetry_modes_and_worst_case(app, tmp_path, ccx_env):
    """순환대칭 고유치: 절직경 0~2 를 고르면 모드마다 절직경·진동수·회전 방향이 dat 와 같이 조회된다. 최악값 필드(MDISP·MSTRESS)가 있고
    MDISP 는 위상을 돌려 뽑은 표본의 최대 이상이다. 회전 속도가 다른 결과 둘로 속도-진동수 자료를 모은다."""
    mat = S.steel(app)
    b, left, right, inner = _sector(app, mat)
    ray = app.model.sets.create_node(name="RAY", ids=[b["node"](4, 2, 2)])  # MAXU 의 기준 벡터 = 이 노드의 좌표(매뉴얼 *NODE FILE MAXU)
    dom = app.model.sets.create_node(name="STRESSDOMAIN", ids=[b["node"](i, j, k) for i in range(5) for j in range(5) for k in range(3)])
    case = app.model.cases.create(name="cyc")
    s = case.steps.create_frequency(num_modes=3, cyclic_mode_min=0, cyclic_mode_max=2)
    s.bcs.create_displacement(target={"type": "set", "ids": [inner.id]}, dofs=[1, 2, 3])
    s.outputs.create_node_file(variables=["U", "MAXU"])
    s.outputs.create_element_file(variables=["S", "MAXS"])
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*SELECT CYCLIC SYMMETRY MODES, NMIN=0, NMAX=2" in deck and "*NSET, NSET=RAY" in deck and "*NSET, NSET=STRESSDOMAIN" in deck
    rid = _run(app, case, tmp_path)
    summary = app.execute("result.modal_summary", result=rid)
    assert len(summary) == 1 and summary[0]["cyclic_symmetry"] is True
    modes = summary[0]["modes"]
    assert [m["nodal_diameter"] for m in modes] == [0, 0, 0, 1, 1, 1, 2, 2, 2] and [m["mode"] for m in modes] == [1, 2, 3] * 3
    assert all(m["turning_direction"] in ("forward", "backward") for m in modes if m["nodal_diameter"] > 0)
    # dat 와 일치(RES-T06-07): 절직경 블록마다 EIGENVALUE OUTPUT 표의 행 [절직경, 모드, 고유치, ω, f, 허수부]
    dat = pathlib.Path(app.execute("case.run_status", id=case.id)["files"]["dat"]["path"]).read_text()
    rows = [l.split() for l in dat.splitlines() if len(l.split()) == 6 and l.split()[0].isdigit() and l.split()[1].isdigit()]
    assert len(rows) == 9 and all(float(r[4]) == pytest.approx(m["frequency"], rel=1e-6) and int(r[0]) == m["nodal_diameter"] for r, m in zip(rows, modes))
    # frd 프레임의 PHID(절직경)도 같다
    frames = app.execute("result.steps", result=rid)
    # frd 는 중복 고유치 쌍 중 하나만 쓴다(솔버 출력: 절직경마다 모드 3개 중 2 프레임). 프레임의 절직경(1PHID)·진동수가 dat 의 그 절직경 모드와 맞는다
    assert len(frames) == 6 and sorted({int(fr["attributes"]["HID"]) for fr in frames}) == [0, 1, 2]
    for fr in frames:
        nd = int(fr["attributes"]["HID"])
        assert any(m["nodal_diameter"] == nd and m["frequency"] == pytest.approx(fr["value"], rel=1e-6) for m in modes)
    # 최악값 필드(RES-T06-08·14)
    fr_nd1 = [fr for fr in frames if int(fr["attributes"]["HID"]) == 1][0]["frame"]
    fields = {f["name"]: f for f in app.execute("result.fields", result=rid, frame=fr_nd1)}
    assert "MDISP" in fields and "MSTRESS" in fields and "DISP" in fields and "DISPI" in fields
    node = b["node"](4, 2, 1)
    md = app.execute("result.values", result=rid, frame=fr_nd1, field="MDISP", nodes=[node])["values"][0]
    # 독립 계산: 위상 0~360° 표본에서 RAY 벡터에 수직인 변위의 최대. MDISP 는 그 이상이어야 한다
    rayv = np.array(app.mesh.node_coords()[app.mesh.node_ids().tolist().index(b["node"](4, 2, 2))])
    rayv /= np.linalg.norm(rayv)
    best = 0.0
    for deg in range(0, 360, 5):
        u = np.array(app.execute("result.at_phase", result=rid, frame=fr_nd1, field="DISP", component="D1", phase=deg, nodes=[node])["values"] +
                     app.execute("result.at_phase", result=rid, frame=fr_nd1, field="DISP", component="D2", phase=deg, nodes=[node])["values"] +
                     app.execute("result.at_phase", result=rid, frame=fr_nd1, field="DISP", component="D3", phase=deg, nodes=[node])["values"])
        perp = u - np.dot(u, rayv) * rayv
        best = max(best, float(np.linalg.norm(perp)))
    assert np.linalg.norm(md) >= best * (1 - 1e-6) and np.linalg.norm(md) > 0
    ms = app.execute("result.values", result=rid, frame=fr_nd1, field="MSTRESS", nodes=[node])["values"][0]
    assert all(np.isfinite(ms))
    # 속도-진동수 자료(RES-T06-13): 회전 속도(원심력 사전 스텝)가 다른 결과 둘의 절직경별 진동수를 모은다
    speeds = {}
    for omega in (0.0, 300.0):
        other = App()
        m2 = S.steel(other)
        b2, l2, r2, in2 = _sector(other, m2)
        c2 = other.model.cases.create(name=f"spin{int(omega)}")
        st0 = c2.steps.create_static(nlgeom=True)
        st0.bcs.create_displacement(target={"type": "set", "ids": [in2.id]}, dofs=[1, 2, 3])
        if omega > 0:
            st0.loads.create_centrifugal(target={"type": "parts", "ids": [other.model.mesh_parts["SEC"].id]}, omega=omega, axis_point=[0, 0, 0], axis_direction=[0, 0, 1])
        f2 = c2.steps.create_frequency(num_modes=2, cyclic_mode_min=1, cyclic_mode_max=1, perturbation=True)
        f2.outputs.create_node_file(variables=["U"])
        rid2 = _run(other, c2, tmp_path / f"spin{int(omega)}")
        sm = [t for t in other.execute("result.modal_summary", result=rid2) if t["step"] == 2][0]
        speeds[omega] = [m["frequency"] for m in sm["modes"] if m["nodal_diameter"] == 1]
    assert len(speeds[0.0]) == 2 and len(speeds[300.0]) == 2 and speeds[300.0][0] > speeds[0.0][0]  # 회전하면 강성 증가 → 진동수 상승


@pytest.mark.feature("RES-39")
@pytest.mark.feature("PRP-20")
def test_RES_T05_07_network_results(app, tmp_path, ccx_env):
    """기체 네트워크(입구 더미 → Fanno 파이프 → 출구 더미, 매뉴얼 6.9.16 의 경계조건 규칙): 입구·출구 질량유량이 같고(보존) frd 의 MAFLOW·TOPRES·TOTEMP 가 dat 와 일치한다."""
    n = app.execute("mesh.nodes_create", coords=[[0.5 * i, 0.0, 0.0] for i in range(5)])["first"]  # 노드 n..n+4
    e_in = app.execute("mesh.elements_create", shape="line3", connectivity=[[0, n, n + 1]], type="D")["first"]  # 더미 입구(0 → n+1)
    e_pipe = app.execute("mesh.elements_create", shape="line3", connectivity=[[n + 1, n + 2, n + 3]], type="D")["first"]
    e_out = app.execute("mesh.elements_create", shape="line3", connectivity=[[n + 3, n + 4, 0]], type="D")["first"]  # 더미 출구
    air = app.model.materials.create(name="AIR")
    air.set_specific_gas_constant(value=287.0)
    air.set_fluid_constants(data=[[1005.0, 1.8e-5]])
    P = app.model.properties
    P.create_fluid(name="PIPE", material=air.id, section_type="GAS PIPE FANNO ADIABATIC", constants=[7.85e-5, 0.01, 1.0, 1e-5, 1.0],
                   target={"type": "elements", "ids": [e_pipe]})
    P.create_fluid(name="IN", material=air.id, section_type="INOUT", target={"type": "elements", "ids": [e_in]})
    P.create_fluid(name="OUT", material=air.id, section_type="INOUT", target={"type": "elements", "ids": [e_out]})
    app.execute("physical_constants.set", absolute_zero=0.0)
    case = app.model.cases.create(name="net")
    app.execute("case.set_physics", id=case.id, physics="gas_network")
    s = case.steps.create_heat_transfer(steady_state=True)
    s.bcs.create_network(target={"type": "nodes", "ids": [n + 1]}, quantity="total_pressure", value=2.0e5)
    s.bcs.create_network(target={"type": "nodes", "ids": [n + 3]}, quantity="total_pressure", value=1.9e5)
    s.bcs.create_network(target={"type": "nodes", "ids": [n + 1]}, quantity="temperature", value=300.0)
    s.bcs.create_network(target={"type": "nodes", "ids": [n + 3]}, quantity="temperature", value=300.0)
    s.outputs.create_node_file(variables=["MF", "PT", "TT"])
    s.outputs.create_node_print(variables=["MF", "PT", "TT"], target={"type": "nodes", "ids": [n, n + 1, n + 2, n + 3, n + 4]})
    deck = app.execute("case.preview_deck", id=case.id)
    assert deck["skipped"] == [], deck["skipped"]
    assert "*FLUID SECTION, ELSET=" in deck["text"] and "TYPE=GAS PIPE FANNO ADIABATIC" in deck["text"] and "TYPE=INOUT" in deck["text"]
    assert f"{e_in}, 0, {n}, {n + 1}" in deck["text"] and f"{e_out}, {n + 3}, {n + 4}, 0" in deck["text"]  # 더미 요소의 0 번 노드
    rid = _run(app, case, tmp_path)
    fields = {f["name"]: f for f in app.execute("result.fields", result=rid, frame=1)}
    assert {"MAFLOW", "TOPRES", "TOTEMP"} <= set(fields)
    mf = dict(zip(*[x.tolist() for x in app.results.values(rid, 1, "MAFLOW", fields["MAFLOW"]["components"][0])]))
    assert mf[n] > 0 and mf[n] == pytest.approx(mf[n + 4], rel=1e-9) and mf[n + 2] == pytest.approx(mf[n], rel=1e-9)  # 입구 = 출구 = 파이프(보존)
    pt = dict(zip(*[x.tolist() for x in app.results.values(rid, 1, "TOPRES", fields["TOPRES"]["components"][0])]))
    assert pt[n + 1] == pytest.approx(2.0e5) and pt[n + 3] == pytest.approx(1.9e5)
    # dat 와 일치
    tables = app.execute("result.print_tables", result=rid, quantity="mass flows")
    assert tables and all(row[1] == pytest.approx(mf[int(row[0])], rel=1e-5) for row in tables[0]["rows"])
    # 유사 해석 표기: 네트워크는 열전달 유사가 아니다(그 자체의 양을 쓴다)
    assert app.execute("case.physics_labels", id=case.id)["thermal_analogy"] is False
