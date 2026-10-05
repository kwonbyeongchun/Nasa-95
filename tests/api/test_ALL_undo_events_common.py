"""전수 검사: Undo/Redo(ALL-04), 변경 통지(ALL-05), 객체 공통 동작(ALL-07)."""
import pytest

from nasa95 import Nasa95Error

from conftest import Builder, history_len, total


def all_pairs(build: Builder):
    return build.create_commands()


# ---------------------------------------------------------------- ALL-04
@pytest.mark.feature("CMN-08")
def test_ALL_04_01_02_undo_redo_every_create(app, build):
    for kind, sub in all_pairs(build):
        params = build.params(kind, sub)  # 상위 객체·참조 대상은 미리 만들어 둔다
        before = total(app)
        app.execute(build.command(kind, sub), params)
        after = total(app)
        assert after != before
        assert app.undo()["done"]
        assert total(app) == before, f"{kind}.{sub}: Undo 후 상태가 다름"
        assert app.redo()["done"]
        assert total(app) == after, f"{kind}.{sub}: Redo 후 상태가 다름"


@pytest.mark.feature("CMN-08")
def test_ALL_04_01_02_undo_redo_every_action(app, build):
    """생성 외의 모든 C 명령(속성 변경·이름 변경·복제·삭제·억제·이동·구성 모델)."""
    app.execute("app.history_limit", limit=1000000)  # 이력 한도에 걸리지 않게
    for kind, sub in all_pairs(build):
        oid = build.make(kind, sub)
        change = build.update_value(kind, oid)
        actions = ([("update", dict(id=oid, **change))] if change is not None else []) + [
            ("rename", dict(id=oid, name=f"renamed-{oid}")),
            ("suppress", dict(id=oid)),
            ("unsuppress", dict(id=oid)),
            ("move", dict(id=oid, index=1)),
            ("copy", dict(id=oid)),
            ("delete", dict(id=oid)),
        ]
        if not app.kinds()[kind]["suppressible"]:
            actions = [a for a in actions if a[0] not in ("suppress", "unsuppress")]
        if not app.kinds()[kind]["copyable"]:
            actions = [a for a in actions if a[0] != "copy"]
        # move 가 실제로 순서를 바꾸도록 같은 상위 객체 아래에 형제를 하나 더 둔다
        parent = app.execute(f"{kind}.get", id=oid)["parent"]
        build.make(kind, sub, **({"parent": parent} if parent else {}))
        siblings = [o["id"] for o in app.execute(f"{kind}.list", **({"parent": parent} if parent else {}))]
        actions = [(a, dict(p, index=0 if siblings.index(oid) else 1) if a == "move" else p) for a, p in actions]
        for action, params in actions:
            before, hist = total(app), history_len(app)
            app.execute(f"{kind}.{action}", params)
            after = total(app)
            assert after != before, f"{kind}.{action}: 모델이 바뀌지 않음"
            assert history_len(app) == hist + 1
            assert app.undo()["done"]
            assert total(app) == before, f"{kind}.{action}: Undo 후 상태가 다름"
            assert app.redo()["done"]
            assert total(app) == after, f"{kind}.{action}: Redo 후 상태가 다름"
    # 재료의 구성 모델: 설정 → 제거
    mat = build.make("material")
    cmds = {c["name"]: c for c in app.commands()}
    for name in sorted(n for n in cmds if n.startswith("material.set_") and n != "material.set_orientation"):
        behavior = name[len("material.set_"):]
        for cmd, params in [(name, dict(id=mat, **build.command_params(cmds[name], only_must=False))),
                            (f"material.remove_{behavior}", dict(id=mat))]:
            before = total(app)
            app.execute(cmd, params)
            after = total(app)
            assert after != before, f"{cmd}: 모델이 바뀌지 않음"
            app.undo()
            assert total(app) == before, f"{cmd}: Undo 후 상태가 다름"
            app.redo()
            assert total(app) == after, f"{cmd}: Redo 후 상태가 다름"


@pytest.mark.feature("CMN-15")
def test_ALL_04_03_one_command_one_step(app, build):
    # 하위 객체가 있는 케이스를 복제하면 여러 객체가 생기지만 Undo 는 한 번이다.
    case = build.make("case")
    step = build.make("step", "static", parent=case)
    build.make("load", "force", parent=step)
    before, hist = total(app), history_len(app)
    app.execute("case.copy", id=case)
    assert history_len(app) == hist + 1
    app.undo()
    assert total(app) == before


@pytest.mark.feature("CMN-08")
def test_ALL_04_04_undo_many(app, build):
    start = total(app)
    for i in range(10):
        app.execute("material.create", name=f"m{i}")
    for _ in range(10):
        assert app.undo()["done"]
    assert total(app) == start
    assert not app.undo()["done"]


@pytest.mark.feature("CMN-14")
def test_ALL_04_05_queries_not_in_history(app, build):
    build.make("material")
    hist = history_len(app)
    app.execute("material.list")
    app.execute("project.tree")
    app.execute("project.digest")
    assert history_len(app) == hist


@pytest.mark.feature("WT-19")
def test_ALL_04_06_09_tree_actions_undo_redo(app, build):
    case = build.make("case")
    s1 = build.make("step", "static", parent=case)
    build.make("step", "static", parent=case)
    for name, params in [("step.rename", dict(id=s1, name="first")), ("step.move", dict(id=s1, index=1)),
                         ("step.suppress", dict(id=s1)), ("step.delete", dict(id=s1))]:
        before = total(app)
        app.execute(name, params)
        after = total(app)
        app.undo()
        assert total(app) == before, name
        app.redo()
        assert total(app) == after, name
        app.undo()


# ---------------------------------------------------------------- ALL-05
@pytest.fixture
def events(app):
    log = []
    app.subscribe(log.append)
    return log


@pytest.mark.feature("API-06")
def test_ALL_05_01_created_event(app, build, events):
    for kind, sub in all_pairs(build):
        params = build.params(kind, sub)
        events.clear()
        oid = app.execute(build.command(kind, sub), params)["id"]
        assert len(events) == 1
        assert events[0]["created"] == [oid] and not events[0]["modified"] and not events[0]["deleted"]
        assert events[0]["source"] == "command"


@pytest.mark.feature("API-06")
def test_ALL_05_02_modified_and_deleted_events(app, build, events):
    oid = build.make("material")
    events.clear()
    app.execute("material.rename", id=oid, name="x")
    assert events[-1]["modified"] == [oid] and not events[-1]["created"]
    app.execute("material.delete", id=oid)
    assert events[-1]["deleted"] == [oid]


@pytest.mark.feature("API-06")
def test_ALL_05_03_cascade_in_event(app, build, events):
    case = build.make("case")
    step = build.make("step", "static", parent=case)
    load = build.make("load", "force", parent=step)
    events.clear()
    own = app.execute("load.get", id=load)["parent"]  # 스텝에 직접 만든 하중은 스텝 전용 셋에 들어가고, 셋은 스텝과 함께 지워진다
    app.execute("case.delete", id=case)
    assert sorted(events[-1]["deleted"]) == sorted([case, step, load, own])


@pytest.mark.feature("API-06")
def test_ALL_05_04_undo_redo_events(app, build, events):
    oid = build.make("material")
    events.clear()
    app.undo()
    assert events[-1]["source"] == "undo" and events[-1]["deleted"] == [oid]
    app.redo()
    assert events[-1]["source"] == "redo" and events[-1]["created"] == [oid]


@pytest.mark.feature("API-06")
def test_ALL_05_05_no_event_for_queries(app, build, events):
    build.make("material")
    events.clear()
    app.execute("material.list")
    app.execute("project.tree")
    assert events == []


# ---------------------------------------------------------------- ALL-07
@pytest.mark.feature("WT-10")
def test_ALL_07_01_create_all_kinds(app, build):
    for kind, sub in all_pairs(build):
        params = build.params(kind, sub)
        oid = app.execute(build.command(kind, sub), params)["id"]
        got = app.execute(f"{kind}.get", id=oid)
        for key, value in params.items():
            if key == "parent":
                assert got["parent"] == value
            else:
                assert got["props"][key] == value, f"{kind}.{sub}: {key}"
        assert oid in [o["id"] for o in app.execute(f"{kind}.list")]


@pytest.mark.feature("WT-10")
def test_ALL_07_11_parent_rules(app, build):
    case = build.make("case")
    step = app.execute("step.create_static", parent=case)["id"]
    assert app.execute("step.get", id=step)["parent"] == case
    with pytest.raises(Nasa95Error) as e:
        app.execute("step.create_static", parent=999999)
    assert e.value.code == "not_found"
    with pytest.raises(Nasa95Error) as e:
        app.execute("step.create_static")
    assert e.value.code == "missing_param"


@pytest.mark.feature("WT-11")
def test_ALL_07_02_12_update_only_given_fields(app, build):
    for kind, sub in all_pairs(build):
        oid = build.make(kind, sub)
        before = app.execute(f"{kind}.get", id=oid)["props"]
        change = build.update_value(kind, oid)
        if change is None:
            continue  # 바꿀 속성이 없는 종류
        app.execute(f"{kind}.update", dict(id=oid, **change))
        after = app.execute(f"{kind}.get", id=oid)["props"]
        for key in set(before) | set(after):
            if key in change:
                assert after[key] == change[key], f"{kind}.{sub}: {key} 가 바뀌지 않음"
            else:
                assert after.get(key) == before.get(key), f"{kind}.{sub}: {key} 가 바뀌면 안 됨"


@pytest.mark.feature("WT-12")
def test_ALL_07_03_04_rename_and_name_rules(app, build):
    for kind, sub in all_pairs(build):
        a = build.make(kind, sub)
        parent = app.execute(f"{kind}.get", id=a)["parent"]
        b = build.make(kind, sub, **({"parent": parent} if parent else {}))  # 같은 상위 객체 아래에서 본다
        name = f"alpha-{sub}"  # 같은 종류의 다른 하위 종류와 겹치지 않게
        app.execute(f"{kind}.rename", id=a, name=name)
        assert app.execute(f"{kind}.get", id=a)["name"] == name
        with pytest.raises(Nasa95Error) as e:
            app.execute(f"{kind}.rename", id=b, name=name)
        assert e.value.code == "name_conflict", f"{kind}.{sub}"
    with pytest.raises(Nasa95Error) as e:
        app.execute("material.create", name="has space")
    assert e.value.code == "invalid_name"


@pytest.mark.feature("WT-13")
def test_ALL_07_05_copy_is_independent(app, build):
    for kind, sub in all_pairs(build):
        if not app.kinds()[kind]["copyable"]:
            assert f"{kind}.copy" not in {c["name"] for c in app.commands()}
            continue
        oid = build.make(kind, sub)
        cid = app.execute(f"{kind}.copy", id=oid)["id"]
        src = app.execute(f"{kind}.get", id=oid)
        assert app.execute(f"{kind}.get", id=cid)["props"] == src["props"]
        change = build.update_value(kind, cid)
        if change is None:
            continue  # 바꿀 속성이 없는 종류
        app.execute(f"{kind}.update", dict(id=cid, **change))
        assert app.execute(f"{kind}.get", id=oid)["props"] == src["props"], f"{kind}.{sub}: 원본이 바뀜"


@pytest.mark.feature("WT-13")
def test_ALL_07_06_copy_with_children(app, build):
    case = build.make("case")
    step = build.make("step", "static", parent=case)
    func = build.make("function", "amplitude")
    load = build.make("load", "force", parent=step, amplitude=func)
    copy = app.execute("case.copy", id=case)["id"]
    steps = app.execute("step.list", parent=copy)
    assert len(steps) == 1 and steps[0]["id"] != step
    loads = app.execute("load.list", parent=app.execute("step.own_load_set", id=steps[0]["id"])["id"])  # 복제된 스텝의 전용 셋
    assert len(loads) == 1 and loads[0]["id"] != load
    # 복제 범위 밖의 객체(함수)를 가리키던 참조는 그대로 둔다
    assert app.execute("load.get", id=loads[0]["id"])["props"]["amplitude"] == func


@pytest.mark.feature("WT-14")
def test_ALL_07_07_delete(app, build):
    for kind, sub in all_pairs(build):
        oid = build.make(kind, sub)
        app.execute(f"{kind}.delete", id=oid)
        assert oid not in [o["id"] for o in app.execute(f"{kind}.list")]
        with pytest.raises(Nasa95Error) as e:
            app.execute(f"{kind}.get", id=oid)
        assert e.value.code == "not_found"


@pytest.mark.feature("WT-14")
def test_ALL_07_08_delete_referenced(app, build):
    mat = build.make("material")
    prop = build.make("property", "solid", material=mat)
    with pytest.raises(Nasa95Error) as e:
        app.execute("material.delete", id=mat)
    assert e.value.code == "referenced"
    refs = e.value.details["references"]
    assert [r["id"] for r in refs] == [prop]
    assert refs == app.execute("material.references", id=mat)
    # 참조하는 쪽을 지우면 삭제할 수 있다
    app.execute("property.delete", id=prop)
    app.execute("material.delete", id=mat)


@pytest.mark.feature("WT-15")
def test_ALL_07_09_13_suppress(app, build):
    for kind, sub in all_pairs(build):
        if not app.kinds()[kind]["suppressible"]:
            assert f"{kind}.suppress" not in {c["name"] for c in app.commands()}
            continue
        oid = build.make(kind, sub)
        app.execute(f"{kind}.suppress", id=oid)
        assert app.execute(f"{kind}.get", id=oid)["suppressed"] is True
        app.execute(f"{kind}.unsuppress", id=oid)
        assert app.execute(f"{kind}.get", id=oid)["suppressed"] is False
    # 참조되는 객체를 억제하면 참조하는 쪽이 검사에서 지목된다(정의된 동작)
    mat = build.make("material")
    prop = build.make("property", "solid", material=mat)
    app.execute("material.suppress", id=mat)
    codes = [i["code"] for i in app.execute("property.validate", id=prop)]
    assert "suppressed_reference" in codes


@pytest.mark.feature("WT-16")
def test_ALL_07_10_14_move(app, build):
    case = build.make("case")
    steps = [build.make("step", "static", parent=case) for _ in range(3)]
    app.execute("step.move", id=steps[2], index=0)
    assert [s["id"] for s in app.execute("step.list", parent=case)] == [steps[2], steps[0], steps[1]]
    # 다른 케이스로 옮기기
    case2 = build.make("case")
    load = build.make("load", "force", parent=steps[0])
    app.execute("step.move", id=steps[0], parent=case2)
    assert [s["id"] for s in app.execute("step.list", parent=case2)] == [steps[0]]
    own = app.execute("step.own_load_set", id=steps[0])["id"]
    assert app.execute("load.get", id=load)["parent"] == own and app.execute("load_set.get", id=own)["props"]["step"] == steps[0]  # 전용 셋째로 따라간다
    # 허용되지 않는 상위 객체
    mat = build.make("material")
    before = total(app)
    with pytest.raises(Nasa95Error) as e:
        app.execute("load.move", id=load, parent=mat)
    assert e.value.code == "invalid_parent" and total(app) == before
