"""메시 연산(2단계): 병합, 변환·복사, 돌출·회전, 차수 변환, 분할·세분화, 연결 요소, 재번호,
하중·경계조건의 대상 전개, 합력.

케이스 정의: .agent/tests/tc-02-mesh.md (MSH-T04, T06, T08), tc-04-loads-bcs.md, tc-00-system.md.
"""
import math

import numpy as np
import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total
from meshutil import block, plate


def info(app):
    i = app.execute("project.info")
    return i["nodes"], i["elements"]


def volume(app, **kw):
    return app.execute("mesh.statistics", **kw)["volume"]


def healthy(app):
    c = app.execute("mesh.check")
    assert c["inverted_elements"] == [] and c["duplicate_elements"] == [] and c["unreferenced_nodes"] == [], c


def steel(app):
    m = app.model.materials.create(name="steel")
    m.set_elastic(data=[[210000.0, 0.3]])
    m.set_density(data=[[7.85e-9]])
    return m


# ================================================================ 노드 병합 (MSH-15)
@pytest.mark.feature("MSH-15")
def test_MSH_T04_merge_nodes(app):
    block(app, 2, 2, 2, size=(2.0, 2.0, 2.0))
    block(app, 2, 2, 2, size=(2.0, 2.0, 2.0), origin=(2.0, 0.0, 0.0))  # 면 하나가 겹친다(노드 9개)
    assert info(app) == (54, 16) and app.execute("mesh.free_faces")["count"] == 48
    nset = app.model.sets.create_node(name="N", ids=[28, 29])  # 28 은 둘째 블록의 첫 노드 → 3 으로 병합된다
    before = total(app)
    assert app.execute("mesh.merge_nodes", tolerance=0.0)["merged"] == 0
    r = app.execute("mesh.merge_nodes", tolerance=1e-6)
    assert r["merged"] == 9 and info(app) == (45, 16)
    assert app.execute("mesh.free_faces")["count"] == 40  # 맞닿은 면이 안쪽 면이 됐다
    assert nset.props["ids"] == [3, 29]  # 셋이 가리키던 노드도 따라 바뀐다
    assert volume(app) == pytest.approx(16.0)
    healthy(app)
    app.undo()
    assert total(app) == before
    # 허용 거리와 정확히 같은 거리는 병합하지 않는다
    app.execute("project.new")
    app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0]])
    assert app.execute("mesh.merge_nodes", tolerance=1.0)["merged"] == 0
    assert app.execute("mesh.merge_nodes", tolerance=1.0 + 1e-9)["merged"] == 1
    assert app.mesh.node_ids().tolist() == [1]


# ================================================================ 변환·복사 (MSH-16)
@pytest.mark.feature("MSH-16")
def test_MSH_T04_transform(app):
    block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    xyz = app.mesh.node_coords().copy()
    app.execute("mesh.transform", translate=[1.0, 2.0, 3.0])
    assert np.allclose(app.mesh.node_coords(), xyz + [1.0, 2.0, 3.0])
    app.undo()
    app.execute("mesh.transform", rotate_axis=[0, 0, 2], rotate_angle=90.0)
    assert np.allclose(app.mesh.node_coords(), np.column_stack([-xyz[:, 1], xyz[:, 0], xyz[:, 2]]))
    assert volume(app) == pytest.approx(2.0)
    app.undo()
    app.execute("mesh.transform", scale_factor=2.0, scale_center=[2.0, 0.0, 0.0])
    assert volume(app) == pytest.approx(16.0) and app.mesh.node_coords()[:, 0].max() == pytest.approx(2.0)
    app.undo()
    # 대칭 이동: 좌표가 뒤집혀도 요소는 뒤집히지 않는다
    r = app.execute("mesh.transform", mirror_normal=[1, 0, 0])
    assert r["flipped"] == 2 and np.allclose(sorted(app.mesh.node_coords()[:, 0]), sorted(-xyz[:, 0]))
    assert volume(app) == pytest.approx(2.0)
    healthy(app)
    app.undo()
    assert np.array_equal(app.mesh.node_coords(), xyz)


@pytest.mark.feature("MSH-16")
def test_MSH_T04_transform_copy_and_errors(app):
    part = app.model.mesh_parts.create(name="copy")
    block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    r = app.execute("mesh.transform", ids=[1], mirror_point=[0, 0, 0], mirror_normal=[1, 0, 0], copy=True, copy_part=part.id)
    assert (r["nodes"], r["elements"]) == (8, 1) and info(app) == (20, 3)
    assert volume(app, part=part.id) == pytest.approx(1.0)  # 사본도 부피가 양수
    assert app.execute("mesh.elements", ids=[r["first_element"]])[0]["part"] == part.id
    healthy(app)
    before, hist = total(app), history_len(app)
    for params, code in [
        (dict(), "missing_param"),
        (dict(translate=[1, 0, 0], scale_factor=2.0), "missing_param"),
        (dict(rotate_angle=30.0), "missing_param"),
        (dict(rotate_angle=30.0, rotate_axis=[0, 0, 0]), "out_of_range"),
        (dict(scale_factor=0.0), "out_of_range"),
        (dict(translate=[1, 0, 0], ids=[99]), "not_found"),
    ]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.transform", **params)
        assert e.value.code == code, params
    assert total(app) == before and history_len(app) == hist
    # 노드만 옮기기
    app.execute("mesh.transform", nodes=[1], translate=[0.0, 0.0, -1.0])
    assert app.execute("mesh.nodes", ids=[1])["coords"] == [[0.0, 0.0, -1.0]]


# ================================================================ 돌출·회전 (MSH-16)
@pytest.mark.feature("MSH-16")
def test_MSH_T04_extrude(app):
    plate(app, 4, 2, size=(4.0, 2.0))
    before = total(app)
    r = app.execute("mesh.extrude", direction=[0, 0, 3.0], layers=3, delete_source=True)
    assert r["elements"] == 24 and r["nodes"] == 45
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"hex8": 24} and s["volume"] == pytest.approx(24.0)
    healthy(app)
    app.undo()
    assert total(app) == before
    # 아래로 돌출해도 부피는 양수
    app.execute("mesh.extrude", direction=[0, 0, -1.0], delete_source=True)
    assert volume(app) == pytest.approx(8.0)
    healthy(app)
    # 삼각형 → 웨지, 선 → 사각형
    app.execute("project.new")
    plate(app, 2, 2, size=(2.0, 2.0), shape="tri3")
    app.execute("mesh.extrude", direction=[0, 0, 1.0], delete_source=True)
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"wedge6": 8} and s["volume"] == pytest.approx(4.0)
    app.execute("project.new")
    ids = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0], [2, 0, 0]])["ids"]
    app.execute("mesh.elements_create", shape="line2", connectivity=[ids[:2], ids[1:]])
    app.execute("mesh.extrude", direction=[0, 2.0, 0], layers=2, delete_source=True)
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"quad4": 4} and s["area"] == pytest.approx(4.0)
    with pytest.raises(Nasa95Error) as e:  # 솔리드는 돌출할 수 없다
        block(app, 1, 1, 1, origin=(50.0, 0.0, 0.0))
        app.execute("mesh.extrude", direction=[0, 0, 1.0])
    assert e.value.code == "unsupported"


@pytest.mark.feature("MSH-16")
def test_MSH_T04_revolve(app):
    r0, r1, h = 1.0, 2.0, 1.0

    def section():
        ids = app.execute("mesh.nodes_create", coords=[[r0, 0, 0], [r1, 0, 0], [r1, 0, h], [r0, 0, h]])["ids"]
        app.execute("mesh.elements_create", shape="quad4", connectivity=[ids])

    section()
    n = 36
    r = app.execute("mesh.revolve", axis_point=[0, 0, 0], axis_direction=[0, 0, 1], angle=360.0, layers=n, delete_source=True)
    assert r["elements"] == n and info(app) == (4 * n, n)  # 닫힌 회전: 마지막 층이 처음 노드에 이어진다
    exact = math.pi * (r1**2 - r0**2) * h
    assert volume(app) == pytest.approx(exact, rel=0.01)
    assert app.execute("mesh.free_faces")["count"] == 4 * n
    healthy(app)
    app.execute("project.new")
    section()
    app.execute("mesh.revolve", axis_point=[0, 0, 0], axis_direction=[0, 0, 1], angle=90.0, layers=9, delete_source=True)
    assert info(app) == (40, 9) and volume(app) == pytest.approx(exact / 4, rel=0.01)
    before = total(app)
    for params, code in [
        (dict(angle=0.0), "out_of_range"),
        (dict(angle=400.0), "out_of_range"),
        (dict(angle=360.0, layers=2), "out_of_range"),
        (dict(angle=90.0, axis_direction=[0, 0, 0]), "out_of_range"),
    ]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.revolve", **{"axis_point": [0, 0, 0], "axis_direction": [0, 0, 1], **params})
        assert e.value.code == code, params
    assert total(app) == before
    # 회전축 위에 노드가 있으면 거부
    app.execute("project.new")
    plate(app, 1, 1, size=(1.0, 1.0))
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.revolve", axis_point=[0, 0, 0], axis_direction=[0, 1, 0], angle=90.0)
    assert e.value.code == "unsupported"


# ================================================================ 차수 변환 (MSH-12)
@pytest.mark.feature("MSH-12")
def test_MSH_T04_convert_order(app):
    block(app, 2, 2, 1, size=(2.0, 2.0, 1.0))
    app.execute("mesh.set_element_type", type="C3D8R")
    before, hist = total(app), history_len(app)
    r = app.execute("mesh.convert_order", order=2)
    # 노드 18 + 변 33 (공유하는 변의 중간 절점은 하나)
    assert r["elements"] == 4 and r["nodes_added"] == 33 and info(app) == (51, 4)
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"hex20": 4} and s["by_type"] == {"C3D20R": 4} and s["volume"] == pytest.approx(4.0)
    e1 = app.execute("mesh.elements", ids=[1])[0]["nodes"]
    xyz = dict(zip(app.mesh.node_ids().tolist(), app.mesh.node_coords()))
    assert np.allclose(xyz[e1[8]], (xyz[e1[0]] + xyz[e1[1]]) / 2)  # 9번째 절점은 1-2 변의 중점
    assert np.allclose(xyz[e1[16]], (xyz[e1[0]] + xyz[e1[4]]) / 2)  # 17번째 절점은 1-5 변의 중점
    healthy(app)
    assert app.execute("mesh.convert_order", order=2)["elements"] == 0  # 이미 2차
    r = app.execute("mesh.convert_order", order=1)
    assert r["nodes_removed"] == 33 and info(app) == (18, 4)
    assert app.execute("mesh.statistics")["by_type"] == {"C3D8R": 4}
    while history_len(app) > hist:
        app.undo()
    assert total(app) == before


@pytest.mark.feature("MSH-12")
def test_MSH_T04_convert_order_shapes(app):
    ids = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 0]])["ids"]
    app.execute("mesh.elements_create", shape="tet4", connectivity=[ids[:4]])
    app.execute("mesh.elements_create", shape="tri3", connectivity=[[ids[1], ids[4], ids[2]]])
    app.execute("mesh.elements_create", shape="line2", connectivity=[[ids[0], ids[1]]])
    app.execute("mesh.elements_create", shape="pyramid5", connectivity=[ids])
    app.execute("mesh.convert_order", order=2)
    shapes = [e["shape"] for e in app.execute("mesh.elements")]
    assert shapes == ["tet10", "tri6", "line3", "pyramid5"]  # 2차형이 없는 형상은 그대로
    line = app.execute("mesh.elements", ids=[3])[0]["nodes"]
    assert [line[0], line[2]] == [ids[0], ids[1]]  # 끝-가운데-끝
    tet = app.execute("mesh.elements", ids=[1])[0]["nodes"]
    assert line[1] == tet[4]  # 사면체와 선이 같은 변의 중간 절점을 함께 쓴다
    app.execute("mesh.convert_order", order=1)
    assert [e["shape"] for e in app.execute("mesh.elements")] == ["tet4", "tri3", "line2", "pyramid5"]
    assert info(app)[0] == 5
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.convert_order", order=3)
    assert e.value.code == "out_of_range"


# ================================================================ 분할·결합·세분화 (MSH-14)
@pytest.mark.feature("MSH-14")
def test_MSH_T04_split_combine(app):
    block(app, 2, 2, 2, size=(2.0, 2.0, 2.0))
    before = total(app)
    r = app.execute("mesh.elements_split")
    assert (r["removed"], r["created"]) == (8, 48)
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"tet4": 48} and s["volume"] == pytest.approx(8.0) and s["nodes"] == 27
    healthy(app)
    app.undo()
    assert total(app) == before
    app.execute("project.new")
    plate(app, 2, 1, size=(2.0, 1.0))
    app.execute("mesh.elements_split", ids=[1])
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"quad4": 1, "tri3": 2} and s["area"] == pytest.approx(2.0)
    tris = app.execute("mesh.find", what="elements", shape="tri3")["ids"]
    q = app.execute("mesh.elements_combine", ids=tris)["id"]
    s = app.execute("mesh.statistics")
    assert s["by_shape"] == {"quad4": 2} and s["area"] == pytest.approx(2.0)
    assert sorted(app.execute("mesh.elements", ids=[q])[0]["nodes"]) == [1, 2, 4, 5]
    assert app.execute("mesh.check")["inconsistent_normals"] == []
    for ids, code in [([q], "invalid_param_type"), ([q, 2], "unsupported")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.elements_combine", ids=ids)
        assert e.value.code == code


@pytest.mark.feature("MSH-14")
def test_MSH_T04_refine(app):
    block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    app.execute("mesh.set_element_type", type="C3D8R")
    before = total(app)
    r = app.execute("mesh.refine")
    assert (r["removed"], r["created"], r["partial"]) == (2, 16, False)
    s = app.execute("mesh.statistics")
    assert s["nodes"] == 5 * 3 * 3 and s["by_type"] == {"C3D8R": 16} and s["volume"] == pytest.approx(2.0)
    assert app.execute("mesh.free_faces")["count"] == 4 * 10  # 안쪽에 틈이 없다
    healthy(app)
    app.undo()
    assert total(app) == before
    assert app.execute("mesh.refine", ids=[1])["partial"] is True
    for shape, count, nodes in [("quad4", 4, 9), ("tri3", 8, 9)]:
        app.execute("project.new")
        plate(app, 1, 1, size=(1.0, 1.0), shape=shape)
        assert app.execute("mesh.refine")["created"] == count
        s = app.execute("mesh.statistics")
        assert s["nodes"] == nodes and s["area"] == pytest.approx(1.0)
        assert app.execute("mesh.free_edges")["count"] == 8 and app.execute("mesh.check")["inconsistent_normals"] == []
    app.execute("project.new")
    ids = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]])["ids"]
    app.execute("mesh.elements_create", shape="tet4", connectivity=[ids])
    assert app.execute("mesh.refine")["created"] == 8
    assert volume(app) == pytest.approx(1 / 6) and info(app) == (10, 8)
    assert app.execute("mesh.free_faces")["count"] == 16
    healthy(app)
    app.execute("mesh.convert_order", order=2)
    with pytest.raises(Nasa95Error) as e:
        app.execute("mesh.refine")
    assert e.value.code == "unsupported"


# ================================================================ 연결 요소·네트워크 (MSH-21, MSH-32, MSH-33)
@pytest.mark.feature("MSH-21")
@pytest.mark.feature("MSH-32")
@pytest.mark.feature("MSH-33")
def test_MSH_T08_connectors(app):
    ids = app.execute("mesh.nodes_create", coords=[[0, 0, 0], [1, 0, 0], [2, 0, 0]])["ids"]
    want = {"spring": ("line2", "SPRINGA"), "spring_fixed_direction": ("line2", "SPRING2"), "dashpot": ("line2", "DASHPOTA"),
            "gap": ("line2", "GAPUNI"), "spring_to_ground": ("point1", "SPRING1"), "mass": ("point1", "MASS"),
            "coupling": ("point1", "DCOUP3D")}
    for kind, (shape, etype) in want.items():
        nodes = ids[:2] if shape == "line2" else ids[:1]
        eid = app.execute("mesh.create_connector", kind=kind, nodes=nodes)["ids"][0]
        e = app.execute("mesh.elements", ids=[eid])[0]
        assert (e["shape"], e["type"], e["nodes"]) == (shape, etype, nodes)
    before = total(app)
    for params, code in [(dict(kind="spring", nodes=ids[:1]), "invalid_param_type"),
                         (dict(kind="mass", nodes=ids[:2]), "invalid_param_type"),
                         (dict(kind="rope", nodes=ids[:2]), "out_of_range"),
                         (dict(kind="spring", nodes=[1, 99]), "not_found")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.create_connector", **params)
        assert e.value.code == code, params
    assert total(app) == before
    hist = history_len(app)
    r = app.execute("mesh.create_network", connectivity=[[0, ids[0], ids[1]], [ids[1], ids[2], 0]])
    assert history_len(app) == hist + 1
    assert [e["type"] for e in app.execute("mesh.elements", ids=r["ids"])] == ["D", "D"]


@pytest.mark.feature("MSH-36")
@pytest.mark.feature("MSH-20")
def test_MSH_T08_normals_and_beam_direction(app):
    plate(app, 1, 1, size=(1.0, 1.0))
    assert app.execute("mesh.set_normal", entries=[[1, 1, 0.0, 0.0, 1.0], [1, 2, 0.0, 1.0, 1.0]])["count"] == 2
    assert app.execute("mesh.set_normal", entries=[[1, 1, 1.0, 0.0, 0.0]])["count"] == 2  # 같은 (요소, 절점)은 바뀐다
    for entries, code in [([[1, 9, 0, 0, 1]], "not_found"), ([[1, 1, 0, 0, 0]], "out_of_range"), ([[7, 1, 0, 0, 1]], "not_found")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.set_normal", entries=entries)
        assert e.value.code == code
    assert app.execute("mesh.set_normal", entries=[])["count"] == 0
    beam = app.execute("mesh.elements_create", shape="line2", connectivity=[[1, 2]])["ids"][0]  # x 방향
    before = total(app)
    app.execute("mesh.set_beam_direction", ids=[beam], direction=[0, 0, 5.0])
    assert total(app) != before
    for params, code in [(dict(ids=[beam], direction=[1, 0, 0]), "invalid_geometry"),  # 축과 평행
                         (dict(ids=[1], direction=[0, 0, 1]), "unsupported")]:  # 보가 아님
        with pytest.raises(Nasa95Error) as e:
            app.execute("mesh.set_beam_direction", **params)
        assert e.value.code == code
    app.undo()
    assert total(app) == before


# ================================================================ 번호 (CMN-06, MSH-23)
@pytest.mark.feature("CMN-06")
@pytest.mark.feature("MSH-23")
def test_SYS_renumber(app):
    block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    nset = app.model.sets.create_node(name="N", ids=[1, 12])
    eset = app.model.sets.create_element(name="E", ids=[2])
    surf = app.model.sets.create_surface(name="S", faces=[[2, 4]])
    bc = app.model.cases.create().steps.create_static().bcs.create_displacement(target={"type": "nodes", "ids": [1, 2]}, dofs=[1])
    xyz = app.mesh.node_coords().copy()
    conn = app.execute("mesh.elements", ids=[2])[0]["nodes"]
    before = total(app)
    r = app.execute("id.renumber", what="nodes", start=1001)
    assert (r["count"], r["first"], r["last"]) == (12, 1001, 1012)
    assert app.mesh.node_ids().tolist() == list(range(1001, 1013)) and np.array_equal(app.mesh.node_coords(), xyz)
    assert app.execute("mesh.elements", ids=[2])[0]["nodes"] == [n + 1000 for n in conn]
    assert nset.props["ids"] == [1001, 1012] and bc.props["target"]["ids"] == [1001, 1002]
    assert eset.props["ids"] == [2]  # 요소 번호는 그대로
    app.execute("id.renumber", what="elements", start=500)
    assert app.mesh.element_ids().tolist() == [500, 501]
    assert eset.props["ids"] == [501] and surf.props["faces"] == [[501, 4]]
    assert nset.props["ids"] == [1001, 1012]
    assert volume(app) == pytest.approx(2.0)
    c = app.execute("id.check")
    assert c["missing"] == [] and c["nodes"] == {"count": 12, "min": 1001, "max": 1012, "contiguous": True}
    app.undo(), app.undo()
    assert total(app) == before
    # 일부만: 번호가 겹치면 거부
    for params, code in [(dict(what="nodes", start=2, ids=[1]), "name_conflict"),
                         (dict(what="nodes", start=0), "out_of_range"),
                         (dict(what="nodes", start=50, ids=[99]), "not_found"),
                         (dict(what="faces", start=1), "out_of_range")]:
        with pytest.raises(Nasa95Error) as e:
            app.execute("id.renumber", **params)
        assert e.value.code == code, params
    assert total(app) == before
    with pytest.raises(Nasa95Error) as e:  # 1→2, 2→3: 3 이 이미 있다
        app.execute("id.renumber", what="nodes", start=2, ids=[1, 2])
    assert e.value.code == "name_conflict" and total(app) == before
    app.execute("id.renumber", what="nodes", start=12, ids=[11, 12])  # 11→12, 12→13: 옮기는 번호끼리는 겹쳐도 된다
    assert app.mesh.node_ids().tolist() == list(range(1, 11)) + [12, 13] and nset.props["ids"] == [1, 13]


@pytest.mark.feature("CMN-06")
def test_SYS_id_check_missing(app):
    block(app, 1, 1, 1)
    s = app.model.sets.create_node(name="N", ids=[1, 77])
    app.model.sets.create_element(name="E", ids=[1, 5])
    missing = app.execute("id.check")["missing"]
    assert [(m["what"], m["id"]) for m in missing] == [("node", 77), ("element", 5)] and missing[0]["object"] == s.id


# ================================================================ 대상 전개 (LOD-14, BC-01)
@pytest.mark.feature("LOD-14")
@pytest.mark.feature("BC-01")
def test_LOD_BC_resolve(app):
    part = app.model.mesh_parts.create(name="P")
    b = block(app, 2, 1, 1, size=(2.0, 1.0, 1.0), part=part.id)
    step = app.model.cases.create().steps.create_static()
    left = sorted(b["node"](0, j, k) for j in range(2) for k in range(2))
    surf = app.model.sets.create_surface(name="left", faces=[[1, 6]])  # 육면체 6번 면 = 4-8-5-1 (x=0)
    nset = app.model.sets.create_node(name="n", ids=[3, 1])
    eset = app.model.sets.create_element(name="e", ids=[2])
    cases = [
        (step.bcs.create_displacement(target={"type": "set", "ids": [surf.id]}, dofs=[1]), "nodes", left),
        (step.bcs.create_displacement(target={"type": "set", "ids": [nset.id]}, dofs=[1]), "nodes", [1, 3]),
        (step.loads.create_force(target={"type": "nodes", "ids": [5, 2, 5]}, components=[1.0, 0, 0]), "nodes", [2, 5]),
        (step.loads.create_force(target={"type": "set", "ids": [surf.id]}, components=[1.0, 0, 0]), "nodes", left),
        (step.loads.create_pressure(target={"type": "set", "ids": [surf.id]}, value=1.0), "faces", [[1, 6]]),
        (step.loads.create_pressure(target={"type": "faces", "ids": [[2, 4], [1, 6]]}, value=1.0), "faces", [[1, 6], [2, 4]]),
        (step.loads.create_gravity(target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0, 0, -1]), "elements", [1, 2]),
        (step.loads.create_gravity(target={"type": "set", "ids": [eset.id]}, value=9810.0, direction=[0, 0, -1]), "elements", [2]),
    ]
    for obj, what, want in cases:
        r = app.execute(f"{obj.kind}.resolve", id=obj.id)
        assert (r["what"], r["count"], r[what]) == (what, len(want), want), obj.props
    bad = step.loads.create_pressure(target={"type": "faces", "ids": [[1, 7]]}, value=1.0)
    with pytest.raises(Nasa95Error) as e:
        app.execute("load.resolve", id=bad.id)
    assert e.value.code == "out_of_range"
    with pytest.raises(Nasa95Error) as e:
        app.execute("bc.resolve", id=bad.id)
    assert e.value.code == "wrong_kind"
    geo_part = app.model.parts.create(name="G")
    geo = step.loads.create_force(target={"type": "geometry", "ids": [[geo_part.id, "face", 1]]}, components=[1.0, 0, 0])
    with pytest.raises(Nasa95Error) as e:  # 메싱하지 않은 형상은 풀 수 없다
        app.execute("load.resolve", id=geo.id)
    assert e.value.code == "not_available"


# ================================================================ 구속 검사: 미구속 강체 운동 (BC-14)
@pytest.mark.feature("BC-14")
def test_BC_T03_rigid_body_motion(app):
    b = block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    step = app.model.cases.create().steps.create_static()

    def free():
        hits = [i for i in app.execute("bc.check", id=step.id) if i["code"] == "rigid_body_motion"]
        assert len(hits) <= 1 and all(h["severity"] == "warning" and h["object"] == step.id for h in hits)
        return int(hits[0]["message"].split("강체 운동 ")[1][0]) if hits else 0

    n = b["node"]
    one = step.bcs.create_displacement(name="a", target={"type": "nodes", "ids": [n(0, 0, 0)]}, dofs=[1, 2, 3])
    assert free() == 3  # 한 점 고정: 회전 3개가 남는다
    two = step.bcs.create_displacement(name="b", target={"type": "nodes", "ids": [n(2, 0, 0)]}, dofs=[2, 3])
    assert free() == 1  # x 축 둘레 회전이 남는다
    three = step.bcs.create_displacement(name="c", target={"type": "nodes", "ids": [n(0, 1, 0)]}, dofs=[3])
    assert free() == 0  # 3-2-1 구속
    for bc in (one, two, three):
        bc.delete()
    # 한 방향만 구속한 면: 그 방향 병진과 면 밖 회전 2개만 막는다
    left = app.model.sets.create_surface(name="left", faces=[[1, 6]])
    face = step.bcs.create_displacement(name="face", target={"type": "set", "ids": [left.id]}, dofs=[1])
    assert free() == 3
    # 대칭 3면 = 병진 3개와 회전 3개를 모두 막는다
    face.delete()
    for axis, ids in [("x", [n(0, j, k) for j in range(2) for k in range(2)]),
                      ("y", [n(i, 0, k) for i in range(3) for k in range(2)]),
                      ("z", [n(i, j, 0) for i in range(3) for j in range(2)])]:
        step.bcs.create_symmetry(name=axis, target={"type": "nodes", "ids": ids}, normal=axis)
    assert free() == 0
    # 한 줄 위의 노드를 모두 고정: 그 줄 둘레 회전이 남는다
    app.execute("project.new")
    b = block(app, 2, 1, 1, size=(2.0, 1.0, 1.0))
    step = app.model.cases.create().steps.create_static()
    line = step.bcs.create_displacement(target={"type": "nodes", "ids": [b["node"](i, 0, 0) for i in range(3)]}, dofs=[1, 2, 3])
    assert free() == 1
    line.update(csys=app.model.csys.create_rectangular().id)  # 국부 좌표계가 있으면 판정하지 않는다
    assert free() == 0


# ================================================================ 합력 (LOD-17)
@pytest.mark.feature("LOD-17")
def test_LOD_resultant(app):
    part = app.model.mesh_parts.create(name="P")
    b = block(app, 4, 2, 2, size=(100.0, 20.0, 10.0), part=part.id)
    mat = steel(app)
    app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [part.id]})
    step = app.model.cases.create().steps.create_static()
    assert app.execute("load.resultant", id=step.id) == {"force": [0, 0, 0], "moment": [0, 0, 0], "terms": [], "unsupported": []}
    # 끝 면(x=100)의 노드 9개에 힘
    tip = [b["node"](4, j, k) for j in range(3) for k in range(3)]
    step.loads.create_force(target={"type": "nodes", "ids": tip}, components=[0.0, 0.0, -10.0])
    r = app.execute("load.resultant", id=step.id)
    assert r["force"] == pytest.approx([0, 0, -90.0])
    assert r["moment"] == pytest.approx([-90.0 * 10.0, 90.0 * 100.0, 0.0])  # r × F, 작용점 (100, 10, 5)
    # 윗면(z=10) 전체에 압력 2: 아래로 2 × 100 × 20
    top = [[e, 2] for e in app.execute("mesh.find", what="elements", box_min=[0, 0, 5], box_max=[100, 20, 10])["ids"]]
    p = step.loads.create_pressure(target={"type": "faces", "ids": top}, value=2.0)
    r = app.execute("load.resultant", id=step.id)
    assert r["force"] == pytest.approx([0, 0, -90.0 - 4000.0])
    assert r["terms"][1]["id"] == p.id and r["terms"][1]["force"] == pytest.approx([0, 0, -4000.0])
    assert r["terms"][1]["moment"] == pytest.approx([-4000.0 * 10.0, 4000.0 * 50.0, 0.0])
    # 닫힌 면 전체에 압력: 합력 0
    s2 = app.model.cases.create(name="c2").steps.create_static()
    s2.loads.create_pressure(target={"type": "faces", "ids": app.execute("mesh.free_faces")["faces"]}, value=7.0)
    r = app.execute("load.resultant", id=s2.id)
    assert r["force"] == pytest.approx([0, 0, 0], abs=1e-9) and r["moment"] == pytest.approx([0, 0, 0], abs=1e-6)
    # 중력: 질량 × g, 작용점은 무게 중심
    g = s2.loads.create_gravity(target={"type": "parts", "ids": [part.id]}, value=9810.0, direction=[0, 0, -2])
    w = 7.85e-9 * 20000.0 * 9810.0
    r = app.execute("load.resultant", id=s2.id)
    assert r["force"] == pytest.approx([0, 0, -w]) and r["moment"] == pytest.approx([-w * 10.0, w * 50.0, 0.0])
    # 억제한 하중은 빠진다. 계산할 수 없는 하중은 unsupported 로 알린다
    g.suppress()
    assert app.execute("load.resultant", id=s2.id)["force"] == pytest.approx([0, 0, 0], abs=1e-9)
    c = s2.loads.create_centrifugal(target={"type": "parts", "ids": [part.id]}, omega=10.0, axis_point=[0, 0, 0], axis_direction=[0, 0, 1])
    assert app.execute("load.resultant", id=s2.id)["unsupported"] == [c.id]
    with pytest.raises(Nasa95Error) as e:
        app.execute("load.resultant", id=mat.id)
    assert e.value.code == "wrong_kind"


# ================================================================ 프로퍼티 할당 현황 (PRP-14)
@pytest.mark.feature("PRP-14")
def test_PRP_T02_assignments_with_mesh(app):
    p1 = app.model.mesh_parts.create(name="A")
    block(app, 2, 1, 1, part=p1.id)
    shell = plate(app, 1, 1)["elements"][0]
    mat = steel(app)
    r = app.execute("property.assignments")
    assert r["unassigned"] == [1, 2, shell] and r["assigned"] == 0
    solid = app.model.properties.create_solid(material=mat.id, target={"type": "parts", "ids": [p1.id]})
    r = app.execute("property.assignments")
    assert r["unassigned"] == [shell] and r["mismatched"] == [] and [a["id"] for a in r["assignments"]] == [solid.id]
    bad = app.model.properties.create_solid(name="bad", material=mat.id, target={"type": "elements", "ids": [shell]})
    r = app.execute("property.assignments")
    assert r["unassigned"] == [] and r["mismatched"] == [{"element": shell, "property": bad.id}]
    bad.suppress()
    assert app.execute("property.assignments")["unassigned"] == [shell]


# ================================================================ 모든 메시 연산의 Undo·Redo (ALL-04)
@pytest.mark.feature("CMN-08")
def test_ALL_04_mesh_ops_undo_redo(app):
    plate(app, 2, 2, size=(2.0, 2.0))
    app.model.sets.create_node(name="N", ids=[1, 2, 3])
    app.model.sets.create_element(name="E", ids=[1, 2])
    steps = [
        ("mesh.extrude", dict(direction=[0, 0, 1.0], layers=2)),
        ("mesh.transform", dict(ids=[5, 6], translate=[10.0, 0, 0], copy=True)),
        ("mesh.transform", dict(mirror_normal=[0, 1, 0])),
        ("mesh.convert_order", dict(order=2)),
        ("mesh.convert_order", dict(order=1)),
        ("mesh.elements_split", dict(ids=[1, 5])),
        ("mesh.refine", dict(ids=[2])),
        ("id.renumber", dict(what="nodes", start=5000)),
        ("id.renumber", dict(what="elements", start=300)),
        ("mesh.merge_nodes", dict(tolerance=0.6)),
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
    while history_len(app):
        app.undo()
    assert info(app) == (0, 0)


@pytest.mark.feature("API-08")
def test_SYS_06_journal_mesh_ops(app, tmp_path):
    path = str(tmp_path / "ops.journal")
    app.execute("journal.start", path=path)
    plate(app, 2, 2, size=(2.0, 2.0))
    app.execute("mesh.extrude", direction=[0, 0, 1.0], layers=2, delete_source=True)
    app.execute("mesh.convert_order", order=2)
    app.execute("id.renumber", what="nodes", start=100)
    app.execute("journal.stop")
    other = App()
    other.execute("journal.replay", path=path)
    assert total(other) == total(app)
