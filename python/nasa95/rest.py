"""내장 웹 서버(REST, API-26~37). 명령 계층의 또 하나의 호출자일 뿐이다 — 기능을 따로 구현하지 않는다.

기본: 꺼짐, 127.0.0.1, 토큰 인증. 경로는 명령 등록부와 객체 종류 정의에서 나온다.
  GET  /api/version · /api/commands · /api/openapi · /api/tree · /api/digest · /api/info
  GET  /api/<collection>            객체 목록(예: /api/materials)
  GET  /api/<collection>/<id>       객체 속성
  POST /api/commands/<name>         명령 실행(본문: JSON 매개변수)
  POST /api/files/<name>            파일 올리기(본문 = 내용. 서버의 올림 폴더에 저장, 그 경로를 돌려준다 — 덱·형상 파일을 보내 명령에 쓴다)
  GET  /api/files/<name>            파일 내려받기(올림 폴더의 파일, 또는 ?path= 로 지정한 파일)
  GET  /api/events                  변경 통지 스트림(Server-Sent Events: 모델 변경·명령 실행이 한 줄씩 온다)
  GET  /api/arrays/mesh/<name>      메시 배열을 이진으로(API-32): node_ids(int64)·node_coords(float64, N×3)·element_ids·element_offsets·element_nodes.
                                    본문 = 리틀엔디언 원시 바이트, 머리글 X-Dtype·X-Shape("12,3")
  GET  /api/arrays/results/values?result=&frame=&field=&component=   결과 값 배열(float64). 첫 열 묶음은 X-Node-Ids 가 아니라 따로 /api/arrays/results/nodes 로
  POST /api/commands/<name>?array=<매개변수>&dtype=float64&shape=N,3[&params=<JSON>]   본문(application/octet-stream)을 그 매개변수의 배열로 넘겨 실행(대량 입력)
명령 실행은 executor 로 직렬화한다(UI 에서는 GUI 스레드로 넘긴다).
"""
from __future__ import annotations

import json
import os
import queue
import re
import secrets
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable
from urllib.parse import parse_qs, urlparse

from .api import App, Nasa95Error

Executor = Callable[[str, dict], Any]


class RestServer:
    def __init__(self, app: App):
        self.app = app
        self.host = "127.0.0.1"
        self.port = 8765
        self.tokens: dict[str, str] = {}   # 토큰 → 권한 범위 "full" | "read"(조회 Q 만)
        self.allow_script = False          # script.run 은 별도 권한(API-36)
        self.log: list[dict] = []
        self.log_limit = 1000
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()
        self.executor: Executor = self._direct
        self._registry: list[dict] = []
        self._api_version = app.execute("app.version")["api_version"]
        self.upload_dir = ""                 # 비어 있으면 임시 폴더 아래 nasa95-rest
        self._subscribers: list[queue.Queue] = []  # 이벤트 스트림(SSE) 구독자마다 대기열
        self._stopping = False
        self._subscription = app.subscribe(self._on_event)
        self._register_commands()
        self.refresh_registry()

    def _on_event(self, event: dict) -> None:
        """App 통지 → 모든 스트림 구독자의 대기열(명령 실행 스레드에서 불린다. 가득 찬 대기열은 오래된 것을 버린다)."""
        for q in list(self._subscribers):
            try:
                q.put_nowait(event)
            except queue.Full:
                try:
                    q.get_nowait()
                    q.put_nowait(event)
                except queue.Empty:
                    pass

    def files_dir(self) -> str:
        d = self.upload_dir or os.path.join(tempfile.gettempdir(), "nasa95-rest")
        os.makedirs(d, exist_ok=True)
        return d

    @staticmethod
    def safe_name(name: str) -> str:
        if not name or not re.fullmatch(r"[A-Za-z0-9._\-가-힣]+", name) or name in (".", ".."):
            raise Nasa95Error("invalid_param", "파일 이름은 글자·숫자·._- 만 쓴다(폴더 없이)", {"name": name})
        return name

    def refresh_registry(self) -> None:
        """명령 등록부 사본(명령 안에서는 다른 명령을 부를 수 없으므로 명세는 사본으로 만든다)."""
        self._registry = self.app.commands()

    # ------------------------------------------------------------ 실행
    def _direct(self, name: str, params: dict):
        with self._lock:
            return self.app.execute(name, params)

    def call(self, name: str, params: dict, scope: str = "full"):
        if name == "script.run" and not self.allow_script:
            raise Nasa95Error("forbidden", "REST 로는 스크립트 실행이 막혀 있습니다(server.configure allow_script)", {"command": name})
        if scope == "read":
            kind = next((c["kind"] for c in self._registry if c["name"] == name), "")
            if kind != "Q":
                raise Nasa95Error("forbidden", "읽기 전용 토큰으로는 조회 명령(Q)만 실행할 수 있습니다", {"command": name, "kind": kind})
        return self.executor(name, params)

    # ------------------------------------------------------------ 서버
    def start(self, host: str | None = None, port: int | None = None) -> dict:
        if self._server:
            raise Nasa95Error("invalid_state", "서버가 이미 켜져 있습니다", {})
        if host:
            self.host = host
        if port is not None:
            self.port = int(port)  # 0 = 빈 포트를 고른다
        if not self.tokens:
            self.tokens[secrets.token_urlsafe(24)] = "full"
        self.refresh_registry()
        server = self
        app = self.app

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):  # 표준 오류에 쓰지 않는다
                pass

            def _send(self, status: int, body: Any):
                data = json.dumps(body, ensure_ascii=False).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def _auth(self) -> bool:
                token = self.headers.get("X-Token") or ""
                auth = self.headers.get("Authorization") or ""
                if auth.lower().startswith("bearer "):
                    token = auth[7:].strip()
                self.scope = server.tokens.get(token, "")
                return token in server.tokens

            def _record(self, status, command=None):
                server.log.append({"time": time.time(), "method": self.command, "path": self.path, "status": status,
                                   "command": command, "client": self.client_address[0]})
                del server.log[:-server.log_limit]

            def _send_bytes(self, data: bytes, dtype: str, shape: tuple, command: str):
                self._record(200, command)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("X-Dtype", dtype)
                self.send_header("X-Shape", ",".join(str(int(n)) for n in shape))
                self.end_headers()
                self.wfile.write(data)

            def _send_array(self, group: str, name: str, query: dict):
                """메시·결과 배열을 이진으로 보낸다(API-32). 리틀엔디언, C 순서."""
                import numpy as np
                if group == "mesh":
                    mesh = server.app.mesh
                    getters = {"node_ids": lambda: mesh.node_ids(), "node_coords": lambda: mesh.node_coords(),
                               "element_ids": lambda: mesh.element_ids(), "element_offsets": lambda: mesh.element_nodes()[0],
                               "element_nodes": lambda: mesh.element_nodes()[1]}
                    if name not in getters:
                        raise Nasa95Error("not_found", "없는 배열입니다: " + name, {"available": sorted(getters)})
                    with server._lock:
                        arr = np.ascontiguousarray(getters[name]())
                    command = "mesh.nodes" if name.startswith("node") else "mesh.elements"
                elif group == "results":
                    if name not in ("values", "nodes"):
                        raise Nasa95Error("not_found", "없는 배열입니다: " + name, {"available": ["values", "nodes"]})
                    try:
                        result, frame = int(query["result"][0]), int(query["frame"][0])
                        field = query["field"][0]
                    except (KeyError, ValueError):
                        raise Nasa95Error("missing_param", "result, frame, field 가 필요합니다", {"param": "result"})
                    component = query.get("component", [""])[0]
                    with server._lock:
                        nodes, values = server.app.results.values(result, frame, field, component)
                    arr = np.ascontiguousarray(nodes if name == "nodes" else values)
                    command = "result.values"
                else:
                    raise Nasa95Error("not_found", "배열 묶음은 mesh 또는 results 입니다", {"group": group})
                dtype = arr.dtype.newbyteorder("<")
                self._send_bytes(arr.astype(dtype, copy=False).tobytes(order="C"), str(np.dtype(dtype).name), arr.shape, command)

            def _command_with_array(self, name: str, query: dict):
                """본문의 원시 바이트를 배열 매개변수로 넘겨 명령을 실행한다(API-32 이진 올리기)."""
                import numpy as np
                if self.scope == "read":
                    raise Nasa95Error("forbidden", "읽기 전용 토큰으로는 조회 명령(Q)만 실행할 수 있습니다", {"command": name})
                if name not in {c["name"] for c in server._registry}:
                    raise Nasa95Error("not_found", f"없는 명령입니다: {name}", {"command": name})
                try:
                    param = query["array"][0]
                    dtype = np.dtype(query.get("dtype", ["float64"])[0]).newbyteorder("<")
                    shape = tuple(int(x) for x in query["shape"][0].split(",") if x)
                except (KeyError, ValueError, TypeError) as e:
                    raise Nasa95Error("missing_param", "array, shape(, dtype) 이 필요합니다: " + str(e), {"param": "array"})
                length = int(self.headers.get("Content-Length") or 0)
                data = self.rfile.read(length) if length else b""
                try:
                    arr = np.frombuffer(data, dtype=dtype).reshape(shape)
                except ValueError as e:
                    raise Nasa95Error("invalid_param_type", "본문 길이가 shape·dtype 과 맞지 않습니다: " + str(e), {"param": param})
                params = json.loads(query["params"][0]) if "params" in query else {}
                params[param] = np.array(arr)  # 쓰기 가능한 사본(frombuffer 는 읽기 전용)
                result = server.call(name, params, self.scope)
                self._record(200, name)
                self._send(200, result)

            def _send_file(self, path: str):
                try:
                    size = os.path.getsize(path)
                except OSError:
                    self._record(404)
                    return self._send(404, {"code": "not_found", "message": "파일이 없습니다", "details": {"path": path}})
                self._record(200, "file.download")
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Content-Disposition", 'attachment; filename="%s"' % os.path.basename(path))
                self.end_headers()
                with open(path, "rb") as f:
                    while chunk := f.read(1 << 16):
                        self.wfile.write(chunk)

            def _stream_events(self):
                """SSE: 통지마다 "data: <json>" 한 묶음. 3초마다 주석 줄로 연결을 유지한다(끊긴 연결도 그때 안다). 클라이언트가 끊으면 끝난다."""
                q: queue.Queue = queue.Queue(maxsize=1000)
                server._subscribers.append(q)
                self._record(200, "event.subscribe")
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                    self.send_header("Cache-Control", "no-cache")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(b": connected\n\n")
                    self.wfile.flush()
                    while not server._stopping:
                        try:
                            ev = q.get(timeout=3)  # 짧은 주기로 써 보아야 끊긴 연결을 안다
                            self.wfile.write(("data: " + json.dumps(ev, ensure_ascii=False) + "\n\n").encode("utf-8"))
                        except queue.Empty:
                            self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionError, OSError):
                    pass
                finally:
                    if q in server._subscribers:
                        server._subscribers.remove(q)
                self.close_connection = True

            def _handle(self, method: str):
                url = urlparse(self.path)
                parts = [p for p in url.path.split("/") if p]
                if len(parts) < 2 or parts[0] != "api":
                    self._record(404)
                    return self._send(404, {"code": "not_found", "message": "경로가 없습니다", "details": {}})
                if not self._auth():
                    self._record(401)
                    return self._send(401, {"code": "unauthorized", "message": "토큰이 필요합니다(Authorization: Bearer <token>)", "details": {}})
                try:
                    if method == "GET" and parts[1] == "events" and len(parts) == 2:
                        return self._stream_events()
                    if method == "GET" and parts[1] == "arrays" and len(parts) == 4:
                        return self._send_array(parts[2], parts[3], parse_qs(url.query))
                    if method == "POST" and parts[1] == "commands" and len(parts) == 3 and \
                            (self.headers.get("Content-Type") or "").split(";")[0].strip() == "application/octet-stream":
                        return self._command_with_array(parts[2], parse_qs(url.query))
                    if parts[1] == "files":
                        query = parse_qs(url.query)
                        if method == "POST" and len(parts) == 3:
                            if self.scope == "read":
                                raise Nasa95Error("forbidden", "읽기 전용 토큰으로는 파일을 올릴 수 없습니다", {"command": "file.upload"})
                            name = server.safe_name(parts[2])
                            length = int(self.headers.get("Content-Length") or 0)
                            data = self.rfile.read(length) if length else b""
                            path = os.path.join(server.files_dir(), name)
                            with open(path, "wb") as f:
                                f.write(data)
                            self._record(200, "file.upload")
                            return self._send(200, {"name": name, "path": path, "bytes": len(data)})
                        if method == "GET" and (len(parts) == 3 or "path" in query):
                            path = query["path"][0] if "path" in query else os.path.join(server.files_dir(), server.safe_name(parts[2]))
                            return self._send_file(path)
                    body = {}
                    length = int(self.headers.get("Content-Length") or 0)
                    if length:
                        body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
                    status, result, command = server.route(method, parts[1:], parse_qs(url.query), body, self.scope)
                    self._record(status, command)  # 응답을 보내기 전에 기록한다(보낸 뒤면 호출자가 기록을 먼저 조회할 수 있다)
                    self._send(status, result)
                except Nasa95Error as e:
                    code = e.code
                    status = 404 if code == "not_found" else 403 if code == "forbidden" else 400
                    self._record(status)
                    self._send(status, {"code": code, "message": str(e), "details": e.details})
                except (ValueError, json.JSONDecodeError) as e:
                    self._record(400)
                    self._send(400, {"code": "bad_request", "message": str(e), "details": {}})

            def do_GET(self):
                self._handle("GET")

            def do_POST(self):
                self._handle("POST")

        class Server(ThreadingHTTPServer):
            allow_reuse_address = False  # Windows 에서는 재사용을 켜면 이미 듣고 있는 포트에도 bind 가 되어 다른 서버의 요청을 가로챈다

        try:
            self._server = Server((self.host, self.port), Handler)
        except OSError as e:
            raise Nasa95Error("port_in_use", f"포트를 열 수 없습니다: {self.host}:{self.port} ({e.strerror or e})", {"host": self.host, "port": self.port}) from e
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, name="nasa95-rest", daemon=True)
        self._thread.start()
        del app
        return self.status()

    def stop(self) -> dict:
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
            self._thread = None
        return self.status()

    def status(self) -> dict:
        return {"running": self._server is not None, "host": self.host, "port": self.port, "url": f"http://{self.host}:{self.port}/api",
                "tokens": len(self.tokens), "allow_script": self.allow_script, "calls": len(self.log), "files_dir": self.files_dir(),
                "event_subscribers": len(self._subscribers)}

    # ------------------------------------------------------------ 경로
    def route(self, method: str, parts: list[str], query: dict, body: dict, scope: str = "full"):
        kinds = self.app.kinds()  # 정의는 바뀌지 않으므로 잠그지 않아도 된다
        by_collection = {spec["collection"]: kind for kind, spec in kinds.items()}
        head = parts[0]
        if method == "GET":
            if head == "openapi":
                self.refresh_registry()
                return 200, self.openapi(), "server.openapi"
            simple = {"version": "app.version", "commands": "app.commands", "tree": "project.tree", "digest": "project.digest",
                      "info": "project.info"}
            if head in simple:
                return 200, self.call(simple[head], {}, scope), simple[head]
            if head in by_collection:
                kind = by_collection[head]
                if len(parts) == 1:
                    params = {k: json.loads(v[0]) if v[0][:1] in "[{0123456789" else v[0] for k, v in query.items()}
                    return 200, self.call(f"{kind}.list", params, scope), f"{kind}.list"
                return 200, self.call(f"{kind}.get", {"id": int(parts[1])}, scope), f"{kind}.get"
        elif method == "POST" and head == "commands" and len(parts) == 2:
            name = parts[1]
            if name not in {c["name"] for c in self._registry}:
                raise Nasa95Error("not_found", f"없는 명령입니다: {name}", {"command": name})
            return 200, self.call(name, body if isinstance(body, dict) else {}, scope), name
        raise Nasa95Error("not_found", "경로가 없습니다: " + "/".join(parts), {})

    def openapi(self) -> dict:
        """명령 등록부에서 만든 OpenAPI 3 명세(API-29)."""
        types = {"number": {"type": "number"}, "integer": {"type": "integer"}, "string": {"type": "string"}, "bool": {"type": "boolean"},
                 "vector3": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                 "number_list": {"type": "array", "items": {"type": "number"}}, "integer_list": {"type": "array", "items": {"type": "integer"}},
                 "string_list": {"type": "array", "items": {"type": "string"}}, "ref": {"type": "integer"},
                 "ref_list": {"type": "array", "items": {"type": "integer"}}, "table": {"type": "array", "items": {"type": "array"}},
                 "target": {"type": "object", "properties": {"type": {"type": "string"}, "ids": {"type": "array"}}}}
        paths = {}
        for c in self._registry:
            props, required = {}, []
            for f in c["params"]:
                schema = dict(types.get(f["type"], {}))
                schema["description"] = f.get("desc", "")
                if f.get("choices"):
                    schema["enum"] = f["choices"]
                props[f["name"]] = schema
                if f.get("must"):
                    required.append(f["name"])
            body = {"type": "object", "properties": props}
            if required:
                body["required"] = required
            paths[f"/api/commands/{c['name']}"] = {"post": {"summary": c["desc"], "tags": [c["name"].split(".")[0]],
                                                            "x-kind": c["kind"], "x-features": c.get("features", ""),
                                                            "requestBody": {"content": {"application/json": {"schema": body}}},
                                                            "responses": {"200": {"description": "결과"}}}}
        for kind, spec in self.app.kinds().items():
            col = spec["collection"]
            paths[f"/api/{col}"] = {"get": {"summary": f"{spec['label']} 목록", "tags": [kind]}}
            paths[f"/api/{col}/{{id}}"] = {"get": {"summary": f"{spec['label']} 속성", "tags": [kind],
                                                   "parameters": [{"name": "id", "in": "path", "required": True, "schema": {"type": "integer"}}]}}
        paths["/api/files/{name}"] = {
            "post": {"summary": "파일 올리기(본문 = 내용)", "tags": ["file"],
                     "parameters": [{"name": "name", "in": "path", "required": True, "schema": {"type": "string"}}],
                     "requestBody": {"content": {"application/octet-stream": {"schema": {"type": "string", "format": "binary"}}}},
                     "responses": {"200": {"description": "{name, path, bytes}"}}},
            "get": {"summary": "파일 내려받기(올림 폴더의 파일, 또는 ?path=)", "tags": ["file"],
                    "parameters": [{"name": "name", "in": "path", "required": True, "schema": {"type": "string"}}],
                    "responses": {"200": {"description": "파일 내용"}}}}
        paths["/api/events"] = {"get": {"summary": "변경 통지 스트림(text/event-stream)", "tags": ["event"], "responses": {"200": {"description": "SSE"}}}}
        return {"openapi": "3.0.3", "info": {"title": "NASA-95", "version": self._api_version},
                "components": {"securitySchemes": {"token": {"type": "http", "scheme": "bearer"}}}, "security": [{"token": []}], "paths": paths}

    # ------------------------------------------------------------ 명령 등록(server.*)
    def _register_commands(self):
        app = self.app

        def register(name, kind, desc, params, features, fn):
            app.register_command(name, fn, kind=kind, desc=desc, params=params, features=features, composite=False)

        register("server.start", "S", "내장 웹 서버를 켠다(기본 127.0.0.1:8765, 토큰 인증)", [
            {"name": "host", "type": "string", "desc": "주소(기본 127.0.0.1)"}, {"name": "port", "type": "integer", "desc": "포트(0 = 빈 포트)"}],
            "API-26, API-35", lambda p: self.start(p.get("host"), p.get("port")))
        register("server.stop", "S", "내장 웹 서버를 끈다", [], "API-26", lambda p: self.stop())
        register("server.status", "S", "서버 상태(주소·포트·토큰 수·호출 수)를 조회한다", [], "API-26", lambda p: self.status())
        register("server.configure", "S", "주소·포트·스크립트 실행 허용을 설정한다(다음에 켤 때부터)", [
            {"name": "host", "type": "string", "desc": "주소. 외부 접속은 0.0.0.0 처럼 명시적으로 켠다"},
            {"name": "port", "type": "integer", "desc": "포트"},
            {"name": "allow_script", "type": "bool", "desc": "REST 로 script.run 을 허용한다(기본 꺼짐)"},
            {"name": "upload_dir", "type": "string", "desc": "올린 파일을 둘 폴더(비우면 임시 폴더 아래 nasa95-rest)"}],
            "API-35, API-36, API-33", self._configure)
        register("server.openapi", "Q", "명령 등록부에서 만든 OpenAPI 명세를 조회한다", [], "API-29", lambda p: self.openapi())
        register("server.token_create", "S", "접속 토큰을 만든다(scope: full = 전부, read = 조회 명령(Q)만)", [
            {"name": "scope", "type": "string", "desc": "권한 범위(기본 full)", "choices": ["full", "read"]}], "API-34, API-36", self._token_create)
        register("server.token_revoke", "S", "접속 토큰을 폐기한다", [{"name": "token", "type": "string", "desc": "폐기할 토큰", "must": True}],
                 "API-34", self._token_revoke)
        register("server.log", "Q", "REST 호출 기록을 조회한다", [{"name": "last", "type": "integer", "desc": "끝에서 몇 건(기본 100)"}],
                 "API-37", lambda p: self.log[-int(p.get("last") or 100):])
        # 파일 올리기·내려받기(API-33): REST 의 /api/files 와 같은 올림 폴더를 쓰는 명령 꼴. 내용은 글(text) 또는 base64 다.
        register("file.upload", "S", "내용을 올림 폴더의 파일로 저장하고 그 경로를 돌려준다(명령의 path 로 쓴다). REST 는 POST /api/files/<이름>", [
            {"name": "name", "type": "string", "desc": "파일 이름(폴더 없이)", "must": True, "example": "model.inp"},
            {"name": "text", "type": "string", "desc": "글 내용(UTF-8)"},
            {"name": "base64", "type": "string", "desc": "이진 내용(base64)"}],
            "API-33", self._file_upload)
        register("file.download", "Q", "파일 내용을 읽는다(올림 폴더의 이름 또는 path). 글이면 text, 아니면 base64 로 돌려준다. REST 는 GET /api/files/<이름>", [
            {"name": "name", "type": "string", "desc": "올림 폴더의 파일 이름", "example": "model.inp"},
            {"name": "path", "type": "string", "desc": "파일 경로(name 대신)"},
            {"name": "encoding", "type": "string", "desc": "text(기본: UTF-8 로 읽히면 글, 아니면 base64) 또는 base64", "choices": ["text", "base64"]}],
            "API-33", self._file_download)

    def _file_upload(self, p):
        import base64
        name = self.safe_name(p["name"])
        if (p.get("text") is None) == (p.get("base64") is None):
            raise Nasa95Error("missing_param", "text 또는 base64 가운데 하나가 필요합니다", {"param": "text"})
        data = p["text"].encode("utf-8") if p.get("text") is not None else base64.b64decode(p["base64"])
        path = os.path.join(self.files_dir(), name)
        with open(path, "wb") as f:
            f.write(data)
        return {"name": name, "path": path, "bytes": len(data)}

    def _file_download(self, p):
        import base64
        if p.get("path"):
            path = p["path"]
        elif p.get("name"):
            path = os.path.join(self.files_dir(), self.safe_name(p["name"]))
        else:
            raise Nasa95Error("missing_param", "name 또는 path 가 필요합니다", {"param": "name"})
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            raise Nasa95Error("io_error", f"파일을 읽을 수 없습니다: {path}", {"path": path, "reason": str(e)}) from None
        encoding = p.get("encoding") or ""
        if encoding != "base64":
            try:
                return {"path": path, "bytes": len(data), "encoding": "text", "text": data.decode("utf-8")}
            except UnicodeDecodeError:
                if encoding == "text":
                    raise Nasa95Error("invalid_param", "UTF-8 글이 아닙니다(base64 로 받으세요)", {"path": path}) from None
        return {"path": path, "bytes": len(data), "encoding": "base64", "base64": base64.b64encode(data).decode("ascii")}

    def _configure(self, p):
        if self._server:
            raise Nasa95Error("invalid_state", "서버를 끈 뒤에 설정하세요", {})
        if p.get("host"):
            self.host = p["host"]
        if p.get("port") is not None:
            self.port = int(p["port"])
        if "allow_script" in p:
            self.allow_script = bool(p["allow_script"])
        if "upload_dir" in p:
            self.upload_dir = p["upload_dir"] or ""
        return self.status()

    def _token_create(self, p):
        scope = p.get("scope") or "full"
        if scope not in ("full", "read"):
            raise Nasa95Error("out_of_range", "scope 는 full 또는 read 입니다", {"param": "scope"})
        token = secrets.token_urlsafe(24)
        self.tokens[token] = scope
        return {"token": token, "scope": scope}

    def _token_revoke(self, p):
        self.tokens.pop(p["token"], None)
        return {"tokens": len(self.tokens)}


def install(app: App) -> RestServer:
    """App 에 server.* 명령을 붙인다(App 생성 뒤 한 번)."""
    server = RestServer(app)
    app.rest = server
    return server
