"""자동 메싱(6단계): 형상 → 사면체·삼각형 메시, 형상-메시 연관, 형상 대상의 전개, 재메싱.

케이스 정의: .agent/tests/tc-02-mesh.md (MSH-T02, T03). 형상 커널·메셔 없이 빌드했으면 건너뛴다.
"""
import math

import numpy as np
import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total

_version = App().execute("app.version")
pytestmark = pytest.mark.skipif(not _version.get("geometry") or not _version.get("mesher"), reason="형상 커널 또는 자동 메셔 없이 빌드됨")


def box(app, name="BOX", size=(10.0, 20.0, 30.0)):
    part = app.model.parts.create(name=name)
    part.features.create_box(size=list(size))
    return part


def coords(app):
    return dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords()))


def face_area(app, pairs, xyz):
    """요소면들의 넓이(꼭짓점 기준)."""
    table = {"tet4": [[0, 1, 2], [0, 3, 1], [1, 3, 2], [2, 3, 0]]}
    elems = {e["id"]: e for e in app.execute("mesh.elements", ids=sorted({p[0] for p in pairs}))}
    area = 0.0
    for e, f in pairs:
        el = elems[e]
        a, b, c = (xyz[el["nodes"][k]] for k in table["tet4"][f - 1])
        area += 0.5 * np.linalg.norm(np.cross(b - a, c - a))
    return area


# ================================================================ MSH-T02 사면체 메싱과 연관
@pytest.mark.feature("MSH-07")
@pytest.mark.feature("MSH-09")
def test_MSH_T02_tet_mesh_and_association(app):
    p = box(app)
    hist = history_len(app)
    r = app.execute("mesh.generate", id=p.id, size=5.0)
    assert history_len(app) == hist + 1 and r["shape"] == "tet4" and r["mesher"] == "netgen"
    s = app.execute("mesh.statistics")
    assert s["volume"] == pytest.approx(6000.0, rel=1e-9) and s["by_type"] == {"C3D4": r["elements"]} and s["nodes"] == r["nodes"]
    assert app.execute("mesh.check")["ok"] and app.execute("mesh.free_faces")["count"] > 0
    q = app.execute("mesh.quality")["worst"]
    assert q["jacobian"] > 0 and q["aspect"] < 10
    mp = app.execute("mesh_part.get", id=r["mesh_part"])
    assert mp["name"] == "BOX" and mp["props"]["geometry"] == p.id
    assert {e["part"] for e in app.execute("mesh.elements")} == {r["mesh_part"]}
    xyz = coords(app)
    # 면: 그 면의 요소면 넓이의 합 = 형상 면의 넓이, 노드는 모두 그 면 위
    total_faces = 0
    for f in app.execute("geometry.entities", id=p.id, type="face")["entities"]:
        a = app.execute("mesh.association", id=p.id, type="face", index=f["index"])
        assert face_area(app, a["faces"], xyz) == pytest.approx(f["area"], rel=1e-9)
        lo, hi = np.array(f["bbox"]["min"]) - 1e-6, np.array(f["bbox"]["max"]) + 1e-6
        assert all(np.all(xyz[n] >= lo) and np.all(xyz[n] <= hi) for n in a["nodes"])
        total_faces += len(a["faces"])
    assert total_faces == app.execute("mesh.free_faces")["count"]  # 모든 표면 요소면이 어느 면엔가 속한다
    # 모서리: 노드가 그 모서리 위에 있고, 요소 크기에 맞는 개수
    for e in app.execute("geometry.entities", id=p.id, type="edge")["entities"]:
        nodes = app.execute("mesh.association", id=p.id, type="edge", index=e["index"])["nodes"]
        lo, hi = np.array(e["bbox"]["min"]) - 1e-6, np.array(e["bbox"]["max"]) + 1e-6
        assert all(np.all(xyz[n] >= lo) and np.all(xyz[n] <= hi) for n in nodes)
        assert len(nodes) >= e["length"] / 5.0 + 1 - 1e-9
    # 꼭짓점
    for v in range(1, 9):
        info = app.execute("geometry.entity_info", id=p.id, type="vertex", index=v)
        node, = app.execute("mesh.association", id=p.id, type="vertex", index=v)["nodes"]
        assert xyz[node] == pytest.approx(info["point"], abs=1e-9)
    solid = app.execute("mesh.association", id=p.id, type="solid", index=1)
    assert len(solid["elements"]) == r["elements"] and len(solid["nodes"]) == r["nodes"]
    # 한 번에 되돌린다
    app.undo()
    assert app.execute("project.info")["nodes"] == 0 and app.execute("mesh_part.list") == []


@pytest.mark.feature("MSH-07")
@pytest.mark.feature("MSH-04")
def test_MSH_T02_second_order_and_sizes(app):
    part = app.model.parts.create(name="CYL")
    part.features.create_cylinder(radius=10.0, height=30.0)
    coarse = app.execute("mesh.generate", id=part.id, size=8.0)
    v1 = app.execute("mesh.statistics")["volume"]
    r = app.execute("mesh.generate", id=part.id, size=8.0, order=2, element_type="C3D10")
    assert r["shape"] == "tet10" and r["elements"] == coarse["elements"] and r["mesh_part"] == coarse["mesh_part"]  # 같은 메시 파트를 다시 쓴다
    assert app.execute("mesh.statistics")["by_type"] == {"C3D10": r["elements"]} and app.execute("mesh.check")["ok"]
    # 2차 요소의 중간 절점은 곡면 위에 있다: 원통 면의 모든 노드가 반지름 10
    xyz = coords(app)
    faces = app.execute("geometry.entities", id=part.id, type="face")["entities"]
    cyl = [f["index"] for f in faces if f["surface"] == "cylinder"][0]
    nodes = app.execute("mesh.association", id=part.id, type="face", index=cyl)["nodes"]
    radius = [math.hypot(xyz[n][0], xyz[n][1]) for n in nodes]
    assert min(radius) == pytest.approx(10.0, abs=1e-6) and max(radius) == pytest.approx(10.0, abs=1e-6)
    # 2차 요소의 변 순서가 맞다: 중간 절점이 그 변의 두 꼭짓점 사이에 있다
    edges = [(0, 1), (1, 2), (2, 0), (0, 3), (1, 3), (2, 3)]
    for e in app.execute("mesh.elements")[:50]:
        n = e["nodes"]
        for k, (i, j) in enumerate(edges):
            mid, half = xyz[n[4 + k]], 0.5 * (xyz[n[i]] + xyz[n[j]])
            assert np.linalg.norm(mid - half) < 0.3 * np.linalg.norm(xyz[n[i]] - xyz[n[j]])
    # 크기를 줄이면 요소가 늘고 부피가 참값에 가까워진다
    exact = math.pi * 100 * 30
    fine = app.execute("mesh.generate", id=part.id, size=3.0)
    v2 = app.execute("mesh.statistics")["volume"]
    assert fine["elements"] > 3 * coarse["elements"] and abs(v2 - exact) < abs(v1 - exact) and v2 == pytest.approx(exact, rel=0.03)
    # 면별 크기: 한쪽 끝 면만 잘게
    plane = [f["index"] for f in faces if f["surface"] == "plane"][0]
    app.execute("mesh.generate", id=part.id, size=8.0)
    n_before = len(app.execute("mesh.association", id=part.id, type="face", index=plane)["faces"])
    app.execute("mesh.generate", id=part.id, size=8.0, face_sizes=[[plane, 2.0]])
    assert len(app.execute("mesh.association", id=part.id, type="face", index=plane)["faces"]) > 3 * n_before
    for params, code in [(dict(size=0.0), "out_of_range"), (dict(order=3), "out_of_range"), (dict(face_sizes=[[99, 1.0]]), "not_found"),
                         (dict(element_type="C3D8"), "out_of_range")]:
        before = total(app)
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.generate", id=part.id, **params)
        assert e.value.code == code and total(app) == before, params


@pytest.mark.feature("MSH-06")
def test_MSH_T02_surface_mesh(app):
    p = box(app)
    r = app.execute("mesh.generate", id=p.id, size=5.0, dimension=2)
    s = app.execute("mesh.statistics")
    assert r["shape"] == "tri3" and s["area"] == pytest.approx(2200.0, rel=1e-9) and s["by_type"] == {"S3": r["elements"]}
    assert app.execute("mesh.free_edges")["count"] == 0 and app.execute("mesh.check")["inconsistent_normals"] == []  # 닫힌 껍질
    a = app.execute("mesh.association", id=p.id, type="face", index=1)
    assert "faces" not in a and len(a["elements"]) > 0  # 쉘 메시에서는 면이 요소에 대응한다
    assert sum(len(app.execute("mesh.association", id=p.id, type="face", index=f)["elements"]) for f in range(1, 7)) == r["elements"]
    empty = app.model.parts.create(name="EMPTY")
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.generate", id=empty.id)
    assert e.value.code == "invalid_state"


# ================================================================ MSH-T03 형상 대상, 상태, 재메싱
@pytest.mark.feature("WT-29")
@pytest.mark.feature("MSH-09")
@pytest.mark.feature("LOD-14")
@pytest.mark.feature("MSH-11")
@pytest.mark.feature("GEO-45")
def test_MSH_T03_geometry_targets_survive_remesh(app):
    p = box(app, size=(100.0, 20.0, 10.0))
    faces = {tuple(round(c, 6) for c in f["center"]): f["index"] for f in app.execute("geometry.entities", id=p.id, type="face")["entities"]}
    x0, x1, top = faces[(0.0, 10.0, 5.0)], faces[(100.0, 10.0, 5.0)], faces[(50.0, 10.0, 10.0)]
    geo = lambda kind, i: {"type": "geometry", "ids": [[p.id, kind, i]]}
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    mat.set_density(data=[[7.85e-9]])
    step = app.model.cases.create().steps.create_static()
    bc = step.bcs.create_displacement(target=geo("face", x0), dofs=[1, 2, 3])
    pr = step.loads.create_pressure(target=geo("face", top), value=0.5)
    gr = step.loads.create_gravity(target=geo("solid", 1), value=9810.0, direction=[0.0, 0.0, -1.0])
    # 메시가 없으면 풀 수 없다
    assert app.execute("mesh.status") == [{"part": p.id, "name": "BOX", "state": "none", "mesh_part": None, "elements": 0}]
    with pytest.raises(Nasa95Error) as e:
        app.execute("bc.resolve", id=bc.id)
    assert e.value.code == "not_available"
    r = app.execute("mesh.generate", id=p.id, size=10.0)
    prop = app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [r["mesh_part"]]})
    assert app.execute("mesh.status")[0]["state"] == "current"
    xyz = coords(app)
    fixed = app.execute("bc.resolve", id=bc.id)["nodes"]
    assert fixed and all(abs(xyz[n][0]) < 1e-9 for n in fixed)
    res = app.execute("load.resultant", id=step.id)
    weight = 7.85e-9 * 20000.0 * 9810.0
    assert res["force"] == pytest.approx([0.0, 0.0, -0.5 * 2000.0 - weight], rel=1e-9) and res["unsupported"] == []
    assert app.execute("load.resolve", id=pr.id)["what"] == "faces" and app.execute("load.resolve", id=gr.id)["count"] == r["elements"]
    # 형상을 바꾸면 메시는 '갱신 필요' 가 되고, 다시 메싱하면 대상·할당이 그대로 따라온다
    feature = app.execute("feature.list", parent=p.id)[0]["id"]
    app.execute("feature.update", id=feature, size=[200.0, 20.0, 10.0])
    assert app.execute("mesh.status")[0]["state"] == "outdated"
    done = app.execute("mesh.remesh")["remeshed"]
    assert len(done) == 1 and done[0]["mesh_part"] == r["mesh_part"] and done[0]["size"] == 10.0
    assert app.execute("mesh.status")[0]["state"] == "current" and app.execute("mesh.remesh")["remeshed"] == []
    assert app.execute("mesh.statistics")["volume"] == pytest.approx(40000.0, rel=1e-9)
    res = app.execute("load.resultant", id=step.id)
    assert res["force"] == pytest.approx([0.0, 0.0, -0.5 * 4000.0 - 2 * weight], rel=1e-9)
    cover = app.execute("property.assignments")
    assert cover["unassigned"] == [] and cover["assigned"] == done[0]["elements"] and prop.id
    # 번호를 다시 매겨도 연관이 따라간다
    app.execute("id.renumber", what="nodes", start=5000)
    app.execute("id.renumber", what="elements", start=9000)
    xyz = coords(app)
    fixed = app.execute("bc.resolve", id=bc.id)["nodes"]
    assert min(fixed) >= 5000 and all(abs(xyz[n][0]) < 1e-9 for n in fixed)
    assert app.execute("load.resultant", id=step.id)["force"] == pytest.approx(res["force"], rel=1e-9)
    # 메시 지우기: 메시 파트와 할당은 남는다
    c = app.execute("mesh.clear", id=p.id)
    assert c["elements"] == done[0]["elements"] and app.execute("project.info")["nodes"] == 0
    assert app.execute("mesh.status")[0]["state"] == "none" and len(app.execute("mesh_part.list")) == 1
    # 형상 대상이 가리키는 파트는 지울 수 없다
    with pytest.raises(Nasa95Error) as e:
        p.delete()
    assert e.value.code == "referenced"
    for bad in ([[p.id, "wire", 1]], [[p.id, "face"]], [1], [[99999, "face", 1]]):
        with pytest.raises(Nasa95Error) as e:
            step.loads.create_pressure(target={"type": "geometry", "ids": bad}, value=1.0)
        assert e.value.code in ("invalid_param_type", "not_found")
    assert x1


# ================================================================ 형상 → 메시 → 풀이 → 결과
def _ccx():
    import test_SOLVER_ccx
    return test_SOLVER_ccx.CCX


@pytest.mark.feature("CAS-13")
@pytest.mark.feature("MSH-07")
def test_E2E_geometry_to_result(app, tmp_path, monkeypatch):
    """외팔보를 형상에서 시작해 풀고, 끝단 처짐을 보 이론과 비교한다."""
    if _ccx() is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", _ccx())
    p = box(app, "BEAM", size=(100.0, 20.0, 10.0))
    faces = {tuple(round(c, 6) for c in f["center"]): f["index"] for f in app.execute("geometry.entities", id=p.id, type="face")["entities"]}
    geo = lambda i: {"type": "geometry", "ids": [[p.id, "face", i]]}
    r = app.execute("mesh.generate", id=p.id, size=5.0, order=2)
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [r["mesh_part"]]})
    case = app.model.cases.create(name="beam")
    case.update(work_directory=str(tmp_path))
    step = case.steps.create_static()
    step.bcs.create_displacement(target=geo(faces[(0.0, 10.0, 5.0)]), dofs=[1, 2, 3])
    tip = geo(faces[(100.0, 10.0, 5.0)])
    # 끝 면에 아래 방향 전단을 주려고, 끝 면의 노드에 힘을 고르게 나눈다(2차 요소라 합력만 본다)
    tip_nodes = app.execute("mesh.association", id=p.id, type="face", index=faces[(100.0, 10.0, 5.0)])["nodes"]
    step.loads.create_force(target=tip, components=[0.0, 0.0, -1000.0 / len(tip_nodes)])
    step.outputs.create_node_file(variables=["U", "RF"])
    step.outputs.create_element_file(variables=["S"])
    assert app.execute("load.resultant", id=step.id)["force"] == pytest.approx([0.0, 0.0, -1000.0])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    uz = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=tip_nodes)["values"]
    theory = -1000.0 * 100.0**3 / (3 * 210000.0 * (20.0 * 10.0**3 / 12))
    assert sum(uz) / len(uz) == pytest.approx(theory, rel=0.03)
    root = app.execute("mesh.association", id=p.id, type="face", index=faces[(0.0, 10.0, 5.0)])["nodes"]
    assert app.execute("result.reaction_sum", result=rid, frame=1, nodes=root)["force"] == pytest.approx([0.0, 0.0, 1000.0], abs=0.05)
    # 굽힘 응력: 뿌리 근처 윗면의 σxx ≈ M c / I
    mm = app.execute("result.minmax", result=rid, frame=1, field="STRESS", component="SXX")
    sigma = 1000.0 * 100.0 * 5.0 / (20.0 * 10.0**3 / 12)
    assert mm["max"]["value"] > 0.8 * sigma and mm["min"]["value"] < -0.8 * sigma


@pytest.mark.feature("CMN-04")
@pytest.mark.feature("MSH-09")
def test_MSH_T03_set_from_geometry(app):
    p = box(app)
    app.execute("mesh.generate", id=p.id, size=5.0)
    face = [[p.id, "face", 1]]
    want = app.execute("mesh.association", id=p.id, type="face", index=1)
    n = app.execute("set.from_geometry", entities=face, **{"as": "node"}, name="FIX")
    s = app.execute("set.from_geometry", entities=face, **{"as": "surface"})
    e = app.execute("set.from_geometry", entities=[[p.id, "solid", 1]], **{"as": "element"})
    assert app.execute("set.get", id=n["id"])["name"] == "FIX" and app.execute("set.members", id=n["id"])["members"] == want["nodes"]
    assert app.execute("set.members", id=s["id"])["members"] == want["faces"]
    assert len(app.execute("set.members", id=e["id"])["members"]) == app.execute("project.info")["elements"]
    # 여러 엔티티를 한 셋으로
    two = app.execute("set.from_geometry", entities=[[p.id, "edge", 1], [p.id, "vertex", 8]], **{"as": "node"})
    edge = app.execute("mesh.association", id=p.id, type="edge", index=1)["nodes"]
    vertex = app.execute("mesh.association", id=p.id, type="vertex", index=8)["nodes"]
    assert app.execute("set.members", id=two["id"])["members"] == sorted(set(edge) | set(vertex))
    before = total(app)
    for params, code in [(dict(entities=[[p.id, "edge", 1]], **{"as": "surface"}), "invalid_state"),
                         (dict(entities=[[p.id, "face", 99]], **{"as": "node"}), "not_found"),
                         (dict(entities=[[p.id, "blob", 1]], **{"as": "node"}), "invalid_param_type"),
                         (dict(entities=face, **{"as": "edge"}), "out_of_range")]:
        with pytest.raises(Nasa95Error) as err:
            app.execute("set.from_geometry", **params)
        assert err.value.code == code and total(app) == before, params


# ================================================================ 배경 메싱(아키텍처 규칙 5): 작업 스레드·진행률·취소
def wait_job(app, timeout=120.0):
    import time
    t0 = time.time()
    samples = []
    while True:
        s = app.execute("mesh.job_status")
        samples.append(s)
        if s["state"] != "running":
            return s, samples
        assert time.time() - t0 < timeout, "메싱이 끝나지 않습니다"
        time.sleep(0.02)


@pytest.mark.feature("MSH-05")
@pytest.mark.feature("API-07")
@pytest.mark.feature("API-30")
def test_MSH_T02_background_job(app):
    """배경 메싱: 상태·진행률을 조회하고, 끝난 결과를 모델에 넣으면 동기 메싱과 같다. 되돌리기도 같다."""
    part = box(app, size=(40.0, 20.0, 10.0))
    assert app.execute("mesh.job_status") == {"state": "none"}
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.job_finish")
    assert e.value.code == "invalid_state"
    sync = app.execute("mesh.generate", id=part.id, size=1.0)
    app.undo()
    assert app.execute("project.info")["elements"] == 0
    r = app.execute("mesh.generate", id=part.id, size=1.0, background=True)
    assert r == {"job": "mesh", "part": part.id, "state": "running", "size": 1.0}
    with pytest.raises(Nasa95Error) as e:  # 한 번에 하나
        app.execute("mesh.generate", id=part.id, size=1.0, background=True)
    assert e.value.code == "busy"
    assert app.execute("project.info")["elements"] == 0  # 도는 동안 모델은 그대로
    s, samples = wait_job(app)
    assert s["state"] == "done" and s["applied"] is False and s["percent"] == 100.0
    assert s["nodes"] >= sync["nodes"] and s["elements"] == sync["elements"]  # 메셔가 남긴 점은 넣을 때 뺀다
    assert any(x["state"] == "running" and "percent" in x and "task" in x for x in samples)
    n = history_len(app)
    f = app.execute("mesh.job_finish")
    assert (f["nodes"], f["elements"], f["shape"]) == (sync["nodes"], sync["elements"], sync["shape"])
    assert app.execute("project.info")["elements"] == sync["elements"] and history_len(app) == n + 1
    assert app.execute("mesh.job_status")["applied"] is True
    with pytest.raises(Nasa95Error):  # 두 번 넣지 못한다
        app.execute("mesh.job_finish")
    app.undo()
    assert app.execute("project.info")["elements"] == 0
    assert app.execute("mesh.status")[0]["state"] == "none"


@pytest.mark.feature("MSH-05")
@pytest.mark.feature("API-07")
@pytest.mark.feature("API-30")
def test_MSH_T02_background_cancel_and_geometry_change(app):
    """돌고 있는 메싱을 멈출 수 있고, 메싱하는 동안 형상이 바뀌면 결과를 넣지 않는다."""
    import time
    part = box(app, size=(100.0, 50.0, 20.0))
    app.execute("mesh.generate", id=part.id, size=0.7, background=True)
    time.sleep(0.2)
    assert app.execute("mesh.job_cancel")["cancelled"] is True
    s, _ = wait_job(app)
    assert s["state"] == "cancelled" and s["error"]["code"] == "cancelled"
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.job_finish")
    assert e.value.code == "cancelled"
    assert app.execute("mesh.job_cancel") == {"cancelled": False}
    assert app.execute("project.info")["elements"] == 0
    # 형상이 바뀌면
    app.execute("mesh.generate", id=part.id, size=5.0, background=True)
    part.features.create_box(size=[1.0, 1.0, 1.0])  # 메싱하는 동안 형상 수정(피처 추가)
    s, _ = wait_job(app)
    assert s["state"] == "done"
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.job_finish")
    assert e.value.code == "geometry_changed"
    assert app.execute("project.info")["elements"] == 0
    # 되돌려 형상을 원래대로 하면 넣을 수 있다
    app.undo()
    assert app.execute("mesh.job_finish")["elements"] > 0


@pytest.mark.feature("MSH-39")
def test_MSH_T07_import_refined_mesh(app, tmp_path, monkeypatch):
    """솔버의 자동 세분화(*REFINE MESH)가 만든 메시(<job>.rfn.inp)를 비교하고 모델에 넣는다.

    ccx 2.22 로 확인: 파일은 대체된 원래 요소의 *MODEL CHANGE REMOVE + 전체 *NODE + 요소마다 *ELEMENT,PARENT=…,TYPE=C3D10 이다.
    이 ccx 빌드는 1차 사면체(C3D4)의 세분화 중 힙 손상으로 죽으므로 2차 사면체로 돌린다.
    """
    ccx = _ccx()
    if ccx is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", ccx)
    part = box(app, size=(40.0, 10.0, 10.0))
    app.execute("mesh.generate", id=part.id, size=4.0, order=2)
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    mp = app.execute("mesh_part.list")[0]["id"]
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [mp]})
    root = {"type": "geometry", "ids": [[part.id, "face", 1]]}
    tip = {"type": "geometry", "ids": [[part.id, "face", 2]]}
    case = app.model.cases.create(name="refine")
    case.update(work_directory=str(tmp_path), threads=1)
    s = case.steps.create_static()
    s.bcs.create_displacement(target=root, dofs=[1, 2, 3])
    s.loads.create_force(target=tip, components=[0.0, 0.0, -1.0])
    s.outputs.create_node_file(variables=["U"])
    s.outputs.create_element_file(variables=["S"])
    app.execute("deck_block.create", parent=s.id, text="*REFINE MESH, LIMIT=0.5\nS\n")  # 모델에 없는 카드는 보존 블록으로 넣는다
    before = app.execute("mesh.statistics")
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.import_refined", id=case.id)
    assert e.value.code == "invalid_state"
    run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", (run["errors"], run["log"])
    assert (tmp_path / "refine.rfn.inp").exists()
    cmp = app.execute("mesh.compare", path=str(tmp_path / "refine.rfn.inp"))
    assert cmp["diff"]["elements"]["ratio"] > 1.0 and "C3D10" in cmp["other"]["by_type"]
    # 세분화된 요소(원래 요소 중 일부)를 지우고 새 요소를 번호 그대로 합치면 절점이 이어진 메시가 된다
    r = app.execute("mesh.import_refined", id=case.id, replace=True)
    assert r["removed_listed"] > 0 and r["removed"] == r["removed_listed"] and r["node_offset"] == 0 and r["element_offset"] == 0
    after = app.execute("mesh.statistics")
    assert after["elements"] == before["elements"] - r["removed"] + r["elements"] and after["elements"] > before["elements"]
    assert after["volume"] == pytest.approx(4000.0, rel=0.02)  # 세분화 메시가 원래 부피를 메운다
    assert app.execute("mesh.free_faces")["count"] == app.execute("mesh.free_faces", ids=None)["count"]
    assert app.execute("mesh.merge_nodes", tolerance=1e-6)["merged"] == 0  # 번호를 이어 썼으므로 겹치는 절점이 없다
    app.undo(), app.undo()
    assert app.execute("mesh.statistics")["elements"] == before["elements"]


# ================================================================ MSH-T02 매핑 육면체 메싱(자체 구현)
@pytest.mark.feature("MSH-07")
@pytest.mark.feature("MSH-08")
@pytest.mark.feature("MSH-09")
def test_MSH_T02_hex_mapped(app):
    """육면체 위상의 솔리드를 매핑 메싱한다: 분할 수·체적·연관이 정확하고, 곡면의 노드는 곡면 위에 있다."""
    part = box(app, size=(100.0, 20.0, 10.0))
    r = app.execute("mesh.generate", id=part.id, method="hex_mapped", size=5.0)
    assert r["mesher"] == "mapped" and r["shape"] == "hex8" and r["divisions"] == [20, 4, 2] and r["elements"] == 160  # 블록 축 = 전역 x, y, z
    s = app.execute("mesh.statistics")
    assert s["volume"] == pytest.approx(20000.0) and s["by_type"] == {"C3D8": 160} and app.execute("mesh.check")["ok"]
    assert app.execute("mesh.free_faces")["count"] == 2 * (2 * 4 + 4 * 20 + 2 * 20)
    # 연관: 면마다 요소면 수 = 그 면의 분할 수 곱, 모서리 노드 수 = 분할 + 1, 꼭짓점은 형상 꼭짓점 위
    areas = {}
    for f in range(1, 7):
        info = app.execute("geometry.entity_info", id=part.id, type="face", index=f)
        faces = app.execute("mesh.association", id=part.id, type="face", index=f)["faces"]
        areas[f] = (info["area"], len(faces))
    assert sorted(n for _, n in areas.values()) == [8, 8, 40, 40, 80, 80]
    assert all(n == {200.0: 8, 1000.0: 40, 2000.0: 80}[round(a)] for a, n in areas.values())
    xyz = coords(app)
    verts = app.execute("geometry.entities", id=part.id)["vertices"]
    for v in range(1, verts + 1):
        node = app.execute("mesh.association", id=part.id, type="vertex", index=v)["nodes"][0]
        assert np.allclose(xyz[node], app.execute("geometry.entity_info", id=part.id, type="vertex", index=v)["point"])
    assert sorted(len(app.execute("mesh.association", id=part.id, type="edge", index=e)["nodes"]) for e in range(1, 13)) == [3] * 4 + [5] * 4 + [21] * 4
    assert app.execute("mesh.status")[0]["state"] == "current"
    # 다시 메싱(remesh)은 같은 방법을 쓴다
    r2 = app.execute("mesh.remesh")
    assert app.execute("mesh.statistics")["by_type"] == {"C3D8": 160}
    # 곡면: 원통 1/4 조각(회전) — 분할 수 지정, 2차. 노드가 안·바깥 원통면 위에 놓인다
    sec = app.model.parts.create(name="SECTOR")
    sec.features.create_revolve(points=[[10, 0, 0], [20, 0, 0], [20, 0, 5], [10, 0, 5]], point=[0, 0, 0], axis=[0, 0, 1], angle=90.0)
    r = app.execute("mesh.generate", id=sec.id, method="hex_mapped", divisions=[8, 4, 2], order=2)
    assert r["shape"] == "hex20" and r["elements"] == 64
    stats = app.execute("mesh.statistics")
    assert stats["by_type"]["C3D20"] == 64 and stats["volume"] - 20000.0 == pytest.approx(math.pi / 4 * (400 - 100) * 5, rel=0.03)
    pts = np.array([xyz for n, xyz in coords(app).items() if n >= r["first_node"]])
    rad = np.hypot(pts[:, 0], pts[:, 1])
    assert rad.min() == pytest.approx(10.0, abs=1e-9) and rad.max() == pytest.approx(20.0, abs=1e-9)
    corners = pts[:: 1][np.isin(np.round(rad, 9), [10.0, 20.0])]
    assert len(corners) > 0 and np.all((np.round(np.hypot(corners[:, 0], corners[:, 1]), 9) == 10.0) | (np.round(np.hypot(corners[:, 0], corners[:, 1]), 9) == 20.0))
    quality = app.execute("mesh.quality")
    assert app.execute("mesh.check")["ok"] and quality["failed"] == [] and quality["worst"]["jacobian"] > 0.3
    # 육면체 위상이 아니면 거부, 분할 수 검사
    cyl = app.model.parts.create(name="CYL")
    cyl.features.create_cylinder(radius=5.0, height=10.0)
    before = app.execute("mesh.statistics")["elements"]
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.generate", id=cyl.id, method="hex_mapped")
    assert e.value.code == "not_block" and app.execute("mesh.statistics")["elements"] == before  # 실패 보고, 기존 메시 유지(MSH-T02-12)
    with pytest.raises(Nasa95Error):
        app.execute("mesh.generate", id=part.id, method="hex_mapped", divisions=[0, 1, 1])
    with pytest.raises(Nasa95Error):
        app.execute("mesh.generate", id=part.id, method="hex_mapped", divisions=[1, 1])


@pytest.mark.feature("MSH-07")
@pytest.mark.feature("CAS-13")
def test_E2E_hex_mapped_cantilever(app, tmp_path, monkeypatch):
    """매핑 육면체 메시로 외팔보를 풀면 처짐이 보 이론과 맞는다."""
    ccx = _ccx()
    if ccx is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", ccx)
    part = box(app, size=(100.0, 10.0, 10.0))
    app.execute("mesh.generate", id=part.id, method="hex_mapped", divisions=[20, 2, 2], element_type="C3D8I")
    mat = app.model.materials.create(name="STEEL")
    mat.set_elastic(data=[[210000.0, 0.3]])
    mp = app.execute("mesh_part.list")[0]["id"]
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [mp]})
    faces = {app.execute("geometry.entity_info", id=part.id, type="face", index=f)["center"][0]: f for f in range(1, 7)}
    root, tip = faces[0.0], faces[100.0]
    case = app.model.cases.create(name="hex")
    case.update(work_directory=str(tmp_path), threads=1)
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "geometry", "ids": [[part.id, "face", root]]}, dofs=[1, 2, 3])
    tip_nodes = app.execute("mesh.association", id=part.id, type="face", index=tip)["nodes"]
    s.loads.create_force(target={"type": "geometry", "ids": [[part.id, "face", tip]]}, components=[0.0, 0.0, -100.0 / len(tip_nodes)])
    s.outputs.create_node_file(variables=["U"])
    run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed", (run["errors"], run["log"])
    rid = app.execute("result.open", case=case.id)["id"]
    uz = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=tip_nodes)["values"]
    theory = -100.0 * 100.0**3 / (3 * 210000.0 * (10.0 * 10.0**3 / 12))  # P L³ / 3EI
    assert sum(uz) / len(uz) == pytest.approx(theory, rel=0.06)


# ================================================================ MSH-T02 메시 제어 객체(MSH-04)
@pytest.mark.feature("MSH-04")
def test_MSH_T02_mesh_controls(app):
    """메시 제어 객체: 전역 크기·면 크기(사면체), 모서리 분할·편향(매핑), 곡률. 명령 매개변수가 제어보다 우선한다."""
    part = box(app, size=(100.0, 20.0, 10.0))
    faces = {tuple(app.execute("geometry.entity_info", id=part.id, type="face", index=f)["center"]): f for f in range(1, 7)}
    edges = {tuple(app.execute("geometry.entity_info", id=part.id, type="edge", index=e)["center"]): e for e in range(1, 13)}
    left, right = faces[(0.0, 10.0, 5.0)], faces[(100.0, 10.0, 5.0)]
    g = app.model.mesh_controls.create_global_size(size=8.0, min_size=0.5, grading=0.5)
    l = app.model.mesh_controls.create_local_size(target={"type": "geometry", "ids": [[part.id, "face", left]]}, size=2.0)
    r = app.execute("mesh.generate", id=part.id)
    assert r["size"] == 8.0
    nl, nr = (len(app.execute("mesh.association", id=part.id, type="face", index=f)["faces"]) for f in (left, right))
    assert nl > 4 * nr  # 왼쪽 면이 훨씬 촘촘하다
    stored = app.execute("mesh_part.get", id=r["mesh_part"])["props"]["mesh_params"]
    assert stored["size"] == 8.0
    # 명령의 size 가 우선, 제어를 억제하면 안 쓴다
    assert app.execute("mesh.generate", id=part.id, size=10.0)["size"] == 10.0
    app.execute("mesh_control.suppress", id=l.id)
    app.execute("mesh.generate", id=part.id)
    nl2, nr2 = (len(app.execute("mesh.association", id=part.id, type="face", index=f)["faces"]) for f in (left, right))
    assert nl2 < nl and abs(nl2 - nr2) <= max(nl2, nr2)  # 억제하면 양쪽이 비슷하다
    app.execute("mesh_control.unsuppress", id=l.id)
    # 다른 파트를 가리키는 제어는 쓰지 않는다
    other = box(app, "OTHER", size=(10.0, 10.0, 10.0))
    g.update(parts=[other.id])
    assert app.execute("mesh.generate", id=part.id)["size"] != 8.0
    g.update(parts=[])
    # 모서리 분할·편향(매핑 메싱): x 방향 모서리 하나에 분할 10, 편향 4 → 그 방향 전체가 10 분할, 간격 비 4
    long_edge = [e for c, e in edges.items() if c[1] == 0.0 and c[2] == 0.0][0]
    app.model.mesh_controls.create_edge_division(target={"type": "geometry", "ids": [[part.id, "edge", long_edge]]}, divisions=10, bias=4.0)
    r = app.execute("mesh.generate", id=part.id, method="hex_mapped", size=5.0)
    assert r["divisions"] == [10, 4, 2]
    xyz = coords(app)
    for e in [e for c, e in edges.items() if c[0] == 50.0]:  # x 방향 모서리 4개 모두 같은 분할·편향
        xs = sorted(xyz[n][0] for n in app.execute("mesh.association", id=part.id, type="edge", index=e)["nodes"])
        d = np.diff(xs)
        assert len(xs) == 11 and d[-1] / d[0] == pytest.approx(4.0, rel=1e-6) and sum(d) == pytest.approx(100.0)
    # 사면체 메셔는 모서리 분할을 길이/분할 수의 크기로 쓴다(편향은 무시): 그 모서리의 노드가 분할 수에 가깝다
    r = app.execute("mesh.generate", id=part.id, size=20.0)
    n_edge = len(app.execute("mesh.association", id=part.id, type="edge", index=long_edge)["nodes"])
    assert 8 <= n_edge <= 14
    # 곡률 제어: 원통에서 safety 를 올리면 요소가 는다
    app.execute("project.new")
    cyl = app.model.parts.create(name="CYL")
    cyl.features.create_cylinder(radius=10.0, height=10.0)
    coarse = app.execute("mesh.generate", id=cyl.id, size=20.0)["elements"]
    app.model.mesh_controls.create_curvature(safety=8.0)
    fine = app.execute("mesh.generate", id=cyl.id, size=20.0)["elements"]
    assert fine > 1.5 * coarse
    with pytest.raises(Nasa95Error) as e:
        app.model.mesh_controls.create_local_size(target={"type": "geometry", "ids": [[cyl.id, "edge", 99]]}, size=1.0)
        app.execute("mesh.generate", id=cyl.id)
    assert e.value.code == "not_found"


@pytest.mark.feature("MSH-10")
def test_MSH_T03_04_05_node_matching(app):
    """절점 일치: 공유 토폴로지를 만든 두 솔리드를 메싱하면 공유 면에서 같은 노드를 쓴다(중복 노드 0). 공유 없이 따로 메싱하면 접촉면 노드가 파트별로 따로 있다."""
    sh = app.model.parts.create(name="SH")
    sh.features.create_box(size=[10, 10, 10])
    sh.features.create_box(origin=[10, 0, 0], size=[10, 10, 10])
    sh.features.create_share_topology()
    r = app.execute("mesh.generate", id=sh.id, size=2.5)
    xyz = coords(app)
    on_plane = [n for n, p in xyz.items() if abs(p[0] - 10.0) < 1e-9]
    assert len(on_plane) >= 9
    key = {}
    for n in on_plane:
        key.setdefault(tuple(np.round(xyz[n], 6)), []).append(n)
    assert all(len(v) == 1 for v in key.values())  # 같은 자리의 노드가 하나씩 → 두 솔리드가 공유
    assert app.execute("mesh.check")["ok"]
    # 공유 면은 양쪽 솔리드가 같은 노드를 쓰므로 자유 면이 아니다: 자유 면(사면체 면 = 3노드)의 노드가 모두 x=10 위에 있는 것이 없다
    tet_faces = [[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]]  # 자유 면 판정에는 어느 노드 셋인지만 중요하다
    free = app.execute("mesh.free_faces")
    on_shared = 0
    for e, f in free["faces"]:
        nodes = app.execute("mesh.elements", ids=[e])[0]["nodes"][:4]
        fn = [nodes[k] for k in tet_faces[f - 1]]
        if all(abs(xyz[m][0] - 10.0) < 1e-9 for m in fn):
            on_shared += 1
    assert free["count"] > 0 and on_shared == 0
    # 공유 없이: 파트 둘을 따로 메싱 → 접촉면 노드가 두 벌
    app.execute("project.new")
    a = app.model.parts.create(name="A")
    a.features.create_box(size=[10, 10, 10])
    b = app.model.parts.create(name="B")
    b.features.create_box(origin=[10, 0, 0], size=[10, 10, 10])
    app.execute("mesh.generate", id=a.id, size=2.5)
    app.execute("mesh.generate", id=b.id, size=2.5)
    xyz = coords(app)
    key = {}
    for n, p in xyz.items():
        if abs(p[0] - 10.0) < 1e-9:
            key.setdefault(tuple(np.round(p, 6)), []).append(n)
    # 따로 메싱한 두 파트는 접촉면의 면 메시가 서로 다르다(공유 노드 없음): 네 꼭짓점은 자리가 같지만 노드는 파트마다 따로 있고, 나머지는 서로 다른 자리
    corners = [k for k, v in key.items() if k[1] in (0.0, 10.0) and k[2] in (0.0, 10.0)]
    assert len(corners) == 4 and all(len(key[k]) == 2 for k in corners)
    assert any(len(v) == 1 for v in key.values())
    dup = sum(1 for v in key.values() if len(v) == 2)
    before = app.execute("mesh.statistics")["nodes"]
    app.execute("mesh.merge_nodes", tolerance=1e-6)
    assert before - app.execute("mesh.statistics")["nodes"] == dup  # 같은 자리의 노드만 병합된다


@pytest.mark.feature("WT-29")
def test_SYS_20_04_broken_reference(app):
    """하중의 대상 형상 면을 없애면(형상 수정으로 면 수가 줄면) 그 하중이 끊어진 참조로 진단된다. 메시 요소·셋 대상도 같다."""
    part = box(app, size=(10.0, 10.0, 10.0))
    fillet = part.features.create_fillet(edges=[1], radius=1.0)  # 면 7개
    assert app.execute("geometry.entities", id=part.id)["faces"] == 7
    case = app.model.cases.create()
    step = case.steps.create_static()
    cyl = [e["index"] for e in app.execute("geometry.entities", id=part.id, type="face")["entities"] if e["surface"] == "cylinder"][0]
    load = step.loads.create_pressure(target={"type": "geometry", "ids": [[part.id, "face", cyl]]}, value=1.0)  # 필렛 면(이름표가 붙는다)
    assert load.validate() == []
    fillet.delete()  # 필렛 면이 없어진다(상자 면은 이름표로 이어지므로 번호가 밀려도 끊기지 않는다)
    issues = load.validate()
    assert [i["code"] for i in issues] == ["broken_target"] and issues[0]["field"] == "target"
    tree = {b["kind"]: b for b in app.execute("project.tree")}
    own = app.execute("step.own_load_set", id=step.id)["id"]  # 하중은 스텝 전용 하중 셋 아래에
    set_item = next(i for i in tree["load_set"]["items"] if i["id"] == own)
    assert [c["status"] for c in set_item["children"] if c["kind"] == "load"] == ["error"]
    app.undo()
    assert load.validate() == []
    # 메시 노드·요소 대상: 메시가 있을 때만 판정한다(메싱 전의 모델 정의는 끊어진 것이 아니다)
    f = step.loads.create_force(target={"type": "nodes", "ids": [999999]}, components=[0.0, 0.0, -1.0])
    assert f.validate() == []
    app.execute("mesh.generate", id=part.id, size=5.0)
    assert [i["code"] for i in f.validate()] == ["broken_target"]
    s = app.model.sets.create_node(name="N", ids=[1])
    bc = step.bcs.create_displacement(target={"type": "set", "ids": [s.id]}, dofs=[1])
    assert bc.validate() == []


@pytest.mark.feature("MSH-05")
@pytest.mark.feature("MSH-09")
def test_MSH_T02_06_15_line_mesh(app):
    """1D 메싱(자체 구현): 길이 100 모서리를 크기 10 으로 → 선 요소 10개(B31). 반원 모서리를 10 분할 → 요소 10개, 노드가 원호 위.
    2차(B32)는 가운데 절점도 곡선 위. 모서리 분할 제어가 크기보다 우선. 이어진 모서리의 꼭짓점 노드는 공유되고 형상-메시 연관(edge·vertex)이 기록된다."""
    part = app.model.parts.create(name="L")
    part.features.create_line(start=[0.0, 0.0, 0.0], end=[100.0, 0.0, 0.0])
    r = app.execute("mesh.generate", id=part.id, size=10.0)  # 면이 없는 파트의 기본 = 1D
    assert r["elements"] == 10 and r["nodes"] == 11 and r["shape"] == "line2" and r["mesher"] == "lines"
    xyz = coords(app)
    assert sorted(round(p[0], 9) for p in xyz.values()) == [float(i * 10) for i in range(11)]
    assert app.execute("mesh.statistics")["by_type"] == {"B31": 10}
    with pytest.raises(Nasa95Error) as e:  # 면이 없는데 2D 를 요구하면 오류
        app.execute("mesh.generate", id=part.id, size=10.0, dimension=2)
    assert e.value.code == "invalid_state"
    # 반원(MSH-T02-15): 반지름 50, 10 분할. 노드가 원호 위(중심에서 50), 요소 10개. 2차는 가운데 절점도 원호 위
    app.execute("project.new")
    arc = app.model.parts.create(name="A")
    arc.features.create_arc(start=[-50.0, 0.0, 0.0], middle=[0.0, 50.0, 0.0], end=[50.0, 0.0, 0.0])
    app.model.mesh_controls.create_edge_division(target={"type": "geometry", "ids": [[arc.id, "edge", 1]]}, divisions=10)
    r = app.execute("mesh.generate", id=arc.id, size=1.0, order=2)  # 분할 제어가 크기(1.0 → 158개)보다 우선
    assert r["elements"] == 10 and r["nodes"] == 21 and r["shape"] == "line3"
    xyz = coords(app)
    assert all(abs(math.hypot(p[0], p[1]) - 50.0) < 1e-9 for p in xyz.values())
    el = app.execute("mesh.elements", ids=[r["first_element"]])[0]
    a, m, b = (xyz[n] for n in el["nodes"])
    assert abs(np.linalg.norm(m - a) - np.linalg.norm(b - m)) < 1e-9  # 가운데 절점이 호 길이의 가운데
    assert app.execute("mesh.statistics")["by_type"] == {"B32": 10}
    # 이어진 두 모서리(ㄱ 자): 공유 꼭짓점의 노드는 하나, 연관에 edge 2개·vertex 3개
    app.execute("project.new")
    ell = app.model.parts.create(name="ELL")
    ell.features.create_line(start=[0.0, 0.0, 0.0], end=[40.0, 0.0, 0.0])
    ell.features.create_line(start=[40.0, 0.0, 0.0], end=[40.0, 30.0, 0.0])
    r = app.execute("mesh.generate", id=ell.id, size=10.0)
    assert r["elements"] == 7 and r["nodes"] == 8
    assoc = app.model.mesh_parts[r["mesh_part"]].props["association"]
    assert assoc["dimension"] == 1 and len(assoc["edges"]) == 2 and len(assoc["vertices"]) == 3
    corner = app.execute("geometry.find", id=ell.id, type="vertex", box_min=[39.0, -1.0, -1.0], box_max=[41.0, 1.0, 1.0])["indices"][0]
    assert sum(1 for n in app.execute("mesh.nodes")["ids"] if n == assoc["vertices"][str(corner)]) == 1
    # 꼭짓점을 형상 대상으로 쓰는 하중은 그 노드로 전개된다
    case = app.model.cases.create(name="c")
    step = case.steps.create_static()
    tip = app.execute("geometry.find", id=ell.id, type="vertex", box_min=[39.0, 29.0, -1.0], box_max=[41.0, 31.0, 1.0])["indices"][0]
    load = step.loads.create_force(target={"type": "geometry", "ids": [[ell.id, "vertex", tip]]}, components=[0.0, 0.0, -1.0])
    assert app.execute("load.resolve", id=load.id)["nodes"] == [assoc["vertices"][str(tip)]]
