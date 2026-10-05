# NASA-95

CAE Pre/Post 프로세서(유한요소 해석 전·후처리). 모델은 솔버 중립이고 CalculiX·OpenSees·MyStran 을 별도 실행 파일로 돌린다. 구현 진행 중이다.
(2026-10-05 까지의 이름은 open-fep 였다. 저장소를 https://github.com/kwonbyeongchun/Nasa-95.git 로 옮기면서 패키지 `nasa95`·모듈 `_nasa95`·환경 변수 `NASA95_*`·프로젝트 파일 `.nasa95` 로 바꿨다. 옛 `.ofep` 프로젝트 파일은 그대로 열린다.)

- 설계·계획 문서: 작업 저장소 `open-fep-works` (`agent.md`, `.agent/`, `plans/`)
- 구성: C++ 코어(`core/`, OpenCASCADE 형상·Netgen 메싱) + Vulkan 렌더러(`render/`) + Python API(`python/nasa95/`) + PySide6 UI(`python/nasa95/ui/`). 모든 기능은 코어의 명령 계층을 거친다.
- 외부 준비물(저장소 밖 `..\third_party\`): CalculiX ccx, OpenCASCADE 8.0.1, Netgen 빌드, Vulkan SDK. 없으면 그 부분 없이 빌드된다.

## 빌드와 테스트 (Windows, Visual Studio 2022)

```powershell
python -m venv --system-site-packages .venv
.venv\Scripts\python -m pip install pybind11 pytest
.\scripts\build.ps1
.\scripts\test.ps1
```

UI 실행: `nasa-95.bat` 또는 `.\scripts\ui.ps1`.

## 배포본과 설치 파일

```powershell
.\scripts\ci-deps.ps1        # 외부 의존성 준비(OpenCASCADE 8.0.1 내려받기, Netgen 소스 빌드, Vulkan SDK 연결) — 새 PC·CI
.\scripts\package.ps1        # dist\NASA-95\ (임베디드 Python 포터블) + dist\NASA-95-<버전>-win64.zip + NASA-95-<버전>-setup.exe (Inno Setup)
```
GitHub Actions(`.github/workflows/release.yml`): 태그 `v<버전>` 을 푸시하면 Windows 러너가 빌드해 그 태그의 Release 에 설치 파일과 zip 을 올린다.
수동 실행은 아티팩트만 남긴다(`publish` 를 켜면 Release 도 만든다). 솔버(CalculiX·OpenSees·MyStran)는 배포본에 넣지 않는다 — 설정에서 실행 파일 경로를 지정한다.

### 리본 UI 의존성

UI는 SARibbon v2.9.5(MIT)의 공식 PySide6 바인딩을 사용한다. Windows x64 / Python 3.12 / PySide6·Qt 6.11.1로 버전을 맞춘다.

```powershell
.venv\Scripts\python -m pip install PySide6==6.11.1
.\scripts\setup_ribbon.ps1
```

처음 실행 시 Git과 Visual Studio 2022 C++ 빌드 도구가 필요하다. 스크립트는 `..\third_party\`에 고정된 SARibbon 소스와 Qt 개발 파일, 별도 빌드 환경을 준비하고 `.venv`에 바인딩과 MIT 라이선스를 설치한다. 기존 설치를 다시 빌드하려면 `-Force`, 다른 외부 의존성 폴더를 사용하려면 `-ThirdParty <경로>`를 지정한다. 런타임 Qt DLL은 PySide6의 것을 동적으로 사용한다. SARibbon은 UI에만 필요하다.

홈·형상·메시 등의 리본 탭은 제목 표시줄에 표시된다. 화면 설정에서 라이트·다크·시스템 테마를 고를 수 있다. 좁은 창에서는 명령 검색이 숨겨지며 `Ctrl+Shift+P`로 열 수 있다.

## 사용 예

```python
import sys; sys.path.insert(0, "python")
from nasa95 import App

app = App()                                   # 창 없이 실행
part = app.model.parts.create(name="beam")
part.features.create_box(size=[100.0, 20.0, 10.0])
app.execute("mesh.generate", id=part.id, size=5.0)          # Netgen 사면체 메시
steel = app.model.materials.create(name="steel")
steel.set_elastic(data=[[210000.0, 0.3]])                   # 구성 모델은 표로 준다([E, nu] 행)
mesh_part = app.execute("mesh_part.list")[0]["id"]
app.model.properties.create_solid(material=steel.id, target={"type": "parts", "ids": [mesh_part]})
case = app.model.cases.create(name="static")
step = case.steps.create_static()
root = {"type": "geometry", "ids": [[part.id, "face", 1]]}   # 적용 대상: 형상 엔티티·노드·요소·셋
step.bcs.create_displacement(target=root, dofs=[1, 2, 3])
step.loads.create_force(target={"type": "nodes", "ids": [8]}, components=[0.0, 0.0, -100.0])
print(app.execute("case.preview_deck", id=case.id)["text"][:200])
app.undo()                                    # 모든 변경 명령은 되돌릴 수 있다
print(app.execute("project.tree"))
```

솔버 실행은 `app.execute("case.run", id=case.id, wait=True)` (ccx 경로: 환경 변수 `NASA95_CCX` 또는 케이스의 `solver_executable`),
결과는 `app.execute("result.open", case=case.id)` 로 연다. 명령 목록은 `app.commands()`, 전체 정의는 작업 저장소의 `.agent/proj-api-full-list.md`.
