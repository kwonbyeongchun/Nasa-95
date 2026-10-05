"""입체 하중 화살표의 원통 폭·원뿔 테이퍼·곡면 음영을 실제 Vulkan 픽셀로 검사한다."""
import numpy as np
import pytest

from nasa95 import App
from test_VIEW_render import _available
from test_RND_orbit import project

pytestmark = [pytest.mark.skipif(not _available(), reason="Vulkan 필요"),
              pytest.mark.feature("RND-34"), pytest.mark.feature("LOD-16")]


@pytest.fixture(scope="module")
def arrow_app():
    return App()


@pytest.mark.parametrize("background", [[255, 255, 255], [58, 58, 58]])
@pytest.mark.parametrize("back,up", [([0., 0., 1.], [0., 1., 0.]),
                                    ([0., 1., 0.], [0., 0., 1.]),
                                    ([0., 1., 1.], [0., 0., 1.])])
def test_RND_T03_28_cylinder_cone_silhouette_and_shading(arrow_app, background, back, up):
    app = arrow_app
    app.execute("project.new")
    app.execute("mesh.nodes_create", coords=[[0., 0., 0.], [-12., -12., -12.], [12., 12., 12.]])
    loads = app.model.load_sets.create(name="Force")
    loads.loads.create_force(target={"type": "nodes", "ids": [1]}, components=[1., 0., 0.])
    center = np.array([-3., 0., 0.])
    back = np.asarray(back) / np.linalg.norm(back)
    camera = app.execute("view.camera_set", eye=(center + 30. * back).tolist(), target=center.tolist(),
                         up=up, projection="orthographic", height=16.)
    app.execute("view.hud", triad=False, legend=False)
    app.execute("view.overlay", background=background)
    app.execute("view.quality", antialiasing="ssaa2")
    plain = app.view.render(600, 400)[0]
    base = app.execute("view.diagnostics")["last_frame"]
    before, history = app.digest(), app.execute("app.history")
    app.execute("view.symbols", sets=[loads.id], size=8.)
    rgba, ids = app.view.render(600, 400)
    rgb = rgba[:, :, :3].astype(float)
    red = (rgb[:, :, 0] > 50) & (rgb[:, :, 0] > 2 * rgb[:, :, 1]) & (rgb[:, :, 0] > 2 * rgb[:, :, 2])

    def column(fraction):
        x, y = project(camera, [-8. * fraction, 0., 0.], 600, 400)
        return int(round(x)), int(round(y))

    def width(fraction):
        x, y = column(fraction)
        return np.count_nonzero(red[y-40:y+41, x])

    # L=8, 25px/단위. 몸통 지름=.09L → 18px, 촉 밑면 지름=.26L → 52px.
    assert 15 <= width(.55) <= 21
    assert abs(width(.55) - width(.85)) <= 2  # 원통 몸통은 일정한 폭
    assert 32 <= width(.20) <= 42             # 원뿔의 선형 테이퍼
    assert width(.06) < width(.55) < width(.20)
    x, y = column(.55)
    shades = rgb[y-7:y+8, x, 0]
    assert shades.max() - shades.min() > 25  # 법선 보간으로 곡면 음영
    assert not ids[red].any()                # 하중에 모델 ID를 부여하지 않는다
    frame = app.execute("view.diagnostics")["last_frame"]
    assert frame["lines"] == base["lines"] and frame["triangles"] > base["triangles"]
    assert app.execute("view.camera_get") == camera
    assert app.digest() == before and app.execute("app.history") == history
    app.execute("view.hide", ids=[loads.id])
    assert np.array_equal(app.view.render(600, 400)[0], plain)
    app.execute("view.show", ids=[loads.id])
    assert np.array_equal(app.view.render(600, 400)[0], rgba)
    app.execute("view.symbols")
    assert np.array_equal(app.view.render(600, 400)[0], plain)


def test_RND_T03_29_load_arrow_size_direction_and_wireframe(arrow_app):
    app = arrow_app
    app.execute("project.new")
    app.execute("mesh.nodes_create", coords=[[0., 0., 0.], [-12., -12., -12.], [12., 12., 12.]])
    loads = app.model.load_sets.create(name="Force")
    force = loads.loads.create_force(target={"type": "nodes", "ids": [1]}, components=[1., 0., 0.])
    app.execute("view.camera_set", eye=[0., 0., 30.], target=[0., 0., 0.], up=[0., 1., 0.], height=24.)
    app.execute("view.hud", triad=False, legend=False)
    app.execute("view.overlay", background=[255, 255, 255])
    app.execute("view.display_mode", mode="wireframe")

    def red_pixels(size):
        app.execute("view.symbols", sets=[loads.id], size=size)
        rgb = app.view.render(600, 400)[0][:, :, :3].astype(float)
        return np.nonzero((rgb[:, :, 0] > 50) & (rgb[:, :, 0] > 2 * rgb[:, :, 1]) & (rgb[:, :, 0] > 2 * rgb[:, :, 2]))

    y, x = red_pixels(4.)
    yy, xx = red_pixels(8.)
    assert len(xx) > 3 * len(x)  # 길이와 두께 모두 두 배
    assert x.max() <= 301 and xx.max() <= 301
    assert xx.max() - xx.min() > 1.9 * (x.max() - x.min())
    force.update(components=[-1., 0., 0.])
    _, reverse_x = red_pixels(8.)
    assert reverse_x.min() >= 298 and reverse_x.max() > 425
    force.update(components=[0., 0., 0.])
    assert len(red_pixels(8.)[0]) == 0
