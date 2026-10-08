"""Solid mesh remains visible when the UI switches to wireframe."""
import numpy as np
import pytest
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QApplication
from nasa95 import App
from nasa95.ui import MainWindow


@pytest.mark.parametrize("theme", ["light", "dark"])
@pytest.mark.parametrize("edges", [True, False])
@pytest.mark.parametrize("label", ["와이어프레임", "음영+경계선"])
def test_solid_wireframe_visible_from_view_menu(theme, edges, label, tmp_path):
    qt = QApplication.instance() or QApplication([])
    app = App()
    window = MainWindow(app)
    try:
        window.theme.set_mode(theme)
        bg = [30, 30, 30] if theme == "dark" else [255, 255, 255]
        nodes = app.execute("mesh.nodes_create", coords=[
            [0,0,0], [1,0,0], [1,1,0], [0,1,0],
            [0,0,1], [1,0,1], [1,1,1], [0,1,1]])["ids"]
        app.execute("mesh.elements_create", shape="hex8", connectivity=[nodes])
        app.execute("view.standard", name="iso")
        app.execute("view.overlay", background=bg, triad=False)
        app.execute("view.hud", triad=False, navigation_cube=False, legend=False)
        app.execute("view.mesh_options", edges=edges)
        next(a for a in window.findChildren(QAction) if a.text() == label).trigger()
        rgba, _ = app.view.render(640, 480)
        contrast = np.max(np.abs(rgba[:,:,:3].astype(int) - np.array(bg)), axis=2)
        assert np.count_nonzero(contrast > 40) > 1000, "Wireframe must be visible against the viewport background"
        assert app.execute("view.mesh_options_get")["edges"] is True
        # Screenshot exports use white, independently of the viewport theme.
        path = tmp_path / "wireframe.png"
        info = app.execute("view.screenshot", path=str(path), width=640, height=480)
        assert info["lines"] == 12
        assert info["triangles"] == (0 if label == "와이어프레임" else 12)
        from PIL import Image
        rgb = np.asarray(Image.open(path))[:,:,:3].astype(int)
        assert np.count_nonzero(np.max(abs(rgb-255), axis=2) > 40) > 1000
    finally:
        window.close()
        qt.processEvents()
