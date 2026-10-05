r"""대표 해석 예제 31개(X:\project_files_git\open-fep\test_file\examples_lib.py — CalculiX 11(선박 화물창 01_CAL_11 포함, 2~5 분)·MyStran 10·OpenSees 10)가 로컬 App 으로 풀리고
닫힌 해·다른 솔버와의 대조를 통과하는지(CAS-T12). 솔버 실행 파일이 없는 그룹은 건너뛴다. 예제 폴더의 project.nasa95 는 건드리지 않는다(save=False).
REST 재현은 창이 있어야 하므로 여기서는 돌리지 않는다(`python examples_lib.py rest all`)."""
import os
import pathlib
import sys

import pytest

EXAMPLES_DIR = pathlib.Path(__file__).resolve().parents[3] / "test_file"
pytestmark = pytest.mark.skipif(not (EXAMPLES_DIR / "examples_lib.py").exists(), reason="test_file/examples_lib.py 가 없습니다")
if (EXAMPLES_DIR / "examples_lib.py").exists():
    sys.path.insert(0, str(EXAMPLES_DIR))
    import examples_lib as L  # noqa: E402
    NAMES = list(L.EXAMPLES)
else:
    NAMES = []


@pytest.mark.feature("CAS-44")
@pytest.mark.feature("CAS-45")
@pytest.mark.parametrize("name", NAMES)
def test_CAS_T12_01_examples(name, monkeypatch):
    solver = L.solver_of(name)
    exe = L.solver_exe(solver)
    if not exe:
        pytest.skip(f"{solver} 실행 파일이 없습니다")
    monkeypatch.setenv({"calculix": "NASA95_CCX", "mystran": "NASA95_MYSTRAN", "opensees": "NASA95_OPENSEES"}[solver], exe)
    ok, checks, elapsed = L.run_example(name, L.LocalExec(), save=False)
    assert ok, [c for c in checks if not c["ok"]]
