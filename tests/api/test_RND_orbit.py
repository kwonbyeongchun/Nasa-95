"""RND-T01-19, 26~31: 화면 축 회전과 클릭점 고정. 투영은 numpy 로 독립 계산한다."""
import numpy as np
import pytest

from openfep import OfepError
from conftest import history_len
from test_VIEW_render import box, needs_geometry


def unit(v):
    return v / np.linalg.norm(v)


def project(camera, point, width=800, height=600):
    eye, target, up = (np.array(camera[k]) for k in ("eye", "target", "up"))
    forward = unit(target - eye)
    right = unit(np.cross(forward, up))
    up = np.cross(right, forward)
    rel = np.array(point) - eye
    half = camera["height"] / 2 if camera["projection"] == "orthographic" else np.dot(rel, forward) * np.tan(np.radians(camera["fov"] / 2))
    return np.array([width / 2 + np.dot(rel, right) * height / (2 * half),
                     height / 2 - np.dot(rel, up) * height / (2 * half)])


def top_camera(app, projection="orthographic"):
    return app.execute("view.camera_set", eye=[5., 10., 70.], target=[5., 10., 15.], up=[0., 1., 0.],
                       projection=projection, height=40., fov=50.)


@pytest.mark.feature("RND-09")
@pytest.mark.parametrize("projection", ["orthographic", "perspective"])
def test_RND_T01_19_explicit_pivot_stays_at_screen_position(app, projection):
    camera = top_camera(app, projection)
    pivot = [2., 7., 30.]
    app.execute("view.camera_set", pivot=pivot)
    pixel = project(camera, pivot)
    distance = np.linalg.norm(np.subtract(camera["eye"], camera["target"]))
    before, history = app.digest(), history_len(app)
    for dx, dy in [(45., 80.), (-70., 50.), (0., 600.), (190., -320.)] * 20:
        camera = app.view.orbit(dx, dy, height=600)
        assert project(camera, pivot) == pytest.approx(pixel, abs=1e-8)
        assert camera["pivot"] == pivot
        forward = np.subtract(camera["target"], camera["eye"])
        assert np.linalg.norm(forward) == pytest.approx(distance)
        assert np.linalg.norm(camera["up"]) == pytest.approx(1.)
        assert np.dot(unit(forward), camera["up"]) == pytest.approx(0., abs=1e-12)
    assert app.digest() == before and history_len(app) == history


@needs_geometry
@pytest.mark.feature("RND-09")
@pytest.mark.parametrize("projection", ["orthographic", "perspective"])
@pytest.mark.parametrize("mode", ["shaded_edges", "wireframe"])
def test_RND_T01_26_click_surface_without_recentering(app, projection, mode):
    box(app)
    camera = top_camera(app, projection)
    app.execute("view.display_mode", mode=mode)
    pivot = [7., 13., 30.]
    x, y = project(camera, pivot)
    before, history = app.digest(), history_len(app)
    hit = app.view.orbit_begin(x, y, width=800, height=600)
    assert hit["hit"] and hit["pivot"] == pytest.approx(pivot, abs=1e-10)
    after = app.execute("view.camera_get")
    assert {k: v for k, v in after.items() if k != "pivot"} == {k: v for k, v in camera.items() if k != "pivot"}
    rotated = app.view.orbit(55., -35., height=600)
    assert project(rotated, pivot) == pytest.approx([x, y], abs=1e-9)
    assert app.execute("view.display_mode")["mode"] == mode
    assert app.digest() == before and history_len(app) == history


@pytest.mark.feature("RND-09")
def test_RND_T01_27_screen_axes_and_free_poles(app):
    # 화면을 90도 롤한 카메라: 화면 세로는 세계 X 축이다.
    app.execute("view.camera_set", eye=[0., 0., 10.], target=[0., 0., 0.], up=[1., 0., 0.])
    horizontal = app.view.orbit(300., 0., height=600)
    assert horizontal["eye"] == pytest.approx([0., 10., 0.], abs=1e-12)
    assert horizontal["up"] == pytest.approx([1., 0., 0.], abs=1e-12)
    app.execute("view.camera_set", eye=[0., 0., 10.], target=[0., 0., 0.], up=[0., 1., 0.])
    # 작은 연속 이동으로 종전 극점 제한을 지나 한 바퀴 돌린다.
    for i in range(1, 121):
        camera = app.view.orbit(0., 10., height=600)
        angle = np.pi * i / 60
        assert camera["eye"] == pytest.approx([0., 10 * np.sin(angle), 10 * np.cos(angle)], abs=1e-10)
        assert camera["up"] == pytest.approx([0., np.cos(angle), -np.sin(angle)], abs=1e-10)


@needs_geometry
@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-19")
@pytest.mark.feature("RND-23")
def test_RND_T01_28_visibility_clipping_and_empty_fallback(app):
    part = box(app)
    camera = top_camera(app)
    app.execute("view.camera_set", pivot=[3., 4., 5.])
    assert app.view.orbit_begin(1., 1.) == {"hit": False, "pivot": [3., 4., 5.]}
    x, y = project(camera, [7., 13., 30.])
    app.execute("view.hide", ids=[part.id])
    assert not app.view.orbit_begin(x, y)["hit"]
    app.execute("view.show_all")
    # alpha 는 API 에서 (0,1] 범위. 0.001 은 렌더러의 8비트 알파에서 0 이 된다.
    app.execute("view.set_appearance", id=part.id, alpha=0.001)
    assert not app.view.orbit_begin(x, y)["hit"]
    app.execute("view.set_appearance", id=part.id, alpha=0.3)
    assert app.view.orbit_begin(x, y)["pivot"] == pytest.approx([7., 13., 30.])
    app.execute("view.clip_add", point=[0., 0., 10.], normal=[0., 0., -1.])
    clipped = app.view.orbit_begin(x, y)
    assert clipped["hit"] and clipped["pivot"] == pytest.approx([7., 13., 0.])
    app.execute("view.hide", ids=[part.id])
    assert app.view.orbit_begin(x, y) == {"hit": False, "pivot": clipped["pivot"]}


@needs_geometry
@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
def test_RND_T01_29_saved_pivot_and_fit_reset(app):
    box(app)
    top_camera(app)
    camera = app.execute("view.camera_set", pivot=[7., 13., 30.])
    app.execute("view.save", name="pivot")
    app.view.orbit(60., 90.)
    app.execute("view.restore", name="pivot")
    assert app.execute("view.camera_get") == camera
    for command, params in [("view.fit", {}), ("view.standard", {"name": "iso"})]:
        app.execute("view.camera_set", pivot=[99., 98., 97.])
        fit = app.execute(command, **params)
        assert fit["pivot"] == fit["target"] == [5., 10., 15.]
    app.execute("project.new")
    assert app.view.orbit_begin(0., 0.) == {"hit": False, "pivot": [0., 0., 0.]}


@pytest.mark.feature("RND-09")
@pytest.mark.parametrize("command,params", [
    ("view.orbit_begin", {"x": -1., "y": 0.}),
    ("view.orbit_begin", {"x": 800., "y": 0.}),
    ("view.orbit_begin", {"x": 0., "y": 600.}),
    ("view.orbit_begin", {"x": 0., "y": 0., "width": 0}),
    ("view.orbit_begin", {"x": 0., "y": 0., "height": 0}),
    ("view.orbit", {"dx": 1., "dy": 1., "height": -1}),
])
def test_RND_T01_30_invalid_input_preserves_camera(app, command, params):
    camera = top_camera(app)
    with pytest.raises(OfepError):
        app.execute(command, **params)
    assert app.execute("view.camera_get") == camera


@needs_geometry
@pytest.mark.feature("RND-09")
@pytest.mark.parametrize("scale", [1., 1.25, 1.5, 2.])
def test_RND_T01_31_coordinate_scale_independence(app, scale):
    box(app)
    camera = top_camera(app)
    point = [7., 13., 30.]
    x, y = project(camera, point)
    first = app.view.orbit_begin(x, y, width=800, height=600)
    expected = app.view.orbit(50., -30., height=600)
    app.execute("view.camera_set", **camera)
    second = app.view.orbit_begin(x * scale, y * scale, width=int(800 * scale), height=int(600 * scale))
    actual = app.view.orbit(50. * scale, -30. * scale, height=int(600 * scale))
    assert first["pivot"] == pytest.approx(second["pivot"])
    for key in ("eye", "target", "up", "pivot"):
        assert actual[key] == pytest.approx(expected[key], abs=1e-10)


@pytest.mark.feature("RND-09")
@pytest.mark.parametrize("projection", ["orthographic", "perspective"])
def test_RND_T01_45_zoom_region(app, projection):
    """영역 확대: 화면 사각형의 가운데가 화면 가운데로 오고, 사각형이 가로·세로 모두 들어가는 배율로 확대된다."""
    camera = top_camera(app, projection)
    w, h = 800, 600
    x0, y0, x1, y1 = 500., 100., 700., 250.  # 가운데 (600, 175), 200×150
    # 사각형 가운데를 지나는 월드 점(목표 평면 위): 이전 카메라로 역투영
    eye, target, up = (np.array(camera[k]) for k in ("eye", "target", "up"))
    forward = unit(target - eye)
    right = unit(np.cross(forward, up))
    up = np.cross(right, forward)
    half = camera["height"] / 2 if projection == "orthographic" else np.linalg.norm(target - eye) * np.tan(np.radians(camera["fov"] / 2))
    world_per_pixel = 2 * half / h
    centre_point = target + right * (600 - w / 2) * world_per_pixel - up * (175 - h / 2) * world_per_pixel
    assert project(camera, centre_point, w, h) == pytest.approx([600., 175.])
    before, history = app.digest(), history_len(app)
    after = app.execute("view.zoom_region", x0=x0, y0=y0, x1=x1, y1=y1, width=w, height=h)
    assert project(after, centre_point, w, h) == pytest.approx([w / 2, h / 2], abs=1e-8)
    assert after["pivot"] == pytest.approx(after["target"]) and after["target"] == pytest.approx(centre_point.tolist())
    factor = h / max(150., 200. * h / w)  # 세로 150 과 가로 200(화면 비율 보정 → 150) 가운데 큰 쪽 = 4배
    assert factor == pytest.approx(4.0)
    if projection == "orthographic":
        assert after["height"] == pytest.approx(camera["height"] / factor)
    else:
        assert np.linalg.norm(np.subtract(after["eye"], after["target"])) == pytest.approx(np.linalg.norm(target - eye) / factor)
    # 사각형의 네 모서리 점이 모두 화면 안에, 그리고 긴 변이 화면에 꽉 찬다
    for px, py in ((x0, y0), (x1, y1)):
        p = target + right * (px - w / 2) * world_per_pixel - up * (py - h / 2) * world_per_pixel
        s = project(after, p, w, h)
        assert -1e-9 <= s[0] <= w + 1e-9 and -1e-9 <= s[1] <= h + 1e-9
    assert project(after, target + right * (x0 - w / 2) * world_per_pixel - up * (y0 - h / 2) * world_per_pixel, w, h)[1] == pytest.approx(0.0, abs=1e-8)
    assert app.digest() == before and history_len(app) == history
    # 1픽셀 이하 사각형(클릭만)은 오류 없이 처리되고, 범위 밖 크기는 구조화 오류
    app.execute("view.zoom_region", x0=10, y0=10, x1=10, y1=10, width=w, height=h)
    with pytest.raises(OfepError) as e:
        app.execute("view.zoom_region", x0=0, y0=0, x1=5, y1=5, width=0, height=h)
    assert e.value.code == "out_of_range"
