"""Pixelica editor project/object data model (stdlib only, no GUI/runtime imports).

Trust boundary: everything read from disk goes through ``Project.from_dict``,
which validates strictly and returns a *new* object, so a failed load can never
touch an in-memory project. Saving validates the same way before touching disk.

Limitations: asset files are only checked by extension + magic bytes here (the
runtime decodes them with Pillow); missing asset files are tolerated on load.
Unknown JSON-safe keys on the root, objects and components are preserved.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
import re
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

FORMAT = "pixelica.project"
VERSION = 1

MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_ASSET_BYTES = 32 * 1024 * 1024
MAX_OBJECTS = 512
MAX_DIMENSION = 2048
MAX_PIXELS = 1048576
MAX_NAME = 128
MAX_ASSET_PATH = 240
ASSET_EXTENSIONS = (".png", ".jpg", ".jpeg", ".ppm", ".bmp")
ASSETS_DIR = "assets"

_POS_LIMIT = 1.0e6
_ROT_LIMIT = 1.0e5
_SCALE_LIMIT = 1.0e4
_SIZE_LIMIT = 16384.0
_SPEED_LIMIT = 10000.0
_LAYER_LIMIT = 1_000_000
_ID_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
_COLOR_RE = re.compile(r"^#[0-9a-fA-F]{6}$")
_CTRL_RE = re.compile(r"[\x00-\x1f\x7f]")


class ProjectError(ValueError):
    """Invalid project data, unsafe path or failed save/import."""


# ---------------------------------------------------------------- validation
def _num(v: Any, where: str, lo: float, hi: float) -> float:
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ProjectError(f"{where}: number expected")
    try:
        f = float(v)
    except OverflowError:
        raise ProjectError(f"{where}: number out of range") from None
    if not math.isfinite(f) or f < lo or f > hi:
        raise ProjectError(f"{where}: must be finite and within [{lo:g}, {hi:g}]")
    return f


def _int(v: Any, where: str, lo: int, hi: int) -> int:
    if isinstance(v, float) and v.is_integer():
        v = int(v)
    if isinstance(v, bool) or not isinstance(v, int):
        raise ProjectError(f"{where}: integer expected")
    if v < lo or v > hi:
        raise ProjectError(f"{where}: must be within [{lo}, {hi}]")
    return v


def _str(v: Any, where: str, maxlen: int = MAX_NAME, allow_empty: bool = True) -> str:
    if not isinstance(v, str):
        raise ProjectError(f"{where}: string expected")
    if len(v) > maxlen or _CTRL_RE.search(v) or (not allow_empty and not v.strip()):
        raise ProjectError(f"{where}: invalid string")
    return v


def _color(v: Any, where: str) -> str:
    if not isinstance(v, str) or not _COLOR_RE.match(v):
        raise ProjectError(f"{where}: '#rrggbb' expected")
    return v.lower()


def _vec(v: Any, n: int, where: str, lo: float, hi: float) -> list[float]:
    if not isinstance(v, (list, tuple)) or len(v) != n:
        raise ProjectError(f"{where}: list of {n} numbers expected")
    return [_num(x, f"{where}[{i}]", lo, hi) for i, x in enumerate(v)]


def _json_safe(v: Any, where: str, depth: int = 0) -> Any:
    """Deep-copy a JSON-safe value, rejecting anything else."""
    if depth > 16:
        raise ProjectError(f"{where}: nested too deeply")
    if v is None or isinstance(v, (bool, str)):
        return v
    if isinstance(v, int):
        if abs(v) > 2**53:
            raise ProjectError(f"{where}: integer out of range")
        return v
    if isinstance(v, float):
        if not math.isfinite(v):
            raise ProjectError(f"{where}: non-finite number")
        return v
    if isinstance(v, (list, tuple)):
        return [_json_safe(x, f"{where}[{i}]", depth + 1) for i, x in enumerate(v)]
    if isinstance(v, dict):
        out = {}
        for k, x in v.items():
            if not isinstance(k, str):
                raise ProjectError(f"{where}: keys must be strings")
            out[k] = _json_safe(x, f"{where}.{k}", depth + 1)
        return out
    raise ProjectError(f"{where}: unsupported value type {type(v).__name__}")


def normalize_asset_path(v: Any, where: str = "asset") -> str:
    """Return a safe project-relative posix asset path or raise ProjectError."""
    if not isinstance(v, str) or not v or len(v) > MAX_ASSET_PATH:
        raise ProjectError(f"{where}: non-empty relative path expected")
    if "\\" in v or ":" in v or v.startswith("/") or _CTRL_RE.search(v):
        raise ProjectError(f"{where}: unsafe asset path {v!r}")
    parts = v.split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise ProjectError(f"{where}: unsafe asset path {v!r}")
    if not v.lower().endswith(ASSET_EXTENSIONS):
        raise ProjectError(f"{where}: unsupported asset type (PNG/JPEG/PPM/BMP only)")
    return v


def _asset_file(root: Path, rel: str) -> Path:
    """Resolve rel under root; reject symlink/junction escapes."""
    root_r = root.resolve()
    full = (root_r / Path(*rel.split("/"))).resolve()
    try:
        full.relative_to(root_r)
    except ValueError:
        raise ProjectError(f"asset escapes project folder: {rel}") from None
    return full


# ---------------------------------------------------------------- components
def _sprite2d(d: Any, where: str) -> dict:
    if not isinstance(d, dict):
        raise ProjectError(f"{where}: object expected")
    out = _json_safe(d, where)
    out["width"] = _num(d.get("width", 32), f"{where}.width", 0.0, _SIZE_LIMIT)
    out["height"] = _num(d.get("height", 32), f"{where}.height", 0.0, _SIZE_LIMIT)
    if out["width"] <= 0 or out["height"] <= 0:
        raise ProjectError(f"{where}: width/height must be positive")
    out["color"] = _color(d.get("color", "#edbd52"), f"{where}.color")
    asset = d.get("asset")
    out["asset"] = None if asset is None else normalize_asset_path(asset, f"{where}.asset")
    out["opacity"] = _int(d.get("opacity", 255), f"{where}.opacity", 0, 255)
    out["layer"] = _int(d.get("layer", 0), f"{where}.layer", -_LAYER_LIMIT, _LAYER_LIMIT)
    return out


def _motion2d(d: Any, where: str) -> dict:
    if not isinstance(d, dict):
        raise ProjectError(f"{where}: object expected")
    out = _json_safe(d, where)
    controlled = d.get("controlled", False)
    if not isinstance(controlled, bool):
        raise ProjectError(f"{where}.controlled: boolean expected")
    out["controlled"] = controlled
    out["speed"] = _num(d.get("speed", 80), f"{where}.speed", 0.0, _SPEED_LIMIT)
    out["velocity"] = _vec(d.get("velocity", [0.0, 0.0]), 2, f"{where}.velocity",
                           -_SPEED_LIMIT, _SPEED_LIMIT)
    return out


def _bool(v: Any, where: str) -> bool:
    if not isinstance(v, bool):
        raise ProjectError(f"{where}: boolean expected")
    return v


def _collider2d(d: Any, where: str) -> dict:
    if not isinstance(d, dict):
        raise ProjectError(f"{where}: object expected")
    out = _json_safe(d, where)
    out["enabled"] = _bool(d.get("enabled", True), f"{where}.enabled")
    out["trigger"] = _bool(d.get("trigger", False), f"{where}.trigger")
    out["fit_sprite"] = _bool(d.get("fit_sprite", True), f"{where}.fit_sprite")
    out["offset"] = _vec(d.get("offset", [0.0, 0.0]), 2, f"{where}.offset",
                         -_SIZE_LIMIT, _SIZE_LIMIT)
    out["size"] = _vec(d.get("size", [32.0, 32.0]), 2, f"{where}.size", 0.001, _SIZE_LIMIT)
    return out


def _body2d(d: Any, where: str) -> dict:
    if not isinstance(d, dict):
        raise ProjectError(f"{where}: object expected")
    out = _json_safe(d, where)
    kind = d.get("type", "static")
    if kind not in ("static", "dynamic") or not isinstance(kind, str):
        raise ProjectError(f"{where}.type: 'static' or 'dynamic' expected")
    out["type"] = kind
    out["gravity"] = _num(d.get("gravity", 600.0), f"{where}.gravity", 0.0, _SPEED_LIMIT)
    out["jump_speed"] = _num(d.get("jump_speed", 220.0), f"{where}.jump_speed", 0.0, _SPEED_LIMIT)
    out["max_fall_speed"] = _num(d.get("max_fall_speed", 600.0), f"{where}.max_fall_speed",
                                 0.001, _SPEED_LIMIT)
    return out


_COMPONENT_VALIDATORS = {"sprite2d": _sprite2d, "motion2d": _motion2d,
                         "collider2d": _collider2d, "body2d": _body2d}

_COLLIDER_COORD_LIMIT = 1.0e8
_COLLIDER_MIN_EXTENT = 1.0e-4


def default_collider2d() -> dict:
    return {"enabled": True, "trigger": False, "fit_sprite": True,
            "offset": [0.0, 0.0], "size": [32.0, 32.0]}


def default_body2d() -> dict:
    return {"type": "static", "gravity": 600.0, "jump_speed": 220.0, "max_fall_speed": 600.0}


def collider_bounds(obj) -> Optional[tuple]:
    """World AABB (minx, miny, maxx, maxy) of the object's collider, shared by runtime/overlay.

    None when the collider is missing or disabled. The collider rectangle (the sprite
    rectangle when ``fit_sprite``, else offset/size in sprite-local space) is scaled
    then rotated clockwise (y down) about the object position; the result is the
    *enclosing AABB* of the four corners, not an oriented box. An enabled collider
    with invalid/degenerate geometry raises ProjectError (never a silent bypass).
    """
    comps = obj.components if isinstance(obj.components, dict) else {}
    col = comps.get("collider2d")
    if not isinstance(col, dict):
        return None
    enabled = col.get("enabled", True)
    if not isinstance(enabled, bool):
        raise ProjectError("collider2d.enabled: boolean expected")
    if not enabled:
        return None
    fit = col.get("fit_sprite", True)
    if not isinstance(fit, bool):
        raise ProjectError("collider2d.fit_sprite: boolean expected")
    if fit:
        sp = comps.get("sprite2d")
        if not isinstance(sp, dict):
            raise ProjectError("collider2d: fit_sprite needs a sprite2d component")
        ox = oy = 0.0
        w = _num(sp.get("width", 32), "sprite2d.width", 0.0, _SIZE_LIMIT)
        h = _num(sp.get("height", 32), "sprite2d.height", 0.0, _SIZE_LIMIT)
    else:
        ox, oy = _vec(col.get("offset", [0.0, 0.0]), 2, "collider2d.offset",
                      -_SIZE_LIMIT, _SIZE_LIMIT)
        w, h = _vec(col.get("size", [32.0, 32.0]), 2, "collider2d.size", 0.001, _SIZE_LIMIT)
    t = obj.transform
    px, py = _vec(list(t.position)[:2], 2, "transform.position", -_POS_LIMIT, _POS_LIMIT)
    sx, sy = _vec(list(t.scale)[:2], 2, "transform.scale", -_SCALE_LIMIT, _SCALE_LIMIT)
    rot = _num(list(t.rotation)[2], "transform.rotation", -_ROT_LIMIT, _ROT_LIMIT)
    c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    xs, ys = [], []
    for lx, ly in ((ox, oy), (ox + w, oy), (ox + w, oy + h), (ox, oy + h)):
        lx, ly = lx * sx, ly * sy
        xs.append(px + lx * c - ly * s)
        ys.append(py + lx * s + ly * c)
    box = (min(xs), min(ys), max(xs), max(ys))
    if not all(math.isfinite(v) and abs(v) <= _COLLIDER_COORD_LIMIT for v in box):
        raise ProjectError("collider2d: bounds out of range")
    if box[2] - box[0] < _COLLIDER_MIN_EXTENT or box[3] - box[1] < _COLLIDER_MIN_EXTENT:
        raise ProjectError("collider2d: bounds are degenerate (zero extent)")
    return box


# ---------------------------------------------------------------- dataclasses
def _vec3(default: float) -> list[float]:
    return [default, default, default]


@dataclass
class Transform:
    position: list = field(default_factory=lambda: _vec3(0.0))
    rotation: list = field(default_factory=lambda: _vec3(0.0))  # Euler degrees
    scale: list = field(default_factory=lambda: _vec3(1.0))

    def to_dict(self) -> dict:
        return {"position": list(self.position),
                "rotation": list(self.rotation),
                "scale": list(self.scale)}

    @staticmethod
    def from_dict(d: Any, where: str = "transform") -> "Transform":
        if not isinstance(d, dict):
            raise ProjectError(f"{where}: object expected")
        unknown = set(d) - {"position", "rotation", "scale"}
        if unknown:
            raise ProjectError(f"{where}: unknown keys {sorted(unknown)}")
        t = Transform(
            _vec(d.get("position", _vec3(0.0)), 3, f"{where}.position", -_POS_LIMIT, _POS_LIMIT),
            _vec(d.get("rotation", _vec3(0.0)), 3, f"{where}.rotation", -_ROT_LIMIT, _ROT_LIMIT),
            _vec(d.get("scale", _vec3(1.0)), 3, f"{where}.scale", -_SCALE_LIMIT, _SCALE_LIMIT),
        )
        for i, s in enumerate(t.scale):
            if s == 0.0:
                raise ProjectError(f"{where}.scale[{i}]: must not be zero")
        return t


def default_sprite2d() -> dict:
    return {"width": 32.0, "height": 32.0, "color": "#edbd52",
            "asset": None, "opacity": 255, "layer": 0}


@dataclass
class SceneObject:
    id: str
    name: str
    kind: str = "sprite2d"
    transform: Transform = field(default_factory=Transform)
    visible: bool = True
    components: dict = field(default_factory=dict)
    extra: dict = field(default_factory=dict)  # unknown top-level keys, preserved

    def to_dict(self) -> dict:
        out = copy.deepcopy(self.extra)
        out.update({"id": self.id, "name": self.name, "kind": self.kind,
                    "transform": self.transform.to_dict(), "visible": self.visible,
                    "components": copy.deepcopy(self.components)})
        return out

    @staticmethod
    def from_dict(d: Any, where: str = "object") -> "SceneObject":
        if not isinstance(d, dict):
            raise ProjectError(f"{where}: object expected")
        oid = d.get("id")
        if not isinstance(oid, str) or not _ID_RE.match(oid):
            raise ProjectError(f"{where}.id: 1-64 chars of A-Z a-z 0-9 _ . - expected")
        kind = _str(d.get("kind"), f"{where}.kind", 64, allow_empty=False)
        visible = d.get("visible", True)
        if not isinstance(visible, bool):
            raise ProjectError(f"{where}.visible: boolean expected")
        comps_in = d.get("components", {})
        if not isinstance(comps_in, dict):
            raise ProjectError(f"{where}.components: object expected")
        comps = {}
        for cname, cval in comps_in.items():
            _str(cname, f"{where}.components key", 64, allow_empty=False)
            check = _COMPONENT_VALIDATORS.get(cname)
            cwhere = f"{where}.components.{cname}"
            comps[cname] = check(cval, cwhere) if check else _json_safe(cval, cwhere)
        if kind == "sprite2d" and "sprite2d" not in comps:
            comps["sprite2d"] = default_sprite2d()
        known = {"id", "name", "kind", "transform", "visible", "components"}
        extra = {k: _json_safe(v, f"{where}.{k}") for k, v in d.items() if k not in known}
        result = SceneObject(
            id=oid, name=_str(d.get("name", oid), f"{where}.name"), kind=kind,
            transform=Transform.from_dict(d.get("transform", {}), f"{where}.transform"),
            visible=visible, components=comps, extra=extra)
        if kind == "sprite2d":
            sprite = comps["sprite2d"]
            for axis, size in enumerate((sprite["width"], sprite["height"])):
                extent = abs(size * result.transform.scale[axis])
                if not 0.0001 <= extent <= 1.0e6:
                    raise ProjectError(f"{where}: scaled sprite extent must be 0.0001..1000000")
        if "collider2d" in comps:
            try:
                collider_bounds(result)
            except ProjectError as e:
                raise ProjectError(f"{where}.components.collider2d: {e}") from None
        return result


@dataclass
class Project:
    name: str = "Untitled"
    width: int = 320
    height: int = 180
    background: str = "#17212c"
    objects: list = field(default_factory=list)
    path: Optional[Path] = None  # editor-only, never serialized
    extra: dict = field(default_factory=dict)  # unknown root keys, preserved

    # -- serialization
    def to_dict(self) -> dict:
        out = copy.deepcopy(self.extra)
        out.update({"format": FORMAT, "version": VERSION, "name": self.name,
                    "width": self.width, "height": self.height,
                    "background": self.background,
                    "objects": [o.to_dict() for o in self.objects]})
        return out

    @staticmethod
    def from_dict(data: Any) -> "Project":
        if not isinstance(data, dict):
            raise ProjectError("project root must be an object")
        if data.get("format") != FORMAT:
            raise ProjectError(f"not a {FORMAT} file")
        ver = data.get("version")
        if isinstance(ver, bool) or not isinstance(ver, int):
            raise ProjectError("version: integer expected")
        if ver != VERSION:
            raise ProjectError(f"unsupported project version {ver} (supported: {VERSION})")
        width = _int(data.get("width", 320), "width", 1, MAX_DIMENSION)
        height = _int(data.get("height", 180), "height", 1, MAX_DIMENSION)
        if width * height > MAX_PIXELS:
            raise ProjectError(f"resolution exceeds {MAX_PIXELS} pixels")
        raw = data.get("objects", [])
        if not isinstance(raw, list):
            raise ProjectError("objects: list expected")
        if len(raw) > MAX_OBJECTS:
            raise ProjectError(f"too many objects (max {MAX_OBJECTS})")
        objects, seen = [], set()
        for i, od in enumerate(raw):
            obj = SceneObject.from_dict(od, f"objects[{i}]")
            if obj.id in seen:
                raise ProjectError(f"objects[{i}].id: duplicate id {obj.id!r}")
            seen.add(obj.id)
            objects.append(obj)
        known = {"format", "version", "name", "width", "height", "background", "objects"}
        return Project(
            name=_str(data.get("name", "Untitled"), "name"), width=width, height=height,
            background=_color(data.get("background", "#17212c"), "background"),
            objects=objects, path=None,
            extra={k: _json_safe(v, k) for k, v in data.items() if k not in known})

    def validate(self) -> "Project":
        """Return a normalized, validated copy (path kept); raises ProjectError."""
        p = Project.from_dict(self.to_dict())
        p.path = self.path
        return p

    def clone(self) -> "Project":
        return copy.deepcopy(self)

    # -- scene editing
    def object_by_id(self, oid: str) -> Optional[SceneObject]:
        for o in self.objects:
            if o.id == oid:
                return o
        return None

    def _new_id(self) -> str:
        used = {o.id for o in self.objects}
        n = 1
        while f"obj-{n}" in used:
            n += 1
        return f"obj-{n}"

    def _check_room(self) -> None:
        if len(self.objects) >= MAX_OBJECTS:
            raise ProjectError(f"too many objects (max {MAX_OBJECTS})")

    def add_object(self, name: str = "Sprite", kind: str = "sprite2d") -> SceneObject:
        self._check_room()
        obj = SceneObject(id=self._new_id(), name=name, kind=kind)
        if kind == "sprite2d":
            obj.components["sprite2d"] = default_sprite2d()
            sprite = obj.components["sprite2d"]
            obj.transform.position = [(self.width - sprite["width"]) / 2.0,
                                      (self.height - sprite["height"]) / 2.0, 0.0]
        else:
            obj.transform.position = [self.width / 2.0, self.height / 2.0, 0.0]
        self.objects.append(obj)
        return obj

    def remove_object(self, oid: str) -> bool:
        for i, o in enumerate(self.objects):
            if o.id == oid:
                del self.objects[i]
                return True
        return False

    def duplicate_object(self, oid: str) -> SceneObject:
        src = self.object_by_id(oid)
        if src is None:
            raise ProjectError(f"no object with id {oid!r}")
        self._check_room()
        dup = copy.deepcopy(src)
        dup.id = self._new_id()
        dup.name = (src.name + " copy")[:MAX_NAME]
        dup.transform.position[0] += 8.0
        dup.transform.position[1] += 8.0
        self.objects.insert(self.objects.index(src) + 1, dup)
        return dup


# ---------------------------------------------------------------- sample
def _rect(oid, name, x, y, w, h, color, layer, rot_z=0.0, motion=None,
          collider=None, body=None) -> SceneObject:
    comps = {"sprite2d": {"width": float(w), "height": float(h), "color": color,
                          "asset": None, "opacity": 255, "layer": layer}}
    if motion is not None:
        comps["motion2d"] = motion
    if collider is not None:
        comps["collider2d"] = dict(default_collider2d(), **collider)
    if body is not None:
        comps["body2d"] = dict(default_body2d(), **body)
    return SceneObject(
        id=oid, name=name, kind="sprite2d",
        transform=Transform([float(x), float(y), 0.0], [0.0, 0.0, float(rot_z)], [1.0, 1.0, 1.0]),
        visible=True, components=comps)


def new_project() -> Project:
    """Built-in SAMPLE scene made of colored rectangles (no external assets)."""
    p = Project(name="Pixelica Sample Scene", width=320, height=180, background="#17212c")
    p.objects = [
        _rect("sample-moon", "Moon", 262, 22, 18, 18, "#f3e9c6", -8),
        _rect("sample-hill-far", "Hill (far)", 70, 96, 80, 80, "#223548", -6, 45),
        _rect("sample-hill-near", "Hill (near)", 205, 110, 70, 70, "#1b2b3b", -5, 45),
        _rect("sample-ground", "Ground", 0, 148, 320, 32, "#2f5d50", 0, collider={}, body={}),
        _rect("sample-grass", "Grass", 0, 148, 320, 4, "#4f9a6b", 1),
        _rect("sample-platform-1", "Platform 1", 40, 112, 70, 8, "#8a6f4d", 1,
              collider={}, body={}),
        # Reachable from platform 1 with one jump (gap 40px, rise 24px, jump apex ~40px).
        _rect("sample-platform-2", "Platform 2", 150, 88, 80, 8, "#8a6f4d", 1,
              collider={}, body={}),
        _rect("sample-coin-1", "Coin 1", 66, 100, 8, 8, "#edbd52", 2,
              collider={"trigger": True}),
        _rect("sample-coin-2", "Coin 2", 182, 76, 8, 8, "#edbd52", 2,
              collider={"trigger": True}),
        _rect("sample-coin-3", "Coin 3", 200, 76, 8, 8, "#edbd52", 2,
              collider={"trigger": True}),
        # Bottom edge (y 124 + 24) rests exactly on the ground top (y 148).
        _rect("sample-player", "Player", 152, 124, 16, 24, "#5fd0c4", 5,
              motion={"controlled": True, "speed": 80.0, "velocity": [0.0, 0.0]},
              collider={}, body={"type": "dynamic"}),
    ]
    return p


# ---------------------------------------------------------------- file I/O
def _atomic_write(dest: Path, data: bytes) -> None:
    """Write data to dest via a temp file in the same dir + os.replace."""
    fd, tmp = tempfile.mkstemp(dir=str(dest.parent), prefix="." + dest.name + ".",
                               suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dest)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _reject_const(name: str):
    raise ProjectError(f"non-finite number {name} not allowed")


def _no_dup_pairs(pairs):
    d = {}
    for k, v in pairs:
        if k in d:
            raise ProjectError(f"duplicate JSON key {k!r}")
        d[k] = v
    return d


def _check_asset_files(project: Project, root: Path) -> None:
    for o in project.objects:
        asset = (o.components.get("sprite2d") or {}).get("asset")
        if isinstance(asset, str):
            _asset_file(root, asset)  # raises on escape; missing files are tolerated


def load_project(path) -> Project:
    """Load and validate a project file. Raises ProjectError/OSError; never partial."""
    path = Path(os.path.abspath(path))
    try:
        with open(path, "rb") as f:
            raw = f.read(MAX_JSON_BYTES + 1)
    except IsADirectoryError:
        raise ProjectError(f"not a file: {path}") from None
    if len(raw) > MAX_JSON_BYTES:
        raise ProjectError(f"project file larger than {MAX_JSON_BYTES} bytes")
    try:
        data = json.loads(raw.decode("utf-8"), parse_constant=_reject_const,
                          object_pairs_hook=_no_dup_pairs)
    except (UnicodeDecodeError, ValueError, RecursionError) as e:
        if isinstance(e, ProjectError):
            raise
        raise ProjectError(f"invalid project JSON: {e}") from None
    project = Project.from_dict(data)
    _check_asset_files(project, path.parent)
    project.path = path
    return project


def save_project(project: Project, path) -> None:
    """Atomically save; Save As (different folder) copies referenced assets first.

    project.path is updated only after the JSON commit succeeded. On failure the
    previous file is untouched and newly copied assets are removed again.
    """
    path = Path(os.path.abspath(path))
    if path.is_dir():
        raise ProjectError(f"target is a directory: {path}")
    norm = project.validate()
    payload = (json.dumps(norm.to_dict(), indent=2, ensure_ascii=False, allow_nan=False)
               + "\n").encode("utf-8")
    if len(payload) > MAX_JSON_BYTES:
        raise ProjectError("project too large to save")
    path.parent.mkdir(parents=True, exist_ok=True)
    new_root = path.parent
    _check_asset_files(norm, new_root)
    created: list[Path] = []
    try:
        if project.path is not None:
            old_root = Path(project.path).parent
            if old_root.resolve() != new_root.resolve():
                _copy_assets(norm, old_root, new_root, created)
        _atomic_write(path, payload)
    except BaseException:
        for c in created:
            try:
                c.unlink()
            except OSError:
                pass
        raise
    project.path = path


def _copy_assets(project: Project, old_root: Path, new_root: Path, created: list) -> None:
    done = set()
    for o in project.objects:
        rel = (o.components.get("sprite2d") or {}).get("asset")
        if not isinstance(rel, str) or rel in done:
            continue
        done.add(rel)
        src = _asset_file(old_root, rel)
        if not src.is_file():
            continue  # already-missing asset: keep the reference, nothing to copy
        if src.stat().st_size > MAX_ASSET_BYTES:
            raise ProjectError(f"asset too large: {rel}")
        data = src.read_bytes()
        dst = _asset_file(new_root, rel)
        if dst.exists():
            if dst.is_file() and dst.read_bytes() == data:
                continue
            raise ProjectError(f"asset already exists with different content: {rel}")
        dst.parent.mkdir(parents=True, exist_ok=True)
        _atomic_write(dst, data)
        created.append(dst)


def _magic_ok(ext: str, head: bytes) -> bool:
    if ext == ".png":
        return head.startswith(b"\x89PNG\r\n\x1a\n")
    if ext in (".jpg", ".jpeg"):
        return head.startswith(b"\xff\xd8\xff")
    if ext == ".bmp":
        return head.startswith(b"BM")
    if ext == ".ppm":
        return head[:2] in (b"P3", b"P6")
    return False


def import_asset(project: Project, source) -> str:
    """Copy an image into <project folder>/assets/<sha256-prefix><ext>; return rel path."""
    if project.path is None:
        raise ProjectError("save the project before importing assets")
    src = Path(source)
    ext = src.suffix.lower()
    if ext not in ASSET_EXTENSIONS:
        raise ProjectError("unsupported asset type (PNG/JPEG/PPM/BMP only)")
    if not src.is_file():
        raise ProjectError(f"asset not found: {src}")
    with open(src, "rb") as f:
        data = f.read(MAX_ASSET_BYTES + 1)
    if len(data) > MAX_ASSET_BYTES:
        raise ProjectError("asset too large")
    if not _magic_ok(ext, data[:16]):
        raise ProjectError(f"file content does not look like {ext[1:].upper()}")
    rel = f"{ASSETS_DIR}/{hashlib.sha256(data).hexdigest()[:24]}{ext}"
    root = Path(project.path).parent
    dst = _asset_file(root, rel)
    if dst.exists():
        if dst.is_file() and dst.read_bytes() == data:
            return rel
        raise ProjectError(f"asset name collision: {rel}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.parent.resolve() != _asset_file(root, ASSETS_DIR):
        raise ProjectError("assets folder escapes project folder")
    _atomic_write(dst, data)
    return rel
