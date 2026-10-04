"""명령 등록부 위의 객체형 API (API-10, API-11).

- `App.execute(name, **params)` 가 유일한 실행 경로다.
- `app.model.materials.create(...)` 같은 객체형 접근은 등록부(app.kinds)에서 만들어진다.
- Python 객체(Handle)는 ID 만 가진다. 데이터는 코어에 있다.
"""
from __future__ import annotations

import json
from typing import Any, Callable, Iterator

from . import _ofep


class OfepError(Exception):
    """코어가 돌려준 구조화된 오류(API-05)."""

    def __init__(self, code: str, message: str, details: dict | None = None):
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message
        self.details = details or {}


class App:
    """창 없이 쓸 수 있는 프로그램 본체(API-15)."""

    def __init__(self) -> None:
        self._core = _ofep.App()
        self._kinds = {k["kind"]: k for k in self.execute("app.kinds")}
        self.model = Model(self)
        self.mesh = MeshArrays(self)
        self.results = ResultArrays(self)
        self.geometry = GeometryArrays(self)
        self.view = ViewArrays(self)
        from . import rest  # 내장 웹 서버의 server.* 명령(기본은 꺼짐)
        rest.install(self)
        from . import extensions  # 확장(extension.*): 폴더의 확장을 찾아 켠다(API-17)
        extensions.install(self).load_all()
        self.register_command(
            "script.run", self._run_script, kind="J", composite=False, features="API-14, API-36",
            desc="Python 스크립트를 실행한다. 스크립트 안의 명령은 각각 Undo 이력에 들어간다"
                 "(한 단계로 묶으려면 스크립트에서 app.transaction 을 쓴다)",
            params=[
                {"name": "path", "type": "string", "desc": "스크립트 파일", "example": "script.py"},
                {"name": "code", "type": "string", "desc": "스크립트 코드(파일 대신)", "example": "app.execute('material.create')"},
            ],
        )

    def register_command(self, name: str, fn: Callable[[dict], Any], *, desc: str, kind: str = "C",
                         params: list[dict] | None = None, features: str = "", composite: bool | None = None) -> None:
        """Python 함수로 명령을 등록한다(확장의 명령 추가, API-18).

        등록한 명령은 등록부·Python·매크로에 나타난다. kind 가 "C" 이면 함수 안에서 실행한 명령들이
        Undo 한 단계로 묶인다(기존 명령의 묶음으로 구현하므로 Undo 가 저절로 성립한다).
        """
        if composite is None:
            composite = kind == "C"

        def call(params_json: str) -> str:
            result = fn(json.loads(params_json))
            return json.dumps(result if result is not None else {})

        try:
            self._core.register_command(name, kind, desc, json.dumps(params or []), features, composite, call)
        except _ofep.CoreError as e:
            raise self._error(e) from None

    def unregister_command(self, name: str) -> None:
        try:
            self._core.unregister_command(name)
        except _ofep.CoreError as e:
            raise self._error(e) from None

    @staticmethod
    def _error(e: Exception) -> "OfepError":
        try:
            d = json.loads(str(e))
        except ValueError:
            return OfepError("internal", str(e))
        return OfepError(d.get("code", "internal"), d.get("message", ""), d.get("details"))

    def _run_script(self, params: dict) -> dict:
        if params.get("path"):
            try:
                with open(params["path"], encoding="utf-8") as f:
                    code, origin = f.read(), params["path"]
            except OSError as e:
                raise OfepError("io_error", f"스크립트 파일을 열 수 없습니다: {params['path']}", {"path": params["path"]}) from e
        elif params.get("code") is not None:
            code, origin = params["code"], "<script>"
        else:
            raise OfepError("missing_param", "path 또는 code 가 필요합니다", {"param": "path"})
        try:
            exec(compile(code, origin, "exec"), {"app": self, "model": self.model, "__name__": "__ofep_script__"})
        except OfepError:
            raise
        except Exception as e:  # 스크립트 자체의 오류: 위치를 알려 준다
            import traceback

            tb = traceback.extract_tb(e.__traceback__)
            line = next((f.lineno for f in reversed(tb) if f.filename == origin), None)
            raise OfepError("script_error", f"{type(e).__name__}: {e}", {"line": line, "path": origin}) from e
        return {}

    def execute(self, command: str, params: dict | None = None, /, **kwargs: Any) -> Any:
        # command·params 는 위치 전용이다(명령의 매개변수 이름 "name", "params" 와 겹치지 않게).
        p = dict(params or {})
        p.update(kwargs)
        # numpy 배열은 JSON 으로 바꾸지 않고 그대로 넘긴다(대량 입력, API-09).
        arrays = {k: v for k, v in p.items() if type(v).__module__ == "numpy" and hasattr(v, "dtype") and v.ndim > 0}
        for k in arrays:
            del p[k]
        try:
            return json.loads(self._core.execute(command, json.dumps(p), arrays))
        except _ofep.CoreError as e:
            raise self._error(e) from None

    def subscribe(self, callback: Callable[[dict], None]) -> int:
        """모델 변경 통지를 구독한다(API-06). 돌려주는 토큰으로 `unsubscribe` 한다."""
        return self._core.subscribe(lambda s: callback(json.loads(s)))

    def unsubscribe(self, token: int) -> None:
        self._core.unsubscribe(token)

    # 자주 쓰는 시스템 명령
    def undo(self) -> dict:
        return self.execute("app.undo")

    def redo(self) -> dict:
        return self.execute("app.redo")

    def digest(self) -> dict:
        return self.execute("project.digest")

    def commands(self) -> list[dict]:
        return self.execute("app.commands")

    def kinds(self) -> dict[str, dict]:
        return self._kinds

    def transaction(self, name: str = "") -> "_Transaction":
        """`with app.transaction("이름"):` 안의 명령을 Undo 한 단계로 묶는다(API-04)."""
        return _Transaction(self, name)

    @property
    def version(self) -> str:
        return self.execute("app.version")["version"]


class MeshArrays:
    """메시를 numpy 배열로 읽는다(API-12). 노드·요소는 ID 오름차순이다.

    돌려주는 배열은 읽기 전용 사본이다. 메시를 바꾸려면 `mesh.*` 명령을 쓴다.
    """

    def __init__(self, app: App):
        self._app = app

    def node_ids(self):
        return self._app._core.mesh_node_ids()

    def node_coords(self):
        """(노드 수, 3) 배열."""
        return self._app._core.mesh_node_coords()

    def element_ids(self):
        return self._app._core.mesh_element_ids()

    def element_shapes(self) -> list[str]:
        return self._app._core.mesh_element_shapes()

    def element_nodes(self):
        """(시작 위치 배열, 절점 ID 배열). i 번째 요소의 절점은 nodes[offset[i]:offset[i+1]]."""
        return self._app._core.mesh_element_nodes()


class ResultArrays:
    """결과를 numpy 배열로 읽는다(API-12, API-32). 돌려주는 배열은 읽기 전용 사본이다."""

    def __init__(self, app: App):
        self._app = app

    def values(self, result: int, frame: int, field: str, component: str = ""):
        """(절점 번호, 값). component 가 없으면 값은 (절점 수, 성분 수), 있으면 1차원(파생량도 가능)."""
        try:
            return self._app._core.result_values(result, frame, field, component)
        except _ofep.CoreError as e:
            raise self._app._error(e) from None


class GeometryArrays:
    """형상의 표시용 삼각화를 numpy 배열로 읽는다(GEO-06)."""

    def __init__(self, app: App):
        self._app = app

    def tessellation(self, part: int, deflection: float = 0.0, angle: float = 0.0) -> dict:
        """points·normals (N,3), triangles (M,3), triangle_face (M,), edge_points (K,3), edge_offsets (모서리 수+1,)."""
        try:
            keys = ("points", "normals", "triangles", "triangle_face", "edge_points", "edge_offsets")
            return dict(zip(keys, self._app._core.geometry_tessellation(part, deflection, angle)))
        except _ofep.CoreError as e:
            raise self._app._error(e) from None


class ViewArrays:
    """현재 뷰를 이미지로 그려 numpy 배열로 읽는다(창 없이 그린다, RND-45)."""

    def __init__(self, app: App):
        self._app = app

    def render(self, width: int = 800, height: int = 600):
        """(색, ID). 색은 (높이, 너비, 4) uint8, ID 는 (높이, 너비) uint32(0 = 아무것도 없음)."""
        if not hasattr(self._app._core, "view_render"):
            raise OfepError("not_available", "이 빌드에는 렌더러가 없습니다", {})
        try:
            return self._app._core.view_render(width, height)
        except _ofep.CoreError as e:
            raise self._app._error(e) from None

    # --- 창 연동(UI 가 쓴다). 네이티브 창 핸들을 붙이면 present 가 그 창에 그린다.
    def attach_window(self, native_handle: int) -> None:
        self._call("view_attach_window", int(native_handle))

    def detach_window(self) -> None:
        self._call("view_detach_window")

    def present(self) -> tuple[int, int]:
        """현재 뷰를 창에 그린다. 창 크기를 돌려준다(0 이면 그리지 않음)."""
        return self._call("view_present")

    def pick(self, x: int, y: int) -> dict:
        return json.loads(self._call("view_pick_window", int(x), int(y)))

    def hover(self, x: int, y: int) -> dict:
        """마우스 아래 객체를 강조 대상으로 삼는다. changed 가 True 면 다시 그려야 한다."""
        return json.loads(self._call("view_hover", int(x), int(y)))

    def orbit_begin(self, x: float, y: float, *, width: int | None = None, height: int | None = None) -> dict:
        """드래그 시작점의 모델 표면을 중심으로 고른다. 빈 곳이면 기존 중심을 유지한다."""
        params = {"x": float(x), "y": float(y)}
        if width is not None:
            params["width"] = width
        if height is not None:
            params["height"] = height
        return self._app.execute("view.orbit_begin", **params)

    def orbit(self, dx: float, dy: float, *, height: int | None = None) -> dict:
        """화면 축으로 회전한다. 이동량과 height 는 같은 좌표 단위를 사용한다."""
        params = {"dx": float(dx), "dy": float(dy)}
        if height is not None:
            params["height"] = height
        return self._app.execute("view.orbit", **params)

    def pan(self, dx: float, dy: float) -> dict:
        return json.loads(self._call("view_pan", float(dx), float(dy)))

    def zoom(self, factor: float, x: float = 0.0, y: float = 0.0) -> dict:
        return json.loads(self._call("view_zoom", float(factor), float(x), float(y)))

    def _call(self, name: str, *args):
        if not hasattr(self._app._core, name):
            raise OfepError("not_available", "이 빌드에는 렌더러가 없습니다", {})
        try:
            return getattr(self._app._core, name)(*args)
        except _ofep.CoreError as e:
            raise self._app._error(e) from None


class _Transaction:
    def __init__(self, app: App, name: str):
        self._app, self._name = app, name

    def __enter__(self) -> "_Transaction":
        self._app.execute("app.transaction_begin", name=self._name)
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        if exc_type is None:
            self._app.execute("app.transaction_commit")
        elif self._app.execute("app.history")["in_transaction"]:
            # 명령 실패로 코어가 이미 묶음을 되돌렸으면 열려 있지 않다.
            self._app.execute("app.transaction_rollback")
        return False


class Handle:
    """모델 객체를 가리키는 가벼운 핸들. 삭제된 객체에 쓰면 OfepError(not_found) 가 난다."""

    def __init__(self, app: App, kind: str, id: int):
        self._app, self.kind, self.id = app, kind, id

    def __repr__(self) -> str:
        return f"<{self.kind} id={self.id}>"

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Handle) and (other.kind, other.id) == (self.kind, self.id)

    def __hash__(self) -> int:
        return hash((self.kind, self.id))

    def _call(self, action: str, /, **params: Any) -> Any:
        return self._app.execute(f"{self.kind}.{action}", id=self.id, **params)

    def get(self) -> dict:
        return self._call("get")

    @property
    def name(self) -> str:
        return self.get()["name"]

    @property
    def props(self) -> dict:
        return self.get()["props"]

    def update(self, **fields: Any) -> "Handle":
        self._call("update", **fields)
        return self

    def rename(self, name: str) -> "Handle":
        self._call("rename", name=name)
        return self

    def copy(self, **params: Any) -> "Handle":
        return Handle(self._app, self.kind, self._call("copy", **params)["id"])

    def delete(self) -> None:
        self._call("delete")

    def suppress(self) -> "Handle":
        self._call("suppress")
        return self

    def unsuppress(self) -> "Handle":
        self._call("unsuppress")
        return self

    def move(self, **params: Any) -> "Handle":
        self._call("move", **params)
        return self

    def references(self) -> list[dict]:
        return self._call("references")

    def validate(self) -> list[dict]:
        return self._call("validate")

    def __getattr__(self, name: str) -> Any:
        # 하위 컬렉션(예: case.steps, step.loads) 또는 종류 전용 명령(예: material.set_elastic)
        if name.startswith("_"):
            raise AttributeError(name)
        if self.kind == "step" and name in ("loads", "bcs"):
            # 하중은 하중 셋 안에만, 구속은 구속 셋 안에만(2026-10-04). step.loads 는 이 스텝 전용 셋의 모음이다(없으면 만든다)
            own = self._app.execute("step.own_load_set" if name == "loads" else "step.own_bc_set", id=self.id)["id"]
            return Collection(self._app, "load" if name == "loads" else "bc", parent=own)
        for k in self._app.kinds().values():
            if k["collection"] == name and self.kind in k["parents"]:
                return Collection(self._app, k["kind"], parent=self.id)

        def call(**params: Any) -> Any:
            return self._call(name, **params)

        return call


class Collection:
    """한 종류의 객체 모음. 워크 트리의 가지와 같은 경로로 접근한다."""

    def __init__(self, app: App, kind: str, parent: int | None = None):
        self._app, self.kind, self._parent = app, kind, parent

    def _list(self) -> list[dict]:
        params = {} if self._parent is None else {"parent": self._parent}
        return self._app.execute(f"{self.kind}.list", **params)

    def list(self) -> list[dict]:
        return self._list()

    def __len__(self) -> int:
        return len(self._list())

    def __iter__(self) -> Iterator[Handle]:
        return (Handle(self._app, self.kind, o["id"]) for o in self._list())

    def __getitem__(self, key: int | str) -> Handle:
        """정수는 ID, 문자열은 이름."""
        for o in self._list():
            if (isinstance(key, str) and o["name"] == key) or (not isinstance(key, str) and o["id"] == key):
                return Handle(self._app, self.kind, o["id"])
        raise OfepError("not_found", f"{self.kind} 에서 찾을 수 없습니다: {key!r}", {"kind": self.kind})

    def __getattr__(self, name: str) -> Any:
        # create, create_<하위 종류>
        if not name.startswith("create"):
            raise AttributeError(name)

        def create(**params: Any) -> Handle:
            if self._parent is not None:
                params.setdefault("parent", self._parent)
            return Handle(self._app, self.kind, self._app.execute(f"{self.kind}.{name}", **params)["id"])

        return create


class Model:
    """최상위 컬렉션들. 이름은 객체 종류 정의의 `collection` 이다."""

    def __init__(self, app: App):
        self._app = app

    def __getattr__(self, name: str) -> Collection:
        if name.startswith("_"):
            raise AttributeError(name)
        for k in self._app.kinds().values():
            if k["collection"] == name:
                return Collection(self._app, k["kind"])
        raise AttributeError(f"알 수 없는 컬렉션: {name}")

    def handle(self, kind: str, id: int) -> Handle:
        return Handle(self._app, kind, id)
