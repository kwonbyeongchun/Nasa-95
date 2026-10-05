"""API 테스트 공통 도구.

테스트 방법·판정 원칙은 작업 폴더의 `.agent/proj-api-test.md`, 케이스 정의는 `.agent/tests/tc-*.md`.
테스트 함수 이름에 TC ID 를 넣고, `@pytest.mark.feature` 로 검증하는 기능 ID 를 표시한다.

전수 검사는 명령 등록부와 객체 종류 정의에서 유효한 입력을 만들어 쓴다(Builder).
객체 종류나 속성을 추가하면 따로 손대지 않아도 전수 검사에 포함된다.
"""
from __future__ import annotations

import copy
import os
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from nasa95 import App  # noqa: E402

# 설계 문서(작업 폴더). 환경 변수로 바꿀 수 있다.
WORKS = Path(os.environ.get("NASA95_WORKS", ROOT.parent / "open-fep-works"))
API_FULL_LIST = WORKS / ".agent" / "proj-api-full-list.md"

NODES = {"type": "nodes", "ids": [1]}

# 속성 변경 테스트에서 바꿀 속성을 고르는 우선순위
ALT_ORDER = ["number", "integer", "bool", "string", "vector3", "number_list", "integer_list", "table",
             "string_list", "target", "ref_list"]


def in_range(f: dict, x: float) -> bool:
    lo, hi = f.get("min"), f.get("max")
    if lo is not None and (x <= lo if f.get("min_exclusive") else x < lo):
        return False
    if hi is not None and (x >= hi if f.get("max_exclusive") else x > hi):
        return False
    return True


def pick_number(f: dict, integer: bool):
    candidates = [1, 2, 3, 5, 10] if integer else [1.5, 0.5, 0.25, 2.5, -0.1, 10.0, 100.0]
    lo, hi = f.get("min"), f.get("max")
    if lo is not None and hi is not None:
        mid = (lo + hi) / 2
        candidates.insert(0, int(mid) if integer else mid)
    elif lo is not None:  # 하한만 있으면 그 바로 위
        candidates.append(int(lo) + 1 if integer else lo + 1.0)
    elif hi is not None:
        candidates.append(int(hi) - 1 if integer else hi - 1.0)
    for c in candidates:
        if in_range(f, c):
            return c
    raise AssertionError(f"유효한 값을 고를 수 없음: {f}")


class Builder:
    """유효한 입력으로 객체를 만들어 주는 도우미. 필요한 상위 객체·참조 대상을 함께 만든다."""

    def __init__(self, app: App):
        self.app = app
        self.kinds = app.kinds()
        self._building: list[str] = []

    # --- 종류·속성 정의
    def create_commands(self) -> list[tuple[str, str]]:
        """(종류, 하위 종류) 전부."""
        out = []
        for k in self.kinds.values():
            subs = [s["name"] for s in k["subtypes"]] or [""]
            out += [(k["kind"], s) for s in subs]
        return out

    def first_sub(self, kind: str) -> str:
        subs = self.kinds[kind]["subtypes"]
        return subs[0]["name"] if subs else ""

    def fields(self, kind: str, sub: str = "") -> list[dict]:
        k = self.kinds[kind]
        out = list(k["fields"])
        for s in k["subtypes"]:
            if s["name"] == sub:
                out += s["fields"]
        return out

    def command(self, kind: str, sub: str = "") -> str:
        return f"{kind}.create" + (f"_{sub}" if sub else "")

    # --- 값 만들기
    def sample(self, f: dict):
        """속성 정의에서 유효한 값을 만든다. 만들 수 없으면(자기 종류를 가리키는 참조) None."""
        t = f["type"]
        if t == "ref":
            kind = f.get("ref_kind") or "material"
            return None if kind in self._building else self.make(kind)
        if t == "ref_list":
            kind = f.get("ref_kind") or "material"
            return None if kind in self._building else [self.make(kind)]
        if t == "object_list":
            item = {}
            for sub in f["items"]:
                v = self.sample(sub)
                if v is not None:
                    item[sub["name"]] = v
                elif sub.get("must") or sub.get("required"):
                    return None  # 필수 항목(자기 종류를 가리키는 참조 등)을 못 채우면 목록 자체를 뺀다(선택 속성)
            return [item]
        if "example" in f:
            return copy.deepcopy(f["example"])
        if t == "string":
            return f["choices"][0] if f.get("choices") else "text"
        if t == "number":
            return pick_number(f, False)
        if t == "integer":
            return pick_number(f, True)
        if t == "bool":
            return True
        if t == "vector3":
            return [1.0, 0.0, 0.0]
        if t == "number_list":
            return [pick_number(f, False)] * 2
        if t == "integer_list":
            return [pick_number(f, True)]
        if t == "string_list":
            return ["a"]
        if t == "table":
            n = f.get("cols") or 2
            return [[float(i + j) for j in range(n)] for i in range(2)]
        if t == "target":
            return {"type": (f.get("choices") or ["nodes"])[0], "ids": [1]}
        if t == "object":
            return {}
        raise AssertionError(f"예시 값이 필요한 속성: {f['name']} ({t})")

    def alt(self, f: dict, cur):
        """현재 값과 다른 유효한 값. 만들 수 없으면 None."""
        t = f["type"]
        if t in ("number", "integer"):
            for c in ([cur + 1, cur - 1, cur * 2] if t == "integer" else [cur * 2, cur / 2, cur + 0.25, cur - 0.25]):
                if c != cur and in_range(f, c):
                    return c
            return None
        if t == "bool":
            return not cur
        if t == "string":
            if f.get("choices"):
                others = [c for c in f["choices"] if c != cur]
                return others[0] if others else None
            return str(cur) + "_2"
        if t == "vector3":
            return [x + 1.0 for x in cur]
        if t == "number_list":
            out = [x * 2 if x else 1.0 for x in cur]
            return out if all(in_range(f, x) for x in out) else None
        if t == "integer_list":
            nxt = max(cur) + 1
            return cur + [nxt] if in_range(f, nxt) else None
        if t == "table":
            return [row[:1] + [x * 2 + 1 for x in row[1:]] for row in cur] if cur and len(cur[0]) > 1 else None
        if t == "string_list":
            return cur + ["X"]
        if t == "target":
            return {"type": cur["type"], "ids": [[7, 2]] if cur["type"] == "faces" else [7]}
        if t == "ref_list":
            return cur + [self.make(f.get("ref_kind") or "material")]
        return None

    def params(self, kind: str, sub: str = "", **override) -> dict:
        """생성 명령의 유효한 매개변수(모든 속성 + 상위 객체)."""
        self._building.append(kind)
        try:
            p = {}
            for f in self.fields(kind, sub):
                if f["name"] in override:
                    continue
                v = self.sample(f)
                if v is not None:
                    p[f["name"]] = v
            parents = [k for k in self.kinds[kind]["parents"] if k]  # "" = 최상위도 된다
            if parents and "parent" not in override:
                p["parent"] = self.make(parents[0])
        finally:
            self._building.pop()
        p.update({k: v for k, v in override.items() if v is not None})
        return p

    def make(self, kind: str, sub: str | None = None, **override) -> int:
        sub = self.first_sub(kind) if sub is None else sub
        return self.app.execute(self.command(kind, sub), self.params(kind, sub, **override))["id"]

    def update_value(self, kind: str, oid: int) -> dict | None:
        """속성 하나를 다른 유효한 값으로 바꾸는 입력. 바꿀 속성이 없는 종류(예: explode 피처)면 None."""
        obj = self.app.execute(f"{kind}.get", id=oid)
        fields = self.fields(kind, obj["props"].get("type", ""))
        if not [f for f in fields if f["name"] != "type"]:
            return None
        for t in ALT_ORDER:
            for f in fields:
                if f["type"] != t or f["name"] not in obj["props"]:
                    continue
                v = self.alt(f, obj["props"][f["name"]])
                if v is not None and v != obj["props"][f["name"]]:
                    return {f["name"]: v}
        raise AssertionError(f"{kind}: 바꿀 속성을 찾지 못함")

    def command_params(self, cmd: dict, only_must: bool = True) -> dict:
        """명령의 매개변수 정의에서 유효한 입력을 만든다(id·parent 제외)."""
        p = {}
        for f in cmd["params"]:
            if f["name"] in ("id", "parent") or (only_must and not f["must"]):
                continue
            v = self.sample(f)
            if v is not None:
                p[f["name"]] = v
        return p

    def populate(self) -> dict[tuple[str, str], int]:
        """모든 (종류, 하위 종류)를 하나씩 만든다."""
        return {(k, s): self.make(k, s) for k, s in self.create_commands()}


@pytest.fixture
def app() -> App:
    return App()


@pytest.fixture
def build(app: App) -> Builder:
    return Builder(app)


def total(app: App) -> str:
    return app.digest()["total"]


def history_len(app: App) -> int:
    return len(app.execute("app.history")["undo"])


def documented_commands() -> dict[str, str] | None:
    """전체 API 목록 문서의 {이름: 종류}. 문서가 없으면 None."""
    if not API_FULL_LIST.exists():
        return None
    text = API_FULL_LIST.read_text(encoding="utf-8")
    return dict(re.findall(r"^\| `([^`]+)` \| (\w) \|", text, flags=re.M))
