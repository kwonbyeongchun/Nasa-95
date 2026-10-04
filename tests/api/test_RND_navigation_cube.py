"""사용자 참고 이미지의 방향 큐브: 고정 화면 표시·회전 동기화·면 조회."""
import numpy as np
import pytest

from conftest import history_len
from test_VIEW_render import _available

pytestmark = pytest.mark.skipif(not _available(), reason="Vulkan 렌더러 필요")


@pytest.mark.feature("RND-10")
@pytest.mark.feature("RND-37")
@pytest.mark.parametrize("name", ["front", "back", "left", "right", "top", "bottom"])
def test_RND_T01_34_cube_standard_face_and_query_invariants(app, name):
    app.execute("view.hud", navigation_cube=True, legend=False)
    camera = app.execute("view.standard", name=name)
    before, history = app.digest(), history_len(app)
    assert app.execute("view.cube_pick", x=88., y=220., width=400, height=300) == {"hit": True, "view": name}
    assert app.execute("view.cube_pick", x=88., y=520.) == {"hit": True, "view": name}
    assert app.execute("view.cube_pick", x=200., y=100., width=400, height=300) == {"hit": False}
    assert app.execute("view.camera_get") == camera
    assert app.digest() == before and history_len(app) == history


@pytest.mark.feature("RND-18")
@pytest.mark.feature("RND-36")
@pytest.mark.feature("RND-37")
def test_RND_T01_35_cube_fixed_screen_size_and_visibility(app):
    app.execute("view.hud", navigation_cube=True, legend=False)
    camera = app.execute("view.standard", name="iso")
    images = []
    for aa in ["none", "ssaa2"]:
        app.execute("view.quality", antialiasing=aa)
        small, ids = app.view.render(400, 300)
        large, _ = app.view.render(620, 420)
        assert not ids.any()
        # 해상도에 따라 clip 좌표의 float 반올림이 공유 모서리의 색을 1~2픽셀 바꿀 수 있다.
        # 고정 위치·크기는 배경을 제외한 전체 화면 영역이 정확히 같은지로 검증한다.
        assert np.array_equal(np.any(small[-240:, :240, :3] != 255, axis=2),
                              np.any(large[-240:, :240, :3] != 255, axis=2))
        mask = np.any(small[:, :, :3] != 255, axis=2)
        y, x = np.nonzero(mask)
        assert len(x) > 2000 and x.min() > 0 and x.max() < 240 and y.min() > 60 and y.max() < 300
        images.append((x.min(), x.max(), y.min(), y.max()))
        cube = small[160:260, 40:140, :3]
        gray = np.max(cube, axis=2) - np.min(cube, axis=2) < 8
        assert ((cube[:, :, 0] > 180) & (cube[:, :, 0] < 250) & gray).sum() > 800  # 밝은 회색 면
        assert ((cube[:, :, 0] < 160) & gray).sum() > 80  # 면 이름과 테두리
        assert ((small[:, :, 0] > small[:, :, 1] + 20) & (small[:, :, 1] < 170)).any()  # 색상 축
    assert np.max(np.abs(np.subtract(images[0], images[1]))) <= 2
    reference = app.view.render(400, 300)[0]
    shift = np.array([12., -7., 19.])
    app.execute("view.camera_set", eye=(np.array(camera["eye"]) + shift).tolist(),
                target=(np.array(camera["target"]) + shift).tolist(), height=0.01, projection="perspective", fov=80.)
    assert np.array_equal(app.view.render(400, 300)[0], reference)
    app.execute("view.hud", triad=False)
    assert np.all(app.view.render(400, 300)[0] == 255)
    assert app.execute("view.cube_pick", x=88., y=220., width=400, height=300) == {"hit": False}
    app.execute("view.hud", triad=True, navigation_cube=False)
    assert app.execute("view.cube_pick", x=88., y=220., width=400, height=300) == {"hit": False}


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-37")
def test_RND_T01_36_cube_visible_faces_follow_camera(app):
    app.execute("view.hud", navigation_cube=True, legend=False)
    app.execute("view.camera_set", eye=[4., -3., 2.], target=[0., 0., 0.], up=[0., 0., 1.])
    faces = [("right", [1., 0., 0.]), ("left", [-1., 0., 0.]), ("front", [0., -1., 0.]),
             ("back", [0., 1., 0.]), ("top", [0., 0., 1.]), ("bottom", [0., 0., -1.])]
    for dx, dy in [(0., 0.), (100., 90.), (-40., 160.), (30., -25.)]:
        camera = app.view.orbit(dx, dy, height=300)
        back = np.subtract(camera["eye"], camera["target"])
        back /= np.linalg.norm(back)
        right = np.cross(-back, camera["up"])
        right /= np.linalg.norm(right)
        up = np.cross(back, right)
        image, ids = app.view.render(400, 300)
        assert not ids.any()
        for name, normal in faces:
            if np.dot(normal, back) <= 0.2:
                continue
            # 큐브 단위 면 중심을 독립 카메라 기저로 화면에 투영한다.
            x, y = 88 + 24 * np.dot(right, normal), 220 - 24 * np.dot(up, normal)
            assert app.execute("view.cube_pick", x=float(x), y=float(y), width=400, height=300) == {"hit": True, "view": name}
            assert np.all(image[round(y), round(x), :3] < 252)
