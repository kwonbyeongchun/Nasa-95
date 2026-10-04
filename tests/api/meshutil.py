"""테스트용 메시 만들기. 형상·메셔 없이 API 로 직접 만든다."""
from __future__ import annotations

import numpy as np

from openfep import App


def block(app: App, nx: int, ny: int, nz: int, size=(100.0, 20.0, 10.0), part: int | None = None,
          origin=(0.0, 0.0, 0.0)) -> dict:
    """직육면체를 육면체 요소 nx×ny×nz 로 나눈 메시. 돌려주는 값: 노드·요소 ID 범위와 노드 번호 함수."""
    xs = np.linspace(0, size[0], nx + 1) + origin[0]
    ys = np.linspace(0, size[1], ny + 1) + origin[1]
    zs = np.linspace(0, size[2], nz + 1) + origin[2]
    kk, jj, ii = np.meshgrid(np.arange(nz + 1), np.arange(ny + 1), np.arange(nx + 1), indexing="ij")
    coords = np.column_stack([xs[ii.ravel()], ys[jj.ravel()], zs[kk.ravel()]])
    r = app.execute("mesh.nodes_create", coords=coords)
    first = r["first"]

    def node(i: int, j: int, k: int) -> int:
        return first + i + (nx + 1) * (j + (ny + 1) * k)

    conn = []
    for k in range(nz):
        for j in range(ny):
            for i in range(nx):
                conn.append([node(i, j, k), node(i + 1, j, k), node(i + 1, j + 1, k), node(i, j + 1, k),
                             node(i, j, k + 1), node(i + 1, j, k + 1), node(i + 1, j + 1, k + 1), node(i, j + 1, k + 1)])
    params = dict(shape="hex8", connectivity=np.array(conn, dtype=np.int64))
    if part is not None:
        params["part"] = part
    e = app.execute("mesh.elements_create", **params)
    return {"nodes": (r["first"], r["last"]), "elements": (e["first"], e["last"]), "node": node}


def plate(app: App, nx: int, ny: int, size=(10.0, 4.0), shape: str = "quad4") -> dict:
    """z=0 평면의 사각형(또는 삼각형) 쉘 메시."""
    xs, ys = np.linspace(0, size[0], nx + 1), np.linspace(0, size[1], ny + 1)
    jj, ii = np.meshgrid(np.arange(ny + 1), np.arange(nx + 1), indexing="ij")
    coords = np.column_stack([xs[ii.ravel()], ys[jj.ravel()], np.zeros(ii.size)])
    r = app.execute("mesh.nodes_create", coords=coords)
    first = r["first"]

    def node(i: int, j: int) -> int:
        return first + i + (nx + 1) * j

    conn = []
    for j in range(ny):
        for i in range(nx):
            q = [node(i, j), node(i + 1, j), node(i + 1, j + 1), node(i, j + 1)]
            conn += [q] if shape == "quad4" else [[q[0], q[1], q[2]], [q[0], q[2], q[3]]]
    e = app.execute("mesh.elements_create", shape=shape, connectivity=conn)
    return {"nodes": (r["first"], r["last"]), "elements": (e["first"], e["last"]), "node": node}
