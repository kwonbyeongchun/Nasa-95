"""프로젝트 자체 SVG 아이콘. 리본과 모델 트리가 같은 도형을 사용한다."""
from functools import lru_cache

from PySide6.QtCore import QByteArray, Qt
from PySide6.QtGui import QIcon, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer

PATHS = {
    "window_minimize": '<path d="M4 12h16"/>',
    "window_maximize": '<rect x="4" y="4" width="16" height="16"/>',
    "window_restore": '<path d="M8 8V4h12v12h-4M4 8h12v12H4z"/>',
    "window_close": '<path d="m5 5 14 14M5 19 19 5"/>',
    "explorer": '<path d="M8 3h12v15H8zM4 7H3v15h12v-1"/>',
    "console": '<path d="m4 6 5 5-5 5M12 17h8"/>',
    "collapse_all": '<path d="M8 3h12v14M4 7h12v14H4zM7 14h6"/>',
    "expand_all": '<path d="M8 3h12v14M4 7h12v14H4zM7 14h6m-3-3v6"/>',
    "filter": '<path d="M3 5h18M6 11h12M10 17h4"/>',
    "box": '<path d="m12 3 9 5v9l-9 5-9-5V8zM3 8l9 5 9-5M12 13v9"/>',
    "folder": '<path d="M3 7V5h7l2 3h9v12H3z"/>',
    "palette": '<path d="M12 3a9 9 0 1 0 0 18c1.5 0 2-1 2-2s-1-1.5-1-2.5 1-1.5 2.5-1.5H18a3 3 0 0 0 3-3c0-5-4-9-9-9z"/><circle cx="7.5" cy="11" r="1.2"/><circle cx="10" cy="7" r="1.2"/><circle cx="15" cy="7" r="1.2"/>',
    "mesh": '<path d="M7 5h14l-4 14H3z"/><path d="M14 5l-4 14M5 12h14" stroke-width="1.2"/>',  # 살짝 기울어진 평행사변형 + 2×2 요소 격자
    "node_labels": '<circle cx="5" cy="6" r="2"/><circle cx="5" cy="18" r="2"/><circle cx="13" cy="12" r="2"/><path d="m7 7 4 4M7 17l4-4M18 7l3-2v14m-3 0h5"/>',
    "element_labels": '<path d="M3 5h12v14H3zM3 5l12 14M18 7l3-2v14m-3 0h5"/>',
    "material": '<path d="m12 3 9 5-9 5-9-5zM3 12l9 5 9-5M3 16l9 5 9-5"/>',
    "property": '<path d="M5 3h14v18H5zM8 7h8M8 11h8M8 15h5"/>',
    "section": '<path d="M5 3h14v4h-5v10h5v4H5v-4h5V7H5z"/>',
    "datum": '<path d="m3 16 5-8h13l-5 8zM12 3v18M3 12h18"/>',
    "feature": '<path d="M3 6h11v11H3zM7 3h11v8M18 14v8m-4-4h8"/>',
    "orientation": '<circle cx="12" cy="12" r="9"/><path d="m16 7-3 8-6 2 3-8zM10 9l3 6"/>',
    "function": '<path d="M3 3v18h18M5 15c3 0 3-10 6-10s3 12 6 12 3-8 5-8"/>',
    "mesh_control": '<path d="M3 3h14v14H3zM3 10h14M10 3v14M15 12v9m-3-5h6M21 10v11m-3-8h5"/>',
    "parameter": '<path d="M3 6h4m4 0h10M3 12h10m4 0h4M3 18h4m4 0h10"/><circle cx="9" cy="6" r="2"/><circle cx="15" cy="12" r="2"/><circle cx="9" cy="18" r="2"/>',
    "time_points": '<circle cx="10" cy="10" r="7"/><path d="M10 6v4l3 2M3 21h18M5 19v3m7-3v3m7-3v3"/>',
    "constraint": '<path d="m9 15-2 2a4 4 0 0 1-6-6l5-5a4 4 0 0 1 6 0m0 3 2-2a4 4 0 0 1 6 6l-5 5a4 4 0 0 1-6 0M8 14l8-8"/>',
    "contact_property": '<path d="M3 4h6v16H3zM15 4h6v16h-6zM11 7h2m-2 5h2m-2 5h2"/>',
    "case": '<path d="M8 6V3h8v3M3 6h18v15H3zM9 10l7 4-7 4z"/>',
    "step": '<path d="M3 19h6v-7h6V5h6"/><circle cx="3" cy="19" r="2"/><circle cx="21" cy="5" r="2"/>',
    "code": '<path d="M5 3h14v18H5zM10 8l-3 4 3 4m4-8 3 4-3 4"/>',
    "report": '<path d="M5 3h14v18H5zM8 7h8M8 17v-4m4 4V9m4 8v-6"/>',
    "result_file": '<path d="M3 3h18v18H3zM3 9c6-6 12 6 18 0M3 15c6-6 12 6 18 0"/>',
    "derived_result": '<path d="M19 4H5l8 8-8 8h14M17 3v4m0 10v4"/>',
    "initial_condition": '<path d="M5 22V3h14l-3 4 3 4H5M2 22h7"/>',
    "step_change": '<path d="M3 7h17m-4-4 4 4-4 4M21 17H4m4-4-4 4 4 4"/>',
    "output_request": '<path d="M12 3v12m-5-5 5 5 5-5M3 15v6h18v-6"/>',
    "status": '<rect x="3" y="3" width="18" height="18" rx="1"/><path d="M5 13h4l2-6 3 11 2-5h3"/>',
    "cylinder": '<ellipse cx="12" cy="6" rx="8" ry="3"/><path d="M4 6v12c0 4 16 4 16 0V6"/>',
    "load": '<path d="M4 12h16m-6-6 6 6-6 6M4 5v14"/>',
    "load_set": '<path d="M3 5v14M3 7h17m-4-4 4 4-4 4M3 17h17m-4-4 4 4-4 4"/>',
    "bc": '<path d="M12 3v9m-4-4 4 4 4-4M4 16h16M5 16l-3 5m8-5-3 5m8-5-3 5m8-5-3 5"/>',
    "bc_set": '<path d="M7 3v5m-4 6 4-6 4 6zM17 3v5m-4 6 4-6 4 6zM3 17h18M6 17l-3 4m9-4-3 4m9-4-3 4m6-4-3 4"/>',
    "contact": '<path d="M3 4h7v16H3zM14 4h7v16h-7M12 7v2m0 3v2m0 3v2"/>',
    "result": '<path d="M4 3v17h17M7 15l4-5 4 3 6-8"/>',
    "sketch": '<path d="M4 17 17 4l3 3L7 20H4zM14 7l3 3M3 3h7M3 3v7"/>',
    "csys": '<path d="M6 18h15m-4-4 4 4-4 4M6 18V3m-4 4 4-4 4 4M6 18l9-9"/>',
    "set": '<rect x="4" y="4" width="6" height="6" rx="1"/><rect x="14" y="4" width="6" height="6" rx="1"/><rect x="4" y="14" width="6" height="6" rx="1"/><rect x="14" y="14" width="6" height="6" rx="1"/>',
    "run": '<path d="m7 3 14 9-14 9z"/>',
    "stop": '<rect x="5" y="5" width="14" height="14" rx="2"/>',
    "save": '<path d="M4 3h13l4 4v14H3V3zM7 3v6h10V3M7 21v-8h10v8"/>',
    "open": '<path d="M3 11V5h7l2 3h8v3M2 11h20l-3 10H5z"/>',
    "new": '<path d="M5 3h10l4 4v14H5zM15 3v5h4M8 14h8m-4-4v8"/>',
    "undo": '<path d="m8 4-5 5 5 5M3 9h10a7 7 0 0 1 0 14"/>',
    "redo": '<path d="m16 4 5 5-5 5M21 9h-10a7 7 0 0 0 0 14"/>',
    "eye": '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
    "eye_off": '<path d="m3 3 18 18M9 5a12 12 0 0 1 13 7l-3 4M6 6a18 18 0 0 0-4 6s4 7 10 7l4-1"/>',
    "fit": '<path d="M3 9V3h6m6 0h6v6m0 6v6h-6m-6 0H3v-6M8 8h8v8H8z"/>',
    "shrink": '<path d="M2 8V2h6m8 0h6v6m0 8v6h-6m-8 0H2v-6M5 5l4 4m-4 0h4V5m10 14-4-4m4 0h-4v4"/><rect x="10" y="10" width="4" height="4"/>',
    "solid_section": '<path d="M3 17h6M3 17l3-3h6l-3 3M9 17l3-3M4 7h5M4 7l2-2h5l-2 2M9 7v10M15 5l3 2 3-2v12l-3 2-3-2z"/>',
    "beam_axis": '<path d="M3 18h18M12 18V6m-4 4 4-4 4 4"/>',
    "zoom_region": '<path d="M3 3h7v7H3zM6 6l5 5" stroke-dasharray="2 1.5"/><circle cx="14" cy="14" r="5"/><path d="m18 18 3 3"/>',
    "search": '<circle cx="10" cy="10" r="6"/><path d="m15 15 6 6"/>',
    "settings": '<circle cx="12" cy="12" r="3"/><path d="m10 2 4 0 .7 3 2 .9 2.6-1.5 2 3.5-2.2 2v2.2l2.2 2-2 3.5-2.6-1.5-2 .9-.7 3h-4l-.7-3-2-.9-2.6 1.5-2-3.5 2.2-2V9.9l-2.2-2 2-3.5 2.6 1.5 2-.9z"/>',
    "sun": '<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1 1m12 12 1 1M5 19l1-1M18 6l1-1"/>',
    "moon": '<path d="M20 15A9 9 0 0 1 9 3a9 9 0 1 0 11 12z"/>',
    "edit": '<path d="m4 16 12-12 4 4L8 20H4zM13 7l4 4"/>',
    "delete": '<path d="M3 6h18M9 6V3h6v3M5 6l1 15h12l1-15M9 10v7m6-7v7"/>',
    "check": '<path d="m4 12 5 5L20 6"/>',
    "warning": '<path d="m12 3 10 18H2zM12 9v5m0 3v1"/>',
    "chevron": '<path d="m8 4 8 8-8 8"/>',
    "down": '<path d="m5 8 7 7 7-7"/>',
    "up": '<path d="m5 16 7-7 7 7"/>',
}

KINDS = {"part": "box", "datum": "datum", "feature": "feature", "mesh_part": "mesh", "mesh_control": "mesh_control",
         "material": "material", "property": "section", "load": "load", "bc": "bc",
         "load_set": "load_set", "bc_set": "bc_set",
         "constraint": "constraint", "contact_pair": "contact", "contact_property": "contact_property",
         "case": "case", "step": "step", "result_file": "result_file", "derived_result": "derived_result",
         "plot": "result", "report": "report", "csys": "csys", "orientation": "orientation",
         "sketch": "sketch", "set": "set", "folder": "folder", "function": "function",
         "parameter": "parameter", "time_points": "time_points", "initial_condition": "initial_condition",
         "output_request": "output_request", "step_change": "step_change", "deck_block": "code"}


@lru_cache(maxsize=512)
def icon(name: str, color: str = "#52647A", fill: str = "") -> QIcon:
    """선 아이콘. fill 을 주면 바깥 모양을 그 색으로 칠한다(메시 파트·재료·프로퍼티의 표시 색 견본)."""
    shape = PATHS.get(KINDS.get(name, name), PATHS["property"])
    fill_attr = f'fill="{fill}" fill-opacity="0.85"' if fill else 'fill="none"'
    svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"><g {fill_attr} stroke="{color}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">{shape}</g></svg>'
    renderer = QSvgRenderer(QByteArray(svg.encode()))
    result = QIcon()
    for size in (16, 20, 24, 32):
        pixmap = QPixmap(size * 2, size * 2)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        renderer.render(painter)
        painter.end()
        pixmap.setDevicePixelRatio(2)
        result.addPixmap(pixmap)
    return result
