"""RND-T01-38~40: 부드러운 표준 뷰 전환, 최단 회전, 중단과 재시작."""
import numpy as np
import pytest

from nasa95 import Nasa95Error
from conftest import history_len
from test_VIEW_render import box, needs_geometry


def orientation(camera):
    back = np.subtract(camera["eye"], camera["target"])
    back /= np.linalg.norm(back)
    right = np.cross(-back, camera["up"])
    right /= np.linalg.norm(right)
    return np.column_stack((right, np.cross(back, right), back))


def angle(a, b):
    return np.arccos(np.clip((np.trace(a.T @ b) - 1) / 2, -1, 1))


@needs_geometry
@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
@pytest.mark.parametrize("projection", ["orthographic", "perspective"])
@pytest.mark.parametrize("name", ["front", "back", "left", "right", "top", "bottom", "iso"])
def test_RND_T01_38_smooth_transition_matches_standard(app, projection, name):
    box(app)
    start = app.execute("view.camera_set", eye=[42., -58., 49.], target=[2., 6., 12.], up=[0.2, 0.3, 1.],
                        height=80., projection=projection, pivot=[1., 2., 3.])
    goal = app.execute("view.standard", name=name)
    app.execute("view.camera_set", **start)
    before, history = app.digest(), history_len(app)
    began = app.execute("view.transition_begin", name=name)
    assert began == {"active": True, "camera": start, "target": goal}
    assert app.execute("view.transition_step", progress=0.)["camera"] == start
    first, last = orientation(start), orientation(goal)
    total_angle = angle(first, last)
    for t in [0.01, 0.05, 0.25, 0.5, 0.75, 0.95, 0.99]:
        frame = app.execute("view.transition_step", progress=t)
        camera = frame["camera"]
        assert frame["active"]
        basis = orientation(camera)
        assert angle(first, basis) == pytest.approx(total_angle * (6*t**5 - 15*t**4 + 10*t**3), abs=1e-6)
        assert basis.T @ basis == pytest.approx(np.eye(3), abs=1e-12)
        assert np.linalg.norm(camera["up"]) == pytest.approx(1.)
        assert min(start["height"], goal["height"]) <= camera["height"] <= max(start["height"], goal["height"])
        assert camera["projection"] == projection
    assert app.execute("view.transition_step", progress=1.) == {"active": False, "camera": goal}
    assert app.execute("view.transition_step", progress=0.5) == {"active": False, "camera": goal}
    assert not app.execute("view.transition_begin", name=name)["active"]
    assert app.digest() == before and history_len(app) == history


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
@pytest.mark.parametrize("source,target", [("front", "back"), ("left", "right"), ("top", "bottom")])
def test_RND_T01_39_opposite_view_never_collapses_camera(app, source, target):
    start = app.execute("view.standard", name=source)
    distance = np.linalg.norm(np.subtract(start["eye"], start["target"]))
    previous = orientation(start)
    end = app.execute("view.transition_begin", name=target)["target"]
    angles = []
    for t in np.linspace(0., 1., 61):
        camera = app.execute("view.transition_step", progress=float(t))["camera"]
        current = orientation(camera)
        assert np.isfinite(current).all()
        assert np.linalg.norm(np.subtract(camera["eye"], camera["target"])) == pytest.approx(distance)
        angles.append(angle(previous, current))
        previous = current
    assert max(angles) < np.radians(6.)
    assert sum(angles) == pytest.approx(np.pi, abs=1e-6)
    assert angles[1] < angles[30] / 100 and angles[-1] < angles[30] / 100
    assert app.execute("view.camera_get") == end


@pytest.mark.feature("RND-09")
@pytest.mark.feature("RND-10")
def test_RND_T01_40_cancel_retarget_and_external_change(app):
    app.execute("view.standard", name="iso")
    app.execute("view.transition_begin", name="front")
    middle = app.execute("view.transition_step", progress=0.4)["camera"]
    restarted = app.execute("view.transition_begin", name="top")
    assert restarted["camera"] == middle
    assert app.execute("view.transition_cancel") == {"active": False, "camera": middle}
    assert app.execute("view.transition_step", progress=1.)["camera"] == middle
    for change in [lambda: app.view.orbit(50., 40.), lambda: app.execute("view.standard", name="right"),
                   lambda: app.execute("project.new")]:
        app.execute("view.transition_begin", name="bottom")
        change()
        current = app.execute("view.camera_get")
        assert app.execute("view.transition_step", progress=1.) == {"active": False, "camera": current}
    app.execute("view.transition_begin", name="front")
    before = app.execute("view.camera_get")
    for t in [-0.1, 1.1]:
        with pytest.raises(Nasa95Error):
            app.execute("view.transition_step", progress=t)
        assert app.execute("view.camera_get") == before
    assert not app.execute("view.transition_step", progress=1.)["active"]
