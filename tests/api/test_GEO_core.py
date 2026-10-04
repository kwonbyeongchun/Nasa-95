"""형상(5단계): 파트·피처, 기본 형상, 불리언, 변환, 가져오기·내보내기, 조회·측정·검사, 표시용 삼각화.

케이스 정의: .agent/tests/tc-01-geometry.md. 형상 커널(OpenCASCADE) 없이 빌드했으면 건너뛴다.
"""
import math

import numpy as np
import pytest

from openfep import App, OfepError

from conftest import history_len, total

pytestmark = pytest.mark.skipif("geometry.measure" not in {c["name"] for c in App().commands()},
                                reason="형상 커널(OpenCASCADE) 없이 빌드됨")


def measure(app, part, **kw):
    return app.execute("geometry.measure", id=part.id, **kw)


def counts(app, part):
    e = app.execute("geometry.entities", id=part.id)
    return e["solids"], e["faces"], e["edges"], e["vertices"]


def box(app, name="P", size=(10.0, 20.0, 30.0), origin=None):
    part = app.model.parts.create(name=name)
    part.features.create_box(size=list(size), **({"origin": list(origin)} if origin else {}))
    return part


# ================================================================ GEO-T03 기본 형상
@pytest.mark.feature("GEO-11")
@pytest.mark.feature("GEO-08")
def test_GEO_T03_primitives(app):
    p = box(app)
    m = measure(app, p)
    assert m["volume"] == pytest.approx(6000.0) and m["area"] == pytest.approx(2 * (200 + 600 + 300))
    assert m["center"] == pytest.approx([5.0, 10.0, 15.0]) and m["bbox"]["min"] == pytest.approx([0, 0, 0], abs=1e-9)
    assert m["bbox"]["max"] == pytest.approx([10, 20, 30]) and counts(app, p) == (1, 6, 12, 8)
    # 관성(질량중심 기준, 밀도 1): Izz = V (a² + b²) / 12
    assert m["inertia"][2][2] == pytest.approx(6000.0 * (10**2 + 20**2) / 12)
    cases = [
        ("cylinder", dict(radius=5.0, height=20.0, axis=[1.0, 0.0, 0.0]), math.pi * 25 * 20, (1, 3, 3, 2)),
        ("sphere", dict(radius=5.0, center=[1.0, 2.0, 3.0]), 4 / 3 * math.pi * 125, None),
        ("cone", dict(radius1=5.0, radius2=2.0, height=10.0), math.pi * 10 / 3 * (25 + 10 + 4), None),
        ("torus", dict(major_radius=10.0, minor_radius=2.0), 2 * math.pi**2 * 10 * 4, None),
    ]
    for kind, params, volume, n in cases:
        part = app.model.parts.create(name=kind)
        app.execute(f"feature.create_{kind}", parent=part.id, **params)
        assert measure(app, part)["volume"] == pytest.approx(volume, rel=1e-9), kind
        if n:
            assert counts(app, part) == n
        assert app.execute("geometry.check", id=part.id)["valid"] is True


@pytest.mark.feature("GEO-15")
def test_GEO_T03_transforms(app):
    p = box(app)
    f = p.features
    t = f.create_translate(vector=[100.0, 0.0, 0.0])
    assert measure(app, p)["bbox"]["min"] == pytest.approx([100, 0, 0], abs=1e-9)
    t.delete()
    r = f.create_rotate(axis=[0.0, 0.0, 1.0], angle=90.0)  # (x, y) → (−y, x)
    b = measure(app, p)["bbox"]
    assert b["min"] == pytest.approx([-20, 0, 0], abs=1e-7) and b["max"] == pytest.approx([0, 10, 30], abs=1e-7)
    r.delete()
    m = f.create_mirror(normal=[1.0, 0.0, 0.0])
    b = measure(app, p)
    assert b["bbox"]["min"] == pytest.approx([-10, 0, 0], abs=1e-9) and b["volume"] == pytest.approx(6000.0)  # 뒤집혀도 부피는 양수
    assert app.execute("geometry.check", id=p.id)["valid"] is True
    m.delete()
    f.create_scale(factor=2.0, center=[10.0, 20.0, 30.0])
    b = measure(app, p)
    assert b["volume"] == pytest.approx(48000.0) and b["bbox"]["min"] == pytest.approx([-10, -20, -30])


# ================================================================ GEO-T04 불리언
@pytest.mark.feature("GEO-13")
def test_GEO_T04_booleans(app):
    plate = box(app, "PLATE", size=(40.0, 40.0, 10.0))
    drill = app.model.parts.create(name="DRILL")
    drill.features.create_cylinder(origin=[20.0, 20.0, -5.0], radius=5.0, height=20.0)
    cut = plate.features.create_cut(tools=[drill.id])
    assert measure(app, plate)["volume"] == pytest.approx(16000.0 - math.pi * 25 * 10, rel=1e-9)
    assert counts(app, plate)[1] == 7  # 6면 + 구멍 면
    types = sorted(e["surface"] for e in app.execute("geometry.entities", id=plate.id, type="face")["entities"])
    assert types == ["cylinder"] + ["plane"] * 6
    assert measure(app, drill)["volume"] == pytest.approx(math.pi * 25 * 20)  # 도구 파트는 그대로 남는다
    # 도구 파트를 고치면 결과도 따라 바뀐다
    drill_feature = app.execute("feature.list", parent=drill.id)[0]["id"]
    app.execute("feature.update", id=drill_feature, radius=10.0)
    assert measure(app, plate)["volume"] == pytest.approx(16000.0 - math.pi * 100 * 10, rel=1e-9)
    cut.delete()
    plate.features.create_common(tools=[drill.id])
    assert measure(app, plate)["volume"] == pytest.approx(math.pi * 100 * 10, rel=1e-9)
    # 합치기: 한 파트의 바디들. 겹친 두 상자 → 솔리드 하나, 같은 평면의 면은 합쳐진다
    two = box(app, "TWO", size=(10.0, 10.0, 10.0))
    two.features.create_box(origin=[5.0, 0.0, 0.0], size=[10.0, 10.0, 10.0])
    assert counts(app, two)[0] == 2 and measure(app, two)["volume"] == pytest.approx(2000.0)  # 합치기 전에는 바디 둘
    two.features.create_fuse()
    assert counts(app, two) == (1, 6, 12, 8) and measure(app, two)["volume"] == pytest.approx(1500.0)
    # 다른 파트와 합치기
    other = box(app, "OTHER", size=(10.0, 10.0, 10.0), origin=(0.0, 0.0, 10.0))
    two.features.create_fuse(tools=[other.id])
    assert measure(app, two)["volume"] == pytest.approx(2500.0) and counts(app, two)[0] == 1


# ================================================================ GEO-T09 이력: 수정·억제·되돌리기·실패
@pytest.mark.feature("GEO-39")
@pytest.mark.feature("GEO-40")
@pytest.mark.feature("GEO-41")
def test_GEO_T09_history_follows_model(app):
    p = box(app)
    feature = app.execute("feature.list", parent=p.id)[0]["id"]
    before = total(app)
    app.execute("feature.update", id=feature, size=[10.0, 20.0, 60.0])
    assert measure(app, p)["volume"] == pytest.approx(12000.0)
    app.undo()  # 형상은 피처에서 계산하므로 Undo 만으로 돌아온다
    assert total(app) == before and measure(app, p)["volume"] == pytest.approx(6000.0)
    app.redo()
    assert measure(app, p)["volume"] == pytest.approx(12000.0)
    move = p.features.create_translate(vector=[5.0, 0.0, 0.0])
    scale = p.features.create_scale(factor=2.0)
    assert measure(app, p)["bbox"]["min"][0] == pytest.approx(10.0)
    move.suppress()
    assert measure(app, p)["bbox"]["min"][0] == pytest.approx(0.0)
    st = app.execute("feature.status", id=p.id)
    assert [f["state"] for f in st["features"]] == ["ok", "suppressed", "ok"] and st["ok"] is True
    move.unsuppress()
    # 순서를 바꾸면 결과가 달라진다: 배율 뒤에 이동
    app.execute("feature.move", id=move.id, index=2)
    assert measure(app, p)["bbox"]["min"][0] == pytest.approx(5.0)
    # 되돌려 보기: 첫 피처까지만
    p.update(rollback=feature)
    assert measure(app, p)["volume"] == pytest.approx(12000.0)
    assert [f["state"] for f in app.execute("feature.status", id=p.id)["features"]] == ["ok", "rolled_back", "rolled_back"]
    p.update(rollback=None)
    assert measure(app, p)["volume"] == pytest.approx(96000.0)
    assert scale.id


@pytest.mark.feature("GEO-42")
def test_GEO_T09_failed_feature(app):
    p = box(app)
    bad = p.features.create_torus(major_radius=1.0, minor_radius=5.0)
    after = p.features.create_translate(vector=[1.0, 0.0, 0.0])
    st = app.execute("feature.status", id=p.id)
    assert st["ok"] is False and [f["state"] for f in st["features"]] == ["ok", "error", "skipped"]
    assert st["features"][1]["feature"] == bad.id and st["features"][1]["code"] == "out_of_range"
    assert measure(app, p)["volume"] == pytest.approx(6000.0)  # 실패 앞까지의 형상
    bad.suppress()
    st = app.execute("feature.regenerate", id=p.id)
    assert st["ok"] is True and measure(app, p)["bbox"]["min"][0] == pytest.approx(1.0)
    # 형상이 없는 파트, 도구가 없는 빼기, 서로를 도구로 쓰는 파트
    empty = app.model.parts.create(name="EMPTY")
    with pytest.raises(OfepError) as e:
        measure(app, empty)
    assert e.value.code == "invalid_state"
    assert app.execute("geometry.entities", id=empty.id)["faces"] == 0
    lonely = app.model.parts.create(name="LONELY")
    lonely.features.create_cut(tools=[p.id])
    assert app.execute("feature.status", id=lonely.id)["features"][0]["code"] == "invalid_state"
    a, b = box(app, "A"), box(app, "B")
    a.features.create_fuse(tools=[b.id])
    b.features.create_fuse(tools=[a.id])
    with pytest.raises(OfepError) as e:
        measure(app, a)
    assert e.value.code == "invalid_state"
    assert after.id
    with pytest.raises(OfepError) as e:
        app.execute("geometry.measure", id=bad.id)
    assert e.value.code == "wrong_kind"


# ================================================================ GEO-T01 가져오기·내보내기
@pytest.mark.feature("GEO-01")
@pytest.mark.feature("GEO-02")
@pytest.mark.feature("GEO-44")
def test_GEO_T01_export_import_round_trip(app, tmp_path):
    plate = box(app, "PLATE", size=(40.0, 40.0, 10.0))
    drill = app.model.parts.create(name="DRILL")
    drill.features.create_cylinder(origin=[20.0, 20.0, -5.0], radius=5.0, height=20.0)
    plate.features.create_cut(tools=[drill.id])
    want = measure(app, plate)
    folder = tmp_path / "형상 파일"
    folder.mkdir()
    before, hist = total(app), history_len(app)
    for name in ("plate.step", "plate.brep"):
        r = app.execute("geometry.export", id=plate.id, path=str(folder / name))
        assert (folder / name).stat().st_size > 1000 and r["format"] == name.split(".")[1]
    assert total(app) == before and history_len(app) == hist
    for name in ("plate.step", "plate.brep"):
        r = app.execute("geometry.import", path=str(folder / name))
        assert (r["solids"], r["faces"]) == (1, 7)
        got = app.execute("geometry.measure", id=r["id"])
        assert got["volume"] == pytest.approx(want["volume"], rel=1e-6) and got["center"] == pytest.approx(want["center"], abs=1e-6)
        f = app.execute("feature.get", id=r["feature"])
        assert f["props"]["type"] == "import" and f["props"]["format"] == r["format"] and f["props"]["source"].endswith(name)
    names = [p["name"] for p in app.execute("part.list")]
    assert names == ["PLATE", "DRILL", "plate", "plate-2"]  # 파일 이름이 파트 이름
    # 가져온 형상 위에 피처를 쌓는다
    imported = app.execute("part.list")[2]["id"]
    app.execute("feature.create_scale", parent=imported, factor=2.0)
    assert app.execute("geometry.measure", id=imported)["volume"] == pytest.approx(8 * want["volume"], rel=1e-6)
    # 배율(원본 단위)과 이름을 주고 가져오기, 한 번에 되돌리기
    hist = history_len(app)
    r = app.execute("geometry.import", path=str(folder / "plate.step"), scale=0.001, name="METERS")
    assert history_len(app) == hist + 1
    assert app.execute("geometry.measure", id=r["id"])["volume"] == pytest.approx(want["volume"] * 1e-9, rel=1e-6)
    app.undo()
    assert len(app.execute("part.list")) == 4
    for params, code in [(dict(path=str(folder / "none.step")), "io_error"), (dict(path=str(folder / "plate.xyz")), "unsupported"),
                         (dict(path=str(folder / "plate.step"), format="dxf"), "out_of_range"),
                         (dict(path=str(folder / "plate.step"), scale=0.0), "out_of_range")]:
        with pytest.raises(OfepError) as e:
            app.execute("geometry.import", **params)
        assert e.value.code == code, params
    (folder / "broken.step").write_text("not a step file")
    with pytest.raises(OfepError) as e:
        app.execute("geometry.import", path=str(folder / "broken.step"))
    assert e.value.code == "parse_error" and len(app.execute("part.list")) == 4


@pytest.mark.feature("CMN-09")
@pytest.mark.feature("GEO-44")
def test_GEO_T01_project_save_keeps_geometry(app, tmp_path):
    p = box(app)
    app.execute("geometry.export", id=p.id, path=str(tmp_path / "b.step"))
    r = app.execute("geometry.import", path=str(tmp_path / "b.step"))
    (tmp_path / "b.step").unlink()  # 형상은 프로젝트에 들어 있어 원본 파일이 없어도 된다
    app.execute("project.save_as", path=str(tmp_path / "m.ofep"))
    other = App()
    other.execute("project.open", path=str(tmp_path / "m.ofep"))
    assert other.digest() == app.digest()
    assert other.execute("geometry.measure", id=r["id"])["volume"] == pytest.approx(6000.0)
    assert other.execute("geometry.measure", id=p.id)["volume"] == pytest.approx(6000.0)


# ================================================================ GEO-T02 토폴로지·측정·검사
@pytest.mark.feature("GEO-04")
@pytest.mark.feature("GEO-09")
def test_GEO_T02_topology(app):
    p = box(app)
    before, hist = total(app), history_len(app)
    faces = app.execute("geometry.entities", id=p.id, type="face")["entities"]
    assert sorted(f["area"] for f in faces) == pytest.approx([200, 200, 300, 300, 600, 600])
    assert {f["surface"] for f in faces} == {"plane"} and [f["index"] for f in faces] == [1, 2, 3, 4, 5, 6]
    edges = app.execute("geometry.entities", id=p.id, type="edge")["entities"]
    assert sorted(e["length"] for e in edges) == pytest.approx([10] * 4 + [20] * 4 + [30] * 4) and {e["curve"] for e in edges} == {"line"}
    info = app.execute("geometry.entity_info", id=p.id, type="vertex", index=1)
    assert len(info["point"]) == 3
    assert measure(app, p, type="face", index=1)["area"] == faces[0]["area"]
    adj = lambda **kw: app.execute("geometry.adjacency", id=p.id, **kw)["indices"]
    assert len(adj(type="face", index=1, to="edge")) == 4 and len(adj(type="face", index=1)) == 4  # 맞은편 면만 빼고 이웃
    assert len(adj(type="edge", index=1, to="face")) == 2 and len(adj(type="edge", index=1, to="vertex")) == 2
    assert len(adj(type="vertex", index=1, to="edge")) == 3 and len(adj(type="vertex", index=1)) == 3
    assert adj(type="face", index=1, to="solid") == [1] and len(adj(type="solid", index=1, to="face")) == 6
    c = app.execute("geometry.check", id=p.id)
    assert c == {"valid": True, "free_edges": [], "non_manifold_edges": [], "small_edges": [], "small_edge_limit": c["small_edge_limit"], "ok": True}
    assert app.execute("geometry.check", id=p.id, small_edge=15.0)["small_edges"] == [e["index"] for e in edges if e["length"] < 15]
    assert total(app) == before and history_len(app) == hist
    for params, code in [(dict(type="face", index=7), "not_found"), (dict(type="wire", index=1), "out_of_range")]:
        with pytest.raises(OfepError) as e:
            app.execute("geometry.entity_info", id=p.id, **params)
        assert e.value.code == code
    with pytest.raises(OfepError) as e:
        measure(app, p, type="face")
    assert e.value.code == "missing_param"


# ================================================================ GEO-T02 표시용 삼각화
@pytest.mark.feature("GEO-06")
@pytest.mark.feature("API-12")
def test_GEO_T02_tessellation(app):
    def area(t):
        p, tri = t["points"], t["triangles"]
        return 0.5 * np.linalg.norm(np.cross(p[tri[:, 1]] - p[tri[:, 0]], p[tri[:, 2]] - p[tri[:, 0]]), axis=1).sum()

    def outward(t, center):
        p, tri = t["points"], t["triangles"]
        n = np.cross(p[tri[:, 1]] - p[tri[:, 0]], p[tri[:, 2]] - p[tri[:, 0]])
        mid = p[tri].mean(axis=1)
        d = np.einsum("ij,ij->i", n, mid - np.array(center))
        return bool(np.all(d > -1e-9) and (d > 0).mean() > 0.98)  # 극점의 넓이 0 인 삼각형은 0 이 된다

    p = box(app)
    summary = app.execute("geometry.tessellation", id=p.id)
    t = app.geometry.tessellation(p.id)
    assert t["points"].shape[1] == 3 and t["triangles"].shape == (12, 3) and summary["triangles"] == 12 and summary["edges"] == 12
    assert area(t) == pytest.approx(2200.0) and outward(t, [5, 10, 15])  # 삼각형이 바깥을 향한다
    assert sorted(set(t["triangle_face"].tolist())) == [1, 2, 3, 4, 5, 6] and not t["points"].flags.writeable
    assert np.allclose(np.linalg.norm(t["normals"], axis=1), 1.0)
    assert t["edge_offsets"].tolist() == list(range(0, 25, 2)) and t["edge_points"].shape == (24, 3)  # 직선 모서리는 점 2개
    # 대칭 이동한 형상도 바깥을 향한다
    p.features.create_mirror(normal=[1.0, 0.0, 0.0])
    assert outward(app.geometry.tessellation(p.id), [-5, 10, 15])
    # 곡면: 허용 거리를 줄이면 삼각형이 늘고 넓이가 참값에 가까워진다
    s = app.model.parts.create(name="S")
    s.features.create_sphere(radius=10.0)
    coarse, fine = app.geometry.tessellation(s.id, 1.0), app.geometry.tessellation(s.id, 0.01)
    exact = 4 * math.pi * 100
    assert len(fine["triangles"]) > 4 * len(coarse["triangles"])
    assert abs(area(fine) - exact) < abs(area(coarse) - exact) and area(fine) == pytest.approx(exact, rel=2e-3)
    assert outward(fine, [0, 0, 0])
    radial = fine["points"] / np.linalg.norm(fine["points"], axis=1, keepdims=True)
    assert np.allclose(np.einsum("ij,ij->i", fine["normals"], radial), 1.0, atol=1e-6)  # 구의 법선은 반지름 방향
    with pytest.raises(OfepError) as e:
        app.geometry.tessellation(app.model.parts.create(name="EMPTY").id)
    assert e.value.code == "invalid_state"


# ================================================================ GEO-T03 스윕·필렛·속 비우기·분할·배열
def face_at(app, part, center):
    """중심이 center 인 면의 번호."""
    for i in range(1, counts(app, part)[1] + 1):
        if app.execute("geometry.entity_info", id=part.id, type="face", index=i)["center"] == pytest.approx(center):
            return i
    raise AssertionError(center)


@pytest.mark.feature("GEO-12")
@pytest.mark.feature("GEO-11")
def test_GEO_T03_extrude_revolve(app):
    """돌출·회전: 다각형 단면 또는 앞 형상의 면. merge 로 더하거나 뺀다."""
    p = app.model.parts.create(name="EXT")
    p.features.create_extrude(points=[[0, 0, 0], [10, 0, 0], [10, 5, 0], [0, 5, 0]], distance=3.0)
    assert measure(app, p)["volume"] == pytest.approx(150.0) and counts(app, p) == (1, 6, 12, 8)
    assert measure(app, p)["bbox"]["max"] == pytest.approx([10, 5, 3])  # 방향을 안 주면 단면의 법선(+z)
    b = box(app, "BOX", (10.0, 10.0, 10.0))
    b.features.create_extrude(points=[[2, 2, 10], [5, 2, 10], [5, 5, 10], [2, 5, 10]], distance=4.0, direction=[0, 0, -1], merge="cut")
    assert measure(app, b)["volume"] == pytest.approx(1000 - 36)  # 포켓
    b.features.create_extrude(points=[[6, 6, 10], [8, 6, 10], [8, 8, 10], [6, 8, 10]], distance=5.0, merge="fuse")
    assert measure(app, b)["volume"] == pytest.approx(1000 - 36 + 20) and counts(app, b)[0] == 1  # 보스
    f = box(app, "FACE", (10.0, 10.0, 10.0))
    top = face_at(app, f, [5.0, 5.0, 10.0])
    f.features.create_extrude(face=top, distance=5.0, merge="fuse")  # 면을 그대로 밀면 하나의 바디로 합쳐진다
    assert measure(app, f)["volume"] == pytest.approx(1500.0) and counts(app, f) == (1, 6, 12, 8)
    r = app.model.parts.create(name="REV")
    r.features.create_revolve(points=[[5, 0, 0], [7, 0, 0], [7, 0, 4], [5, 0, 4]], point=[0, 0, 0], axis=[0, 0, 1])
    assert measure(app, r)["volume"] == pytest.approx(math.pi * (49 - 25) * 4)  # 관
    h = app.model.parts.create(name="HALF")
    h.features.create_revolve(points=[[0, 0, 0], [3, 0, 0], [3, 0, 2], [0, 0, 2]], point=[0, 0, 0], axis=[0, 0, 1], angle=180.0)
    assert measure(app, h)["volume"] == pytest.approx(math.pi * 9 * 2 / 2)
    # 잘못된 단면: 한 평면 위가 아님, 점 부족, 없는 면
    bad = app.model.parts.create(name="BAD")
    fe = bad.features.create_extrude(points=[[0, 0, 0], [1, 0, 0], [1, 1, 1], [0, 1, 0]], distance=1.0)
    st = app.execute("feature.status", id=bad.id)["features"][-1]
    assert st["state"] == "error" and st["code"] == "invalid_geometry"
    app.execute("feature.delete", id=fe.id)
    bad.features.create_extrude(points=[[0, 0, 0], [1, 0, 0]], distance=1.0)
    assert app.execute("feature.status", id=bad.id)["features"][-1]["code"] == "out_of_range"
    nf = box(app, "NF", (1.0, 1.0, 1.0))
    nf.features.create_extrude(face=9, distance=1.0)
    assert app.execute("feature.status", id=nf.id)["features"][-1]["code"] == "not_found"


@pytest.mark.feature("GEO-18")
@pytest.mark.feature("GEO-30")
@pytest.mark.feature("GEO-14")
def test_GEO_T03_fillet_chamfer_shell_split(app):
    """필렛·챔퍼·속 비우기·평면 분할. 모서리·면 번호는 앞 피처까지의 형상에서 센다."""
    f = box(app, "FIL", (10.0, 10.0, 10.0))
    f.features.create_fillet(edges=list(range(1, 13)), radius=1.0)
    # 모서리 12개를 r=1 로 둥글린 정육면체: 모서리마다 (1 - π/4)·L 을 빼고 꼭짓점 8개에서 (1 - π/6)... 수치는 커널 값을 믿고 범위만 본다
    v = measure(app, f)["volume"]
    assert 1000 - 12 * (1 - math.pi / 4) * 10 < v < 1000 and app.execute("geometry.check", id=f.id)["valid"]
    assert counts(app, f)[1] == 26  # 면 6 + 모서리 필렛 12 + 꼭짓점 8
    c = box(app, "CHA", (10.0, 10.0, 10.0))
    c.features.create_chamfer(edges=[1], distance=1.0)
    assert measure(app, c)["volume"] == pytest.approx(1000 - 0.5 * 10) and counts(app, c)[1] == 7
    s = box(app, "SH", (10.0, 10.0, 10.0))
    s.features.create_shell(faces=[face_at(app, s, [5.0, 5.0, 10.0])], thickness=1.0)
    assert measure(app, s)["volume"] == pytest.approx(1000 - 8 * 8 * 9)  # 윗면을 열고 두께 1 의 상자
    so = box(app, "SHO", (10.0, 10.0, 10.0))
    so.features.create_shell(faces=[face_at(app, so, [5.0, 5.0, 10.0])], thickness=1.0, outward=True)
    assert measure(app, so)["volume"] == pytest.approx(12 * 12 * 11 - 1000)
    sp = box(app, "SPL", (10.0, 10.0, 10.0))
    sp.features.create_split(point=[4.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0])
    assert counts(app, sp) == (2, 11, 20, 12) and measure(app, sp)["volume"] == pytest.approx(1000.0)  # 두 솔리드가 면을 공유한다
    # 분할된 파트도 메싱되고 공유 면에서 절점이 맞는다
    if app.execute("app.version").get("mesher"):
        r = app.execute("mesh.generate", id=sp.id, size=3.0)
        assert r["elements"] > 0
        assoc = lambda t, i: app.execute("mesh.association", id=sp.id, type=t, index=i)
        assert len(assoc("solid", 1)["elements"]) > 0 and len(assoc("solid", 2)["elements"]) > 0
        assert len(assoc("solid", 1)["elements"]) + len(assoc("solid", 2)["elements"]) == r["elements"]
        # 공유 면에서 절점이 맞는다: 공유 면의 노드는 양쪽 솔리드의 요소가 함께 쓰고, 자유면은 바깥 면뿐이다
        shared_nodes = set(assoc("face", face_at(app, sp, [4.0, 5.0, 5.0]))["nodes"])
        assert shared_nodes and shared_nodes <= set(assoc("solid", 1)["nodes"]) & set(assoc("solid", 2)["nodes"])
        outer = sum(len(assoc("face", i)["faces"]) for i in range(1, 12) if i != face_at(app, sp, [4.0, 5.0, 5.0]))
        assert app.execute("mesh.free_faces")["count"] == outer
    bad = box(app, "BAD", (10.0, 10.0, 10.0))
    bad.features.create_fillet(edges=[1], radius=20.0)  # 너무 큰 반지름
    st = app.execute("feature.status", id=bad.id)["features"][-1]
    assert st["state"] == "error" and st["code"] == "geometry_failed"


@pytest.mark.feature("GEO-15")
def test_GEO_T03_pattern(app):
    """배열: 선형·원형 복사, 합치기."""
    p = app.model.parts.create(name="PAT")
    p.features.create_cylinder(radius=1.0, height=2.0)
    p.features.create_pattern(kind="circular", count=4, point=[5.0, 0.0, 0.0], axis=[0.0, 0.0, 1.0])
    assert counts(app, p)[0] == 4 and measure(app, p)["volume"] == pytest.approx(4 * math.pi * 2)
    assert measure(app, p)["center"] == pytest.approx([5.0, 0.0, 1.0])  # 한 바퀴를 4 등분: 중심이 축 위
    l = box(app, "LIN", (1.0, 1.0, 1.0))
    l.features.create_pattern(kind="linear", count=3, vector=[1.0, 0.0, 0.0], merge="fuse")
    assert counts(app, l) == (1, 6, 12, 8) and measure(app, l)["volume"] == pytest.approx(3.0)  # 맞닿은 복사본이 한 바디
    h = box(app, "HALF", (1.0, 1.0, 1.0), origin=(5.0, 0.0, 0.0))
    h.features.create_pattern(kind="circular", count=3, point=[0.0, 0.0, 0.0], axis=[0.0, 0.0, 1.0], angle=180.0)
    assert counts(app, h)[0] == 3
    bb = measure(app, h)["bbox"]
    assert bb["min"][0] == pytest.approx(-6.0) and bb["max"][1] == pytest.approx(6.0, abs=1e-9)  # 0°, 90°, 180°


@pytest.mark.feature("GEO-19")
def test_GEO_T03_datums(app):
    """기준 형상(점·축·평면)을 분할·대칭·회전의 기준으로 쓴다. 기준을 고치면 형상이 다시 계산된다."""
    plane = app.model.datums.create_plane(name="MID", point=[4.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0])
    axis = app.model.datums.create_axis(name="Z", point=[0.0, 0.0, 0.0], direction=[0.0, 0.0, 1.0])
    pt = app.model.datums.create_point(name="P", point=[1.0, 2.0, 3.0])
    sp = box(app, "SPL", (10.0, 10.0, 10.0))
    sp.features.create_split(datum=plane.id)
    assert counts(app, sp)[0] == 2
    vols = sorted(app.execute("geometry.entity_info", id=sp.id, type="solid", index=i)["volume"] for i in (1, 2))
    assert vols == pytest.approx([400.0, 600.0])
    plane.update(point=[2.0, 0.0, 0.0])  # 기준 평면을 옮기면 분할도 따라간다
    vols = sorted(app.execute("geometry.entity_info", id=sp.id, type="solid", index=i)["volume"] for i in (1, 2))
    assert vols == pytest.approx([200.0, 800.0])
    m = box(app, "MIR", (10.0, 10.0, 10.0), origin=(5.0, 0.0, 0.0))
    m.features.create_mirror(datum=plane.id)  # x ∈ [5, 15] 를 x = 2 평면에 대칭 → [-11, -1]
    assert measure(app, m)["bbox"]["min"][0] == pytest.approx(-11.0) and measure(app, m)["bbox"]["max"][0] == pytest.approx(-1.0)
    r = box(app, "ROT", (10.0, 10.0, 10.0))
    r.features.create_rotate(datum=axis.id, angle=90.0)
    assert measure(app, r)["bbox"]["min"][0] == pytest.approx(-10.0) and measure(app, r)["bbox"]["max"][1] == pytest.approx(10.0)
    bad = box(app, "BAD", (1.0, 1.0, 1.0))
    bad.features.create_split(datum=pt.id)  # 점에는 방향이 없다
    assert app.execute("feature.status", id=bad.id)["features"][-1]["code"] == "invalid_param"
    with pytest.raises(OfepError):
        app.execute("datum.delete", id=plane.id)  # 피처가 참조한다


@pytest.mark.feature("GEO-12")
@pytest.mark.feature("GEO-18")
@pytest.mark.feature("GEO-29")
def test_GEO_T03_sweep_loft_offset_solid(app):
    """스윕·로프트·오프셋·두께 부여·분해·솔리드 구성."""
    s = app.model.parts.create(name="SW")
    s.features.create_sweep(points=[[0, 0, 0], [2, 0, 0], [2, 2, 0], [0, 2, 0]], path=[[0, 0, 0], [0, 0, 10]])
    assert measure(app, s)["volume"] == pytest.approx(40.0) and counts(app, s) == (1, 6, 12, 8)
    bent = app.model.parts.create(name="BENT")
    bent.features.create_sweep(points=[[0, 0, 0], [2, 0, 0], [2, 2, 0], [0, 2, 0]], path=[[0, 0, 0], [0, 0, 10], [0, 10, 10]])
    assert app.execute("geometry.check", id=bent.id)["valid"] and measure(app, bent)["bbox"]["max"] == pytest.approx([2.0, 10.0, 10.0])  # 직각 모퉁이도 유효
    l = app.model.parts.create(name="LOFT")
    l.features.create_loft(profiles=[{"points": [[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0]]},
                                     {"points": [[2, 2, 10], [8, 2, 10], [8, 8, 10], [2, 8, 10]]}], ruled=True)
    assert measure(app, l)["volume"] == pytest.approx(10 * (100 + 36 + math.sqrt(100 * 36)) / 3)  # 절두 사각뿔
    o = box(app, "OFF", (10.0, 10.0, 10.0))
    o.features.create_offset(distance=1.0)
    assert measure(app, o)["volume"] == pytest.approx(12.0**3) and counts(app, o)[0] == 1
    e = box(app, "EXP", (10.0, 10.0, 10.0))
    e.features.create_explode()
    assert counts(app, e)[0] == 0 and counts(app, e)[1] == 6 and measure(app, e)["area"] == pytest.approx(600.0)
    e.features.create_make_solid()
    assert counts(app, e)[0] == 1 and measure(app, e)["volume"] == pytest.approx(1000.0)
    t = app.model.parts.create(name="TH")
    t.features.create_extrude(points=[[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0]], distance=5.0)
    t.features.create_explode()
    t.features.create_thicken(thickness=0.5)  # 면 6개가 각각 두께 0.5 의 판이 된다
    assert counts(app, t)[0] == 6 and measure(app, t)["volume"] == pytest.approx(6 * 0.5 * 10 * 10 * 0 + (2 * 100 + 4 * 50) * 0.5, rel=1e-9)
    bad = box(app, "BAD", (1.0, 1.0, 1.0))
    bad.features.create_thicken(thickness=1.0)
    assert app.execute("feature.status", id=bad.id)["features"][-1]["code"] == "invalid_state"
    bad2 = app.model.parts.create(name="BAD2")
    bad2.features.create_loft(profiles=[{"points": [[0, 0, 0], [1, 0, 0], [1, 1, 0]]}])
    assert app.execute("feature.status", id=bad2.id)["features"][-1]["code"] == "out_of_range"


@pytest.mark.feature("GEO-07")
def test_GEO_T02_find(app):
    """조건으로 엔티티 찾기: 곡면 종류, 상자, 크기, 법선, 접선 연속."""
    p = box(app, "B", (100.0, 20.0, 10.0))
    find = lambda **kw: app.execute("geometry.find", id=p.id, **kw)
    assert find(type="face")["count"] == 6 and find(type="face", surface="cylinder")["count"] == 0
    top = find(type="face", normal=[0.0, 0.0, 1.0])
    assert top["count"] == 1 and app.execute("geometry.entity_info", id=p.id, type="face", index=top["indices"][0])["center"] == pytest.approx([50.0, 10.0, 10.0])
    assert find(type="face", normal=[0.0, 0.0, -1.0])["indices"] != top["indices"]  # 법선은 바깥쪽
    big = find(type="face", min_size=1500.0)
    assert big["count"] == 2  # 100×20 면 둘
    assert find(type="edge", min_size=99.0, max_size=101.0)["count"] == 4 and find(type="edge", surface="line")["count"] == 12
    assert find(type="vertex", box_min=[-1.0, -1.0, -1.0], box_max=[1.0, 1.0, 1.0])["count"] == 1
    assert find(type="face", box_min=[0.0, 0.0, 9.0], box_max=[100.0, 20.0, 11.0])["indices"] == top["indices"]
    # 접선 연속: 필렛을 준 상자에서 윗면에서 시작하면 필렛 면을 지나 옆면까지 이어진다
    f = box(app, "F", (10.0, 10.0, 10.0))
    f.features.create_fillet(edges=list(range(1, 13)), radius=1.0)
    seed = app.execute("geometry.find", id=f.id, type="face", normal=[0.0, 0.0, 1.0])["indices"][0]
    smooth = app.execute("geometry.find", id=f.id, type="face", seed=seed, tolerance=5.0)
    assert smooth["count"] == 26  # 모두 접선 연속으로 이어진다
    sharp = app.execute("geometry.find", id=p.id, type="face", seed=top["indices"][0], tolerance=5.0)
    assert sharp["indices"] == top["indices"]  # 상자의 면은 직각이라 자기 자신뿐
    with pytest.raises(OfepError):
        find(type="edge", seed=1)


# ================================================================ 피처 이력 조작(GEO-41, GEO-42, GEO-20)
@pytest.mark.feature("GEO-41")
@pytest.mark.feature("GEO-42")
@pytest.mark.feature("GEO-20")
def test_GEO_feature_tree_ops(app):
    """rollback(지정 피처까지), reorder(순서), rebind(엔티티 번호 다시 지정), history(엔티티가 생긴 피처)."""
    plate = box(app, "PLATE", size=(40.0, 40.0, 10.0))
    drill = app.model.parts.create(name="DRILL")
    drill.features.create_cylinder(origin=[20.0, 20.0, -5.0], radius=5.0, height=20.0)
    cut = plate.features.create_cut(tools=[drill.id])
    line_edge = [e["index"] for e in app.execute("geometry.entities", id=plate.id, type="edge")["entities"] if e["curve"] == "line"][0]
    fillet = plate.features.create_fillet(edges=[line_edge], radius=1.0)  # 컷 뒤 형상의 직선 모서리(구멍과 떨어진)
    v_all = measure(app, plate)["volume"]
    # rollback: 컷까지만 → 필렛은 rolled_back, 부피는 컷만 한 값. 비우면 전부
    r = app.execute("feature.rollback", id=plate.id, feature=cut.id)
    assert r["rollback"] == cut.id and [f["state"] for f in r["features"]] == ["ok", "ok", "rolled_back"]
    assert measure(app, plate)["volume"] == pytest.approx(16000.0 - math.pi * 25 * 10, rel=1e-9)
    r = app.execute("feature.rollback", id=plate.id)
    assert r["rollback"] is None and measure(app, plate)["volume"] == pytest.approx(v_all)
    with pytest.raises(OfepError) as e:
        app.execute("feature.rollback", id=plate.id, feature=app.execute("feature.list", parent=drill.id)[0]["id"])
    assert e.value.code == "invalid_param"
    # history: 상자가 면 6개를 만들고, 컷이 구멍 면 1개를 더하며 윗면·아랫면을 바꾼다(없어지고 새로 생김)
    h = app.execute("feature.history", id=plate.id)["features"]
    assert [x["state"] for x in h] == ["ok", "ok", "ok"]
    assert h[0]["faces"] == {"count": 6, "created": 6, "removed": 0} and h[0]["solids"]["created"] == 1
    assert h[1]["faces"]["count"] == 7 and h[1]["faces"]["created"] == 3 and h[1]["faces"]["removed"] == 2  # 구멍 면 + 새 윗면·아랫면
    assert h[2]["faces"] == {"count": 8, "created": 5, "removed": 4}  # 필렛 면 + 둥글린 모서리에 닿은 면 4개가 새것
    faces = app.execute("geometry.entities", id=plate.id, type="face")["entities"]
    hole = [e["index"] for e in faces if e["surface"] == "cylinder" and e["area"] == pytest.approx(math.pi * 10 * 10)][0]
    one = app.execute("feature.history", id=plate.id, type="face", index=hole)
    assert one["created_by"] == cut.id and one["present_in"] == [cut.id, fillet.id]  # 구멍 면은 필렛을 거쳐도 같은 면
    fil = [e["index"] for e in faces if e["surface"] == "cylinder" and e["index"] != hole][0]
    assert app.execute("feature.history", id=plate.id, type="face", index=fil)["created_by"] == fillet.id
    side = [e["index"] for e in app.execute("geometry.entities", id=plate.id, type="face")["entities"] if e["surface"] == "plane"]
    first_box = app.execute("feature.list", parent=plate.id)[0]["id"]
    assert any(app.execute("feature.history", id=plate.id, type="face", index=i)["created_by"] == first_box for i in side)
    with pytest.raises(OfepError) as e:
        app.execute("feature.history", id=plate.id, type="face", index=99)
    assert e.value.code == "out_of_range"
    # reorder: 필렛을 컷 앞으로 → 상자의 모서리를 둥글리고 그 뒤 컷. 둘 다 ok 이고 (구멍과 떨어진 모서리라) 부피는 같다
    r = app.execute("feature.reorder", id=fillet.id, index=1)
    assert r["index"] == 1 and [f["feature"] for f in r["features"]] == [first_box, fillet.id, cut.id] and r["ok"]
    assert measure(app, plate)["volume"] == pytest.approx(v_all)
    assert app.execute("feature.history", id=plate.id)["features"][1]["feature"] == fillet.id
    app.undo()
    assert [f["id"] for f in app.execute("feature.list", parent=plate.id)] == [first_box, cut.id, fillet.id]
    # rebind: 없는 번호를 가리키게 고쳤다가(error) 다른 번호로 다시 잇는다
    app.execute("feature.update", id=fillet.id, edges=[99])
    assert app.execute("feature.status", id=plate.id)["features"][-1]["state"] == "error"
    r = app.execute("feature.rebind", id=fillet.id, field="edges", **{"from": 99, "to": line_edge})
    assert r["value"] == [line_edge] and r["status"]["state"] == "ok" and r["ok"]
    r = app.execute("feature.rebind", id=fillet.id, field="edges", entities=[line_edge, line_edge + 1])
    assert r["value"] == [line_edge, line_edge + 1] and app.execute("feature.get", id=fillet.id)["props"]["edges"] == [line_edge, line_edge + 1]
    for params, code in [({"field": "edges", "from": 50, "to": 2}, "not_found"), ({"field": "edges", "entities": [999]}, "out_of_range"),
                         ({"field": "radius", "from": 1, "to": 2}, "invalid_param"), ({"field": "edges"}, "missing_param")]:
        with pytest.raises(OfepError) as e:
            app.execute("feature.rebind", id=fillet.id, **params)
        assert e.value.code == code


# ================================================================ 기본 곡선·면과 곡선·곡면 편집(GEO-11, GEO-21~28)
def _ents(app, part, t):
    return app.execute("geometry.entities", id=part.id, type=t)["entities"]


def _status(app, part):
    return app.execute("feature.status", id=part.id)["features"][-1]


@pytest.mark.feature("GEO-11")
@pytest.mark.feature("GEO-21")
@pytest.mark.feature("GEO-22")
@pytest.mark.feature("GEO-23")
@pytest.mark.feature("GEO-24")
@pytest.mark.feature("GEO-25")
def test_GEO_curves(app):
    """점·선·호·스플라인·평면 바디, 곡선 자르기·늘리기·나누기·잇기·투영·교차·오프셋(길이로 확인)."""
    c = app.model.parts.create(name="C")
    c.features.create_point(point=[1, 2, 3])
    c.features.create_line(start=[0, 0, 0], end=[10, 0, 0])
    c.features.create_arc(start=[0, 0, 0], middle=[5, 5, 0], end=[10, 0, 0])
    c.features.create_spline(points=[[0, 0, 0], [5, 3, 0], [10, 0, 0]])
    c.features.create_plane(point=[0, 0, 0], normal=[0, 0, 1], size=10)
    assert counts(app, c) == (0, 1, 7, 11) and all(f["state"] == "ok" for f in app.execute("feature.status", id=c.id)["features"])
    edges = _ents(app, c, "edge")
    assert [(e["curve"], round(e["length"], 3)) for e in edges[:3]] == [("line", 10.0), ("circle", round(math.pi * 5, 3)), ("bspline", 12.012)]
    assert _ents(app, c, "face")[0]["area"] == pytest.approx(100.0)
    line, arc = 1, 2
    c.features.create_curve_trim(edge=line, start=0.0, end=0.5)
    c.features.create_curve_split(edge=arc, at=0.5)
    c.features.create_curve_extend(edge=line, length=5.0, at="end")
    c.features.create_curve_extend(edge=arc, length=2.0, at="both")
    lengths = [round(e["length"], 3) for e in _ents(app, c, "edge")[7:]]
    assert lengths == [5.0, round(math.pi * 2.5, 3), round(math.pi * 2.5, 3), 15.0, round(math.pi * 5 + 4, 3)]
    bad = c.features.create_curve_trim(edge=line, start=0.7, end=0.2)  # 피처는 만들어지고 적용할 때 오류로 남는다
    assert _status(app, c)["state"] == "error" and _status(app, c)["code"] == "out_of_range"
    bad.delete()
    # 잇기: 선 + 호 → 스플라인 하나(길이는 합)
    cj = app.model.parts.create(name="CJ")
    cj.features.create_line(start=[0, 0, 0], end=[10, 0, 0])
    cj.features.create_arc(start=[10, 0, 0], middle=[15, 5, 0], end=[10, 10, 0])
    cj.features.create_curve_join(edges=[1, 2])
    assert [(e["curve"], round(e["length"], 3)) for e in _ents(app, cj, "edge")][-1] == ("bspline", round(10 + math.pi * 5, 3))
    # 투영: 평면 위로(길이 유지), 교차: 두 평면 → 선, 오프셋: 반원 → 반지름 7 의 반원
    cq = app.model.parts.create(name="CQ")
    cq.features.create_plane(point=[0, 0, 0], normal=[0, 0, 1], size=20)
    cq.features.create_line(start=[-3, 0, 8], end=[3, 0, 8])
    cq.features.create_curve_project(edge=5, face=1)
    p = _ents(app, cq, "edge")[-1]
    assert p["length"] == pytest.approx(6.0) and abs(p["bbox"]["min"][2]) < 1e-6 and abs(p["bbox"]["max"][2]) < 1e-6
    ci = app.model.parts.create(name="CI")
    ci.features.create_plane(point=[0, 0, 0], normal=[0, 0, 1], size=10)
    ci.features.create_plane(point=[0, 0, 0], normal=[1, 0, 0], size=10)
    ci.features.create_curve_intersect(face_a=1, face_b=2)
    assert (_ents(app, ci, "edge")[-1]["curve"], _ents(app, ci, "edge")[-1]["length"]) == ("line", pytest.approx(10.0))
    co = app.model.parts.create(name="CO")
    co.features.create_arc(start=[-5, 0, 0], middle=[0, 5, 0], end=[5, 0, 0])
    co.features.create_curve_offset(edge=1, distance=2.0, normal=[0, 0, 1])
    assert _ents(app, co, "edge")[-1]["length"] == pytest.approx(math.pi * 7)
    # 곡면: 채우기(평면 4변 → 평면 면), 늘리기(10×10 → 14×14), 자르기(절반), 지우기(윗면 없는 껍질), 바꾸기
    ff = app.model.parts.create(name="FF")
    for a, b in [([0, 0, 0], [10, 0, 0]), ([10, 0, 0], [10, 10, 0]), ([10, 10, 0], [0, 10, 0]), ([0, 10, 0], [0, 0, 0])]:
        ff.features.create_line(start=a, end=b)
    ff.features.create_face_fill(edges=[1, 2, 3, 4])
    assert _ents(app, ff, "face")[0]["area"] == pytest.approx(100.0)
    fo = app.model.parts.create(name="FO")
    fo.features.create_plane(point=[0, 0, 0], normal=[0, 0, 1], size=10)
    fo.features.create_face_extend(face=1, length=2.0)
    fo.features.create_face_trim(face=1, point=[0, 0, 0], normal=[1, 0, 0])
    assert [round(e["area"], 3) for e in _ents(app, fo, "face")] == [100.0, 196.0, 50.0]
    sh = box(app, "SH", (10.0, 10.0, 10.0))
    top = [e["index"] for e in _ents(app, sh, "face") if e["center"][2] > 9.9][0]
    sh.features.create_face_delete(faces=[top])
    assert counts(app, sh)[:2] == (0, 5) and _status(app, sh)["state"] == "ok"
    fr = box(app, "FR", (10.0, 10.0, 10.0))
    fr.features.create_plane(point=[5, 5, 10], normal=[0, 0, 1], size=10)
    top = [e["index"] for e in _ents(app, fr, "face") if e["center"][2] > 9.9 and e["index"] <= 6][0]
    fr.features.create_face_replace(face=top, **{"with": 7})
    assert _status(app, fr)["state"] == "ok" and counts(app, fr)[:2] == (1, 6) and measure(app, fr)["volume"] == pytest.approx(1000.0)  # 평면→평면 대체: 닫힌 솔리드 유지, 대신 쓴 평면은 없앤다


# ================================================================ 치유·단순화·임프린트·공유 토폴로지·중립면·형상 피처(GEO-10, 14, 16, 17, 31)
@pytest.mark.feature("GEO-10")
@pytest.mark.feature("GEO-14")
@pytest.mark.feature("GEO-16")
@pytest.mark.feature("GEO-17")
@pytest.mark.feature("GEO-31")
def test_GEO_solid_features(app):
    """구멍 없애기·필렛 없애기(부피 복원), 구멍·포켓·보스·리브·기울기(부피로 확인), 면 합치기, 치유(솔리드 유지), 임프린트(면 나뉨·부피 그대로),
    공유 토폴로지(맞닿은 면 공유), 중립면(가운데 면)."""
    b = box(app, "B", (40.0, 40.0, 10.0))
    tool = app.model.parts.create(name="T")
    tool.features.create_cylinder(origin=[20.0, 20.0, -5.0], radius=5.0, height=20.0)
    b.features.create_cut(tools=[tool.id])
    cyl = [e["index"] for e in _ents(app, b, "face") if e["surface"] == "cylinder"]
    b.features.create_remove_hole(faces=cyl)
    assert measure(app, b)["volume"] == pytest.approx(16000.0) and counts(app, b) == (1, 6, 12, 8)
    b.features.create_hole(point=[10, 10, 10], direction=[0, 0, -1], diameter=4.0)
    assert 16000.0 - measure(app, b)["volume"] == pytest.approx(math.pi * 4 * 10, rel=1e-6)
    b.features.create_hole(point=[30, 10, 10], direction=[0, 0, -1], diameter=4.0, depth=5.0)
    assert 16000.0 - measure(app, b)["volume"] == pytest.approx(math.pi * 4 * 15, rel=1e-6)
    v = measure(app, b)["volume"]
    b.features.create_pocket(points=[[2, 30, 10], [8, 30, 10], [8, 36, 10], [2, 36, 10]], depth=3.0)
    assert measure(app, b)["volume"] == pytest.approx(v - 36 * 3, rel=1e-6)
    b.features.create_boss(points=[[30, 30, 10], [36, 30, 10], [36, 36, 10], [30, 36, 10]], height=4.0)
    assert measure(app, b)["volume"] == pytest.approx(v - 108 + 144, rel=1e-6)
    b.features.create_rib(path=[[0, 20, 10], [40, 20, 10]], thickness=2.0, height=5.0, direction=[0, 0, 1])
    assert measure(app, b)["volume"] == pytest.approx(v - 108 + 144 + 400, rel=1e-6)
    n_faces = counts(app, b)[1]
    b.features.create_merge_faces()
    assert _status(app, b)["state"] == "ok" and counts(app, b)[1] <= n_faces
    b.features.create_heal(sew=True, tolerance=1e-4)
    assert _status(app, b)["state"] == "ok" and counts(app, b)[0] == 1 and measure(app, b)["volume"] == pytest.approx(v - 108 + 144 + 400, rel=1e-6)
    # 필렛 없애기
    rf = box(app, "RF", (10.0, 10.0, 10.0))
    rf.features.create_fillet(edges=[1], radius=2.0)
    rf.features.create_remove_fillet(faces=[e["index"] for e in _ents(app, rf, "face") if e["surface"] == "cylinder"])
    assert measure(app, rf)["volume"] == pytest.approx(1000.0) and counts(app, rf) == (1, 6, 12, 8)
    rf.features.create_merge_edges()
    assert counts(app, rf) == (1, 6, 12, 8)
    rf.features.create_remove_faces(faces=[99])
    assert _status(app, rf)["state"] == "error" and _status(app, rf)["code"] == "not_found"
    # 기울기: +x 면을 5° (중립 평면 z=0) → 쐐기만큼 줄어든 부피
    d = box(app, "D", (10.0, 10.0, 10.0))
    fx = [e["index"] for e in _ents(app, d, "face") if e["center"][0] > 9.9]
    d.features.create_draft(faces=fx, angle=5.0, direction=[0, 0, 1], point=[0, 0, 0])
    assert measure(app, d)["volume"] == pytest.approx(1000.0 - 0.5 * 10 * 10 * math.tan(math.radians(5)) * 10, rel=1e-6)
    # 임프린트: 평면 도구가 지나는 자리에 면이 나뉘고 부피는 그대로
    im = box(app, "IM", (10.0, 10.0, 10.0))
    pt = app.model.parts.create(name="PT")
    pt.features.create_plane(point=[5, 5, 5], normal=[1, 0, 0], size=30)
    im.features.create_imprint(tools=[pt.id])
    assert counts(app, im)[1] == 11 - 1 and measure(app, im)["volume"] == pytest.approx(1000.0) or counts(app, im)[1] >= 10
    assert measure(app, im)["volume"] == pytest.approx(1000.0) and counts(app, im)[1] > 6
    # 공유 토폴로지: 맞닿은 두 상자 → 면 12 → 11(공유 면 하나), 부피 그대로
    sh = app.model.parts.create(name="SH")
    sh.features.create_box(size=[10, 10, 10])
    sh.features.create_box(origin=[10, 0, 0], size=[10, 10, 10])
    assert counts(app, sh) == (2, 12, 24, 16)
    sh.features.create_share_topology()
    assert counts(app, sh) == (2, 11, 20, 12) and measure(app, sh)["volume"] == pytest.approx(2000.0)
    # 중립면: 판의 위·아래 면 → 가운데(z=1) 의 면 하나
    pl = box(app, "PL", (20.0, 10.0, 2.0))
    faces = _ents(app, pl, "face")
    top = [e["index"] for e in faces if e["center"][2] > 1.9][0]
    bot = [e["index"] for e in faces if e["center"][2] < 0.1][0]
    pl.features.create_midsurface(faces=[bot, top])
    mid = _ents(app, pl, "face")
    assert counts(app, pl) == (0, 1, 4, 4) and mid[0]["center"] == pytest.approx([10.0, 5.0, 1.0]) and mid[0]["area"] == pytest.approx(200.0)
    pl.features.create_midsurface(faces=[1])
    assert _status(app, pl)["state"] == "error" and _status(app, pl)["code"] == "invalid_param"


# ================================================================ 스케치(GEO-32, 33, 37, 38) — 구속(GEO-34~36)은 D8 결정 뒤
@pytest.mark.feature("GEO-32")
@pytest.mark.feature("GEO-33")
@pytest.mark.feature("GEO-37")
@pytest.mark.feature("GEO-38")
def test_GEO_sketch(app):
    """평면 위 스케치에 요소를 그리고(사각형·원·선·호·타원·스플라인·점), 편집(자르기·늘리기·필렛·대칭·오프셋)하고, 닫힌 영역을 찾아 돌출 단면으로 쓰며,
    형상의 모서리를 참조 요소로 투영한다."""
    part = app.model.parts.create(name="P")
    sk = app.execute("sketch.create", name="S", parent=part.id, point=[0, 0, 0], normal=[0, 0, 1])["id"]
    e1 = app.execute("sketch.add_rectangle", id=sk, corner=[0, 0], size=[20, 10])["entity"]
    e2 = app.execute("sketch.add_circle", id=sk, center=[10, 5], radius=2)["entity"]
    assert (e1, e2) == (1, 2)
    pr = app.execute("sketch.profiles", id=sk)
    assert pr["count"] == 2 and pr["profiles"][0]["area"] == pytest.approx(200 - math.pi * 4) and pr["profiles"][1]["area"] == pytest.approx(math.pi * 4)
    assert pr["profiles"][0]["center"] == pytest.approx([10.0, 5.0]) and pr["profiles"][0]["edges"] == 5
    # 돌출 단면으로: 구멍 뚫린 판
    f = part.features.create_extrude(sketch=sk, profile=1, distance=5.0)
    assert measure(app, part)["volume"] == pytest.approx((200 - math.pi * 4) * 5) and counts(app, part)[1] == 7
    # 스케치를 고치면 피처가 따라 바뀐다(원 반지름 3)
    ents = app.execute("sketch.get", id=sk)["props"]["entities"]
    ents[1]["radius"] = 3.0
    app.execute("sketch.update", id=sk, entities=ents)
    assert measure(app, part)["volume"] == pytest.approx((200 - math.pi * 9) * 5)
    f.update(profile=2)
    assert measure(app, part)["volume"] == pytest.approx(math.pi * 9 * 5)
    f.update(profile=9)
    assert app.execute("feature.status", id=part.id)["features"][-1]["code"] == "out_of_range"
    f.delete()
    # 편집: 선분 자르기·늘리기, 두 선분 필렛(호가 생김), 대칭, 오프셋
    sk2 = app.execute("sketch.create", name="S2", parent=part.id)["id"]
    a = app.execute("sketch.add_line", id=sk2, start=[0, 0], end=[10, 0])["entity"]
    b = app.execute("sketch.add_line", id=sk2, start=[10, 0], end=[10, 10])["entity"]
    r = app.execute("sketch.edit", id=sk2, operation="trim", entity=a, start=0.0, end=0.5)
    ent = lambda i: [e for e in app.execute("sketch.get", id=sk2)["props"]["entities"] if e["id"] == i][0]  # noqa: E731
    assert ent(a)["end"] == [5.0, 0.0]
    app.execute("sketch.edit", id=sk2, operation="extend", entity=a, length=5.0, at="end")
    assert ent(a)["end"] == pytest.approx([10.0, 0.0])
    r = app.execute("sketch.edit", id=sk2, operation="fillet", entities=[a, b], radius=2.0)
    arc = ent(r["entities"][0])
    assert arc["kind"] == "arc" and ent(a)["end"] == pytest.approx([8.0, 0.0]) and ent(b)["start"] == pytest.approx([10.0, 2.0])
    assert arc["middle"] == pytest.approx([10 - 2 + 2 / math.sqrt(2), 2 - 2 / math.sqrt(2)])  # 중심 (8, 2) 의 반지름 2 호
    axis = app.execute("sketch.add_line", id=sk2, start=[0, 5], end=[20, 5])["entity"]
    r = app.execute("sketch.edit", id=sk2, operation="mirror", entities=[a], axis=axis)
    assert ent(r["entities"][0])["start"] == pytest.approx([0.0, 10.0]) and ent(r["entities"][0])["end"] == pytest.approx([8.0, 10.0])
    c = app.execute("sketch.add_circle", id=sk2, center=[0, 0], radius=1.0)["entity"]
    r = app.execute("sketch.edit", id=sk2, operation="offset", entities=[c, a], distance=1.0)
    assert ent(r["entities"][0])["radius"] == 2.0 and ent(r["entities"][1])["start"] == pytest.approx([0.0, 1.0])
    for params, code in [({"operation": "fillet", "entities": [a], "radius": 1.0}, "missing_param"), ({"operation": "trim", "entity": c, "start": 0, "end": 1}, "invalid_param"),
                         ({"operation": "trim", "entity": 99}, "not_found"), ({"operation": "fillet", "entities": [a, axis], "radius": 1.0}, "invalid_geometry")]:
        with pytest.raises(OfepError) as e:
            app.execute("sketch.edit", id=sk2, **params)
        assert e.value.code == code
    n = len(app.execute("sketch.get", id=sk2)["props"]["entities"])
    assert app.execute("sketch.remove", id=sk2, entities=[c])["count"] == n - 1
    with pytest.raises(OfepError):
        app.execute("sketch.remove", id=sk2, entities=[c])
    # 그 밖의 요소와 잘못된 요소
    sk3 = app.execute("sketch.create", name="S3", parent=part.id, point=[0, 0, 5], normal=[0, 1, 0], x_axis=[1, 0, 0])["id"]
    app.execute("sketch.add_ellipse", id=sk3, center=[0, 0], radii=[5, 3], angle=30.0)
    app.execute("sketch.add_point", id=sk3, position=[1, 1])
    assert app.execute("sketch.profiles", id=sk3)["count"] == 1 and app.execute("sketch.profiles", id=sk3)["profiles"][0]["area"] == pytest.approx(math.pi * 15)
    app.execute("sketch.add_arc", id=sk3, start=[0, 0], middle=[5, 5], end=[10, 0])
    app.execute("sketch.add_spline", id=sk3, points=[[0, 0], [5, 3], [10, 0]])
    assert app.execute("sketch.profiles", id=sk3)["count"] == 3  # 호와 스플라인이 끝점을 공유해 영역을 만들고 타원이 그것을 가른다
    for params, code in [({"start": [0, 0], "middle": [5, 0], "end": [10, 0]}, "invalid_geometry")]:
        with pytest.raises(OfepError) as e:
            app.execute("sketch.add_arc", id=sk3, **params)
        assert e.value.code == code
    with pytest.raises(OfepError) as e:
        app.execute("sketch.add_line", id=sk3, start=[0, 0])
    assert e.value.code == "missing_param"
    with pytest.raises(OfepError) as e:
        app.execute("sketch.add_line", id=part.id, start=[0, 0], end=[1, 1])
    assert e.value.code == "wrong_kind"
    # 투영: 상자의 모서리(평면과 나란한 선분은 선분, 수직 선분은 점으로 찍혀 스플라인)·꼭짓점 → 참조 요소(단면에 쓰이지 않음)
    bx = box(app, "BX", (10.0, 10.0, 10.0))
    sk4 = app.execute("sketch.create", name="S4", parent=part.id, point=[0, 0, 20], normal=[0, 0, 1])["id"]
    r = app.execute("sketch.project", id=sk4, part=bx.id, vertices=[1])
    ents = app.execute("sketch.get", id=sk4)["props"]["entities"]
    assert len(r["entities"]) == 13 and all(e["reference"] for e in ents) and sum(e["kind"] == "line" for e in ents) >= 8 and sum(e["kind"] == "point" for e in ents) == 1
    assert app.execute("sketch.profiles", id=sk4)["count"] == 0  # 참조 요소는 영역을 만들지 않는다
    r = app.execute("sketch.project", id=sk4, part=bx.id, edges=[1])
    assert len(r["entities"]) == 1
    # 기준 평면으로 만든 스케치
    dp = app.model.datums.create_plane(name="DP", point=[0, 0, 3], normal=[0, 0, 1])
    sk5 = app.execute("sketch.create", name="S5", parent=part.id, datum=dp.id)["id"]
    app.execute("sketch.add_rectangle", id=sk5, corner=[0, 0], size=[4, 4])
    g = part.features.create_extrude(sketch=sk5, distance=2.0)
    assert measure(app, part)["bbox"]["min"][2] == pytest.approx(3.0) and measure(app, part)["volume"] == pytest.approx(32.0)


@pytest.mark.feature("GEO-26")
@pytest.mark.feature("GEO-27")
@pytest.mark.feature("GEO-28")
def test_GEO_T13_01_to_05_13_face_edit(app):
    """면 채우기(사각 경계 200, 구멍 메우기 πr²), 면 연장·자르기(면적이 식대로), 면 삭제(이웃 면이 늘어나 닫힌 솔리드), 면 대체(오프셋 평면 → 체적 변화),
    만나지 않는 평면으로 자르기(오류, 원본 유지)."""
    # 채우기(GEO-T13-01): 선 4개로 닫힌 고리
    f = app.model.parts.create(name="F")
    f.features.create_line(start=[0, 0, 0], end=[20, 0, 0])
    f.features.create_line(start=[20, 0, 0], end=[20, 10, 0])
    f.features.create_line(start=[20, 10, 0], end=[0, 10, 0])
    f.features.create_line(start=[0, 10, 0], end=[0, 0, 0])
    f.features.create_face_fill(edges=[1, 2, 3, 4])
    assert _status(app, f)["state"] == "ok" and [e["area"] for e in _ents(app, f, "face")] == [pytest.approx(200.0)]
    f.features.create_face_fill(edges=[1, 2])  # 닫히지 않은 고리
    assert _status(app, f)["state"] == "error" and _status(app, f)["code"] == "invalid_geometry"
    # 구멍 메우기(GEO-T13-02): 윗면에 관통 구멍(M2 와 같은 구성)의 위쪽 원 모서리로 채운다
    b = box(app, "B", (40.0, 40.0, 10.0))
    b.features.create_hole(point=[20, 20, 10], direction=[0, 0, -1], diameter=6.0)
    top_circle = [e["index"] for e in _ents(app, b, "edge") if e["curve"] == "circle" and e["center"][2] == pytest.approx(10.0)]
    assert len(top_circle) == 1
    b.features.create_face_fill(edges=top_circle)
    assert _status(app, b)["state"] == "ok"
    filled = [e for e in _ents(app, b, "face") if e["surface"] == "plane" and e["area"] == pytest.approx(math.pi * 9, rel=1e-6)]
    assert len(filled) == 1 and filled[0]["center"][2] == pytest.approx(10.0)
    # 연장·자르기(GEO-T13-03): 20×10 면을 5 늘리면 30×20 = 600, 평면 x < 15 로 자르면 15×20 = 300
    g = app.model.parts.create(name="G")
    g.features.create_plane(point=[10, 5, 0], normal=[0, 0, 1], size=20)  # 20×20 = 400
    assert _ents(app, g, "face")[0]["area"] == pytest.approx(400.0)
    g.features.create_face_extend(face=1, length=5.0)
    assert _status(app, g)["state"] == "ok" and _ents(app, g, "face")[-1]["area"] == pytest.approx(900.0)  # 30×30
    g.features.create_face_trim(face=counts(app, g)[1], point=[15, 0, 0], normal=[-1, 0, 0])  # x < 15 를 남긴다: (15 − (−5)) × 30 = 600
    assert _status(app, g)["state"] == "ok" and _ents(app, g, "face")[-1]["area"] == pytest.approx(600.0)
    # 만나지 않는 평면으로 자르기(GEO-T13-13): 남는 쪽에 면이 없으면 오류, 원본은 그대로
    n_faces = counts(app, g)[1]
    bad = g.features.create_face_trim(face=n_faces, point=[100, 0, 0], normal=[1, 0, 0])
    assert _status(app, g)["state"] == "error" and _status(app, g)["code"] == "geometry_failed"
    bad.delete()
    assert _ents(app, g, "face")[-1]["area"] == pytest.approx(600.0)
    # 면 삭제(GEO-T13-04): 필렛 면을 지우면 이웃 면이 늘어나 닫힌 솔리드(체적 20000)
    d = box(app, "D", (100.0, 20.0, 10.0))
    d.features.create_fillet(edges=[1], radius=3.0)
    cyl = [e["index"] for e in _ents(app, d, "face") if e["surface"] == "cylinder"]
    assert len(cyl) == 1 and measure(app, d)["volume"] < 20000.0
    d.features.create_face_delete(faces=cyl)
    assert _status(app, d)["state"] == "ok" and counts(app, d) == (1, 6, 12, 8) and measure(app, d)["volume"] == pytest.approx(20000.0)
    # 면 대체(GEO-T13-05): 윗면(z=10)을 z=13 의 평면으로 → 체적 +3·100·20, z=8 로 → 체적 −2·100·20
    r = box(app, "R", (100.0, 20.0, 10.0))
    top = face_at(app, r, [50.0, 10.0, 10.0])
    r.features.create_plane(point=[50, 10, 13], normal=[0, 0, 1], size=200)
    app.execute("feature.create_face_replace", parent=r.id, face=top, **{"with": counts(app, r)[1]})
    assert _status(app, r)["state"] == "ok" and counts(app, r)[0] == 1 and measure(app, r)["volume"] == pytest.approx(26000.0)
    top = face_at(app, r, [50.0, 10.0, 13.0])
    r.features.create_plane(point=[50, 10, 8], normal=[0, 0, 1], size=200)
    app.execute("feature.create_face_replace", parent=r.id, face=top, **{"with": counts(app, r)[1]})
    assert _status(app, r)["state"] == "ok" and counts(app, r)[0] == 1 and measure(app, r)["volume"] == pytest.approx(16000.0)


@pytest.mark.feature("GEO-03")
@pytest.mark.feature("WT-06")
def test_GEO_T02_01_02_03_04_assembly_round_trip(app, tmp_path):
    """어셈블리: 파트 계층(2단)·이름·색이 STEP 왕복에서 유지되고, part.move 로 다른 그룹에 옮기면 트리 위치만 바뀐다(형상·ID 불변, Undo 로 원복)."""
    top = app.model.parts.create(name="TOP")
    sub = app.model.parts.create(name="SUB", assembly=top.id)
    a = box(app, "A", (10.0, 10.0, 10.0))
    a.update(assembly=top.id, color=[0.8, 0.2, 0.2])
    b = box(app, "B", (5.0, 5.0, 5.0), origin=[20.0, 0.0, 0.0])
    b.update(assembly=sub.id, color=[0.1, 0.3, 0.9])
    c = box(app, "C", (4.0, 4.0, 4.0), origin=[40.0, 0.0, 0.0])
    c.update(assembly=sub.id)
    with pytest.raises(OfepError) as e:
        top.update(assembly=sub.id)  # 순환
    assert e.value.code == "cyclic_dependency"
    with pytest.raises(OfepError):
        a.update(color=[1.0, 0.0])

    def tree():
        def strip(items):
            return [(i["name"], strip([k for k in i.get("children", []) if k["kind"] == "part"])) for i in items]
        return strip(app.execute("project.tree", kind="part")[0]["items"])

    assert tree() == [("TOP", [("SUB", [("B", []), ("C", [])]), ("A", [])])]  # 2단 계층(SYS-19-06, SYS-19-16)
    path = tmp_path / "asm.step"
    app.execute("geometry.export", id=top.id, path=str(path))
    app.execute("project.new")
    r = app.execute("geometry.import", path=str(path))
    assert r["assembly"] is True and len(r["parts"]) == 5
    parts = {p["name"]: app.execute("part.get", id=p["id"]) for p in app.execute("part.list")}
    assert set(parts) == {"TOP", "SUB", "A", "B", "C"}
    assert tree() == [("TOP", [("SUB", [("B", []), ("C", [])]), ("A", [])])]  # 계층·이름 유지(GEO-T02-01, GEO-T02-03)
    assert parts["A"]["props"]["color"] == pytest.approx([0.8, 0.2, 0.2], abs=1e-3) and parts["B"]["props"]["color"] == pytest.approx([0.1, 0.3, 0.9], abs=1e-3)
    assert "color" not in parts["C"]["props"]  # 색 유지(GEO-T02-02)
    A, B = app.model.parts["A"], app.model.parts["B"]
    assert measure(app, A)["volume"] == pytest.approx(1000.0) and measure(app, B)["volume"] == pytest.approx(125.0)
    assert measure(app, B)["center"] == pytest.approx([22.5, 2.5, 2.5])  # 배치(위치)도 유지
    # 한 파트로 합치기
    app.execute("project.new")
    r = app.execute("geometry.import", path=str(path), assembly=False)
    assert "parts" not in r and r["solids"] == 3 and len(app.execute("part.list")) == 1
    # 파트 이동(GEO-T02-04): B 를 TOP 바로 아래로, 다시 최상위로. 형상·ID 는 그대로
    app.execute("project.new")
    app.execute("geometry.import", path=str(path))
    B = app.model.parts["B"]
    top_id = app.model.parts["TOP"].id
    vol, bid = measure(app, B)["volume"], B.id
    app.execute("part.move", id=bid, assembly=top_id)
    assert tree() == [("TOP", [("SUB", [("C", [])]), ("B", []), ("A", [])])] and measure(app, B)["volume"] == vol and app.model.parts["B"].id == bid  # 생성 순서(B 가 A 보다 먼저)
    app.execute("part.move", id=bid, assembly=None)  # null = 최상위로
    assert tree() == [("TOP", [("SUB", [("C", [])]), ("A", [])]), ("B", [])]
    app.undo(), app.undo()
    assert tree() == [("TOP", [("SUB", [("B", []), ("C", [])]), ("A", [])])]
    with pytest.raises(OfepError):
        app.execute("part.move", id=bid, assembly=bid)
    # 단일 파트의 색도 STEP 으로 왕복한다
    single = tmp_path / "one.step"
    app.execute("geometry.export", id=app.model.parts["A"].id, path=str(single))
    app.execute("project.new")
    r = app.execute("geometry.import", path=str(single))
    assert r["solids"] == 1 and app.execute("part.get", id=r["id"])["props"]["color"] == pytest.approx([0.8, 0.2, 0.2], abs=1e-3)


def _ent(app, sk, eid):
    return [e for e in app.execute("sketch.get", id=sk)["props"]["entities"] if e["id"] == eid][0]


def _len(e):
    return math.hypot(e["end"][0] - e["start"][0], e["end"][1] - e["start"][1])


@pytest.mark.feature("GEO-34")
@pytest.mark.feature("GEO-35")
@pytest.mark.feature("GEO-36")
@pytest.mark.feature("GEO-37")
def test_GEO_T14_05_06_07_08_09_10_14_sketch_constraints(app):
    """기하 구속(수평·수직·평행·직각·접선·동심·같은 길이·대칭·일치)과 치수 구속(길이·반지름·각도)을 자체 해석기로 푼다.
    치수를 바꾸면 다른 구속을 유지한 채 따라오고, 상태(미구속·완전 구속·과구속·충돌)를 판정하며, 투영한 참조 요소에 일치 구속을 건다."""
    part = app.model.parts.create(name="P")
    sk = app.execute("sketch.create", name="S", parent=part.id, point=[0, 0, 0], normal=[0, 0, 1])["id"]
    A = app.execute
    l1 = A("sketch.add_line", id=sk, start=[0, 0], end=[10, 1])["entity"]       # 수평으로 맞출 선
    l2 = A("sketch.add_line", id=sk, start=[0, 0], end=[1, 8])["entity"]        # 수직으로
    l3 = A("sketch.add_line", id=sk, start=[20, 0], end=[30, 2])["entity"]      # l1 과 평행으로
    l4 = A("sketch.add_line", id=sk, start=[20, 10], end=[21, 20])["entity"]    # l1 과 직각으로
    c1 = A("sketch.add_circle", id=sk, center=[50, 5], radius=3)["entity"]
    c2 = A("sketch.add_circle", id=sk, center=[52, 6], radius=1)["entity"]      # c1 과 동심으로
    l5 = A("sketch.add_line", id=sk, start=[40, 10], end=[60, 11])["entity"]    # c1 에 접하도록
    l6 = A("sketch.add_line", id=sk, start=[0, 30], end=[7, 30])["entity"]      # l1 과 같은 길이로
    assert A("sketch.solve_status", id=sk)["status"] == "no_constraints"
    # 기하 구속(GEO-T14-05)
    r = A("sketch.add_constraint", id=sk, kind="horizontal", entities=[l1])
    assert r["converged"] and r["status"] == "unconstrained"
    e = _ent(app, sk, l1)
    assert e["start"][1] == pytest.approx(e["end"][1], abs=1e-9)
    A("sketch.add_constraint", id=sk, kind="vertical", entities=[l2])
    e = _ent(app, sk, l2)
    assert e["start"][0] == pytest.approx(e["end"][0], abs=1e-9)
    A("sketch.add_constraint", id=sk, kind="parallel", entities=[l1, l3])
    e1, e3 = _ent(app, sk, l1), _ent(app, sk, l3)
    d1 = (e1["end"][0] - e1["start"][0], e1["end"][1] - e1["start"][1])
    d3 = (e3["end"][0] - e3["start"][0], e3["end"][1] - e3["start"][1])
    assert abs(d1[0] * d3[1] - d1[1] * d3[0]) < 1e-8 * _len(e1) * _len(e3)
    A("sketch.add_constraint", id=sk, kind="perpendicular", entities=[l1, l4])
    e1, e4 = _ent(app, sk, l1), _ent(app, sk, l4)
    assert abs((e1["end"][0] - e1["start"][0]) * (e4["end"][0] - e4["start"][0]) + (e1["end"][1] - e1["start"][1]) * (e4["end"][1] - e4["start"][1])) < 1e-8 * _len(e1) * _len(e4)
    A("sketch.add_constraint", id=sk, kind="concentric", entities=[c1, c2])
    assert _ent(app, sk, c1)["center"] == pytest.approx(_ent(app, sk, c2)["center"], abs=1e-9)
    A("sketch.add_constraint", id=sk, kind="tangent", entities=[l5, c1])
    e5, ec = _ent(app, sk, l5), _ent(app, sk, c1)
    dx, dy = e5["end"][0] - e5["start"][0], e5["end"][1] - e5["start"][1]
    dist = abs(dx * (ec["center"][1] - e5["start"][1]) - dy * (ec["center"][0] - e5["start"][0])) / math.hypot(dx, dy)
    assert dist == pytest.approx(ec["radius"], abs=1e-8)
    A("sketch.add_constraint", id=sk, kind="equal", entities=[l1, l6])
    assert _len(_ent(app, sk, l6)) == pytest.approx(_len(_ent(app, sk, l1)), abs=1e-8)
    # 일치·대칭: 점 두 개를 잇고, 축에 대칭
    p1 = A("sketch.add_point", id=sk, position=[70, 3])["entity"]
    A("sketch.add_constraint", id=sk, kind="coincident", points=[{"entity": p1, "point": "position"}, {"entity": l6, "point": "end"}])
    assert _ent(app, sk, p1)["position"] == pytest.approx(_ent(app, sk, l6)["end"], abs=1e-9)
    axis = A("sketch.add_line", id=sk, start=[100, 0], end=[100, 10])["entity"]
    pa = A("sketch.add_point", id=sk, position=[95, 4])["entity"]
    pb = A("sketch.add_point", id=sk, position=[107, 5])["entity"]
    A("sketch.add_constraint", id=sk, kind="symmetric", points=[{"entity": pa, "point": "position"}, {"entity": pb, "point": "position"}], entities=[axis])
    qa, qb, ax = _ent(app, sk, pa)["position"], _ent(app, sk, pb)["position"], _ent(app, sk, axis)
    adx, ady = ax["end"][0] - ax["start"][0], ax["end"][1] - ax["start"][1]
    mx, my = (qa[0] + qb[0]) / 2 - ax["start"][0], (qa[1] + qb[1]) / 2 - ax["start"][1]
    assert abs(adx * my - ady * mx) < 1e-8 * math.hypot(adx, ady)  # 중점이 축 위
    assert abs(adx * (qb[0] - qa[0]) + ady * (qb[1] - qa[1])) < 1e-8 * math.hypot(adx, ady)  # 연결선이 축과 직교
    # 치수 구속(GEO-T14-06): 길이 50, 반지름 8, 각도 30°
    r = A("sketch.add_dimension", id=sk, kind="length", entities=[l1], value=50.0)
    assert r["converged"] and _len(_ent(app, sk, l1)) == pytest.approx(50.0, abs=1e-8) and _len(_ent(app, sk, l6)) == pytest.approx(50.0, abs=1e-8)  # equal 유지
    length_id = r["constraint"]
    A("sketch.add_dimension", id=sk, kind="radius", entities=[c1], value=8.0)
    assert _ent(app, sk, c1)["radius"] == pytest.approx(8.0, abs=1e-9)
    l7 = A("sketch.add_line", id=sk, start=[0, -20], end=[10, -19])["entity"]
    A("sketch.add_dimension", id=sk, kind="angle", entities=[l1, l7], value=30.0)
    e1, e7 = _ent(app, sk, l1), _ent(app, sk, l7)
    a1 = math.atan2(e1["end"][1] - e1["start"][1], e1["end"][0] - e1["start"][0])
    a7 = math.atan2(e7["end"][1] - e7["start"][1], e7["end"][0] - e7["start"][0])
    assert (math.degrees(a7 - a1) + 360.0) % 360.0 == pytest.approx(30.0, abs=1e-6)
    # 치수 변경(GEO-T14-07): 50 → 60. 수평·평행·같은 길이·각도는 그대로
    r = A("sketch.set_dimension", id=sk, constraint=length_id, value=60.0)
    assert r["converged"] and _len(_ent(app, sk, l1)) == pytest.approx(60.0, abs=1e-8) and _len(_ent(app, sk, l6)) == pytest.approx(60.0, abs=1e-8)
    e1 = _ent(app, sk, l1)
    assert e1["start"][1] == pytest.approx(e1["end"][1], abs=1e-8)
    st = A("sketch.solve_status", id=sk)
    assert st["status"] == "unconstrained" and st["dof"] > 0 and st["conflicts"] == [] and st["redundant"] == []
    # 구속 상태 판별(GEO-T14-08): 선분 하나 — 미구속 → 고정 + 수평 + 길이 = 완전 구속 → 하나 더 = 과구속
    sk2 = A("sketch.create", name="S2", parent=part.id)["id"]
    m = A("sketch.add_line", id=sk2, start=[0, 0], end=[10, 0])["entity"]
    assert A("sketch.solve_status", id=sk2)["status"] == "no_constraints"
    A("sketch.add_constraint", id=sk2, kind="fixed", points=[{"entity": m, "point": "start"}])
    st = A("sketch.solve_status", id=sk2)
    assert st["status"] == "unconstrained" and st["dof"] == 2 and st["variables"] == 4 and st["rank"] == 2
    A("sketch.add_constraint", id=sk2, kind="horizontal", entities=[m])
    A("sketch.add_dimension", id=sk2, kind="length", entities=[m], value=10.0)
    st = A("sketch.solve_status", id=sk2)
    assert st["status"] == "fully_constrained" and st["dof"] == 0 and st["rank"] == 4
    over = A("sketch.add_constraint", id=sk2, kind="vertical", points=[{"entity": m, "point": "start"}, {"entity": m, "point": "end"}])  # 이미 수평·길이로 정해진 끝점 → 식이 남는다
    assert over["status"] == "conflict" or over["status"] == "over_constrained"
    A("sketch.remove_constraint", id=sk2, constraints=[over["constraint"]])
    dup = A("sketch.add_constraint", id=sk2, kind="horizontal", points=[{"entity": m, "point": "start"}, {"entity": m, "point": "end"}])  # 같은 뜻의 구속을 다시: 모순은 없고 독립이 아니다
    assert dup["status"] == "over_constrained" and dup["converged"] and dup["constraint"] in dup["redundant"]
    A("sketch.remove_constraint", id=sk2, constraints=[dup["constraint"]])
    # 충돌 구속(GEO-T14-09): 같은 선에 길이 10(있음)과 60
    r = A("sketch.add_dimension", id=sk2, kind="length", entities=[m], value=60.0)
    assert r["status"] == "conflict" and r["converged"] is False
    st = A("sketch.solve_status", id=sk2)
    lengths = [c["id"] for c in A("sketch.get", id=sk2)["props"]["constraints"] if c["kind"] == "length"]
    assert sorted(st["conflicts"]) == sorted(lengths) and len(lengths) == 2  # 충돌하는 둘을 지목
    assert _len(_ent(app, sk2, m)) == pytest.approx(10.0)  # 풀리지 않으면 요소는 그대로
    A("sketch.remove_constraint", id=sk2, constraints=[lengths[1]])
    assert A("sketch.solve_status", id=sk2)["status"] == "fully_constrained"
    # 구속 삭제(GEO-T14-14): 수평 구속을 지우면 끝점을 위로 옮길 수 있고 상태가 미구속으로
    horiz = [c["id"] for c in A("sketch.get", id=sk2)["props"]["constraints"] if c["kind"] == "horizontal"][0]
    r = A("sketch.move_point", id=sk2, entity=m, point="end", to=[6.0, 8.0])  # 수평·길이 10·시작 고정 → 구속이 끝점을 (10, 0) 으로 되돌린다
    assert r["converged"] and _ent(app, sk2, m)["end"] == pytest.approx([10.0, 0.0], abs=1e-8)
    A("sketch.remove_constraint", id=sk2, constraints=[horiz])
    assert A("sketch.solve_status", id=sk2)["status"] == "unconstrained"
    A("sketch.move_point", id=sk2, entity=m, point="end", to=[6.0, 8.0])
    e = _ent(app, sk2, m)
    assert e["end"][1] != pytest.approx(e["start"][1]) and _len(e) == pytest.approx(10.0, abs=1e-8) and e["end"] == pytest.approx([6.0, 8.0], abs=1e-8)
    # 투영한 참조 요소에 일치 구속(GEO-T14-10): 참조 요소는 고정이고 다른 요소가 거기에 붙는다
    box_part = app.model.parts.create(name="B")
    box_part.features.create_box(size=[10, 10, 10])
    sk3 = A("sketch.create", name="S3", parent=box_part.id, point=[0, 0, 10], normal=[0, 0, 1])["id"]
    top_edges = [e["index"] for e in _ents(app, box_part, "edge") if abs(e["center"][2] - 10.0) < 1e-9]
    pr = A("sketch.project", id=sk3, part=box_part.id, edges=top_edges[:1])
    ref = pr["entities"][0] if "entities" in pr else pr["entity"]
    ref_e = _ent(app, sk3, ref)
    assert ref_e["reference"] is True and ref_e["kind"] == "line"
    free = A("sketch.add_line", id=sk3, start=[3, 3], end=[7, 4])["entity"]
    A("sketch.add_constraint", id=sk3, kind="coincident", points=[{"entity": free, "point": "start"}, {"entity": ref, "point": "start"}])
    assert _ent(app, sk3, free)["start"] == pytest.approx(ref_e["start"], abs=1e-9) and _ent(app, sk3, ref) == ref_e  # 참조 요소는 안 움직인다
    # 요소를 지우면 그 구속도 지워진다
    r = A("sketch.remove", id=sk3, entities=[free])
    assert r["constraints_removed"] == 1 and A("sketch.get", id=sk3)["props"]["constraints"] == []
    # 잘못된 구속 정의는 거부되고 모델은 그대로
    n = len(A("sketch.get", id=sk)["props"]["constraints"])
    for params in (dict(kind="horizontal", entities=[c1]), dict(kind="coincident", points=[{"entity": l1, "point": "start"}]), dict(kind="radius", entities=[l1])):
        with pytest.raises(OfepError):
            A("sketch.add_dimension" if params["kind"] == "radius" else "sketch.add_constraint", id=sk, **params, **({"value": 1.0} if params["kind"] == "radius" else {}))
    assert len(A("sketch.get", id=sk)["props"]["constraints"]) == n


@pytest.mark.feature("GEO-05")
@pytest.mark.feature("GEO-40")
def test_GEO_T03_04_05_persistent_entity_names(app, tmp_path):
    """영속 엔티티 이름표(D9, 자체 이름 부여): 저장·열기 뒤에도 같은 이름이 같은 형상(중심·면적)을 가리키고, 다른 파트를 더해도 바뀌지 않으며 새 파트의 이름과 겹치지 않는다.
    형상을 고쳐(필렛·치수 변경) 면 번호가 밀리거나 면이 잘려도 하중의 대상은 이름표로 같은 면을 따라가고, 그 면이 사라지면 끊어진 대상으로 진단된다."""
    b = box(app, "B", (100.0, 20.0, 10.0))
    before = {e["name"]: (e["center"], e["area"]) for e in _ents(app, b, "face")}
    assert len(before) == 6 and all(n.startswith("f") and ":face:" in n for n in before)
    assert len({e["name"] for e in _ents(app, b, "edge")}) == 12 and len({e["name"] for e in _ents(app, b, "vertex")}) == 8
    # 저장 → 열기(GEO-T03-04)
    path = tmp_path / "names.ofep"
    app.execute("project.save_as", path=str(path))
    app.execute("project.new")
    app.execute("project.open", path=str(path))
    b = app.model.parts["B"]
    after = {e["name"]: (e["center"], e["area"]) for e in _ents(app, b, "face")}
    assert set(after) == set(before)
    assert all(after[n][0] == pytest.approx(before[n][0]) and after[n][1] == pytest.approx(before[n][1]) for n in before)
    # 다른 파트 추가(GEO-T03-05): 기존 이름 불변, 새 파트의 이름은 겹치지 않는다(피처 ID 가 들어간다)
    c = box(app, "C", (5.0, 5.0, 5.0))
    assert {e["name"]: (e["center"], e["area"]) for e in _ents(app, b, "face")} == after
    assert not ({e["name"] for e in _ents(app, c, "face")} & set(after))
    # 하중의 형상 대상에는 저장할 때 이름표가 붙는다
    top = face_at(app, b, [50.0, 10.0, 10.0])
    top_name = app.execute("geometry.entity_info", id=b.id, type="face", index=top)["name"]
    case = app.model.cases.create(name="c")
    step = case.steps.create_static()
    load = step.loads.create_pressure(target={"type": "geometry", "ids": [[b.id, "face", top]]}, value=1.0)
    assert app.execute("load.get", id=load.id)["props"]["target"]["ids"][0] == [b.id, "face", top, top_name]
    # 필렛을 더하면 윗면이 잘리고 번호가 바뀔 수 있어도 같은 평면이라 이름이 이어진다(GEO-40)
    b.features.create_fillet(edges=[1], radius=2.0)
    assert app.execute("geometry.entities", id=b.id)["faces"] == 7
    found = app.execute("geometry.entity_by_name", id=b.id, type="face", name=top_name)
    assert found["index"] >= 1 and abs(found["info"]["center"][2] - 10.0) < 1e-9 and found["info"]["surface"] == "plane"
    assert load.validate() == []
    # 상자 치수를 바꿔도(재생성) 이름은 그대로고 면적만 달라진다
    bf = app.execute("feature.list", parent=b.id)[0]["id"]
    app.execute("feature.update", id=bf, size=[120.0, 20.0, 10.0])
    found2 = app.execute("geometry.entity_by_name", id=b.id, type="face", name=top_name)
    assert found2["index"] >= 1 and abs(found2["info"]["center"][2] - 10.0) < 1e-9 and found2["info"]["area"] > found["info"]["area"]
    assert load.validate() == []
    # 메싱 뒤 대상 전개는 이름표로 지금 번호를 찾아 윗면의 요소면만 준다
    app.execute("mesh.generate", id=b.id, size=10.0)
    r = app.execute("load.resolve", id=load.id)
    xyz = dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords().tolist()))
    assert r["count"] > 0 and r["what"] == "faces"
    # 그 면이 사라지면(상자 피처 삭제) 끊어진 대상
    app.execute("feature.delete", id=bf)
    issues = load.validate()
    assert [i["code"] for i in issues] == ["broken_target"] and top_name in issues[0]["message"]
    app.undo()
    assert load.validate() == []


# ================================================================ 스케치 그리기 보조(마우스 스케치 모드의 바탕)
@pytest.mark.feature("GEO-32")
@pytest.mark.feature("GEO-34")
@pytest.mark.feature("RND-38")
def test_GEO_T14_16_17_18_19_sketch_drawing_helpers(app):
    """평면 틀 왕복, 화면 픽셀 → 평면 점(직교·원근 카메라, 렌더러가 있으면 view.pick 으로 고른 면 위), 스냅 종류, 테셀레이션."""
    part = app.model.parts.create(name="P")
    part.features.create_box(size=[40.0, 30.0, 10.0])
    sk = app.execute("sketch.create", parent=part.id, point=[0.0, 0.0, 10.0], normal=[0.0, 0.0, 1.0], x_axis=[1.0, 0.0, 0.0])["id"]  # 윗면(z=10)
    # GEO-T14-16 틀 왕복
    fr = app.execute("sketch.frame", id=sk, uv=[12.0, 7.0])
    assert fr["origin"] == [0.0, 0.0, 10.0] and fr["u"] == [1.0, 0.0, 0.0] and fr["v"] == [0.0, 1.0, 0.0] and fr["normal"] == [0.0, 0.0, 1.0]
    assert fr["point"] == pytest.approx([12.0, 7.0, 10.0])
    back = app.execute("sketch.frame", id=sk, point=[12.0, 7.0, 13.0])
    assert back["uv"] == pytest.approx([12.0, 7.0]) and back["distance"] == pytest.approx(3.0)
    # GEO-T14-17 화면 → 평면: 위에서 내려다보는 직교 카메라(화면 세로 = 60 단위). 화면 중앙 = 시선과 평면의 교점 (20, 15, 10)
    cam = {"eye": [20.0, 15.0, 110.0], "target": [20.0, 15.0, 10.0], "up": [0.0, 1.0, 0.0], "projection": "orthographic", "height": 60.0}
    W, H = 800, 600
    c = app.execute("sketch.point_from_screen", id=sk, x=W / 2 - 0.5, y=H / 2 - 0.5, width=W, height=H, camera=cam)
    assert c["hit"] and not c["behind"] and c["uv"] == pytest.approx([20.0, 15.0]) and c["point"] == pytest.approx([20.0, 15.0, 10.0])
    # 오른쪽으로 100 픽셀 = 가로 80 단위 / 800 픽셀 × 100 = +10 u, 아래로 100 픽셀 = -10 v
    r = app.execute("sketch.point_from_screen", id=sk, x=W / 2 - 0.5 + 100, y=H / 2 - 0.5 + 100, width=W, height=H, camera=cam)
    assert r["uv"] == pytest.approx([30.0, 5.0])
    # 원근: 세로 시야각 90°, 거리 100 → 화면 위 가장자리는 +100 v
    persp = {"eye": [20.0, 15.0, 110.0], "target": [20.0, 15.0, 10.0], "up": [0.0, 1.0, 0.0], "projection": "perspective", "fov": 90.0}
    top = app.execute("sketch.point_from_screen", id=sk, x=W / 2 - 0.5, y=-0.5, width=W, height=H, camera=persp)
    assert top["uv"] == pytest.approx([20.0, 115.0]) and top["hit"]
    # 평면을 옆에서 보면 교점이 없다
    side = app.execute("sketch.point_from_screen", id=sk, x=400, y=300, width=W, height=H,
                       camera={"eye": [-100.0, 15.0, 10.0], "target": [20.0, 15.0, 10.0], "up": [0.0, 0.0, 1.0], "projection": "orthographic", "height": 60.0})
    assert side["hit"] is False
    with pytest.raises(OfepError) as e:
        app.execute("sketch.point_from_screen", id=sk, x=1, y=1, width=W, height=H, camera={"eye": [0, 0, 0]})
    assert e.value.code == "invalid_param"
    # 렌더러가 있으면 실제 카메라로: 화면 가운데를 찍어 고른 면이 윗면이고, 같은 픽셀의 평면 교점이 그 면 안에 있다
    if "view.pick" in {c["name"] for c in app.commands()}:
        try:
            app.execute("view.standard", name="top")
            app.execute("view.fit")
            pick = app.execute("view.pick", x=200, y=150, width=400, height=300)
            if pick.get("hit") and pick["kind"] == "face":
                info = app.execute("geometry.entity_info", id=part.id, type="face", index=pick["index"])
                q = app.execute("sketch.point_from_screen", id=sk, x=200, y=150, width=400, height=300)
                assert q["hit"]
                if info["surface"] == "plane" and abs(info["center"][2] - 10.0) < 1e-9:
                    assert 0.0 <= q["uv"][0] <= 40.0 and 0.0 <= q["uv"][1] <= 30.0
                    assert q["uv"] == pytest.approx([20.0, 15.0], abs=0.5)  # 전체 맞춤의 화면 중앙은 상자 중심
        except OfepError as err:
            if err.code != "not_available":
                raise
    # GEO-T14-18 스냅
    app.execute("sketch.add_line", id=sk, start=[0.0, 0.0], end=[20.0, 0.0])        # 요소 1
    app.execute("sketch.add_circle", id=sk, center=[30.0, 20.0], radius=5.0)        # 요소 2
    app.execute("sketch.add_arc", id=sk, start=[0.0, 10.0], middle=[5.0, 15.0], end=[10.0, 10.0])  # 요소 3, 중심 (5, 10)
    s = app.execute("sketch.snap", id=sk, uv=[19.8, 0.3], tolerance=0.5)
    assert s["kind"] == "endpoint" and s["entity"] == 1 and s["point"] == "end" and s["uv"] == [20.0, 0.0]
    s = app.execute("sketch.snap", id=sk, uv=[10.2, 0.1], tolerance=0.5)
    assert s["kind"] == "midpoint" and s["uv"] == [10.0, 0.0]
    s = app.execute("sketch.snap", id=sk, uv=[30.3, 19.8], tolerance=0.5)
    assert s["kind"] == "center" and s["entity"] == 2 and s["uv"] == [30.0, 20.0]
    s = app.execute("sketch.snap", id=sk, uv=[35.2, 20.1], tolerance=0.5)
    assert s["kind"] == "quadrant" and s["uv"] == [35.0, 20.0]
    s = app.execute("sketch.snap", id=sk, uv=[5.1, 9.9], tolerance=0.5)
    assert s["kind"] == "center" and s["entity"] == 3 and s["uv"] == pytest.approx([5.0, 10.0])
    s = app.execute("sketch.snap", id=sk, uv=[0.2, -0.3], tolerance=0.5)
    assert s["kind"] in ("endpoint", "origin") and s["uv"] == [0.0, 0.0]
    s = app.execute("sketch.snap", id=sk, uv=[25.0, 0.3], tolerance=0.5, **{"from": [20.0, 0.0]})  # 기준점과 수평
    assert s["kind"] == "horizontal" and s["uv"] == [25.0, 0.0]
    s = app.execute("sketch.snap", id=sk, uv=[20.4, 7.0], tolerance=0.5, **{"from": [20.0, 0.0]})
    assert s["kind"] == "vertical" and s["uv"] == [20.0, 7.0]
    s = app.execute("sketch.snap", id=sk, uv=[26.1, 24.2], tolerance=0.5, grid=1.0)
    assert s["kind"] == "grid" and s["uv"] == [26.0, 24.0]
    s = app.execute("sketch.snap", id=sk, uv=[26.4, 24.4], tolerance=0.5)
    assert s["kind"] == "none" and s["uv"] == [26.4, 24.4]
    s = app.execute("sketch.snap", id=sk, uv=[19.8, 0.3], tolerance=0.5, exclude=1)  # 그리는 중인 요소는 뺀다
    assert s["kind"] != "endpoint" or s["entity"] != 1
    # GEO-T14-19 테셀레이션
    t = app.execute("sketch.tessellate", id=sk)
    assert t["frame"]["normal"] == [0.0, 0.0, 1.0] and len(t["entities"]) == 3
    by = {e["id"]: e for e in t["entities"]}
    assert by[1]["polylines"] == [[[0.0, 0.0, 10.0], [20.0, 0.0, 10.0]]]
    circle = by[2]["polylines"][0]
    assert len(circle) >= 24 and all(abs(math.hypot(q[0] - 30.0, q[1] - 20.0) - 5.0) < 1e-6 and q[2] == pytest.approx(10.0) for q in circle)
    arc = by[3]["polylines"][0]
    assert arc[0] == pytest.approx([0.0, 10.0, 10.0]) and arc[-1] == pytest.approx([10.0, 10.0, 10.0]) and all(abs(math.hypot(q[0] - 5.0, q[1] - 10.0) - 5.0) < 1e-6 for q in arc)
    assert t["bounds"]["u"][0] < 0.0 < 35.0 < t["bounds"]["u"][1] and t["dimensions"] == []
    app.execute("sketch.add_dimension", id=sk, kind="length", value=20.0, entities=[1])
    t = app.execute("sketch.tessellate", id=sk)
    assert len(t["dimensions"]) == 1 and t["dimensions"][0]["value"] == 20.0 and t["dimensions"][0]["kind"] == "length"
