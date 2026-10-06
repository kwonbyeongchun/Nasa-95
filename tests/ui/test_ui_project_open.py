"""프로젝트 열기 UI: 이전 파일의 메시 파트 보완이 화면 갱신 전에 완료된다."""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))
pytest.importorskip("PySide6")
from PySide6.QtWidgets import QApplication, QFileDialog
from nasa95 import App, Nasa95Error
from nasa95.ui import MainWindow


@pytest.mark.feature("CMN-09")
@pytest.mark.parametrize("file_format", ["NASA-95", "open-fep"])
@pytest.mark.parametrize("part", [0, 99, 7])
def test_SYS_08_07_open_legacy_mesh_from_dialog(tmp_path, monkeypatch, file_format, part):
    qt = QApplication.instance() or QApplication([])
    # 0: 파트 미지정, 99: 사라진 파트, 7: 기존 MESH 재사용.
    objects = ([dict(id=7, kind="mesh_part", name="MESH", parent=0, order=0,
                     props={}, suppressed=False)] if part == 7 else [])
    data = dict(format=file_format, version=1, next_id=8, objects=objects,
                mesh=dict(node_ids=[1, 2], node_xyz=[0, 0, 0, 100, 0, 0], elem_ids=[1],
                          elem_shape=[1], elem_part=[0 if part == 7 else part], elem_type=["B31"],
                          elem_offset=[0, 2], conn=[1, 2]))
    path = tmp_path / "이전 프로젝트.nasa95"
    original = json.dumps(data).encode()
    path.write_bytes(original)
    monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *a, **k: (str(path), ""))
    app = App()
    w = MainWindow(app)
    notifications = []
    token = app.subscribe(lambda event: notifications.append(app.execute("project.info"))
                          if event.get("command") == "project.open" else None)
    try:
        w._open()
        qt.processEvents()
        parts = app.execute("mesh_part.list")
        assert len(parts) == 1 and parts[0]["name"] == "MESH"
        if part == 7:
            assert parts[0]["id"] == 7
        element = app.execute("mesh.elements", ids=[1])[0]
        assert element["part"] == parts[0]["id"]
        assert element["nodes"] == [1, 2] and element["type"] == "B31"
        assert len(notifications) == 1 and notifications[0]["counts"]["mesh_part"] == 1
        assert notifications[0]["nodes"] == 2 and notifications[0]["elements"] == 1
        assert not notifications[0]["modified"]
        assert not app.execute("app.history")["undo"]
        group = next(w.tree.topLevelItem(i) for i in range(w.tree.topLevelItemCount())
                     if w.tree.topLevelItem(i).text(1) == "mesh_part")
        assert group.childCount() == 1 and "MESH" in group.child(0).text(0)
        assert path.read_bytes() == original
        saved = tmp_path / "다시 저장.nasa95"
        app.execute("project.save", path=str(saved))
        before = app.digest()
        monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *a, **k: (str(saved), ""))
        w._open()
        qt.processEvents()
        assert app.digest() == before
    finally:
        app.unsubscribe(token)
        w.close()
        qt.processEvents()


@pytest.mark.feature("CMN-09")
def test_SYS_08_02_open_invalid_file_preserves_current_project(tmp_path, monkeypatch):
    qt = QApplication.instance() or QApplication([])
    app = App()
    app.model.materials.create(name="EXISTING_MATERIAL")
    before, history = app.digest(), app.execute("app.history")
    path = tmp_path / "invalid.nasa95"
    path.write_text('{"format": "NASA-95", "objects": [', encoding="utf-8")
    monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *a, **k: (str(path), ""))
    w = MainWindow(app)
    try:
        with pytest.raises(Nasa95Error):
            w._open()
        assert app.digest() == before
        assert app.execute("app.history") == history
    finally:
        w.close()
        qt.processEvents()
