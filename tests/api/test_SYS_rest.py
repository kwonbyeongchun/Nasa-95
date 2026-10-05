"""내장 웹 서버(REST, 9단계): 켜고 끄기, 토큰 인증, 자원 조회, 명령 실행, 명세, 기록, 접속 범위.

케이스 정의: .agent/tests/tc-00-system.md (SYS-16~18).
"""
import json
import urllib.error
import urllib.parse
import urllib.request

import pytest

from nasa95 import App, Nasa95Error

from conftest import history_len, total


def call(base, token, method, path, body=None):
    req = urllib.request.Request(base + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"} if token else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


@pytest.fixture
def server(app):
    st = app.execute("server.start", port=0)  # 빈 포트
    token = app.execute("server.token_create")["token"]
    yield st["url"], token
    app.execute("server.stop")


@pytest.mark.feature("API-26")
@pytest.mark.feature("API-34")
@pytest.mark.feature("API-35")
def test_SYS_16_start_stop_auth(app, server):
    base, token = server
    st = app.execute("server.status")
    assert st["running"] and st["host"] == "127.0.0.1" and base.endswith("/api") and st["port"] > 0
    assert st["port"] != 8765  # port=0 은 빈 포트를 고른다(기본 8765 가 아니다)
    # 같은 포트에 두 번째 서버를 열 수 없다(Windows 는 주소 재사용을 켜면 조용히 bind 되어 요청을 가로챈다): 구조화된 오류
    from nasa95.api import App as _App
    other = _App()
    with pytest.raises(Nasa95Error) as e:
        other.execute("server.start", port=st["port"])
    assert e.value.code == "port_in_use" and e.value.details["port"] == st["port"]
    assert call(base, None, "GET", "/version")[0] == 401  # 토큰 없음
    assert call(base, "wrong", "GET", "/version")[0] == 401
    status, body = call(base, token, "GET", "/version")
    assert status == 200 and body["version"] == app.version
    assert call(base, token, "GET", "/nothing")[0] == 404 and call(base, token, "GET", "/../x")[0] == 404
    app.execute("server.token_revoke", token=token)
    assert call(base, token, "GET", "/version")[0] == 401
    with pytest.raises(Nasa95Error) as e:  # 두 번 켤 수 없다
        app.execute("server.start")
    assert e.value.code == "invalid_state"
    app.execute("server.stop")
    assert not app.execute("server.status")["running"]
    with pytest.raises(Exception):
        call(base, token, "GET", "/version")
    app.execute("server.configure", port=0, allow_script=True)
    assert app.execute("server.status")["allow_script"] is True


@pytest.mark.feature("API-27")
@pytest.mark.feature("API-28")
@pytest.mark.feature("API-37")
def test_SYS_17_resources_and_commands(app, server):
    base, token = server
    status, r = call(base, token, "POST", "/commands/material.create", {"name": "STEEL"})
    assert status == 200 and r["id"] == 1 and history_len(app) == 1  # REST 로 실행한 명령도 Undo 이력에 들어간다
    status, r = call(base, token, "POST", "/commands/material.set_elastic", {"id": 1, "data": [[210000.0, 0.3]]})
    assert status == 200
    status, lst = call(base, token, "GET", "/materials")
    assert status == 200 and [m["name"] for m in lst] == ["STEEL"]
    status, one = call(base, token, "GET", "/materials/1")
    assert status == 200 and one["props"]["behaviors"]["elastic"]["data"] == [[210000.0, 0.3]]
    assert call(base, token, "GET", "/materials/99")[0] == 404
    assert call(base, token, "GET", "/tree")[1][0]["kind"] == "part"
    assert call(base, token, "GET", "/digest")[1] == app.digest()
    assert call(base, token, "GET", "/info")[1]["objects"] == 1
    # 오류는 코드와 함께 JSON 으로
    status, err = call(base, token, "POST", "/commands/material.create", {"name": "이름 규칙 위반"})
    assert status == 400 and err["code"] == "invalid_name" and err["details"]["param"] == "name"
    assert call(base, token, "POST", "/commands/no.such", {})[0] == 404
    status, err = call(base, token, "POST", "/commands/script.run", {"path": "x.py"})
    assert status == 403 and err["code"] == "forbidden"  # 스크립트 실행은 따로 허용해야 한다
    before = total(app)
    status, err = call(base, token, "POST", "/commands/material.create", {"name": "A", "bogus": 1})
    assert status == 400 and total(app) == before
    # 기록
    log = app.execute("server.log", last=5)
    assert log[-1]["path"] == "/api/commands/material.create" and log[-1]["status"] == 400 and log[-1]["client"] == "127.0.0.1"
    assert any(entry["command"] == "material.set_elastic" for entry in app.execute("server.log"))
    assert app.execute("server.status")["calls"] == len(app.execute("server.log"))


@pytest.mark.feature("API-29")
def test_SYS_18_openapi(app, server):
    base, token = server
    status, spec = call(base, token, "GET", "/openapi")
    assert status == 200 and spec["openapi"].startswith("3.") and spec["info"]["title"] == "NASA-95"
    paths = spec["paths"]
    names = {c["name"] for c in app.commands()}
    assert all(f"/api/commands/{n}" in paths for n in names)  # 모든 명령이 명세에 있다
    post = paths["/api/commands/load.create_force"]["post"]
    schema = post["requestBody"]["content"]["application/json"]["schema"]
    assert schema["properties"]["components"]["type"] == "array" and "parent" in schema["required"]
    assert paths["/api/materials"]["get"] and paths["/api/materials/{id}"]["get"]
    assert spec == app.execute("server.openapi")


@pytest.mark.feature("API-33")
def test_SYS_19_files(app, server, tmp_path):
    """파일 올리기·내려받기: 올린 파일의 경로를 명령에 넘겨 쓰고, 명령이 만든 파일을 내려받는다."""
    base, token = server
    app.execute("server.stop")
    app.execute("server.configure", upload_dir=str(tmp_path / "올림"))
    st = app.execute("server.start", port=0)
    base = st["url"]
    deck = "*NODE, NSET=NALL\n1, 0., 0., 0.\n2, 1., 0., 0.\n3, 0., 1., 0.\n4, 0., 0., 1.\n*ELEMENT, TYPE=C3D4, ELSET=E1\n1, 1, 2, 3, 4\n"
    req = urllib.request.Request(base + "/files/tet.inp", method="POST", data=deck.encode(), headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=10) as r:
        up = json.loads(r.read())
    assert up["name"] == "tet.inp" and up["bytes"] == len(deck.encode()) and up["path"].startswith(str(tmp_path / "올림"))
    status, r = call(base, token, "POST", "/commands/mesh.import", {"path": up["path"]})
    assert status == 200 and r["elements"] == 1
    status, r = call(base, token, "POST", "/commands/mesh.export", {"path": str(tmp_path / "out.inp")})
    assert status == 200
    req = urllib.request.Request(base + "/files/x?path=" + urllib.parse.quote(str(tmp_path / "out.inp")), headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = r.read().decode()
        assert r.headers["Content-Type"] == "application/octet-stream" and "*ELEMENT" in body
    req = urllib.request.Request(base + "/files/tet.inp", headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=10) as r:
        assert r.read().decode() == deck
    assert call(base, token, "GET", "/files/nothing.inp")[0] == 404
    assert call(base, token, "POST", "/files/..", {})[0] == 400  # 폴더 밖으로 못 나간다
    assert call(base, None, "POST", "/files/a.inp", {})[0] == 401
    assert any(e["command"] == "file.upload" for e in app.execute("server.log")) and app.execute("server.status")["files_dir"] == str(tmp_path / "올림")


@pytest.mark.feature("API-31")
@pytest.mark.feature("API-22")
def test_SYS_19_event_stream(app, server):
    """변경 통지 스트림(SSE): 명령이 모델을 바꾸면 구독자에게 한 묶음씩 온다."""
    import threading
    base, token = server
    got = []
    ready = threading.Event()

    def listen():
        req = urllib.request.Request(base + "/events", headers={"Authorization": f"Bearer {token}"})
        with urllib.request.urlopen(req, timeout=10) as r:
            assert r.headers["Content-Type"].startswith("text/event-stream")
            ready.set()
            for raw in r:
                line = raw.decode("utf-8").rstrip("\n")
                if line.startswith("data: "):
                    got.append(json.loads(line[6:]))
                    if len(got) >= 2:
                        break

    t = threading.Thread(target=listen, daemon=True)
    t.start()
    assert ready.wait(5)
    import time
    for _ in range(50):
        if app.execute("server.status")["event_subscribers"] == 1:
            break
        time.sleep(0.05)
    assert app.execute("server.status")["event_subscribers"] == 1
    m = app.model.materials.create(name="A")             # 직접 실행도 통지된다
    call(base, token, "POST", "/commands/material.create", {"name": "B"})  # REST 로 실행한 것도
    t.join(5)
    assert not t.is_alive() and [e["command"] for e in got] == ["material.create", "material.create"] and got[0]["created"] == [m.id]
    for _ in range(100):  # 서버는 다음에 쓸 때(3초 주기의 유지 줄) 끊긴 것을 안다
        if app.execute("server.status")["event_subscribers"] == 0:
            break
        time.sleep(0.1)
    assert app.execute("server.status")["event_subscribers"] == 0  # 끊으면 구독이 빠진다


@pytest.mark.feature("API-31")
def test_SYS_19_stop_with_open_stream(app, server):
    """스트림 구독자가 있어도 서버를 끌 수 있다(몇 초 안에)."""
    import threading, time
    base, token = server
    opened = threading.Event()

    def listen():
        req = urllib.request.Request(base + "/events", headers={"Authorization": f"Bearer {token}"})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                opened.set()
                for _ in r:
                    pass
        except Exception:
            opened.set()

    t = threading.Thread(target=listen, daemon=True)
    t.start()
    assert opened.wait(5)
    t0 = time.time()
    app.execute("server.stop")
    assert time.time() - t0 < 10 and not app.execute("server.status")["running"]


def call_raw(base, token, method, path, data=None, headers=None):
    """원시 바이트 요청. (상태, 본문 bytes, 머리글)."""
    req = urllib.request.Request(base + path, method=method, data=data,
                                 headers={"Authorization": f"Bearer {token}", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


@pytest.mark.feature("API-32")
def test_SYS_16_04_08_binary_arrays(app, server):
    """노드 좌표를 이진으로 받으면 Python 배열과 같고, 좌표 배열을 이진으로 보내 대량 생성하면 노드 수·좌표가 일치한다."""
    import numpy as np
    from meshutil import block
    base, token = server
    block(app, 3, 2, 1)
    status, body, h = call_raw(base, token, "GET", "/arrays/mesh/node_coords")
    assert status == 200 and h["X-Dtype"] == "float64" and h["X-Shape"] == "24,3" and h["Content-Type"] == "application/octet-stream"
    coords = np.frombuffer(body, dtype="<f8").reshape(24, 3)
    assert np.array_equal(coords, app.mesh.node_coords()) and coords.tolist() == app.execute("mesh.nodes")["coords"]
    status, body, h = call_raw(base, token, "GET", "/arrays/mesh/node_ids")
    assert status == 200 and h["X-Dtype"] == "uint64" and np.frombuffer(body, dtype="<u8").tolist() == list(range(1, 25))  # ID 는 부호 없는 64비트
    status, body, h = call_raw(base, token, "GET", "/arrays/mesh/element_nodes")
    assert status == 200 and np.frombuffer(body, dtype="<" + np.dtype(h["X-Dtype"]).str[1:]).tolist() == app.mesh.element_nodes()[1].tolist()
    st, body, h = call_raw(base, token, "GET", "/arrays/mesh/element_offsets")
    assert np.frombuffer(body, dtype="<" + np.dtype(h["X-Dtype"]).str[1:]).tolist() == [0, 8, 16, 24, 32, 40, 48]
    assert call_raw(base, token, "GET", "/arrays/mesh/bogus")[0] == 404 and call_raw(base, token, "GET", "/arrays/nope/x")[0] == 404
    # 이진 올리기(SYS-16-08): 노드 2000개
    xyz = np.random.default_rng(1).random((2000, 3)) * 100.0
    status, body, h = call_raw(base, token, "POST", "/commands/mesh.nodes_create?array=coords&dtype=float64&shape=2000,3",
                               data=xyz.astype("<f8").tobytes(), headers={"Content-Type": "application/octet-stream"})
    assert status == 200, body
    r = json.loads(body)
    assert r["count"] == 2000 and app.execute("project.info")["nodes"] == 2024
    assert np.allclose(app.mesh.node_coords()[24:], xyz) and history_len(app) == 3  # block 의 노드·요소 생성 2 + 이진 올리기 1
    status, body, _ = call_raw(base, token, "POST", "/commands/mesh.nodes_create?array=coords&dtype=float64&shape=3,3",
                               data=b"\x00" * 16, headers={"Content-Type": "application/octet-stream"})
    assert status == 400 and json.loads(body)["code"] == "invalid_param_type"
    assert app.execute("project.info")["nodes"] == 2024
    # 결과 배열은 결과가 없으면 오류로
    assert call_raw(base, token, "GET", "/arrays/results/values?result=1&frame=1&field=DISP")[0] in (400, 404)


@pytest.mark.feature("API-36")
def test_SYS_17_05_06_token_scope(app, server):
    """읽기 전용 토큰은 조회(Q)만 되고 변경·작업·파일 올리기는 403. 스크립트 실행은 허용하지 않은 토큰이면 403."""
    base, token = server
    ro = app.execute("server.token_create", scope="read")
    assert ro["scope"] == "read"
    with pytest.raises(Nasa95Error) as e:
        app.execute("server.token_create", scope="admin")
    assert e.value.code == "out_of_range"
    app.model.materials.create(name="STEEL")
    assert call(base, ro["token"], "GET", "/materials")[0] == 200 and call(base, ro["token"], "GET", "/materials/1")[0] == 200
    assert call(base, ro["token"], "POST", "/commands/material.get", {"id": 1})[0] == 200
    status, err = call(base, ro["token"], "POST", "/commands/material.create", {"name": "X"})
    assert status == 403 and err["code"] == "forbidden" and err["details"]["kind"] == "C"
    assert call(base, ro["token"], "POST", "/commands/app.undo", {})[0] == 403
    assert call(base, ro["token"], "POST", "/commands/case.export_deck", {"id": 1, "path": "x.inp"})[0] == 403  # 작업(J)
    assert call_raw(base, ro["token"], "POST", "/files/a.txt", data=b"x")[0] == 403
    assert call_raw(base, ro["token"], "POST", "/commands/mesh.nodes_create?array=coords&dtype=float64&shape=1,3", data=b"\x00" * 24,
                    headers={"Content-Type": "application/octet-stream"})[0] == 403
    assert len(app.execute("material.list")) == 1
    # 스크립트 실행 권한(SYS-17-06): 전체 권한 토큰도 allow_script 가 꺼져 있으면 거부
    assert call(base, token, "POST", "/commands/script.run", {"path": "x.py"})[0] == 403
    assert call(base, ro["token"], "POST", "/commands/script.run", {"path": "x.py"})[0] == 403
    app.execute("server.token_revoke", token=ro["token"])
    assert call(base, ro["token"], "GET", "/materials")[0] == 401


@pytest.mark.feature("API-38")
@pytest.mark.feature("API-27")
def test_SYS_18_02_03_headless_server_mixed(app, server, tmp_path):
    """창 없는 서버(UI 없이 App + REST)로 같은 작업을 하면 다이제스트가 같고, Python 과 REST 를 섞어 써도 Undo 이력에 순서대로 들어간다."""
    base, token = server

    def prepare(exec_py, exec_rest):
        exec_py("material.create", {"name": "steel"})
        exec_rest("material.set_elastic", {"id": 1, "data": [[210000.0, 0.3]]})
        exec_py("property.create_solid", {"name": "solid1", "material": 1})
        exec_rest("case.create", {"name": "static"})
        exec_py("step.create_static", {"parent": 3, "name": "load-step"})
        exec_rest("load.create_force", {"parent": 4, "target": {"type": "nodes", "ids": [8]}, "components": [0.0, 0.0, -100.0]})
        exec_py("bc.create_displacement", {"parent": 4, "target": {"type": "nodes", "ids": [1]}, "dofs": [1, 2, 3]})

    def py(name, params):
        return app.execute(name, params)

    def rest(name, params):
        status, r = call(base, token, "POST", f"/commands/{name}", params)
        assert status == 200, r
        return r

    prepare(py, py)  # 전부 Python
    digest_py = app.digest()
    hist_py = [h["name"] for h in app.execute("app.history")["undo"]]
    app.execute("project.new")
    prepare(rest, rest)  # 전부 REST(창 없는 서버 = 이 테스트의 App 에는 UI 가 없다)
    assert app.digest() == digest_py and call(base, token, "GET", "/digest")[1] == digest_py
    app.execute("project.new")
    prepare(py, rest)  # 섞어 쓰기(SYS-18-03)
    assert app.digest() == digest_py
    assert [h["name"] for h in app.execute("app.history")["undo"]] == hist_py  # 순서대로
    app.execute("app.undo")
    assert app.digest() != digest_py and len(app.execute("bc.list")) == 0
    app.execute("app.redo")
    assert app.digest() == digest_py
