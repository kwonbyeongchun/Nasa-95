"""QSS·팔레트·사용자 UI 설정. 모델 데이터와 독립적으로 저장한다."""
import os
from pathlib import Path

from PySide6.QtCore import QObject, QSettings, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPalette
from PySide6.QtWidgets import QApplication

COLORS = {
    # VS Code Light Modern / Dark Modern workbench 색상 기준.
    # https://github.com/microsoft/vscode/tree/main/extensions/theme-defaults/themes
    "light": dict(bg="#FFFFFF", viewport="#FFFFFF", panel="#F8F8F8", input="#FFFFFF", text="#3B3B3B", muted="#616161",
                  border="#E5E5E5", inputborder="#CECECE", hover="#F2F2F2", selected="#E8E8E8", ribbon="#F3F3F3", group_title="#E9E9E9",
                  accent="#005FB8", error="#C72E0F", disabled="#909090", scroll="#C1C1C1",
                  blue="#005FB8", green="#388A34", orange="#B07800", purple="#8B46A8"),
    "dark": dict(bg="#1F1F1F", viewport="#3A3A3A", panel="#181818", input="#313131", text="#CCCCCC", muted="#9D9D9D",
                 border="#2B2B2B", inputborder="#3C3C3C", hover="#2A2D2E", selected="#37373D", ribbon="#181818", group_title="#222222",
                 accent="#0078D4", error="#F85149", disabled="#6E6E6E", scroll="#484848",
                 blue="#75BEFF", green="#89D185", orange="#D7BA7D", purple="#C586C0"),
}


def ui_settings() -> QSettings:
    # 테스트의 NASA95_SETTINGS 격리를 UI 설정에도 적용한다.
    location = os.environ.get("NASA95_UI_SETTINGS")
    if not location and os.environ.get("NASA95_SETTINGS"):
        location = str(Path(os.environ["NASA95_SETTINGS"]).with_suffix(".ui.ini"))
    if location:
        return QSettings(location, QSettings.Format.IniFormat)
    return QSettings(QSettings.Format.IniFormat, QSettings.Scope.UserScope, "NASA-95", "ui")


class Theme(QObject):
    changed = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.settings = ui_settings()
        self.mode = str(self.settings.value("theme", "light"))
        if self.mode not in ("light", "dark", "system"):
            self.mode = "light"
        self.compact = self.settings.value("compact", False, type=bool)
        QApplication.instance().styleHints().colorSchemeChanged.connect(self._system_changed)
        self.apply()

    @property
    def colors(self):
        dark = self.mode == "dark" or (self.mode == "system" and QApplication.instance().styleHints().colorScheme() == Qt.ColorScheme.Dark)
        return COLORS["dark" if dark else "light"]

    def _system_changed(self, _scheme):
        if self.mode == "system":
            self.apply()

    def set_mode(self, mode):
        if mode not in ("light", "dark", "system"):
            raise ValueError(mode)
        self.mode = mode
        self.settings.setValue("theme", mode)
        self.apply()

    def set_compact(self, compact):
        self.compact = compact
        self.settings.setValue("compact", compact)
        self.apply()

    def apply(self):
        app = QApplication.instance()
        app.setStyle("Fusion")
        font = QFont("Segoe UI")
        font.setPixelSize(13)
        app.setFont(font)
        c = self.colors
        palette = QPalette()
        for role, key in ((QPalette.ColorRole.Window, "bg"), (QPalette.ColorRole.Base, "input"),
                          (QPalette.ColorRole.AlternateBase, "hover"), (QPalette.ColorRole.Button, "panel"),
                          (QPalette.ColorRole.WindowText, "text"), (QPalette.ColorRole.Text, "text"),
                          (QPalette.ColorRole.ButtonText, "text"), (QPalette.ColorRole.ToolTipBase, "panel"),
                          (QPalette.ColorRole.ToolTipText, "text"), (QPalette.ColorRole.Highlight, "selected"),
                          (QPalette.ColorRole.HighlightedText, "text"), (QPalette.ColorRole.PlaceholderText, "muted")):
            palette.setColor(role, QColor(c[key]))
        for role in (QPalette.ColorRole.WindowText, QPalette.ColorRole.Text, QPalette.ColorRole.ButtonText):
            palette.setColor(QPalette.ColorGroup.Disabled, role, QColor(c["disabled"]))
        app.setPalette(palette)
        qss = Path(__file__).with_name("themes").joinpath("base.qss").read_text(encoding="utf-8")
        for key, value in c.items():
            qss = qss.replace("@" + key + "@", value)
        qss = qss.replace("@assets@", Path(__file__).with_name("themes").as_posix())
        qss = qss.replace("@variant@", "dark" if c is COLORS["dark"] else "light")
        qss = qss.replace("@rowheight@", "18" if self.compact else "22")
        app.setStyleSheet(qss)
        self.changed.emit()

    def apply_window_frame(self, window):
        """Windows 기본 제목 표시줄도 테마에 맞춘다. 지원되지 않는 OS는 기본값을 유지한다."""
        if os.name != "nt":
            return
        import ctypes
        from ctypes import wintypes
        try:
            setter = ctypes.windll.dwmapi.DwmSetWindowAttribute
            setter.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
            setter.restype = ctypes.c_long
            def set_value(attribute, value):
                data = wintypes.DWORD(value)
                setter(int(window.winId()), attribute, ctypes.byref(data), ctypes.sizeof(data))
            set_value(20, int(self.colors is COLORS["dark"]))
            for attribute, key in ((35, "panel"), (36, "text")):
                c = QColor(self.colors[key])
                set_value(attribute, c.red() | (c.green() << 8) | (c.blue() << 16))
        except (AttributeError, OSError):
            pass
