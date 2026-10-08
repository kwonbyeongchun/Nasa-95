"""최근 파일(사용자 요청 2026-10-08): 열기·저장한 프로젝트가 최근 순으로 쌓이고, 메뉴에서 바로 연다. 없어진 파일은 비활성."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))
pytest.importorskip("PySide6")
from PySide6.QtWidgets import QApplication, QFileDialog
from nasa95 import App
from nasa95.ui import MainWindow


@pytest.mark.feature("CMN-09")
def test_UI_recent_files(tmp_path, monkeypatch):
    monkeypatch.setenv("NASA95_UI_SETTINGS", str(tmp_path / "ui.ini"))
    qt = QApplication.instance() or QApplication([])
    app = App()
    w = MainWindow(app)
    try:
        w._fill_recent_menu()
        assert [a.text() for a in w._recent_menu.actions()] == ["(최근 파일 없음)"]
        paths = []
        for name in ("a", "b", "c"):  # 저장 → 최근 목록
            p = tmp_path / f"{name}.nasa95"
            monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *a, p=p, **k: (str(p), ""))
            w._save(True)
            paths.append(str(p.resolve()))
        assert w._recent_list() == paths[::-1]
        w._open_path(paths[0])  # 다시 열면 맨 앞으로, 중복 없음
        assert w._recent_list() == [paths[0], paths[2], paths[1]]
        assert w.windowTitle().startswith("a.nasa95")
        os.remove(paths[1])
        w._fill_recent_menu()
        acts = [a for a in w._recent_menu.actions() if not a.isSeparator()]
        assert [a.isEnabled() for a in acts[:3]] == [True, True, False] and "(없음)" in acts[2].text()
        acts[1].trigger()  # 메뉴에서 바로 연다
        qt.processEvents()
        assert app.execute("project.info")["path"].replace("\\", "/").endswith("c.nasa95")
        for i in range(12):  # 최대 10개
            w._remember_recent(str(tmp_path / f"x{i}.nasa95"))
        assert len(w._recent_list()) == 10
        w._clear_recent()
        assert w._recent_list() == []
    finally:
        w.close()
