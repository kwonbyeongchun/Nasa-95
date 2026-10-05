"""시스템·공통: 시작(SYS-01), 이력(SYS-02), 되돌릴 수 없는 명령(SYS-03), 묶음 실행(SYS-04),
저널(SYS-06), 저장·열기·다이제스트(SYS-08), 핸들(SYS-09), 참조 무결성(SYS-11), 좌표계(SYS-13),
워크 트리 조회·상태·탐색(SYS-19~21) 중 현재 구현된 범위."""
import json

import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total


def prepare_static_case(app: App) -> None:
    """작은 정적 해석 준비 과정. 저널·저장 테스트가 함께 쓴다."""
    m = app.model.materials.create(name="steel")
    m.set_elastic(data=[[210000.0, 0.3]])
    m.set_density(data=[[7.85e-9]])
    app.model.properties.create_solid(name="solid1", material=m.id)
    case = app.model.cases.create(name="static")
    step = case.steps.create_static(name="load-step")
    step.loads.create_force(target={"type": "nodes", "ids": [8]}, components=[0.0, 0.0, -100.0])
    step.bcs.create_displacement(target={"type": "nodes", "ids": [1]}, dofs=[1, 2, 3])


# ---------------------------------------------------------------- SYS-01
@pytest.mark.feature("API-15")
def test_SYS_01_01_headless_start(app):
    assert app.execute("project.info")["objects"] == 0


@pytest.mark.feature("API-25")
def test_SYS_01_02_version(app):
    v = app.execute("app.version")
    assert v["version"] and v["api_version"]


@pytest.mark.feature("API-10")
def test_SYS_01_03_04_object_style_access(app):
    prepare_static_case(app)
    step = app.model.cases["static"].steps["load-step"]
    assert len(step.loads) == 1 and len(step.bcs) == 1
    tree = app.execute("project.tree", kind="case")
    assert tree[0]["items"][0]["children"][0]["id"] == step.id
    with pytest.raises(Nasa95Error) as e:
        app.model.cases["없는 이름"]
    assert e.value.code == "not_found"


@pytest.mark.feature("API-15")
def test_SYS_01_05_headless_flow(app):
    prepare_static_case(app)
    assert app.execute("project.validate") == []


# ---------------------------------------------------------------- SYS-02
@pytest.mark.feature("CMN-16")
def test_SYS_02_01_02_history_and_goto(app):
    digests = [total(app)]
    for i in range(5):
        app.execute("material.create", name=f"m{i}")
        digests.append(total(app))
    hist = app.execute("app.history")["undo"]
    assert [h["name"] for h in hist] == ["material.create"] * 5
    app.execute("app.history_goto", serial=hist[1]["serial"])
    assert total(app) == digests[2]
    app.execute("app.history_goto", serial=hist[4]["serial"])  # 앞으로도 간다
    assert total(app) == digests[5]
    app.execute("app.history_goto", serial=0)
    assert total(app) == digests[0]


@pytest.mark.feature("CMN-17")
def test_SYS_02_03_history_limit(app):
    app.execute("app.history_limit", limit=5)
    for i in range(6):
        app.execute("material.create", name=f"m{i}")
    undone = 0
    while app.undo()["done"]:
        undone += 1
    assert undone == 5
    assert len(app.model.materials) == 1  # 가장 오래된 명령은 되돌릴 수 없다


# ---------------------------------------------------------------- SYS-03
@pytest.mark.feature("CMN-18")
def test_SYS_03_undoable_flag(app):
    cmds = {c["name"]: c for c in app.commands()}
    assert cmds["material.create"]["undoable"] is True
    for name in ("project.save", "project.open", "journal.replay"):
        assert cmds[name]["undoable"] is False


# ---------------------------------------------------------------- SYS-04
@pytest.mark.feature("API-04")
def test_SYS_04_01_transaction_commit(app):
    before, hist = total(app), history_len(app)
    with app.transaction("세 재료"):
        for i in range(3):
            app.execute("material.create", name=f"m{i}")
    assert history_len(app) == hist + 1
    assert app.execute("app.history")["undo"][-1]["name"] == "세 재료"
    app.undo()
    assert total(app) == before


@pytest.mark.feature("API-04")
def test_SYS_04_02_transaction_rollback(app):
    before, hist = total(app), history_len(app)
    app.execute("app.transaction_begin", name="취소할 묶음")
    for i in range(3):
        app.execute("material.create", name=f"m{i}")
    app.execute("app.transaction_rollback")
    assert total(app) == before and history_len(app) == hist


@pytest.mark.feature("API-04")
def test_SYS_04_03_transaction_fails_midway(app):
    before, hist = total(app), history_len(app)
    with pytest.raises(Nasa95Error):
        with app.transaction("실패하는 묶음"):
            app.execute("material.create", name="ok")
            app.execute("material.create", name="ok")  # 이름 충돌로 실패
    assert total(app) == before and history_len(app) == hist
    assert app.execute("app.history")["in_transaction"] is False


@pytest.mark.feature("WT-17")
def test_SYS_04_04_05_batch_actions(app):
    m1 = app.model.materials.create(name="a")
    m2 = app.model.materials.create(name="b")
    props = [app.model.properties.create_solid(material=m1.id) for _ in range(3)]
    before = total(app)
    with app.transaction("재료 일괄 변경"):
        for p in props:
            p.update(material=m2.id)
    assert all(p.props["material"] == m2.id for p in props)
    app.undo()
    assert total(app) == before
    with app.transaction("일괄 삭제"):
        for p in props:
            p.delete()
    assert len(app.model.properties) == 0
    app.undo()
    assert total(app) == before


@pytest.mark.feature("API-04")
def test_SYS_04_nested_transaction_rejected(app):
    app.execute("app.transaction_begin")
    with pytest.raises(Nasa95Error) as e:
        app.execute("app.transaction_begin")
    assert e.value.code == "invalid_state"
    app.execute("app.transaction_rollback")


# ---------------------------------------------------------------- SYS-06
@pytest.mark.feature("API-08")
def test_SYS_06_01_journal_replay(app, tmp_path):
    path = str(tmp_path / "run.journal")
    app.execute("journal.start", path=path)
    prepare_static_case(app)
    app.undo()  # Undo·Redo 도 기록되어야 같은 ID 로 재생된다
    app.redo()
    app.execute("journal.stop")
    other = App()
    assert other.execute("journal.replay", path=path)["executed"] > 0
    assert total(other) == total(app)


@pytest.mark.feature("API-08")
def test_SYS_06_02_failed_commands_not_journaled(app, tmp_path):
    path = str(tmp_path / "run.journal")
    app.execute("journal.start", path=path)
    app.execute("material.create", name="a")
    with pytest.raises(Nasa95Error):
        app.execute("material.create", name="a")
    app.execute("material.list")  # 조회는 기록하지 않는다
    app.execute("journal.stop")
    lines = [json.loads(s) for s in open(path, encoding="utf-8").read().splitlines()]
    assert [l["command"] for l in lines] == ["material.create"]
    other = App()
    other.execute("journal.replay", path=path)
    assert total(other) == total(app)


# ---------------------------------------------------------------- SYS-08
@pytest.mark.feature("CMN-09")
def test_SYS_08_01_save_and_open(app, tmp_path):
    prepare_static_case(app)
    path = str(tmp_path / "model.nasa95")
    app.execute("project.save_as", path=path)
    before = total(app)
    other = App()
    other.execute("project.open", path=path)
    assert total(other) == before
    # 연 뒤에 만든 객체의 ID 가 기존 것과 겹치지 않는다
    new_id = other.execute("material.create", name="extra")["id"]
    assert new_id not in [o["id"] for o in app.execute("project.search")]


@pytest.mark.feature("CMN-09")
def test_SYS_08_02_open_damaged_file(app, tmp_path):
    prepare_static_case(app)
    before = total(app)
    bad = tmp_path / "bad.nasa95"
    bad.write_text('{"format": "NASA-95", "objects": [', encoding="utf-8")
    with pytest.raises(Nasa95Error) as e:
        app.execute("project.open", path=str(bad))
    assert e.value.code == "invalid_file"
    assert total(app) == before  # 실패하면 현재 모델을 그대로 둔다
    with pytest.raises(Nasa95Error) as e:
        app.execute("project.open", path=str(tmp_path / "없는 파일.nasa95"))
    assert e.value.code == "io_error"


@pytest.mark.feature("CMN-20")
def test_SYS_08_03_06_modified_flag(app, tmp_path):
    assert app.execute("project.info")["modified"] is False
    app.execute("material.create", name="a")
    assert app.execute("project.info")["modified"] is True
    app.execute("project.save_as", path=str(tmp_path / "a.nasa95"))
    assert app.execute("project.info")["modified"] is False
    app.execute("material.create", name="b")
    assert app.execute("project.info")["modified"] is True
    app.undo()  # 저장 시점으로 돌아오면 "변경 없음"
    assert app.execute("project.info")["modified"] is False
    app.undo()
    assert app.execute("project.info")["modified"] is True
    app.execute("project.save_as", path=str(tmp_path / "b.nasa95"))
    info = app.execute("project.info")
    assert info["modified"] is False and info["path"].endswith("b.nasa95")


@pytest.mark.feature("API-39")
def test_SYS_08_04_digest_by_area(app):
    prepare_static_case(app)
    before = app.digest()
    app.model.materials["steel"].set_elastic(data=[[200000.0, 0.3]])
    after = app.digest()
    assert after["total"] != before["total"]
    assert after["areas"]["material"] != before["areas"]["material"]
    for area in before["areas"]:
        if area != "material":
            assert after["areas"][area] == before["areas"][area], area


# ---------------------------------------------------------------- SYS-09
@pytest.mark.feature("API-11")
def test_SYS_09_01_02_handle_of_deleted_object(app):
    m = app.model.materials.create(name="a")
    m.delete()
    with pytest.raises(Nasa95Error) as e:
        m.get()
    assert e.value.code == "not_found"
    app.undo()
    assert m.get()["name"] == "a"  # 삭제를 되돌리면 같은 핸들이 다시 유효하다


# ---------------------------------------------------------------- SYS-11
@pytest.mark.feature("CMN-07")
def test_SYS_11_04_05_references(app):
    m = app.model.materials.create(name="a")
    unused = app.model.materials.create(name="b")
    f = app.model.functions.create_amplitude(name="amp", points=[[0, 0], [1, 1]])
    p = app.model.properties.create_solid(material=m.id)
    case = app.model.cases.create()
    load = case.steps.create_static().loads.create_force(amplitude=f.id)
    assert [r["id"] for r in m.references()] == [p.id]
    assert [r["id"] for r in f.references()] == [load.id]
    assert unused.references() == []
    for h in (m, f):
        with pytest.raises(Nasa95Error) as e:
            h.delete()
        assert e.value.code == "referenced"
    # 참조하는 쪽까지 함께 지우는 삭제(케이스 삭제)는 막지 않는다
    case.delete()
    f.delete()


# ---------------------------------------------------------------- SYS-13
@pytest.mark.feature("CMN-02")
def test_SYS_13_02_parallel_axes_rejected(app):
    with pytest.raises(Nasa95Error) as e:
        app.model.csys.create_cylindrical(origin=[0, 0, 0], axis1_point=[0, 0, 1], plane12_point=[0, 0, 3])
    assert e.value.code == "invalid_geometry"
    assert len(app.model.csys) == 0


# ---------------------------------------------------------------- SYS-19 ~ SYS-21 (현재 구현 범위)
@pytest.mark.feature("WT-01")
def test_SYS_19_01_11_tree(app):
    empty = app.execute("project.tree")
    assert all(b["count"] == 0 and b["items"] == [] for b in empty)
    prepare_static_case(app)
    tree = {b["kind"]: b for b in app.execute("project.tree")}
    assert tree["material"]["count"] == 1 and tree["case"]["count"] == 1
    assert "step" not in tree and "load" not in tree  # 스텝·하중은 케이스 아래에 나온다
    step = tree["case"]["items"][0]["children"][0]
    assert not step.get("children") and step["summary"]["loads"] == 1 and step["summary"]["bcs"] == 1  # 하중·구속은 셋 아래
    assert tree["load_set"]["count"] == 1 and tree["bc_set"]["count"] == 1


@pytest.mark.feature("WT-27")
@pytest.mark.feature("WT-28")
def test_SYS_20_01_02_09_10_status(app):
    m = app.model.materials.create(name="bad")
    m.set_elastic(data=[[210000.0, 0.6]])
    assert [i["code"] for i in m.validate()] == ["out_of_range"]
    p = app.model.properties.create_solid()  # 재료 없는 프로퍼티 = 미완성
    assert [i["severity"] for i in p.validate()] == ["incomplete"]
    tree = {b["kind"]: b for b in app.execute("project.tree")}
    assert tree["material"]["items"][0]["status"] == "error"
    assert tree["property"]["items"][0]["status"] == "incomplete"
    # 고치면 표시가 사라진다
    m.set_elastic(data=[[210000.0, 0.3]])
    p.update(material=m.id)
    assert app.execute("project.validate") == []
    tree = {b["kind"]: b for b in app.execute("project.tree")}
    assert "status" not in tree["material"]["items"][0] and "status" not in tree["property"]["items"][0]


@pytest.mark.feature("WT-34")
@pytest.mark.feature("WT-35")
def test_SYS_21_01_02_08_09_search(app):
    prepare_static_case(app)
    app.model.materials.create(name="steel-2").suppress()
    assert [o["name"] for o in app.execute("project.search", text="steel")] == ["steel", "steel-2"]
    assert app.execute("project.search", text="없는 이름") == []
    found = app.execute("project.search", kind="material", suppressed=True)
    assert [o["name"] for o in found] == ["steel-2"]


# ================================================================ 2026-10-02 점검 보고서에서 재현된 문제의 회귀 테스트
@pytest.mark.feature("CMN-09")
@pytest.mark.feature("API-08")
def test_SYS_08_05_korean_paths(app, tmp_path):
    """한글 경로의 프로젝트 저장·열기와 저널."""
    app.model.materials.create(name="A")
    path = tmp_path / "한글 폴더" / "한글프로젝트.nasa95"
    path.parent.mkdir()
    assert app.execute("project.save_as", path=str(path))["path"] == str(path) and path.exists()
    app.execute("project.new")
    assert app.execute("project.open", path=str(path))["path"] == str(path) and len(app.execute("material.list")) == 1
    journal = tmp_path / "한글 폴더" / "기록.py"
    app.execute("journal.start", path=str(journal))
    app.model.materials.create(name="B")
    app.execute("journal.stop")
    assert journal.exists() and "material.create" in journal.read_text(encoding="utf-8")
    app.execute("project.new")
    assert app.execute("journal.replay", path=str(journal))["executed"] >= 1


@pytest.mark.feature("API-06")
@pytest.mark.feature("CMN-19")
@pytest.mark.feature("CMN-20")
def test_SYS_11_04_listener_error_does_not_break_command(app):
    """통지 구독자가 예외를 내도 명령은 성공하고 변경·이력·수정 상태가 함께 간다. 오류는 project.info 에 남는다."""
    calls = []
    app.subscribe(lambda ev: calls.append(ev["command"]))
    app.subscribe(lambda ev: (_ for _ in ()).throw(RuntimeError("boom")))
    m = app.model.materials.create(name="M")
    assert m.id and len(app.execute("material.list")) == 1
    assert len(app.execute("app.history")["undo"]) == 1 and app.execute("project.info")["modified"] is True
    err = app.execute("project.info")["listener_error"]
    assert err["command"] == "material.create" and "boom" in err["message"]
    assert calls == ["material.create"]  # 다른 구독자는 그대로 받는다
    app.undo()
    assert app.execute("material.list") == [] and app.execute("project.info")["modified"] is False


def test_SYS_01_06_readme_example():
    """README 의 사용 예가 그대로 실행된다."""
    import pathlib
    text = (pathlib.Path(__file__).resolve().parents[2] / "README.md").read_text(encoding="utf-8")
    code = text.split("```python")[1].split("```")[0].replace('import sys; sys.path.insert(0, "python")', "")
    if not App().execute("app.version").get("geometry") or not App().execute("app.version").get("mesher"):
        pytest.skip("형상 커널 또는 자동 메셔 없이 빌드됨")
    exec(code, {"__name__": "readme"})


@pytest.mark.feature("CAS-41")
def test_SYS_20_program_settings(app, tmp_path, monkeypatch):
    """프로그램 설정: 파일에 저장되고 새 App 이 다시 읽는다. 모델·Undo 와 무관하다. threads 는 검사한다."""
    import json
    path = tmp_path / "settings.json"
    monkeypatch.setenv("NASA95_SETTINGS", str(path))
    a = App()
    assert a.execute("app.settings_get") == {"path": str(path), "values": {}}
    from conftest import history_len
    h = history_len(a)
    r = a.execute("app.settings_set", key="solver_executable", value="C:/ccx/ccx.exe")
    assert r["value"] == "C:/ccx/ccx.exe" and path.exists() and json.loads(path.read_text(encoding="utf-8")) == {"solver_executable": "C:/ccx/ccx.exe"}
    a.execute("app.settings_set", key="threads", value=4)
    a.execute("app.settings_set", key="ui", value={"theme": "dark", "recent": ["a.nasa95"]})
    assert a.execute("app.settings_get", key="ui")["value"] == {"theme": "dark", "recent": ["a.nasa95"]}
    assert history_len(a) == h and a.execute("project.info")["modified"] is False  # 설정은 모델이 아니다
    b = App()  # 새 프로세스처럼 다시 읽는다
    assert b.execute("app.settings_get")["values"] == {"solver_executable": "C:/ccx/ccx.exe", "threads": 4, "ui": {"theme": "dark", "recent": ["a.nasa95"]}}
    b.execute("app.settings_set", key="threads", value=None)
    assert "threads" not in App().execute("app.settings_get")["values"]
    with pytest.raises(Nasa95Error) as e:
        b.execute("app.settings_set", key="threads", value=0)
    assert e.value.code == "invalid_param" and "threads" not in App().execute("app.settings_get")["values"]
    path.write_text("{ broken", encoding="utf-8")
    assert App().execute("app.settings_get")["values"] == {}  # 깨진 파일은 빈 설정으로


# ================================================================ 확장(API-17, API-18)
@pytest.mark.feature("API-17")
@pytest.mark.feature("API-18")
def test_SYS_22_extensions(tmp_path, monkeypatch):
    """확장 폴더의 패키지를 찾아 켜고, 등록한 명령이 등록부·Python 에 나타나며, 끄면 사라지고 설정에 남는다. 깨진 확장은 오류로만 남는다."""
    import pathlib, shutil
    root = pathlib.Path(__file__).resolve().parents[2]
    ext_dir = tmp_path / "ext"
    shutil.copytree(root / "examples" / "extensions" / "hello_ext", ext_dir / "hello_ext")
    (ext_dir / "broken_ext").mkdir()
    (ext_dir / "broken_ext" / "__init__.py").write_text("def register(app):\n    app.register_command('broken.x', lambda p: {}, kind='Q', desc='x')\n    raise RuntimeError('boom')\n", encoding="utf-8")
    (ext_dir / "not_a_package").mkdir()
    monkeypatch.setenv("NASA95_EXTENSIONS", str(ext_dir))
    monkeypatch.setenv("NASA95_SETTINGS", str(tmp_path / "s.json"))
    app = App()
    exts = {e["name"]: e for e in app.execute("extension.list")}
    assert set(exts) == {"hello_ext", "broken_ext"}
    assert exts["hello_ext"]["enabled"] and exts["hello_ext"]["version"] == "0.1" and exts["hello_ext"]["commands"] == ["hello.count_materials"]
    assert not exts["broken_ext"]["enabled"] and "boom" in exts["broken_ext"]["error"]
    assert "broken.x" not in {c["name"] for c in app.commands()}  # 실패한 확장이 등록한 것은 되돌린다
    app.model.materials.create(name="A")
    r = app.execute("hello.count_materials", greeting="hi")
    assert r == {"count": 1, "names": ["A"], "greeting": "hi"}
    spec = [c for c in app.commands() if c["name"] == "hello.count_materials"][0]
    assert spec["kind"] == "Q" and spec["params"][0]["name"] == "greeting"
    # 끄기: 명령이 사라지고 설정에 남아 새 App 에서도 꺼져 있다
    off = app.execute("extension.disable", name="hello_ext")
    assert not off["enabled"] and "hello.count_materials" not in {c["name"] for c in app.commands()}
    assert app.execute("app.settings_get", key="disabled_extensions")["value"] == ["hello_ext"]
    again = App()
    assert not {e["name"]: e for e in again.execute("extension.list")}["hello_ext"]["enabled"]
    on = again.execute("extension.enable", name="hello_ext")
    assert on["enabled"] and again.execute("hello.count_materials")["count"] == 0
    assert again.execute("app.settings_get", key="disabled_extensions")["value"] is None
    with pytest.raises(Nasa95Error) as e:
        again.execute("extension.enable", name="nope")
    assert e.value.code == "not_found"
    # 새 확장을 나중에 넣으면 rescan 으로 찾는다
    shutil.copytree(ext_dir / "hello_ext", ext_dir / "hello_ext2")
    assert "hello_ext2" not in {e["name"] for e in again.execute("extension.list")}
    assert "hello_ext2" in {e["name"] for e in again.execute("extension.list", rescan=True)}


# ================================================================ 폴더(WT-07)
@pytest.mark.feature("WT-07")
@pytest.mark.feature("WT-19")
def test_SYS_23_folders(app):
    """폴더: 같은 상위 아래의 같은 종류 항목을 묶는다. 트리에 폴더 아래로 나오고, 지우면 항목은 밖으로 나오며, Undo 된다."""
    a, b, c = (app.model.materials.create(name=n) for n in "ABC")
    f = app.model.folders.create(name="BOLTS", kind="material")
    app.execute("material.move", id=a.id, folder=f.id)
    app.execute("material.move", id=b.id, folder=f.id)
    branch = [br for br in app.execute("project.tree") if br["kind"] == "material"][0]
    assert branch["count"] == 3 and [i["name"] for i in branch["items"]] == ["BOLTS", "C"]
    assert branch["items"][0]["kind"] == "folder" and branch["items"][0]["folder_kind"] == "material"
    assert [ch["name"] for ch in branch["items"][0]["children"]] == ["A", "B"]
    assert not any(br["kind"] == "folder" for br in app.execute("project.tree"))  # 폴더는 따로 가지가 없다
    assert app.execute("material.get", id=a.id)["props"]["folder"] == f.id
    # 빼기, 상위가 바뀌면 자동으로 빠짐, 다른 종류·상위의 폴더는 거부
    app.execute("material.move", id=b.id, out_of_folder=True)
    assert "folder" not in app.execute("material.get", id=b.id)["props"]
    case = app.model.cases.create()
    s = case.steps.create_static()
    own = app.execute("step.own_load_set", id=s.id)["id"]  # 하중은 셋 안에만 — 하중 폴더의 상위는 하중 셋
    lf = app.execute("folder.create", name="LOADS", kind="load", parent=own)["id"]
    with pytest.raises(Nasa95Error) as e:
        app.execute("material.move", id=c.id, folder=lf)
    assert e.value.code == "invalid_param"
    ld = s.loads.create_force(target={"type": "nodes", "ids": [1]}, components=[1.0, 0.0, 0.0])
    app.execute("load.move", id=ld.id, folder=lf)
    tree = next(i for i in app.execute("project.tree", kind="load_set")[0]["items"] if i["id"] == own)["children"]  # 하중 셋 → 폴더 → 하중
    assert tree[0]["kind"] == "folder" and tree[0]["children"][0]["id"] == ld.id
    s2 = case.steps.create_static()
    app.execute("load.move", id=ld.id, parent=s2.id)  # 스텝을 주면 그 스텝 전용 셋으로
    assert "folder" not in app.execute("load.get", id=ld.id)["props"]
    # 폴더 삭제: 항목은 남고 폴더에서 나온다. Undo 로 되돌아간다
    app.execute("folder.delete", id=f.id)
    assert "folder" not in app.execute("material.get", id=a.id)["props"] and len(app.execute("material.list")) == 3
    app.undo()
    assert app.execute("material.get", id=a.id)["props"]["folder"] == f.id
    assert [i["name"] for i in [br for br in app.execute("project.tree") if br["kind"] == "material"][0]["items"]] == ["BOLTS", "B", "C"]


# ================================================================ 미리보기(RND-39)·통지 폴링(API-06)·확장 데이터(API-23)
@pytest.mark.feature("RND-39")
def test_SYS_24_command_preview(app):
    """변경 명령을 미리보면 결과와 변경 요약을 주고 모델·이력은 그대로다. 변경 명령이 아니면 거절한다."""
    m = app.model.materials.create(name="steel")
    before = (history_len(app), app.digest()["total"])
    pv = app.execute("app.command_preview", command="material.create", params={"name": "new"})
    assert pv["changes"]["created"] == [{"id": pv["result"]["id"], "kind": "material", "name": "new"}]
    assert pv["changes"]["updated"] == [] and pv["changes"]["deleted"] == [] and pv["changes"]["mesh"] is False
    assert (history_len(app), app.digest()["total"]) == before and [o["name"] for o in app.execute("material.list")] == ["steel"]
    pv = app.execute("app.command_preview", command="material.update", params={"id": m.id, "description": "d"})
    assert [c["id"] for c in pv["changes"]["updated"]] == [m.id] and "description" not in app.execute("material.get", id=m.id)["props"]
    pv = app.execute("app.command_preview", command="material.delete", params={"id": m.id})
    assert pv["changes"]["deleted"][0]["id"] == m.id and app.execute("material.get", id=m.id)["id"] == m.id
    pv = app.execute("app.command_preview", command="mesh.nodes_create", params={"coords": [[0.0, 0.0, 0.0]]})
    assert pv["changes"]["mesh"] is True and app.execute("project.info")["nodes"] == 0
    # 실패하는 명령은 오류를 그대로 내고 아무것도 남기지 않는다
    with pytest.raises(Nasa95Error) as e:
        app.execute("app.command_preview", command="material.create", params={"name": "steel"})
    assert e.value.code == "name_conflict" and (history_len(app), app.digest()["total"]) == before
    for cmd, code in [("app.undo", "not_available"), ("material.list", "not_available"), ("nope.x", "unknown_command")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("app.command_preview", command=cmd)
        assert e.value.code == code


@pytest.mark.feature("API-06")
@pytest.mark.feature("API-22")
@pytest.mark.feature("API-31")
def test_SYS_25_event_polling(app):
    """명령 계층의 구독: 쌓인 통지를 꺼내면 비워지고, 한도를 넘으면 오래된 것부터 버린 수를 알린다."""
    s = app.execute("event.subscribe", limit=2)
    assert s["events"] == [] and s["dropped"] == 0
    a = app.model.materials.create(name="a")
    got = app.execute("event.subscribe", id=s["id"])
    assert [(e["source"], e["command"], e["created"]) for e in got["events"]] == [("command", "material.create", [a.id])]
    assert app.execute("event.subscribe", id=s["id"])["events"] == []  # 꺼내면 비워진다
    app.model.materials.create(name="b")
    app.model.materials.create(name="c")
    app.undo()
    got = app.execute("event.subscribe", id=s["id"])
    assert got["dropped"] == 1 and [e["source"] for e in got["events"]] == ["command", "undo"]
    other = app.execute("event.subscribe")
    assert other["id"] != s["id"]
    assert app.execute("event.unsubscribe", id=s["id"])["remaining"] == 1
    app.model.materials.create(name="d")
    assert len(app.execute("event.subscribe", id=other["id"])["events"]) == 1
    for params in ({"id": s["id"]}, {"id": 99}):
        with pytest.raises(Nasa95Error) as e:
            app.execute("event.subscribe", **params)
        assert e.value.code == "not_found"
    with pytest.raises(Nasa95Error):
        app.execute("event.unsubscribe", id=s["id"])


@pytest.mark.feature("API-23")
def test_SYS_26_extension_storage(app, tmp_path):
    """확장 데이터는 프로젝트에 저장되고(저장·열기) Undo 된다. null 로 지운다."""
    assert app.execute("ext.storage_get", extension="x")["values"] == {}
    assert app.execute("ext.storage_get", extension="x", key="k")["value"] is None
    app.execute("ext.storage_set", extension="x", key="k", value={"n": 1})
    app.execute("ext.storage_set", extension="x", key="list", value=[1, 2])
    app.execute("ext.storage_set", extension="y", key="k", value="s")
    assert app.execute("ext.storage_get", extension="x")["values"] == {"k": {"n": 1}, "list": [1, 2]}
    assert app.execute("ext.storage_get", extension="y", key="k")["value"] == "s"
    path = tmp_path / "p.json"
    app.execute("project.save", path=str(path))
    again = App()
    again.execute("project.open", path=str(path))
    assert again.execute("ext.storage_get", extension="x")["values"] == {"k": {"n": 1}, "list": [1, 2]}
    app.undo()
    assert app.execute("ext.storage_get", extension="y")["values"] == {}
    app.execute("ext.storage_set", extension="x", key="k", value=None)
    assert app.execute("ext.storage_get", extension="x")["values"] == {"list": [1, 2]}
    assert app.execute("unit.get")["system"] == "mm-t-s"  # 같은 설정 객체의 다른 값은 그대로
    with pytest.raises(Nasa95Error) as e:
        app.execute("ext.storage_set", extension="", key="k", value=1)
    assert e.value.code == "invalid_param"


# ================================================================ 확장 등록 API(API-18~21)
@pytest.mark.feature("API-18")
@pytest.mark.feature("API-19")
@pytest.mark.feature("API-20")
@pytest.mark.feature("API-21")
def test_SYS_27_ext_register(app, tmp_path):
    """ext.register_*: 코드·묶음으로 만든 명령, UI 항목 등록, 가져오기/내보내기 형식의 file.import/export 분배, 파생 결과 계산·보고서 형식."""
    names = lambda: {c["name"] for c in app.commands()}  # noqa: E731
    # 코드 본문 명령(조회)
    r = app.execute("ext.register_command", name="my.count", kind="Q", desc="재료 수",
                    code="def run(app, params):\n    return {'count': len(app.execute('material.list')), 'tag': params.get('tag')}\n",
                    params=[{"name": "tag", "type": "string", "desc": "표시"}])
    assert r["body"] == "code" and "my.count" in names()
    app.model.materials.create(name="a")
    assert app.execute("my.count", tag="t") == {"count": 1, "tag": "t"}
    # 묶음 본문 명령(변경): 안의 명령들이 Undo 한 단계
    app.execute("ext.register_command", name="my.two_materials", kind="C", desc="재료 둘",
                steps=[{"command": "material.create", "params": {"name": "$first"}}, {"command": "material.create", "params": {"name": "$second"}}],
                params=[{"name": "first", "type": "string", "desc": "", "must": True}, {"name": "second", "type": "string", "desc": "", "must": True}])
    n = history_len(app)
    r = app.execute("my.two_materials", first="b", second="c")
    assert r["name"] == "c" and len(app.execute("material.list")) == 3 and history_len(app) == n + 1
    app.undo()
    assert len(app.execute("material.list")) == 1
    with pytest.raises(Nasa95Error) as e:
        app.execute("my.two_materials", first="x")
    assert e.value.code == "missing_param"
    for bad, code in [({"name": "my.count", "code": "def run(app, p): pass"}, "name_conflict"), ({"name": "bad name", "code": "x=1"}, "invalid_param"),
                      ({"name": "my.x"}, "missing_param"), ({"name": "my.x", "code": "def nope(): pass"}, "invalid_param"),
                      ({"name": "my.x", "code": "def run(:"}, "invalid_param")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("ext.register_command", desc="", **bad)
        assert e.value.code == code
    # UI 항목: 창이 없으면 등록만 되고 ext.registrations 에 나온다
    app.execute("ext.register_menu", label="세기", command="my.count", menu="확장")
    app.execute("ext.register_tree_action", label="세기", command="my.count", kind="material")
    regs = app.execute("ext.registrations")
    assert [(i["type"], i["label"]) for i in regs["ui"]] == [("menu", "세기"), ("tree_action", "세기")] and regs["ui"][1]["kind"] == "material"
    # 가져오기 형식: .cnt 파일을 읽어 재료를 만든다
    app.execute("ext.register_command", name="my.import_cnt", kind="C", desc="cnt 가져오기",
                code="def run(app, params):\n    names = open(params['path'], encoding='utf-8').read().split()\n"
                     "    return {'ids': [app.execute('material.create', name=n)['id'] for n in names]}\n",
                params=[{"name": "path", "type": "string", "desc": "", "must": True}])
    app.execute("ext.register_importer", extension=".CNT", command="my.import_cnt", label="재료 이름 목록")
    cnt = tmp_path / "m.cnt"
    cnt.write_text("p q\n", encoding="utf-8")
    r = app.execute("file.import", path=str(cnt))
    assert len(r["ids"]) == 2 and {o["name"] for o in app.execute("material.list")} >= {"p", "q"}
    app.undo()
    assert not {o["name"] for o in app.execute("material.list")} & {"p", "q"}
    assert app.execute("ext.registrations")["importers"] == [{"extension": ".cnt", "command": "my.import_cnt", "label": "재료 이름 목록", "ext_name": ""}]
    with pytest.raises(Nasa95Error) as e:
        app.execute("file.import", path="x.unknown")
    assert e.value.code == "not_available" and ".cnt" in e.value.details["available"] and ".step" in e.value.details["available"]
    with pytest.raises(Nasa95Error) as e:
        app.execute("ext.register_exporter", extension="xyz", command="my.count")
    assert e.value.code == "invalid_param"
    with pytest.raises(Nasa95Error) as e:
        app.execute("ext.register_exporter", extension=".xyz", command="nope.cmd")
    assert e.value.code == "unknown_command"
    # 내보내기 형식: 내장 .inp 는 mesh.export
    ids = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]])["ids"]
    app.execute("mesh.elements_create", shape="tet4", connectivity=[ids])
    out = tmp_path / "m.inp"
    app.execute("file.export", path=str(out))
    assert "*ELEMENT" in out.read_text()
    # 파생 결과 계산·보고서 형식 등록
    app.execute("ext.register_command", name="my.ones", kind="Q", desc="노드마다 1",
                code="def run(app, params):\n    ids = params.get('nodes') or [1, 2]\n    return {'ids': ids, 'values': [1.0] * len(ids), 'frame': params.get('frame')}\n")
    r = app.execute("ext.register_derived_result", name="ones", command="my.ones", label="1")
    assert r["registered"] == ["ones"]
    app.execute("ext.register_command", name="my.report_txt", kind="J", desc="글 보고서",
                code="def run(app, params):\n    title = app.execute('report.get', id=params['id'])['props'].get('title', '')\n"
                     "    open(params['path'], 'w', encoding='utf-8').write('REPORT ' + title)\n    return {'path': params['path']}\n",
                params=[{"name": "id", "type": "integer", "desc": "보고서"}, {"name": "path", "type": "string", "desc": "파일"}])
    app.execute("ext.register_report", name="txt", command="my.report_txt")
    rep = app.model.reports.create(name="r", title="T", items=[{"kind": "text", "text": "x"}])
    out = tmp_path / "r.txt"
    r = app.execute("report.generate", id=rep.id, path=str(out), format="txt")
    assert r["format"] == "txt" and out.read_text(encoding="utf-8") == "REPORT T"
    with pytest.raises(Nasa95Error) as e:
        app.execute("report.generate", id=rep.id, path=str(out), format="docx")
    assert e.value.code == "not_found"
    regs = app.execute("ext.registrations")
    assert [d["name"] for d in regs["derived_results"]] == ["ones"] and [f["name"] for f in regs["report_formats"]] == ["txt"]


@pytest.mark.feature("API-33")
def test_SYS_28_file_upload_download(app, tmp_path):
    """file.upload 는 올림 폴더에 저장하고 경로를 주며, file.download 는 글 또는 base64 로 읽는다."""
    import base64
    app.execute("server.configure", upload_dir=str(tmp_path / "up"))
    r = app.execute("file.upload", name="a.txt", text="안녕\n")
    assert r["bytes"] == 7 and (tmp_path / "up" / "a.txt").read_text(encoding="utf-8") == "안녕\n"
    assert app.execute("file.download", name="a.txt") == {"path": r["path"], "bytes": 7, "encoding": "text", "text": "안녕\n"}
    raw = bytes(range(256))
    r = app.execute("file.upload", name="b.bin", base64=base64.b64encode(raw).decode())
    d = app.execute("file.download", path=r["path"])
    assert d["encoding"] == "base64" and base64.b64decode(d["base64"]) == raw
    assert base64.b64decode(app.execute("file.download", name="a.txt", encoding="base64")["base64"]) == "안녕\n".encode()
    for params, code in [({"name": "../x", "text": ""}, "invalid_param"), ({"name": "c.txt"}, "missing_param"), ({"name": "c.txt", "text": "", "base64": ""}, "missing_param")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("file.upload", **params)
        assert e.value.code == code
    for params, code in [({"name": "nope.txt"}, "io_error"), ({}, "missing_param"), ({"name": "b.bin", "encoding": "text"}, "invalid_param")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("file.download", **params)
        assert e.value.code == code
