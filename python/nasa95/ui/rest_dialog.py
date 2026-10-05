"""REST 서버 상태 표시줄 표시 + 대화 상자(사용자 요청 2026-10-05): 상태 표시줄 오른쪽(모델·NASA-95·Vulkan 옆)의 "REST ON/OFF" 를
클릭하면 켜지고 꺼진다. 켜지면 주소·포트·토큰 대화 상자가 떠서 복사해 에이전트(외부 프로그램)에게 줄 수 있다. 오른쪽 클릭은 대화 상자만.

서버 조작은 명령(server.start / server.stop / server.status / server.configure / server.token_create)으로만 한다.
토큰은 서버가 처음 켜질 때 만든 full 권한 토큰(또는 --token 으로 준 것)을 보여 준다.
"""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QApplication, QCheckBox, QDialog, QDialogButtonBox, QFormLayout, QHBoxLayout, QLabel, QLineEdit, QPushButton,
                               QSpinBox, QVBoxLayout)

from ..api import App, Nasa95Error


def server_status(app: App) -> dict:
    try:
        return app.execute("server.status")
    except Nasa95Error:
        return {"running": False, "host": "127.0.0.1", "port": 8765, "url": ""}


def full_token(app: App) -> str:
    """서버의 full 권한 토큰 하나(없으면 빈 문자열). 서버 객체의 토큰 표를 읽는다(명령으로는 토큰을 되돌려 주지 않는다)."""
    rest = getattr(app, "rest", None)
    tokens = getattr(rest, "tokens", {}) or {}
    for token, scope in tokens.items():
        if scope == "full":
            return token
    return ""


class RestStatusButton(QLabel):
    """상태 표시줄 오른쪽(모델·NASA-95·Vulkan 글자와 같은 모양): 'REST OFF' / 'REST ON'. 왼쪽 클릭 = 켜기·끄기(켜지면 주소·토큰 대화 상자),
    오른쪽 클릭 = 대화 상자만."""

    def __init__(self, app: App, parent=None):
        super().__init__(parent)
        self.app = app
        self.setObjectName("restStatus")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.refresh()

    def is_on(self) -> bool:
        return bool(server_status(self.app).get("running"))

    def refresh(self) -> None:
        st = server_status(self.app)
        on = bool(st.get("running"))
        self.setText("REST ON" if on else "REST OFF")
        self.setProperty("on", "true" if on else "false")
        self.setToolTip((f"REST 서버 켜짐 · {st.get('url', '')} · 클릭: 끄기 · 오른쪽 클릭: 주소·토큰 보기·복사") if on
                        else "REST 서버 꺼짐 · 클릭: 켜기(주소·토큰이 뜬다) · 오른쪽 클릭: 설정")
        self.style().unpolish(self), self.style().polish(self)

    def toggle(self) -> None:
        """켜기·끄기. 켜지면 주소·토큰을 복사할 수 있는 대화 상자를 연다."""
        try:
            if self.is_on():
                self.app.execute("server.stop")
            else:
                self.app.execute("server.start")
                if not full_token(self.app):
                    self.app.execute("server.token_create")
        except Nasa95Error as e:
            self.refresh()
            RestDialog(self.app, parent=self.window(), error=str(e)).exec()
            self.refresh()
            return
        self.refresh()
        if self.is_on():
            self.open_dialog()

    def open_dialog(self) -> None:
        dlg = RestDialog(self.app, parent=self.window())
        dlg.exec()
        self.refresh()

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.toggle()
        elif event.button() == Qt.MouseButton.RightButton:
            self.open_dialog()
        else:
            super().mousePressEvent(event)


class RestDialog(QDialog):
    """주소·포트·토큰 보기·복사, 켜기·끄기."""

    def __init__(self, app: App, parent=None, error: str = ""):
        super().__init__(parent)
        self.app = app
        self.setWindowTitle("REST 서버")
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.state = QLabel()
        form.addRow("상태", self.state)
        self.host = QLineEdit()
        self.port = QSpinBox()
        self.port.setRange(0, 65535)
        self.port.setToolTip("0 이면 빈 포트를 고른다")
        hp = QHBoxLayout()
        hp.addWidget(self.host, 1)
        hp.addWidget(QLabel("포트"))
        hp.addWidget(self.port)
        form.addRow("주소", hp)
        self.url = QLineEdit()
        self.url.setReadOnly(True)
        form.addRow("URL", self._with_copy(self.url, "주소 복사"))
        self.token = QLineEdit()
        self.token.setReadOnly(True)
        form.addRow("토큰", self._with_copy(self.token, "토큰 복사"))
        self.allow_script = QCheckBox("REST 로 script.run 허용(다음에 켤 때부터)")
        form.addRow("", self.allow_script)
        layout.addLayout(form)
        hint = QLabel("에이전트에게 줄 때: 아래 '한 줄 복사' 를 눌러 붙여 넣으면 된다. 요청마다 헤더 Authorization: Bearer <토큰>, 명령은 POST <URL>/commands/<이름>.")
        hint.setWordWrap(True)
        hint.setProperty("role", "muted")
        layout.addWidget(hint)
        row = QHBoxLayout()
        self.toggle = QPushButton()
        self.toggle.clicked.connect(self._toggle)
        self.one_line = QPushButton("한 줄 복사(에이전트용)")
        self.one_line.setToolTip("REST <URL> token=<토큰> 형식으로 클립보드에 복사")
        self.one_line.clicked.connect(self._copy_line)
        new_token = QPushButton("새 토큰")
        new_token.setToolTip("full 권한 토큰을 하나 더 만든다(기존 토큰은 그대로)")
        new_token.clicked.connect(self._new_token)
        row.addWidget(self.toggle)
        row.addWidget(self.one_line)
        row.addWidget(new_token)
        row.addStretch(1)
        layout.addLayout(row)
        self.error = QLabel()
        self.error.setProperty("role", "error")
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.resize(560, 300)
        self.refresh()
        if error:
            self.error.setText(error)

    def _with_copy(self, edit: QLineEdit, label: str):
        row = QHBoxLayout()
        row.addWidget(edit, 1)
        btn = QPushButton(label)
        btn.clicked.connect(lambda: QApplication.clipboard().setText(edit.text()))
        row.addWidget(btn)
        return row

    def refresh(self) -> None:
        st = server_status(self.app)
        running = bool(st.get("running"))
        self.state.setText("켜짐" if running else "꺼짐")
        self.host.setText(st.get("host", "127.0.0.1"))
        self.port.setValue(int(st.get("port", 8765)))
        self.host.setEnabled(not running), self.port.setEnabled(not running)
        self.url.setText(st.get("url", "") if running else "")
        self.token.setText(full_token(self.app) if running else "")
        self.allow_script.blockSignals(True)
        self.allow_script.setChecked(bool(st.get("allow_script")))
        self.allow_script.blockSignals(False)
        self.toggle.setText("끄기" if running else "켜기")
        self.one_line.setEnabled(running)

    def _toggle(self) -> None:
        self.error.setText("")
        try:
            st = server_status(self.app)
            if st.get("running"):
                self.app.execute("server.stop")
            else:
                self.app.execute("server.configure", host=self.host.text().strip() or "127.0.0.1", port=self.port.value(),
                                 allow_script=self.allow_script.isChecked())
                self.app.execute("server.start")
                if not full_token(self.app):
                    self.app.execute("server.token_create")
        except Nasa95Error as e:
            self.error.setText(str(e))
        self.refresh()

    def _new_token(self) -> None:
        try:
            token = self.app.execute("server.token_create")["token"]
            self.token.setText(token)
        except Nasa95Error as e:
            self.error.setText(str(e))

    def one_line_text(self) -> str:
        return f"REST {self.url.text()} token={self.token.text()}"

    def _copy_line(self) -> None:
        QApplication.clipboard().setText(self.one_line_text())
        self.error.setText("")
        self.state.setText("켜짐 · 복사했습니다")
