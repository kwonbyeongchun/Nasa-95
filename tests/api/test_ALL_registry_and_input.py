"""전수 검사: 명령 등록부(ALL-01), 모든 API 호출(ALL-02), 잘못된 입력(ALL-03), 뷰·조회의 모델 불변(ALL-06)."""
import pytest

from nasa95 import App, Nasa95Error

from conftest import Builder, documented_commands, history_len, total

FIELD_TYPES = {"number", "integer", "string", "bool", "vector3", "number_list", "integer_list", "string_list", "table",
               "array", "object", "target", "ref", "ref_list", "object_list", "any"}


def c_commands(app: App) -> list[dict]:
    return [c for c in app.commands() if c["undoable"]]


# ---------------------------------------------------------------- ALL-01
@pytest.mark.feature("API-01")
def test_ALL_01_01_names_match_document(app):
    doc = documented_commands()
    if doc is None:
        pytest.skip("전체 API 목록 문서를 찾을 수 없음(NASA95_WORKS)")
    registered = {c["name"] for c in app.commands()}
    undocumented = sorted(registered - set(doc))
    assert not undocumented, f"문서에 없는 명령: {undocumented}"
    # 문서에만 있는 것은 아직 구현되지 않은 명령이다. 개수를 남긴다(실패로 보지 않는다 — 구현 진행 중).
    print(f"\n구현된 명령 {len(registered)} / 문서의 명령 {len(doc)} (미구현 {len(set(doc) - registered)})")


@pytest.mark.feature("API-01")
def test_ALL_01_02_param_definitions(app):
    for c in app.commands():
        names = [p["name"] for p in c["params"]]
        assert len(names) == len(set(names)), f"{c['name']}: 매개변수 이름 중복"
        for p in c["params"]:
            assert p["name"] and p["desc"], f"{c['name']}.{p['name']}: 이름·설명 누락"
            assert p["type"] in FIELD_TYPES, f"{c['name']}.{p['name']}: 알 수 없는 타입 {p['type']}"
            if p["type"] in ("ref", "ref_list") and p["name"] not in ("id", "ids", "parent"):
                assert p.get("ref_kind"), f"{c['name']}.{p['name']}: 참조 대상 종류 누락"


@pytest.mark.feature("API-01")
def test_ALL_01_03_description_and_kind(app):
    doc = documented_commands() or {}
    for c in app.commands():
        assert c["desc"].strip(), f"{c['name']}: 설명 없음"
        assert c["kind"] in "CQJVS"
        if c["name"] in doc:
            assert c["kind"] == doc[c["name"]], f"{c['name']}: 종류가 문서({doc[c['name']]})와 다름"
        # C 명령은 되돌릴 수 있어야 한다(되돌릴 수 없는 C 명령이 생기면 그때 예외 목록을 둔다)
        if c["kind"] == "C":
            assert c["undoable"], f"{c['name']}: C 명령인데 Undo 대상이 아님"


@pytest.mark.feature("API-03")
def test_ALL_01_04_list_on_empty_model(app):
    before = total(app)
    for kind in app.kinds():
        assert app.execute(f"{kind}.list") == []
    assert total(app) == before and history_len(app) == 0


@pytest.mark.feature("API-03")
def test_ALL_01_05_query_is_repeatable(app, build):
    build.populate()
    before = total(app)
    for kind in app.kinds():
        assert app.execute(f"{kind}.list") == app.execute(f"{kind}.list")
    assert app.execute("project.tree") == app.execute("project.tree")
    assert total(app) == before


# ---------------------------------------------------------------- ALL-02
@pytest.mark.feature("API-02")
def test_ALL_02_02_execute_by_name(app):
    a2 = App()
    app.execute("material.create", name="m1")
    a2.execute("app.execute", command="material.create", params={"name": "m1"})
    assert total(app) == total(a2)


@pytest.mark.feature("API-02")
def test_ALL_02_03_unknown_command(app):
    with pytest.raises(Nasa95Error) as e:
        app.execute("no.such_command")
    assert e.value.code == "unknown_command"


# ---------------------------------------------------------------- ALL-03
def expect_rejected(app: App, name: str, params: dict, codes: set[str], check_state: bool = True):
    """오류가 구조화되어 있고, 모델과 이력이 바뀌지 않았는지 본다.

    check_state=False 면 상태 비교를 호출자가 묶어서 한다(전수 검사에서 호출 수가 많을 때).
    """
    if check_state:
        before, hist = total(app), history_len(app)
    with pytest.raises(Nasa95Error) as e:
        app.execute(name, params)
    assert e.value.code in codes, f"{name}: 오류 코드 {e.value.code} (기대 {codes})"
    assert e.value.message
    assert e.value.details.get("command") == name
    if check_state:
        assert total(app) == before, f"{name}: 실패했는데 모델이 바뀜"
        assert history_len(app) == hist, f"{name}: 실패했는데 이력이 생김"
    return e.value


WRONG = {"number": "x", "integer": 1.5, "string": 1, "bool": "x", "vector3": [1, 2], "number_list": "x",
         "integer_list": [1.5], "string_list": [1], "table": [1], "array": 1, "object": 1, "target": 1,
         "ref": "x", "ref_list": 1, "object_list": [1]}


@pytest.mark.feature("API-05")
def test_ALL_03_01_wrong_type(app, build):
    build.populate()
    for c in c_commands(app):
        before, hist = total(app), history_len(app)
        for p in c["params"]:
            if p["type"] == "any":  # 호출 수준에서 타입을 보지 않는 매개변수
                continue
            err = expect_rejected(app, c["name"], {p["name"]: WRONG[p["type"]]}, {"invalid_param_type"}, False)
            assert err.details.get("param") == p["name"]
        assert total(app) == before and history_len(app) == hist, f"{c['name']}: 실패했는데 모델·이력이 바뀜"


@pytest.mark.feature("API-05")
def test_ALL_03_02_unknown_id(app, build):
    build.populate()
    for c in c_commands(app):
        if any(p["name"] == "id" for p in c["params"]):
            params = {"id": 999999, **build.command_params(c)}
            expect_rejected(app, c["name"], params, {"not_found"})


@pytest.mark.feature("API-05")
def test_ALL_03_03_out_of_range(app, build):
    mat = build.make("material")
    case = build.make("case")
    expect_rejected(app, "property.create_shell", {"material": mat, "thickness": -1.0}, {"out_of_range"})
    expect_rejected(app, "property.create_shell", {"material": mat, "thickness": 0.0}, {"out_of_range"})
    expect_rejected(app, "step.create_frequency", {"parent": case, "num_modes": 0}, {"out_of_range"})
    expect_rejected(app, "case.create", {"physics": "없는 분야"}, {"out_of_range"})
    expect_rejected(app, "function.create_amplitude", {"points": [[0, 0]], "time": "nope"}, {"out_of_range"})
    # 객체 명령이 아닌 일반 명령도 선택지·범위·목록 항목을 스키마대로 검사한다(내부 오류가 아니라 매개변수 이름이 붙은 오류)
    part = app.model.parts.create(name="P")
    sk = app.execute("sketch.create", parent=part.id, point=[0, 0, 0], normal=[0, 0, 1], x_axis=[1, 0, 0])["id"]
    app.execute("sketch.add_rectangle", id=sk, corner=[0, 0], size=[10, 5])
    with pytest.raises(Nasa95Error) as e:
        app.execute("sketch.add_dimension", id=sk, kind="horizontal_distance", value=10.0, points=[{"entity": 1, "point": 1}, {"entity": 1, "point": 2}])
    assert e.value.code == "invalid_param_type" and e.value.details["param"] == "points" and e.value.details["item"] == "points[0].point"
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.generate", id=part.id, method="voxel")
    assert e.value.code == "out_of_range" and e.value.details["param"] == "method"
    with pytest.raises(Nasa95Error) as e:
        app.execute("view.result_show", result=1, frame=0)
    assert e.value.code == "out_of_range" and e.value.details["param"] == "frame"


@pytest.mark.feature("API-05")
def test_ALL_03_04_missing_required(app, build):
    build.populate()
    for c in c_commands(app):
        musts = [p["name"] for p in c["params"] if p["must"]]
        if musts:
            err = expect_rejected(app, c["name"], {}, {"missing_param"})
            assert err.details.get("param") in musts


@pytest.mark.feature("API-05")
def test_ALL_03_05_unknown_param(app, build):
    build.populate()
    for c in app.commands():
        err = expect_rejected(app, c["name"], {"__no_such_param__": 1}, {"unknown_param"})
        assert err.details.get("param") == "__no_such_param__"


@pytest.mark.feature("CMN-19")
def test_ALL_03_06_failure_restores_model(app, build):
    # 생성 도중 검사에서 실패하는 입력: 좌표계의 두 축이 평행
    expect_rejected(app, "csys.create_rectangular",
                    dict(origin=[0, 0, 0], axis1_point=[1, 0, 0], plane12_point=[2, 0, 0]), {"invalid_geometry"})
    # 수정 도중 실패
    cid = build.make("csys", "rectangular")
    expect_rejected(app, "csys.update", dict(id=cid, plane12_point=[5, 0, 0]), {"invalid_geometry"})
    assert app.model.csys[cid].props["plane12_point"] == [0, 1, 0]


@pytest.mark.feature("API-05")
def test_ALL_03_wrong_kind_and_bad_reference(app, build):
    mat, case = build.make("material"), build.make("case")
    expect_rejected(app, "material.get", {"id": case}, {"wrong_kind"})
    expect_rejected(app, "property.create_solid", {"material": case}, {"wrong_kind"})
    expect_rejected(app, "property.create_solid", {"material": 999999}, {"not_found"})
    expect_rejected(app, "step.create_static", {"parent": mat}, {"invalid_parent"})
    expect_rejected(app, "material.create", {"name": "이름 규칙 위반"}, {"invalid_name"})
    expect_rejected(app, "property.update", {"id": build.make("property", "solid"), "offset": 1.0},
                    {"unknown_param"})


# ---------------------------------------------------------------- ALL-06
@pytest.mark.feature("API-16")
def test_ALL_06_02_queries_do_not_change_model(app, build):
    ids = build.populate()
    for c in app.commands():
        if c["kind"] != "Q":
            continue
        params = {}
        if any(p["name"] == "id" and p["must"] for p in c["params"]):
            params["id"] = next(i for (k, _), i in ids.items() if k == c["target"])
        params.update(build.command_params(c))  # 참조 매개변수의 표본 객체는 여기서 만들어진다(조회가 바꾼 것이 아니다)
        before, hist = total(app), history_len(app)
        try:
            app.execute(c["name"], params)
        except Nasa95Error:
            pass  # 조건이 맞지 않아 거부된 조회도 모델을 바꾸면 안 된다
        assert total(app) == before and history_len(app) == hist, c["name"]
