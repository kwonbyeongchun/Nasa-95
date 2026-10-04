"""스케치 모드 확장(참고 구현, API-19): 뷰포트에서 마우스로 스케치 요소를 그린다.

흐름: 패널에서 파트와 평면(기준 평면 또는 평면 면)을 고르고 "스케치 시작" → 도구(선·사각형·원·호·점)를 고르고 뷰포트에서 클릭·끌기 →
`sketch.point_from_screen`(픽셀 → 평면 점) → `sketch.snap`(끝점·중심·수평/수직·격자) → `sketch.add_*`. 치수는 칸에 적어 `sketch.add_dimension`.
"완료" 는 돌출 양식(`feature.create_extrude`)을 연다. 코어의 명령 계층만 쓰고 뷰포트 위젯은 이벤트 필터로만 건드린다(렌더러·창 코드 수정 없음).
표시(요소·고무줄)는 렌더러의 몫(RND-38)이라 이 확장은 그리지 않는다 — 패널의 좌표·상태 글로 확인한다.
"""
from __future__ import annotations

NAME = "sketch_mode"
VERSION = "0.1"
DESCRIPTION = "마우스로 스케치 요소를 그리는 스케치 모드(참고 구현)"


def register(app):  # 새 명령은 없다. 코어의 sketch.* 를 그대로 쓴다
    return None


def ui(window):
    from PySide6.QtCore import QEvent, QObject, Qt
    from PySide6.QtWidgets import (QComboBox, QDockWidget, QDoubleSpinBox, QFormLayout, QHBoxLayout, QLabel, QLineEdit, QPushButton, QVBoxLayout,
                                   QWidget)

    app = window.app

    class SketchMode(QObject):
        TOOLS = ["select", "line", "rectangle", "circle", "arc", "point"]

        def __init__(self):
            super().__init__(window)
            self.sketch = None       # 스케치 객체 ID
            self.tool = "line"
            self.points = []         # 그리는 중 모은 평면 점 [u, v]
            self.last = None         # 마지막으로 찍은 점(연속 선 그리기)
            self.active = False
            self._build()
            window.viewport.installEventFilter(self)

        # ------------------------------------------------------------ 패널
        def _build(self):
            body = QWidget()
            form = QFormLayout(body)
            self.part_box = QComboBox()
            self.plane_box = QComboBox()
            self.refresh_button = QPushButton("목록 새로 고침")
            self.refresh_button.clicked.connect(self.refresh_lists)
            row = QHBoxLayout()
            row.addWidget(self.part_box, 1)
            row.addWidget(self.refresh_button)
            form.addRow("파트", row)
            form.addRow("평면", self.plane_box)
            self.start_button = QPushButton("스케치 시작")
            self.start_button.clicked.connect(self.start)
            self.stop_button = QPushButton("스케치 끝(완료 → 돌출)")
            self.stop_button.clicked.connect(self.finish)
            form.addRow(self.start_button, self.stop_button)
            self.tool_box = QComboBox()
            for t in self.TOOLS:
                self.tool_box.addItem({"select": "선택", "line": "선", "rectangle": "사각형", "circle": "원", "arc": "호(세 점)", "point": "점"}[t], t)
            self.tool_box.setCurrentIndex(1)
            self.tool_box.currentIndexChanged.connect(lambda _i: self.set_tool(self.tool_box.currentData()))
            form.addRow("도구", self.tool_box)
            self.grid = QDoubleSpinBox()
            self.grid.setRange(0.0, 1e6)
            self.grid.setValue(1.0)
            self.snap_tol = QDoubleSpinBox()
            self.snap_tol.setRange(0.0, 1e6)
            self.snap_tol.setValue(0.5)
            form.addRow("격자 간격", self.grid)
            form.addRow("스냅 거리", self.snap_tol)
            self.dim_kind = QComboBox()
            for k in ("length", "radius", "diameter", "distance", "horizontal_distance", "vertical_distance", "angle"):
                self.dim_kind.addItem(k)
            self.dim_entities = QLineEdit()
            self.dim_entities.setPlaceholderText("요소 번호: 1 또는 1, 2")
            self.dim_value = QLineEdit()
            self.dim_value.setPlaceholderText("값")
            self.dim_button = QPushButton("치수 추가")
            self.dim_button.clicked.connect(self.add_dimension)
            dim = QHBoxLayout()
            dim.addWidget(self.dim_kind)
            dim.addWidget(self.dim_entities, 1)
            dim.addWidget(self.dim_value)
            dim.addWidget(self.dim_button)
            form.addRow("치수", dim)
            # 좌표를 쳐서 점을 찍는다(마우스 클릭과 같은 효과: 도구의 다음 점이 된다)
            self.typed_u = QLineEdit()
            self.typed_u.setPlaceholderText("u")
            self.typed_v = QLineEdit()
            self.typed_v.setPlaceholderText("v")
            self.typed_button = QPushButton("점 찍기")
            self.typed_button.clicked.connect(self.add_typed_point)
            typed = QHBoxLayout()
            typed.addWidget(self.typed_u)
            typed.addWidget(self.typed_v)
            typed.addWidget(self.typed_button)
            form.addRow("좌표 입력", typed)
            self.status = QLabel("파트와 평면을 고르고 스케치를 시작하세요.")
            self.status.setWordWrap(True)
            form.addRow(self.status)
            self.coords = QLabel("")
            form.addRow("마우스", self.coords)
            self.entities = QLabel("")
            self.entities.setWordWrap(True)
            form.addRow("요소", self.entities)
            dock = QDockWidget("스케치", window)
            dock.setObjectName("sketch_mode_dock")
            dock.setWidget(body)
            window.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
            self.dock = dock
            self.refresh_lists()

        def refresh_lists(self):
            self.part_box.clear()
            for p in app.execute("part.list"):
                self.part_box.addItem(f"{p['name']} (#{p['id']})", p["id"])
            self.plane_box.clear()
            self.plane_box.addItem("XY 평면 (z = 0)", {"point": [0, 0, 0], "normal": [0, 0, 1], "x_axis": [1, 0, 0]})
            self.plane_box.addItem("YZ 평면 (x = 0)", {"point": [0, 0, 0], "normal": [1, 0, 0], "x_axis": [0, 1, 0]})
            self.plane_box.addItem("XZ 평면 (y = 0)", {"point": [0, 0, 0], "normal": [0, 1, 0], "x_axis": [1, 0, 0]})
            for d in app.execute("datum.list"):
                if d.get("type") == "plane":
                    self.plane_box.addItem(f"기준 평면 {d['name']} (#{d['id']})", {"datum": d["id"]})
            # 화면에서 고른 평면 면
            for h in getattr(window, "_picked", []) or []:
                if h.get("kind") == "face":
                    try:
                        info = app.execute("geometry.entity_info", id=h["part"], type="face", index=h["index"])
                    except Exception:  # noqa: BLE001
                        continue
                    if info.get("surface") == "plane" and info.get("normal"):
                        self.plane_box.addItem(f"선택한 면 (파트 #{h['part']} 면 {h['index']})", {"point": info["center"], "normal": info["normal"]})

        # ------------------------------------------------------------ 상태
        def start(self):
            # 트리에서 스케치를 골라 두었으면 그 스케치를 이어서 그린다(새로 만들지 않음)
            selected = getattr(window, "_selected", None)
            if selected and selected[0] == "sketch":
                self.sketch = selected[1]
            else:
                part = self.part_box.currentData()
                plane = self.plane_box.currentData()
                if part is None or plane is None:
                    self.status.setText("파트와 평면을 고르세요(또는 트리에서 스케치를 고르세요).")
                    return
                params = {"parent": part, "name": "sketch"}
                params.update(plane)
                self.sketch = app.execute("sketch.create", **params)["id"]
            self.active, self.points, self.last = True, [], None
            self.status.setText(f"스케치 #{self.sketch} 그리는 중 — 도구: {self.tool}. 클릭으로 점을 찍고, Esc 로 취소합니다.")
            self.update_entities()

        def set_tool(self, tool):
            self.tool, self.points, self.last = tool, [], None
            if self.active:
                self.status.setText(f"도구: {tool}")

        def finish(self):
            if not self.sketch:
                return
            from openfep.ui.forms import CommandDialog
            sk = self.sketch
            self.active = False
            profiles = app.execute("sketch.profiles", id=sk)
            self.status.setText(f"스케치 #{sk} 끝. 닫힌 영역 {profiles['count']}개.")
            if profiles["count"]:
                part = app.execute("sketch.get", id=sk)["parent"]
                # 기본 프로파일 = 둘레 모서리가 가장 많은 영역(사각형 안에 원이 있으면 구멍 뚫린 고리). 양식에서 바꿀 수 있다
                ring = max(profiles["profiles"], key=lambda q: (q["edges"], q["area"]))["profile"]
                self.status.setText(f"스케치 #{sk} 끝. 닫힌 영역 {profiles['count']}개: " + ", ".join(f"{q['profile']}: 넓이 {q['area']:.1f}" for q in profiles["profiles"]))
                dlg = CommandDialog(app, "feature.create_extrude", fixed={"parent": part, "sketch": sk},
                                    current={"profile": ring, "distance": 10.0, "direction": [0.0, 0.0, 1.0]}, parent=window)
                dlg.exec()

        # ------------------------------------------------------------ 마우스
        def plane_point(self, pos):
            vp = window.viewport
            r = app.execute("sketch.point_from_screen", id=self.sketch, x=float(pos.x()), y=float(pos.y()), width=max(vp.width(), 1), height=max(vp.height(), 1))
            if not r.get("hit"):
                return None, "평면을 옆에서 보고 있어 점을 찍을 수 없습니다"
            s = app.execute("sketch.snap", id=self.sketch, uv=r["uv"], tolerance=self.snap_tol.value(), grid=self.grid.value() or None,
                            **({"from": self.last} if self.last else {}))
            return s["uv"], s["kind"]

        def eventFilter(self, obj, event):
            if not self.active or not self.sketch:
                return False
            t = event.type()
            if t == QEvent.Type.KeyPress and event.key() == Qt.Key.Key_Escape:
                self.points, self.last = [], None
                self.status.setText("취소. 다음 요소를 그리세요.")
                return True
            if t == QEvent.Type.MouseMove:
                uv, kind = self.plane_point(event.position().toPoint())
                self.coords.setText("" if uv is None else f"u = {uv[0]:.3f}, v = {uv[1]:.3f}  ({kind})")
                return False  # 카메라 조작(끌기)은 그대로 둔다
            if t == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton and self.tool != "select":
                uv, kind = self.plane_point(event.position().toPoint())
                if uv is None:
                    self.status.setText(kind)
                    return True
                self.points.append(uv)
                self.commit_if_ready()
                return True  # 그리는 중에는 왼쪽 클릭이 선택·회전으로 가지 않게
            return False

        def commit_if_ready(self):
            sk, pts = self.sketch, self.points
            try:
                if self.tool == "point":
                    app.execute("sketch.add_point", id=sk, position=pts[0])
                    self.points = []
                elif self.tool == "line" and len(pts) == 2:
                    if pts[0] != pts[1]:
                        app.execute("sketch.add_line", id=sk, start=pts[0], end=pts[1])
                    self.points, self.last = [pts[1]], pts[1]  # 이어서 그린다
                elif self.tool == "rectangle" and len(pts) == 2:
                    u0, v0 = min(pts[0][0], pts[1][0]), min(pts[0][1], pts[1][1])
                    w, h = abs(pts[1][0] - pts[0][0]), abs(pts[1][1] - pts[0][1])
                    if w > 0 and h > 0:
                        app.execute("sketch.add_rectangle", id=sk, corner=[u0, v0], size=[w, h])
                    self.points, self.last = [], None
                elif self.tool == "circle" and len(pts) == 2:
                    r = ((pts[1][0] - pts[0][0]) ** 2 + (pts[1][1] - pts[0][1]) ** 2) ** 0.5
                    if r > 0:
                        app.execute("sketch.add_circle", id=sk, center=pts[0], radius=r)
                    self.points, self.last = [], None
                elif self.tool == "arc" and len(pts) == 3:
                    app.execute("sketch.add_arc", id=sk, start=pts[0], middle=pts[1], end=pts[2])
                    self.points, self.last = [], None
                else:
                    self.last = pts[-1] if self.tool == "line" else self.last
                    self.status.setText(f"{self.tool}: 점 {len(pts)}개 찍음, 더 찍으세요.")
                    return
                self.status.setText(f"{self.tool} 추가. 다음 요소를 그리세요.")
            except Exception as e:  # noqa: BLE001 — 구조화된 오류를 그대로 보인다
                self.points, self.last = [], None
                self.status.setText(f"오류: {e}")
            self.update_entities()

        def add_typed_point(self):
            """좌표 칸의 (u, v) 를 클릭한 것처럼 점으로 더한다. Enter 대신 단추. 선 도구면 두 번 찍으면 선이 된다."""
            if not self.active or not self.sketch:
                self.status.setText("먼저 스케치를 시작하세요.")
                return
            try:
                uv = [float(self.typed_u.text()), float(self.typed_v.text())]
            except ValueError:
                self.status.setText("u, v 에 숫자를 적으세요.")
                return
            self.points.append(uv)
            self.commit_if_ready()
            self.typed_u.clear(), self.typed_v.clear()
            self.typed_u.setFocus()

        def add_dimension(self):
            if not self.sketch:
                return
            try:
                ents = [int(x) for x in self.dim_entities.text().replace(" ", "").split(",") if x]
                r = app.execute("sketch.add_dimension", id=self.sketch, kind=self.dim_kind.currentText(), value=float(self.dim_value.text()), entities=ents)
                st = app.execute("sketch.solve_status", id=self.sketch)
                self.status.setText(f"치수 추가(#{r.get('id', '?')}). 구속 상태: {st['status']}, 자유도 {st['dof']}")
            except Exception as e:  # noqa: BLE001
                self.status.setText(f"오류: {e}")
            self.update_entities()

        def update_entities(self):
            if not self.sketch:
                return
            ents = app.execute("sketch.get", id=self.sketch)["props"].get("entities", [])
            self.entities.setText(", ".join(f"{e['id']}:{e['kind']}" for e in ents) or "(없음)")

    window.sketch_mode = SketchMode()
