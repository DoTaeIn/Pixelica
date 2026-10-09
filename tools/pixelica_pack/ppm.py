"""Bounded PPM (P6 binary / P3 ascii) reader producing RGB565 texture data."""

import array

import pixelica_format
from pixelica_format import PixelicaPackError, TextureData, rgb565

# Input cap. A 4096x4096 P6 is 48 MiB; the P3TX output limit (16 MiB) is the
# real bound and is enforced from the header before any pixel is decoded.
MAX_PPM_BYTES = 64 * 1024 * 1024
MAX_MAXVAL = 65535

_WHITESPACE = b" \t\r\n\x0b\x0c"
_TOKEN_END = _WHITESPACE + b"#"
# magic(4)+version/header/size/count(12) + 2 table entries(32) + INFO(8)
_P3TX_FIXED_BYTES = 16 + 2 * 16 + 8


def _next_token(data, pos):
    """Return (token, new_pos); skips whitespace and '#' comments."""
    size = len(data)
    while pos < size:
        c = data[pos]
        if c in _WHITESPACE:
            pos += 1
        elif c == 0x23:  # '#'
            while pos < size and data[pos] not in b"\r\n":
                pos += 1
        else:
            break
    start = pos
    while pos < size and data[pos] not in _TOKEN_END:
        pos += 1
    return data[start:pos], pos


def _header_int(token, what, lo, hi):
    if not token or len(token) > 10 or not token.isdigit():
        raise PixelicaPackError("PPM: invalid %s %r" % (what, token))
    value = int(token)
    if not lo <= value <= hi:
        raise PixelicaPackError("PPM: %s %d outside %d..%d" % (what, value, lo, hi))
    return value


def _to8(value, maxval):
    if maxval == 255:
        return value
    return (value * 255 + maxval // 2) // maxval


def parse_ppm(data):
    """Parse a P6/P3 PPM into TextureData (RGB565, top row first).

    Errors on bad headers, sizes beyond the texture limits, samples above
    maxval, truncated rasters and trailing garbage.
    """
    data = bytes(data)
    if len(data) > MAX_PPM_BYTES:
        raise PixelicaPackError("PPM is larger than %d bytes" % MAX_PPM_BYTES)
    magic, pos = _next_token(data, 0)
    if magic not in (b"P6", b"P3"):
        raise PixelicaPackError("PPM: unsupported magic %r (need P6 or P3)" % magic)
    token, pos = _next_token(data, pos)
    width = _header_int(token, "width", 1, pixelica_format.MAX_TEXTURE_DIM)
    token, pos = _next_token(data, pos)
    height = _header_int(token, "height", 1, pixelica_format.MAX_TEXTURE_DIM)
    token, pos = _next_token(data, pos)
    maxval = _header_int(token, "maxval", 1, MAX_MAXVAL)
    count = width * height
    if _P3TX_FIXED_BYTES + count * 2 > pixelica_format.MAX_FILE_BYTES:
        raise PixelicaPackError(
            "PPM: %dx%d would need a %d byte .p3tex, over the %d byte limit"
            % (width, height, _P3TX_FIXED_BYTES + count * 2,
               pixelica_format.MAX_FILE_BYTES))

    samples = count * 3
    pixels = array.array("H")
    if magic == b"P6":
        if pos >= len(data) or data[pos] not in _WHITESPACE:
            raise PixelicaPackError("PPM: missing whitespace before raster data")
        # Exactly one whitespace byte ends the header; CR LF counts as that
        # one delimiter (Windows tools). Nothing else is skipped: a raster
        # may legitimately start with 0x0A, 0x20, 0x23 or 0x0D.
        if data[pos] == 0x0D and data[pos + 1:pos + 2] == b"\n":
            pos += 2
        else:
            pos += 1
        wide = maxval > 255
        expected = samples * (2 if wide else 1)
        raster = data[pos:]
        if len(raster) < expected:
            raise PixelicaPackError("PPM: truncated raster (%d of %d bytes)"
                              % (len(raster), expected))
        if len(raster) > expected:
            raise PixelicaPackError("PPM: %d trailing bytes after the raster"
                              % (len(raster) - expected))
        if wide:
            values = [(raster[i] << 8) | raster[i + 1]
                      for i in range(0, expected, 2)]
        else:
            values = raster
    else:
        rest = data[pos:]
        tokens = rest.split() if b"#" not in rest else _tokens_with_comments(rest)
        if len(tokens) < samples:
            raise PixelicaPackError("PPM: truncated raster (%d of %d samples)"
                              % (len(tokens), samples))
        if len(tokens) > samples:
            raise PixelicaPackError("PPM: %d extra samples after the raster"
                              % (len(tokens) - samples))
        values = []
        for tok in tokens:
            if len(tok) > 5 or not tok.isdigit():
                raise PixelicaPackError("PPM: invalid sample %r" % tok)
            values.append(int(tok))

    if max(values) > maxval:
        raise PixelicaPackError("PPM: sample %d exceeds maxval %d"
                          % (max(values), maxval))
    if maxval != 255:
        values = [_to8(v, maxval) for v in values]
    for i in range(0, samples, 3):
        pixels.append(rgb565(values[i], values[i + 1], values[i + 2]))
    return TextureData(width, height, pixels)


def _tokens_with_comments(rest):
    tokens = []
    pos = 0
    while True:
        token, pos = _next_token(rest, pos)
        if not token:
            if pos >= len(rest):
                return tokens
            raise PixelicaPackError("PPM: malformed raster near byte %d" % pos)
        tokens.append(token)


def ppm_to_p3tex(data):
    """PPM bytes -> .p3tex bytes."""
    return pixelica_format.encode_texture(parse_ppm(data))
