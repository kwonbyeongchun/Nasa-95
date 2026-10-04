"""방향 큐브 26개 방향 및 실제 판정 영역과 같은 호버 표시."""
import numpy as np
import pytest

from conftest import history_len
from test_VIEW_render import _available
from openfep import App

pytestmark = pytest.mark.skipif(not _available(), reason="Vulkan 필요")
TARGETS = {
    "right": (1, 0, 0), "left": (-1, 0, 0), "front": (0, -1, 0), "back": (0, 1, 0), "top": (0, 0, 1), "bottom": (0, 0, -1),
    "front-right": (1, -1, 0), "front-left": (-1, -1, 0), "back-right": (1, 1, 0), "back-left": (-1, 1, 0),
    "right-top": (1, 0, 1), "right-bottom": (1, 0, -1), "left-top": (-1, 0, 1), "left-bottom": (-1, 0, -1),
    "front-top": (0, -1, 1), "front-bottom": (0, -1, -1), "back-top": (0, 1, 1), "back-bottom": (0, 1, -1),
    "front-right-top": (1, -1, 1), "front-right-bottom": (1, -1, -1), "front-left-top": (-1, -1, 1), "front-left-bottom": (-1, -1, -1),
    "back-right-top": (1, 1, 1), "back-right-bottom": (1, 1, -1), "back-left-top": (-1, 1, 1), "back-left-bottom": (-1, 1, -1),
}
PIXEL = dict(x=88., y=220., width=400, height=300)


@pytest.fixture(scope="module")
def cube_app():
    # 26개 방향은 같은 Vulkan 장치를 재사용한다. 장치 생성 자체는 이 검사의 대상이 아니다.
    return App()


@pytest.fixture
def app(cube_app):
    cube_app.execute("project.new")
    return cube_app


@pytest.mark.feature("RND-10")
@pytest.mark.feature("RND-37")
@pytest.mark.feature("RND-49")
@pytest.mark.parametrize("name,direction", TARGETS.items())
def test_RND_T01_42_RND_T01_43_all_regions_pick_hover_and_animate(app, name, direction):
    d = np.array(direction, dtype=float)
    up = [0., 1., 0.] if d[0] == d[1] == 0 else [0., 0., 1.]
    camera = app.execute("view.camera_set", eye=(d * 10).tolist(), target=[0., 0., 0.], up=up, height=10.)
    app.execute("view.hud", navigation_cube=True, legend=False)
    before, history = app.digest(), history_len(app)
    # 각 면·에지·꼭짓점을 정면으로 보면 그 영역이 큐브 화면 중심에 놓인다.
    assert app.execute("view.cube_pick", **PIXEL) == {"hit": True, "view": name}
    image, ids = app.view.render(400, 300)
    assert app.execute("view.cube_hover", **PIXEL) == {"hit": True, "view": name, "changed": True}
    assert app.execute("view.cube_hover", **PIXEL) == {"hit": True, "view": name, "changed": False}
    highlighted, highlighted_ids = app.view.render(400, 300)
    assert np.any(image != highlighted, axis=2).sum() > 5
    assert np.array_equal(ids, highlighted_ids) and not ids.any()
    assert app.execute("view.camera_get") == camera
    assert app.execute("view.cube_hover") == {"hit": False, "changed": True}
    assert np.array_equal(image, app.view.render(400, 300)[0])
    app.execute("view.transition_begin", name=name)
    app.execute("view.transition_step", progress=0.5)
    final = app.execute("view.transition_step", progress=1.)
    back = np.subtract(final["camera"]["eye"], final["camera"]["target"])
    assert back / np.linalg.norm(back) == pytest.approx(d / np.linalg.norm(d), abs=1e-12)
    assert not final["active"]
    assert app.digest() == before and history_len(app) == history


@pytest.mark.feature("RND-10")
@pytest.mark.feature("RND-37")
@pytest.mark.feature("RND-49")
def test_RND_T01_42_RND_T01_43_wide_edge_corner_and_hover_clear(app):
    app.execute("view.standard", name="front")
    app.execute("view.hud", navigation_cube=True, legend=False)
    for x, y, expected in [(88., 220., "front"), (88 + 24 * 0.7, 220., "front-right"),
                           (88 + 24 * 0.7, 220 - 24 * 0.7, "front-right-top")]:
        assert app.execute("view.cube_pick", x=x, y=y, width=400, height=300) == {"hit": True, "view": expected}
        assert app.execute("view.cube_hover", x=x, y=y, width=400, height=300)["view"] == expected
    assert app.execute("view.cube_hover", x=0., y=0., width=400, height=300) == {"hit": False, "changed": True}
    assert app.execute("view.cube_hover") == {"hit": False, "changed": False}
    app.execute("view.cube_hover", **PIXEL)
    app.execute("view.hud", triad=False)
    assert app.execute("view.cube_hover", **PIXEL) == {"hit": False, "changed": False}
    app.execute("view.hud", triad=True)
    app.execute("view.cube_hover", **PIXEL)
    app.execute("project.new")
    assert app.execute("view.cube_hover") == {"hit": False, "changed": False}
