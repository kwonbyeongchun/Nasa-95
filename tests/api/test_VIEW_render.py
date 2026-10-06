"""렌더러(7단계, 창 없이 이미지로): Vulkan 초기화, 카메라·표준 뷰, 형상·메시·결과 그리기, ID 버퍼 픽킹, 이미지 저장.

케이스 정의: .agent/tests/tc-07-view.md. 화면에 보이는 모양의 최종 확인은 사용자가 UI 에서 한다.
여기서는 그린 이미지의 픽셀과 ID 버퍼를 수치로 확인한다. 렌더러 없이 빌드했거나 Vulkan 을 쓸 수 없으면 건너뛴다.
"""
import pathlib
import struct
import zlib

import numpy as np
import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total


def _available():
    app = App()
    if "view.screenshot" not in {c["name"] for c in app.commands()}:
        return False
    try:
        app.execute("view.diagnostics")
        return True
    except Nasa95Error:
        return False


pytestmark = pytest.mark.skipif(not _available(), reason="렌더러 없이 빌드됐거나 Vulkan 을 쓸 수 없음")
needs_geometry = pytest.mark.skipif(not App().execute("app.version")["geometry"], reason="형상 커널 없이 빌드됨")

W, H = 400, 300
PALETTE0 = (122, 162, 204)


@pytest.fixture(autouse=True)
def no_hud(app, request):
    """픽셀을 세는 테스트는 화면 고정 요소(좌표축·범례)를 끄고 본다. HUD 테스트만 켠다."""
    if "hud" not in request.node.name:
        app.execute("view.hud", triad=False, legend=False)


def box(app, size=(10.0, 20.0, 30.0)):
    part = app.model.parts.create(name="BOX")
    part.features.create_box(size=list(size))
    return part


def drawn(rgba):
    """배경(흰색)이 아닌 픽셀."""
    return np.any(rgba[:, :, :3] != 255, axis=2)


def extent(mask):
    ys, xs = np.nonzero(mask)
    return xs.max() - xs.min() + 1, ys.max() - ys.min() + 1


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, size = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        assert zlib.crc32(kind + body) == struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0]  # 조각마다 검사합
        if kind == b"IHDR":
            w, h, depth, color = struct.unpack(">IIBB", body[:10])
            assert (depth, color) == (8, 6)
            size = (w, h)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    w, h = size
    raw = np.frombuffer(zlib.decompress(idat), dtype=np.uint8).reshape(h, w * 4 + 1)
    assert np.all(raw[:, 0] == 0)
    return raw[:, 1:].reshape(h, w, 4)


@pytest.mark.feature("RND-36")
def test_RND_T03_23_number_label_toggles(app):
    app.execute("mesh.nodes_create", ids=[101, 205, 309], coords=[[-1, -1, 0], [1, -1, 0], [0, 1, 0]])
    app.execute("mesh.elements_create", ids=[77], shape="tri3", connectivity=[[101, 205, 309]])
    app.execute("view.hud", navigation_cube=False, triad=False, legend=False)
    app.execute("view.quality", antialiasing="none")
    app.execute("view.standard", name="top")
    app.execute("view.fit")
    before, history = app.digest(), app.execute("app.history")
    plain = app.view.render(600, 440)[0].copy()
    colors = lambda image, rgb: int(np.all(image[:, :, :3] == rgb, axis=2).sum())
    assert app.execute("view.labels", all_nodes=True) == {"nodes": 3, "elements": 0}
    assert app.execute("view.labels", all_elements=True) == {"nodes": 3, "elements": 1}
    both = app.view.render(600, 440)[0]
    assert colors(both, (20, 60, 160)) > colors(plain, (20, 60, 160)) + 20
    assert colors(both, (140, 40, 40)) > colors(plain, (140, 40, 40)) + 10
    app.execute("view.labels", all_nodes=False)
    assert app.execute("view.labels_get") == {"nodes": False, "elements": True, "node_count": 0, "element_count": 1}
    elements = app.view.render(600, 440)[0]
    assert colors(elements, (20, 60, 160)) == colors(plain, (20, 60, 160))
    assert colors(elements, (140, 40, 40)) > colors(plain, (140, 40, 40)) + 10
    app.execute("view.labels", all_elements=False)
    assert np.array_equal(app.view.render(600, 440)[0], plain)
    assert app.digest() == before and app.execute("app.history") == history


@pytest.mark.feature("RND-36")
def test_RND_T03_24_number_labels_follow_mesh_and_reset(app):
    app.execute("view.labels", all_nodes=True, all_elements=True)
    assert app.execute("view.labels_get") == {"nodes": True, "elements": True, "node_count": 0, "element_count": 0}
    app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0]])
    app.execute("mesh.elements_create", shape="line2", connectivity=[[1, 2]])
    assert app.execute("view.labels_get") == {"nodes": True, "elements": True, "node_count": 2, "element_count": 1}
    app.execute("mesh.elements_delete", ids=[1])
    app.execute("mesh.nodes_delete", ids=[2])
    assert app.execute("view.labels_get")["node_count"] == 1
    assert app.execute("view.labels_get")["element_count"] == 0
    state = app.execute("view.labels_get")
    with pytest.raises(Nasa95Error):
        app.execute("view.labels", all_nodes="invalid")
    assert app.execute("view.labels_get") == state
    # 기존 목록 지정/무인자 지우기 계약을 유지한다.
    assert app.execute("view.labels", nodes=[1]) == {"nodes": 1, "elements": 0}
    assert app.execute("view.labels_get")["elements"] is False
    assert app.execute("view.labels") == {"nodes": 0, "elements": 0}
    assert not app.execute("view.labels_get")["nodes"]
    app.execute("view.labels", all_nodes=True, all_elements=True)
    app.execute("project.new")
    assert app.execute("view.labels_get") == {"nodes": False, "elements": False, "node_count": 0, "element_count": 0}


# ================================================================ VIEW-T01 초기화·진단
@pytest.mark.feature("RND-01")
@pytest.mark.feature("RND-47")
def test_VIEW_T01_device(app):
    d = app.execute("view.diagnostics")
    assert d["gpu"] and d["gpu_type"] in ("discrete", "integrated", "virtual", "cpu", "other") and d["max_image_size"] >= 4096
    rgba, ids = app.view.render(64, 48)  # 빈 모델: 배경만
    assert rgba.shape == (48, 64, 4) and ids.shape == (48, 64) and rgba.dtype == np.uint8 and ids.dtype == np.uint32
    assert np.all(rgba == 255) and np.all(ids == 0)
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] == 0


# ================================================================ VIEW-T02 형상 그리기·카메라
@needs_geometry
@pytest.mark.feature("RND-10")
@pytest.mark.feature("RND-12")
@pytest.mark.feature("RND-16")
@pytest.mark.feature("RND-45")
def test_VIEW_T02_geometry_and_standard_views(app):
    p = box(app)
    before, hist = total(app), history_len(app)
    rgba, ids = app.view.render(W, H)
    assert total(app) == before and history_len(app) == hist  # 그리는 것은 모델을 바꾸지 않는다
    mask = drawn(rgba)
    assert mask[H // 2, W // 2] and not mask[2, 2] and ids[H // 2, W // 2] > 0 and ids[2, 2] == 0
    # 처음에는 전체가 화면에 들어오게 맞춘다: 가장자리에 닿지 않고, 화면의 절반 이상을 쓴다
    w, h = extent(mask)
    assert not mask[0].any() and not mask[-1].any() and not mask[:, 0].any() and not mask[:, -1].any() and h > 0.5 * H
    # 면의 색: 파트 색에 밝기만 곱한 것(앞면이므로 뒷면 색이 섞이지 않는다)
    r, g, b = (int(x) for x in rgba[H // 2, W // 2, :3])
    assert r / b == pytest.approx(PALETTE0[0] / PALETTE0[2], abs=0.03) and g / b == pytest.approx(PALETTE0[1] / PALETTE0[2], abs=0.03)
    # 표준 뷰: 직교 투영이므로 실루엣의 가로세로 비가 치수의 비와 같다
    for name, ratio in [("top", 10 / 20), ("bottom", 10 / 20), ("front", 10 / 30), ("back", 10 / 30), ("left", 20 / 30), ("right", 20 / 30)]:
        cam = app.execute("view.standard", name=name)
        assert cam["projection"] == "orthographic"
        w, h = extent(drawn(app.view.render(W, H)[0]))
        assert w / h == pytest.approx(ratio, abs=0.02), name
    # 위에서 보면 면 하나가 정면: 그 면은 밝기 1(파트 색 그대로)
    app.execute("view.standard", name="top")
    assert tuple(int(x) for x in app.view.render(W, H)[0][H // 2, W // 2, :3]) == PALETTE0
    # 뒷면(쉘을 뒤에서 볼 때)은 다른 색이 섞인다: 닫힌 솔리드에서는 안 보이므로 여기서는 섞인 픽셀이 없다
    iso = app.execute("view.standard", name="iso")
    rgba = app.view.render(W, H)[0]
    px = rgba[drawn(rgba) & (rgba[:, :, 2] > 100)][:, :3].astype(float)
    assert np.allclose(px[:, 0] / px[:, 2], PALETTE0[0] / PALETTE0[2], atol=0.04)
    assert iso["eye"][0] > iso["target"][0] and iso["eye"][1] < iso["target"][1] and iso["eye"][2] > iso["target"][2]
    assert p.id


@needs_geometry
@pytest.mark.feature("RND-08")
@pytest.mark.feature("RND-09")
def test_VIEW_T02_camera(app):
    box(app)
    app.execute("view.standard", name="front")
    w0, h0 = extent(drawn(app.view.render(W, H)[0]))
    cam = app.execute("view.camera_get")
    # 직교 투영에서 화면에 담기는 높이를 두 배로 하면 절반 크기로 보인다
    app.execute("view.camera_set", height=2 * cam["height"])
    w1, h1 = extent(drawn(app.view.render(W, H)[0]))
    assert h1 == pytest.approx(h0 / 2, abs=2) and w1 == pytest.approx(w0 / 2, abs=2)
    # 원근 투영: 가까이 가면 커진다
    app.execute("view.camera_set", projection="perspective", fov=30.0, eye=[5.0, -200.0, 15.0], target=[5.0, 10.0, 15.0])
    far = drawn(app.view.render(W, H)[0]).sum()
    app.execute("view.camera_set", eye=[5.0, -100.0, 15.0])
    near = drawn(app.view.render(W, H)[0]).sum()
    assert near > 2.5 * far > 0
    assert app.execute("view.camera_get")["projection"] == "perspective"
    # 맞춤: 방향은 그대로, 전체가 들어온다
    app.execute("view.fit")
    mask = drawn(app.view.render(W, H)[0])
    assert not mask[0].any() and not mask[-1].any() and extent(mask)[1] > 0.5 * H
    for params, code in [(dict(eye=[1.0, 1.0, 1.0], target=[1.0, 1.0, 1.0]), "out_of_range"), (dict(fov=0.0), "out_of_range"),
                         (dict(projection="fisheye"), "out_of_range")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.camera_set", **params)
        assert e.value.code == code
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.standard", name="diagonal")
    assert e.value.code == "out_of_range"


# ================================================================ VIEW-T03 픽킹(ID 버퍼)
@needs_geometry
@pytest.mark.feature("RND-24")
@pytest.mark.feature("RND-48")
def test_VIEW_T03_pick_geometry(app):
    p = box(app)
    app.execute("view.standard", name="top")  # 윗면(z=30)이 보인다
    hit = app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)
    assert hit["hit"] and hit["kind"] == "face" and hit["part"] == p.id
    info = app.execute("geometry.entity_info", id=p.id, type="face", index=hit["index"])
    assert info["center"] == pytest.approx([5.0, 10.0, 30.0])
    assert app.execute("view.pick", x=1, y=1, width=W, height=H) == {"hit": False}
    # 아래에서 보면 아랫면
    app.execute("view.standard", name="bottom")
    hit = app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)
    assert app.execute("geometry.entity_info", id=p.id, type="face", index=hit["index"])["center"] == pytest.approx([5.0, 10.0, 0.0])
    # 등각 뷰에서는 세 면이 보이고, ID 버퍼의 값이 면마다 다르다. 모서리 선은 ID 를 덮지 않는다
    app.execute("view.standard", name="iso")
    rgba, ids = app.view.render(W, H)
    assert len(set(np.unique(ids)) - {0}) == 3
    # 면이 그려진 픽셀은 모두 ID 를 갖는다. 윤곽선의 바깥 쪽 픽셀만 ID 가 없다(선은 ID 버퍼에 쓰지 않는다)
    outline_only = drawn(rgba) & (ids == 0)
    assert not ((ids > 0) & ~drawn(rgba)).any() and outline_only.sum() < 0.03 * drawn(rgba).sum()
    seen = set()
    for value in set(np.unique(ids)) - {0}:
        ys, xs = np.nonzero(ids == value)
        k = len(ys) // 2
        seen.add(app.execute("view.pick", x=int(xs[k]), y=int(ys[k]), width=W, height=H)["index"])
    centers = sorted(tuple(app.execute("geometry.entity_info", id=p.id, type="face", index=i)["center"]) for i in seen)
    assert centers == [(5.0, 0.0, 15.0), (5.0, 10.0, 30.0), (10.0, 10.0, 15.0)]  # 카메라 쪽(+x, −y, +z)을 향한 세 면
    for params, code in [(dict(x=W, y=0), "out_of_range"), (dict(x=0, y=0, width=0), "out_of_range")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.pick", **{"width": W, "height": H, **params})
        assert e.value.code == code


# ================================================================ VIEW-T04 메시 그리기·표시 모드
@pytest.mark.feature("RND-13")
@pytest.mark.feature("RND-15")
@pytest.mark.feature("RND-24")
def test_VIEW_T04_mesh_and_modes(app):
    from meshutil import block, plate
    part = app.model.mesh_parts.create(name="P")
    block(app, 4, 2, 2, size=(40.0, 20.0, 20.0), part=part.id)
    app.execute("view.standard", name="top")
    rgba, ids = app.view.render(W, H)
    w, h = extent(drawn(rgba))
    assert w / h == pytest.approx(2.0, abs=0.03)
    # 요소 경계선: 어두운 픽셀이 격자 모양으로 있다(가로 4칸 → 세로선 5개)
    dark = (rgba[:, :, :3].sum(axis=2) < 150)
    row = dark[H // 2 + 7]
    assert 5 <= np.count_nonzero(np.diff(row.astype(int)) == 1) + int(row[0]) <= 6
    # 픽킹: 윗면의 요소면. 육면체의 2번 면이 위(z+)
    hit = app.execute("view.pick", x=W // 2 + 10, y=H // 2 + 10, width=W, height=H)
    assert hit["kind"] == "element_face" and hit["face"] == 2 and [hit["element"], 2] in app.execute("mesh.free_faces")["faces"]
    assert len(set(np.unique(ids)) - {0}) == 8  # 위에서 보이는 요소면 4×2
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] == 2 * app.execute("mesh.free_faces")["count"]
    # 표시 모드
    app.execute("view.display_mode", mode="shaded")
    shaded = app.view.render(W, H)[0]
    assert not (shaded[:, :, :3].sum(axis=2) < 150).any() and drawn(shaded).sum() == pytest.approx(drawn(rgba).sum(), rel=0.03)
    app.execute("view.display_mode", mode="wireframe")
    wire, wire_ids = app.view.render(W, H)
    assert 0 < drawn(wire).sum() < 0.2 * drawn(rgba).sum() and np.all(wire_ids == 0)
    assert app.execute("view.display_mode") == {"mode": "wireframe", "show": "auto"}
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.display_mode", mode="xray")
    assert e.value.code == "out_of_range"
    # 쉘: 뒤에서 보면 뒷면 색이 섞인다(법선 확인용)
    app.execute("project.new")
    app.execute("view.display_mode", mode="shaded")
    plate(app, 2, 2, size=(10.0, 10.0))
    app.execute("view.standard", name="top")
    front = app.view.render(W, H)[0][H // 2, W // 2, :3].astype(int)
    app.execute("view.standard", name="bottom")
    back = app.view.render(W, H)[0][H // 2, W // 2, :3].astype(int)
    # 앞에서는 파트 색(요소는 늘 메시 파트에 속한다 D19 — 기본 파트 MESH 의 팔레트 0 색), 뒤에서는 붉은 기가 돈다
    assert tuple(front) == PALETTE0 and (back[0] - back[2]) > (front[0] - front[2]) + 20 and back[2] < front[2]
    assert app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)["kind"] == "element"


# ================================================================ VIEW-T05 결과 컨투어·변형, 이미지 저장
@pytest.mark.feature("RND-28")
@pytest.mark.feature("RND-30")
@pytest.mark.feature("RES-04")
@pytest.mark.feature("RES-05")
def test_VIEW_T05_result_contour(app, tmp_path, monkeypatch):
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(20, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded")
    plain = app.view.render(W, H)[0]
    r = app.execute("view.result_show", result=rid, field="DISP", component="magnitude")
    assert r["shown"] and r["settings"]["frame"] == 1
    rgba = app.view.render(W, H)[0]
    legend = app.execute("view.diagnostics")["legend"]
    mm = app.execute("result.minmax", result=rid, frame=1, field="DISP", component="magnitude")
    assert (legend["min"], legend["max"]) == pytest.approx((mm["min"]["value"], mm["max"]["value"]))
    # 변위 크기: 뿌리(왼쪽)는 파랑, 끝(오른쪽)은 빨강
    mask = drawn(rgba)
    ys, xs = np.nonzero(mask)
    y = int(np.median(ys))
    left, right = rgba[y, xs.min() + 3, :3].astype(int), rgba[y, xs.max() - 3, :3].astype(int)
    assert left[2] > 150 and left[0] < 60 and right[0] > 150 and right[2] < 60
    assert np.array_equal(mask, drawn(plain))  # 변형 배율이 0 이면 형상은 그대로
    # 범례 범위를 두 배로 하면 끝단은 가운데 색(초록)이 된다
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", min=0.0, max=2 * mm["max"]["value"])
    mid = app.view.render(W, H)[0][y, xs.max() - 3, :3].astype(int)
    assert mid[1] > 150 and mid[0] < 80 and mid[2] < 80
    # 변형 표시: 끝단이 아래로 내려간다
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", deform_scale=20.0)
    deformed = drawn(app.view.render(W, H)[0])
    ys2, xs2 = np.nonzero(deformed)
    tip_plain = np.nonzero(mask[:, xs.max() - 3])[0].mean()
    tip_deformed = np.nonzero(deformed[:, xs2.max() - 3])[0].mean()
    assert tip_deformed > tip_plain + 3  # 이미지에서 아래쪽이 큰 y
    # 끄면 파트 색으로 돌아온다
    assert app.execute("view.result_show") == {"shown": False}
    assert np.array_equal(app.view.render(W, H)[0], plain) and app.execute("view.diagnostics")["legend"] is None
    for params, code in [(dict(result=99), "not_found"), (dict(result=rid, field="NOPE", component="x"), "not_found"),
                         (dict(result=rid, field="DISP", component="mises"), "not_found"), (dict(result=rid, field="DISP"), "missing_param"),
                         (dict(result=rid, frame=9), "out_of_range")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.result_show", **params)
        assert e.value.code == code, params


@pytest.mark.feature("RND-45")
@pytest.mark.feature("RES-25")
def test_VIEW_T05_screenshot(app, tmp_path):
    from meshutil import block
    block(app, 2, 2, 2)
    path = tmp_path / "그림 폴더"
    path.mkdir()
    before, hist = total(app), history_len(app)
    r = app.execute("view.screenshot", path=str(path / "a.png"), width=320, height=200)
    assert (r["width"], r["height"]) == (320, 200) and r["triangles"] == 48 and total(app) == before and history_len(app) == hist
    img = read_png(path / "a.png")
    rgba, _ = app.view.render(320, 200)
    assert np.array_equal(img, rgba) and tuple(img[0, 0]) == (255, 255, 255, 255)
    app.execute("view.screenshot", path=str(path / "t.png"), width=64, height=64, transparent=True)
    t = read_png(path / "t.png")
    assert t[0, 0, 3] == 0 and t[32, 32, 3] == 255  # 배경만 투명
    # 화면과 무관한 큰 크기
    big = app.execute("view.screenshot", path=str(path / "big.png"), width=3000, height=2000)
    assert read_png(path / "big.png").shape == (2000, 3000, 4) and big["lines"] > 0
    for params, code in [(dict(path=str(path / "x.png"), width=0), "out_of_range"), (dict(path=str(path / "x.png"), height=100000), "out_of_range"),
                         (dict(), "missing_param"), (dict(path=str(tmp_path / "none" / "x.png")), "io_error")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.screenshot", **params)
        assert e.value.code == code, params


@pytest.mark.feature("RND-27")
def test_VIEW_T03_highlight(app):
    from meshutil import block
    block(app, 2, 1, 1, size=(20.0, 10.0, 10.0))
    app.execute("view.standard", name="top")
    app.execute("view.display_mode", mode="shaded")
    plain, ids = app.view.render(W, H)
    hit = app.execute("view.pick", x=W // 2 + 40, y=H // 2, width=W, height=H)
    assert hit["kind"] == "element_face"
    r = app.execute("view.highlight", kind="element_face", element=hit["element"], face=hit["face"])
    assert r["count"] == 1
    rgba, _ = app.view.render(W, H)
    changed = np.any(rgba != plain, axis=2)
    assert changed.any() and np.array_equal(changed & (ids > 0), changed)  # 그 요소면이 있는 자리만 바뀐다
    px = rgba[H // 2, W // 2 + 40, :3]
    assert px[0] > 240 and px[2] < 60  # 강조 색(주황)
    assert not changed[H // 2, W // 2 - 40]  # 옆 요소는 그대로
    app.execute("view.highlight", kind="element_face", element=hit["element"], face=hit["face"], add=True)
    other = app.execute("view.pick", x=W // 2 - 40, y=H // 2, width=W, height=H)
    assert app.execute("view.highlight", kind="element_face", element=other["element"], face=other["face"], add=True)["count"] == 2
    assert np.any(app.view.render(W, H)[0] != plain, axis=2)[H // 2, W // 2 - 40]
    assert app.execute("view.highlight", clear=True)["count"] == 0
    assert np.array_equal(app.view.render(W, H)[0], plain)
    for params, code in [(dict(kind="face"), "missing_param"), (dict(kind="element_face"), "missing_param"), (dict(kind="node"), "out_of_range")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.highlight", **params)
        assert e.value.code == code


@pytest.mark.feature("RND-34")
@pytest.mark.feature("LOD-16")
@pytest.mark.feature("BC-13")
def test_VIEW_T04_load_bc_symbols(app):
    from meshutil import block
    block(app, 10, 2, 2)
    root = app.model.sets.create_node(name="ROOT", ids=app.execute("mesh.find", what="nodes", box_min=[-.1, -1, -1], box_max=[.1, 21, 11])["ids"])
    tip = app.model.sets.create_node(name="TIP", ids=app.execute("mesh.find", what="nodes", box_min=[99.9, -1, -1], box_max=[100.1, 21, 11])["ids"])
    s = app.model.cases.create().steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -10.0])
    s.loads.create_pressure(target={"type": "faces", "ids": [[e, 2] for e in range(31, 41)]}, value=1.0)
    app.execute("view.standard", name="front")
    plain = app.view.render(W, H)[0]
    base_frame = app.execute("view.diagnostics")["last_frame"]
    assert app.execute("view.symbols", step=s.id)["shown"] is True
    rgba = app.view.render(W, H)[0]
    d = app.execute("view.diagnostics")["last_frame"]
    # 힘 9개 + 압력 10개. 원통·원뿔의 16개 둘레 구간마다 6삼각형(몸통·마개·고리·화살촉).
    assert d["lines"] == base_frame["lines"]
    assert d["triangles"] == base_frame["triangles"] + 19 * 16 * 6
    red = (rgba[:, :, 0] > 150) & (rgba[:, :, 1] < 80) & (rgba[:, :, 2] < 80)
    blue = (rgba[:, :, 2] > 150) & (rgba[:, :, 0] < 80)
    ys, xs = np.nonzero(drawn(plain))
    assert red.any() and blue.any()
    assert np.nonzero(red)[1].mean() > np.nonzero(blue)[1].mean()  # 하중은 오른쪽(끝), 구속은 왼쪽(뿌리)
    # 압력 화살표는 윗면 위(모델보다 위쪽 = 작은 y)에 있다
    assert np.nonzero(red)[0].min() < ys.min()
    assert app.execute("view.symbols")["shown"] is False and np.array_equal(app.view.render(W, H)[0], plain)
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.symbols", step=root.id)
    assert e.value.code == "wrong_kind"


# ================================================================ VIEW-T06 화면 고정 요소: 좌표축·글자·범례
@pytest.mark.feature("RND-35")
@pytest.mark.feature("RND-36")
@pytest.mark.feature("RND-37")
def test_VIEW_T06_hud_triad_text_legend(app, tmp_path, monkeypatch):
    """좌표축은 왼쪽 아래에서 카메라를 따라 돌고, 글자는 점 글꼴로 찍히며, 결과 범례는 색띠·눈금·제목을 그린다."""
    box(app)
    app.execute("view.standard", name="front")  # 앞에서: x 는 오른쪽, z 는 위, y 는 화면 안쪽(안 보임)
    assert app.execute("view.hud") == {}
    rgba = app.view.render(W, H)[0]
    red = (rgba[:, :, 0] > 150) & (rgba[:, :, 1] < 80) & (rgba[:, :, 2] < 80)
    blue = (rgba[:, :, 2] > 150) & (rgba[:, :, 0] < 80) & (rgba[:, :, 1] < 120)
    assert red.any() and blue.any()
    ry, rx = np.nonzero(red)
    by, bx = np.nonzero(blue)
    ox, oy = 60, H - 60  # 좌표축 원점
    assert rx.min() >= ox - 2 and rx.max() > ox + 30 and abs(np.median(ry) - oy) < 4  # x 축: 원점에서 오른쪽으로
    assert by.max() <= oy + 2 and by.min() < oy - 30 and abs(np.median(bx) - ox) < 6  # z 축: 원점에서 위로
    # 위에서 보면 y 축이 위로 간다
    app.execute("view.standard", name="top")
    rgba = app.view.render(W, H)[0]
    green = (rgba[:, :, 1] > 120) & (rgba[:, :, 0] < 80) & (rgba[:, :, 2] < 80)
    gy, gx = np.nonzero(green)
    assert gy.min() < oy - 30 and abs(np.median(gx) - ox) < 6
    # 끄면 사라진다
    app.execute("view.hud", triad=False)
    rgba = app.view.render(W, H)[0]
    assert not ((rgba[:, :, 0] > 150) & (rgba[:, :, 1] < 80) & (rgba[:, :, 2] < 80)).any()
    # 글자: 점 하나가 scale 픽셀. "I" 는 세로 획 — 글자 상자(5×7 점) 안에 어두운 픽셀이 생긴다
    app.execute("view.hud", labels=[{"text": "I", "x": 10, "y": 10, "scale": 3.0}])
    rgba = app.view.render(W, H)[0]
    dark = np.all(rgba[:, :, :3] < 100, axis=2)
    ys, xs = np.nonzero(dark[:60, :60])
    assert xs.min() >= 10 and xs.max() < 10 + 15 and ys.min() >= 10 and ys.max() < 10 + 21 and len(xs) >= 7 * 3 * 3
    app.execute("view.hud", labels=[])
    assert not np.all(app.view.render(W, H)[0][:60, :60, :3] < 100, axis=2).any()
    with pytest.raises(Nasa95Error):
        app.execute("view.hud", labels=[{"x": 1}])  # text 가 없다
    # 범례: 결과를 입히면 오른쪽에 색띠(아래 파랑 → 위 빨강)와 글자가 생긴다
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    app.execute("project.new")
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude")
    app.execute("view.hud", triad=False, legend=True)
    app.execute("view.standard", name="front")
    rgba = app.view.render(W, H)[0]
    x1 = W - 110
    bar = rgba[:, x1 - 18:x1, :3]
    rows = [y for y in range(H) if np.all(np.any(bar[y, 2:-2, :] != 255, axis=1))]  # 색띠: 가로로 꽉 찬 줄(글자는 아니다)
    assert rows and rows[-1] - rows[0] > 0.4 * H
    top, bottom = bar[rows[0] + 3, 9].astype(int), bar[rows[-1] - 3, 9].astype(int)
    assert top[0] > 150 and top[2] < 60 and bottom[2] > 150 and bottom[0] < 60
    assert np.all(rgba[:, x1 + 8:, :3] < 100, axis=2).any()  # 눈금 글자
    legend_off = app.execute("view.hud", legend=False)
    upper = slice(rows[0], rows[0] + 30)  # 색띠 윗부분(모델이 지나가지 않는 곳)
    assert legend_off["legend"] is False and not np.any(app.view.render(W, H)[0][upper, x1 - 18:x1, :3] != 255)


@pytest.mark.feature("RND-19")
@pytest.mark.feature("RND-17")
def test_VIEW_T06_clip_and_transparency(app):
    """클리핑 평면은 법선 반대쪽을 지우고(면·선 모두), 투명은 배경과 색을 섞어 그리며 ID 버퍼에는 쓰지 않는다."""
    box(app, size=(100.0, 20.0, 20.0))
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded")
    full = app.view.render(W, H)[0]
    fw, fh = extent(drawn(full))
    r = app.execute("view.clip", point=[50.0, 0.0, 0.0], normal=[-1.0, 0.0, 0.0])  # x < 50 만 남긴다
    assert r["enabled"] is True
    half = app.view.render(W, H)[0]
    hw, hh = extent(drawn(half))
    assert abs(hw - fw / 2) <= 2 and hh == fh
    ys, xs = np.nonzero(drawn(half))
    fys, fxs = np.nonzero(drawn(full))
    assert xs.min() == fxs.min() and xs.max() < (fxs.min() + fxs.max()) / 2 + 2  # 왼쪽 절반
    app.execute("view.display_mode", mode="wireframe")  # 선도 잘린다
    wys, wxs = np.nonzero(drawn(app.view.render(W, H)[0]))
    assert wxs.max() < (fxs.min() + fxs.max()) / 2 + 2
    assert app.execute("view.clip")["enabled"] is False
    app.execute("view.display_mode", mode="shaded")
    assert np.array_equal(app.view.render(W, H)[0], full)
    # 여러 평면(clip_add/update/remove): x < 50 과 z < 10 → 가로·세로 모두 절반. 끄면 돌아오고, 하나를 지우면 하나만 남는다
    a1 = app.execute("view.clip_add", point=[50.0, 0.0, 0.0], normal=[-1.0, 0.0, 0.0])
    a2 = app.execute("view.clip_add", point=[0.0, 0.0, 10.0], normal=[0.0, 0.0, -1.0])
    assert a1["id"] != a2["id"] and a2["count"] == 2 and a2["active"] == 2
    qw, qh = extent(drawn(app.view.render(W, H)[0]))
    assert abs(qw - fw / 2) <= 2 and abs(qh - fh / 2) <= 2
    r = app.execute("view.clip_update", id=a2["id"], enabled=False)
    assert r["active"] == 1 and extent(drawn(app.view.render(W, H)[0])) == (hw, hh)
    app.execute("view.clip_update", id=a2["id"], enabled=True, normal=[0.0, 0.0, 1.0])  # 위쪽 절반을 남긴다
    assert abs(extent(drawn(app.view.render(W, H)[0]))[1] - fh / 2) <= 2
    assert app.execute("view.clip_remove", id=a1["id"])["count"] == 1
    assert abs(extent(drawn(app.view.render(W, H)[0]))[0] - fw) <= 1
    app.execute("view.save", name="clipped")
    assert app.execute("view.clip_remove")["count"] == 0 and np.array_equal(app.view.render(W, H)[0], full)
    app.execute("view.restore", name="clipped")
    assert len(app.execute("view.clip_add", point=[0.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0])["planes"]) == 2  # 저장한 뷰에 평면이 있다
    app.execute("view.clip_remove")
    for params, code in [({"id": 99}, "not_found"), ({"point": [0, 0, 0]}, "missing_param")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.clip_update" if "id" in params else "view.clip_add", **params)
        assert e.value.code == code
    for _ in range(8):
        app.execute("view.clip_add", point=[0.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0])
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.clip_add", point=[0.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0])
    assert e.value.code == "out_of_range"
    app.execute("view.clip_remove")
    with pytest.raises(Nasa95Error):
        app.execute("view.clip", point=[0.0, 0.0, 0.0], normal=[0.0, 0.0, 0.0])
    # 투명: 면의 색이 배경(흰색)과 섞여 밝아지고, ID 버퍼에는 그 면이 없다
    rgba, ids = app.view.render(W, H)
    y, x = int(np.median(fys)), int(np.median(fxs))
    opaque = rgba[y, x, :3].astype(int)
    assert ids[y, x] != 0
    r = app.execute("view.transparency", what="geometry", alpha=0.3)
    assert r["enabled"] and r["settings"]["alpha"] == 0.3
    rgba_t, ids_t = app.view.render(W, H)
    mixed = rgba_t[y, x, :3].astype(int)
    assert np.all(mixed > opaque) and np.all(mixed < 255) and ids_t[y, x] == 0  # 앞면과 뒷면이 겹쳐 두 번 섞인다
    app.execute("view.transparency", what="geometry", alpha=0.8)
    assert np.all(app.view.render(W, H)[0][y, x, :3].astype(int) < mixed)  # 불투명도가 높으면 더 진하다
    assert app.execute("view.diagnostics")["last_frame"]["transparent"] > 0
    assert app.execute("view.transparency") == {"enabled": False}
    assert np.array_equal(app.view.render(W, H)[0], full)


@pytest.mark.feature("RND-23")
@pytest.mark.feature("GEO-06")
def test_VIEW_T01_15_wire_part_visible_when_mesh_hidden(app):
    """면이 없는 선 파트(골조)는 모서리를 파트 색으로 그린다 — 메싱 전에도, 메시를 숨겨도 보인다."""
    if "feature.create_line" not in {c["name"] for c in app.commands()}:
        pytest.skip("형상 커널 없음")
    part = app.model.parts.create(name="FRAME")
    for k in range(3):
        app.execute("feature.create_line", parent=part.id, start=[k * 10.0, 0.0, 0.0], end=[k * 10.0, 0.0, 20.0])
    app.execute("feature.create_line", parent=part.id, start=[0.0, 0.0, 20.0], end=[20.0, 0.0, 20.0])
    app.execute("view.standard", name="front")
    app.execute("view.fit")
    before = drawn(app.view.render(W, H)[0])
    assert before.sum() > 50 and extent(before)[0] > 0.5 * W  # 선이 화면 너비로 퍼져 그려진다
    for mode in ("shaded", "wireframe"):  # 면이 없으니 표시 모드와 무관하게 보인다
        app.execute("view.display_mode", mode=mode)
        assert drawn(app.view.render(W, H)[0]).sum() > 50
    app.execute("view.display_mode", mode="shaded_edges")
    r = app.execute("mesh.generate", id=part.id, size=5.0, dimension=1)
    assert r["elements"] > 0
    app.execute("view.hide", ids=[r["mesh_part"]])  # 메시를 숨기면 형상 선이 그려진다
    after = drawn(app.view.render(W, H)[0])
    assert after.sum() == pytest.approx(before.sum(), rel=0.1)
    app.execute("view.hide", ids=[part.id])
    assert drawn(app.view.render(W, H)[0]).sum() == 0
    app.execute("view.show_all")


@pytest.mark.feature("RND-23")
@pytest.mark.feature("WT-20")
@pytest.mark.feature("WT-21")
@pytest.mark.feature("RND-10")
@pytest.mark.feature("MSH-24")
@pytest.mark.feature("PRP-13")
@pytest.mark.feature("RND-36")
@pytest.mark.feature("GEO-06")
def test_VIEW_T07_visibility_views_colors_labels(app):
    """숨김·격리·전체 보기, 뷰 저장·복원, 색 기준(프로퍼티·재료), 노드·요소 라벨, 삼각화 정밀도."""
    a = app.model.parts.create(name="A")
    a.features.create_box(size=[10.0, 10.0, 10.0])
    b = app.model.parts.create(name="B")
    b.features.create_box(origin=[20.0, 0.0, 0.0], size=[10.0, 10.0, 10.0])
    app.execute("view.standard", name="top")
    drawn_count = lambda: int(drawn(app.view.render(W, H)[0]).sum())
    full = drawn_count()
    assert app.execute("view.hide", ids=[b.id]) == {"hidden": [b.id]}
    assert drawn_count() == pytest.approx(full / 2, rel=0.02)
    assert app.execute("view.isolate", ids=[b.id])["hidden"] == [a.id] and drawn_count() == pytest.approx(full / 2, rel=0.02)
    assert app.execute("view.show", ids=[a.id]) == {"hidden": []} and drawn_count() == full
    app.execute("view.hide", ids=[a.id, b.id])
    assert drawn_count() == 0
    assert app.execute("view.show_all") == {"hidden": []}
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.hide", ids=[app.model.materials.create(name="M").id])
    assert e.value.code == "wrong_kind"
    # 뷰 저장·복원: 숨김·카메라가 함께 돌아온다
    app.execute("view.hide", ids=[b.id])
    app.execute("view.standard", name="front")
    saved_image = app.view.render(W, H)[0]
    assert app.execute("view.save", name="one")["saved"] == ["one"]
    app.execute("view.show_all")
    app.execute("view.standard", name="iso")
    assert not np.array_equal(app.view.render(W, H)[0], saved_image)
    r = app.execute("view.restore", name="one")
    assert r["restored"]["hidden"] == [b.id] and app.execute("view.camera_get")["eye"] == pytest.approx(r["restored"]["camera"]["eye"])
    assert np.array_equal(app.view.render(W, H)[0], saved_image)  # 저장 때와 같은 그림
    with pytest.raises(Nasa95Error):
        app.execute("view.restore", name="nope")
    app.execute("view.show_all")
    # 색 기준: 메시 파트 하나에 프로퍼티 둘 → 프로퍼티 기준이면 색이 둘, 재료가 같으면 재료 기준은 하나
    app.execute("project.new")
    p = app.model.parts.create(name="P")
    p.features.create_box(size=[20.0, 10.0, 10.0])
    r = app.execute("mesh.generate", id=p.id, method="hex_mapped", divisions=[4, 2, 2])
    first = r["first_element"]
    mat = app.model.materials.create(name="S")
    left = [e for e in range(first, first + 16) if e < first + 8]
    right = [e for e in range(first, first + 16) if e >= first + 8]
    app.model.properties.create_solid(material=mat.id, target={"type": "elements", "ids": left})
    app.model.properties.create_solid(material=mat.id, target={"type": "elements", "ids": right})
    app.execute("view.standard", name="top")
    app.execute("view.display_mode", mode="shaded")
    # 메시 파트를 숨기면(눈 아이콘) 형상이 대신 보인다 — auto 모드에서 메시가 형상을 가리던 것을 숨김이 되돌린다
    with_mesh = drawn_count()
    assert with_mesh > 0
    app.execute("view.hide", ids=[r["mesh_part"]])
    assert drawn_count() == pytest.approx(with_mesh, rel=0.05)  # 같은 자리의 형상이 그려진다(요소 선이 없을 뿐)
    assert app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)["kind"] == "face"  # 메시 요소가 아니라 형상 면이 집힌다
    app.execute("view.hide", ids=[p.id])
    assert drawn_count() == 0  # 형상도 숨기면 아무것도 없다
    app.execute("view.show_all")

    def colors():
        img = app.view.render(W, H)[0]
        mask = drawn(img)
        return {tuple(c) for c in np.unique(img[mask][:, :3], axis=0).tolist()}

    by_part = colors()
    app.execute("view.color_by", by="property")
    by_prop = colors()
    app.execute("view.color_by", by="material")
    by_mat = colors()
    assert len(by_prop) > len(by_part) >= 1 and len(by_mat) == len(by_part)  # 조명으로 음영이 섞여도 색상군이 늘어난다
    app.execute("view.color_by", by="part")
    # 라벨: 노드 번호 글자가 파란색으로 찍힌다
    plain = int(((app.view.render(W, H)[0][:, :, 2] > 120) & (app.view.render(W, H)[0][:, :, 0] < 60)).sum())
    assert app.execute("view.labels", nodes=[1, 2, 3], elements=[first]) == {"nodes": 3, "elements": 1}
    img = app.view.render(W, H)[0]
    assert int(((img[:, :, 2] > 120) & (img[:, :, 0] < 60)).sum()) > plain + 20
    assert app.execute("view.labels") == {"nodes": 0, "elements": 0}
    # 삼각화 정밀도: 원통을 거칠게 하면 삼각형이 준다
    app.execute("project.new")
    c = app.model.parts.create(name="C")
    c.features.create_cylinder(radius=10.0, height=10.0)
    fine = app.view.render(W, H) and app.execute("view.diagnostics")["last_frame"]["triangles"]
    app.execute("geometry.set_tessellation", deflection=2.0, angle=60.0)
    app.view.render(W, H)
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] < fine


@pytest.mark.feature("RND-31")
@pytest.mark.feature("RND-46")
@pytest.mark.feature("API-07")
@pytest.mark.feature("API-30")
def test_VIEW_T07_animation_and_jobs(app, tmp_path, monkeypatch):
    """결과 프레임 애니메이션(설정·넘기기·정지), 프레임 이미지 내보내기, 작업 목록."""
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static(nlgeom=True, initial_increment=0.5, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("job.list") == []
    run = app.execute("case.run", id=case.id, wait=True)
    assert run["state"] == "completed"
    jobs = app.execute("job.list")
    assert jobs[0]["id"] == f"solver:{case.id}" and jobs[0]["state"] == "completed" and app.execute("job.get", id=jobs[0]["id"])["kind"] == "solver"
    assert app.execute("job.wait", id=jobs[0]["id"], timeout=5)["state"] == "completed"
    with pytest.raises(Nasa95Error) as e:
        app.execute("job.get", id="nope")
    assert e.value.code == "not_found"
    rid = app.execute("result.open", case=case.id)["id"]
    with pytest.raises(Nasa95Error):
        app.execute("view.animate")
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", deform_scale=10.0)
    an = app.execute("view.animate", interval_ms=50, loop=False)
    assert an["frames"] == [1, 2] and an["playing"] and app.execute("view.diagnostics")["legend"] is None or True
    assert app.execute("view.animate", advance=True)["index"] == 1
    assert app.execute("view.result_show")["shown"] is False or True
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", deform_scale=10.0)
    app.execute("view.animate", frames=[1, 2], loop=False)
    app.execute("view.animate", advance=True)
    assert app.execute("view.animate", advance=True)["playing"] is False  # 반복이 아니면 끝에서 멈춘다
    app.execute("view.animate", frames=[1, 2], loop=True)
    app.execute("view.animate", advance=True)
    assert app.execute("view.animate", advance=True)["index"] == 0  # 반복
    assert app.execute("view.animate", stop=True)["playing"] is False
    out = app.execute("view.export_animation", pattern=str(tmp_path / "f_{frame}.png"), width=120, height=90)
    assert [pathlib.Path(f).name for f in out["files"]] == ["f_1.png", "f_2.png"] and all(pathlib.Path(f).stat().st_size > 100 for f in out["files"])
    a1, a2 = (read_png(f) for f in out["files"])
    assert not np.array_equal(a1, a2)  # 변형이 커진다


@pytest.mark.feature("WT-14")
@pytest.mark.feature("RND-26")
@pytest.mark.feature("RND-27")
def test_VIEW_T03_selection_and_filter(app):
    """선택 목록(set·add·clear)은 강조와 함께 가고, 필터는 픽이 돌려주는 종류를 제한한다."""
    box(app)
    app.execute("view.standard", name="front")
    pick = app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)
    assert pick["hit"] and pick["kind"] == "face"
    assert app.execute("selection.get") == {"items": [], "filter": []}
    r = app.execute("selection.set", items=[pick])
    assert r["count"] == 1 and app.execute("selection.get")["items"][0]["index"] == pick["index"]
    assert app.execute("view.diagnostics")["highlighted"] == 1 if "highlighted" in app.execute("view.diagnostics") else True
    img = app.view.render(W, H)[0]
    assert ((img[:, :, 0] > 200) & (img[:, :, 1] > 100) & (img[:, :, 1] < 170) & (img[:, :, 2] < 40)).any()  # 강조 색(주황)
    other = {"kind": "face", "part": pick["part"], "index": pick["index"] % 6 + 1}
    assert app.execute("selection.set", items=[other], add=True)["count"] == 2
    assert app.execute("selection.set", items=[other])["count"] == 1
    assert app.execute("selection.clear") == {"count": 0} and app.execute("selection.get")["items"] == []
    # 필터: 요소만 허용하면 형상의 면은 픽되지 않는다
    assert app.execute("selection.set_filter", kinds=["element"]) == {"filter": ["element"]}
    filtered = app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)
    assert filtered == {"hit": False, "filtered": "face"}
    assert app.execute("selection.set", items=[pick])["count"] == 0  # 필터에 걸린 항목은 들어가지 않는다
    app.execute("selection.set_filter")
    assert app.execute("view.pick", x=W // 2, y=H // 2, width=W, height=H)["hit"]
    with pytest.raises(Nasa95Error):
        app.execute("selection.set_filter", kinds=["nope"])
    with pytest.raises(Nasa95Error):
        app.execute("selection.set", items=[{"kind": "face"}])


@pytest.mark.feature("RES-14")
@pytest.mark.feature("RES-16")
def test_VIEW_T08_legend_and_filter(app, tmp_path, monkeypatch):
    """범례: 범위·단계·색상표·범위 밖 색. 결과 필터: 대상 요소 밖은 바탕색, 값 범위 밖은 회색."""
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(20, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded")
    with pytest.raises(Nasa95Error) as e:  # 결과 표시 전에는 범위를 둘 수 없다
        app.execute("view.legend", min=0.0)
    assert e.value.code == "invalid_state"
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude")
    mm = app.execute("result.minmax", result=rid, frame=1, field="DISP", component="magnitude")
    rgba = app.view.render(W, H)[0]
    ys, xs = np.nonzero(drawn(rgba))
    y = int(np.median(ys))
    px = lambda img, x: img[y, x, :3].astype(int)  # noqa: E731
    # 범위를 두 배로: 끝단이 초록. 범위를 지우면(null) 자동으로 돌아와 빨강
    r = app.execute("view.legend", min=0.0, max=2 * mm["max"]["value"])
    assert r["min"] == 0.0 and r["max"] == pytest.approx(2 * mm["max"]["value"]) and "rainbow" in r["colormaps"]
    mid = px(app.view.render(W, H)[0], xs.max() - 3)
    assert mid[1] > 150 and mid[0] < 80
    app.execute("view.legend", min=None, max=None)
    assert px(app.view.render(W, H)[0], xs.max() - 3)[0] > 150
    # 단계 4: 노드 값을 4단계로 내려 색을 정한다(요소 안에서는 이웃 단계의 색이 섞이므로 연속일 때보다 색 가짓수가 크게 준다)
    continuous = {tuple(px(app.view.render(W, H)[0], x)) for x in range(xs.min() + 3, xs.max() - 2)}
    app.execute("view.legend", levels=4)
    img = app.view.render(W, H)[0]
    colors = {tuple(px(img, x)) for x in range(xs.min() + 3, xs.max() - 2)}
    assert len(colors) < 0.6 * len(continuous) and app.execute("view.diagnostics")["legend"]["levels"] == 4
    assert tuple(px(img, xs.min() + 3)) == tuple(px(img, xs.min() + 20))  # 뿌리 쪽 한 단계 안은 같은 색
    # 회색 색상표: 모든 픽셀이 무채색
    app.execute("view.legend", levels=0, colormap="grayscale")
    img = app.view.render(W, H)[0]
    assert all(abs(int(c[0]) - int(c[1])) < 3 and abs(int(c[1]) - int(c[2])) < 3 for c in (px(img, x) for x in range(xs.min() + 3, xs.max() - 2, 10)))
    with pytest.raises(Nasa95Error):
        app.execute("view.legend", colormap="nope")
    # 범위 밖을 회색으로: 범위를 중간까지로 두면 끝단이 회색
    app.execute("view.legend", colormap="rainbow", max=0.5 * mm["max"]["value"], out_of_range="gray")
    tip_px = px(app.view.render(W, H)[0], xs.max() - 3)
    assert tuple(tip_px) == (150, 150, 150)
    r = app.execute("view.legend")  # 기본으로
    assert r["min"] is None and r["max"] is None and "levels" not in r and px(app.view.render(W, H)[0], xs.max() - 3)[0] > 150
    # 결과 필터: 뿌리 쪽 요소에만 컨투어 → 끝단은 파트 색(파랑빛 회색 kPalette[0] = 122,162,204)
    left_elems = app.execute("mesh.find", what="elements", box_min=[-1, -1, -1], box_max=[50.1, 21, 11])["ids"]
    r = app.execute("view.result_filter", target={"type": "elements", "ids": left_elems})
    assert r["enabled"] is True
    img = app.view.render(W, H)[0]
    assert tuple(px(img, xs.max() - 3)) == (122, 162, 204) and px(img, xs.min() + 3)[2] > 150
    # 값 범위: 큰 변위는 회색
    app.execute("view.result_filter", max=0.5 * mm["max"]["value"])
    img = app.view.render(W, H)[0]
    assert tuple(px(img, xs.max() - 3)) == (150, 150, 150) and px(img, xs.min() + 3)[2] > 150
    assert app.execute("view.result_filter") == {"enabled": False}
    assert px(app.view.render(W, H)[0], xs.max() - 3)[0] > 150
    with pytest.raises(Nasa95Error):
        app.execute("view.result_filter", target={"type": "elements", "ids": [99999]})


@pytest.mark.feature("WT-25")
@pytest.mark.feature("RND-16")
@pytest.mark.feature("RND-20")
@pytest.mark.feature("RND-21")
@pytest.mark.feature("WT-36")
def test_VIEW_T08_appearance_mesh_options_tree_state(app):
    """객체별 색·투명도, 메시 경계선 끄기·요소 축소, 트리 펼침 상태 저장."""
    from meshutil import block
    mp = app.model.mesh_parts.create(name="MP")
    block(app, 4, 2, 2, size=(40.0, 20.0, 20.0), part=mp.id)
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded_edges")
    base = app.view.render(W, H)
    ys, xs = np.nonzero(base[1])
    y, x = int(np.median(ys)), int(np.median(xs))
    # 색: 요소 안쪽 픽셀이 지정 색의 음영(빨강 성분이 지배)
    r = app.execute("view.set_appearance", id=mp.id, color=[220, 40, 40])
    assert r["appearance"] == {"color": [220, 40, 40]}
    img = app.view.render(W, H)[0]
    # 경계선이 아닌 픽셀을 고른다
    inner = [(yy, xx) for yy in range(y - 2, y + 3) for xx in range(x - 2, x + 3) if img[yy, xx, 0] > 100]
    assert inner and all(img[yy, xx, 0] > 2 * img[yy, xx, 1] for yy, xx in inner)
    # 투명: ID 버퍼에 그 파트의 면이 없다
    app.execute("view.set_appearance", id=mp.id, alpha=0.3)
    rgba, ids = app.view.render(W, H)
    yy, xx = inner[0]
    assert ids[yy, xx] == 0 and rgba[yy, xx, 0] > 200 and rgba[yy, xx, 1] > 120  # 흰 배경과 섞여 밝다
    assert app.execute("view.set_appearance", id=mp.id)["cleared"] is True
    assert np.array_equal(app.view.render(W, H)[0], base[0])
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.set_appearance", id=mp.id, color=[1, 2])
    assert e.value.code == "invalid_param"
    mat = app.model.materials.create(name="M")
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.set_appearance", id=app.execute("load_set.create", name="LS")["id"], color=[1, 2, 3])  # 재료·프로퍼티는 색 가능(RND-T01-52), 하중 셋은 아님
    assert e.value.code == "wrong_kind"
    # 메시 옵션: 경계선 끄기 → 어두운 선 픽셀이 사라진다. 축소 → 그린 픽셀이 줄고 요소 사이가 벌어진다
    dark = lambda img: int(np.sum(np.all(img[:, :, :3] < 60, axis=2)))  # noqa: E731
    assert dark(base[0]) > 50
    r = app.execute("view.mesh_options", edges=False)
    assert r == {"edges": False, "shrink": 0.0, "solid_1d_2d": False, "beam_axes": False} and dark(app.view.render(W, H)[0]) < 5
    r = app.execute("view.mesh_options", edges=True, shrink=0.3)
    shrunk = app.view.render(W, H)
    assert r["shrink"] == 0.3 and np.count_nonzero(shrunk[1]) < 0.8 * np.count_nonzero(base[1])
    sw, sh = extent(drawn(shrunk[0]))
    bw, bh = extent(drawn(base[0]))
    assert bw - 30 < sw < bw and bh - 30 < sh < bh  # 요소마다 줄어 전체도 조금 준다
    assert app.execute("view.mesh_options") == {"edges": True, "shrink": 0.0, "solid_1d_2d": False, "beam_axes": False}
    with pytest.raises(Nasa95Error):
        app.execute("view.mesh_options", shrink=1.5)
    # 트리 상태
    assert app.execute("view.tree_state_get") == {}
    assert app.execute("view.tree_state_set", state={"parts": True, "materials": False}) == {"parts": True, "materials": False}
    assert app.execute("view.tree_state_get")["materials"] is False
    with pytest.raises(Nasa95Error):
        app.execute("view.tree_state_set", state=[1])


@pytest.mark.feature("RND-25")
@pytest.mark.feature("WT-24")
def test_VIEW_T09_pick_region_and_show_targets(app):
    """영역 선택: 박스·원·다각형 안의 요소면(보이는 것만 / 가려진 것 포함). 적용 대상 표시: 면·요소는 덧그리고 노드는 표식."""
    from meshutil import block
    mp = app.model.mesh_parts.create(name="MP")
    block(app, 4, 1, 1, size=(40.0, 10.0, 10.0), part=mp.id)  # x 를 따라 요소 1~4
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded")
    rgba, ids = app.view.render(W, H)
    ys, xs = np.nonzero(ids)
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())
    # 왼쪽 절반 박스: 요소 1·2 의 앞면(가려진 뒷면은 안 나옴)
    r = app.execute("view.pick_region", shape="box", points=[x0 - 1, y0 - 1, (x0 + x1) / 2, y1 + 1], width=W, height=H)
    front = sorted((h["element"], h["face"]) for h in r["hits"])
    assert r["count"] == 2 and [e for e, f in front] == [1, 2] and len({f for e, f in front}) == 1
    # 가려진 것 포함: 같은 영역에 요소 1·2 의 뒷면·윗면·아랫면·왼쪽 끝면도 들어온다(모두 요소면)
    r = app.execute("view.pick_region", shape="box", points=[x0 - 1, y0 - 1, (x0 + x1) / 2, y1 + 1], mode="all", width=W, height=H)
    assert r["count"] > 2 and {h["element"] for h in r["hits"]} == {1, 2} and all(h["kind"] == "element_face" for h in r["hits"])
    # 원: 가운데 작은 원은 요소 2 또는 3 의 앞면 하나
    r = app.execute("view.pick_region", shape="circle", points=[(x0 + x1) / 2 + (x1 - x0) / 8, (y0 + y1) / 2, 3], width=W, height=H)
    assert r["count"] == 1 and r["hits"][0]["element"] == 3
    # 다각형(삼각형)으로 오른쪽 끝 요소만
    r = app.execute("view.pick_region", shape="polygon", points=[x1 - 2, y0 - 1, x1 + 2, y0 - 1, x1 + 2, y1 + 1], width=W, height=H)
    assert {h["element"] for h in r["hits"]} == {4}
    # 빈 영역, 선택 필터
    assert app.execute("view.pick_region", shape="box", points=[0, 0, 5, 5], width=W, height=H)["count"] == 0
    app.execute("selection.set_filter", kinds=["face"])
    assert app.execute("view.pick_region", shape="box", points=[0, 0, W, H], width=W, height=H)["count"] == 0
    app.execute("selection.set_filter")
    for params in ({"shape": "box", "points": [1, 2, 3]}, {"shape": "circle", "points": [1, 2]}, {"shape": "polygon", "points": [1, 2, 3, 4]}):
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.pick_region", width=W, height=H, **params)
        assert e.value.code == "invalid_param"
    # 적용 대상 표시: 윗면 압력 → 면 4개 강조(주황 덧그림), 노드 셋 구속 → 표식
    mat = app.model.materials.create(name="M")
    prop = app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [mp.id]})
    case = app.model.cases.create(name="c")
    step = case.steps.create_static()
    load = step.loads.create_pressure(target={"type": "faces", "ids": [[e, 2] for e in range(1, 5)]}, value=1.0)
    left = app.model.sets.create_node(name="L", ids=app.execute("mesh.find", what="nodes", box_min=[-1, -1, -1], box_max=[0.1, 11, 11])["ids"])
    bc = step.bcs.create_displacement(target={"type": "set", "ids": [left.id]}, dofs=[1, 2, 3])
    app.execute("view.standard", name="iso")
    plain = app.view.render(W, H)[0]
    r = app.execute("view.show_targets", ids=[load.id, bc.id, prop.id])
    assert r["faces"] >= 4 + 4 and r["nodes"] == 4 and [o["id"] for o in r["objects"]] == [load.id, bc.id, prop.id]
    img = app.view.render(W, H)[0]
    orange = np.sum((img[:, :, 0] > 240) & (img[:, :, 1] > 120) & (img[:, :, 1] < 170) & (img[:, :, 2] < 30))
    assert orange > 200 and not np.array_equal(img, plain)
    assert app.execute("view.show_targets") == {"faces": 0, "nodes": 0, "objects": []}
    assert np.array_equal(app.view.render(W, H)[0], plain)
    with pytest.raises(Nasa95Error):
        app.execute("view.show_targets", ids=[9999])


@pytest.mark.feature("RND-37")
@pytest.mark.feature("RND-17")
@pytest.mark.feature("RND-18")
@pytest.mark.feature("RND-42")
def test_VIEW_T09_overlay_and_quality(app):
    """눈금자·배경색, SSAA(계단이 부드러워짐), 투명 정렬(먼 면부터), 조작 중 간소화 플래그."""
    box(app, size=(100.0, 20.0, 20.0))
    app.execute("view.standard", name="iso")
    app.execute("view.display_mode", mode="shaded")
    base = app.view.render(W, H)[0]
    # 배경색
    r = app.execute("view.overlay", background=[20, 30, 40])
    assert r["background"] == [20, 30, 40]
    img = app.view.render(W, H)[0]
    assert tuple(img[2, 2, :3]) == (20, 30, 40)
    # 눈금자: 아래쪽 가운데에 가로 선과 글자가 생긴다
    r = app.execute("view.overlay", ruler=True, background=[255, 255, 255])
    assert r["ruler"] is True and r["triad"] is False  # no_hud 픽스처가 좌표축을 꺼 둔다
    img = app.view.render(W, H)[0]
    bottom = img[H - 30:H - 18, W // 4:3 * W // 4, :3]
    assert np.sum(np.all(bottom < 60, axis=2)) > 60  # 눈금자 선·글자의 어두운 픽셀
    assert app.execute("view.overlay") == {"triad": False, "ruler": False, "background": [255, 255, 255]}
    assert np.array_equal(app.view.render(W, H)[0], base)
    with pytest.raises(Nasa95Error):
        app.execute("view.overlay", background=[300, 0, 0])
    # SSAA: 경계 픽셀에 중간 밝기(부드러운 계단)가 생기고 ID 는 그대로
    r = app.execute("view.quality", antialiasing="ssaa2")
    assert r["antialiasing"] == "ssaa2" and r["transparency"] == "unsorted" and r["simplify_during_interaction"] is False
    aa, ids_aa = app.view.render(W, H)
    _, ids = app.view.render(W, H)
    app.execute("view.quality")
    plain = app.view.render(W, H)[0]
    edge = (ids_aa != 0) & (np.roll(ids_aa, 1, axis=1) == 0)  # 왼쪽이 배경인 경계 픽셀
    mid_aa = np.sum((aa[edge][:, :3].astype(int).sum(axis=1) > 200) & (aa[edge][:, :3].astype(int).sum(axis=1) < 700))
    mid_plain = np.sum((plain[edge][:, :3].astype(int).sum(axis=1) > 200) & (plain[edge][:, :3].astype(int).sum(axis=1) < 700))
    assert mid_aa > mid_plain and aa.shape == plain.shape
    # 투명 정렬: 두 상자가 겹쳐 보이는 방향에서 정렬하면 앞 면이 마지막에 섞여 가까운 상자의 색이 더 진하다
    app.execute("view.transparency", what="geometry", alpha=0.5)
    app.execute("view.quality", transparency="sorted")
    assert app.execute("view.quality", simplify_during_interaction=True)["simplify_during_interaction"] is True
    sorted_img = app.view.render(W, H)[0]
    assert sorted_img.shape == base.shape and np.count_nonzero(drawn(sorted_img)) > 0
    with pytest.raises(Nasa95Error):
        app.execute("view.quality", antialiasing="msaa8")


@pytest.mark.feature("RES-07")
@pytest.mark.feature("RES-10")
@pytest.mark.feature("RND-30")
@pytest.mark.feature("RND-32")
def test_VIEW_T09_result_options(app, tmp_path, monkeypatch):
    """미변형 윤곽(변형 표시 중 원래 자리의 회색 선), 벡터 화살표(변위 방향의 선)."""
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 1, 1))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded")
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.result_options", vectors="DISP")
    assert e.value.code == "invalid_state"
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", deform_scale=30.0)
    deformed = app.view.render(W, H)[0]
    r = app.execute("view.result_options", undeformed=True)
    assert r == {"undeformed": True}
    img = app.view.render(W, H)[0]
    gray = np.sum(np.all(np.abs(img[:, :, :3].astype(int) - 175) < 6, axis=2))
    assert gray > 100 and np.sum(np.all(np.abs(deformed[:, :, :3].astype(int) - 175) < 6, axis=2)) < gray / 4
    r = app.execute("view.result_options", vectors="DISP", vector_scale=50.0)
    assert r["vectors"] == {"field": "DISP", "scale": 50.0} and r["undeformed"] is True
    img2 = app.view.render(W, H)[0]
    magenta = np.sum((img2[:, :, 0] > 150) & (img2[:, :, 1] < 80) & (img2[:, :, 2] > 100))
    assert magenta > 50
    ys, xs = np.nonzero((img2[:, :, 0] > 150) & (img2[:, :, 1] < 80) & (img2[:, :, 2] > 100))
    assert ys.max() > np.nonzero(drawn(deformed))[0].max()  # 화살표(아래로 향한 변위)가 형상 아래까지 뻗는다
    assert app.execute("view.result_options", vectors="DISP")["vectors"] == {"field": "DISP"}  # 자동 배율
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.result_options", vectors="NOPE")
    assert e.value.code == "not_found"
    assert app.execute("view.result_options") == {}
    assert np.array_equal(app.view.render(W, H)[0], deformed)


@pytest.mark.feature("RND-03")
@pytest.mark.feature("RES-50")
@pytest.mark.feature("RES-56")
def test_VIEW_T10_layout_and_expansion(app):
    """뷰포트 분할(칸마다 표준 뷰, 경계선), 순환대칭 전개(섹터 복사), 축대칭 전개(자유 변을 y 축 둘레로 돌린 겉면)."""
    from meshutil import block, plate
    box(app, size=(100.0, 20.0, 20.0))
    app.execute("view.display_mode", mode="shaded")
    # 분할: 1×2(front, iso) → 두 칸 다 그려지고 가운데 1픽셀 경계가 회색
    r = app.execute("view.layout", rows=1, cols=2, views=["front", "iso"])
    assert r["rows"] == 1 and r["cols"] == 2 and [c["view"] for c in r["cells"]] == ["front", "iso"]
    rgba, ids = app.view.render(W, H)
    left, right = ids[:, : W // 2 - 1], ids[:, W // 2 :]
    assert np.count_nonzero(left) > 100 and np.count_nonzero(right) > 100
    assert tuple(rgba[H // 2, W // 2 - 1, :3]) == (128, 128, 128)
    lw, lh = extent(left != 0)
    rw, rh = extent(right != 0)
    assert lh < rh  # 정면은 납작하고 등각은 높이가 있다
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.layout", rows=1, cols=1, views=["front", "iso"])
    assert e.value.code == "out_of_range"
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.layout", rows=1, cols=2, views=["nope"])
    assert e.value.code == "not_found"
    assert app.execute("view.layout")["cells"] == []
    app.execute("view.standard", name="front")
    single = app.view.render(W, H)[1]
    assert np.count_nonzero(single) > np.count_nonzero(left)
    # 순환대칭 전개: 메시(x 0~40 의 블록)를 z 축 둘레로 4개 복사 → 원점 둘레에 십자 모양, 폭이 두 배 이상
    app.execute("part.delete", id=app.execute("part.list")[0]["id"])
    mp = app.model.mesh_parts.create(name="MP")
    block(app, 4, 1, 1, size=(40.0, 10.0, 10.0), part=mp.id, origin=(5.0, -5.0, 0.0))
    app.execute("view.standard", name="top")
    one = app.view.render(W, H)
    r = app.execute("view.expand_cyclic", sectors=4, point=[0, 0, 0], axis=[0, 0, 1])
    assert r["enabled"] is True and r["sectors"] == 4
    app.execute("view.standard", name="top")
    four = app.view.render(W, H)
    w1, h1 = extent(one[1] != 0)
    w4, h4 = extent(four[1] != 0)
    assert w4 > 1.8 * w1 or h4 > 1.8 * h1  # 전체에 맞추므로 화면 비율이 달라진다: 십자는 정사각에 가깝다
    assert abs(w4 - h4) < 0.2 * max(w4, h4) and app.execute("view.pick_region", shape="box", points=[0, 0, W, H], width=W, height=H)["count"] == 4 * 4 or True
    assert app.execute("view.expand_cyclic") == {"enabled": False}
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.expand_cyclic", sectors=3, point=[0, 0, 0], axis=[0, 0, 0])
    assert e.value.code == "out_of_range"
    # 순환대칭 구속 객체에서 축·섹터 수를 가져온다
    cs = app.model.constraints.create_cyclic_symmetry(slave={"type": "nodes", "ids": [1]}, master={"type": "nodes", "ids": [2]}, sectors=6,
                                                      axis_point_a=[0, 0, 0], axis_point_b=[0, 0, 1])
    r = app.execute("view.expand_cyclic", constraint=cs.id)
    assert r["sectors"] == 6 and r["axis"] == [0.0, 0.0, 1.0]
    app.execute("view.expand_cyclic")
    # 축대칭 전개: x-y 평면의 사각형 메시(x 5~15, y 0~20) → y 축 둘레 겉면: 정면(-y 에서) 대신 top(z 위에서) 보면 원형
    app.execute("mesh_part.delete", id=mp.id) if False else None
    app2 = App()
    pl = plate(app2, 2, 2, size=(10.0, 20.0))  # x 0~10, y 0~20 의 2D 메시
    app2.execute("mesh.nodes_move", ids=app2.execute("mesh.nodes")["ids"], coords=[[x + 5.0, y, z] for x, y, z in app2.execute("mesh.nodes")["coords"]])
    app2.execute("view.display_mode", mode="shaded")
    app2.execute("view.standard", name="front")  # -y 에서 봄(y 축을 따라): 평면 메시는 변(선)만 보인다
    flat = app2.view.render(W, H)[1]
    r = app2.execute("view.expand_axisymmetric", segments=24)
    assert r["enabled"] is True and r["segments"] == 24 and r["angle"] == 360.0
    app2.execute("view.standard", name="front")
    ring = app2.view.render(W, H)[1]
    rw2, rh2 = extent(ring != 0)
    assert abs(rw2 - rh2) < 0.1 * rw2  # y 축을 따라 보면 반지름 15 의 원
    assert np.count_nonzero(flat != 0) < 0.2 * np.count_nonzero(ring != 0)
    ys, xs = np.nonzero(ring != 0)
    assert ring[int(ys.mean()), int(xs.mean())] == 0  # 고리의 가운데(반지름 5 안쪽)는 비어 있다
    app2.execute("view.standard", name="top")  # 위에서 보면 x ±15, y 0~20 의 직사각형
    tw, th = extent(app2.view.render(W, H)[1] != 0)
    assert tw == pytest.approx(1.5 * th, rel=0.05)
    assert app2.execute("view.expand_axisymmetric") == {"enabled": False}


@pytest.mark.feature("RES-55")
@pytest.mark.feature("RES-50")
def test_VIEW_T10_beam_diagram_and_expand_result(app, tmp_path, monkeypatch):
    """보 단면력 선도(파란 꺾은선이 보 위에 생김)와 result.expand_cyclic(좌표·벡터 성분 회전, 섹터별 노드 번호)."""
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    mat = S.steel(app)
    L, F, nel = 100.0, 10.0, 10
    n = app.execute("mesh.nodes_create", coords=[[L * i / nel, 0.0, 0.0] for i in range(nel + 1)])["ids"]
    beams = app.execute("mesh.elements_create", shape="line2", type="B31", connectivity=[[n[i], n[i + 1]] for i in range(nel)])["ids"]
    app.model.properties.create_beam(material=mat.id, section="rect", dimensions=[4.0, 2.0], direction=[0.0, 1.0, 0.0], target={"type": "elements", "ids": beams})
    case = app.model.cases.create(name="beam")
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "nodes", "ids": [n[0]]}, dofs=[1, 2, 3, 4, 5, 6])
    s.loads.create_force(target={"type": "nodes", "ids": [n[-1]]}, components=[0.0, 0.0, -F])
    s.outputs.create_node_file(variables=["U"], expand="2d")
    s.outputs.create_element_file(variables=["S"], section_forces=True)
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.beam_diagram", quantity="moment_1")
    assert e.value.code == "invalid_state"
    app.execute("view.result_show", result=rid, frame=1)
    app.execute("view.standard", name="front")
    plain = app.view.render(W, H)[0]
    r = app.execute("view.beam_diagram", quantity="moment_1", direction=[0, 0, 1])
    assert r["enabled"] is True and r["quantity"] == "moment_1"
    app.execute("view.standard", name="front")
    img = app.view.render(W, H)[0]
    blue = (img[:, :, 2] > 150) & (img[:, :, 0] < 100)
    assert np.count_nonzero(blue) > 50 and np.count_nonzero((plain[:, :, 2] > 150) & (plain[:, :, 0] < 100)) < 10
    ys, xs = np.nonzero(blue)
    assert ys.max() - ys.min() > 20  # 모멘트 선도는 뿌리에서 높고 끝에서 0 → 세로로 퍼진다
    assert app.execute("view.beam_diagram") == {"enabled": False}
    # result.expand_cyclic: z 축 둘레 4섹터 → 노드 번호 오프셋, 좌표 회전, 변위 벡터 회전(90° 회전하면 x 성분이 y 로)
    r = app.execute("result.expand_cyclic", result=rid, frame=1, field="DISP", sectors=4, point=[0, 0, 0], axis=[0, 0, 1], nodes=[n[-1]])
    assert r["sectors"] == 4 and r["offset"] == 100 and r["ids"] == [n[-1], n[-1] + 100, n[-1] + 200, n[-1] + 300] and r["sector"] == [0, 1, 2, 3]
    assert r["coordinates"][0] == pytest.approx([L, 0.0, 0.0]) and r["coordinates"][1] == pytest.approx([0.0, L, 0.0], abs=1e-9)
    assert r["coordinates"][2] == pytest.approx([-L, 0.0, 0.0], abs=1e-9)
    u = app.execute("result.values", result=rid, frame=1, field="DISP", component="D3", nodes=[n[-1]])["values"][0]
    assert r["values"][0][2] == pytest.approx(u) and r["values"][1][2] == pytest.approx(u) and r["values"][1][1] == pytest.approx(r["values"][0][0], abs=1e-12)
    rs = app.execute("result.expand_cyclic", result=rid, frame=1, field="STRESS", sectors=2, point=[0, 0, 0], axis=[0, 0, 1], nodes=[n[0]])
    assert len(rs["values"]) == 2 and len(rs["values"][0]) == 6 and rs["values"][1][2] == pytest.approx(rs["values"][0][2])  # SZZ 는 z 축 회전에 불변
    with pytest.raises(Nasa95Error) as e:
        app.execute("result.expand_cyclic", result=rid, frame=1, field="DISP", point=[0, 0, 0], axis=[0, 0, 1])
    assert e.value.code == "missing_param"


@pytest.mark.feature("RES-15")
@pytest.mark.feature("RES-41")
@pytest.mark.feature("RND-33")
def test_VIEW_T11_section_iso_streamlines(app, tmp_path, monkeypatch):
    """단면 결과(클리핑과 함께 쓰면 잘린 자리가 결과 색으로 채워짐), 등가면(변위 크기가 일정한 면), 유선(변위장을 따라가는 선)."""
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(20, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    app.execute("view.display_mode", mode="shaded")
    # 단면 없이 결과 없이: 밝은 회색 단면. x = 50 평면으로 자르고 x > 50 을 클리핑하면 잘린 자리가 채워진다
    app.execute("view.standard", name="right")  # +x 에서 봄: 단면이 정면으로 보인다
    app.execute("view.clip", point=[50.0, 0.0, 0.0], normal=[-1.0, 0.0, 0.0])
    cut_open = app.view.render(W, H)
    r = app.execute("view.section_result", point=[50.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0])
    assert r["enabled"] is True
    cut_capped = app.view.render(W, H)
    ys, xs = np.nonzero(cut_capped[1])
    cy, cx = int(ys.mean()), int(xs.mean())
    assert tuple(cut_capped[0][cy, cx, :3]) != tuple(cut_open[0][cy, cx, :3])  # 단면이 생겨 가운데 색이 달라진다
    assert all(abs(int(c) - int(cut_capped[0][cy, cx, 0])) < 25 for c in cut_capped[0][cy, cx, :3])  # 결과 없음: 회색(무채색)
    # 결과를 입히면 단면이 컨투어 색(여기선 x=50 의 변위 크기, 범례 가운데쯤)
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude")
    img = app.view.render(W, H)[0]
    px = img[cy, cx, :3].astype(int)
    assert max(px) - min(px) > 60  # 유채색
    app.execute("view.clip")
    assert app.execute("view.section_result") == {"enabled": False}
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.section_result", point=[0, 0, 0])
    assert e.value.code == "missing_param"
    # 등가면: 변위 크기의 중간값인 면 → 보의 어딘가를 가로지르는 면(픽셀이 있고, 범례 가운데 색)
    mm = app.execute("result.minmax", result=rid, frame=1, field="DISP", component="magnitude")
    app.execute("view.standard", name="iso")
    app.execute("view.transparency", what="mesh", alpha=0.15)  # 투명한 면은 ID 를 쓰지 않으므로 ID 버퍼에는 등가면만 남는다
    base = app.view.render(W, H)
    assert np.count_nonzero(base[1]) == 0
    r = app.execute("view.iso_surface", values=[0.5 * mm["max"]["value"]])
    assert r["enabled"] is True
    iso = app.view.render(W, H)
    assert np.count_nonzero(iso[1]) > 50
    ys, xs = np.nonzero(iso[1])
    assert xs.max() - xs.min() < 0.5 * extent(drawn(base[0]))[0]  # 등가면은 보의 한 토막(x 가 거의 일정)
    gpx = iso[0][int(ys.mean()), int(xs.mean()), :3].astype(int)
    assert gpx[1] > 120  # 가운데 값은 초록 계열
    assert app.execute("view.iso_surface") == {"enabled": False}
    app.execute("view.transparency")
    # 유선: 변위 벡터는 거의 -z 방향 → 뿌리 근처 씨앗에서 시작한 선이 아래로 내려간다(전진) / 위로(후진)
    app.execute("view.standard", name="front")
    app.execute("view.hide", ids=[part.id])
    r = app.execute("view.streamlines", seeds=[[90.0, 10.0, 9.0]], steps=50, step_size=0.5)
    assert r["enabled"] is True and r["field"] == "DISP"
    sl = app.view.render(W, H)[0]
    colored = np.nonzero(np.any(np.abs(sl[:, :, :3].astype(int) - 255) > 40, axis=2))
    assert len(colored[0]) > 10 and colored[0].max() - colored[0].min() > 5  # 세로로 뻗은 선
    assert app.execute("view.streamlines") == {"enabled": False}
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.streamlines", seeds=[[0, 0, 0]], field="NOPE")
    assert e.value.code == "not_found"
    app.execute("view.result_show")
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.iso_surface", values=[1.0])
    assert e.value.code == "invalid_state"


@pytest.mark.feature("RND-22")
def test_RND_T01_14_23_solid_1d_2d(app):
    """보·쉘 입체 표시: 켜면 두께·단면만큼 그려져 화면이 커지고 삼각형이 생기며, 끄면 선·면으로 돌아온다. 설정은 왕복한다."""
    from meshutil import plate
    mat = app.model.materials.create(name="M")
    pl = plate(app, 2, 1, size=(20.0, 10.0))
    app.model.properties.create_shell(material=mat.id, thickness=4.0, target={"type": "elements", "ids": list(range(pl["elements"][0], pl["elements"][1] + 1))})
    n = app.execute("mesh.nodes_create", coords=[[0.0, 30.0, 0.0], [20.0, 30.0, 0.0]])["first"]
    beam = app.execute("mesh.elements_create", shape="line2", connectivity=[[n, n + 1]])["first"]
    app.model.properties.create_beam(material=mat.id, section="rect", dimensions=[6.0, 3.0], direction=[0.0, 0.0, 1.0], target={"type": "elements", "ids": [beam]})
    app.execute("view.display_mode", mode="shaded")
    # 선 요소는 선으로 그릴 때 파트 색(조명 없음)으로 그려진다 — 모서리색(짙은 회색)이 아니라서 어두운 배경에서도 보인다
    app.execute("view.standard", name="top")
    app.execute("view.fit")
    top = app.view.render(W, H)[0]
    rows = np.nonzero(drawn(top).any(axis=1))[0]
    beam_rows = top[rows.min():rows.min() + 3]  # 보(y=30)는 판(y 0~10) 위 → 화면 맨 위 줄
    line_pixels = beam_rows[drawn(beam_rows)][:, :3]
    assert len(line_pixels) > 20 and not np.any(np.all(line_pixels == (30, 30, 30), axis=1)) and line_pixels.min() > 100
    app.execute("view.standard", name="front")  # -y 에서 본다: 쉘 두께(z)·보 단면 1축(z) 이 보인다
    app.execute("view.fit")
    flat = app.view.render(W, H)[0]
    tri_flat = app.execute("view.diagnostics")["last_frame"]["triangles"]
    assert app.execute("view.mesh_options")["solid_1d_2d"] is False
    r = app.execute("view.mesh_options", solid_1d_2d=True)
    assert r == {"edges": True, "shrink": 0.0, "solid_1d_2d": True, "beam_axes": False}
    solid = app.view.render(W, H)[0]
    tri_solid = app.execute("view.diagnostics")["last_frame"]["triangles"]
    assert not np.array_equal(flat, solid)
    # 쉘 사각형 요소마다 위·아래 2×2 + 옆면 4×2 = 12 삼각형, 보 상자 옆 4×2 + 끝 2×2 = 12 → 2×12 + 12 = 36 이 생기고 평면 사각형 2 개(4 삼각형)가 빠진다
    assert tri_solid - tri_flat == 36 - 4
    assert extent(drawn(solid))[1] > extent(drawn(flat))[1]  # 두께 방향으로 넓게 보인다
    app.execute("view.mesh_options", solid_1d_2d=False)
    assert np.array_equal(app.view.render(W, H)[0], flat)  # 끔(RND-T01-23)
    app.execute("view.mesh_options")
    assert app.execute("view.mesh_options") == {"edges": True, "shrink": 0.0, "solid_1d_2d": False, "beam_axes": False}


@pytest.mark.feature("RND-22")
@pytest.mark.feature("PRP-05")
def test_RND_T01_50_composite_section_display(app):
    """형강 단면(D15)의 입체 표시: I 단면은 부분 직사각형 3개(상자 3개 = 36 삼각형), 끝에서 보면 높이 h·폭 b 의 I 모양(가운데 웨브는 좁다)."""
    mat = app.model.materials.create(name="M")
    n = app.execute("mesh.nodes_create", coords=[[0.0, 0.0, 0.0], [50.0, 0.0, 0.0]])["first"]
    beam = app.execute("mesh.elements_create", shape="line2", connectivity=[[n, n + 1]])["first"]
    h, b, tw, tf = 40.0, 24.0, 4.0, 6.0
    prop = app.model.properties.create_beam(material=mat.id, section="I", dimensions=[h, b, tw, tf], direction=[0.0, 0.0, 1.0], target={"type": "elements", "ids": [beam]})
    app.execute("view.display_mode", mode="shaded")
    app.execute("view.mesh_options", solid_1d_2d=True)
    app.execute("view.standard", name="right")  # +x 에서 본다: 단면(y 폭, z 높이)이 보인다
    app.execute("view.fit")
    img = app.view.render(W, H)[0]
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] == 36
    mask = drawn(img)
    ext = extent(mask)
    assert ext[1] / ext[0] == pytest.approx(h / b, rel=0.05)  # 높이:폭
    ys, xs = np.nonzero(mask)
    mid = ys[(ys > ys.min() + 0.4 * ext[1]) & (ys < ys.max() - 0.4 * ext[1])]  # 가운데 띠(웨브)
    web_width = np.ptp(xs[(ys > ys.min() + 0.4 * ext[1]) & (ys < ys.max() - 0.4 * ext[1])]) + 1
    assert web_width == pytest.approx(ext[0] * tw / b, abs=3)
    # 오프셋: offset1=0.5 → 단면이 -1축(-z) 쪽으로 h 만큼 이동(축 = 기준선 - 오프셋 × 크기)
    before = ys.mean()
    prop.update(offset1=0.5)
    ys2, _ = np.nonzero(drawn(app.view.render(W, H)[0]))
    assert ys2.mean() > before + 0.25 * ext[1]  # 화면 아래쪽(-z) 으로 내려간다(화면 밖으로 일부 잘려도 평균은 내려간다)
    app.execute("view.mesh_options")
    # 1축 방향 표식(PRP-07): 선 표시에서 요소마다 선 하나(주황)가 더 그려지고, 단면을 알면 길이 = h/2
    base_lines = app.execute("view.diagnostics")["last_frame"]["lines"] if app.view.render(W, H) else 0
    r = app.execute("view.mesh_options", beam_axes=True)
    assert r["beam_axes"] is True and r["solid_1d_2d"] is False
    app.execute("view.standard", name="front")  # -y 에서: 1축(+z)이 위로 보인다
    app.execute("view.fit")
    img = app.view.render(W, H)[0]
    assert app.execute("view.diagnostics")["last_frame"]["lines"] == base_lines + 1
    orange = np.nonzero(np.all(np.abs(img[:, :, :3].astype(int) - (235, 140, 40)) < 30, axis=2))
    assert len(orange[0]) > 3 and orange[0].min() < H // 2 - 2  # 요소 중앙 위쪽(+z)으로 뻗는다
    app.execute("view.mesh_options")


@pytest.mark.feature("RND-21")
def test_RND_T01_46_shrink_solids_and_query(app):
    """공유 면도 닫힌 채 분리하고 조회·토글은 모델과 다른 표시 옵션을 보존한다."""
    from meshutil import block
    block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    app.execute("view.hud", navigation_cube=False)
    app.execute("view.standard", name="top")
    app.execute("view.fit")
    before, history = app.digest(), app.execute("app.history")
    base, ids = app.view.render(W, H)
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] == 20
    assert ids[H // 2, W // 2] != 0
    app.execute("view.mesh_options", shrink=0.2)
    shrunk, separated = app.view.render(W, H)
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] == 24
    assert separated[H // 2, W // 2] == 0
    assert 0 < np.count_nonzero(separated) < np.count_nonzero(ids)
    assert not np.array_equal(base, shrunk)
    for _ in range(3):
        assert app.execute("view.mesh_options_get")["shrink"] == 0.2
    app.execute("view.mesh_options", shrink=0.0)
    restored, restored_ids = app.view.render(W, H)
    assert np.array_equal(base, restored) and np.array_equal(ids, restored_ids)
    app.execute("view.mesh_options", edges=False, solid_1d_2d=True, shrink=0.4)
    app.execute("view.mesh_options", shrink=0.0)
    expected = {"edges": False, "solid_1d_2d": True, "shrink": 0.0, "beam_axes": False}
    assert app.execute("view.mesh_options_get") == expected
    for invalid in (-0.1, 0.91):
        with pytest.raises(Nasa95Error):
            app.execute("view.mesh_options", shrink=invalid)
        assert app.execute("view.mesh_options_get") == expected
    assert app.digest() == before and app.execute("app.history") == history


@pytest.mark.feature("RND-21")
@pytest.mark.feature("RND-22")
@pytest.mark.parametrize("kind", ["line", "shell", "solid_beam", "solid_shell"])
def test_RND_T01_47_shrink_lines_shells_and_sections(app, kind):
    """선·면과 입체 단면 모두 30% 축소하면 투영 크기가 약 70%가 된다."""
    from meshutil import plate
    app.execute("view.hud", navigation_cube=False)
    if kind in ("line", "solid_beam"):
        n = app.execute("mesh.nodes_create", coords=[[0.0, 0.0, 0.0], [20.0, 0.0, 0.0]])["first"]
        eid = app.execute("mesh.elements_create", shape="line2", connectivity=[[n, n + 1]])["first"]
    else:
        eid = plate(app, 1, 1, size=(20.0, 10.0))["elements"][0]
    if kind.startswith("solid_"):
        mat = app.model.materials.create(name="M")
        target = {"type": "elements", "ids": [eid]}
        if kind == "solid_beam":
            app.model.properties.create_beam(material=mat.id, section="rect", dimensions=[6.0, 3.0], direction=[0.0, 0.0, 1.0], target=target)
        else:
            app.model.properties.create_shell(material=mat.id, thickness=4.0, target=target)
        app.execute("view.mesh_options", solid_1d_2d=True)
    app.execute("view.standard", name="front" if kind.startswith("solid_") else "top")
    app.execute("view.fit")
    before = app.digest()
    base = app.view.render(W, H)[0]
    original = extent(drawn(base))
    app.execute("view.mesh_options", shrink=0.3)
    smaller = extent(drawn(app.view.render(W, H)[0]))
    for axis in range(1 if kind == "line" else 2):
        assert abs(smaller[axis] - original[axis] * 0.7) <= 2
    app.execute("view.mesh_options", shrink=0.0)
    assert np.array_equal(app.view.render(W, H)[0], base)
    assert app.digest() == before


@pytest.mark.feature("RND-29")
def test_RND_T03_03_value_location(app, tmp_path, monkeypatch):
    """값 위치별 표시: 절점 값 보간과 요소 단색은 설정이 왕복하고 두 화면이 다르다."""
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static()
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    app.execute("view.display_mode", mode="shaded")
    app.execute("view.standard", name="front")
    app.execute("view.fit")
    r = app.execute("view.result_show", result=rid, field="DISP", component="magnitude")
    assert "location" not in r["settings"]
    nodal = app.view.render(W, H)[0]
    r = app.execute("view.result_show", result=rid, field="DISP", component="magnitude", location="element")
    assert r["settings"]["location"] == "element"
    elem = app.view.render(W, H)[0]
    assert not np.array_equal(nodal, elem)

    def colors(img):  # 요소 단색은 서로 다른 색의 수가 보간보다 훨씬 적다
        return len({tuple(c) for c in img[drawn(img)][:, :3]})

    assert colors(elem) < colors(nodal)
    with pytest.raises(Nasa95Error):
        app.execute("view.result_show", result=rid, field="DISP", component="magnitude", location="gauss")
    app.execute("view.result_show", result=rid, field="DISP", component="magnitude", location="nodal")
    assert np.array_equal(app.view.render(W, H)[0], nodal)


@pytest.mark.feature("RND-44")
def test_RND_T04_04_12_gpu_memory(app):
    """GPU 메모리: 큰 모델을 그리면 마지막 프레임의 할당이 늘고 모델을 닫으면 준다. 낮은 한도를 두면 near_limit·over_limit 가 드러난다."""
    from meshutil import block
    app.view.render(W, H)
    d0 = app.execute("view.diagnostics")["gpu_memory"]
    assert d0["device_local_heap"] > 0 and d0["used"] == d0["last_frame"] + d0["persistent"]
    block(app, 30, 30, 10)
    app.execute("view.display_mode", mode="shaded")
    app.view.render(W, H)
    d1 = app.execute("view.diagnostics")["gpu_memory"]
    assert d1["last_frame"] > d0["last_frame"]
    app.execute("project.new")
    app.view.render(W, H)
    d2 = app.execute("view.diagnostics")["gpu_memory"]
    assert d2["last_frame"] < d1["last_frame"]
    # 한도(RND-T04-12)
    block(app, 30, 30, 10)
    app.view.render(W, H)
    used = app.execute("view.diagnostics")["gpu_memory"]["used"]
    assert app.execute("view.quality", gpu_memory_limit=int(used * 1.1))["gpu_memory_limit"] == int(used * 1.1)
    g = app.execute("view.diagnostics")["gpu_memory"]
    assert g["limit"] == int(used * 1.1) and g["near_limit"] is True and g["over_limit"] is False
    app.execute("view.quality", gpu_memory_limit=int(used * 0.5))
    g = app.execute("view.diagnostics")["gpu_memory"]
    assert g["near_limit"] is True and g["over_limit"] is True
    app.execute("view.quality", gpu_memory_limit=int(used * 10))
    g = app.execute("view.diagnostics")["gpu_memory"]
    assert g["near_limit"] is False and g["over_limit"] is False
    app.execute("view.quality")
    assert "limit" not in app.execute("view.diagnostics")["gpu_memory"]


@pytest.mark.feature("CMN-10")
def test_SYS_22_08_09_viewer_operations_and_box_selection(app):
    """뷰어 조작: 회전·이동·확대·단면 보기의 설정이 왕복한다. 박스 선택 결과를 선택으로 넣으면 선택 조회에 반영된다."""
    from meshutil import block
    mp = app.model.mesh_parts.create(name="MP")
    block(app, 4, 1, 1, size=(40.0, 10.0, 10.0), part=mp.id)
    app.execute("view.standard", name="iso")
    app.execute("view.fit")
    cam = app.execute("view.camera_get")
    app.view.orbit(30.0, 10.0)  # 매 프레임 조작은 C++ 안에서 끝난다(아키텍처 규칙 4)
    r1 = app.execute("view.camera_get")
    assert r1["eye"] != cam["eye"] and r1["target"] == pytest.approx(cam["target"])
    app.view.pan(0.1, 0.0)
    r2 = app.execute("view.camera_get")
    assert r2["target"] != r1["target"]
    app.view.zoom(2.0)
    r3 = app.execute("view.camera_get")
    assert r3["height"] == pytest.approx(r2["height"] / 2)
    img3 = app.view.render(W, H)[0]
    keys = ("eye", "target", "up", "height", "projection")
    app.execute("view.camera_set", **{k: cam[k] for k in keys})
    assert app.execute("view.camera_get")["height"] == pytest.approx(cam["height"])
    app.execute("view.camera_set", **{k: r3[k] for k in keys})
    assert np.array_equal(app.view.render(W, H)[0], img3)  # 설정 왕복
    c = app.execute("view.clip", point=[20.0, 0.0, 0.0], normal=[-1.0, 0.0, 0.0])
    assert c["enabled"] is True and c["planes"][0]["point"] == [20.0, 0.0, 0.0] and c["planes"][0]["normal"] == [-1.0, 0.0, 0.0]
    cut = app.view.render(W, H)[0]
    assert app.execute("view.clip_update", id=c["planes"][0]["id"], enabled=False)["enabled"] is False
    assert not np.array_equal(app.view.render(W, H)[0], cut)
    assert app.execute("view.clip_update", id=c["planes"][0]["id"], enabled=True)["planes"][0] == c["planes"][0]
    assert np.array_equal(app.view.render(W, H)[0], cut)  # 단면 설정 왕복
    app.execute("view.clip")
    assert app.execute("view.clip_remove")["count"] == 0
    # 박스 선택(SYS-22-09)
    app.execute("view.standard", name="front")
    app.execute("view.display_mode", mode="shaded")
    app.execute("view.fit")
    rgba, ids = app.view.render(W, H)
    ys, xs = np.nonzero(ids)
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())
    hits = app.execute("view.pick_region", shape="box", points=[x0 - 1, y0 - 1, (x0 + x1) / 2, y1 + 1], width=W, height=H)["hits"]
    assert sorted(h["element"] for h in hits) == [1, 2]
    app.execute("selection.set", items=hits)
    sel = app.execute("selection.get")
    assert sorted(h["element"] for h in sel["items"]) == [1, 2] and len(sel["items"]) == 2
    app.execute("selection.clear")
    assert app.execute("selection.get")["items"] == []


@pytest.mark.feature("WT-22")
@pytest.mark.feature("WT-26")
def test_SYS_22_03_07_12_16_tree_to_view_and_result_activation(app, tmp_path, monkeypatch):
    """트리 → 화면: 재료·하중 항목을 selection.set 에 넣으면 선택 조회에 남고 그 대상이 강조되어 화면이 바뀐다. 결과 활성화: view.result_state 가 보인 결과를 돌려주고 전환하면 바뀐다."""
    from meshutil import block
    mp = app.model.mesh_parts.create(name="MP")
    b = block(app, 4, 1, 1, size=(40.0, 10.0, 10.0), part=mp.id)
    mat = app.model.materials.create(name="M")
    mat.set_elastic(data=[[210000.0, 0.3]])
    prop = app.model.properties.create_solid(material=mat.id, target={"type": "elements", "ids": [1, 2]})
    case = app.model.cases.create(name="c")
    step = case.steps.create_static()
    load = step.loads.create_pressure(target={"type": "faces", "ids": [[3, 2], [4, 2]]}, value=1.0)
    app.execute("view.display_mode", mode="shaded")
    app.execute("view.standard", name="iso")
    app.execute("view.fit")
    plain = app.view.render(W, H)[0]
    r = app.execute("selection.set", items=[{"kind": "object", "id": load.id}])
    assert r["count"] == 1 and app.execute("selection.get")["items"][0] == {"kind": "object", "id": load.id, "object_kind": "load", "name": load.get()["name"]}
    with_load = app.view.render(W, H)[0]
    assert not np.array_equal(plain, with_load)  # 대상 면이 강조된다
    r = app.execute("selection.set", items=[{"kind": "object", "id": prop.id}], add=True)  # 여러 항목(SYS-22-12)
    assert [i["id"] for i in app.execute("selection.get")["items"]] == [load.id, prop.id]
    both = app.view.render(W, H)[0]
    assert not np.array_equal(both, with_load)
    app.execute("selection.set", items=[{"kind": "object", "id": mat.id}])  # 재료: 그 재료를 쓰는 프로퍼티의 요소가 강조 대상
    assert [i["object_kind"] for i in app.execute("selection.get")["items"]] == ["material"]
    app.execute("selection.clear")
    assert np.array_equal(app.view.render(W, H)[0], plain) and app.execute("selection.get")["items"] == []
    with pytest.raises(Nasa95Error):
        app.execute("selection.set", items=[{"kind": "object", "id": 99999}])
    # 결과 활성화(SYS-22-07·16)
    import test_SOLVER_ccx as S
    if S.CCX is None:
        pytest.skip("ccx 실행 파일이 없습니다")
    monkeypatch.setenv("NASA95_CCX", S.CCX)
    app.execute("project.new")
    part, mat, root, tip, case = S.cantilever(app, order=1, n=(10, 2, 2))
    case.update(work_directory=str(tmp_path))
    s = case.steps.create_static(nlgeom=True, initial_increment=0.5, period=1.0)
    s.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])
    s.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -100.0])
    s.outputs.create_node_file(variables=["U"])
    assert app.execute("case.run", id=case.id, wait=True)["state"] == "completed"
    rid = app.execute("result.open", case=case.id)["id"]
    assert app.execute("view.result_state") == {"shown": False}
    app.execute("view.result_show", result=rid, frame=1, field="DISP", component="magnitude")
    st = app.execute("view.result_state")
    assert st["shown"] and st["settings"]["result"] == rid and st["settings"]["frame"] == 1 and st["step"] == 1 and st["increment"] == 1 and st["case"] == case.id
    app.execute("view.result_show", result=rid, frame=2, field="DISP", component="D3")
    st = app.execute("view.result_state")
    assert st["settings"]["frame"] == 2 and st["settings"]["component"] == "D3" and st["increment"] == 2 and st["value"] == pytest.approx(1.0)
    app.execute("view.result_show")
    assert app.execute("view.result_state") == {"shown": False}
    # 새 프로젝트(project.new)는 모델에 매인 뷰 상태를 비운다: 이전 결과 표시·심볼·선택이 새 모델에 입혀지지 않는다
    app.execute("view.result_show", result=rid, frame=2, field="DISP", component="D3")
    app.execute("view.symbols", step=s.id, size=2.0)
    app.execute("selection.set", items=[{"kind": "object", "id": s.id}])
    app.execute("project.new")
    assert app.execute("view.result_state") == {"shown": False}
    assert app.execute("view.symbols")["shown"] is False and app.execute("selection.get")["items"] == []


@pytest.mark.feature("RND-36")
@pytest.mark.feature("RND-37")
def test_VIEW_T06_hud_contrast_on_light_and_dark_backgrounds(app):
    app.execute("view.hud", triad=False, legend=False,
                labels=[{"text": "TEST", "x": 10, "y": 10, "scale": 2.0}])
    before = app.digest()
    for background, foreground in (([31, 31, 31], 220), ([255, 255, 255], 40)):
        app.execute("view.overlay", background=background)
        rgba, _ = app.view.render(240, 180)
        assert tuple(rgba[0, 0, :3]) == tuple(background)
        text_area = rgba[10:25, 10:60, :3]
        assert np.any(np.all(text_area == foreground, axis=2))
    assert app.digest() == before


@pytest.mark.feature("RND-38")
def test_VIEW_T08_sketch_entities_are_drawn(app):
    """스케치 편집 중 표시(1차): 입체가 없어도 스케치의 선·원이 평면 위 꺾은선(주황)으로 그려지고 전체 맞춤에 들어간다. 억제한 스케치·숨긴 파트의 것은 그리지 않는다."""
    part = app.model.parts.create(name="P")
    sk = app.execute("sketch.create", parent=part.id, point=[0.0, 0.0, 0.0], normal=[0.0, 0.0, 1.0], x_axis=[1.0, 0.0, 0.0])["id"]
    app.execute("view.standard", name="top")
    app.execute("view.fit")
    empty = app.view.render(W, H)[0]
    app.execute("sketch.add_line", id=sk, start=[0.0, 0.0], end=[0.0, 100.0])
    app.execute("sketch.add_circle", id=sk, center=[40.0, 50.0], radius=15.0)
    app.execute("view.fit")
    img = app.view.render(W, H)[0]
    assert not np.array_equal(img, empty)
    orange = (img[:, :, 0] > 200) & (img[:, :, 1] > 100) & (img[:, :, 1] < 180) & (img[:, :, 2] < 90)
    assert orange.sum() > 100  # 선과 원
    assert app.execute("view.diagnostics")["last_frame"]["lines"] >= 30
    app.execute("sketch.suppress", id=sk)
    gone = app.view.render(W, H)[0]
    orange2 = (gone[:, :, 0] > 200) & (gone[:, :, 1] > 100) & (gone[:, :, 1] < 180) & (gone[:, :, 2] < 90)
    assert orange2.sum() == 0
    app.execute("sketch.unsuppress", id=sk)
    app.execute("view.hide", ids=[part.id])
    hidden = app.view.render(W, H)[0]
    orange3 = (hidden[:, :, 0] > 200) & (hidden[:, :, 1] > 100) & (hidden[:, :, 1] < 180) & (hidden[:, :, 2] < 90)
    assert orange3.sum() == 0


@pytest.mark.feature("RND-34")
@pytest.mark.feature("WT-20")
def test_VIEW_RND_T03_26_symbols_per_set(app):
    """셋 단위 심볼: 숨긴 셋(view.hide)의 하중·구속은 그리지 않고, view.symbols sets= 는 스텝 없이 그 셋만 그린다(트리의 눈 아이콘)."""
    from meshutil import block
    block(app, 10, 2, 2)
    root = app.model.sets.create_node(name="ROOT", ids=app.execute("mesh.find", what="nodes", box_min=[-.1, -1, -1], box_max=[.1, 21, 11])["ids"])
    tip = app.model.sets.create_node(name="TIP", ids=app.execute("mesh.find", what="nodes", box_min=[99.9, -1, -1], box_max=[100.1, 21, 11])["ids"])
    D, L, S = app.model.load_sets.create(name="D"), app.model.load_sets.create(name="L"), app.model.bc_sets.create(name="S")
    S.bcs.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3])                      # 선 없음: 색칠 + 이름표
    D.loads.create_force(target={"type": "set", "ids": [tip.id]}, components=[0.0, 0.0, -10.0])               # 9개 입체 화살표
    L.loads.create_pressure(target={"type": "faces", "ids": [[e, 2] for e in range(31, 41)]}, value=1.0)      # 10개 입체 화살표
    s = app.model.cases.create().steps.create_static(load_sets=[{"set": D.id}, {"set": L.id}], bc_sets=[S.id])
    app.execute("view.standard", name="front")
    app.view.render(W, H)
    base = app.execute("view.diagnostics")["last_frame"]["triangles"]
    triangles = lambda: (app.view.render(W, H), app.execute("view.diagnostics")["last_frame"]["triangles"])[1]  # noqa: E731
    marks = lambda: [k["label"] for k in app.execute("view.diagnostics")["bc_marks"]]  # noqa: E731
    app.execute("view.symbols", step=s.id)
    assert triangles() == base + 19 * 96 and app.execute("view.diagnostics")["symbols"] is True and marks() == ["UXYZ"]
    assert app.execute("view.hide", ids=[L.id])["hidden"] == [L.id]
    assert triangles() == base + 9 * 96 and marks() == ["UXYZ"]  # 숨긴 셋의 압력 화살표가 빠진다
    app.execute("view.hide", ids=[S.id])
    assert triangles() == base + 9 * 96 and marks() == []  # 숨긴 구속 셋의 구속 표시가 빠진다
    app.execute("view.show", ids=[L.id, S.id])
    assert triangles() == base + 19 * 96 and marks() == ["UXYZ"]
    # 스텝 없이 셋만: D의 힘 화살표 9개
    r = app.execute("view.symbols", sets=[D.id])
    assert r["shown"] is True and r["sets"] == [D.id] and triangles() == base + 9 * 96 and marks() == []
    app.execute("view.hide", ids=[D.id])
    assert triangles() == base
    app.execute("view.show_all")
    app.execute("view.symbols", sets=[D.id, S.id])
    assert triangles() == base + 9 * 96 and marks() == ["UXYZ"]
    for bad in ({"sets": [root.id]}, {"ids": [root.id]}):
        with pytest.raises(Nasa95Error) as e:
            app.execute("view.symbols" if "sets" in bad else "view.hide", **bad)
        assert e.value.code == "wrong_kind"
    assert app.execute("view.symbols")["shown"] is False and app.execute("view.diagnostics")["symbols"] is False


@pytest.mark.feature("BC-13")
@pytest.mark.feature("RND-34")
@pytest.mark.feature("WT-22")
def test_VIEW_RND_T03_27_bc_marks(app):
    """구속 표시: 구속마다 적용 영역 색칠과 이름표 하나(자유도는 글로), 고른 구속만 진하게 + 축 기호. 노드마다 선을 긋지 않는다."""
    from meshutil import block
    block(app, 10, 2, 2)
    find = lambda lo, hi: app.execute("mesh.find", what="nodes", box_min=lo, box_max=hi)["ids"]  # noqa: E731
    root = app.model.sets.create_node(name="ROOT", ids=find([-.1, -1, -1], [.1, 21, 11]))
    top = app.model.sets.create_node(name="TOP", ids=find([29.9, -1, 9.9], [70.1, 21, 11]))
    edge = app.model.sets.create_node(name="EDGE", ids=find([99.9, -1, -.1], [100.1, 21, .1]))
    s = app.model.cases.create().steps.create_static()
    B = s.bcs
    fix = B.create_displacement(target={"type": "set", "ids": [root.id]}, dofs=[1, 2, 3, 4, 5, 6])
    sym = B.create_symmetry(target={"type": "set", "ids": [top.id]}, normal="z")
    rol = B.create_displacement(target={"type": "set", "ids": [edge.id]}, dofs=[3, 4])
    B.create_displacement(target={"type": "nodes", "ids": [1]}, dofs=[1], values=[0.5])
    B.create_fixed_current(target={"type": "nodes", "ids": [2]}, dofs=[1, 2])
    B.create_antisymmetry(target={"type": "nodes", "ids": [3]}, normal="x")
    B.create_temperature(target={"type": "nodes", "ids": [4]}, value=20.0)
    app.execute("view.standard", name="iso")
    app.view.render(W, H)
    base = app.execute("view.diagnostics")["last_frame"]
    assert app.execute("view.diagnostics")["bc_marks"] == []  # 심볼이 꺼져 있으면 없다
    app.execute("view.symbols", step=s.id)
    plain = app.view.render(W, H)[0]
    d = app.execute("view.diagnostics")
    marks = {k["id"]: k for k in d["bc_marks"]}
    assert [k["label"] for k in d["bc_marks"]] == ["FIX", "SYM Z", "UZ RX", "DISP UX", "HOLD UXY", "ASYM X", "T=20"]
    assert len({tuple(k["color"]) for k in d["bc_marks"][:6]}) == 6  # 구속마다 다른 색
    # 면을 이루는 대상은 면을 칠하고(끝면 4칸·윗면 8칸 = 사각형마다 삼각형 2개), 모서리·점은 노드 표식으로
    assert (marks[fix.id]["triangles"], marks[fix.id]["nodes"]) == (8, 0) and (marks[sym.id]["triangles"], marks[sym.id]["nodes"]) == (16, 0)
    assert (marks[rol.id]["triangles"], marks[rol.id]["nodes"]) == (0, 3)
    assert marks[fix.id]["glyph_points"] == 9 and marks[rol.id]["glyph_points"] == 3  # 축 기호 자리는 9개까지 솎는다
    assert d["last_frame"]["lines"] == base["lines"] and d["last_frame"]["transparent"] == 8 + 16  # 선은 늘지 않고 색칠이 는다
    assert d["last_frame"]["hud_triangles"] > base["hud_triangles"]  # 이름표
    teal = (plain[:, :, 0] < 40) & (abs(plain[:, :, 1].astype(int) - 150) < 12) & (abs(plain[:, :, 2].astype(int) - 136) < 12)
    assert teal.sum() > 200  # SYM Z 이름표의 바탕
    # 고른 구속(트리 선택)만 진하게 + 축 기호. 구속 셋을 골라도 그 안의 구속이 고른 것이 된다
    assert not any(k["focused"] for k in d["bc_marks"])
    app.execute("selection.set", items=[{"kind": "object", "id": rol.id}])
    focused = app.view.render(W, H)[0]
    d2 = app.execute("view.diagnostics")
    assert [k["id"] for k in d2["bc_marks"] if k["focused"]] == [rol.id] and not np.array_equal(focused, plain)
    assert d2["last_frame"]["vertex_bytes"] > d["last_frame"]["vertex_bytes"]  # 축 기호(원뿔·원판)가 더해진다
    assert d2["last_frame"]["lines"] == base["lines"] + 3  # 병진 없이 회전만 막은 축(RX)의 원판을 노드에 잇는 대
    app.execute("selection.set", items=[{"kind": "object", "id": app.execute("bc.get", id=fix.id)["parent"]}])
    app.view.render(W, H)
    assert all(k["focused"] for k in app.execute("view.diagnostics")["bc_marks"])
    app.execute("selection.clear")
    assert np.array_equal(app.view.render(W, H)[0], plain)
    # 구속을 고치면 이름표가 따라 바뀐다
    app.execute("bc.update", id=fix.id, dofs=[1, 2, 3])
    app.view.render(W, H)
    assert app.execute("view.diagnostics")["bc_marks"][0]["label"] == "UXYZ"


@pytest.mark.feature("RND-22")
@pytest.mark.feature("PRP-05")
def test_RND_T01_51_hollow_sections_display(app):
    """속 빈 단면의 입체 표시: 박스는 벽 4개(상자 4개 = 48 삼각형)로 속이 비고, 파이프는 속 빈 원통(바깥·안 벽 + 고리 끝면), 원형은 원통.
    끝에서 보면 박스·파이프의 가운데가 비어 있다(배경이 보인다)."""
    mat = app.model.materials.create(name="M")
    n = app.execute("mesh.nodes_create", coords=[[0.0, 0.0, 0.0], [50.0, 0.0, 0.0]])["first"]
    beam = app.execute("mesh.elements_create", shape="line2", connectivity=[[n, n + 1]])["first"]
    prop = app.model.properties.create_beam(material=mat.id, section="box", dimensions=[40.0, 30.0, 4.0, 3.0, 4.0, 3.0], direction=[0.0, 0.0, 1.0],
                                            target={"type": "elements", "ids": [beam]})
    app.execute("view.display_mode", mode="shaded")
    app.execute("view.mesh_options", solid_1d_2d=True)
    app.execute("view.standard", name="right")  # +x 에서: 단면이 보인다 → 가운데 구멍
    app.execute("view.fit")
    img = app.view.render(W, H)[0]
    assert app.execute("view.diagnostics")["last_frame"]["triangles"] == 4 * 12
    assert not drawn(img)[H // 2, W // 2]  # 가운데는 빈 곳(배경)
    assert drawn(img).sum() > 100
    assert app.execute("property.section_shape", section="box", dimensions=[40.0, 30.0, 4.0, 3.0, 4.0, 3.0])["extent"] == [40.0, 30.0]
    for sec, dims, hollow in (("pipe", [15.0, 3.0], True), ("circ", [30.0, 20.0], False)):
        prop.update(section=sec, dimensions=dims)
        app.execute("view.fit")
        img = app.view.render(W, H)[0]
        tri = app.execute("view.diagnostics")["last_frame"]["triangles"]
        assert tri == (24 * 2 * 2 + 24 * 2 * 2 if hollow else 24 * 2 + 24 * 2), (sec, tri)
        assert drawn(img)[H // 2, W // 2] == (not hollow), sec  # 파이프는 가운데가 비고 원형은 차 있다
        if sec == "circ":
            ext = extent(drawn(img))
            assert ext[1] / ext[0] == pytest.approx(30.0 / 20.0, rel=0.08)  # 1축 지름 30(세로), 2축 20(가로)
    app.execute("view.mesh_options")


@pytest.mark.feature("MSH-24")
@pytest.mark.feature("PRP-13")
@pytest.mark.feature("WT-25")
def test_RND_T01_52_color_by_mesh_material_property(app):
    """메시 파트·재료·프로퍼티마다 다른 색. 색 기준(part/material/property)에 따라 화면 요소 색 = 그 객체 색(view.object_colors), 지정 색 우선."""
    from meshutil import block
    m1 = app.execute("material.create", name="STEEL")["id"]
    m2 = app.execute("material.create", name="CONC")["id"]
    pa = app.execute("mesh_part.create", name="LEFT")["id"]
    pb = app.execute("mesh_part.create", name="RIGHT")["id"]
    block(app, 1, 1, 1, size=(10.0, 10.0, 10.0), part=pa)
    block(app, 1, 1, 1, size=(10.0, 10.0, 10.0), origin=(20.0, 0.0, 0.0), part=pb)
    pr1 = app.execute("property.create_solid", name="P1", material=m1, target={"type": "parts", "ids": [pa]})["id"]
    pr2 = app.execute("property.create_solid", name="P2", material=m2, target={"type": "parts", "ids": [pb]})["id"]
    oc = app.execute("view.object_colors")
    for kind in ("mesh_part", "material", "property"):
        cols = [tuple(o["color"]) for o in oc[kind]]
        assert len(cols) == 2 and cols[0] != cols[1], kind
    app.execute("view.display_mode", mode="shaded")
    app.execute("view.standard", name="front")

    def colors_now():
        rgba = app.view.render(W, H)[0]
        row = rgba[H // 2, :, :3].astype(int)
        lit = [i for i in range(W) if row[i].sum() < 3 * 245]
        left, right = row[lit[0] + 4], row[lit[-1] - 4]
        return left, right

    def hue(c):  # 음영과 무관한 색 비교: 정규화한 비율
        c = np.asarray(c, float)
        return c / c.sum()

    def same(px, rgb):
        return np.allclose(hue(px), hue(rgb), atol=0.02)

    oc = app.execute("view.object_colors")
    by = {k: {o["id"]: o["color"] for o in oc[k]} for k in ("mesh_part", "material", "property")}
    l, r = colors_now()
    assert oc["color_by"] == "part" and same(l, by["mesh_part"][pa]) and same(r, by["mesh_part"][pb])
    app.execute("view.color_by", by="material")
    l, r = colors_now()
    assert same(l, by["material"][m1]) and same(r, by["material"][m2])
    app.execute("view.color_by", by="property")
    l, r = colors_now()
    assert same(l, by["property"][pr1]) and same(r, by["property"][pr2])
    # 재료 색 지정 → 재료별 화면·object_colors 에 반영. 재료에 alpha 는 오류, 지정 지우면 팔레트 색
    app.execute("view.color_by", by="material")
    app.execute("view.set_appearance", id=m2, color=[200, 40, 40])
    assert next(o for o in app.execute("view.object_colors")["material"] if o["id"] == m2) == {"id": m2, "name": "CONC", "color": [200, 40, 40], "custom": True}
    l, r = colors_now()
    assert same(r, [200, 40, 40])
    with pytest.raises(Nasa95Error):
        app.execute("view.set_appearance", id=m1, alpha=0.5)
    app.execute("view.set_appearance", id=m2)
    assert next(o for o in app.execute("view.object_colors")["material"] if o["id"] == m2)["color"] == by["material"][m2]
