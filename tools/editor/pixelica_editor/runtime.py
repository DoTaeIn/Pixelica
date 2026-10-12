"""Native Pixelica2D preview renderer and fixed-step play session.

``Renderer2D`` draws a project through the real Pixelica2D software renderer,
loaded from the ``pixelica_editor_bridge`` shared library with ctypes (see
tools/editor/native/bridge.h). There is deliberately no pure-Python fallback:
if the library is missing, construction fails with ``NativeLibraryError``.

``PlaySession`` runs a deep copy of a project at a fixed time step. It only
moves objects with a ``motion2d`` component; it never executes project code.

Limitations (v0.1):
  * Only ``sprite2d`` objects are drawn; other visible kinds are refused.
  * 2D only: position.z, rotation.x/y and scale.z are kept in the model but
    ignored here.
  * Texture change detection uses file mtime/size; an edit that preserves both
    is not noticed until the cache entry is evicted or the renderer recreated.
  * Output is quantized to RGB565 by the engine, then expanded to RGB24.
"""

from __future__ import annotations

import ctypes
import hashlib
import io
import math
import os
import re
import sys
import warnings
from collections import OrderedDict
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import TYPE_CHECKING, Iterable

from PIL import Image, ImageChops

if TYPE_CHECKING:  # pragma: no cover - typing only
    from .model import Project

__all__ = [
    "ABI_VERSION",
    "AssetError",
    "FIXED_DT",
    "NativeLibraryError",
    "PlaySession",
    "Renderer2D",
    "RendererError",
    "find_library",
    "library_filename",
]

ABI_VERSION = 2
FIXED_DT = 1.0 / 60.0

MAX_DIMENSION = 2048
MAX_PIXELS = 1048576
MAX_TEXTURE_DIMENSION = 4096
MAX_ASSET_FILE_BYTES = 32 * 1024 * 1024
# Decoded RGBA bytes allowed for one image (checked before decoding).
MAX_DECODED_BYTES = 64 * 1024 * 1024
# Texture cache bounds (native bytes = w*h*3, as accounted by the bridge).
TEXTURE_CACHE_ENTRIES = 64
TEXTURE_CACHE_BYTES = 64 * 1024 * 1024
MAX_COORD = 1.0e6
MAX_EXTENT = 1.0e6
ASSET_SUFFIXES = {".png", ".jpg", ".jpeg", ".ppm", ".bmp"}
ASSET_FORMATS = {"PNG", "JPEG", "PPM", "BMP"}

_REPO_ROOT = Path(__file__).resolve().parents[3]
_NATIVE_DIR = Path(__file__).resolve().parents[1] / "native"
_HEX_COLOR = re.compile(r"^#[0-9a-fA-F]{6}$")

_PE_STATUS_TEXT = {
    1: "invalid argument",
    2: "out of memory",
    3: "texture capacity exhausted",
    4: "no frame started",
    5: "engine rejected the call",
}


class RendererError(RuntimeError):
    """A frame could not be rendered (bad data or native failure)."""


class NativeLibraryError(RendererError):
    """The pixelica_editor_bridge shared library is missing or unusable."""


class AssetError(RendererError):
    """An image asset is missing, unsafe, too large or not decodable."""


# ---------------------------------------------------------------------------
# Library discovery and loading
# ---------------------------------------------------------------------------

def library_filename() -> str:
    if sys.platform == "win32":
        return "pixelica_editor_bridge.dll"
    if sys.platform == "darwin":
        return "pixelica_editor_bridge.dylib"
    return "pixelica_editor_bridge.so"


def library_candidates(library_path: Path | None = None) -> list[Path]:
    """Search order: explicit path, $PIXELICA_EDITOR_LIBRARY, build-editor/Release,
    build-editor, tools/editor/native (bundled)."""
    if library_path is not None:
        return [Path(library_path)]
    env = os.environ.get("PIXELICA_EDITOR_LIBRARY")
    if env:
        return [Path(env)]
    name = library_filename()
    build = _REPO_ROOT / "build-editor"
    return [build / "Release" / name, build / name, _NATIVE_DIR / name]


def find_library(library_path: Path | None = None) -> Path:
    candidates = library_candidates(library_path)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    tried = ", ".join(str(c) for c in candidates)
    raise NativeLibraryError(
        f"Pixelica editor bridge not found (tried: {tried}). Build it with "
        "'python tools/editor/launch_editor.py --build' or "
        "'cmake -S tools/editor/native -B build-editor && cmake --build build-editor --config Release'."
    )


class _Vec2(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


class _AABB2D(ctypes.Structure):  # PIXELICA_AABB2D
    _fields_ = [("min", _Vec2), ("max", _Vec2)]


class _Body2D(ctypes.Structure):  # PIXELICA_Body2D, 32 bytes
    _fields_ = [("bounds", _AABB2D), ("velocity", _Vec2),
                ("contacts", ctypes.c_uint32), ("grounded", ctypes.c_uint32)]


_LOADED: dict[str, ctypes.CDLL] = {}


def _load(path: Path) -> ctypes.CDLL:
    key = str(path)
    lib = _LOADED.get(key)
    if lib is not None:
        return lib
    try:
        lib = ctypes.CDLL(key)
    except OSError as exc:
        raise NativeLibraryError(f"cannot load {path}: {exc}") from exc
    try:
        c_double, c_int, c_uint32 = ctypes.c_double, ctypes.c_int, ctypes.c_uint32
        lib.pe_abi_version.argtypes = []
        lib.pe_abi_version.restype = c_int
        lib.pe_status_message.argtypes = [c_int]
        lib.pe_status_message.restype = ctypes.c_char_p
        lib.pe_create.argtypes = [c_int, c_int, ctypes.POINTER(ctypes.c_void_p)]
        lib.pe_create.restype = c_int
        lib.pe_destroy.argtypes = [ctypes.c_void_p]
        lib.pe_destroy.restype = None
        lib.pe_begin.argtypes = [ctypes.c_void_p, c_uint32]
        lib.pe_begin.restype = c_int
        lib.pe_draw_rect.argtypes = [ctypes.c_void_p, c_double, c_double, c_double, c_double,
                                     c_double, c_uint32, c_int]
        lib.pe_draw_rect.restype = c_int
        lib.pe_texture_create.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                                          c_int, c_int, ctypes.POINTER(c_uint32)]
        lib.pe_texture_create.restype = c_int
        lib.pe_texture_destroy.argtypes = [ctypes.c_void_p, c_uint32]
        lib.pe_texture_destroy.restype = c_int
        lib.pe_texture_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(c_uint32),
                                         ctypes.POINTER(ctypes.c_size_t)]
        lib.pe_texture_stats.restype = c_int
        lib.pe_draw_texture.argtypes = [ctypes.c_void_p, c_uint32, c_double, c_double, c_double,
                                        c_double, c_double, c_int]
        lib.pe_draw_texture.restype = c_int
        lib.pe_read_rgb.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        lib.pe_read_rgb.restype = c_int
        lib.pe_body2d_step.argtypes = [ctypes.POINTER(_Body2D), c_double, c_double, c_double, c_double,
                                       c_int, ctypes.POINTER(_AABB2D), ctypes.c_size_t]
        lib.pe_body2d_step.restype = c_int
        lib.pe_aabb2d_overlaps.argtypes = [ctypes.POINTER(_AABB2D), ctypes.POINTER(_AABB2D)]
        lib.pe_aabb2d_overlaps.restype = c_int
    except AttributeError as exc:
        raise NativeLibraryError(f"{path} is not a Pixelica editor bridge: {exc}") from exc
    version = lib.pe_abi_version()
    if version != ABI_VERSION:
        raise NativeLibraryError(f"{path} has bridge ABI {version}, expected {ABI_VERSION}; rebuild it")
    _LOADED[key] = lib
    return lib


# ---------------------------------------------------------------------------
# Value validation helpers
# ---------------------------------------------------------------------------

def _number(value: object, what: str, limit: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise RendererError(f"{what} must be a number")
    try:
        result = float(value)
    except OverflowError as exc:
        raise RendererError(f"{what} must be finite and within +/-{limit:g}") from exc
    if not math.isfinite(result) or abs(result) > limit:
        raise RendererError(f"{what} must be finite and within +/-{limit:g}")
    return result


def _integer(value: object, what: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise RendererError(f"{what} must be an integer in {low}..{high}")
    return value


def _color(value: object, what: str) -> int:
    if not isinstance(value, str) or not _HEX_COLOR.match(value):
        raise RendererError(f"{what} must be '#rrggbb'")
    return int(value[1:], 16)


def _vector(value: object, what: str, needed: int) -> list:
    if not isinstance(value, (list, tuple)) or len(value) < needed:
        raise RendererError(f"{what} needs at least {needed} components")
    return list(value)


def _check_dimensions(width: object, height: object) -> tuple[int, int]:
    w = _integer(width, "width", 1, MAX_DIMENSION)
    h = _integer(height, "height", 1, MAX_DIMENSION)
    if w * h > MAX_PIXELS:
        raise RendererError(f"{w}x{h} exceeds {MAX_PIXELS} pixels")
    return w, h


# ---------------------------------------------------------------------------
# Renderer2D
# ---------------------------------------------------------------------------

class _Texture:
    __slots__ = ("id", "bytes", "signature", "digest")

    def __init__(self, tex_id: int, nbytes: int, signature: tuple, digest: str) -> None:
        self.id = tex_id
        self.bytes = nbytes
        self.signature = signature
        self.digest = digest


class Renderer2D:
    """Renders a Project to an RGB ``PIL.Image`` through Pixelica2D.

    Objects are drawn in stable order (sprite layer ascending, then scene
    index). position.xy is the top-left pivot, rotation.z is degrees
    clockwise on screen, width/height are multiplied by scale.xy (negative
    scale mirrors). Images are tinted by ``color`` (channel multiply) and
    blended with their alpha times ``opacity``.
    """

    def __init__(self, width: int, height: int, library_path: Path | None = None) -> None:
        self._bridge: ctypes.c_void_p | None = None
        self._textures: "OrderedDict[tuple[str, int], _Texture]" = OrderedDict()
        self._texture_bytes = 0
        self.width, self.height = _check_dimensions(width, height)
        self.library_path: Path = find_library(library_path)
        self._lib = _load(self.library_path)
        self._bridge = self._create(self.width, self.height)

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Release native memory. Safe to call more than once."""
        bridge, self._bridge = self._bridge, None
        self._textures.clear()
        self._texture_bytes = 0
        if bridge is not None:
            self._lib.pe_destroy(bridge)

    def __enter__(self) -> "Renderer2D":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:  # best effort; close() is the real API
        try:
            self.close()
        except Exception:
            pass

    @property
    def texture_count(self) -> int:
        return len(self._textures)

    def _create(self, width: int, height: int) -> ctypes.c_void_p:
        handle = ctypes.c_void_p()
        self._check(self._lib.pe_create(width, height, ctypes.byref(handle)), "create render target")
        if not handle.value:
            raise NativeLibraryError("pe_create returned no target")
        return handle

    def _check(self, status: int, what: str) -> None:
        if status == 0:
            return
        try:
            text = self._lib.pe_status_message(status).decode("ascii", "replace")
        except Exception:
            text = _PE_STATUS_TEXT.get(status, "unknown error")
        raise RendererError(f"Pixelica2D could not {what}: {text} (status {status})")

    def _resize(self, width: int, height: int) -> None:
        if (width, height) == (self.width, self.height) and self._bridge is not None:
            return
        new_bridge = self._create(width, height)
        self.close()
        self._bridge = new_bridge
        self.width, self.height = width, height

    # -- rendering ---------------------------------------------------------
    def render(self, project: "Project") -> Image.Image:
        if self._bridge is None:
            raise RendererError("renderer is closed")
        width, height = _check_dimensions(project.width, project.height)
        background = _color(project.background, "project background")
        draws = self._collect(project)
        self._resize(width, height)
        bridge = self._bridge
        self._check(self._lib.pe_begin(bridge, background), "clear the frame")
        for name, x, y, w, h, rot, rgb, opacity, asset in draws:
            if asset is None:
                status = self._lib.pe_draw_rect(bridge, x, y, w, h, rot, rgb, opacity)
            else:
                tex = self._texture(project, asset, rgb)
                status = self._lib.pe_draw_texture(bridge, tex.id, x, y, w, h, rot, opacity)
            self._check(status, f"draw '{name}'")
        size = width * height * 3
        buffer = ctypes.create_string_buffer(size)
        self._check(self._lib.pe_read_rgb(bridge, buffer, size), "read the frame")
        return Image.frombytes("RGB", (width, height), buffer.raw[:size])

    def _collect(self, project: "Project") -> list[tuple]:
        """Validate every visible object before touching the native target."""
        items = []
        for index, obj in enumerate(project.objects):
            if not obj.visible:
                continue
            label = getattr(obj, "name", None) or getattr(obj, "id", "?")
            if obj.kind != "sprite2d":
                raise RendererError(f"Renderer2D cannot draw '{label}': unsupported kind '{obj.kind}'")
            sprite = obj.components.get("sprite2d") if isinstance(obj.components, dict) else None
            if not isinstance(sprite, dict):
                raise RendererError(f"'{label}' has no sprite2d component")
            t = obj.transform
            pos = _vector(t.position, f"'{label}' position", 2)
            rot = _vector(t.rotation, f"'{label}' rotation", 3)
            scale = _vector(t.scale, f"'{label}' scale", 2)
            x = _number(pos[0], f"'{label}' x", MAX_COORD)
            y = _number(pos[1], f"'{label}' y", MAX_COORD)
            sx = _number(scale[0], f"'{label}' scale.x", MAX_EXTENT)
            sy = _number(scale[1], f"'{label}' scale.y", MAX_EXTENT)
            sw = _number(sprite.get("width", 32.0), f"'{label}' width", MAX_EXTENT)
            sh = _number(sprite.get("height", 32.0), f"'{label}' height", MAX_EXTENT)
            if sw <= 0 or sh <= 0:
                raise RendererError(f"'{label}' width/height must be positive")
            w, h = sw * sx, sh * sy
            if w == 0 or h == 0 or abs(w) > MAX_EXTENT or abs(h) > MAX_EXTENT:
                raise RendererError(f"'{label}' scaled size must be non-zero and within {MAX_EXTENT:g}")
            degrees = _number(rot[2], f"'{label}' rotation.z", 1.0e9)
            radians = math.radians(math.fmod(degrees, 360.0))
            rgb = _color(sprite.get("color", "#edbd52"), f"'{label}' color")
            opacity = _integer(sprite.get("opacity", 255), f"'{label}' opacity", 0, 255)
            layer = _integer(sprite.get("layer", 0), f"'{label}' layer", -(2**31), 2**31 - 1)
            asset = sprite.get("asset")
            if asset is not None and not isinstance(asset, str):
                raise AssetError(f"'{label}' asset must be a relative path string")
            items.append((layer, index, (label, x, y, w, h, radians, rgb, opacity, asset or None)))
        items.sort(key=lambda item: (item[0], item[1]))
        return [item[2] for item in items]

    # -- textures ----------------------------------------------------------
    def _texture(self, project: "Project", asset: str, tint: int) -> _Texture:
        path = resolve_asset(project, asset)
        try:
            st = path.stat()
        except OSError as exc:
            raise AssetError(f"cannot read asset '{asset}': {exc}") from exc
        signature = (st.st_mtime_ns, st.st_size)
        key = (str(path), tint)
        cached = self._textures.get(key)
        if cached is not None and cached.signature == signature:
            self._textures.move_to_end(key)
            return cached
        data = _read_asset(path, asset, st.st_size)
        digest = hashlib.sha256(data).hexdigest()
        if cached is not None and cached.digest == digest:
            cached.signature = signature
            self._textures.move_to_end(key)
            return cached
        rgba, w, h = _decode_rgba(data, asset, tint)
        nbytes = w * h * 3
        if cached is not None:
            self._drop(key)
        self._evict_for(nbytes)
        tex_id = ctypes.c_uint32(0)
        self._check(self._lib.pe_texture_create(self._bridge, rgba, len(rgba), w, h, ctypes.byref(tex_id)),
                    f"upload asset '{asset}'")
        entry = _Texture(tex_id.value, nbytes, signature, digest)
        self._textures[key] = entry
        self._texture_bytes += nbytes
        return entry

    def _drop(self, key: tuple[str, int]) -> None:
        entry = self._textures.pop(key)
        self._texture_bytes -= entry.bytes
        if self._bridge is not None:
            self._lib.pe_texture_destroy(self._bridge, entry.id)

    def _evict_for(self, nbytes: int) -> None:
        if nbytes > TEXTURE_CACHE_BYTES:
            raise AssetError("image too large for the texture cache")
        while self._textures and (len(self._textures) >= TEXTURE_CACHE_ENTRIES
                                  or self._texture_bytes + nbytes > TEXTURE_CACHE_BYTES):
            self._drop(next(iter(self._textures)))


def resolve_asset(project: "Project", asset: str) -> Path:
    """Resolve a project-relative asset path, refusing escapes and symlinks."""
    if not asset or "\x00" in asset:
        raise AssetError("asset path is empty or invalid")
    posix, windows = PurePosixPath(asset), PureWindowsPath(asset)
    if posix.is_absolute() or windows.is_absolute() or windows.drive or windows.root:
        raise AssetError(f"asset '{asset}' must be a relative path")
    parts = windows.parts  # splits on both '/' and '\\'
    if any(part in ("..", ".") for part in parts) or not parts:
        raise AssetError(f"asset '{asset}' must not contain '.' or '..'")
    if Path(asset).suffix.lower() not in ASSET_SUFFIXES:
        raise AssetError(f"asset '{asset}' must be PNG, JPEG, PPM or BMP")
    project_path = getattr(project, "path", None)
    if project_path is None:
        raise AssetError(f"asset '{asset}' needs a saved project to resolve against")
    base = Path(project_path).parent.resolve()
    current = base
    for part in parts:
        current = current / part
        if current.is_symlink():
            raise AssetError(f"asset '{asset}' goes through a symbolic link")
    try:
        resolved = current.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise AssetError(f"asset '{asset}' not found under {base}") from exc
    try:
        resolved.relative_to(base)
    except ValueError as exc:
        raise AssetError(f"asset '{asset}' resolves outside the project folder") from exc
    if not resolved.is_file():
        raise AssetError(f"asset '{asset}' is not a regular file")
    return resolved


def _read_asset(path: Path, asset: str, size: int) -> bytes:
    if size > MAX_ASSET_FILE_BYTES:
        raise AssetError(f"asset '{asset}' is larger than {MAX_ASSET_FILE_BYTES} bytes")
    try:
        with open(path, "rb") as handle:
            data = handle.read(MAX_ASSET_FILE_BYTES + 1)
    except OSError as exc:
        raise AssetError(f"cannot read asset '{asset}': {exc}") from exc
    if len(data) > MAX_ASSET_FILE_BYTES:
        raise AssetError(f"asset '{asset}' is larger than {MAX_ASSET_FILE_BYTES} bytes")
    return data


def _decode_rgba(data: bytes, asset: str, tint: int) -> tuple[bytes, int, int]:
    """Decode with Pillow (dimensions checked before pixel decoding), apply the
    RGB tint by channel multiplication and return straight-alpha RGBA bytes."""
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data)) as img:
                if img.format not in ASSET_FORMATS:
                    raise AssetError(f"asset '{asset}' has unsupported format {img.format}")
                w, h = img.size
                if not (1 <= w <= MAX_TEXTURE_DIMENSION and 1 <= h <= MAX_TEXTURE_DIMENSION):
                    raise AssetError(f"asset '{asset}' is {w}x{h}; limit is {MAX_TEXTURE_DIMENSION} per side")
                if w * h * 4 > MAX_DECODED_BYTES:
                    raise AssetError(f"asset '{asset}' is too large to decode ({w}x{h})")
                rgba = img.convert("RGBA")
    except AssetError:
        raise
    except (OSError, ValueError, SyntaxError, Image.DecompressionBombError,
            Image.DecompressionBombWarning, MemoryError) as exc:
        raise AssetError(f"cannot decode asset '{asset}': {exc}") from exc
    if tint != 0xFFFFFF:
        r, g, b, a = rgba.split()
        rgb = Image.merge("RGB", (r, g, b))
        color = ((tint >> 16) & 0xFF, (tint >> 8) & 0xFF, tint & 0xFF)
        rgb = ImageChops.multiply(rgb, Image.new("RGB", rgba.size, color))
        rgba = Image.merge("RGBA", (*rgb.split(), a))
    raw = rgba.tobytes()
    if len(raw) != w * h * 4:
        raise AssetError(f"asset '{asset}' decoded to an unexpected size")
    return raw, w, h


# ---------------------------------------------------------------------------
# PlaySession
# ---------------------------------------------------------------------------

_KEY_DIRECTIONS = {
    "w": (0.0, -1.0), "up": (0.0, -1.0),
    "s": (0.0, 1.0), "down": (0.0, 1.0),
    "a": (-1.0, 0.0), "left": (-1.0, 0.0),
    "d": (1.0, 0.0), "right": (1.0, 0.0),
}
MAX_STEP = 0.25
MAX_CATCH_UP_STEPS = 5


def _finite_or(value: object, default: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    try:
        result = float(value)
    except OverflowError:
        return default
    return result if math.isfinite(result) else default


class PlaySession:
    """Fixed-step simulation over a deep copy of a project.

    ``step(dt, keys)`` advances one tick: every ``motion2d`` object moves by
    its velocity, and ``controlled`` ones also move by ``speed`` in the WASD /
    arrow direction (diagonals normalized). Key names are case-insensitive
    ('w', 'Up', 'Left', ...). Positions are clamped to +/-1e6 so long runs stay
    renderable. The source project is never mutated.

    Collision (native, via pe_body2d_step / pe_aabb2d_overlaps; the bridge is
    loaded only when an enabled ``collider2d`` exists): enabled solid static
    colliders block and never move; enabled ``body2d`` type ``dynamic`` bodies
    fall and collide with static solids (dynamic/dynamic is ignored); jump is
    a rising edge of Space/W/Up. Trigger overlaps are sampled at tick end and
    published as ``trigger_events`` (phase, trigger_id, other_id). A tick is
    transactional: any failure raises RendererError and publishes nothing.
    """

    def __init__(self, project: "Project", library_path: Path | None = None) -> None:
        self.project = project.clone()
        self.ticks = 0
        self._accumulator = 0.0
        self._library_path = library_path
        self._lib: ctypes.CDLL | None = None  # loaded lazily, only when collision is used
        self._bodies: dict[str, tuple] = {}  # id -> (state key, _Body2D) kept between ticks
        self._velocity: dict[str, tuple[float, float]] = {}
        self._jump_held = False
        self.grounded: set[str] = set()
        self.active_triggers: set[tuple[str, str]] = set()
        self.trigger_events: list[tuple[str, str, str]] = []

    # -- collision helpers -------------------------------------------------
    def _native(self) -> ctypes.CDLL:
        if self._lib is None:
            self._lib = _load(find_library(self._library_path))
        return self._lib

    def _check(self, status: int, what: str) -> None:
        if status == 0:
            return
        try:
            text = self._native().pe_status_message(status).decode("ascii", "replace")
        except Exception:
            text = _PE_STATUS_TEXT.get(status, "unknown error")
        raise RendererError(f"Pixelica2D could not {what}: {text} (status {status})")

    @staticmethod
    def _collider_info(obj) -> dict | None:
        """Enabled collider description or None (legacy free motion applies)."""
        from . import model as _model

        comps = obj.components if isinstance(obj.components, dict) else {}
        collider = comps.get("collider2d")
        if not isinstance(collider, dict) or collider.get("enabled", True) is False:
            return None
        label = getattr(obj, "id", "?")
        try:
            bounds = _model.collider_bounds(obj)
        except (ValueError, TypeError, KeyError, IndexError, OverflowError) as exc:
            raise RendererError(f"collider of '{label}' is invalid: {exc}") from exc
        if bounds is None:
            return None
        if len(bounds) != 4:
            raise RendererError(f"collider of '{label}' returned malformed bounds")
        values = [_number(v, f"collider of '{label}' bounds", _model._COLLIDER_COORD_LIMIT) for v in bounds]
        if not (values[0] < values[2] and values[1] < values[3]):
            raise RendererError(f"collider of '{label}' has no area")
        body = comps.get("body2d")
        dynamic = isinstance(body, dict) and body.get("type", "static") == "dynamic"
        info = {"bounds": values, "trigger": collider.get("trigger", False) is True, "dynamic": dynamic}
        if dynamic:
            def param(name: str, default: float, low: float) -> float:
                v = _number(body.get(name, default), f"'{label}' body2d.{name}", 10000.0)
                if v < low:
                    raise RendererError(f"'{label}' body2d.{name} must be >= {low:g}")
                return v
            info["gravity"] = param("gravity", 600.0, 0.0)
            info["jump_speed"] = param("jump_speed", 220.0, 0.0)
            info["max_fall_speed"] = param("max_fall_speed", 600.0, 0.001)
        return info

    @staticmethod
    def _aabb(values: list[float], label: str) -> _AABB2D:
        box = _AABB2D(_Vec2(values[0], values[1]), _Vec2(values[2], values[3]))
        if not (box.min.x < box.max.x and box.min.y < box.max.y):
            raise RendererError(f"collider of '{label}' collapses to zero size at this position")
        return box

    @staticmethod
    def _axis(keys: set[str]) -> float:
        left = any(k in keys for k in ("a", "left"))
        right = any(k in keys for k in ("d", "right"))
        return float(right) - float(left)

    @staticmethod
    def direction(keys: Iterable[str]) -> tuple[float, float]:
        dx = dy = 0.0
        pressed = {k.lower() for k in keys if isinstance(k, str)}
        for name in pressed:
            vx, vy = _KEY_DIRECTIONS.get(name, (0.0, 0.0))
            dx += vx
            dy += vy
        dx = max(-1.0, min(1.0, dx))
        dy = max(-1.0, min(1.0, dy))
        length = math.hypot(dx, dy)
        if length > 0.0:
            dx, dy = dx / length, dy / length
        return dx, dy

    def step(self, dt: float, keys: set[str]) -> None:
        if isinstance(dt, bool) or not isinstance(dt, (int, float)) or not 0.0 < dt <= MAX_STEP:
            raise ValueError(f"dt must be in (0, {MAX_STEP}] seconds")
        keys = keys or ()
        dx, dy = self.direction(keys)
        pressed = {k.lower() for k in keys if isinstance(k, str)}
        axis = self._axis(pressed)
        jump_down = bool(pressed & {"space", " ", "w", "up"})
        jump_edge = jump_down and not self._jump_held

        # Everything below only stages results; nothing is published until the
        # whole tick (including every native call) has succeeded.
        objects = self.project.objects
        infos = [self._collider_info(obj) for obj in objects]
        solids = []
        for obj, info in zip(objects, infos):
            if info is not None and not info["trigger"] and not info["dynamic"]:
                solids.append(self._aabb(info["bounds"], obj.id))
        obstacle_array = (_AABB2D * max(1, len(solids)))(*solids) if solids else (_AABB2D * 1)()
        lib = self._native() if any(i is not None for i in infos) else None

        moves: dict[int, tuple[float, float]] = {}
        new_bodies: dict[str, tuple] = {}
        new_velocity: dict[str, tuple[float, float]] = {}
        final_boxes: dict[str, _AABB2D] = {}
        grounded: set[str] = set()
        for index, (obj, info) in enumerate(zip(objects, infos)):
            comps = obj.components if isinstance(obj.components, dict) else {}
            motion = comps.get("motion2d")
            motion = motion if isinstance(motion, dict) else None
            position = obj.transform.position
            has_position = isinstance(position, list) and len(position) >= 2
            iv = [0.0, 0.0]
            controlled, speed = False, 0.0
            if motion is not None:
                velocity = motion.get("velocity")
                if isinstance(velocity, (list, tuple)) and len(velocity) >= 2:
                    iv = [_finite_or(velocity[0], 0.0), _finite_or(velocity[1], 0.0)]
                controlled = motion.get("controlled") is True
                speed = _finite_or(motion.get("speed", 80.0), 0.0)

            if info is not None and not info["dynamic"]:
                if not info["trigger"]:
                    final_boxes[obj.id] = self._aabb(info["bounds"], obj.id)
                    continue  # static solids never move
                final_boxes[obj.id] = self._aabb(info["bounds"], obj.id)
            if info is not None and info["dynamic"]:
                if not has_position:
                    raise RendererError(f"'{obj.id}' has no 2D position")
                label = obj.id
                state_key = (repr(position[:2]), repr(obj.transform.rotation), repr(obj.transform.scale),
                             repr(comps.get("sprite2d")), repr(comps.get("collider2d")))
                cached = self._bodies.get(label)
                if cached is not None and cached[0] == state_key:
                    body = _Body2D.from_buffer_copy(cached[1])
                else:
                    vx, vy = self._velocity.get(label, (iv[0], iv[1]))
                    body = _Body2D(self._aabb(info["bounds"], label), _Vec2(vx, vy), 0, 0)
                gravity = info["gravity"]
                if gravity == 0.0:  # free 2D movement, but still stopped by solids
                    vx = iv[0] + (dx * speed if controlled else 0.0)
                    vy = iv[1] + (dy * speed if controlled else 0.0)
                    body.velocity = _Vec2(vx, vy)
                elif controlled:
                    body.velocity.x = axis * speed
                jump = bool(jump_edge and controlled and gravity > 0.0)
                before = (body.bounds.min.x, body.bounds.min.y)
                if info["trigger"]:
                    count, obstacles = 0, obstacle_array
                else:
                    count, obstacles = len(solids), obstacle_array
                self._check(lib.pe_body2d_step(ctypes.byref(body), dt, gravity, info["jump_speed"],
                                               info["max_fall_speed"], 1 if jump else 0, obstacles, count),
                            f"simulate '{label}'")
                x = _finite_or(position[0], 0.0) + (body.bounds.min.x - before[0])
                y = _finite_or(position[1], 0.0) + (body.bounds.min.y - before[1])
                cx, cy = max(-MAX_COORD, min(MAX_COORD, x)), max(-MAX_COORD, min(MAX_COORD, y))
                moves[index] = (cx, cy)
                new_velocity[label] = (body.velocity.x, body.velocity.y)
                if (cx, cy) == (x, y):
                    new_bodies[label] = ((repr([cx, cy]),) + state_key[1:], body)
                final_boxes[label] = (_AABB2D.from_buffer_copy(body.bounds) if (cx, cy) == (x, y)
                                      else self._moved_box(obj, info, (cx, cy), position))
                if body.grounded:
                    grounded.add(label)
                continue

            # Legacy free motion (no enabled collider, or a trigger without a dynamic body).
            if motion is None or not has_position:
                continue
            vx, vy = iv
            if controlled:
                vx += dx * speed
                vy += dy * speed
            if vx == 0.0 and vy == 0.0:
                continue
            x = _finite_or(position[0], 0.0) + vx * dt
            y = _finite_or(position[1], 0.0) + vy * dt
            moves[index] = (max(-MAX_COORD, min(MAX_COORD, x)), max(-MAX_COORD, min(MAX_COORD, y)))
            if info is not None:  # moving trigger: refresh its box from the new position
                final_boxes[obj.id] = self._moved_box(obj, info, moves[index], position)

        # Trigger overlaps, using the native predicate on the final boxes.
        active: set[tuple[str, str]] = set()
        triggers = {obj.id: info["trigger"] for obj, info in zip(objects, infos) if info is not None}
        ids = sorted(final_boxes)
        for i, a in enumerate(ids):
            for b in ids[i + 1:]:
                if not (triggers[a] or triggers[b]):
                    continue
                result = lib.pe_aabb2d_overlaps(ctypes.byref(final_boxes[a]), ctypes.byref(final_boxes[b]))
                if result not in (0, 1):
                    raise RendererError(f"Pixelica2D overlap test failed for '{a}' and '{b}' (status {result})")
                if result:
                    active.add((a, b) if triggers[a] or not triggers[b] else (b, a))

        previous = self.active_triggers
        events = [("stay" if pair in previous else "enter", *pair) for pair in active]
        events += [("exit", *pair) for pair in previous - active]

        # Publish.
        for index, (nx, ny) in moves.items():
            pos = objects[index].transform.position
            pos[0], pos[1] = nx, ny
        self._bodies = new_bodies
        self._velocity.update(new_velocity)
        self._jump_held = jump_down
        self.grounded = grounded
        self.active_triggers = active
        self.trigger_events = sorted(events)
        self.ticks += 1

    def _moved_box(self, obj, info: dict, new_xy: tuple[float, float], position: list) -> _AABB2D:
        """Box of a legacy-moving trigger after its position changes."""
        ox, oy = new_xy[0] - _finite_or(position[0], 0.0), new_xy[1] - _finite_or(position[1], 0.0)
        b = info["bounds"]
        return self._aabb([b[0] + ox, b[1] + oy, b[2] + ox, b[3] + oy], obj.id)

    def advance(self, elapsed: float, keys: set[str]) -> int:
        """Run as many FIXED_DT steps as ``elapsed`` seconds allow (capped at
        MAX_CATCH_UP_STEPS; surplus time is dropped). Returns steps run."""
        if isinstance(elapsed, bool) or not isinstance(elapsed, (int, float)) or not math.isfinite(elapsed):
            raise ValueError("elapsed must be a finite number")
        self._accumulator += max(0.0, float(elapsed))
        steps = 0
        while self._accumulator >= FIXED_DT and steps < MAX_CATCH_UP_STEPS:
            self.step(FIXED_DT, keys)
            self._accumulator -= FIXED_DT
            steps += 1
        if steps == MAX_CATCH_UP_STEPS:
            self._accumulator = min(self._accumulator, FIXED_DT)
        return steps
