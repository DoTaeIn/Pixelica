"""Pixelica 2D 데스크톱 편집기 UI (Tk/ttk + Pillow).

Direction: task-centered native desktop canvas. Neutral blue-gray layers keep
chrome quiet, teal marks focus/selection/play, the viewport is the real
Pixelica2D native render (never a mock), and play state is always visible.

Design notes / limitations
- Edit state (``self.project``) and play state (``self.session.project``, a clone)
  never mix; Stop just drops the session.
- Undo/redo hold at most ``UNDO_LIMIT`` full ``to_dict`` snapshots. Dirty is
  "current serialized project != last saved serialized project".
- Rotation is assumed to be about the sprite pivot (position, top-left),
  positive angles clockwise on screen (y down), matching Renderer2D's contract.
- Property validation: UI parses fields, then the model's ``from_dict`` is the
  final authority; a rejected edit is rolled back as a whole.
- Only 2D sprites are editable; other kinds are listed but not drawn/hit-tested.
- Collision overlays draw ``model.collider_bounds`` (the enclosing AABB the runtime
  uses; rotated colliders are NOT oriented boxes). Overlays are edit-mode only.
"""
from __future__ import annotations

import json
import math
import os
import re
import sys
import time
import tkinter as tk
import tkinter.font as tkfont
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, ttk

from PIL import Image, ImageTk

from .model import (Project, ProjectError, collider_bounds, default_body2d, default_collider2d,
                    import_asset, load_project, new_project, save_project)
from .runtime import PlaySession, Renderer2D

UNDO_LIMIT = 50
GRID = 16
STEP = 1.0 / 60.0
MAX_STEPS = 5
FRAME = 1.0 / 30.0
TICK_MS = 8
ZOOM_MIN, ZOOM_MAX = 0.1, 16.0
MAX_DISPLAY = 4096
POS_LIMIT = 100000.0
HEX_RE = re.compile(r"^#[0-9a-fA-F]{6}$")
RESAMPLE_NEAREST = getattr(Image, "Resampling", Image).NEAREST

P = {
    "bg": "#161c24", "panel": "#1f2833", "field": "#141a21", "border": "#38465a",
    "button": "#2e3b4b", "button_hover": "#3a4a5e", "text": "#e6ebf1",
    "muted": "#9fadbd", "accent": "#2bb3a3", "accent_dark": "#1c7c72",
    "warn": "#8a6d1f", "error": "#ff9a8a",
    "solid": "#ff9f43", "trigger": "#7fb5ff",
}

COLLISION_PRESETS = ("none", "solid", "player", "trigger")
_COL_TEXT_KEYS = ("cox", "coy", "cw", "ch", "gravity", "jump", "maxfall")
_COL_FLAG_KEYS = ("col_enabled", "col_trigger", "col_fit", "body_dynamic")

# Tk keysym (lowercase) -> names handed to PlaySession.step. Arrow aliases are
# included so either naming convention reaches the runtime.
_KEY_NAMES = {
    "w": ("w",), "a": ("a",), "s": ("s",), "d": ("d",), "space": ("space",),
    "up": ("up", "arrowup", "ArrowUp", "Up"),
    "down": ("down", "arrowdown", "ArrowDown", "Down"),
    "left": ("left", "arrowleft", "ArrowLeft", "Left"),
    "right": ("right", "arrowright", "ArrowRight", "Right"),
}


def _fmt(v) -> str:
    try:
        if isinstance(v, float) and v.is_integer():
            return str(int(v))
        return format(v, ".6g")
    except (TypeError, ValueError):
        return str(v)


def sprite_geometry(obj):
    """Return (x, y, w, h, theta_radians) of a sprite2d object, or None."""
    sp = obj.components.get("sprite2d") if isinstance(obj.components, dict) else None
    if not isinstance(sp, dict):
        return None
    t = obj.transform
    w = float(sp.get("width", 32)) * float(t.scale[0])
    h = float(sp.get("height", 32)) * float(t.scale[1])
    return float(t.position[0]), float(t.position[1]), w, h, math.radians(float(t.rotation[2]))


def object_corners(obj):
    """Stage-space corners (TL, TR, BR, BL) of the rotated sprite, or None."""
    g = sprite_geometry(obj)
    if g is None:
        return None
    x, y, w, h, th = g
    c, s = math.cos(th), math.sin(th)
    return [(x + lx * c - ly * s, y + lx * s + ly * c)
            for lx, ly in ((0, 0), (w, 0), (w, h), (0, h))]


def hit_test_project(project, x, y):
    """Topmost visible sprite id containing stage point (x, y), else None."""
    ranked = []
    for index, obj in enumerate(project.objects):
        sp = obj.components.get("sprite2d") if isinstance(obj.components, dict) else None
        layer = sp.get("layer", 0) if isinstance(sp, dict) else 0
        ranked.append((layer, index, obj))
    ranked.sort(key=lambda t: (t[0], t[1]), reverse=True)
    for _layer, _i, obj in ranked:
        if not obj.visible:
            continue
        g = sprite_geometry(obj)
        if g is None:
            continue
        ox, oy, w, h, th = g
        dx, dy = x - ox, y - oy
        c, s = math.cos(th), math.sin(th)
        lx = dx * c + dy * s
        ly = -dx * s + dy * c
        if min(0.0, w) <= lx <= max(0.0, w) and min(0.0, h) <= ly <= max(0.0, h):
            return obj.id
    return None


class EditorApp:
    def __init__(self, root: tk.Tk, project: Project | None = None):
        self.root = root
        self.project = project if project is not None else new_project()
        self.selected_id: str | None = None
        self.session: PlaySession | None = None
        self.paused = False
        self.play_keys: set[str] = set()
        self.status_var = tk.StringVar(master=root, value="")
        self.info_var = tk.StringVar(master=root, value="")
        self.insp_msg = tk.StringVar(master=root, value="")
        self.snap_var = tk.BooleanVar(master=root, value=False)
        self.collision_var = tk.BooleanVar(master=root, value=True)
        self.vars: dict[str, tk.Variable] = {}
        self._orig: dict[str, str] = {}
        self._orig_flags: dict[str, bool] = {}
        self._proj_orig: dict[str, str] = {}
        self._undo: list[dict] = []
        self._redo: list[dict] = []
        self._drag: dict | None = None
        self._pan = None
        self._syncing = False
        self._closed = False
        self._after_id = None
        self._status_error = False
        self.zoom = 1.0
        self.ox = self.oy = 0.0
        self._fit = True
        self._need_render = True
        self._last_render = 0.0
        self._last_tick = time.monotonic()
        self._acc = 0.0
        self._last_image: Image.Image | None = None
        self._img_serial = 0
        self._photo = None
        self._photo_key = None
        self.render_error: str | None = None
        self.renderer = None
        self._saved_key = self._key()

        self._setup_style()
        self._build()
        self._bind()
        self._make_renderer()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.report_callback_exception = self._on_callback_exception
        self._refresh_all()
        if self.render_error is None:
            self._info("샘플 장면을 불러왔습니다. 스프라이트를 클릭하거나 끌어 보세요.")
        self._after_id = self.root.after(TICK_MS, self._tick)

    # ------------------------------------------------------------------ state
    @property
    def playing(self) -> bool:
        return self.session is not None

    def selected(self):
        return self.project.object_by_id(self.selected_id) if self.selected_id else None

    def _key(self) -> str:
        return json.dumps(self.project.to_dict(), sort_keys=True)

    def is_dirty(self) -> bool:
        return self._key() != self._saved_key or self._has_pending()

    # Typed-but-not-applied inspector input. Baselines are what the fields held
    # when last loaded from the model (``_orig`` / ``_orig_flags`` / ``_proj_orig``).
    _OBJ_KEYS = ("name", "x", "y", "rot", "sx", "sy", "w", "h", "color", "opacity", "layer",
                 "speed", "vx", "vy") + _COL_TEXT_KEYS
    _PROJ_KEYS = ("p_name", "p_w", "p_h", "p_bg")

    def _obj_pending(self) -> bool:
        if not self._orig or self.selected() is None:
            return False
        if any(self.vars[k].get().strip() != self._orig.get(k, "").strip() for k in self._OBJ_KEYS):
            return True
        return any(bool(self.vars[k].get()) != v for k, v in self._orig_flags.items())

    def _proj_pending(self) -> bool:
        return any(k in self._proj_orig
                   and self.vars[k].get().strip() != self._proj_orig[k].strip()
                   for k in self._PROJ_KEYS)

    def _has_pending(self) -> bool:
        return self._obj_pending() or self._proj_pending()

    def _commit_pending(self) -> bool:
        """Apply valid pending field input. False (input kept) if any of it is invalid."""
        if self.playing:
            return True
        if self._proj_pending() and not self.apply_project_settings():
            return False
        if self._obj_pending() and not self.apply_properties():
            return False
        return True

    def _snapshot(self) -> dict:
        return json.loads(json.dumps(self.project.to_dict()))

    def _make_renderer(self):
        try:
            self.renderer = Renderer2D(self.project.width, self.project.height)
            self.render_error = None
        except Exception as exc:  # native bridge missing or incompatible
            self.renderer = None
            self.render_error = f"Pixelica2D 렌더러를 불러올 수 없습니다: {exc}"
            self._error(self.render_error)

    # ------------------------------------------------------------------ style
    def _setup_style(self):
        r = self.root
        fams = set(tkfont.families(r))
        family = next((f for f in ("Malgun Gothic", "Segoe UI", "Apple SD Gothic Neo",
                                   "Noto Sans CJK KR", "DejaVu Sans") if f in fams), "TkDefaultFont")
        self.font = (family, 10)
        self.font_bold = (family, 10, "bold")
        r.configure(background=P["panel"])
        st = ttk.Style(r)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure(".", background=P["panel"], foreground=P["text"], font=self.font,
                     fieldbackground=P["field"], bordercolor=P["border"], lightcolor=P["panel"],
                     darkcolor=P["panel"], troughcolor=P["bg"], focuscolor=P["accent"],
                     insertcolor=P["text"], selectbackground=P["accent_dark"],
                     selectforeground="#ffffff")
        st.configure("TButton", padding=(10, 5), background=P["button"])
        st.map("TButton", background=[("disabled", P["panel"]), ("active", P["button_hover"])],
               foreground=[("disabled", P["muted"])])
        st.configure("Accent.TButton", background=P["accent_dark"], foreground="#ffffff")
        st.map("Accent.TButton", background=[("disabled", P["panel"]), ("active", P["accent"])],
               foreground=[("disabled", P["muted"])])
        st.configure("TEntry", padding=3)
        st.map("TEntry", fieldbackground=[("disabled", P["panel"])],
               foreground=[("disabled", P["muted"])])
        st.configure("TSpinbox", padding=3)
        st.map("TSpinbox", fieldbackground=[("disabled", P["panel"])],
               foreground=[("disabled", P["muted"])])
        st.map("TCheckbutton", foreground=[("disabled", P["muted"])])
        st.configure("Treeview", background=P["field"], fieldbackground=P["field"],
                     foreground=P["text"], rowheight=26)
        st.map("Treeview", background=[("selected", P["accent_dark"])],
               foreground=[("selected", "#ffffff")])
        st.configure("Header.TLabel", font=self.font_bold)
        st.configure("Muted.TLabel", foreground=P["muted"])
        st.configure("Error.TLabel", foreground=P["error"])
        st.configure("Edit.TLabel", font=self.font_bold, padding=(10, 4),
                     background=P["button"], foreground=P["text"])
        st.configure("Play.TLabel", font=self.font_bold, padding=(10, 4),
                     background=P["accent_dark"], foreground="#ffffff")
        st.configure("Pause.TLabel", font=self.font_bold, padding=(10, 4),
                     background=P["warn"], foreground="#ffffff")
        st.configure("Section.TLabelframe", background=P["panel"])
        st.configure("Section.TLabelframe.Label", font=self.font_bold,
                     background=P["panel"], foreground=P["text"])

    # ------------------------------------------------------------------ build
    def _build(self):
        r = self.root
        r.title("Pixelica 편집기")
        r.geometry("1280x800")
        r.minsize(1024, 680)
        self.buttons: dict[str, ttk.Button] = {}

        # Two grouped rows so every action stays reachable at the 1024px minimum.
        bar = ttk.Frame(r, padding=(8, 4))
        bar.pack(side="top", fill="x")
        row1 = ttk.Frame(bar)
        row1.pack(side="top", fill="x")
        row2 = ttk.Frame(bar)
        row2.pack(side="top", fill="x", pady=(4, 0))

        def btn(row, key, text, cmd, style=None, pad=2):
            b = ttk.Button(row, text=text, command=self._safe(cmd), style=style or "TButton")
            b.pack(side="left", padx=pad)
            self.buttons[key] = b

        def sep(row):
            ttk.Separator(row, orient="vertical").pack(side="left", fill="y", padx=8)

        btn(row1, "new", "새로 만들기", self.new)
        btn(row1, "open", "열기", self.open_project)
        btn(row1, "save", "저장", self.save)
        btn(row1, "save_as", "다른 이름으로 저장", self.save_as)
        sep(row1)
        btn(row1, "undo", "실행 취소", self.undo)
        btn(row1, "redo", "다시 실행", self.redo)
        btn(row2, "add", "스프라이트 추가", self.add_object)
        sep(row2)
        btn(row2, "play", "재생 (F5)", self.toggle_play, "Accent.TButton")
        btn(row2, "stop", "정지 (Esc)", self.stop_play)
        self.mode_label = ttk.Label(row2, text="편집 모드", style="Edit.TLabel")
        self.mode_label.pack(side="right")
        self.toolbar = bar

        foot = ttk.Frame(r, padding=(10, 4))
        foot.pack(side="bottom", fill="x")
        self.status_label = ttk.Label(foot, textvariable=self.status_var)
        self.status_label.pack(side="left")
        ttk.Label(foot, textvariable=self.info_var, style="Muted.TLabel").pack(side="right")

        panes = ttk.PanedWindow(r, orient="horizontal")
        panes.pack(fill="both", expand=True)
        left = ttk.Frame(panes, width=220, padding=8)
        center = ttk.Frame(panes, padding=0)
        right = ttk.Frame(panes, width=280, padding=0)
        panes.add(left, weight=0)
        panes.add(center, weight=1)
        panes.add(right, weight=0)
        self._build_tree(left)
        self._build_canvas(center)
        self._build_inspector(right)

    def _build_tree(self, parent):
        parent.pack_propagate(False)
        ttk.Label(parent, text="장면 오브젝트", style="Header.TLabel").pack(anchor="w", pady=(0, 6))
        row = ttk.Frame(parent)
        row.pack(side="bottom", fill="x", pady=(8, 0))
        for i, (key, text, cmd) in enumerate((
                ("dup", "복제", self.duplicate_object), ("del", "삭제", self.delete_object))):
            b = ttk.Button(row, text=text, command=self._safe(cmd))
            b.grid(row=0, column=i, sticky="ew", padx=2)
            row.columnconfigure(i, weight=1)
            self.buttons[key] = b
        b = ttk.Button(parent, text="이미지 가져오기", command=self._safe(self.import_image))
        b.pack(side="bottom", fill="x", pady=(8, 0), padx=2)
        self.buttons["import"] = b
        holder = ttk.Frame(parent)
        holder.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(holder, show="tree", selectmode="browse")
        sb = ttk.Scrollbar(holder, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.tree.pack(side="left", fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", self._safe(self._on_tree_select))
        self.tree.bind("<Double-Button-1>", self._safe(self._on_tree_double))

    def _build_canvas(self, parent):
        top = ttk.Frame(parent, padding=(8, 5))
        top.pack(side="top", fill="x")
        actions = ttk.Frame(top)
        actions.pack(fill="x")
        for text, cmd in (("맞춤", self.fit_view), ("축소", lambda: self.zoom_by(1 / 1.25)),
                          ("확대", lambda: self.zoom_by(1.25)), ("100%", lambda: self.set_zoom(1.0))):
            ttk.Button(actions, text=text, width=5, command=self._safe(cmd)).pack(side="left", padx=2)
        ttk.Checkbutton(actions, text=f"{GRID}px 격자 맞춤", variable=self.snap_var).pack(side="left", padx=6)
        hint = ttk.Frame(top)
        hint.pack(fill="x", pady=(4, 0))
        ttk.Checkbutton(hint, text="충돌 영역 표시", variable=self.collision_var,
                        command=self._safe(self._draw_overlay)).pack(side="left")
        ttk.Label(hint, text="휠: 확대/축소 · 가운데 버튼 드래그: 이동",
                  style="Muted.TLabel").pack(side="left", padx=(10, 0))
        self.view_toolbar = top
        self.canvas = tk.Canvas(parent, background=P["bg"], highlightthickness=2,
                                highlightbackground=P["bg"], highlightcolor=P["accent"],
                                takefocus=1, cursor="arrow")
        self.canvas.pack(fill="both", expand=True)

    def _build_inspector(self, parent):
        parent.pack_propagate(False)
        ttk.Label(parent, text="속성", style="Header.TLabel", padding=(10, 8)).pack(anchor="w")
        holder = ttk.Frame(parent)
        holder.pack(fill="both", expand=True)
        self.insp_canvas = tk.Canvas(holder, background=P["panel"], highlightthickness=0, width=260)
        sb = ttk.Scrollbar(holder, orient="vertical", command=self.insp_canvas.yview)
        self.insp_canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.insp_canvas.pack(side="left", fill="both", expand=True)
        self.insp_holder = holder
        inner = ttk.Frame(self.insp_canvas, padding=(10, 0, 6, 10))
        win = self.insp_canvas.create_window(0, 0, window=inner, anchor="nw")
        inner.bind("<Configure>", lambda e: self.insp_canvas.configure(
            scrollregion=self.insp_canvas.bbox("all")))
        self.insp_canvas.bind("<Configure>", lambda e: self.insp_canvas.itemconfigure(win, width=e.width))
        self._obj_widgets: list = []
        self._proj_widgets: list = []

        sec = ttk.LabelFrame(inner, text="선택한 오브젝트", style="Section.TLabelframe", padding=8)
        sec.pack(fill="x", pady=(0, 10))
        sec.columnconfigure(1, weight=1)
        self._row = 0
        w = self._entry(sec, "name", "이름")
        self._pair(sec, "위치 X / Y", "x", "y")
        self._entry(sec, "rot", "회전 (Z°)")
        self._pair(sec, "배율 X / Y", "sx", "sy")
        self._pair(sec, "너비 / 높이", "w", "h")
        # color
        ttk.Label(sec, text="색상").grid(row=self._row, column=0, sticky="w", pady=3)
        f = ttk.Frame(sec)
        f.grid(row=self._row, column=1, sticky="ew", pady=3)
        self.vars["color"] = tk.StringVar(master=self.root)
        e = ttk.Entry(f, textvariable=self.vars["color"], width=9)
        e.pack(side="left", fill="x", expand=True)
        b = ttk.Button(f, text="선택", padding=(6, 4), command=self._safe(self._pick_color))
        b.pack(side="left", padx=(4, 0))
        e.bind("<Return>", self._safe(self.apply_properties))
        self._obj_widgets += [e, b]
        self._row += 1
        self._spin(sec, "opacity", "불투명도 (0-255)", 0, 255)
        self._spin(sec, "layer", "레이어", -1000, 1000)
        self.vars["visible"] = tk.BooleanVar(master=self.root, value=True)
        c = ttk.Checkbutton(sec, text="화면에 표시", variable=self.vars["visible"])
        c.grid(row=self._row, column=0, columnspan=2, sticky="w", pady=3)
        self._obj_widgets.append(c)
        self._row += 1
        # asset
        ttk.Label(sec, text="이미지").grid(row=self._row, column=0, sticky="w", pady=3)
        f = ttk.Frame(sec)
        f.grid(row=self._row, column=1, sticky="ew", pady=3)
        self.vars["asset"] = tk.StringVar(master=self.root, value="(없음)")
        self.asset_row = f
        ttk.Label(f, textvariable=self.vars["asset"], style="Muted.TLabel", width=4).pack(
            side="left", fill="x", expand=True)
        # Natural-width buttons with tight padding so the Korean labels never clip.
        b1 = ttk.Button(f, text="선택", padding=(6, 4), command=self._safe(self.choose_asset))
        b2 = ttk.Button(f, text="해제", padding=(6, 4), command=self._safe(self.clear_asset))
        b1.pack(side="left", padx=(4, 0))
        b2.pack(side="left", padx=(4, 0))
        self._obj_widgets += [b1, b2]
        self._row += 1
        # motion
        ttk.Separator(sec).grid(row=self._row, column=0, columnspan=2, sticky="ew", pady=6)
        self._row += 1
        self.vars["controlled"] = tk.BooleanVar(master=self.root, value=False)
        c = ttk.Checkbutton(sec, text="WASD/방향키로 조작", variable=self.vars["controlled"])
        c.grid(row=self._row, column=0, columnspan=2, sticky="w", pady=3)
        self._obj_widgets.append(c)
        self._row += 1
        self._entry(sec, "speed", "속도")
        self._pair(sec, "초기 속도 X / Y", "vx", "vy")
        ab = ttk.Button(sec, text="적용 (Enter)", style="Accent.TButton",
                        command=self._safe(self.apply_properties))
        ab.grid(row=self._row, column=0, columnspan=2, sticky="ew", pady=(8, 2))
        self._obj_widgets.append(ab)
        self.buttons["apply"] = ab
        self._row += 1
        self.insp_err = ttk.Label(sec, textvariable=self.insp_msg, style="Error.TLabel", wraplength=230)
        self.insp_err.grid(row=self._row, column=0, columnspan=2, sticky="w")

        col = ttk.LabelFrame(inner, text="충돌 / 물리", style="Section.TLabelframe", padding=8)
        col.pack(fill="x", pady=(0, 10))
        col.columnconfigure(1, weight=1)
        self._col_geo: list = []
        self._col_flag: list = []
        self._col_body: list = []
        r = 0
        ttk.Label(col, text="빠른 설정", style="Muted.TLabel").grid(
            row=r, column=0, columnspan=2, sticky="w")
        r += 1
        for key, text in (("none", "충돌 없음"), ("solid", "벽 / 바닥 (고정)"),
                          ("player", "플레이어 (중력·점프)"), ("trigger", "트리거 (감지만)")):
            b = ttk.Button(col, text=text, padding=(6, 4),
                           command=self._safe(lambda k=key: self.apply_collision_preset(k)))
            b.grid(row=r, column=0, columnspan=2, sticky="ew", pady=2)
            self._obj_widgets.append(b)
            self.buttons["preset_" + key] = b
            r += 1
        ttk.Separator(col).grid(row=r, column=0, columnspan=2, sticky="ew", pady=6)
        r += 1

        def check(key, text, group):
            nonlocal r
            self.vars[key] = tk.BooleanVar(master=self.root, value=False)
            c = ttk.Checkbutton(col, text=text, variable=self.vars[key],
                                command=self._safe(self._sync_collision_widgets))
            c.grid(row=r, column=0, columnspan=2, sticky="w", pady=2)
            (self._obj_widgets if group is None else group).append(c)
            r += 1

        def field(key, label, group, pair=None):
            nonlocal r
            ttk.Label(col, text=label).grid(row=r, column=0, sticky="w", pady=3, padx=(0, 8))
            f = ttk.Frame(col)
            f.grid(row=r, column=1, sticky="ew", pady=3)
            f.columnconfigure((0, 1), weight=1, uniform="c")
            for i, k in enumerate((key,) if pair is None else (key, pair)):
                self.vars[k] = tk.StringVar(master=self.root)
                e = ttk.Entry(f, textvariable=self.vars[k], width=6)
                e.grid(row=0, column=i, columnspan=2 if pair is None else 1, sticky="ew",
                       padx=(0 if i == 0 else 4, 0))
                e.bind("<Return>", self._safe(self.apply_properties))
                group.append(e)
            r += 1

        check("col_enabled", "충돌 사용", None)
        check("col_trigger", "트리거 (통과하며 감지만)", self._col_flag)
        check("col_fit", "스프라이트 크기에 맞춤", self._col_flag)
        field("cox", "오프셋 X / Y", self._col_geo, "coy")
        field("cw", "크기 W / H", self._col_geo, "ch")
        ttk.Separator(col).grid(row=r, column=0, columnspan=2, sticky="ew", pady=6)
        r += 1
        check("body_dynamic", "이동 물체 (중력·점프)", self._col_flag)
        field("gravity", "중력", self._col_body)
        field("jump", "점프 속도", self._col_body)
        field("maxfall", "최대 낙하 속도", self._col_body)
        cb = ttk.Button(col, text="충돌 설정 적용", style="Accent.TButton",
                        command=self._safe(self.apply_properties))
        cb.grid(row=r, column=0, columnspan=2, sticky="ew", pady=(8, 2))
        self._obj_widgets.append(cb)
        self.buttons["apply_collision"] = cb
        ttk.Label(col, textvariable=self.insp_msg, style="Error.TLabel", wraplength=230).grid(
            row=r + 1, column=0, columnspan=2, sticky="w")
        ttk.Label(col, text="충돌 영역은 회전을 포함한 외곽 사각형(AABB)입니다.",
                  style="Muted.TLabel", wraplength=230).grid(
            row=r + 2, column=0, columnspan=2, sticky="w", pady=(4, 0))

        proj = ttk.LabelFrame(inner, text="프로젝트 설정", style="Section.TLabelframe", padding=8)
        proj.pack(fill="x")
        proj.columnconfigure(1, weight=1)
        self._row = 0
        self._entry(proj, "p_name", "이름", self._proj_widgets, self.apply_project_settings)
        self._pair(proj, "해상도 W / H", "p_w", "p_h", self._proj_widgets, self.apply_project_settings)
        ttk.Label(proj, text="배경색").grid(row=self._row, column=0, sticky="w", pady=3)
        f = ttk.Frame(proj)
        f.grid(row=self._row, column=1, sticky="ew", pady=3)
        self.vars["p_bg"] = tk.StringVar(master=self.root)
        e = ttk.Entry(f, textvariable=self.vars["p_bg"], width=9)
        e.pack(side="left", fill="x", expand=True)
        b = ttk.Button(f, text="선택", padding=(6, 4), command=self._safe(self._pick_bg))
        b.pack(side="left", padx=(4, 0))
        e.bind("<Return>", self._safe(self.apply_project_settings))
        self._proj_widgets += [e, b]
        self._row += 1
        pb = ttk.Button(proj, text="설정 적용", command=self._safe(self.apply_project_settings))
        pb.grid(row=self._row, column=0, columnspan=2, sticky="ew", pady=(8, 2))
        self._proj_widgets.append(pb)
        self.buttons["apply_project"] = pb
        self.proj_msg = tk.StringVar(master=self.root)
        ttk.Label(proj, textvariable=self.proj_msg, style="Error.TLabel", wraplength=230).grid(
            row=self._row + 1, column=0, columnspan=2, sticky="w")

    def _entry(self, parent, key, label, group=None, cmd=None):
        ttk.Label(parent, text=label).grid(row=self._row, column=0, sticky="w", pady=3, padx=(0, 8))
        self.vars[key] = tk.StringVar(master=self.root)
        e = ttk.Entry(parent, textvariable=self.vars[key])
        e.grid(row=self._row, column=1, sticky="ew", pady=3)
        e.bind("<Return>", self._safe(cmd or self.apply_properties))
        (self._obj_widgets if group is None else group).append(e)
        self._row += 1
        return e

    def _pair(self, parent, label, k1, k2, group=None, cmd=None):
        ttk.Label(parent, text=label).grid(row=self._row, column=0, sticky="w", pady=3, padx=(0, 8))
        f = ttk.Frame(parent)
        f.grid(row=self._row, column=1, sticky="ew", pady=3)
        f.columnconfigure((0, 1), weight=1, uniform="p")
        for i, k in enumerate((k1, k2)):
            self.vars[k] = tk.StringVar(master=self.root)
            e = ttk.Entry(f, textvariable=self.vars[k], width=6)
            e.grid(row=0, column=i, sticky="ew", padx=(0 if i == 0 else 4, 0))
            e.bind("<Return>", self._safe(cmd or self.apply_properties))
            (self._obj_widgets if group is None else group).append(e)
        self._row += 1

    def _spin(self, parent, key, label, lo, hi):
        ttk.Label(parent, text=label).grid(row=self._row, column=0, sticky="w", pady=3, padx=(0, 8))
        self.vars[key] = tk.StringVar(master=self.root)
        s = ttk.Spinbox(parent, from_=lo, to=hi, textvariable=self.vars[key], width=6)
        s.grid(row=self._row, column=1, sticky="ew", pady=3)
        s.bind("<Return>", self._safe(self.apply_properties))
        self._obj_widgets.append(s)
        self._row += 1

    # --------------------------------------------------------------- bindings
    def _bind(self):
        r, c = self.root, self.canvas
        sc = self._safe
        r.bind("<Control-n>", sc(self.new, brk=True))
        r.bind("<Control-o>", sc(self.open_project, brk=True))
        r.bind("<Control-s>", sc(self.save, brk=True))
        r.bind("<Control-S>", sc(self.save_as, brk=True))
        r.bind("<Control-Shift-S>", sc(self.save_as, brk=True))
        r.bind("<Control-z>", sc(self.undo, brk=True))
        r.bind("<Control-y>", sc(self.redo, brk=True))
        r.bind("<Control-d>", sc(self._kb_duplicate, brk=True))
        r.bind("<Delete>", sc(self._kb_delete))
        r.bind("<F5>", sc(self.toggle_play, brk=True))
        r.bind("<Escape>", sc(self.stop_play, brk=True))
        c.bind("<Configure>", sc(self._on_canvas_configure))
        c.bind("<ButtonPress-1>", sc(self._on_press))
        c.bind("<B1-Motion>", sc(self._on_motion))
        c.bind("<ButtonRelease-1>", sc(self._on_release))
        for seq in ("<ButtonPress-2>", "<ButtonPress-3>") if sys.platform == "darwin" else ("<ButtonPress-2>",):
            c.bind(seq, sc(self._pan_start))
        for seq in ("<B2-Motion>", "<B3-Motion>") if sys.platform == "darwin" else ("<B2-Motion>",):
            c.bind(seq, sc(self._pan_move))
        c.bind("<KeyPress>", sc(self._on_key_press))
        c.bind("<KeyRelease>", sc(self._on_key_release))
        c.bind("<FocusOut>", sc(self._on_focus_out))
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            r.bind_all(seq, sc(self._on_wheel))

    def _safe(self, fn, brk=False):
        def run(*event):
            if self._closed:
                return None
            res = None
            try:
                if event and hasattr(event[0], "widget") and not _takes_event(fn):
                    res = fn()
                else:
                    res = fn(*event)
            except Exception as exc:
                self._error(f"오류: {exc}")
            return "break" if brk else (res if res == "break" else None)
        return run

    def _on_callback_exception(self, exc, val, tb):
        try:
            self._error(f"내부 오류: {val}")
        except Exception:
            pass

    def _typing(self) -> bool:
        try:
            w = self.root.focus_get()
        except (KeyError, tk.TclError):
            return False
        return isinstance(w, (tk.Entry, ttk.Entry, ttk.Spinbox, ttk.Combobox, tk.Text))

    # ----------------------------------------------------------------- status
    def _info(self, msg: str):
        self._status_error = False
        self.status_var.set(msg)
        self.status_label.configure(style="TLabel")

    def _error(self, msg: str):
        self._status_error = True
        self.status_var.set(msg)
        try:
            self.status_label.configure(style="Error.TLabel")
        except tk.TclError:
            pass

    @staticmethod
    def _has_colliders(project) -> bool:
        return any(isinstance(o.components.get("collider2d"), dict) for o in project.objects)

    def _session_set(self, name):
        try:
            return set(getattr(self.session, name, ()) or ())
        except TypeError:
            return set()

    def play_collision_text(self) -> str:
        """Names of grounded objects / active trigger pairs (public session fields)."""
        s = self.session
        if s is None or not self._has_colliders(s.project):
            return ""

        def nm(oid):
            o = s.project.object_by_id(oid) if isinstance(oid, str) else None
            return o.name if o is not None else str(oid)

        grounded = ", ".join(sorted(nm(i) for i in self._session_set("grounded"))) or "-"
        pairs = sorted((nm(a), nm(b)) for a, b in self._session_set("active_triggers"))
        trig = ", ".join(f"{a}↔{b}" for a, b in pairs) or "-"
        return f"접지: {grounded} · 트리거: {trig}"

    def _update_info(self):
        engine = "Pixelica2D 네이티브" if self.renderer is not None else "렌더러 없음"
        mode = "편집"
        if self.session is not None:
            mode = ("일시정지" if self.paused else "재생") + f" · 틱 {self.session.ticks}"
            if self._has_colliders(self.session.project):
                mode += (f" · 접지 {len(self._session_set('grounded'))}"
                         f" · 트리거 {len(self._session_set('active_triggers'))}")
        self.info_var.set(
            f"{engine} · {self.project.width}×{self.project.height} · 확대 {self.zoom * 100:.0f}% · "
            f"{'수정됨' if self.is_dirty() else '저장됨'} · {mode}")
        name = self.project.name or "이름 없음"
        self.root.title(f"{'*' if self.is_dirty() else ''}{name} — Pixelica 편집기")

    def _update_controls(self):
        playing = self.playing
        editing_ok = not playing

        def en(w, on):
            try:
                w.state(["!disabled"] if on else ["disabled"])
            except tk.TclError:
                pass

        has_sel = self.selected() is not None
        for k in ("new", "open", "add", "import"):
            en(self.buttons[k], editing_ok)
        en(self.buttons["undo"], editing_ok and bool(self._undo))
        en(self.buttons["redo"], editing_ok and bool(self._redo))
        en(self.buttons["dup"], editing_ok and has_sel)
        en(self.buttons["del"], editing_ok and has_sel)
        en(self.buttons["stop"], playing)
        for w in self._obj_widgets:
            en(w, editing_ok and has_sel)
        for w in self._proj_widgets:
            en(w, editing_ok)
        self._sync_collision_widgets()
        if not playing:
            self.buttons["play"].configure(text="재생 (F5)")
            self.mode_label.configure(text="편집 모드", style="Edit.TLabel")
            self.canvas.configure(highlightbackground=P["bg"])
        elif self.paused:
            self.buttons["play"].configure(text="계속 (F5)")
            self.mode_label.configure(text="일시정지", style="Pause.TLabel")
            self.canvas.configure(highlightbackground=P["warn"])
        else:
            self.buttons["play"].configure(text="일시정지 (F5)")
            self.mode_label.configure(text="재생 중", style="Play.TLabel")
            self.canvas.configure(highlightbackground=P["accent"])

    def _sync_collision_widgets(self):
        """Enable only the collision fields that are relevant to the current toggles."""
        ok = not self.playing and self.selected() is not None
        col_on = ok and bool(self.vars["col_enabled"].get())
        fit = bool(self.vars["col_fit"].get())
        dynamic = col_on and bool(self.vars["body_dynamic"].get())

        def en(ws, on):
            for w in ws:
                try:
                    w.state(["!disabled"] if on else ["disabled"])
                except tk.TclError:
                    pass

        en(self._col_flag, col_on)
        en(self._col_geo, col_on and not fit)
        en(self._col_body, dynamic)

    def _refresh_all(self):
        self._refresh_tree()
        self._load_inspector()
        self._load_project_fields()
        self._update_controls()
        self._update_info()
        self._need_render = True
        self._draw_overlay()

    def _after_change(self, structural=True, keep_pending=True):
        self._need_render = True
        if structural:
            self._refresh_tree()
            self._load_project_fields(keep_pending=keep_pending)
        self._update_controls()
        self._update_info()

    # ------------------------------------------------------------ transactions
    def _transaction(self, fn, render_check=False):
        """Run ``fn`` as one undoable edit. Returns (ok, result); rolls back on error."""
        before = self._snapshot()
        before_key = json.dumps(before, sort_keys=True)
        try:
            result = fn()
            Project.from_dict(json.loads(json.dumps(self.project.to_dict())))
            if render_check and self.renderer is not None:
                self.renderer.render(self.project)
        except Exception as exc:
            self._restore(before)
            self._error(f"변경을 적용하지 못했습니다: {exc}")
            self.insp_msg.set(f"입력을 확인하세요: {exc}")
            return False, None
        if self._key() != before_key:
            self._push_undo(before)
        return True, result

    def _push_undo(self, before: dict):
        self._undo.append(before)
        del self._undo[:-UNDO_LIMIT]
        self._redo.clear()

    def _restore(self, snap: dict):
        old = self.project
        proj = Project.from_dict(json.loads(json.dumps(snap)))
        proj.path = old.path
        self.project = proj
        if self.selected_id and proj.object_by_id(self.selected_id) is None:
            self.selected_id = None
        self._after_change()
        self._load_inspector()
        self._draw_overlay()
        if (old.width, old.height) != (proj.width, proj.height) and self._fit:
            self.fit_view()

    def undo(self) -> bool:
        if self.playing or self._drag or not self._commit_pending() or not self._undo:
            return False
        cur = self._snapshot()
        snap = self._undo.pop()
        self._redo.append(cur)
        del self._redo[:-UNDO_LIMIT]
        self._restore(snap)
        self._info("실행 취소")
        return True

    def redo(self) -> bool:
        if self.playing or self._drag or not self._commit_pending() or not self._redo:
            return False
        cur = self._snapshot()
        snap = self._redo.pop()
        self._undo.append(cur)
        del self._undo[:-UNDO_LIMIT]
        self._restore(snap)
        self._info("다시 실행")
        return True

    # ------------------------------------------------------------ scene edits
    def add_object(self):
        if self.playing or not self._commit_pending():
            return None

        def mutate():
            n = len(self.project.objects) + 1
            obj = self.project.add_object(f"스프라이트 {n}", "sprite2d")
            layers = [o.components["sprite2d"].get("layer", 0) for o in self.project.objects
                      if o is not obj and isinstance(o.components.get("sprite2d"), dict)]
            if layers:
                obj.components["sprite2d"]["layer"] = max(layers) + 1
            return obj.id

        ok, oid = self._transaction(mutate)
        if not ok:
            return None
        self._after_change()
        self.select_object(oid, commit=False)
        self._info("스프라이트를 추가했습니다.")
        return self.project.object_by_id(oid)

    def duplicate_object(self):
        if self.playing or not self.selected_id or not self._commit_pending():
            return None
        oid = self.selected_id
        ok, new_id = self._transaction(lambda: self.project.duplicate_object(oid).id)
        if not ok:
            return None
        self._after_change()
        self.select_object(new_id, commit=False)
        self._info("오브젝트를 복제했습니다.")
        return self.project.object_by_id(new_id)

    def delete_object(self) -> bool:
        if self.playing or not self.selected_id or not self._commit_pending():
            return False
        oid = self.selected_id
        ok, _ = self._transaction(lambda: self.project.remove_object(oid))
        if not ok:
            return False
        self.selected_id = None
        self._after_change()
        self.select_object(None, commit=False)
        self._info("오브젝트를 삭제했습니다.")
        return True

    def _kb_duplicate(self):
        if not self._typing():
            self.duplicate_object()

    def _kb_delete(self):
        if not self._typing():
            self.delete_object()

    def select_object(self, oid, commit=True) -> bool:
        """Select ``oid``; False (selection unchanged) if pending input is invalid."""
        if commit and not self._commit_pending():
            self._sync_tree_selection()
            return False
        if oid is not None and self.project.object_by_id(oid) is None:
            oid = None
        self.selected_id = oid
        self.insp_msg.set("")
        self._sync_tree_selection()
        self._load_inspector()
        self._update_controls()
        self._draw_overlay()
        return True

    def _refresh_tree(self):
        self._syncing = True
        try:
            self.tree.delete(*self.tree.get_children())
            for o in self.project.objects:
                self.tree.insert("", "end", iid=o.id,
                                 text=o.name + ("" if o.visible else "  (숨김)"))
            self._sync_tree_selection()
        finally:
            self._syncing = False

    def _sync_tree_selection(self):
        was = self._syncing
        self._syncing = True
        try:
            if self.selected_id and self.tree.exists(self.selected_id):
                if self.tree.selection() != (self.selected_id,):
                    self.tree.selection_set(self.selected_id)
                self.tree.see(self.selected_id)
            else:
                self.tree.selection_remove(*self.tree.selection())
        finally:
            self._syncing = was

    def _on_tree_select(self):
        # <<TreeviewSelect>> is queued, so programmatic syncs arrive here too;
        # ignore those by comparing with the model selection.
        if self._syncing:
            return
        sel = self.tree.selection()
        wanted = sel[0] if sel else None
        if wanted != self.selected_id:
            self.select_object(wanted)

    def _on_tree_double(self, event):
        if self.playing:
            return
        if self.tree.identify_row(event.y):
            for w in self._obj_widgets[:1]:
                w.focus_set()
        else:
            self.import_image()

    # -------------------------------------------------------------- inspector
    def _load_inspector(self):
        obj = self.selected()
        keys = ("name", "x", "y", "rot", "sx", "sy", "w", "h", "color", "opacity", "layer",
                "speed", "vx", "vy") + _COL_TEXT_KEYS
        if obj is None:
            for k in keys:
                self.vars[k].set("")
            self.vars["asset"].set("(없음)")
            self.vars["visible"].set(True)
            self.vars["controlled"].set(False)
            for k in _COL_FLAG_KEYS:
                self.vars[k].set(False)
            self._orig = {}
            self._orig_flags = {}
            self._sync_collision_widgets()
            return
        t = obj.transform
        sp = obj.components.get("sprite2d") if isinstance(obj.components, dict) else None
        sp = sp if isinstance(sp, dict) else {}
        col = obj.components.get("collider2d") if isinstance(obj.components, dict) else None
        col = col if isinstance(col, dict) else None
        body = obj.components.get("body2d") if isinstance(obj.components, dict) else None
        body = body if isinstance(body, dict) else {}
        c_off = col.get("offset") if col else None
        c_off = c_off if isinstance(c_off, (list, tuple)) and len(c_off) >= 2 else [0, 0]
        c_size = col.get("size") if col else None
        c_size = c_size if isinstance(c_size, (list, tuple)) and len(c_size) >= 2 else \
            [sp.get("width", 32), sp.get("height", 32)]
        dflt_body = default_body2d()
        mo = obj.components.get("motion2d") if isinstance(obj.components, dict) else None
        mo = mo if isinstance(mo, dict) else {}
        vel = mo.get("velocity", [0, 0])
        vel = vel if isinstance(vel, (list, tuple)) and len(vel) >= 2 else [0, 0]
        text = {
            "name": obj.name, "x": _fmt(t.position[0]), "y": _fmt(t.position[1]),
            "rot": _fmt(t.rotation[2]), "sx": _fmt(t.scale[0]), "sy": _fmt(t.scale[1]),
            "w": _fmt(sp.get("width", 32)), "h": _fmt(sp.get("height", 32)),
            "color": str(sp.get("color", "#edbd52")), "opacity": _fmt(sp.get("opacity", 255)),
            "layer": _fmt(sp.get("layer", 0)), "speed": _fmt(mo.get("speed", 80)),
            "vx": _fmt(vel[0]), "vy": _fmt(vel[1]),
            "cox": _fmt(c_off[0]), "coy": _fmt(c_off[1]),
            "cw": _fmt(c_size[0]), "ch": _fmt(c_size[1]),
            "gravity": _fmt(body.get("gravity", dflt_body["gravity"])),
            "jump": _fmt(body.get("jump_speed", dflt_body["jump_speed"])),
            "maxfall": _fmt(body.get("max_fall_speed", dflt_body["max_fall_speed"])),
        }
        for k, v in text.items():
            self.vars[k].set(v)
        self._orig = dict(text)
        self.vars["asset"].set(sp.get("asset") or "(없음)")
        flags = {"visible": bool(obj.visible),
                 "controlled": bool(mo.get("controlled", False)),
                 "col_enabled": bool(col is not None and col.get("enabled", True) is True),
                 "col_trigger": bool(col is not None and col.get("trigger", False) is True),
                 "col_fit": bool(col is None or col.get("fit_sprite", True) is True),
                 "body_dynamic": body.get("type") == "dynamic"}
        for k, v in flags.items():
            self.vars[k].set(v)
        self._orig_flags = flags
        self._sync_collision_widgets()

    def _load_project_fields(self, keep_pending=False):
        p = self.project
        new = {"p_name": p.name, "p_w": str(p.width), "p_h": str(p.height), "p_bg": p.background}
        for k, v in new.items():
            old = self._proj_orig.get(k)
            if keep_pending and old is not None and self.vars[k].get().strip() != old.strip():
                continue
            self.vars[k].set(v)
        self._proj_orig = new

    def _num(self, key, label, lo, hi, integer=False, nonzero=False, force=False):
        text = self.vars[key].get().strip()
        if not force and text == self._orig.get(key):
            return None
        try:
            v = float(text)
        except ValueError:
            raise ValueError(f"{label}: 숫자를 입력하세요.") from None
        if not math.isfinite(v):
            raise ValueError(f"{label}: 유한한 숫자를 입력하세요.")
        if integer:
            if not v.is_integer():
                raise ValueError(f"{label}: 정수를 입력하세요.")
            v = int(v)
        if nonzero and v == 0:
            raise ValueError(f"{label}: 0은 사용할 수 없습니다.")
        if not (lo <= v <= hi):
            raise ValueError(f"{label}: {_fmt(lo)} ~ {_fmt(hi)} 범위로 입력하세요.")
        return v

    def apply_properties(self) -> bool:
        if self.playing:
            return False
        obj = self.selected()
        if obj is None:
            return False
        oid = obj.id
        try:
            name = self.vars["name"].get().strip()
            if not name or len(name) > 64:
                raise ValueError("이름: 1~64자로 입력하세요.")
            v = {
                "x": self._num("x", "위치 X", -POS_LIMIT, POS_LIMIT),
                "y": self._num("y", "위치 Y", -POS_LIMIT, POS_LIMIT),
                "rot": self._num("rot", "회전", -36000, 36000),
                "sx": self._num("sx", "배율 X", -1000, 1000, nonzero=True),
                "sy": self._num("sy", "배율 Y", -1000, 1000, nonzero=True),
                "w": self._num("w", "너비", 1e-3, 8192),
                "h": self._num("h", "높이", 1e-3, 8192),
                "opacity": self._num("opacity", "불투명도", 0, 255, integer=True),
                "layer": self._num("layer", "레이어", -1000, 1000, integer=True),
                "speed": self._num("speed", "속도", -10000, 10000),
                "vx": self._num("vx", "초기 속도 X", -10000, 10000),
                "vy": self._num("vy", "초기 속도 Y", -10000, 10000),
            }
            color = self.vars["color"].get().strip()
            if color != self._orig.get("color"):
                if not HEX_RE.match(color):
                    raise ValueError("색상: #rrggbb 형식으로 입력하세요. 예: #2bb3a3")
                color = color.lower()
            # Collision fields are validated together with the rest before any mutation.
            comps = obj.components if isinstance(obj.components, dict) else {}
            has_col = isinstance(comps.get("collider2d"), dict)
            has_body = isinstance(comps.get("body2d"), dict)

            def changed(*keys):
                return any(self.vars[k].get().strip() != self._orig.get(k, "").strip()
                           if k in _COL_TEXT_KEYS else
                           bool(self.vars[k].get()) != self._orig_flags.get(k, False)
                           for k in keys)

            col_touched = changed("col_enabled", "col_trigger", "col_fit", "cox", "coy", "cw", "ch")
            body_touched = changed("body_dynamic", "gravity", "jump", "maxfall")
            new_col = col_touched and not has_col
            new_body = body_touched and not has_body
            cv = {
                "cox": self._num("cox", "충돌 오프셋 X", -16384, 16384, force=new_col),
                "coy": self._num("coy", "충돌 오프셋 Y", -16384, 16384, force=new_col),
                "cw": self._num("cw", "충돌 너비", 1e-3, 16384, force=new_col),
                "ch": self._num("ch", "충돌 높이", 1e-3, 16384, force=new_col),
                "gravity": self._num("gravity", "중력", 0, 10000, force=new_body),
                "jump": self._num("jump", "점프 속도", 0, 10000, force=new_body),
                "maxfall": self._num("maxfall", "최대 낙하 속도", 1e-3, 10000, force=new_body),
            }
        except ValueError as exc:
            self.insp_msg.set(str(exc))
            self._error(str(exc))
            return False
        visible = bool(self.vars["visible"].get())
        controlled = bool(self.vars["controlled"].get())
        flag = {k: bool(self.vars[k].get()) for k in _COL_FLAG_KEYS}
        flag_changed = {k: flag[k] != self._orig_flags.get(k, False) for k in _COL_FLAG_KEYS}

        def mutate():
            o = self.project.object_by_id(oid)
            o.name = name
            o.visible = visible
            for key, idx, attr in (("x", 0, "position"), ("y", 1, "position"),
                                   ("sx", 0, "scale"), ("sy", 1, "scale")):
                if v[key] is not None:
                    getattr(o.transform, attr)[idx] = float(v[key])
            if v["rot"] is not None:
                o.transform.rotation[2] = float(v["rot"])
            sp = o.components.get("sprite2d")
            if isinstance(sp, dict):
                if v["w"] is not None:
                    sp["width"] = float(v["w"])
                if v["h"] is not None:
                    sp["height"] = float(v["h"])
                if v["opacity"] is not None:
                    sp["opacity"] = int(v["opacity"])
                if v["layer"] is not None:
                    sp["layer"] = int(v["layer"])
                if color != self._orig.get("color"):
                    sp["color"] = color
            mo = o.components.get("motion2d")
            if isinstance(mo, dict):
                mo["controlled"] = controlled
                if v["speed"] is not None:
                    mo["speed"] = float(v["speed"])
                vel = mo.get("velocity")
                vel = list(vel) if isinstance(vel, (list, tuple)) and len(vel) >= 2 else [0.0, 0.0]
                if v["vx"] is not None:
                    vel[0] = float(v["vx"])
                if v["vy"] is not None:
                    vel[1] = float(v["vy"])
                mo["velocity"] = vel
            elif controlled or any(v[k] not in (None, 0, 0.0) for k in ("speed", "vx", "vy")):
                o.components["motion2d"] = {
                    "controlled": controlled,
                    "speed": float(v["speed"] if v["speed"] is not None else 80),
                    "velocity": [float(v["vx"] or 0), float(v["vy"] or 0)]}
            if col_touched:
                c = o.components.get("collider2d")
                fresh = not isinstance(c, dict)
                if fresh:
                    c = default_collider2d()
                    o.components["collider2d"] = c
                for key, component_key in (("col_enabled", "enabled"), ("col_trigger", "trigger"),
                                           ("col_fit", "fit_sprite")):
                    if fresh or flag_changed[key]:
                        c[component_key] = flag[key]
                off = list(c.get("offset") or [0.0, 0.0])
                size = list(c.get("size") or [32.0, 32.0])
                for i, (ko, ks) in enumerate((("cox", "cw"), ("coy", "ch"))):
                    if cv[ko] is not None:
                        off[i] = float(cv[ko])
                    if cv[ks] is not None:
                        size[i] = float(cv[ks])
                c["offset"], c["size"] = off, size
            if body_touched:
                b = o.components.get("body2d")
                fresh = not isinstance(b, dict)
                if fresh:
                    b = default_body2d()
                    o.components["body2d"] = b
                if fresh or flag_changed["body_dynamic"]:
                    b["type"] = "dynamic" if flag["body_dynamic"] else "static"
                for key, component_key in (("gravity", "gravity"), ("jump", "jump_speed"),
                                           ("maxfall", "max_fall_speed")):
                    if cv[key] is not None:
                        b[component_key] = float(cv[key])

        ok, _ = self._transaction(mutate)
        if not ok:
            return False
        self.insp_msg.set("")
        self._after_change()
        self._load_inspector()
        self._draw_overlay()
        self._info("속성을 적용했습니다.")
        return True

    def apply_collision_preset(self, preset: str) -> bool:
        """One undoable edit; keeps sprite and unknown component keys intact."""
        if self.playing or preset not in COLLISION_PRESETS or self.selected() is None:
            return False
        if not self._commit_pending():
            return False
        oid = self.selected_id

        def mutate():
            o = self.project.object_by_id(oid)
            col = o.components.get("collider2d")
            body = o.components.get("body2d")
            if preset == "none":
                if isinstance(col, dict):
                    col["enabled"] = False
                return
            if not isinstance(col, dict):
                col = default_collider2d()
                sp = o.components.get("sprite2d")
                if isinstance(sp, dict):
                    col["size"] = [float(sp.get("width", 32)), float(sp.get("height", 32))]
                o.components["collider2d"] = col
            col["enabled"] = True
            col["trigger"] = preset == "trigger"
            if preset == "player":
                if not isinstance(body, dict):
                    body = default_body2d()
                    o.components["body2d"] = body
                body["type"] = "dynamic"
                if body.get("gravity", 600.0) <= 0:
                    body["gravity"] = 600.0
                if body.get("jump_speed", 220.0) <= 0:
                    body["jump_speed"] = 220.0
                mo = o.components.get("motion2d")
                if isinstance(mo, dict):
                    mo["controlled"] = True
                else:
                    o.components["motion2d"] = {"controlled": True, "speed": 80.0,
                                                "velocity": [0.0, 0.0]}
            elif isinstance(body, dict):
                body["type"] = "static"

        ok, _ = self._transaction(mutate)
        if not ok:
            return False
        self.insp_msg.set("")
        self._after_change()
        self._load_inspector()
        self._draw_overlay()
        self._info("충돌 설정을 적용했습니다.")
        return True

    def apply_project_settings(self) -> bool:
        if self.playing:
            return False
        try:
            name = self.vars["p_name"].get().strip()
            if not name or len(name) > 64:
                raise ValueError("프로젝트 이름: 1~64자로 입력하세요.")
            dims = []
            for k, lab in (("p_w", "너비"), ("p_h", "높이")):
                try:
                    f = float(self.vars[k].get().strip())
                except ValueError:
                    raise ValueError(f"해상도 {lab}: 정수를 입력하세요.") from None
                if not math.isfinite(f) or not f.is_integer() or not (1 <= f <= 2048):
                    raise ValueError(f"해상도 {lab}: 1 ~ 2048 사이 정수로 입력하세요.")
                dims.append(int(f))
            if dims[0] * dims[1] > 1048576:
                raise ValueError("해상도: 너비×높이는 1,048,576 이하여야 합니다.")
            bg = self.vars["p_bg"].get().strip()
            if not HEX_RE.match(bg):
                raise ValueError("배경색: #rrggbb 형식으로 입력하세요.")
        except ValueError as exc:
            self.proj_msg.set(str(exc))
            self._error(str(exc))
            return False

        def mutate():
            p = self.project
            p.name, p.width, p.height, p.background = name, dims[0], dims[1], bg.lower()

        ok, _ = self._transaction(mutate)
        if not ok:
            self.proj_msg.set(self.status_var.get())
            return False
        self.proj_msg.set("")
        self._after_change(keep_pending=False)
        if self._fit:
            self.fit_view()
        self._info("프로젝트 설정을 적용했습니다.")
        return True

    def _pick_color(self):
        if self.selected() is None or self.playing:
            return
        _rgb, hexv = colorchooser.askcolor(color=self.vars["color"].get() or "#ffffff",
                                           parent=self.root, title="색상 선택")
        if hexv:
            self.vars["color"].set(hexv)
            self.apply_properties()

    def _pick_bg(self):
        if self.playing:
            return
        _rgb, hexv = colorchooser.askcolor(color=self.vars["p_bg"].get() or "#17212c",
                                           parent=self.root, title="배경색 선택")
        if hexv:
            self.vars["p_bg"].set(hexv)
            self.apply_project_settings()

    # ------------------------------------------------------------ file dialogs
    # Overridable (tests replace these to avoid modal dialogs).
    def ask_unsaved(self) -> str:
        r = messagebox.askyesnocancel(
            "저장되지 않은 변경",
            f"'{self.project.name}'의 변경 사항이 저장되지 않았습니다.\n\n"
            "예: 저장 후 계속 / 아니요: 저장하지 않고 계속 / 취소: 돌아가기",
            icon="warning", parent=self.root)
        return "save" if r is True else "discard" if r is False else "cancel"

    def ask_open_path(self):
        p = filedialog.askopenfilename(parent=self.root, title="프로젝트 열기",
                                       filetypes=[("Pixelica 프로젝트", "*.json"), ("모든 파일", "*.*")])
        return Path(p) if p else None

    def ask_save_path(self):
        p = filedialog.asksaveasfilename(parent=self.root, title="프로젝트 저장",
                                         defaultextension=".json", initialfile=f"{self.project.name}.json",
                                         filetypes=[("Pixelica 프로젝트", "*.json"), ("모든 파일", "*.*")])
        return Path(p) if p else None

    def ask_image_path(self):
        p = filedialog.askopenfilename(parent=self.root, title="이미지 가져오기",
                                       filetypes=[("이미지", "*.png *.jpg *.jpeg *.bmp *.ppm"),
                                                  ("모든 파일", "*.*")])
        return Path(p) if p else None

    # ------------------------------------------------------------- file flows
    def _confirm_discard(self) -> bool:
        if not self.is_dirty():
            return True
        choice = self.ask_unsaved()
        if choice == "save":
            return self.save()
        return choice == "discard"

    def _replace_project(self, project: Project):
        self.session = None
        self.paused = False
        self.play_keys.clear()
        self._drag = None
        self.project = project
        self.selected_id = None
        self._undo.clear()
        self._redo.clear()
        self._saved_key = self._key()
        self.insp_msg.set("")
        self.proj_msg.set("")
        self._fit = True
        self._last_image = None
        self._refresh_all()
        self.fit_view()

    def new(self) -> bool:
        if self.playing or not self._confirm_discard():
            return False
        try:
            proj = new_project()
        except Exception as exc:
            self._error(f"새 프로젝트를 만들 수 없습니다: {exc}")
            return False
        self._replace_project(proj)
        self._info("새 샘플 장면을 만들었습니다.")
        return True

    def save(self, path=None) -> bool:
        if not self._commit_pending():
            return False
        target = Path(path) if path else self.project.path
        if target is None:
            target = self.ask_save_path()
            if target is None:
                return False
        old_dir = Path(self.project.path).parent if self.project.path is not None else None
        try:
            save_project(self.project, Path(target))
        except Exception as exc:
            self._error(f"저장하지 못했습니다: {exc}")
            return False
        new_dir = Path(self.project.path).parent
        if old_dir is not None and (os.path.normcase(os.path.abspath(old_dir))
                                    != os.path.normcase(os.path.abspath(new_dir))):
            # Snapshots may reference assets relative to the old asset root.
            self._undo.clear()
            self._redo.clear()
        self._saved_key = self._key()
        self._update_controls()
        self._update_info()
        self._info(f"저장했습니다: {target}")
        return True

    def save_as(self, path=None) -> bool:
        target = Path(path) if path else self.ask_save_path()
        if target is None:
            return False
        return self.save(target)

    def open_project(self, path=None) -> bool:
        if self.playing:
            self._info("재생을 정지한 뒤 열 수 있습니다.")
            return False
        target = Path(path) if path else self.ask_open_path()
        if target is None:
            return False
        try:
            proj = load_project(Path(target))
            if self.renderer is not None:
                self.renderer.render(proj)  # readiness check before replacing anything
        except Exception as exc:
            self._error(f"열 수 없습니다 (현재 프로젝트는 유지됨): {exc}")
            return False
        if not self._confirm_discard():
            return False
        self._replace_project(proj)
        self._info(f"열었습니다: {target}")
        return True

    def close(self) -> bool:
        if self._closed:
            return True
        if self.playing:
            self.stop_play()
        if not self._confirm_discard():
            return False
        self._closed = True
        if self._after_id is not None:
            try:
                self.root.after_cancel(self._after_id)
            except tk.TclError:
                pass
            self._after_id = None
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            try:
                self.root.unbind_all(seq)
            except tk.TclError:
                pass
        if self.renderer is not None:
            try:
                self.renderer.close()
            except Exception:
                pass
            self.renderer = None
        try:
            self.root.destroy()
        except tk.TclError:
            pass
        return True

    # ---------------------------------------------------------------- assets
    def _import_source(self, source):
        """Save-if-needed, pick, and import an image. Returns (rel_path, (w, h)) or None."""
        if self.playing or not self._commit_pending():
            return None
        if self.project.path is None:
            self._info("이미지를 가져오려면 먼저 프로젝트를 저장해야 합니다.")
            if not self.save_as():
                return None
        src = Path(source) if source else self.ask_image_path()
        if src is None:
            return None
        try:
            with Image.open(src) as im:
                size = im.size
            rel = import_asset(self.project, src)
        except Exception as exc:
            self._error(f"이미지를 가져오지 못했습니다: {exc}")
            return None
        return rel, size

    def import_image(self, source=None) -> bool:
        got = self._import_source(source)
        if got is None:
            return False
        rel, (iw, ih) = got
        k = min(1.0, 256.0 / max(iw, ih, 1))
        w, h = max(1.0, round(iw * k)), max(1.0, round(ih * k))

        def mutate():
            obj = self.project.add_object(Path(rel).stem[:40] or "이미지", "sprite2d")
            sp = obj.components["sprite2d"]
            sp.update(asset=rel, color="#ffffff", width=float(w), height=float(h))
            others = [o.components["sprite2d"].get("layer", 0) for o in self.project.objects
                      if o is not obj and isinstance(o.components.get("sprite2d"), dict)]
            if others:
                sp["layer"] = max(others) + 1
            return obj.id

        ok, oid = self._transaction(mutate, render_check=True)
        if not ok:
            return False
        self._after_change()
        self.select_object(oid, commit=False)
        self._info(f"이미지 스프라이트를 추가했습니다: {rel}")
        return True

    def choose_asset(self, source=None) -> bool:
        oid = self.selected_id
        if not oid or self.playing:
            return False
        got = self._import_source(source)
        if got is None:
            return False
        rel, _size = got

        def mutate():
            sp = self.project.object_by_id(oid).components["sprite2d"]
            sp["asset"], sp["color"] = rel, "#ffffff"

        ok, _ = self._transaction(mutate, render_check=True)
        if ok:
            self._after_change()
            self._load_inspector()
            self._info(f"이미지를 지정했습니다: {rel}")
        return ok

    def clear_asset(self) -> bool:
        if self.playing or not self.selected_id or not self._commit_pending():
            return False
        oid = self.selected_id
        obj = self.selected()
        if obj is None:
            return False
        if not isinstance(obj.components.get("sprite2d"), dict):
            return False

        def mutate():
            self.project.object_by_id(oid).components["sprite2d"]["asset"] = None

        ok, _ = self._transaction(mutate)
        if ok:
            self._after_change()
            self._load_inspector()
        return ok

    # ------------------------------------------------------------------ play
    def toggle_play(self):
        if self.session is None:
            if self._drag:
                self.end_drag()
            if not self._commit_pending():
                return
            try:
                self.session = PlaySession(self.project)
            except Exception as exc:
                self.session = None
                self._error(f"재생을 시작할 수 없습니다: {exc}")
                return
            self.paused = False
            self.play_keys.clear()
            self._acc = 0.0
            self._last_tick = time.monotonic()
            self.canvas.focus_set()
            self._info("재생 중 — 캔버스에 포커스가 있을 때 WASD/방향키로 이동, "
                       "Space/W/↑로 점프합니다. Esc로 정지.")
        else:
            self.paused = not self.paused
            self.play_keys.clear()
            self._acc = 0.0
            self._last_tick = time.monotonic()
            self._info("일시정지" if self.paused else "재생 중")
        self._need_render = True
        self._update_controls()
        self._update_info()
        self._draw_overlay()

    def stop_play(self):
        if self.session is None:
            return
        self.session = None
        self.paused = False
        self.play_keys.clear()
        self._need_render = True
        self._load_inspector()
        self._update_controls()
        self._update_info()
        self._draw_overlay()
        self._info("정지했습니다. 편집 상태로 돌아왔습니다.")

    def step_play(self, frames: int = 1) -> int:
        """Advance the play session deterministically (tests / manual stepping)."""
        if self.session is None:
            return 0
        for _ in range(max(0, int(frames))):
            self.session.step(STEP, set(self.play_keys))
        self._need_render = True
        self._update_info()
        return self.session.ticks

    def press_key(self, keysym: str):
        names = _KEY_NAMES.get(keysym.lower())
        if names and self.playing:
            self.play_keys.update(names)

    def release_key(self, keysym: str):
        names = _KEY_NAMES.get(keysym.lower())
        if names:
            self.play_keys.difference_update(names)

    def _on_key_press(self, event):
        if self.playing:
            if event.keysym in ("Escape", "F5"):
                return None  # let the window-level shortcuts handle stop/pause
            if event.keysym.lower() in _KEY_NAMES:
                self.press_key(event.keysym)
                return "break"
            return None
        key = event.keysym.lower()
        if key in ("up", "down", "left", "right") and self.selected() is not None:
            step = GRID if self.snap_var.get() else 1
            dx = {"left": -step, "right": step}.get(key, 0)
            dy = {"up": -step, "down": step}.get(key, 0)
            self.nudge(dx, dy)
            return "break"
        return None

    def _on_key_release(self, event):
        self.release_key(event.keysym)

    def _on_focus_out(self):
        self.play_keys.clear()

    def nudge(self, dx: float, dy: float) -> bool:
        if self.playing or not self.selected_id or not self._commit_pending():
            return False
        oid = self.selected_id

        def mutate():
            p = self.project.object_by_id(oid).transform.position
            p[0] = max(-POS_LIMIT, min(POS_LIMIT, p[0] + dx))
            p[1] = max(-POS_LIMIT, min(POS_LIMIT, p[1] + dy))

        ok, _ = self._transaction(mutate)
        if ok:
            self._after_change(structural=False)
            self._load_inspector()
            self._draw_overlay()
        return ok

    # ----------------------------------------------------------- hit / drag
    def hit_test(self, x: float, y: float):
        return hit_test_project(self.project, x, y)

    def begin_drag(self, sx: float, sy: float):
        """Start a drag at stage coords; selects the topmost hit. Returns the id or None."""
        if self.playing:
            return None
        if not self._commit_pending():
            return None
        hit = self.hit_test(sx, sy)
        self.select_object(hit, commit=False)
        if hit is None:
            return None
        obj = self.project.object_by_id(hit)
        before = self._snapshot()
        self._drag = {"id": hit, "start": (sx, sy), "orig": (obj.transform.position[0], obj.transform.position[1]),
                      "before": before, "key": json.dumps(before, sort_keys=True), "moved": False}
        return hit

    def drag_to(self, sx: float, sy: float):
        d = self._drag
        if d is None:
            return
        obj = self.project.object_by_id(d["id"])
        if obj is None:
            self._drag = None
            return
        nx = d["orig"][0] + (sx - d["start"][0])
        ny = d["orig"][1] + (sy - d["start"][1])
        if self.snap_var.get():
            nx, ny = round(nx / GRID) * GRID, round(ny / GRID) * GRID
        nx = max(-POS_LIMIT, min(POS_LIMIT, nx))
        ny = max(-POS_LIMIT, min(POS_LIMIT, ny))
        obj.transform.position[0], obj.transform.position[1] = float(nx), float(ny)
        d["moved"] = True
        self._after_change(structural=False)
        self._load_inspector()
        self._draw_overlay()

    def end_drag(self) -> bool:
        d, self._drag = self._drag, None
        if d is None or not d["moved"]:
            return False
        if self._key() != d["key"]:
            self._push_undo(d["before"])
        self._update_controls()
        self._update_info()
        return True

    # ------------------------------------------------------------ canvas math
    def canvas_to_stage(self, cx, cy):
        return (cx - self.ox) / self.zoom, (cy - self.oy) / self.zoom

    def stage_to_canvas(self, x, y):
        return self.ox + x * self.zoom, self.oy + y * self.zoom

    def _max_zoom(self):
        return min(ZOOM_MAX, MAX_DISPLAY / max(self.project.width, self.project.height, 1))

    def fit_view(self):
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        z = min((cw - 24) / self.project.width, (ch - 24) / self.project.height)
        z = math.floor(z) if z >= 1 else z
        self.zoom = max(ZOOM_MIN, min(self._max_zoom(), z))
        self.ox = (cw - self.project.width * self.zoom) / 2
        self.oy = (ch - self.project.height * self.zoom) / 2
        self._fit = True
        self._present()

    def set_zoom(self, z, cx=None, cy=None):
        cx = self.canvas.winfo_width() / 2 if cx is None else cx
        cy = self.canvas.winfo_height() / 2 if cy is None else cy
        sx, sy = self.canvas_to_stage(cx, cy)
        self.zoom = max(ZOOM_MIN, min(self._max_zoom(), z))
        self.ox, self.oy = cx - sx * self.zoom, cy - sy * self.zoom
        self._fit = False
        self._present()
        self._update_info()

    def zoom_by(self, factor, cx=None, cy=None):
        self.set_zoom(self.zoom * factor, cx, cy)

    def _on_canvas_configure(self):
        if self._fit:
            self.fit_view()
        else:
            self._present()

    def _on_wheel(self, event):
        direction = 1 if (getattr(event, "num", 0) == 4 or getattr(event, "delta", 0) > 0) else -1
        try:
            w = self.root.winfo_containing(event.x_root, event.y_root)
        except (KeyError, tk.TclError):
            return None
        node = w
        while node is not None:
            if node is self.canvas:
                cx = event.x_root - self.canvas.winfo_rootx()
                cy = event.y_root - self.canvas.winfo_rooty()
                self.zoom_by(1.25 if direction > 0 else 1 / 1.25, cx, cy)
                return "break"
            if node is self.insp_holder:
                self.insp_canvas.yview_scroll(-direction * 2, "units")
                return "break"
            node = getattr(node, "master", None)
        return None

    def _pan_start(self, event):
        self._pan = (event.x, event.y, self.ox, self.oy)

    def _pan_move(self, event):
        if self._pan is None:
            return
        x0, y0, ox, oy = self._pan
        self.ox, self.oy = ox + event.x - x0, oy + event.y - y0
        self._fit = False
        self._present()

    def _on_press(self, event):
        self.canvas.focus_set()
        if self.playing:
            return
        self.begin_drag(*self.canvas_to_stage(event.x, event.y))

    def _on_motion(self, event):
        if self._drag is not None:
            self.drag_to(*self.canvas_to_stage(event.x, event.y))

    def _on_release(self, event):
        self.end_drag()

    # -------------------------------------------------------------- rendering
    def _tick(self):
        self._after_id = None
        if self._closed:
            return
        try:
            now = time.monotonic()
            dt = min(now - self._last_tick, 0.25)
            self._last_tick = now
            if self.session is not None and not self.paused:
                self._acc += dt
                n = 0
                while self._acc >= STEP and n < MAX_STEPS:
                    self.session.step(STEP, set(self.play_keys))
                    self._acc -= STEP
                    n += 1
                if n == MAX_STEPS:
                    self._acc = 0.0  # drop backlog instead of spiralling
                if n:
                    self._need_render = True
                    self._update_info()
            if self._need_render and now - self._last_render >= FRAME:
                self.render_now()
        except Exception as exc:
            if self.session is not None:
                self.stop_play()
            self._error(f"재생/렌더링 오류: {exc}")
        finally:
            if not self._closed:
                self._after_id = self.root.after(TICK_MS, self._tick)

    def render_now(self):
        """Render the current edit/play project natively; returns the PIL image or None."""
        self._need_render = False
        self._last_render = time.monotonic()
        src = self.session.project if self.session is not None else self.project
        if self.renderer is not None:
            try:
                self._last_image = self.renderer.render(src)
                self._img_serial += 1
                if self.render_error:
                    self.render_error = None
            except Exception as exc:
                msg = f"렌더링 오류: {exc}"
                if msg != self.render_error:
                    self._error(msg)
                self.render_error = msg
        self._present()
        return self._last_image

    def _present(self):
        c = self.canvas
        c.delete("view")
        img = self._last_image
        w, h = self.project.width, self.project.height
        sw, sh = max(1, round(w * self.zoom)), max(1, round(h * self.zoom))
        if img is not None:
            key = (self._img_serial, sw, sh)
            if key != self._photo_key:
                self._photo = ImageTk.PhotoImage(img.resize((sw, sh), RESAMPLE_NEAREST), master=c)
                self._photo_key = key
            c.create_image(self.ox, self.oy, image=self._photo, anchor="nw", tags="view")
        else:
            c.create_rectangle(self.ox, self.oy, self.ox + sw, self.oy + sh, fill=P["field"],
                               outline="", tags="view")
        c.create_rectangle(self.ox - 1, self.oy - 1, self.ox + sw, self.oy + sh,
                           outline=P["border"], tags="view")
        if self.render_error and img is None:
            c.create_text(self.ox + sw / 2, self.oy + sh / 2, text=self.render_error, fill=P["error"],
                          width=max(200, sw - 40), font=self.font, tags="view")
        if self.playing:
            label = "일시정지 — F5 계속 · Esc 정지" if self.paused else "재생 중 — Esc 정지"
            fill = P["warn"] if self.paused else P["accent_dark"]
            extra = self.play_collision_text()
            if extra:
                label += "\n" + extra
            t = c.create_text(self.ox + 14, self.oy + 12, text=label, anchor="nw", fill="#ffffff",
                              font=self.font_bold, tags="view")
            bb = c.bbox(t)
            r = c.create_rectangle(bb[0] - 6, bb[1] - 3, bb[2] + 6, bb[3] + 3, fill=fill, outline="",
                                   tags="view")
            c.tag_lower(r, t)
        self._draw_overlay()

    def _draw_collider_region(self, obj, col):
        """Custom collider rectangle (scaled+rotated); the AABB around it is what collides."""
        try:
            (ox, oy), (w, h) = col["offset"], col["size"]
            t = obj.transform
            th = math.radians(float(t.rotation[2]))
            cs, sn = math.cos(th), math.sin(th)
            pts = []
            for lx, ly in ((ox, oy), (ox + w, oy), (ox + w, oy + h), (ox, oy + h)):
                lx, ly = lx * t.scale[0], ly * t.scale[1]
                pts.extend(self.stage_to_canvas(t.position[0] + lx * cs - ly * sn,
                                                t.position[1] + lx * sn + ly * cs))
        except (KeyError, TypeError, ValueError):
            return
        self.canvas.create_polygon(*pts, fill="", outline="#ffffff", dash=(2, 2), tags="overlay")

    def _draw_overlay(self):
        c = self.canvas
        c.delete("overlay")
        sel = None if self.playing else self.selected()
        source = self.session.project if self.playing else self.project
        if self.collision_var.get():
            for o in source.objects:
                if not isinstance(o.components, dict) or "collider2d" not in o.components:
                    continue
                try:
                    box = collider_bounds(o)
                except ProjectError:
                    continue  # invalid geometry is reported on apply/save, not drawn
                if box is None:
                    continue
                trig = o.components["collider2d"].get("trigger") is True
                x0, y0 = self.stage_to_canvas(box[0], box[1])
                x1, y1 = self.stage_to_canvas(box[2], box[3])
                c.create_rectangle(x0, y0, x1, y1, outline=P["trigger" if trig else "solid"],
                                   width=2 if o is sel else 1, dash=(4, 3) if trig else (),
                                   tags=("overlay", "collider"))
            if sel is not None and isinstance(sel.components.get("collider2d"), dict):
                col = sel.components["collider2d"]
                if col.get("enabled") is True and col.get("fit_sprite") is False:
                    self._draw_collider_region(sel, col)
        if self.playing:
            return
        obj = sel
        if obj is None:
            return
        corners = object_corners(obj)
        if corners is None:
            return
        pts = []
        for x, y in corners:
            pts.extend(self.stage_to_canvas(x, y))
        c.create_polygon(*pts, fill="", outline=P["accent"], width=2, tags="overlay")
        px, py = self.stage_to_canvas(obj.transform.position[0], obj.transform.position[1])
        c.create_oval(px - 4, py - 4, px + 4, py + 4, fill=P["accent"], outline="#ffffff", tags="overlay")


def _takes_event(fn) -> bool:
    """True if a callback accepts a positional event argument."""
    code = getattr(fn, "__code__", None)
    if code is None:
        return False
    names = code.co_varnames[:code.co_argcount]
    if getattr(fn, "__self__", None) is not None:
        names = names[1:]
    return len(names) > 0 and not _all_defaults(fn, len(names))


def _all_defaults(fn, n_args) -> bool:
    d = getattr(fn, "__defaults__", None) or ()
    return len(d) >= n_args


def main(argv=None) -> None:
    argv = sys.argv[1:] if argv is None else argv
    root = tk.Tk()
    app = EditorApp(root)
    if argv:
        app.open_project(Path(argv[0]))
    root.mainloop()


if __name__ == "__main__":
    main()
