"""해석 구성 트리: 같은 창의 두 트리 사이에서 참조를 추가·제외한다."""
from PySide6.QtCore import QMimeData, Qt, Signal
from PySide6.QtGui import QDrag
from PySide6.QtWidgets import QAbstractItemView, QHeaderView, QStyledItemDelegate, QTreeWidget

KEY = Qt.ItemDataRole.UserRole
STEP = Qt.ItemDataRole.UserRole + 1
MIME = "application/x-nasa95-case-items"


class FactorDelegate(QStyledItemDelegate):
    def createEditor(self, parent, option, index):
        key = index.siblingAtColumn(0).data(KEY)
        if index.column() == 1 and key and key[0] == "load_set":
            return super().createEditor(parent, option, index)
        return None


class CaseTree(QTreeWidget):
    transferRequested = Signal(object)
    dropRequested = Signal(object, object)

    def __init__(self, included=False, parent=None):
        super().__init__(parent)
        self.peer = None
        self.included = included
        self.setColumnCount(2)
        self.setHeaderLabels(["항목", "계수" if included else "내용"])
        self.header().setStretchLastSection(False)
        self.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.header().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.setDragDropMode(QAbstractItemView.DragDropMode.DragDrop)
        self.setDefaultDropAction(Qt.DropAction.CopyAction)
        self.setDragEnabled(True)
        self.setAcceptDrops(True)
        self.setDropIndicatorShown(True)
        self.setExpandsOnDoubleClick(False)
        self.setItemDelegate(FactorDelegate(self))
        self.setEditTriggers(QAbstractItemView.EditTrigger.DoubleClicked | QAbstractItemView.EditTrigger.EditKeyPressed)
        self.itemDoubleClicked.connect(self._double_click)

    def _double_click(self, item, column):
        key = item.data(0, KEY)
        if key and not (self.included and column == 1 and key[0] == "load_set"):
            self.transferRequested.emit([item])
        elif not item.data(0, KEY):
            item.setExpanded(not item.isExpanded())

    def startDrag(self, actions):
        # Qt의 기본 MoveAction은 원본 행을 지운다. 실제 변경은 창의 참조 목록이 맡는다.
        if not self.selectedItems():
            return
        mime = QMimeData()
        mime.setData(MIME, b"references")
        drag = QDrag(self)
        drag.setMimeData(mime)
        drag.exec(Qt.DropAction.CopyAction)

    def _accepts(self, event):
        return event.source() is self.peer and event.mimeData().hasFormat(MIME)

    def dragEnterEvent(self, event):
        if self._accepts(event):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event):
        self.dragEnterEvent(event)

    def dropEvent(self, event):
        if self._accepts(event):
            self.dropRequested.emit(self.peer.selectedItems(), self.itemAt(event.position().toPoint()))
            event.acceptProposedAction()
        else:
            event.ignore()

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Delete) and self.state() != QAbstractItemView.State.EditingState:
            self.transferRequested.emit(self.selectedItems())
            event.accept()
        else:
            super().keyPressEvent(event)
