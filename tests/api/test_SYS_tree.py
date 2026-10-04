"""워크 트리 조회·상태·탐색(SYS-19 ~ SYS-21): 계층, 모델 정의/스텝 정의 구분, 승계 표시, 피처 이력, 요약, 케이스 의존, 비활성, 사용처, 미지원 덱 내용, 대량·정렬.

TC: .agent/tests/tc-00-system.md SYS-19-01~19, SYS-20-03~14, SYS-21-04·05·11·12.
"""
import time

import pytest

from openfep import App, OfepError

from meshutil import block


def _branch(app, kind, **params):
    (b,) = app.execute("project.tree", kind=kind, **params)
    return b


def _names(items):
    return [i["name"] for i in items]


@pytest.mark.feature("CMN-01")
@pytest.mark.feature("WT-02")
def test_SYS_19_01_02_10_12_CAS_T01_16_hierarchy(app):
    """최상위 가지 = 상위 객체가 없는 종류(기능 정의의 계층), 모든 객체가 한 번씩. 타이·접촉·초기 조건은 스텝 밖, 하중·BC 는 스텝 아래."""
    b = block(app, 2, 1, 1)
    m = app.model.materials.create(name="M")
    m.set_elastic(data=[[1.0, 0.3]])
    p = app.model.properties.create_solid(name="P", material=m.id, target={"type": "elements", "ids": [1, 2]})
    s1 = app.model.sets.create_surface(name="A", faces=[[1, 1]])
    s2 = app.model.sets.create_surface(name="B", faces=[[2, 2]])
    cp = app.model.contact_properties.create(name="CP")
    pair = app.model.contact_pairs.create(name="PAIR", slave={"type": "set", "ids": [s1.id]}, master={"type": "set", "ids": [s2.id]}, interaction=cp.id)
    tie = app.model.constraints.create_tie(name="TIE", slave={"type": "set", "ids": [s1.id]}, master={"type": "set", "ids": [s2.id]})
    case = app.model.cases.create(name="C")
    ic = app.model.initial_conditions.create_temperature(name="T0", target={"type": "nodes", "ids": [1]}, value=20.0)
    step = case.steps.create_static(name="S1")
    ld = step.loads.create_force(name="F", target={"type": "nodes", "ids": [1]}, components=[0.0, 0.0, -1.0])
    bc = step.bcs.create_displacement(name="FIX", target={"type": "nodes", "ids": [2]}, dofs=[1])
    tree = app.execute("project.tree")
    kinds = [b["kind"] for b in tree]
    top = [k["kind"] for k in app.execute("app.kinds") if not k.get("parents")]
    assert kinds == top  # 가지 순서 = 종류 등록 순서
    assert all(k not in kinds for k in ("step", "load", "bc", "feature"))  # 하위 종류는 가지가 아니다

    def walk(items, out):
        for it in items:
            out.append((it["kind"], it["id"]))
            walk(it.get("children", []), out)
        return out

    seen = walk([it for b in tree for it in b["items"]], [])
    expect = {(o["kind"], o["id"]) for o in app.execute("project.search")}
    assert sorted(seen) == sorted(expect) and len(seen) == len(expect)  # 모든 객체가 정확히 한 번
    case_item = _branch(app, "case")["items"][0]
    by_kind = {}
    for c in case_item["children"]:
        by_kind.setdefault(c["kind"], []).append(c["id"])
    assert by_kind["step"] == [step.id] and "initial_condition" not in by_kind
    assert _branch(app, "initial_condition")["items"][0]["id"] == ic.id  # 초기 조건은 스텝 밖(모델 정의 가지)
    step_item = next(c for c in case_item["children"] if c["kind"] == "step")
    assert not step_item.get("children")  # 하중·구속은 스텝 아래가 아니라 하중 셋·구속 셋 아래에(2026-10-04 결정)
    own_load = app.execute("step.own_load_set", id=step.id)["id"]
    assert {(c["kind"], c["id"]) for c in next(i for i in _branch(app, "load_set")["items"] if i["id"] == own_load)["children"]} == {("load", ld.id)}
    assert [c["id"] for c in _branch(app, "bc_set")["items"][0]["children"]] == [bc.id]
    assert step_item["summary"]["load_sets"] == [own_load]
    assert _branch(app, "constraint")["items"][0]["id"] == tie.id and _branch(app, "contact_pair")["items"][0]["id"] == pair.id  # 스텝 밖
    # 부분 조회(SYS-19-10)
    only = app.execute("project.tree", kind="material")
    assert len(only) == 1 and only[0]["kind"] == "material" and _names(only[0]["items"]) == ["M"]
    with pytest.raises(OfepError):
        app.execute("project.tree", kind="material", sort="weird")


@pytest.mark.feature("WT-04")
def test_SYS_19_04_14_step_inheritance_display(app):
    case = app.model.cases.create(name="C")
    s1, s2 = case.steps.create_static(name="s1"), case.steps.create_static(name="s2")
    f1 = s1.loads.create_force(name="f1", target={"type": "nodes", "ids": [1]}, components=[0.0, 0.0, -1.0])
    b1 = s1.bcs.create_displacement(name="b1", target={"type": "nodes", "ids": [2]}, dofs=[1])
    f2 = s2.loads.create_force(name="f2", target={"type": "nodes", "ids": [3]}, components=[1.0, 0.0, 0.0])
    f1b = s2.loads.create_force(name="f1-mod", target={"type": "nodes", "ids": [1]}, components=[0.0, 0.0, -2.0])  # 같은 대상·종류 → 교체
    eff = app.execute("step.effective", id=s2.id)
    rows = {e["id"]: e for e in eff["loads"]}
    assert rows[f2.id]["inherited"] is False and rows[f1b.id]["inherited"] is False and f1.id not in rows  # f1 은 f1-mod 로 바뀜
    assert [e["id"] for e in eff["bcs"]] == [b1.id] and eff["bcs"][0]["inherited"] is True
    # 교체 스텝(SYS-19-14): 앞 스텝의 하중·BC 가 모두 빠진다
    app.execute("step.set_inheritance", id=s2.id, loads_inheritance="new", bcs_inheritance="new")
    eff = app.execute("step.effective", id=s2.id)
    assert {e["id"] for e in eff["loads"]} == {f2.id, f1b.id} and eff["bcs"] == []


@pytest.mark.feature("WT-05")
def test_SYS_19_05_15_feature_history_in_tree(app):
    if not app.execute("app.version").get("geometry"):
        pytest.skip("형상 커널 없이 빌드됨")
    part = app.model.parts.create(name="P")
    f1 = app.execute("feature.create_box", parent=part.id, name="box", size=[10.0, 10.0, 10.0])
    f2 = app.execute("feature.create_fillet", parent=part.id, name="fillet", edges=[1], radius=1.0)
    f3 = app.execute("feature.create_box", parent=part.id, name="box2", size=[2.0, 2.0, 2.0], origin=[20.0, 0.0, 0.0])
    item = _branch(app, "part")["items"][0]
    feats = [c for c in item["children"] if c["kind"] == "feature"]
    assert [f["name"] for f in feats] == ["box", "fillet", "box2"] and all(f["state"] == "ok" for f in feats)
    assert "rollback" not in item and item["summary"]["features"] == 3
    app.execute("feature.rollback", id=part.id, feature=f2["id"])
    item = _branch(app, "part")["items"][0]
    feats = [c for c in item["children"] if c["kind"] == "feature"]
    assert item["rollback"] == f2["id"] and [f["state"] for f in feats] == ["ok", "ok", "rolled_back"]
    # 정렬해도 피처 순서는 바뀌지 않는다(SYS-21-05)
    item = _branch(app, "part", sort="name", descending=True)["items"][0]
    assert [c["name"] for c in item["children"] if c["kind"] == "feature"] == ["box", "fillet", "box2"]
    app.execute("feature.rollback", id=part.id)
    app.execute("feature.suppress", id=f2["id"])
    feats = [c for c in _branch(app, "part")["items"][0]["children"] if c["kind"] == "feature"]
    assert feats[1]["suppressed"] is True and feats[1]["state"] == "suppressed" and feats[2]["state"] == "ok"


@pytest.mark.feature("WT-08")
def test_SYS_19_08_18_summary(app):
    m = app.model.materials.create(name="M")
    m.set_elastic(data=[[1.0, 0.3]]), m.set_density(data=[[1.0]])
    part = app.model.mesh_parts.create(name="MP")
    block(app, 2, 1, 1, part=part.id)
    s = app.model.sets.create_node(name="N", ids=[1, 2, 3])
    case = app.model.cases.create(name="C")
    st = case.steps.create_static(name="s")
    st.loads.create_force(target={"type": "set", "ids": [s.id]}, components=[0.0, 0.0, -1.0])
    p = app.model.properties.create_shell(name="SH", material=m.id, thickness=2.0)
    assert _branch(app, "material")["items"][0]["summary"] == {"behaviors": ["density", "elastic"]}
    assert _branch(app, "mesh_part")["items"][0]["summary"] == {"elements": 2} and _branch(app, "mesh_part")["count"] == 1
    assert _branch(app, "set")["items"][0]["summary"] == {"size": 3}
    ci = _branch(app, "case")["items"][0]
    own = app.execute("step.own_load_set", id=st.id)["id"]
    assert ci["summary"] == {"steps": 1} and ci["children"][0]["summary"] == {"loads": 1, "bcs": 0, "load_sets": [own], "bc_sets": []}  # 스텝 요약: 참조하는 셋과 그 항목 수
    assert _branch(app, "property")["items"][0]["summary"] == {"material": "M", "thickness": 2.0}
    # 요약 갱신(SYS-19-18): 요소를 더 만들면 요소 수가 바뀐다
    block(app, 3, 1, 1, part=part.id, origin=(0.0, 50.0, 0.0))
    assert _branch(app, "mesh_part")["items"][0]["summary"] == {"elements": 5}
    assert "summary" not in _branch(app, "mesh_part", summary=False)["items"][0]


@pytest.mark.feature("WT-09")
def test_SYS_19_09_19_case_dependencies(app):
    c1, c2 = app.model.cases.create(name="freq"), app.model.cases.create(name="ssd")
    assert app.execute("case.dependencies") == {"edges": [], "order": [c1.id, c2.id]}
    app.execute("case.link", id=c2.id, source=c1.id)
    dep = app.execute("case.dependencies")
    assert dep["edges"] == [{"case": c2.id, "needs": c1.id}] and dep["order"] == [c1.id, c2.id]
    assert app.execute("case.get", id=c2.id)["props"]["links"] == [c1.id]
    with pytest.raises(OfepError) as e:
        app.execute("case.link", id=c1.id, source=c2.id)  # 순환
    assert e.value.code == "cyclic_dependency"
    # 연결 해제(SYS-19-19)
    app.execute("case.update", id=c2.id, links=[])
    assert app.execute("case.dependencies")["edges"] == []


@pytest.mark.feature("WT-30")
def test_SYS_20_05_11_suppressed_and_inactive(app):
    b = block(app, 4, 1, 1)
    m = app.model.materials.create(name="M")
    m.set_elastic(data=[[1.0, 0.3]])
    p_root = app.model.properties.create_solid(name="ROOT", material=m.id, target={"type": "elements", "ids": [1, 2]})
    p_tip = app.model.properties.create_solid(name="TIP", material=m.id, target={"type": "elements", "ids": [3, 4]})
    case = app.model.cases.create(name="C")
    s1, s2 = case.steps.create_static(name="s1"), case.steps.create_static(name="s2")
    tip_node = b["node"](4, 0, 0)
    f = s1.loads.create_force(name="F", target={"type": "nodes", "ids": [tip_node]}, components=[0.0, 0.0, -1.0])
    g = s1.loads.create_force(name="G", target={"type": "nodes", "ids": [b["node"](0, 0, 0)]}, components=[0.0, 0.0, -1.0])
    s2.changes.create_model_change_element(target={"type": "elements", "ids": [3, 4]}, action="remove")
    f.suppress()
    props = {i["name"]: i for i in _branch(app, "property", step=s2.id)["items"]}
    assert props["TIP"]["inactive"] is True and "inactive" not in props["ROOT"]
    assert "inactive" not in {i["name"]: i for i in _branch(app, "property", step=s1.id)["items"]}["TIP"]
    own = app.execute("step.own_load_set", id=s1.id)["id"]
    loads = {c["name"]: c for c in next(i for i in _branch(app, "load_set", step=s2.id)["items"] if i["id"] == own)["children"]}
    assert loads["F"]["suppressed"] is True and loads["F"]["inactive"] is True  # 끝 노드는 제거된 요소에만 속한다
    assert loads["G"]["suppressed"] is False and "inactive" not in loads["G"]
    f.unsuppress()
    loads = {c["name"]: c for c in next(i for i in _branch(app, "load_set")["items"] if i["id"] == own)["children"]}
    assert loads["F"]["suppressed"] is False and "inactive" not in loads["F"]  # 스텝을 주지 않으면 비활성 판정이 없다
    with pytest.raises(OfepError):
        app.execute("project.tree", step=case.id)


@pytest.mark.feature("WT-31")
def test_SYS_20_06_12_usage(app):
    used, unused = app.model.materials.create(name="used"), app.model.materials.create(name="unused")
    p = app.model.properties.create_solid(name="P", material=used.id)
    assert [(r["kind"], r["id"]) for r in used.references()] == [("property", p.id)] and unused.references() == []
    p.delete()
    assert used.references() == []  # 사용처 변화(SYS-20-12)
    used.delete()  # 미사용이면 지울 수 있다


@pytest.mark.feature("WT-33")
def test_SYS_20_08_14_unsupported_deck_content(app, tmp_path):
    deck = tmp_path / "u.inp"
    deck.write_text("*NODE\n1, 0, 0, 0\n2, 1, 0, 0\n*ELEMENT, TYPE=T3D2, ELSET=E1\n1, 1, 2\n*VIEWFACTOR, READ\n*STEP\n*STATIC\n*RADIATE\n1, R1CR, 300., 0.8\n*END STEP\n")
    r = app.execute("deck.import", path=str(deck))
    listed = app.execute("deck.unsupported", id=r["case"])
    assert len(listed) >= 1 and all(x["case"] == r["case"] for x in listed)
    case_item = _branch(app, "case")["items"][0]

    def walk(items, out):
        for it in items:
            if it["kind"] == "deck_block":
                out.append(it["id"])
            walk(it.get("children", []), out)
        return out

    assert sorted(walk([case_item], [])) == sorted(x["id"] for x in listed)  # 미지원 내용은 케이스(또는 스텝) 아래 별도 항목
    # 전부 지원되는 덱(SYS-20-14)
    app.execute("project.new")
    deck2 = tmp_path / "ok.inp"
    deck2.write_text("*NODE\n1, 0, 0, 0\n2, 1, 0, 0\n*ELEMENT, TYPE=T3D2, ELSET=E1\n1, 1, 2\n*MATERIAL, NAME=M\n*ELASTIC\n1., 0.3\n*SOLID SECTION, ELSET=E1, MATERIAL=M\n1.\n*STEP\n*STATIC\n*BOUNDARY\n1, 1, 3\n*END STEP\n")
    r = app.execute("deck.import", path=str(deck2))
    assert app.execute("deck.unsupported", id=r["case"]) == []
    assert walk([_branch(app, "case")["items"][0]], []) == []


@pytest.mark.feature("WT-37")
@pytest.mark.feature("WT-38")
def test_SYS_21_04_05_11_12_bulk_and_sort(app):
    """대량 항목의 부분 조회는 0.5 초 이내(5천 개 가지), 요청한 범위만 돌려준다. 정렬: 이름·ID·생성 순, 내림차순."""
    app.execute("app.transaction_begin", name="bulk")
    for i in range(5000):
        app.execute("set.create_node", name=f"S{i:04d}", ids=[1])
    app.execute("app.transaction_commit")
    t0 = time.perf_counter()
    b = _branch(app, "set", offset=100, limit=50, summary=False)
    assert time.perf_counter() - t0 < 0.5
    assert b["count"] == 5000 and b["total"] == 5000 and b["offset"] == 100 and len(b["items"]) == 50
    assert _names(b["items"])[0] == "S0100" and _names(b["items"])[-1] == "S0149"
    assert len(_branch(app, "set", offset=4990, limit=50, summary=False)["items"]) == 10
    # 정렬(SYS-21-05·12)
    app.execute("project.new")
    for name in ("b", "c", "a"):
        app.model.materials.create(name=name)
    assert _names(_branch(app, "material")["items"]) == ["b", "c", "a"]  # 생성 순서
    assert _names(_branch(app, "material", sort="name")["items"]) == ["a", "b", "c"]
    assert _names(_branch(app, "material", sort="name", descending=True)["items"]) == ["c", "b", "a"]
    assert _names(_branch(app, "material", sort="id", descending=True)["items"]) == ["a", "c", "b"]
    # 스텝은 정렬해도 실행 순서 그대로
    case = app.model.cases.create(name="C")
    for name in ("s-b", "s-a"):
        case.steps.create_static(name=name)
    steps = [c["name"] for c in _branch(app, "case", sort="name")["items"][0]["children"]]
    assert steps == ["s-b", "s-a"]
