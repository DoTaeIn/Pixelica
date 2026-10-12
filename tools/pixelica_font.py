#!/usr/bin/env python3
"""Convert a Unicode BDF bitmap font into a C header for PIXELICA_UI_Font.

Only the glyphs used by your game strings are emitted. Python stdlib only.

    python tools/pixelica_font.py --bdf my_font.bdf --text strings.txt \
        --name game_font --output src/game_font.h

The header defines `<name>_glyph` (a PIXELICA_UI_GlyphFn) and
`<name>_font(scale)` (returns a ready PIXELICA_UI_Font). Rows use bit31 as the
left pixel. The cell is fixed: width 1..16, height 1..32. By default it is the
BDF FONTBOUNDINGBOX; each glyph is placed using its BBX offsets relative to the
baseline (ascent = bbox height + bbox y offset, left = bbox x offset).

Supported BDF: STARTFONT 2.1/2.2, 1-bit bitmaps, horizontal metrics only.
Rejected: antialiased (SIZE with a 4th field), METRICSSET != 0, SWIDTH1/DWIDTH1/
VVECTOR, unknown keywords, duplicate ENCODING, CHARS mismatch, bad bitmap rows
or non-zero padding bits, data after ENDFONT. Glyphs with ENCODING -1 are
ignored. You must supply a BDF whose license allows this use; none is bundled.
No TTF loading, RTL or complex shaping.
"""
import argparse
import os
import re
import sys
import tempfile

MAX_BDF_BYTES = 16 * 1024 * 1024
MAX_TEXT_BYTES = 4 * 1024 * 1024
MAX_LINE_BYTES = 4096
MAX_CHARS = 65536
MAX_PROPERTIES = 4096
MAX_GLYPH_DIM = 128
MAX_CELL_W, MAX_CELL_H = 16, 32
MAX_UNICODE = 0x10FFFF
MAX_COORD = 1024

_INT = re.compile(r"-?[0-9]{1,9}\Z")
_HEX = re.compile(r"[0-9A-Fa-f]+\Z")
_NAME = re.compile(r"[a-z][a-z0-9_]{0,47}\Z")


class FontError(Exception):
    pass


def _fail(line, msg):
    raise FontError("line %d: %s" % (line, msg))


def _int(line, tok, lo=-MAX_COORD, hi=MAX_COORD):
    if not _INT.match(tok):
        _fail(line, "expected integer, got %r" % tok)
    value = int(tok)
    if not lo <= value <= hi:
        _fail(line, "value %d out of range %d..%d" % (value, lo, hi))
    return value


def _records(data):
    if len(data) > MAX_BDF_BYTES:
        raise FontError("BDF larger than %d bytes" % MAX_BDF_BYTES)
    if b"\0" in data:
        raise FontError("BDF contains a NUL byte")
    recs = []
    for number, raw in enumerate(data.split(b"\n"), 1):
        if raw.endswith(b"\r"):
            raw = raw[:-1]
        if len(raw) > MAX_LINE_BYTES:
            _fail(number, "line longer than %d bytes" % MAX_LINE_BYTES)
        toks = [t.decode("latin-1") for t in raw.split()]
        if toks and toks[0] != "COMMENT":
            recs.append((number, toks))
    return recs


class _Cursor:
    def __init__(self, recs):
        self.recs = recs
        self.i = 0

    def next(self, expected):
        if self.i >= len(self.recs):
            raise FontError("unexpected end of file, expected %s" % expected)
        rec = self.recs[self.i]
        self.i += 1
        return rec


class Glyph:
    def __init__(self, line, name, bbx, rows):
        self.line, self.name, self.bbx, self.rows = line, name, bbx, rows


class BdfFont:
    def __init__(self, bbox, glyphs):
        self.bbox = bbox  # (width, height, xoff, yoff)
        self.glyphs = glyphs  # codepoint -> Glyph


def _parse_glyph(cur, line, toks):
    if toks[0] != "STARTCHAR" or len(toks) < 2:
        _fail(line, "expected STARTCHAR <name>")
    name = toks[1]
    encoding = bbx = dwidth = None
    while True:
        line, toks = cur.next("BITMAP")
        kw = toks[0]
        if kw == "BITMAP":
            if len(toks) != 1:
                _fail(line, "BITMAP takes no arguments")
            break
        if kw == "ENCODING":
            if encoding is not None:
                _fail(line, "duplicate ENCODING")
            if len(toks) == 2:
                encoding = _int(line, toks[1], -1, MAX_UNICODE)
            elif len(toks) == 3 and toks[1] == "-1":
                _int(line, toks[2], -1, 0x7FFFFFFF)
                encoding = -1
            else:
                _fail(line, "malformed ENCODING")
            if 0xD800 <= encoding <= 0xDFFF:
                _fail(line, "ENCODING is a surrogate")
        elif kw == "SWIDTH" or kw == "DWIDTH":
            if len(toks) != 3:
                _fail(line, "%s needs 2 integers" % kw)
            dx, dy = _int(line, toks[1]), _int(line, toks[2])
            if dy != 0:
                _fail(line, "%s with non-zero y is unsupported" % kw)
            if kw == "DWIDTH":
                if dwidth is not None:
                    _fail(line, "duplicate DWIDTH")
                dwidth = dx
        elif kw == "BBX":
            if bbx is not None:
                _fail(line, "duplicate BBX")
            if len(toks) != 5:
                _fail(line, "BBX needs 4 integers")
            bbx = (_int(line, toks[1], 0, MAX_GLYPH_DIM), _int(line, toks[2], 0, MAX_GLYPH_DIM),
                   _int(line, toks[3]), _int(line, toks[4]))
            if (bbx[0] == 0) != (bbx[1] == 0):
                _fail(line, "BBX must have both width and height zero or both positive")
        else:
            _fail(line, "unsupported glyph keyword %s" % kw)
    if encoding is None or bbx is None or dwidth is None:
        _fail(line, "glyph %s needs ENCODING, DWIDTH and BBX before BITMAP" % name)
    width, height = bbx[0], bbx[1]
    nbytes = (width + 7) // 8
    rows = []
    for _ in range(height):
        line, toks = cur.next("bitmap row")
        if len(toks) != 1 or not _HEX.match(toks[0]) or len(toks[0]) != 2 * nbytes:
            _fail(line, "bitmap row must be exactly %d hex digits" % (2 * nbytes))
        value = int(toks[0], 16)
        if value & ((1 << (nbytes * 8 - width)) - 1):
            _fail(line, "non-zero padding bits in bitmap row")
        rows.append(value)
    line, toks = cur.next("ENDCHAR")
    if toks != ["ENDCHAR"]:
        _fail(line, "expected ENDCHAR after %d bitmap rows" % height)
    return encoding, Glyph(line, name, bbx, rows)


def parse_bdf(data):
    """Parse BDF bytes into a BdfFont, raising FontError on anything odd."""
    cur = _Cursor(_records(data))
    line, toks = cur.next("STARTFONT")
    if toks[0] != "STARTFONT" or len(toks) != 2 or toks[1] not in ("2.1", "2.2"):
        _fail(line, "expected STARTFONT 2.1 or 2.2")
    seen = set()
    bbox = None
    nchars = None
    while nchars is None:
        line, toks = cur.next("CHARS")
        kw = toks[0]
        if kw in seen:
            _fail(line, "duplicate %s" % kw)
        seen.add(kw)
        if kw == "FONT":
            if len(toks) < 2:
                _fail(line, "FONT needs a name")
        elif kw == "SIZE":
            if len(toks) != 4:
                _fail(line, "SIZE must have 3 fields (antialiased BDF is unsupported)")
            for tok in toks[1:]:
                _int(line, tok, 1, MAX_COORD)
        elif kw == "FONTBOUNDINGBOX":
            if len(toks) != 5:
                _fail(line, "FONTBOUNDINGBOX needs 4 integers")
            bbox = (_int(line, toks[1], 1, MAX_COORD), _int(line, toks[2], 1, MAX_COORD),
                    _int(line, toks[3]), _int(line, toks[4]))
        elif kw == "METRICSSET":
            if toks[1:] != ["0"]:
                _fail(line, "only METRICSSET 0 (horizontal) is supported")
        elif kw == "CONTENTVERSION":
            if len(toks) != 2:
                _fail(line, "CONTENTVERSION needs 1 integer")
            _int(line, toks[1], 0, 0x7FFFFFFF)
        elif kw == "STARTPROPERTIES":
            if len(toks) != 2:
                _fail(line, "STARTPROPERTIES needs a count")
            count = _int(line, toks[1], 0, MAX_PROPERTIES)
            for _ in range(count):
                pline, ptoks = cur.next("property")
                if ptoks[0] == "ENDPROPERTIES":
                    _fail(pline, "fewer than %d properties" % count)
            pline, ptoks = cur.next("ENDPROPERTIES")
            if ptoks != ["ENDPROPERTIES"]:
                _fail(pline, "expected ENDPROPERTIES after %d properties" % count)
        elif kw == "CHARS":
            if len(toks) != 2:
                _fail(line, "CHARS needs a count")
            if "FONT" not in seen or bbox is None:
                _fail(line, "FONT and FONTBOUNDINGBOX must precede CHARS")
            nchars = _int(line, toks[1], 0, MAX_CHARS)
        else:
            _fail(line, "unsupported or unknown keyword %s" % kw)
    glyphs = {}
    for _ in range(nchars):
        line, toks = cur.next("STARTCHAR")
        encoding, glyph = _parse_glyph(cur, line, toks)
        if encoding < 0:
            continue
        if encoding in glyphs:
            _fail(line, "duplicate ENCODING %d (glyph %s)" % (encoding, glyph.name))
        glyphs[encoding] = glyph
    line, toks = cur.next("ENDFONT")
    if toks != ["ENDFONT"]:
        _fail(line, "expected ENDFONT after %d glyphs (CHARS mismatch?)" % nchars)
    if cur.i != len(cur.recs):
        _fail(cur.recs[cur.i][0], "data after ENDFONT")
    return BdfFont(bbox, glyphs)


def used_codepoints(text):
    """Codepoints needing glyphs. Newlines are skipped; other controls rejected."""
    used = set()
    for ch in text:
        cp = ord(ch)
        if ch in "\r\n":
            continue
        if cp < 0x20 or 0x7F <= cp < 0xA0:
            raise FontError("control character U+%04X in text (only newlines are skipped)" % cp)
        used.add(cp)
    return used


def _cell_rows(font, glyph, cp, cell_w, cell_h, ascent, left):
    width, height, xoff, yoff = glyph.bbx
    nbytes = (width + 7) // 8
    out = [0] * cell_h
    for r, bits in enumerate(glyph.rows):
        y = ascent - (yoff + height) + r
        for c in range(width):
            if not (bits >> (nbytes * 8 - 1 - c)) & 1:
                continue
            x = xoff - left + c
            if not (0 <= x < cell_w and 0 <= y < cell_h):
                raise FontError("glyph U+%04X pixel falls outside the %dx%d cell "
                                "(adjust --cell/--ascent/--left)" % (cp, cell_w, cell_h))
            out[y] |= 1 << (31 - x)
    return out


def _safe_comment(text):
    return re.sub(r"[^A-Za-z0-9._-]", "_", text)


def convert(bdf, text, name, source="font.bdf", cell=None, ascent=None, left=None,
            include_ascii=False, allow_missing=False):
    """Return the C header text for the glyphs of `text` found in `bdf` (bytes)."""
    if not _NAME.match(name) or "__" in name:
        raise FontError("--name must match [a-z][a-z0-9_]* (max 48 chars, no '__')")
    font = parse_bdf(bdf)
    bw, bh, bx, by = font.bbox
    cell_w, cell_h = cell if cell is not None else (bw, bh)
    ascent = bh + by if ascent is None else ascent
    left = bx if left is None else left
    if not 1 <= cell_w <= MAX_CELL_W or not 1 <= cell_h <= MAX_CELL_H:
        raise FontError("cell %dx%d outside 1..%dx1..%d; pass --cell" %
                        (cell_w, cell_h, MAX_CELL_W, MAX_CELL_H))
    wanted = used_codepoints(text)
    if include_ascii:
        wanted.update(range(0x20, 0x7F))
    missing = sorted(cp for cp in wanted if cp not in font.glyphs)
    if missing and not allow_missing:
        shown = ", ".join("U+%04X" % cp for cp in missing[:16])
        more = "" if len(missing) <= 16 else " (+%d more)" % (len(missing) - 16)
        raise FontError("BDF has no glyph for %d codepoint(s): %s%s" % (len(missing), shown, more))
    codes = sorted(cp for cp in wanted if cp in font.glyphs)
    if not codes:
        raise FontError("no glyphs to emit (empty text, or every glyph missing)")
    table = [_cell_rows(font, font.glyphs[cp], cp, cell_w, cell_h, ascent, left) for cp in codes]

    upper = name.upper()
    out = []
    w = out.append
    w("/* Generated by tools/pixelica_font.py - do not edit. */")
    w("/* Source font: %s. The glyph data stays under that font's license. */" % _safe_comment(source))
    w("#ifndef PIXELICA_FONT_%s_H" % upper)
    w("#define PIXELICA_FONT_%s_H" % upper)
    w("#include <stdbool.h>")
    w("#include <stddef.h>")
    w("#include <stdint.h>")
    w('#include "pixelica/ui.h"')
    w("")
    w("#define %s_GLYPH_COUNT %du" % (upper, len(codes)))
    w("#define %s_WIDTH %du" % (upper, cell_w))
    w("#define %s_HEIGHT %du" % (upper, cell_h))
    w("")
    w("static const uint32_t %s_codepoints[%s_GLYPH_COUNT] = {" % (name, upper))
    for i in range(0, len(codes), 8):
        w("    " + ", ".join("0x%04Xu" % cp for cp in codes[i:i + 8]) + ",")
    w("};")
    w("")
    w("/* Rows use bit31 as the left pixel. */")
    w("static const uint32_t %s_rows[%s_GLYPH_COUNT][%s_HEIGHT] = {" % (name, upper, upper))
    for cp, rows in zip(codes, table):
        w("    {" + ", ".join("0x%08Xu" % r for r in rows) + "}, /* U+%04X */" % cp)
    w("};")
    w("")
    w("static inline bool %s_glyph(void *userdata, uint32_t codepoint, uint32_t rows[32])" % name)
    w("{")
    w("    size_t lo = 0, hi = %s_GLYPH_COUNT, i;" % upper)
    w("")
    w("    (void)userdata;")
    w("    if (rows == NULL) {")
    w("        return false;")
    w("    }")
    w("    while (lo < hi) {")
    w("        size_t mid = lo + (hi - lo) / 2;")
    w("")
    w("        if (%s_codepoints[mid] < codepoint) {" % name)
    w("            lo = mid + 1;")
    w("        } else {")
    w("            hi = mid;")
    w("        }")
    w("    }")
    w("    if (lo == %s_GLYPH_COUNT || %s_codepoints[lo] != codepoint) {" % (upper, name))
    w("        return false;")
    w("    }")
    w("    for (i = 0; i < %s_HEIGHT; ++i) {" % upper)
    w("        rows[i] = %s_rows[lo][i];" % name)
    w("    }")
    w("    for (; i < 32; ++i) {")
    w("        rows[i] = 0;")
    w("    }")
    w("    return true;")
    w("}")
    w("")
    w("/* scale 1..4; the font references only static data. */")
    w("static inline PIXELICA_UI_Font %s_font(uint8_t scale)" % name)
    w("{")
    w("    PIXELICA_UI_Font font;")
    w("")
    w("    font.width = %s_WIDTH;" % upper)
    w("    font.height = %s_HEIGHT;" % upper)
    w("    font.scale = scale;")
    w("    font.glyph = %s_glyph;" % name)
    w("    font.userdata = NULL;")
    w("    return font;")
    w("}")
    w("")
    w("#endif /* PIXELICA_FONT_%s_H */" % upper)
    return "\n".join(out) + "\n"


def write_atomic(path, content):
    """Write content to path via a temp file; an existing file survives errors."""
    path = os.path.abspath(path)
    parent = os.path.dirname(path)
    if not os.path.isdir(parent):
        raise FontError("output directory does not exist: %s" % parent)
    if os.path.isdir(path):
        raise FontError("output path is a directory: %s" % path)
    mode = os.stat(path).st_mode & 0o777 if os.path.exists(path) else 0o644
    fd, tmp = tempfile.mkstemp(prefix=".pixelica_font.", suffix=".tmp", dir=parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(content.encode("utf-8"))
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _read_limited(path, limit, what):
    try:
        if os.path.getsize(path) > limit:
            raise FontError("%s larger than %d bytes: %s" % (what, limit, path))
        with open(path, "rb") as handle:
            return handle.read(limit + 1)
    except OSError as exc:
        raise FontError("cannot read %s: %s" % (what, exc))


def _cell_arg(value):
    match = re.fullmatch(r"([0-9]{1,3})x([0-9]{1,3})", value)
    if not match:
        raise argparse.ArgumentTypeError("expected WxH, e.g. 12x12")
    return int(match.group(1)), int(match.group(2))


def main(argv=None):
    ap = argparse.ArgumentParser(description="BDF -> PIXELICA_UI_Font C header (used glyphs only)")
    ap.add_argument("--bdf", required=True, help="Unicode BDF font you are licensed to use")
    ap.add_argument("--text", action="append", required=True,
                    help="UTF-8 file with the game strings (repeatable)")
    ap.add_argument("--output", required=True, help="C header to write")
    ap.add_argument("--name", required=True, help="C identifier prefix, e.g. game_font")
    ap.add_argument("--cell", type=_cell_arg, help="cell WxH (default: FONTBOUNDINGBOX)")
    ap.add_argument("--ascent", type=int, help="baseline row in the cell (default: bbox h + y)")
    ap.add_argument("--left", type=int, help="BDF x of the cell's left edge (default: bbox x)")
    ap.add_argument("--include-ascii", action="store_true",
                    help="also emit printable ASCII (U+0020..U+007E)")
    ap.add_argument("--allow-missing", action="store_true",
                    help="skip codepoints the BDF lacks instead of failing")
    args = ap.parse_args(argv)
    try:
        for path in [args.bdf] + args.text:
            if os.path.exists(args.output) and os.path.exists(path) and \
                    os.path.samefile(path, args.output):
                raise FontError("output path is also an input: %s" % path)
        bdf = _read_limited(args.bdf, MAX_BDF_BYTES, "BDF")
        text = ""
        for path in args.text:
            raw = _read_limited(path, MAX_TEXT_BYTES, "text file")
            try:
                text += raw.decode("utf-8-sig") + "\n"  # a leading BOM is not a glyph
            except UnicodeDecodeError as exc:
                raise FontError("text file is not valid UTF-8: %s: %s" % (path, exc))
        header = convert(bdf, text, args.name, os.path.basename(args.bdf), args.cell,
                         args.ascent, args.left, args.include_ascii, args.allow_missing)
        write_atomic(args.output, header)
    except (FontError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
