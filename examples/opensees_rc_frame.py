r"""RC 평면 골조의 지진 해석 예제(OpenSees 솔버, D16): 3층 4경간 골조를 만들어 중력 → 고유치 → 지진 시간이력을 푼다.

실행:  .venv\Scripts\python examples\opensees_rc_frame.py [출력 폴더]
OpenSees 실행 파일은 환경 변수 NASA95_OPENSEES(없으면 ..\third_party\opensees 아래)에서 찾는다.
치수는 RC_Frame_Seismic_Dataset.xlsx 의 건물 1번이다. 바닥 하중(질량)·감쇠비·지진 기록은 가정값이고 골조는 탄성이다
(지진 기록은 사인파를 합쳐 만든 것이다). 출력: 고유주기, 층간변위비, 최대 변위 시점의 변형 그림(b001_peak.png).
"""
import math
import os
import pathlib
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "python"))
if "NASA95_OPENSEES" not in os.environ:
    hits = sorted((root.parent / "third_party" / "opensees").rglob("OpenSees.exe"))
    if hits:
        os.environ["NASA95_OPENSEES"] = str(hits[0])

from nasa95 import App  # noqa: E402

out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="nasa95_rc_frame_")
os.makedirs(out, exist_ok=True)
stories, bays, span = 3, 4, 5.0
heights = [4.0, 3.0, 3.0]
fc = 35.0e6
E = 4700.0 * math.sqrt(fc / 1e6) * 1e6  # ACI 식(Pa)
col, beam = (0.45, 0.45), (0.25, 0.45)   # 폭, 춤
floor_load = 30.0e3                      # 보에 실리는 바닥 하중(N/m) — 가정
g = 9.81

app = App()
conc = app.model.materials.create(name="C35")
conc.set_elastic(data=[[E, 0.2]])
conc.set_density(data=[[2400.0]])
ys = [0.0]
for h in heights:
    ys.append(ys[-1] + h)
grid = {}
for j, y in enumerate(ys):
    ids = app.execute("mesh.nodes_create", coords=[[i * span, y, 0.0] for i in range(bays + 1)])["ids"]
    for i, n in enumerate(ids):
        grid[i, j] = n
columns = app.execute("mesh.elements_create", shape="line2", connectivity=[[grid[i, j], grid[i, j + 1]] for j in range(stories) for i in range(bays + 1)])["ids"]
beams = app.execute("mesh.elements_create", shape="line2", connectivity=[[grid[i, j], grid[i + 1, j]] for j in range(1, stories + 1) for i in range(bays)])["ids"]
joints = [grid[i, j] for j in range(1, stories + 1) for i in range(bays + 1)]
points = app.execute("mesh.elements_create", shape="point1", connectivity=[[n] for n in joints])["ids"]
P = app.model.properties
P.create_beam(name="COLUMN", material=conc.id, section="rect", dimensions=list(col), target={"type": "elements", "ids": columns})
P.create_beam(name="BEAM", material=conc.id, section="rect", dimensions=list(beam), target={"type": "elements", "ids": beams})
# 바닥 하중의 질량을 절점에 모은다(가운데 절점은 한 경간, 가장자리는 반 경간 몫)
inner = [grid[i, j] for j in range(1, stories + 1) for i in range(1, bays)]
edge = [grid[i, j] for j in range(1, stories + 1) for i in (0, bays)]
pid = dict(zip(joints, points))
P.create_mass(name="FLOOR_INNER", mass=floor_load * span / g, target={"type": "elements", "ids": [pid[n] for n in inner]})
P.create_mass(name="FLOOR_EDGE", mass=floor_load * span / 2 / g, target={"type": "elements", "ids": [pid[n] for n in edge]})

# 만든 지진 기록: 0.5~8 Hz 사인파의 합에 포락선을 씌운 것, 최대 0.3 g
dt, duration = 0.01, 20.0
ts, acc = [], []
for k in range(int(duration / dt) + 1):
    t = k * dt
    env = min(t / 2.0, 1.0) * (1.0 if t < 12 else math.exp(-(t - 12) / 2.5))
    a = sum(math.sin(2 * math.pi * f * t + 1.7 * f * f) / math.sqrt(f) for f in (0.5, 0.9, 1.4, 1.74, 2.3, 3.1, 4.5, 6.0, 8.0))
    ts.append(t), acc.append(env * a)
peak = max(abs(a) for a in acc)
quake = app.model.functions.create_amplitude(name="QUAKE", points=[[t, 0.3 * g * a / peak] for t, a in zip(ts, acc)])

case = app.model.cases.create(name="B001", solver="opensees")
case.update(work_directory=os.path.join(out, "b001_work"))
s1 = case.steps.create_static(name="gravity")
s1.bcs.create_displacement(target={"type": "nodes", "ids": [grid[i, 0] for i in range(bays + 1)]}, dofs=[1, 2, 3, 4, 5, 6])
s1.loads.create_gravity(target={"type": "elements", "ids": columns + beams + points}, value=g, direction=[0.0, -1.0, 0.0])
s2 = case.steps.create_frequency(name="modes", num_modes=3)

# 고유치만 먼저 풀어 1·2차 진동수로 5% 레일리 감쇠를 정한다
r = app.execute("case.run", id=case.id, wait=True)
assert r["state"] == "completed", r["errors"]
rid = app.execute("result.open", case=case.id)["id"]
freq = [f["value"] for f in app.execute("result.steps", result=rid) if f["type"] == "frequency"]
print("고유주기 [s]:", [round(1 / f, 4) for f in freq], " (엑셀의 건물 1: 0.576, 0.166, 0.083)")
app.execute("result.close", result=rid)
w1, w2, zeta = 2 * math.pi * freq[0], 2 * math.pi * freq[1], 0.05
case.update(rayleigh_alpha=2 * zeta * w1 * w2 / (w1 + w2), rayleigh_beta=2 * zeta / (w1 + w2))
s3 = case.steps.create_dynamic(name="quake", initial_increment=dt, period=duration)
s3.bcs.create_base_motion(dof=1, motion="acceleration", amplitude=quake.id)
s3.outputs.create_node_file(variables=["U"], frequency=2)
r = app.execute("case.run", id=case.id, wait=True)
assert r["state"] == "completed", (r["errors"], r["log"][-5:])
print("실행:", round(r["elapsed"], 2), "s, 증분", len(r["increments"]), ", 건너뛴 것", r["skipped"])
rid = app.execute("result.open", case=case.id)["id"]
frames = app.execute("result.steps", result=rid)
times = [f for f in frames if f["type"] == "time"]
print("프레임:", len(frames), "(시간이력", len(times), ")")
# 층간변위비: 가장 왼쪽 기둥 줄의 층별 수평 변위 차 / 층고
left = [grid[0, j] for j in range(stories + 1)]
drift = [0.0] * stories
top_peak, peak_frame = 0.0, times[0]["frame"]
for f in times:
    u = app.execute("result.values", result=rid, frame=f["frame"], field="DISP", component="D1", nodes=left)["values"]
    for j in range(stories):
        drift[j] = max(drift[j], abs(u[j + 1] - u[j]) / heights[j] * 100)
    if abs(u[-1]) > top_peak:
        top_peak, peak_frame = abs(u[-1]), f["frame"]
print("최대 층간변위비 [%]:", [round(d, 3) for d in drift], " 지붕 최대 변위 [m]:", round(top_peak, 4), "프레임", peak_frame)
app.execute("view.result_show", result=rid, frame=peak_frame, field="DISP", component="magnitude", deform_scale=40.0)
app.execute("view.standard", name="top")
app.execute("view.fit")
app.execute("view.screenshot", path=os.path.join(out, "b001_peak.png"), width=1000, height=620)
print("그림:", os.path.join(out, "b001_peak.png"))
