"""예제 확장(API-17~19): 명령 하나와 메뉴 항목 하나를 더한다. 코어의 공개 API(명령 계층)만 쓴다."""
NAME = "hello_ext"
VERSION = "0.1"
DESCRIPTION = "예제 확장: 재료 수를 세는 명령과 메뉴"


def register(app):
    def count(params):
        materials = app.execute("material.list")
        return {"count": len(materials), "names": [m["name"] for m in materials], "greeting": params.get("greeting", "hello")}

    app.register_command("hello.count_materials", count, kind="Q", desc="재료 수를 센다(예제 확장)", features="API-18",
                         params=[{"name": "greeting", "type": "string", "desc": "인사말", "example": "hello"}])


def ui(window):
    """주 창에 메뉴 항목을 더한다(API-19). PySide6 가 없는 환경(창 없는 실행)에서는 불리지 않는다."""
    from PySide6.QtGui import QAction
    menu = window.menuBar().addMenu("확장(&X)")
    action = QAction("재료 수 세기", window)
    action.triggered.connect(lambda: window._info("재료", window.app.execute("hello.count_materials")))
    menu.addAction(action)
    window.extension_menus = getattr(window, "extension_menus", []) + [menu]
