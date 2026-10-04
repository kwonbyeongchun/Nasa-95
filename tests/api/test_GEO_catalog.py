"""형상 카탈로그(`plans/00007 형상 카탈로그.md`, GEO-T20~T27): 해석에 필요한 형상을 API 로 그린다 — 기본체, 불리언, CAE 부품,
필렛/챔퍼(제거 포함), 스윕/회전/돌출, 곡면, 쉘/얇은 형상, 복잡한 토폴로지.
형상마다: 만든다 → 위상 수가 맞다 → 체적/넓이가 식과 맞다 → `geometry.check` → 메싱이 된다.

케이스 정의: .agent/tests/tc-01-geometry.md. 형상 커널·메셔 없이 빌드했으면 건너뛴다.
"""
from __future__ import annotations

import math

import pytest

from openfep import App

PI = math.pi
_version = App().execute("app.version")
pytestmark = pytest.mark.skipif(not _version.get("geometry") or not _version.get("mesher"), reason="형상 커널 또는 자동 메셔 없이 빌드됨")


def part(app: App, name: str):
    return app.model.parts.create(name=name)


def ents(app: App, p):
    return app.execute("geometry.entities", id=p.id)


def measure(app: App, p):
    return app.execute("geometry.measure", id=p.id)


def check(app: App, p, **kw):
    return app.execute("geometry.check", id=p.id, **kw)


def info(app: App, p, type_: str, index: int):
    return app.execute("geometry.entity_info", id=p.id, type=type_, index=index)


def find_faces(app: App, p, **kw):
    return app.execute("geometry.find", id=p.id, type="face", **kw)["indices"]


def find_edges(app: App, p, **kw):
    return app.execute("geometry.find", id=p.id, type="edge", **kw)["indices"]


def edge_lengths(app: App, p):
    return [info(app, p, "edge", i)["length"] for i in range(1, ents(app, p)["edges"] + 1)]


def meshable(app: App, p, size, dimension=None, order=1):
    """메싱이 되고 메시 검사를 통과한다. 결과를 돌려주고 메시는 비운다."""
    kw = {"size": size, "order": order}
    if dimension:
        kw["dimension"] = dimension
    r = app.execute("mesh.generate", id=p.id, **kw)
    assert r["elements"] > 0 and app.execute("mesh.check")["ok"], r
    app.execute("mesh.clear", id=p.id)
    return r


def solid_ok(app: App, p, volume=None, faces=None, rel=1e-6):
    """솔리드 하나 이상, (면 수), (체적), 유효·자유 경계 없음."""
    e = ents(app, p)
    assert e["ok"] and e["solids"] >= 1, e
    if faces is not None:
        assert e["faces"] == faces, e
    m = measure(app, p)
    if volume is not None:
        assert m["volume"] == pytest.approx(volume, rel=rel), (m["volume"], volume)
    c = check(app, p)
    assert c["ok"], c
    return e, m


# ================================================================ GEO-T20 기본체
@pytest.mark.feature("GEO-11")
def test_GEO_T20_01_02_03_primitives_and_entity_ladder(app):
    """상자·실린더·구·원뿔·토러스: 위상 수와 체적이 식과 같고 메싱된다. 꼭짓점 → 모서리 → 와이어(면 채움) → 면 → 솔리드 → 셸 사다리."""
    b = part(app, "box")
    b.features.create_box(size=[10.0, 20.0, 30.0])
    e, _ = solid_ok(app, b, volume=6000.0, faces=6)
    assert e["edges"] == 12 and e["vertices"] == 8
    c = part(app, "cylinder")
    c.features.create_cylinder(radius=5.0, height=20.0)
    solid_ok(app, c, volume=PI * 25 * 20, faces=3)
    s = part(app, "sphere")
    s.features.create_sphere(radius=7.0)
    solid_ok(app, s, volume=4 / 3 * PI * 343, rel=1e-6)
    k = part(app, "cone")
    k.features.create_cone(radius1=10.0, radius2=4.0, height=15.0)
    solid_ok(app, k, volume=PI * 15 / 3 * (100 + 40 + 16), rel=1e-6)
    t = part(app, "torus")
    t.features.create_torus(major_radius=20.0, minor_radius=4.0)
    solid_ok(app, t, volume=2 * PI * PI * 20 * 16, rel=1e-6)
    for p in (b, c, s, k, t):
        assert meshable(app, p, size=4.0)["shape"] == "tet4"
    # 사다리: 점 → 선·호(모서리) → 닫힌 고리를 면으로 채움 → 면을 돌출해 솔리드
    w = part(app, "ladder")
    w.features.create_point(point=[0.0, 0.0, 0.0])
    assert ents(app, w) == {"solids": 0, "faces": 0, "edges": 0, "vertices": 1, "ok": True}
    w.features.create_line(start=[0.0, 0.0, 0.0], end=[10.0, 0.0, 0.0])
    w.features.create_line(start=[10.0, 0.0, 0.0], end=[10.0, 10.0, 0.0])
    w.features.create_arc(start=[10.0, 10.0, 0.0], middle=[5.0, 13.0, 0.0], end=[0.0, 10.0, 0.0])
    w.features.create_line(start=[0.0, 10.0, 0.0], end=[0.0, 0.0, 0.0])
    e = ents(app, w)
    assert e["edges"] == 4 and e["faces"] == 0
    w.features.create_face_fill(edges=[1, 2, 3, 4])
    e = ents(app, w)
    assert e["faces"] == 1 and e["solids"] == 0
    area = measure(app, w)["area"]
    assert area > 100.0  # 사각형 100 + 활꼴
    w.features.create_extrude(face=1, distance=5.0)
    solid_ok(app, w, volume=5.0 * area, faces=6)
    meshable(app, w, size=3.0)
    # 셸: 솔리드의 윗면을 열고 두께 2 만 남긴다
    sh = part(app, "shell")
    sh.features.create_box(size=[20.0, 20.0, 20.0])
    top = find_faces(app, sh, box_min=[-1, -1, 19], box_max=[21, 21, 21])
    assert len(top) == 1
    sh.features.create_shell(faces=top, thickness=2.0)
    solid_ok(app, sh, volume=20 ** 3 - 16 * 16 * 18)
    meshable(app, sh, size=3.0)


# ================================================================ GEO-T21 불리언
@pytest.mark.feature("GEO-13")
def test_GEO_T21_01_02_03_04_booleans(app):
    """상자에 원통 구멍(cut), 두 상자 합(fuse), 원통∩상자(common), 구멍 8개 판(배열 도구로 cut)."""
    b = part(app, "holed")
    b.features.create_box(size=[40.0, 40.0, 10.0])
    tool = part(app, "drill")
    tool.features.create_cylinder(origin=[20.0, 20.0, -1.0], radius=5.0, height=12.0)
    b.features.create_cut(tools=[tool.id])
    solid_ok(app, b, volume=40 * 40 * 10 - PI * 25 * 10, faces=7)
    u = part(app, "union")
    u.features.create_box(size=[20.0, 20.0, 20.0])
    u2 = part(app, "union2")
    u2.features.create_box(origin=[10.0, 10.0, 0.0], size=[20.0, 20.0, 20.0])
    u.features.create_fuse(tools=[u2.id])
    e, _ = solid_ok(app, u, volume=2 * 8000 - 10 * 10 * 20)
    assert e["solids"] == 1
    i = part(app, "common")
    i.features.create_cylinder(origin=[0.0, 0.0, -5.0], radius=10.0, height=20.0)
    i2 = part(app, "common2")
    i2.features.create_box(origin=[0.0, 0.0, 0.0], size=[20.0, 20.0, 10.0])
    i.features.create_common(tools=[i2.id])
    solid_ok(app, i, volume=PI * 100 * 10 / 4)  # 사분원 기둥
    plate = part(app, "plate8")
    plate.features.create_box(size=[100.0, 60.0, 5.0])
    hole = part(app, "hole")
    hole.features.create_cylinder(origin=[15.0, 15.0, -1.0], radius=4.0, height=7.0)
    hole.features.create_pattern(kind="linear", count=4, vector=[23.0, 0.0, 0.0])
    hole.features.create_pattern(kind="linear", count=2, vector=[0.0, 30.0, 0.0])
    assert ents(app, hole)["solids"] == 8
    plate.features.create_cut(tools=[hole.id])
    solid_ok(app, plate, volume=100 * 60 * 5 - 8 * PI * 16 * 5, faces=6 + 8)
    for p in (b, u, i, plate):
        meshable(app, p, size=5.0)


# ================================================================ GEO-T22 CAE 부품
@pytest.mark.feature("GEO-12")
@pytest.mark.feature("GEO-13")
@pytest.mark.feature("GEO-15")
def test_GEO_T22_01_02_03_04_05_06_cae_parts(app):
    """구멍 있는 판(스케치 돌출), L 브래킷(안쪽 필렛), U 브래킷, 플랜지(볼트 구멍 원형 배열), 샤프트(단 + 챔퍼), 구멍 있는 보."""
    # 구멍 있는 판
    pl = part(app, "plate_hole")
    sk = app.execute("sketch.create", parent=pl.id, point=[0.0, 0.0, 0.0], normal=[0.0, 0.0, 1.0], x_axis=[1.0, 0.0, 0.0])["id"]
    app.execute("sketch.add_rectangle", id=sk, corner=[0.0, 0.0], size=[100.0, 60.0])
    app.execute("sketch.add_circle", id=sk, center=[50.0, 30.0], radius=10.0)
    profiles = app.execute("sketch.profiles", id=sk)["profiles"]
    ring = max(profiles, key=lambda q: q["edges"])["profile"]  # 사각형 − 원
    pl.features.create_extrude(sketch=sk, profile=ring, distance=10.0)
    solid_ok(app, pl, volume=(100 * 60 - PI * 100) * 10, faces=7)
    assert meshable(app, pl, size=5.0, order=2)["shape"] == "tet10"
    # L 브래킷: 두 상자 fuse + 안쪽(오목) 모서리 필렛 — 필렛이 재료를 더한다
    L = part(app, "L_bracket")
    L.features.create_box(size=[60.0, 40.0, 6.0])
    L.features.create_box(size=[6.0, 40.0, 50.0])
    L.features.create_fuse()
    assert ents(app, L)["solids"] == 1
    inner = find_edges(app, L, box_min=[5.9, -1.0, 5.9], box_max=[6.1, 41.0, 6.1])  # (6, y, 6) 를 지나는 오목 모서리
    assert len(inner) == 1
    L.features.create_fillet(edges=inner, radius=4.0)
    v_fuse = 60 * 40 * 6 + 6 * 40 * 50 - 6 * 40 * 6
    solid_ok(app, L, volume=v_fuse + 40 * (16 - PI * 4))
    meshable(app, L, size=4.0)
    # U 브래킷
    U = part(app, "U_bracket")
    U.features.create_box(size=[60.0, 40.0, 30.0])
    inner_box = part(app, "U_inner")
    inner_box.features.create_box(origin=[5.0, -1.0, 5.0], size=[50.0, 42.0, 30.0])
    U.features.create_cut(tools=[inner_box.id])
    solid_ok(app, U, volume=60 * 40 * 30 - 50 * 40 * 25, faces=10)
    meshable(app, U, size=5.0)
    # 플랜지: 디스크 + 허브, 가운데 구멍, 볼트 구멍 6개 원형 배열
    F = part(app, "flange")
    F.features.create_cylinder(radius=50.0, height=8.0)
    F.features.create_cylinder(radius=20.0, height=30.0)
    F.features.create_fuse()
    bore = part(app, "bore")
    bore.features.create_cylinder(origin=[0.0, 0.0, -1.0], radius=10.0, height=40.0)
    F.features.create_cut(tools=[bore.id])
    bolt = part(app, "bolt")
    bolt.features.create_cylinder(origin=[38.0, 0.0, -1.0], radius=3.0, height=12.0)
    bolt.features.create_pattern(kind="circular", count=6, point=[0.0, 0.0, 0.0], axis=[0.0, 0.0, 1.0])
    F.features.create_cut(tools=[bolt.id])
    solid_ok(app, F, volume=PI * 2500 * 8 + PI * 400 * 22 - PI * 100 * 30 - 6 * PI * 9 * 8)
    assert len(find_faces(app, F, surface="cylinder")) == 1 + 1 + 1 + 6  # 바깥·허브·보어·볼트 6
    meshable(app, F, size=6.0)
    # 샤프트: 지름이 다른 세 단 + 끝 챔퍼
    S = part(app, "shaft")
    S.features.create_cylinder(axis=[1.0, 0.0, 0.0], radius=10.0, height=40.0)
    S.features.create_cylinder(origin=[40.0, 0.0, 0.0], axis=[1.0, 0.0, 0.0], radius=15.0, height=30.0)
    S.features.create_cylinder(origin=[70.0, 0.0, 0.0], axis=[1.0, 0.0, 0.0], radius=8.0, height=30.0)
    S.features.create_fuse()
    full = PI * 100 * 40 + PI * 225 * 30 + PI * 64 * 30
    solid_ok(app, S, volume=full)
    end_edge = find_edges(app, S, box_min=[99.9, -1, -1], box_max=[100.1, 1, 1], surface="circle")  # 끝 원(중심 (100, 0, 0))
    assert len(end_edge) == 1
    S.features.create_chamfer(edges=end_edge, distance=2.0)
    chamfer = 2 * PI * (8 - 2 / 3) * (0.5 * 2 * 2)  # 파푸스: 삼각형 단면 × 무게중심이 도는 둘레
    e, _ = solid_ok(app, S, volume=full - chamfer)
    assert len(find_faces(app, S, surface="cone")) == 1
    meshable(app, S, size=5.0)
    # 구멍 있는 보: 가로 관통 구멍 3개
    B = part(app, "beam_holes")
    B.features.create_box(size=[200.0, 20.0, 40.0])
    drill = part(app, "beam_drill")
    drill.features.create_cylinder(origin=[50.0, -1.0, 20.0], axis=[0.0, 1.0, 0.0], radius=8.0, height=22.0)
    drill.features.create_pattern(kind="linear", count=3, vector=[50.0, 0.0, 0.0])
    B.features.create_cut(tools=[drill.id])
    solid_ok(app, B, volume=200 * 20 * 40 - 3 * PI * 64 * 20, faces=9)
    meshable(app, B, size=6.0)


# ================================================================ GEO-T23 필렛·챔퍼·필렛 제거
@pytest.mark.feature("GEO-15")
@pytest.mark.feature("GEO-16")
def test_GEO_T23_01_02_03_04_fillet_chamfer_and_removal(app):
    """모서리 필렛 블록, 반경 여럿, 챔퍼 판, 구멍 둘레 필렛 — 그리고 필렛 제거로 원래 체적·면 수로 돌아온다."""
    b = part(app, "fillet_block")
    b.features.create_box(size=[30.0, 20.0, 10.0])
    vertical = find_edges(app, b, min_size=9.9, max_size=10.1)  # 세로 모서리 4개(길이 10)
    assert len(vertical) == 4
    b.features.create_fillet(edges=vertical, radius=3.0)
    v_box = 30 * 20 * 10
    v1 = v_box - 4 * (9 - PI * 9 / 4) * 10
    solid_ok(app, b, volume=v1, faces=10)
    assert len(find_edges(app, b, box_min=[-1, -1, 9.9], box_max=[31, 21, 10.1])) == 8  # 윗면 둘레: 직선 4 + 호 4
    meshable(app, b, size=3.0)
    # 반경이 다른 필렛: 윗면의 긴 모서리 둘에 2 와 1. 엔티티 번호는 "앞 피처까지의 형상" 기준이므로 둘째 모서리는 첫 필렛 뒤에 다시 찾는다
    # (위 블록처럼 세로 필렛 뒤에는 윗면 둘레가 접선 연속이라 필렛이 둘레 전체로 번진다 — OCCT 의 접선 전파)
    b2 = part(app, "two_radii")
    b2.features.create_box(size=[30.0, 20.0, 10.0])
    near = find_edges(app, b2, box_min=[-1, -1, 9.9], box_max=[31, 1, 10.1])  # (15, 0, 10)
    assert len(near) == 1
    b2.features.create_fillet(edges=near, radius=2.0)
    far = find_edges(app, b2, box_min=[-1, 19, 9.9], box_max=[31, 21, 10.1])  # (15, 20, 10)
    assert len(far) == 1
    b2.features.create_fillet(edges=far, radius=1.0)
    solid_ok(app, b2, volume=6000 - (4 - PI) * 30 - (1 - PI / 4) * 30, faces=8)
    assert len(find_faces(app, b2, surface="cylinder")) == 2
    meshable(app, b2, size=3.0)
    # 챔퍼 판: 윗면 둘레 네 모서리
    c = part(app, "chamfer_plate")
    c.features.create_box(size=[40.0, 40.0, 5.0])
    tops = find_edges(app, c, box_min=[-1, -1, 4.9], box_max=[41, 41, 5.1])
    assert len(tops) == 4
    c.features.create_chamfer(edges=tops, distance=1.5)
    e, m = solid_ok(app, c, faces=10)
    prism = 0.5 * 1.5 * 1.5 * 40
    assert 40 * 40 * 5 - 4 * prism < m["volume"] < 40 * 40 * 5 - 4 * prism + 4 * 1.5 ** 3  # 모퉁이가 겹친 만큼 덜 깎인다
    meshable(app, c, size=4.0)
    # 구멍 둘레 필렛 → 필렛 제거(GEO-16)
    h = part(app, "hole_fillet")
    h.features.create_box(size=[40.0, 40.0, 10.0])
    drill = part(app, "hf_drill")
    drill.features.create_cylinder(origin=[20.0, 20.0, -1.0], radius=6.0, height=12.0)
    h.features.create_cut(tools=[drill.id])
    before = measure(app, h)["volume"]
    rim = find_edges(app, h, box_min=[19, 19, 9.9], box_max=[21, 21, 10.1])  # 구멍의 윗 테두리 원(중심 (20, 20, 10))
    assert len(rim) == 1
    h.features.create_fillet(edges=rim, radius=2.0)
    e, m = solid_ok(app, h, faces=8)
    assert m["volume"] < before
    fillet_faces = find_faces(app, h, surface="torus")
    assert len(fillet_faces) == 1
    h.features.create_remove_fillet(faces=fillet_faces)  # 토러스 면을 없애고 이웃 면을 늘려 메운다
    solid_ok(app, h, volume=before, faces=7)
    assert not find_faces(app, h, surface="torus")
    meshable(app, h, size=3.0)


# ================================================================ GEO-T24 스윕·회전·돌출
@pytest.mark.feature("GEO-12")
def test_GEO_T24_01_02_03_sweep_revolve_extrude(app):
    """사각 단면 꺾은선 스윕, 파이프 엘보(호 경로 path_edges + 원 스케치 단면), 반단면 회전(압력 용기 몸체)."""
    s = part(app, "sweep_L")
    s.features.create_sweep(points=[[0.0, -3.0, -3.0], [0.0, 3.0, -3.0], [0.0, 3.0, 3.0], [0.0, -3.0, 3.0]],
                            path=[[0.0, 0.0, 0.0], [50.0, 0.0, 0.0], [50.0, 0.0, 40.0]])
    e, m = solid_ok(app, s)
    assert m["volume"] == pytest.approx(36 * 90, rel=5e-2)  # 모퉁이 처리로 조금 다르다
    meshable(app, s, size=3.0)
    # 파이프 엘보
    el = part(app, "elbow")
    R, r = 40.0, 6.0
    el.features.create_arc(start=[R, 0.0, 0.0], middle=[R * math.cos(PI / 4), R * math.sin(PI / 4), 0.0], end=[0.0, R, 0.0])
    assert ents(app, el)["edges"] == 1 and ents(app, el)["solids"] == 0
    sk = app.execute("sketch.create", parent=el.id, point=[R, 0.0, 0.0], normal=[0.0, 1.0, 0.0], x_axis=[1.0, 0.0, 0.0])["id"]  # 시작점의 접선(+y)에 수직인 평면
    app.execute("sketch.add_circle", id=sk, center=[0.0, 0.0], radius=r)
    el.features.create_sweep(sketch=sk, profile=1, path_edges=[1])
    e, m = solid_ok(app, el)
    assert e["solids"] == 1
    assert m["volume"] == pytest.approx(PI * r * r * (PI / 2 * R), rel=1e-6)  # 파푸스: 단면적 × 호 길이
    assert len(find_faces(app, el, surface="torus")) == 1 and len(find_faces(app, el, surface="plane")) == 2
    meshable(app, el, size=4.0)
    # 압력 용기 몸체: 원통 + 반구 돔 — 반단면(xz 평면 스케치)을 z 축 둘레로 회전
    v = part(app, "vessel")
    skv = app.execute("sketch.create", parent=v.id, point=[0.0, 0.0, 0.0], normal=[0.0, 1.0, 0.0], x_axis=[1.0, 0.0, 0.0])["id"]
    Rv, H = 30.0, 60.0
    app.execute("sketch.add_line", id=skv, start=[0.0, 0.0], end=[Rv, 0.0])
    app.execute("sketch.add_line", id=skv, start=[Rv, 0.0], end=[Rv, H])
    app.execute("sketch.add_arc", id=skv, start=[Rv, H], middle=[Rv * math.cos(PI / 4), H + Rv * math.sin(PI / 4)], end=[0.0, H + Rv])
    app.execute("sketch.add_line", id=skv, start=[0.0, H + Rv], end=[0.0, 0.0])
    assert app.execute("sketch.profiles", id=skv)["count"] == 1
    v.features.create_revolve(sketch=skv, profile=1, point=[0.0, 0.0, 0.0], axis=[0.0, 0.0, 1.0], angle=360.0)
    solid_ok(app, v, volume=PI * Rv * Rv * H + 2 / 3 * PI * Rv ** 3)
    assert len(find_faces(app, v, surface="sphere")) == 1 and len(find_faces(app, v, surface="cylinder")) == 1
    meshable(app, v, size=8.0)


# ================================================================ GEO-T25 곡면
@pytest.mark.feature("GEO-11")
@pytest.mark.feature("GEO-12")
def test_GEO_T25_01_02_03_04_05_surfaces(app):
    """NURBS 곡면(점 격자), 다각형 로프트(절두 피라미드), 비틀린 로프트, 스플라인 단면 로프트(날개), 후드형 자유곡면에 두께."""
    g = part(app, "surface")
    pts = [[x, y, 0.1 * (x - 10) * (y - 10) / 10.0] for y in (0.0, 10.0, 20.0) for x in (0.0, 10.0, 20.0)]  # 안장
    g.features.create_surface_grid(points=pts, columns=3)
    e = ents(app, g)
    assert e["faces"] == 1 and e["solids"] == 0 and e["edges"] == 4 and check(app, g)["valid"]
    assert info(app, g, "face", 1)["surface"] == "bspline"
    assert 400.0 < measure(app, g)["area"] < 420.0  # 평면 20×20 보다 조금 넓다
    assert meshable(app, g, size=2.0, dimension=2)["shape"] == "tri3"
    # 다각형 로프트: 절두 피라미드
    lo = part(app, "loft")
    lo.features.create_loft(profiles=[{"points": [[0, 0, 0], [20, 0, 0], [20, 20, 0], [0, 20, 0]]},
                                      {"points": [[5, 5, 15], [15, 5, 15], [15, 15, 15], [5, 15, 15]]}], ruled=True)
    solid_ok(app, lo, volume=15 / 3 * (400 + 100 + math.sqrt(400 * 100)), faces=6)
    meshable(app, lo, size=4.0)
    # 비틀린 로프트: 사각형 → 45° 돌린 사각형
    tw = part(app, "twisted")
    c, s_ = math.cos(PI / 4), math.sin(PI / 4)
    base = [[10, 10], [-10, 10], [-10, -10], [10, -10]]
    rot = [[x * c - y * s_, x * s_ + y * c, 30.0] for x, y in base]
    tw.features.create_loft(profiles=[{"points": [[x, y, 0.0] for x, y in base]}, {"points": rot}])
    e, m = solid_ok(app, tw)
    assert 20 * 20 * 30 * 0.6 < m["volume"] < 20 * 20 * 30
    side = [i for i in range(1, e["faces"] + 1) if abs(info(app, tw, "face", i)["center"][2] - 15.0) < 1.0]
    assert side and all(info(app, tw, "face", i)["surface"] != "plane" for i in side)  # 옆면은 비틀린 곡면
    meshable(app, tw, size=4.0)
    # 날개: 익형 닮은 닫힌 스플라인 단면 3개(현 길이·위치가 다름)를 매끈하게 잇는다
    def foil(chord, z, dx=0.0):
        out = []
        for k in range(12):
            t = 2 * PI * k / 12
            x = 0.5 * chord * (1 + math.cos(t))
            y = 0.12 * chord * math.sin(t) * (1 - 0.5 * x / chord)
            out.append([x + dx, y, z])
        return out
    bl = part(app, "blade")
    bl.features.create_loft(profiles=[{"points": foil(40.0, 0.0), "spline": True}, {"points": foil(30.0, 40.0, 5.0), "spline": True},
                                      {"points": foil(20.0, 80.0, 10.0), "spline": True}])
    e, m = solid_ok(app, bl)
    assert e["solids"] == 1 and 0.3 * 80 * 0.12 * 30 * 30 < m["volume"] < 80 * 0.24 * 40 * 40
    assert any(info(app, bl, "face", i)["surface"] == "bspline" for i in range(1, e["faces"] + 1))
    assert meshable(app, bl, size=4.0, order=2)["shape"] == "tet10"
    # 후드형 자유곡면 → 두께 1.5 의 얇은 솔리드
    hood = part(app, "hood")
    pts = [[x, y, 5.0 * math.sin(PI * x / 60) * math.sin(PI * y / 40)] for y in (0.0, 13.0, 27.0, 40.0) for x in (0.0, 20.0, 40.0, 60.0)]
    hood.features.create_surface_grid(points=pts, columns=4)
    area = measure(app, hood)["area"]
    hood.features.create_thicken(thickness=1.5)
    e, m = solid_ok(app, hood)
    assert m["volume"] == pytest.approx(1.5 * area, rel=0.05)
    meshable(app, hood, size=4.0)


# ================================================================ GEO-T26 쉘·얇은 형상
@pytest.mark.feature("GEO-17")
@pytest.mark.feature("GEO-18")
def test_GEO_T26_01_02_03_04_thin_and_shell(app):
    """얇은 판(면 메시), 원통 쉘(뚜껑 면 삭제), 압력 용기 중립면(솔리드 → 면), 판금 브래킷(얇은 L 돌출 → 중립면)."""
    thin = part(app, "thin_plate")
    thin.features.create_plane(point=[50.0, 25.0, 0.0], normal=[0.0, 0.0, 1.0], size=100.0)
    assert ents(app, thin)["faces"] == 1 and measure(app, thin)["area"] == pytest.approx(10000.0)
    assert meshable(app, thin, size=10.0, dimension=2, order=2)["shape"] == "tri6"
    cyl = part(app, "cyl_shell")
    cyl.features.create_cylinder(radius=20.0, height=60.0)
    ends = find_faces(app, cyl, surface="plane")
    assert len(ends) == 2
    cyl.features.create_face_delete(faces=ends)
    e = ents(app, cyl)
    assert e["faces"] == 1 and e["solids"] == 0 and measure(app, cyl)["area"] == pytest.approx(2 * PI * 20 * 60)
    assert meshable(app, cyl, size=8.0, dimension=2)["shape"] == "tri3"
    # 압력 용기 쉘: 두께 2 의 빈 원통의 안팎 원통면 사이 중립면(반지름 29)
    ves = part(app, "vessel_mid")
    ves.features.create_cylinder(radius=30.0, height=50.0)
    inner = part(app, "vessel_inner")
    inner.features.create_cylinder(origin=[0.0, 0.0, -1.0], radius=28.0, height=52.0)
    ves.features.create_cut(tools=[inner.id])
    cyls = find_faces(app, ves, surface="cylinder")
    assert len(cyls) == 2
    ves.features.create_midsurface(faces=cyls)
    e = ents(app, ves)
    assert e["solids"] == 0 and e["faces"] >= 1
    assert measure(app, ves)["area"] == pytest.approx(2 * PI * 29 * 50, rel=1e-3)
    meshable(app, ves, size=8.0, dimension=2)
    # 판금 브래킷: 두께 2 의 L 단면을 돌출(솔리드) → 긴 다리의 안팎 면 쌍으로 중립면
    sm = part(app, "sheet_bracket")
    sm.features.create_extrude(points=[[0, 0, 0], [40, 0, 0], [40, 2, 0], [2, 2, 0], [2, 30, 0], [0, 30, 0]], distance=25.0)
    solid_ok(app, sm, volume=(40 * 2 + 28 * 2) * 25, faces=8)
    meshable(app, sm, size=3.0)
    pair = find_faces(app, sm, box_min=[-0.1, 10, -1], box_max=[2.1, 20, 26])  # x=0 과 x=2 면(중심 (0|2, 15|16, 12.5))
    assert len(pair) == 2
    sm.features.create_midsurface(faces=pair)
    e = ents(app, sm)
    assert e["solids"] == 0 and 28 * 25 <= measure(app, sm)["area"] + 1e-6 and measure(app, sm)["area"] <= 30 * 25 + 1e-6
    meshable(app, sm, size=3.0, dimension=2)


# ================================================================ GEO-T27 복잡한 토폴로지
@pytest.mark.feature("GEO-09")
@pytest.mark.feature("GEO-10")
@pytest.mark.feature("GEO-13")
def test_GEO_T27_01_02_03_04_05_06_difficult_topology(app):
    """크기가 다른 구멍 여럿, 아주 짧은 모서리(면 병합으로 제거), 작은 필렛, 거의 붙은 면, T 접합(공유 토폴로지), 비다양체 모서리(검사가 알림)."""
    p = part(app, "many_holes")
    p.features.create_box(size=[100.0, 50.0, 6.0])
    drills = part(app, "drills")
    radii = (2.0, 3.5, 5.0, 1.0, 7.0)
    for i, rad in enumerate(radii):
        drills.features.create_cylinder(origin=[12.0 + 19.0 * i, 25.0, -1.0], radius=rad, height=8.0)
    p.features.create_cut(tools=[drills.id])
    solid_ok(app, p, volume=100 * 50 * 6 - PI * 6 * sum(r * r for r in radii), faces=6 + 5)
    meshable(app, p, size=4.0)
    # 아주 짧은 모서리: 모퉁이를 0.01 만 깎은 윤곽을 돌출 → 폭 0.014 의 가는 면과 길이 0.014 모서리. 검사가 잡고, 그 면을 없애면(remove_faces) 이웃 면이 늘어나 모퉁이가 된다
    q = part(app, "short_edge")
    q.features.create_extrude(points=[[0, 0, 0], [20, 0, 0], [20, 19.99, 0], [19.99, 20, 0], [0, 20, 0]], distance=10.0)
    tiny = math.hypot(0.01, 0.01)
    assert min(edge_lengths(app, q)) == pytest.approx(tiny)
    assert check(app, q)["small_edges"] == [] and len(check(app, q, small_edge=0.05)["small_edges"]) == 2  # 기본 한계는 크기의 1e-4
    sliver = find_faces(app, q, max_size=1.0)
    assert len(sliver) == 1 and info(app, q, "face", sliver[0])["area"] == pytest.approx(tiny * 10)
    q.features.create_remove_faces(faces=sliver)
    e, _ = solid_ok(app, q, volume=20 * 20 * 10, faces=6)
    assert e["edges"] == 12 and min(edge_lengths(app, q)) == pytest.approx(10.0)
    meshable(app, q, size=5.0)
    # 작은 필렛(반지름 0.2): 메싱은 되지만 곡률 때문에 요소가 크게 늘어난다 → 필렛 제거(GEO-16)가 해석용 처방
    f = part(app, "small_fillet")
    f.features.create_box(size=[30.0, 30.0, 10.0])
    f.features.create_fillet(edges=find_edges(app, f, min_size=9.9, max_size=10.1), radius=0.2)
    solid_ok(app, f, faces=10)
    with_fillet = meshable(app, f, size=4.0)["elements"]
    f.features.create_remove_fillet(faces=find_faces(app, f, surface="cylinder"))
    solid_ok(app, f, volume=30 * 30 * 10, faces=6)
    assert meshable(app, f, size=4.0)["elements"] < with_fillet / 5
    # 거의 붙은 면: 1e-5 떨어진 두 상자 — 정확한 fuse 는 둘로 남는다(틈). 퍼지 공차(tolerance)를 준 fuse 가 하나로 합친다
    n = part(app, "near_faces")
    n.features.create_box(size=[10.0, 10.0, 10.0])
    n.features.create_box(origin=[10.00001, 0.0, 0.0], size=[10.0, 10.0, 10.0])
    exact = n.features.create_fuse()
    assert ents(app, n)["solids"] == 2
    exact.delete()
    n.features.create_fuse(tolerance=1e-4)
    e, m = solid_ok(app, n)
    assert e["solids"] == 1 and e["faces"] == 6 and m["volume"] == pytest.approx(2000.0, rel=1e-4)
    meshable(app, n, size=3.0)
    # T 접합: 판 위에 세운 판(면이 닿음) — 공유 토폴로지로 접합면을 함께 쓰면 절점 일치 메시가 된다
    t = part(app, "T_junction")
    t.features.create_box(size=[40.0, 40.0, 4.0])
    t.features.create_box(origin=[18.0, 0.0, 4.0], size=[4.0, 40.0, 30.0])
    t.features.create_share_topology()
    e = ents(app, t)
    assert e["solids"] == 2 and e["faces"] == 13  # 접합면 하나를 함께 쓴다(6 + 6 + 1)
    assert check(app, t)["ok"]  # 공유 면의 모서리는 솔리드마다 면 2개 — 비다양체가 아니다
    t.features.create_translate(vector=[0.0, 0.0, 5.0])  # 변환해도 공유가 풀리지 않는다
    assert ents(app, t)["faces"] == 13 and check(app, t)["ok"]
    r = app.execute("mesh.generate", id=t.id, size=4.0)
    assert r["elements"] > 0 and app.execute("mesh.check")["ok"]
    xyz = app.mesh.node_coords().tolist()
    on = [c3 for c3 in xyz if abs(c3[2] - 9.0) < 1e-9 and 18.0 - 1e-9 <= c3[0] <= 22.0 + 1e-9]
    assert len(on) >= 4 and len({tuple(round(v, 6) for v in c3) for c3 in on}) == len(on)  # 접합면의 노드가 겹치지 않는다(한 벌)
    app.execute("mesh.clear", id=t.id)
    # 비다양체 모서리: 모서리 하나만 공유하는 두 상자를 fuse — 검사가 비다양체 모서리를 알린다(또는 둘로 남긴다)
    nm = part(app, "nonmanifold")
    nm.features.create_box(size=[10.0, 10.0, 10.0])
    nm.features.create_box(origin=[10.0, 10.0, 0.0], size=[10.0, 10.0, 10.0])
    nm.features.create_fuse()
    e, c = ents(app, nm), check(app, nm)
    assert measure(app, nm)["volume"] == pytest.approx(2000.0)
    assert (e["solids"] == 1 and c["non_manifold_edges"]) or e["solids"] == 2
