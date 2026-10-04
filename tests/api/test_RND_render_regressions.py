"""RND-T01-24~25, RND-T04-15~16: 에지 가시성과 품질 조회 회귀 검사."""
import itertools

import numpy as np
import pytest

from conftest import history_len, total
from test_VIEW_render import _available, box, needs_geometry

pytestmark = pytest.mark.skipif(not _available(), reason="Vulkan 렌더러 없음")


@needs_geometry
@pytest.mark.feature("RND-13")
@pytest.mark.feature("RND-15")
def test_RND_T01_24_geometry_edge_mode(app):
    box(app)
    app.execute("view.hud", triad=False, legend=False)
    app.execute("view.standard", name="iso")
    app.execute("view.display_mode", mode="shaded")
    shaded, ids = app.view.render(800, 600)
    app.execute("view.display_mode", mode="shaded_edges")
    edged, edged_ids = app.view.render(800, 600)
    assert not np.any(np.all(shaded[:, :, :3] == 30, axis=2))
    assert np.count_nonzero(np.all(edged[:, :, :3] == 30, axis=2)) > 1000
    assert np.array_equal(ids, edged_ids)


@needs_geometry
@pytest.mark.feature("RND-13")
@pytest.mark.feature("RND-15")
@pytest.mark.parametrize("scale,eye_direction,width,height", [
    (1., (1., -1., 1.), 800, 600),
    (.001, (-1., -1.3, 1.), 500, 400),
    (1000., (2., 1., -1.), 1000, 700),
])
def test_RND_T01_25_visible_and_hidden_edges(app, scale, eye_direction, width, height):
    size = np.array([10., 20., 30.]) * scale
    box(app, size=size)
    app.execute("view.hud", triad=False, legend=False)
    direction = np.array(eye_direction)
    direction /= np.linalg.norm(direction)
    target = size / 2
    camera_height = np.linalg.norm(size) * 1.1
    app.execute("view.camera_set", projection="orthographic", eye=(target + direction * camera_height * 2).tolist(),
                target=target.tolist(), up=[0., 0., 1.], height=camera_height)
    image, _ = app.view.render(width, height)
    dark = np.all(image[:, :, :3] == 30, axis=2)
    forward = -direction
    right = np.cross(forward, [0., 0., 1.])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    for axis in range(3):
        other = [i for i in range(3) if i != axis]
        for ends in itertools.product([0, 1], repeat=2):
            visible = any((2 * end - 1) * direction[i] > 0 for i, end in zip(other, ends))
            a = np.zeros(3)
            for i, end in zip(other, ends):
                a[i] = end * size[i]
            b = a.copy()
            b[axis] = size[axis]
            hits = 0
            for t in np.linspace(.15, .85, 81):
                p = a + t * (b - a) - target
                x = int(np.floor(width / 2 + np.dot(p, right) * height / camera_height))
                y = int(np.floor(height / 2 - np.dot(p, up) * height / camera_height))
                hits += bool(dark[y-1:y+2, x-1:x+2].any())
            if visible:
                assert hits == 81, (a, b, hits)
            else:
                # 뒤쪽 모서리 자체는 없어야 한다. 투영된 앞 에지와 교차하는 소수 표본은 허용.
                assert hits < 9, (a, b, hits)


@pytest.mark.feature("RND-18")
@pytest.mark.feature("RND-42")
@pytest.mark.feature("RND-44")
def test_RND_T04_15_quality_get_preserves_state(app):
    before, hist = total(app), history_len(app)
    expected = app.execute("view.quality", antialiasing="ssaa2", transparency="sorted",
                           simplify_during_interaction=True, gpu_memory_limit=123456)
    for _ in range(3):
        assert app.execute("view.quality_get") == expected
    assert total(app) == before and history_len(app) == hist
    assert app.execute("view.quality", antialiasing="none")["transparency"] == "sorted"
    assert app.execute("view.quality")["antialiasing"] == "none"  # 기존 초기화 계약
    assert "gpu_memory_limit" not in app.execute("view.quality_get")


@needs_geometry
@pytest.mark.feature("RND-18")
@pytest.mark.feature("RND-37")
def test_RND_T04_16_ssaa_keeps_hud_size(app):
    # 빈 모델의 좌표축 크기는 AA 여부에 따라 절반으로 줄어들면 안 된다.
    app.execute("view.hud", triad=True, legend=False)
    plain, _ = app.view.render(800, 600)
    app.execute("view.quality", antialiasing="ssaa2")
    aa, _ = app.view.render(800, 600)
    def bounds(image):
        y, x = np.nonzero(np.any(image[:, :, :3] < 240, axis=2))
        return np.array([x.min(), x.max(), y.min(), y.max()])
    assert np.max(np.abs(bounds(plain) - bounds(aa))) <= 2
