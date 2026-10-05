"""확장(API-17~19, 아키텍처 규칙 12): 지정 폴더의 Python 패키지를 찾아 불러오고 켜고 끈다.

확장 = 폴더 하나(패키지). `__init__.py` 에 다음을 둔다.
  NAME = "my_ext"            # 없으면 폴더 이름
  VERSION = "0.1"
  DESCRIPTION = "..."
  def register(app): ...     # 명령 등록(app.register_command). UI 항목은 ui(window) 로.
  def unregister(app): ...   # 선택. 없으면 register 가 등록한 명령을 자동으로 해제한다
  def ui(window): ...        # 선택. 주 창이 뜰 때 메뉴·패널을 더한다(API-19)
확장은 코어와 같은 공개 API(명령 계층)만 쓴다. 코어 안에 확장 전용 분기를 두지 않는다.

찾는 폴더: 환경 변수 NASA95_EXTENSIONS(경로 목록, os.pathsep), 프로그램 설정 extensions_dir, 사용자 설정 폴더의 extensions/.
끄고 켠 상태는 프로그램 설정 disabled_extensions 에 남는다.
"""
from __future__ import annotations

import importlib.util
import os
import sys
import traceback
from typing import Any

from .api import App, Nasa95Error


class Extension:
    def __init__(self, name: str, path: str):
        self.name, self.path = name, path
        self.module: Any = None
        self.version = ""
        self.description = ""
        self.commands: list[str] = []
        self.error = ""
        self.enabled = False

    def info(self) -> dict:
        return {"name": self.name, "path": self.path, "version": self.version, "description": self.description,
                "enabled": self.enabled, "commands": list(self.commands), "error": self.error}


class ExtensionManager:
    def __init__(self, app: App):
        self.app = app
        self.extensions: dict[str, Extension] = {}
        self.windows: list[Any] = []  # ui(window) 를 부를 창
        # ext.register_* 로 등록한 것(API-18~21). UI 항목은 창이 붙을 때 적용하고, 형식·파생 결과·보고서는 명령 이름의 등록부다.
        self.ui_items: list[dict] = []
        self.importers: dict[str, dict] = {}   # 확장자(".xyz") → {command, label, extension}
        self.exporters: dict[str, dict] = {}
        self.derived_results: dict[str, dict] = {}  # 이름 → {command, label}: 파생 결과 계산 명령(Q, result.derived_values 와 같은 꼴)
        self.report_formats: dict[str, dict] = {}   # 이름 → {command, label}: 보고서 생성 명령(J, id·path 를 받음)
        self._register_commands()

    # ------------------------------------------------------------ 찾기
    def search_dirs(self) -> list[str]:
        dirs = []
        env = os.environ.get("NASA95_EXTENSIONS", "")
        dirs += [d for d in env.split(os.pathsep) if d]
        setting = self.app.execute("app.settings_get", key="extensions_dir")["value"]
        if setting:
            dirs.append(str(setting))
        settings_path = self.app.execute("app.settings_get")["path"]
        dirs.append(os.path.join(os.path.dirname(settings_path), "extensions"))
        return dirs

    def discover(self) -> list[Extension]:
        found: list[Extension] = []
        for d in self.search_dirs():
            if not os.path.isdir(d):
                continue
            for entry in sorted(os.listdir(d)):
                path = os.path.join(d, entry)
                if os.path.isfile(os.path.join(path, "__init__.py")) and entry.isidentifier():
                    found.append(Extension(entry, path))
        return found

    def disabled_names(self) -> set[str]:
        v = self.app.execute("app.settings_get", key="disabled_extensions")["value"]
        return set(v or [])

    # ------------------------------------------------------------ 불러오기
    def load_all(self) -> list[dict]:
        """폴더를 훑어 아직 없는 확장을 등록하고, 끄지 않은 것은 켠다."""
        disabled = self.disabled_names()
        for ext in self.discover():
            if ext.name in self.extensions:
                continue
            self.extensions[ext.name] = ext
            if ext.name not in disabled:
                self.enable(ext.name)
        return [e.info() for e in self.extensions.values()]

    def enable(self, name: str) -> dict:
        ext = self._get(name)
        if ext.enabled:
            return ext.info()
        before = {c["name"] for c in self.app.commands()}
        try:
            spec = importlib.util.spec_from_file_location(f"nasa95_ext.{name}", os.path.join(ext.path, "__init__.py"),
                                                          submodule_search_locations=[ext.path])
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            ext.module = module
            ext.version = str(getattr(module, "VERSION", ""))
            ext.description = str(getattr(module, "DESCRIPTION", ""))
            if hasattr(module, "register"):
                module.register(self.app)
            ext.commands = sorted({c["name"] for c in self.app.commands()} - before)
            for w in self.windows:
                self._call_ui(ext, w)
            ext.enabled, ext.error = True, ""
        except Exception as e:  # 확장의 오류는 프로그램을 멈추지 않는다
            ext.error = f"{type(e).__name__}: {e}\n{traceback.format_exc(limit=3)}"
            for c in {c["name"] for c in self.app.commands()} - before:
                self.app.unregister_command(c)
            ext.enabled = False
        self._save_disabled()
        return ext.info()

    def disable(self, name: str) -> dict:
        ext = self._get(name)
        if ext.enabled:
            try:
                if hasattr(ext.module, "unregister"):
                    ext.module.unregister(self.app)
            except Exception as e:
                ext.error = f"{type(e).__name__}: {e}"
            for c in ext.commands:
                try:
                    self.app.unregister_command(c)
                except Nasa95Error:
                    pass
            ext.commands = []
            ext.enabled = False
        self._save_disabled(disabled_add=name)
        return ext.info()

    def attach_window(self, window: Any) -> None:
        """주 창이 뜨면 켜진 확장의 ui(window) 를 부르고 ext.register_* 로 등록된 UI 항목을 더한다(API-19)."""
        self.windows.append(window)
        for ext in self.extensions.values():
            if ext.enabled:
                self._call_ui(ext, window)
        for item in self.ui_items:
            self._apply_ui_item(item, window)

    def _apply_ui_item(self, item: dict, window: Any) -> None:
        """창의 공개 훅(add_extension_menu·toolbar·panel·dialog·tree_action)으로 UI 항목을 더한다. 훅이 없는 창은 건너뛴다."""
        hook = getattr(window, "add_extension_" + item["type"], None)
        if hook is None:
            return
        try:
            hook(item)
        except Exception as e:  # 확장 UI 의 오류는 창을 멈추지 않는다
            item["error"] = f"{type(e).__name__}: {e}"

    def _call_ui(self, ext: Extension, window: Any) -> None:
        if hasattr(ext.module, "ui"):
            try:
                ext.module.ui(window)
            except Exception as e:
                ext.error = f"ui: {type(e).__name__}: {e}"

    def _get(self, name: str) -> Extension:
        if name not in self.extensions:
            raise Nasa95Error("not_found", f"없는 확장입니다: {name}", {"extension": name, "available": sorted(self.extensions)})
        return self.extensions[name]

    def _save_disabled(self, disabled_add: str | None = None) -> None:
        disabled = {n for n, e in self.extensions.items() if not e.enabled and not e.error} | self.disabled_names()
        enabled = {n for n, e in self.extensions.items() if e.enabled}
        disabled -= enabled
        if disabled_add:
            disabled.add(disabled_add)
        self.app.execute("app.settings_set", key="disabled_extensions", value=sorted(disabled) or None)

    # ------------------------------------------------------------ 명령(extension.*)
    def _register_commands(self) -> None:
        app = self.app
        app.register_command("extension.list", lambda p: self.load_all() if p.get("rescan") else [e.info() for e in self.extensions.values()],
                             kind="Q", composite=False, features="API-17",
                             desc="설치된 확장과 상태(켜짐·오류·등록한 명령)를 조회한다. rescan 이면 폴더를 다시 훑는다",
                             params=[{"name": "rescan", "type": "bool", "desc": "확장 폴더를 다시 훑어 새 확장을 불러온다"}])
        app.register_command("extension.enable", lambda p: self.enable(p["name"]), kind="S", composite=False, features="API-17",
                             desc="확장을 켠다(불러오고 register 를 부른다). 설정에 남는다",
                             params=[{"name": "name", "type": "string", "desc": "확장 이름(폴더 이름)", "must": True, "example": "my_ext"}])
        app.register_command("extension.disable", lambda p: self.disable(p["name"]), kind="S", composite=False, features="API-17",
                             desc="확장을 끈다(등록한 명령을 해제한다). 설정에 남는다",
                             params=[{"name": "name", "type": "string", "desc": "확장 이름", "must": True, "example": "my_ext"}])
        self._register_ext_commands()

    # ------------------------------------------------------------ 명령(ext.*): 확장이 명령 계층을 통해 기능을 더한다(API-18~21)
    def _register_ext_commands(self) -> None:
        app = self.app
        S = lambda name, t="string", desc="", must=False, example=None, **kw: {  # noqa: E731
            "name": name, "type": t, "desc": desc, "must": must, **({"example": example} if example is not None else {}), **kw}

        app.register_command("ext.register_command", self._register_command, kind="S", composite=False, features="API-18",
                             desc="새 명령을 등록부에 추가한다. 본문은 Python 코드(code: run(app, params) 를 정의) 또는 기존 명령의 "
                                  "묶음(steps: [{command, params}], 값이 \"$이름\" 이면 호출 매개변수로 바꾼다)이다. "
                                  "변경 명령(kind C)은 안의 명령들이 Undo 한 단계가 된다",
                             params=[S("name", desc="명령 이름(점으로 영역.동작)", must=True, example="my.hello"),
                                     S("desc", desc="설명", example="인사"),
                                     S("kind", desc="종류(C 변경, Q 조회, J 작업, S 시스템)", example="C", choices=["C", "Q", "J", "S"]),
                                     S("params", "object_list", "매개변수 정의 목록(name·type·desc·must·example)", example=[]),
                                     S("code", desc="Python 코드: def run(app, params): ... 를 정의한다"),
                                     S("steps", "object_list", "명령 묶음 [{command, params}]"),
                                     S("features", desc="대응 기능 ID"),
                                     S("extension", desc="등록하는 확장 이름(기록용)")])
        for what, label in [("menu", "메뉴 항목"), ("toolbar", "도구 모음 단추"), ("panel", "패널(도킹 창)"), ("dialog", "대화상자"),
                            ("tree_action", "워크 트리 상황 메뉴 항목")]:
            app.register_command(f"ext.register_{what}", lambda p, w=what: self._register_ui(w, p), kind="S", composite=False,
                                 features="API-19",
                                 desc=f"{label}을 추가한다. 고르면 command 를 params 로 실행한다(tree_action 은 고른 객체의 id 를 params.id 로 넣는다). "
                                      "창이 떠 있으면 바로 더하고, 아니면 창이 뜰 때 더한다",
                                 params=[S("label", desc="표시 글", must=True, example="인사"),
                                         S("command", desc="실행할 명령", must=True, example="my.hello"),
                                         S("params", "object", "명령 매개변수", example={}),
                                         S("menu", desc="menu: 넣을 메뉴 이름(없으면 '확장')", example="확장"),
                                         S("kind", desc="tree_action: 대상 객체 종류(없으면 모든 종류)", example="part"),
                                         S("extension", desc="등록하는 확장 이름")])
        for what, label in [("importer", "가져오기"), ("exporter", "내보내기")]:
            app.register_command(f"ext.register_{what}", lambda p, w=what: self._register_format(w, p), kind="S", composite=False,
                                 features="API-20",
                                 desc=f"파일 {label} 형식을 추가한다: 확장자에 명령을 잇는다(명령은 path 를 받는다). "
                                      f"file.{'import' if what == 'importer' else 'export'} 와 UI 파일 대화상자가 쓴다",
                                 params=[S("extension", desc="파일 확장자(점 포함)", must=True, example=".xyz"),
                                         S("command", desc="실행할 명령(path 매개변수)", must=True, example="my.import_xyz"),
                                         S("label", desc="형식 이름", example="XYZ 점 파일"),
                                         S("ext_name", desc="등록하는 확장 이름")])
        app.register_command("ext.register_derived_result", lambda p: self._register_named(self.derived_results, p), kind="S",
                             composite=False, features="API-21",
                             desc="파생 결과 계산을 추가한다: 이름에 조회 명령을 잇는다(명령은 result·frame·nodes 를 받아 ids·values 를 돌려준다). "
                                  "result.derived_list 로 조회한다",
                             params=[S("name", desc="파생량 이름", must=True, example="safety_factor"),
                                     S("command", desc="계산 명령(Q)", must=True, example="my.safety_factor"),
                                     S("label", desc="표시 이름", example="안전율"),
                                     S("extension", desc="등록하는 확장 이름")])
        app.register_command("ext.register_report", lambda p: self._register_named(self.report_formats, p), kind="S", composite=False,
                             features="API-21",
                             desc="보고서 형식을 추가한다: 이름에 생성 명령을 잇는다(명령은 보고서 객체 id 와 path 를 받는다). "
                                  "report.generate format= 으로 부른다",
                             params=[S("name", desc="형식 이름", must=True, example="markdown"),
                                     S("command", desc="생성 명령(J)", must=True, example="my.report_md"),
                                     S("label", desc="표시 이름", example="Markdown 보고서"),
                                     S("extension", desc="등록하는 확장 이름")])
        app.register_command("ext.registrations", lambda p: self.registrations(), kind="Q", composite=False, features="API-18, API-19, API-20, API-21",
                             desc="ext.register_* 로 등록된 것(UI 항목·가져오기/내보내기 형식·파생 결과·보고서 형식)을 조회한다")
        app.register_command("file.import", self._file_import, kind="C", composite=True, features="API-20",
                             desc="확장자로 등록된 가져오기 형식을 골라 그 명령을 실행한다(내장 형식: 형상 STEP·IGES·BREP, 메시 .inp)",
                             params=[S("path", desc="파일 경로", must=True, example="model.xyz"), S("params", "object", "형식 명령에 더할 매개변수", example={})])
        app.register_command("file.export", self._file_export, kind="J", composite=False, features="API-20",
                             desc="확장자로 등록된 내보내기 형식을 골라 그 명령을 실행한다(내장 형식: 형상 STEP·IGES·BREP·STL, 메시 .inp)",
                             params=[S("path", desc="파일 경로", must=True, example="model.xyz"), S("params", "object", "형식 명령에 더할 매개변수", example={})])

    def _register_command(self, p: dict) -> dict:
        name = p["name"]
        if "." not in name or not all(part.isidentifier() for part in name.split(".")):
            raise Nasa95Error("invalid_param", "명령 이름은 '영역.동작' 꼴의 식별자여야 합니다", {"param": "name", "name": name})
        if name in {c["name"] for c in self.app.commands()}:
            raise Nasa95Error("name_conflict", f"이미 있는 명령입니다: {name}", {"param": "name", "name": name})
        kind = p.get("kind") or "C"
        code, steps = p.get("code"), p.get("steps")
        if (code is None) == (steps is None):
            raise Nasa95Error("missing_param", "code 또는 steps 가운데 하나가 필요합니다", {"param": "code"})
        app = self.app
        if code is not None:
            ns: dict[str, Any] = {}
            try:
                exec(compile(code, f"<ext command {name}>", "exec"), ns)  # noqa: S102 — 확장 코드는 사용자가 넣는 것이다
            except Exception as e:
                raise Nasa95Error("invalid_param", f"코드를 읽을 수 없습니다: {type(e).__name__}: {e}", {"param": "code"}) from None
            run = ns.get("run")
            if not callable(run):
                raise Nasa95Error("invalid_param", "코드는 run(app, params) 함수를 정의해야 합니다", {"param": "code"})

            def fn(params: dict, run=run) -> Any:
                return run(app, params)
        else:
            def fn(params: dict, steps=list(steps)) -> Any:
                def fill(v):
                    if isinstance(v, str) and v.startswith("$"):
                        key = v[1:]
                        if key not in params:
                            raise Nasa95Error("missing_param", f"매개변수가 없습니다: {key}", {"param": key})
                        return params[key]
                    if isinstance(v, dict):
                        return {k: fill(x) for k, x in v.items()}
                    if isinstance(v, list):
                        return [fill(x) for x in v]
                    return v
                last = None
                for step in steps:
                    last = app.execute(step["command"], fill(step.get("params") or {}))
                return last
        app.register_command(name, fn, desc=p.get("desc") or "", kind=kind, params=p.get("params") or [], features=p.get("features") or "",
                             composite=(kind == "C"))
        ext = self.extensions.get(p.get("extension") or "")
        if ext is not None:
            ext.commands.append(name)
        return {"name": name, "kind": kind, "body": "code" if code is not None else "steps"}

    def _register_ui(self, what: str, p: dict) -> dict:
        item = {"type": what, "label": p["label"], "command": p["command"], "params": p.get("params") or {},
                "menu": p.get("menu") or "확장", "kind": p.get("kind") or "", "extension": p.get("extension") or ""}
        self.ui_items.append(item)
        for w in self.windows:
            self._apply_ui_item(item, w)
        return {"type": what, "label": item["label"], "count": len(self.ui_items), "windows": len(self.windows)}

    def _register_format(self, what: str, p: dict) -> dict:
        ext = p["extension"].lower()
        if not ext.startswith(".") or len(ext) < 2:
            raise Nasa95Error("invalid_param", "확장자는 점으로 시작해야 합니다", {"param": "extension"})
        if p["command"] not in {c["name"] for c in self.app.commands()}:
            raise Nasa95Error("unknown_command", f"등록되지 않은 명령: {p['command']}", {"param": "command"})
        table = self.importers if what == "importer" else self.exporters
        table[ext] = {"extension": ext, "command": p["command"], "label": p.get("label") or ext, "ext_name": p.get("ext_name") or ""}
        return {"extension": ext, "command": p["command"], "registered": sorted(table)}

    def _register_named(self, table: dict, p: dict) -> dict:
        if p["command"] not in {c["name"] for c in self.app.commands()}:
            raise Nasa95Error("unknown_command", f"등록되지 않은 명령: {p['command']}", {"param": "command"})
        table[p["name"]] = {"name": p["name"], "command": p["command"], "label": p.get("label") or p["name"], "extension": p.get("extension") or ""}
        return {"name": p["name"], "command": p["command"], "registered": sorted(table)}

    def registrations(self) -> dict:
        return {"ui": list(self.ui_items), "importers": list(self.importers.values()), "exporters": list(self.exporters.values()),
                "derived_results": list(self.derived_results.values()), "report_formats": list(self.report_formats.values())}

    BUILTIN_IMPORTERS = {".step": "geometry.import", ".stp": "geometry.import", ".iges": "geometry.import", ".igs": "geometry.import",
                         ".brep": "geometry.import", ".inp": "mesh.import"}
    BUILTIN_EXPORTERS = {".step": "geometry.export", ".stp": "geometry.export", ".iges": "geometry.export", ".igs": "geometry.export",
                         ".brep": "geometry.export", ".stl": "geometry.export", ".inp": "mesh.export"}

    def _file_import(self, p: dict) -> Any:
        return self._dispatch(p, self.importers, self.BUILTIN_IMPORTERS)

    def _file_export(self, p: dict) -> Any:
        return self._dispatch(p, self.exporters, self.BUILTIN_EXPORTERS)

    def _dispatch(self, p: dict, table: dict, builtin: dict) -> Any:
        ext = os.path.splitext(p["path"])[1].lower()
        command = table[ext]["command"] if ext in table else builtin.get(ext)
        if command is None:
            raise Nasa95Error("not_available", f"이 확장자의 형식이 없습니다: {ext or '(없음)'}", {"param": "path", "extension": ext,
                                                                                         "available": sorted(set(table) | set(builtin))})
        params = dict(p.get("params") or {})
        params["path"] = p["path"]
        return self.app.execute(command, params)


def install(app: App) -> ExtensionManager:
    manager = ExtensionManager(app)
    app.extensions = manager
    return manager
