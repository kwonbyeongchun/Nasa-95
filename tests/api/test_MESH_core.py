"""메시(2단계): 데이터 모델, 노드·요소 편집, 조회, 통계, 검사, 품질.

케이스 정의: .agent/tests/tc-02-mesh.md (MSH-T01, T04~T06, T08 중 형상·메셔·솔버가 필요 없는 것),
tc-00-system.md (SYS-08~10). M1 = 박스 100×20×10.
"""
import numpy as np
import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total
from meshutil import block, plate

UNIT_CUBE = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]]


def cube(app: App, **kw) -> int:
    ids = app.execute("mesh.nodes_create", coords=UNIT_CUBE)["ids"]
    return app.execute("mesh.elements_create", shape="hex8", connectivity=[ids], **kw)["ids"][0]


# ================================================================ MSH-T01 데이터 모델
SHAPES = {"point1": 1, "line2": 2, "line3": 3, "tri3": 3, "tri6": 6, "quad4": 4, "quad8": 8, "tet4": 4, "tet10": 10,
          "hex8": 8, "hex20": 20, "wedge6": 6, "wedge15": 15, "pyramid5": 5}


@pytest.mark.feature("MSH-01")
def test_MSH_T01_01_04_every_shape(app):
    ids = app.execute("mesh.nodes_create", coords=np.random.default_rng(1).random((20, 3)))["ids"]
    for shape, n in SHAPES.items():
        eid = app.execute("mesh.elements_create", shape=shape, connectivity=[ids[:n]])["ids"][0]
        got = app.execute("mesh.elements", ids=[eid])[0]
        assert got["shape"] == shape and got["nodes"] == ids[:n]
    assert {t["shape"]: t["nodes"] for t in app.execute("mesh.element_types")} == SHAPES
    assert app.execute("project.info")["elements"] == len(SHAPES)


@pytest.mark.feature("MSH-01")
def test_MSH_T01_02_03_invalid_elements(app):
    ids = app.execute("mesh.nodes_create", coords=UNIT_CUBE)["ids"]
    before = total(app)
    for params, code in [
        (dict(shape="hex8", connectivity=[ids[:7]]), "invalid_param_type"),  # 절점 수 부족
        (dict(shape="hex8", connectivity=[ids[:7] + [999]]), "not_found"),  # 없는 노드
        (dict(shape="hex9", connectivity=[ids]), "out_of_range"),  # 없는 형상
        (dict(shape="hex8", connectivity=[ids], type="C3D4"), "out_of_range"),  # 형상에 맞지 않는 타입
        (dict(shape="hex8", connectivity=[ids, ids], ids=[5]), "invalid_param_type"),  # ID 개수 불일치
    ]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.elements_create", **params)
        assert e.value.code == code, params
        assert total(app) == before
    app.execute("mesh.elements_create", shape="hex8", connectivity=[ids], ids=[7])
    with pytest.raises(Nasa95Error) as e:  # 같은 ID
        app.execute("mesh.elements_create", shape="hex8", connectivity=[ids], ids=[7])
    assert e.value.code == "name_conflict"


@pytest.mark.feature("MSH-13")
def test_MSH_T04_03_04_05_node_editing(app):
    eid = cube(app)
    extra = app.execute("mesh.nodes_create", coords=[[5.0, 5.0, 5.0]])["ids"][0]
    app.execute("mesh.nodes_move", ids=[extra], coords=[[6.0, 7.0, 8.0]])
    assert app.execute("mesh.nodes", ids=[extra])["coords"] == [[6.0, 7.0, 8.0]]
    app.execute("mesh.nodes_move", ids=[extra], translation=[1.0, 0.0, 0.0])
    assert app.execute("mesh.nodes", ids=[extra])["coords"] == [[7.0, 7.0, 8.0]]
    app.execute("mesh.nodes_project", ids=[extra], point=[0, 0, 0], normal=[0, 0, 2])
    assert app.execute("mesh.nodes", ids=[extra])["coords"] == [[7.0, 7.0, 0.0]]
    before = total(app)
    with pytest.raises(Nasa95Error) as e:  # 요소가 쓰는 노드는 지울 수 없다
        app.execute("mesh.nodes_delete", ids=[1])
    assert e.value.code == "referenced" and total(app) == before
    app.execute("mesh.nodes_delete", ids=[extra])
    assert app.execute("project.info")["nodes"] == 8
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.nodes_project", ids=[1], point=[0, 0, 0], normal=[0, 0, 0])
    assert e.value.code == "out_of_range"
    assert eid == 1


@pytest.mark.feature("MSH-14")
def test_MSH_T04_06_element_delete(app):
    block(app, 2, 1, 1)
    app.execute("mesh.elements_delete", ids=[1])
    info = app.execute("project.info")
    assert (info["elements"], info["nodes"]) == (1, 12)  # 노드는 남는다
    app.undo()
    r = app.execute("mesh.elements_delete", ids=[1], delete_unused_nodes=True)
    assert r["nodes_deleted"] == 4 and app.execute("project.info")["nodes"] == 8
    app.undo()
    assert app.execute("project.info") | {"path": "", "modified": True} == app.execute("project.info")
    assert app.execute("mesh.check")["ok"]


# ================================================================ Undo·저장·통지 (SYS-08, ALL-04, ALL-05)
@pytest.mark.feature("CMN-08")
@pytest.mark.feature("CMN-15")
def test_ALL_04_mesh_commands_undo_redo(app):
    part = app.model.mesh_parts.create(name="P1")
    block(app, 2, 2, 1, part=part.id)
    steps = [
        ("mesh.nodes_create", dict(coords=[[9.0, 9.0, 9.0]])),
        ("mesh.nodes_move", dict(ids=[1, 2], translation=[0.0, 0.0, -1.0])),
        ("mesh.set_element_type", dict(ids=[1, 2], type="C3D8R")),
        ("mesh.flip_normals", dict(ids=[3])),
        ("mesh.elements_delete", dict(ids=[4], delete_unused_nodes=True)),
        ("mesh.elements_create", dict(shape="tet4", connectivity=[[1, 2, 4, 10]])),
        ("mesh.nodes_delete", dict(ids=[19])),
    ]
    for name, params in steps:
        before, hist = total(app), history_len(app)
        app.execute(name, params)
        after = total(app)
        assert after != before and history_len(app) == hist + 1, name
        app.undo()
        assert total(app) == before, f"{name}: Undo 후 상태가 다름"
        app.redo()
        assert total(app) == after, f"{name}: Redo 후 상태가 다름"


@pytest.mark.feature("CMN-19")
def test_ALL_03_06_mesh_failure_restores(app):
    cube(app)
    before, hist = total(app), history_len(app)
    with pytest.raises(Nasa95Error):  # 일부 요소만 유효하지 않은 타입 → 아무것도 바뀌지 않는다
        app.execute("mesh.set_element_type", type="S4")
    with pytest.raises(Nasa95Error):  # 묶음 안에서 실패하면 묶음 전체가 되돌려진다
        with app.transaction("메시 편집"):
            app.execute("mesh.nodes_create", coords=[[3.0, 3.0, 3.0]])
            app.execute("mesh.nodes_delete", ids=[1])
    assert total(app) == before and history_len(app) == hist


@pytest.mark.feature("CMN-09")
@pytest.mark.feature("API-39")
def test_SYS_08_01_04_save_open_with_mesh(app, tmp_path):
    part = app.model.mesh_parts.create(name="P1")
    block(app, 3, 2, 1, part=part.id)
    app.execute("mesh.set_element_type", ids=[1], type="C3D8I")
    d = app.digest()
    assert "mesh" in d["areas"]
    path = str(tmp_path / "m.nasa95")
    app.execute("project.save_as", path=path)
    other = App()
    other.execute("project.open", path=path)
    assert other.digest() == d
    assert other.execute("mesh.elements", ids=[1])[0]["type"] == "C3D8I"
    # 메시만 바꾸면 메시 영역의 요약값만 달라진다
    app.execute("mesh.nodes_move", ids=[1], translation=[0.1, 0, 0])
    d2 = app.digest()
    assert d2["areas"]["mesh"] != d["areas"]["mesh"] and d2["areas"]["mesh_part"] == d["areas"]["mesh_part"]
    app.execute("project.new")
    assert app.execute("project.info")["nodes"] == 0


@pytest.mark.feature("API-06")
def test_ALL_05_mesh_events(app):
    log = []
    app.subscribe(log.append)
    block(app, 2, 1, 1)
    assert [e["mesh"] for e in log] == [
        {"nodes_added": 12, "nodes_removed": 0, "nodes_moved": 0, "elements_added": 0, "elements_removed": 0, "elements_modified": 0},
        {"nodes_added": 0, "nodes_removed": 0, "nodes_moved": 0, "elements_added": 2, "elements_removed": 0, "elements_modified": 0},
    ]
    app.undo()
    assert log[-1]["source"] == "undo" and log[-1]["mesh"]["elements_removed"] == 2
    log.clear()
    app.execute("mesh.statistics")
    assert log == []


# ================================================================ 대량 입력·배열 (SYS-09, SYS-10)
@pytest.mark.feature("API-09")
def test_SYS_10_01_02_bulk(app):
    n = 100_000
    coords = np.random.default_rng(0).random((n, 3))
    hist = history_len(app)
    r = app.execute("mesh.nodes_create", coords=coords)
    assert r["count"] == n and "ids" not in r and history_len(app) == hist + 1
    ids = app.mesh.node_ids()
    app.execute("mesh.nodes_move", ids=ids, coords=coords * 2.0)
    assert np.array_equal(app.mesh.node_coords(), coords * 2.0)
    app.undo()
    assert np.array_equal(app.mesh.node_coords(), coords)
    app.undo()
    assert app.execute("project.info")["nodes"] == 0


@pytest.mark.feature("API-12")
def test_SYS_09_03_arrays(app):
    block(app, 2, 1, 1)
    ids, xyz = app.mesh.node_ids(), app.mesh.node_coords()
    assert ids.tolist() == list(range(1, 13)) and xyz.shape == (12, 3)
    assert xyz.tolist() == app.execute("mesh.nodes")["coords"]
    with pytest.raises(ValueError):  # 읽기 전용
        xyz[0, 0] = 5.0
    offset, nodes = app.mesh.element_nodes()
    assert offset.tolist() == [0, 8, 16] and app.mesh.element_shapes() == ["hex8", "hex8"]
    assert nodes[:8].tolist() == app.execute("mesh.elements", ids=[1])[0]["nodes"]


@pytest.mark.feature("API-08")
def test_SYS_06_journal_with_arrays(app, tmp_path):
    path = str(tmp_path / "mesh.journal")
    app.execute("journal.start", path=path)
    block(app, 2, 2, 2)
    app.execute("journal.stop")
    other = App()
    other.execute("journal.replay", path=path)  # 배열로 넘긴 입력도 저널에서 재생된다
    assert total(other) == total(app)


# ================================================================ MSH-T06 통계·추출
@pytest.mark.feature("MSH-25")
def test_MSH_T06_07_08_statistics(app):
    p1 = app.model.mesh_parts.create(name="A")
    p2 = app.model.mesh_parts.create(name="B")
    block(app, 20, 4, 2, part=p1.id)
    block(app, 2, 1, 1, size=(2.0, 1.0, 1.0), part=p2.id, origin=(200.0, 0.0, 0.0))
    s = app.execute("mesh.statistics")
    assert s["elements"] == 162 and s["by_shape"] == {"hex8": 162}
    assert s["volume"] == pytest.approx(20000.0 + 2.0)
    assert s["nodes"] == 21 * 5 * 3 + 12
    sp = app.execute("mesh.statistics", part=p2.id)
    assert (sp["elements"], sp["nodes"]) == (2, 12) and sp["volume"] == pytest.approx(2.0)


@pytest.mark.feature("MSH-22")
def test_MSH_T06_03_04_free_boundaries(app):
    block(app, 20, 4, 2)
    assert app.execute("mesh.free_faces")["count"] == 2 * (20 * 4 + 4 * 2 + 2 * 20)
    faces = app.execute("mesh.free_faces", ids=[1])["faces"]
    assert faces == [[1, f] for f in range(1, 7)]
    app.execute("project.new")
    plate(app, 10, 4)
    assert app.execute("mesh.free_edges")["count"] == 2 * (10 + 4)


@pytest.mark.feature("MSH-22")
def test_MSH_T06_face_numbering(app):
    """면 번호는 CalculiX 의 번호와 같아야 한다(육면체: 1 = 1-2-3-4 … 6 = 4-8-5-1)."""
    cube(app)
    lo = app.execute("mesh.find", what="nodes", box_min=[-1, -1, -0.5], box_max=[2, 2, 0.5])["ids"]
    assert lo == [1, 2, 3, 4]
    block(app, 1, 1, 1, size=(1.0, 1.0, 1.0), origin=(0.0, 0.0, 1.0))  # 위에 하나 더 (노드는 공유하지 않음)
    assert app.execute("mesh.free_faces")["count"] == 12


@pytest.mark.feature("MSH-17")
@pytest.mark.feature("MSH-19")
def test_MSH_find(app):
    block(app, 20, 4, 2)
    nodes = app.execute("mesh.find", what="nodes", box_min=[-0.1, -1, -1], box_max=[0.1, 100, 100])["ids"]
    assert len(nodes) == 5 * 3
    elems = app.execute("mesh.find", what="elements", box_min=[0, 0, 0], box_max=[5.0, 20, 10])["ids"]
    assert len(elems) == 4 * 2  # 첫 층(x)의 요소
    assert app.execute("mesh.find", what="elements", shape="tet4")["ids"] == []


# ================================================================ MSH-T05 품질·검사·방향
@pytest.mark.feature("MSH-17")
def test_MSH_T05_01_02_03_05_quality(app):
    cube(app)
    w = app.execute("mesh.quality")["worst"]
    assert w["aspect"] == pytest.approx(1.0) and w["jacobian"] == pytest.approx(1.0)
    app.execute("project.new")
    block(app, 1, 1, 1, size=(2.0, 1.0, 1.0))
    assert app.execute("mesh.quality")["worst"]["aspect"] == pytest.approx(2.0)
    # 뒤집힌 요소
    app.execute("mesh.flip_normals", ids=[1])
    q = app.execute("mesh.quality", jacobian_min=0.0, values=True)
    assert q["worst"]["jacobian"] == pytest.approx(-1.0) and [f["id"] for f in q["failed"]] == [1]
    assert q["values"][0]["size"] == pytest.approx(-2.0)
    assert app.execute("mesh.check")["inverted_elements"] == [1]
    # 기준 미달 목록이 넣은 요소와 정확히 일치
    app.execute("project.new")
    block(app, 4, 1, 1, size=(4.0, 1.0, 1.0))
    n = app.execute("mesh.elements", ids=[2])[0]["nodes"]
    app.execute("mesh.nodes_move", ids=[n[6]], translation=[0.0, 0.0, 3.0])  # 요소 2, 3 이 함께 쓰는 절점
    failed = app.execute("mesh.quality", aspect_max=1.5)["failed"]
    assert sorted({f["id"] for f in failed}) == [2, 3]


@pytest.mark.feature("MSH-17")
def test_MSH_T05_04_shell_metrics(app):
    plate(app, 1, 1, size=(1.0, 1.0))
    q = app.execute("mesh.quality", values=True)["values"][0]
    assert q["skew"] == pytest.approx(0.0) and q["warpage"] == pytest.approx(0.0) and q["size"] == pytest.approx(1.0)
    app.execute("mesh.nodes_move", ids=[4], translation=[0.0, 0.0, 0.5])  # 한 절점을 평면 밖으로
    assert app.execute("mesh.quality", values=True)["values"][0]["warpage"] > 10.0
    assert [f["metric"] for f in app.execute("mesh.quality", warpage_max=5.0)["failed"]] == ["warpage"]


@pytest.mark.feature("MSH-19")
@pytest.mark.feature("MSH-38")
def test_MSH_T05_07_08_T09_03_08_check(app):
    block(app, 2, 1, 1)
    assert app.execute("mesh.check")["ok"]
    nodes = app.execute("mesh.elements", ids=[1])[0]["nodes"]
    dup = app.execute("mesh.elements_create", shape="hex8", connectivity=[nodes])["ids"][0]
    stray = app.execute("mesh.nodes_create", coords=[[50, 50, 50], [60, 60, 60]])["ids"]
    pyr = app.execute("mesh.elements_create", shape="pyramid5", connectivity=[nodes[:5]])["ids"][0]
    c = app.execute("mesh.check")
    assert not c["ok"]
    assert c["duplicate_elements"] == [[1, dup]]
    assert c["unreferenced_nodes"] == stray
    assert c["unsupported_elements"] == [pyr]


@pytest.mark.feature("MSH-20")
def test_MSH_T05_09_10_11_shell_normals(app):
    plate(app, 3, 2)
    assert app.execute("mesh.check")["inconsistent_normals"] == [] and app.execute("mesh.free_edges")["count"] == 10
    app.execute("mesh.flip_normals", ids=[2, 5])
    assert len(app.execute("mesh.check")["inconsistent_normals"]) > 0
    r = app.execute("mesh.align_normals")
    assert r["flipped"] in (2, 4)  # 기준이 되는 요소에 따라 2개 또는 나머지 4개를 뒤집는다
    assert app.execute("mesh.check")["inconsistent_normals"] == []
    before = app.execute("mesh.elements", ids=[1])[0]["nodes"]
    app.execute("mesh.flip_normals")
    assert app.execute("mesh.elements", ids=[1])[0]["nodes"] == [before[0], before[3], before[2], before[1]]
    assert app.execute("mesh.check")["inconsistent_normals"] == []  # 전부 뒤집으면 여전히 일관된다


# ================================================================ MSH-T08 솔버 요소 타입
@pytest.mark.feature("MSH-26")
def test_MSH_T08_01_02_03_element_type(app):
    part = app.model.mesh_parts.create(name="P")
    block(app, 2, 1, 1, part=part.id)
    assert app.execute("mesh.elements", ids=[1])[0]["type"] == "C3D8"  # 형상의 기본 타입
    app.execute("mesh.set_element_type", part=part.id, type="C3D8R")
    assert {e["type"] for e in app.execute("mesh.elements")} == {"C3D8R"}
    assert app.execute("mesh.statistics")["by_type"] == {"C3D8R": 2}
    before = total(app)
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.set_element_type", ids=[1], type="C3D4")
    assert e.value.code == "out_of_range" and total(app) == before
    types = {t["shape"]: t for t in app.execute("mesh.element_types")}
    assert types["hex8"]["types"] == ["C3D8", "C3D8R", "C3D8I", "F3D8"]
    assert types["pyramid5"]["types"] == [] and types["pyramid5"]["default_type"] == ""
    assert "S8R" in types["quad8"]["types"] and "D" in types["line3"]["types"]


@pytest.mark.feature("MSH-33")
def test_MSH_T08_12_network_element(app):
    ids = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0], [2, 0, 0]])["ids"]
    # 입구: 첫 절점이 0. 네트워크 요소(TYPE=D)에서만 허용된다
    app.execute("mesh.elements_create", shape="line3", type="D", connectivity=[[0, ids[0], ids[1]], [ids[1], ids[2], 0]])
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.elements_create", shape="line3", type="B32", connectivity=[[0, ids[0], ids[1]]])
    assert e.value.code == "not_found"
    assert app.execute("mesh.check")["unreferenced_nodes"] == []


@pytest.mark.feature("MSH-01")
def test_mesh_part_reference(app):
    part = app.model.mesh_parts.create(name="P")
    mat = app.model.materials.create(name="m")
    prop = app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [part.id]})
    with pytest.raises(Nasa95Error) as e:  # 프로퍼티가 쓰는 메시 파트는 지울 수 없다
        part.delete()
    assert e.value.code == "referenced" and e.value.details["references"][0]["id"] == prop.id


# ================================================================ 품질 개선(MSH-18)
@pytest.mark.feature("MSH-18")
def test_MSH_improve_smoothing(app):
    """안쪽 노드를 흐트러뜨린 육면체 블록을 스무딩하면 최악 jacobian 이 좋아지고, 경계 노드·keep 노드는 그대로이며, Undo 된다."""
    b = block(app, 4, 4, 4, size=(4.0, 4.0, 4.0))
    node = b["node"]
    inner = [node(i, j, k) for i in range(1, 4) for j in range(1, 4) for k in range(1, 4)]
    rng = np.random.default_rng(3)
    coords = np.array(app.execute("mesh.nodes", ids=inner)["coords"]) + rng.uniform(-0.35, 0.35, (len(inner), 3))
    app.execute("mesh.nodes_move", ids=inner, coords=coords.tolist())
    before = app.execute("mesh.quality")["worst"]
    boundary = [node(i, j, k) for i in range(5) for j in range(5) for k in range(5) if 0 in (i, j, k) or 4 in (i, j, k)]
    bxyz = app.execute("mesh.nodes", ids=boundary)["coords"]
    keep = node(2, 2, 2)
    kxyz = app.execute("mesh.nodes", ids=[keep])["coords"]
    n = history_len(app)
    r = app.execute("mesh.improve", iterations=10, keep=[keep])
    assert r["before"]["jacobian"] == pytest.approx(before["jacobian"]) and r["after"]["jacobian"] > before["jacobian"]
    assert r["after"]["aspect"] <= before["aspect"] and r["moved"] >= 20 and r["fixed"] >= len(boundary) + 1
    after = app.execute("mesh.quality")["worst"]
    assert after["jacobian"] == pytest.approx(r["after"]["jacobian"]) and after["jacobian"] > 0.6
    assert app.execute("mesh.nodes", ids=boundary)["coords"] == bxyz and app.execute("mesh.nodes", ids=[keep])["coords"] == kxyz
    assert history_len(app) == n + 1
    app.undo()
    assert app.execute("mesh.quality")["worst"]["jacobian"] == pytest.approx(before["jacobian"])
    # 2차 요소: 중간 절점이 변의 중점으로 따라온다
    app.execute("mesh.convert_order", order=2)
    r = app.execute("mesh.improve", iterations=10)
    assert r["after"]["jacobian"] > before["jacobian"]
    e = app.execute("mesh.elements", ids=[b["elements"][0]])[0]
    xyz = app.execute("mesh.nodes", ids=e["nodes"])["coords"]
    assert xyz[8] == pytest.approx([(a + c) / 2 for a, c in zip(xyz[0], xyz[1])])  # 첫 변의 중간 절점
    with pytest.raises(Nasa95Error):
        App().execute("mesh.improve")


# ================================================================ 접촉 자동 탐지(BC-11)
@pytest.mark.feature("BC-11")
def test_BC_contact_detect(app):
    """떨어진 두 파트 사이에서 마주 보는 외부 면을 찾고, 그 후보로 타이 구속·접촉 쌍(면 셋 둘)을 만든다."""
    lower = app.model.mesh_parts.create(name="LOWER")
    upper = app.model.mesh_parts.create(name="UPPER")
    block(app, 4, 2, 1, size=(40.0, 20.0, 10.0), part=lower.id)
    block(app, 2, 2, 1, size=(20.0, 20.0, 10.0), part=upper.id, origin=(10.0, 0.0, 10.05))  # 위에 0.05 떠서
    r = app.execute("contact.detect", tolerance=0.1)
    assert len(r["pairs"]) == 1
    pair = r["pairs"][0]
    assert (pair["part_a"], pair["part_b"]) == (lower.id, upper.id)
    assert len(pair["faces_a"]) == 4 and len(pair["faces_b"]) == 4 and pair["master"] == "a"  # 아래 파트의 윗면 4개 ↔ 위 파트의 아랫면 4개
    assert pair["gap_min"] == pytest.approx(0.05) and pair["gap_max"] == pytest.approx(0.05)
    assert app.execute("contact.detect", tolerance=0.01)["pairs"] == []  # 간격보다 작은 허용 오차
    with pytest.raises(Nasa95Error) as e:
        app.execute("contact.detect", tolerance=0.1, parts=[lower.id])
    assert e.value.code == "invalid_state"
    # 타이 구속 만들기
    n = history_len(app)
    t = app.execute("contact.create_from_detection", pair=pair, kind="tie", name="T", position_tolerance=0.2)
    c = app.execute("constraint.get", id=t["id"])["props"]
    assert c["type"] == "tie" and c["master"] == {"type": "set", "ids": [t["master_set"]]} and c["position_tolerance"] == 0.2
    ms = app.execute("set.get", id=t["master_set"])
    assert ms["name"] == "T_master" and ms["props"]["type"] == "surface" and sorted(ms["props"]["faces"]) == sorted(pair["faces_a"])
    assert history_len(app) == n + 1  # 셋 둘과 구속이 한 단계
    # 접촉 쌍: 접촉 속성이 필요
    with pytest.raises(Nasa95Error) as e:
        app.execute("contact.create_from_detection", pair=pair)
    assert e.value.code == "missing_param"
    inter = app.model.contact_properties.create(name="INT1", pressure_overclosure="linear", slope=1e6)
    cp = app.execute("contact.create_from_detection", pair=pair, interaction=inter.id)
    props = app.execute("contact_pair.get", id=cp["id"])["props"]
    assert props["interaction"] == inter.id and props["slave"]["ids"] == [cp["slave_set"]]
    # 덱에 *TIE 와 *CONTACT PAIR 가 나간다
    mat = app.model.materials.create(name="M")
    mat.set_elastic(data=[[210000.0, 0.3]])
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [lower.id, upper.id]})
    case = app.model.cases.create(name="c")
    case.steps.create_static()
    deck = app.execute("case.preview_deck", id=case.id)["text"]
    assert "*TIE" in deck and "*CONTACT PAIR" in deck and "*SURFACE, NAME=T_master, TYPE=ELEMENT" in deck
    with pytest.raises(Nasa95Error) as e:
        app.execute("contact.create_from_detection", pair={"part_a": 1}, kind="tie")
    assert e.value.code == "invalid_param"


# ================================================================ 균열면(MSH-37)
@pytest.mark.feature("MSH-37")
def test_MSH_create_crack(app):
    """블록 가운데 x = 20 의 안쪽 면 가운데 아래쪽 절반을 균열면으로: 균열 안쪽 노드는 복제되고 앞선(위쪽 가장자리) 노드는 이어진다. 양쪽 면 셋이 생기고 Undo 된다."""
    b = block(app, 4, 2, 2, size=(40.0, 20.0, 20.0))
    node = b["node"]
    n0, e0 = app.execute("project.info")["nodes"], app.execute("mesh.quality")["count"]
    # x = 20 평면(i = 2)의 요소면: 왼쪽 요소(i = 1)의 +x 면(면 번호 4: 꼭짓점 1,2,6,5 → hex 면 표 {1,5,6,2} 가 4번째). 아래 절반 k = 0 만
    left = [b["elements"][0] + 1 + 4 * (j + 2 * k) for j in range(2) for k in range(1)]  # i=1 요소들(아래층)
    fr = app.execute("mesh.free_faces")  # 전체에서 혼자 있는 면: 안쪽 면은 free 가 아니다
    inner = [[e, f] for e in left for f in range(1, 7) if [e, f] not in fr["faces"] and f in (3, 4, 5, 6)]
    # +x 면 판정: 면의 노드가 모두 x = 20 인 것
    def face_nodes(e, f):
        el = app.execute("mesh.elements", ids=[e])[0]
        tables = {1: [0, 1, 2, 3], 2: [4, 7, 6, 5], 3: [0, 4, 5, 1], 4: [1, 5, 6, 2], 5: [2, 6, 7, 3], 6: [3, 7, 4, 0]}
        return [el["nodes"][k] for k in tables[f]]
    crack = [[e, f] for e, f in inner if all(abs(app.execute("mesh.nodes", ids=[n])["coords"][0][0] - 20.0) < 1e-9 for n in face_nodes(e, f))]
    assert len(crack) == 2
    r = app.execute("mesh.create_crack", faces={"type": "faces", "ids": crack}, name="crk")
    # 균열면 노드 6개(x=20, k=0,1 의 j=0..2) 가운데 앞선(k=1, 위쪽 모서리) 3개는 이어지고 아래 3개(k=0)만 복제된다
    assert len(r["new_nodes"]) == 3 and r["faces"] == 2 and r["faces_b"] == 2 and len(r["split_elements"]) == 2
    assert app.execute("project.info")["nodes"] == n0 + 3 and app.execute("mesh.quality")["count"] == e0
    xs = [app.execute("mesh.nodes", ids=[n])["coords"][0] for n in r["new_nodes"]]
    assert all(abs(x[0] - 20.0) < 1e-9 and abs(x[2]) < 1e-9 for x in xs)  # 복제 노드는 x = 20, z = 0 (아래층 바닥)
    # 갈라진 요소(오른쪽 i=2 아래층)는 새 노드를 쓰고, 그 위층 요소는 원래 노드를 쓴다
    right_low = [b["elements"][0] + 2 + 4 * (j + 2 * 0) for j in range(2)]
    assert sorted(r["split_elements"]) == sorted(right_low)
    el = app.execute("mesh.elements", ids=[right_low[0]])[0]
    assert set(el["nodes"]) & set(r["new_nodes"])
    sets = {s["name"]: s for s in app.execute("set.list")}
    assert sorted(sets["crk_a"]["props"]["faces"]) == sorted(crack) if "props" in sets["crk_a"] else True
    sa, sb = app.execute("set.get", id=r["set_a"]), app.execute("set.get", id=r["set_b"])
    assert sorted(sa["props"]["faces"]) == sorted(crack) and {e for e, f in sb["props"]["faces"]} == set(right_low)
    # 자유 면이 늘었다(균열 양쪽)
    assert app.execute("mesh.free_faces")["count"] == 4 * 2 * 2 + 4 * 2 * 2 + 2 * 2 * 2 - 0 + 4  # 상자 겉면 + 균열면 2개 × 2
    app.undo()
    assert app.execute("project.info")["nodes"] == n0 and app.execute("set.list") == []
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.create_crack", faces={"type": "faces", "ids": fr["faces"][:1]})  # 겉면은 가를 것이 없다
    assert e.value.code == "invalid_state"
