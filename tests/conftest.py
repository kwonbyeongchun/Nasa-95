"""모든 테스트 공통.

- 프로그램 설정 파일을 임시 폴더로 돌려 사용자의 실제 설정(APPDATA 아래 open-fep)을 건드리지 않는다.
- 보고서용 계측(환경 변수 OFEP_REPORT_DIR 가 있을 때): 테스트마다 기능 ID 마커·결과를 `tests.json` 에,
  App.execute 호출을 명령별 성공·실패 수로 `api-calls.json` 에 남긴다(.agent/tools/gen_test_report.py 가 읽는다).
"""
import json
import os
import sys
import time
from pathlib import Path

import pytest

_REPORT_DIR = os.environ.get("OFEP_REPORT_DIR")
_tests: dict[str, dict] = {}
_calls: dict[str, dict] = {}


@pytest.fixture(autouse=True)
def _isolated_settings(tmp_path, monkeypatch):
    monkeypatch.setenv("OFEP_SETTINGS", str(tmp_path / "settings.json"))


def pytest_configure(config):
    if not _REPORT_DIR:
        return
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
    from openfep import api

    original = api.App.execute

    def counted(self, command, params=None, /, **kw):
        entry = _calls.setdefault(command, {"ok": 0, "error": 0, "codes": {}})
        try:
            r = original(self, command, params, **kw)
        except api.OfepError as e:
            entry["error"] += 1
            entry["codes"][e.code] = entry["codes"].get(e.code, 0) + 1
            raise
        entry["ok"] += 1
        return r

    api.App.execute = counted


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    if not _REPORT_DIR:
        return
    rep = outcome.get_result()
    entry = _tests.setdefault(item.nodeid, {"features": sorted({m.args[0] for m in item.iter_markers("feature")}),
                                             "name": item.name, "outcome": "passed", "duration": 0.0, "message": ""})
    entry["duration"] += getattr(rep, "duration", 0.0)
    if rep.when == "call" or (rep.when == "setup" and rep.outcome != "passed"):
        if rep.skipped:
            entry["outcome"] = "skipped"
            entry["message"] = str(rep.longrepr[-1] if isinstance(rep.longrepr, tuple) else rep.longrepr)[:2000]
        elif rep.failed:
            entry["outcome"] = "failed" if rep.when == "call" else "error"
            entry["message"] = str(rep.longrepr)[:4000]


def pytest_sessionfinish(session, exitstatus):
    if not _REPORT_DIR:
        return
    d = Path(_REPORT_DIR)
    d.mkdir(parents=True, exist_ok=True)
    (d / "tests.json").write_text(json.dumps({"finished": time.strftime("%Y-%m-%d %H:%M:%S"), "exit_status": int(exitstatus),
                                              "tests": _tests}, ensure_ascii=False, indent=1), encoding="utf-8")
    (d / "api-calls.json").write_text(json.dumps(_calls, ensure_ascii=False, indent=1, sort_keys=True), encoding="utf-8")
