# Pixelica asset formats (v1) and the pixelica_pack tool

Edit formats (`.vox`, `.ppm`) are converted offline by `tools/pixelica_pack` (Python
standard library only, desktop only) into three runtime formats:

| Format | Extension | Magic | Contents |
|--------|-----------|-------|----------|
| mesh | `.p3mesh` | `P3MS` | indexed triangle list, per-triangle RGB565 color, optional UVs |
| texture | `.p3tex` | `P3TX` | RGB565 pixels |
| cart | `.p3cart` | `P3CT` | resource package (meshes and textures only; never code) |

All numbers are **little endian**, floats are **IEEE-754 binary32**. Files are
serialized field by field; no C struct is ever dumped. Output is
deterministic: the same input always produces the same bytes.

The C loaders (`include/pixelica/assets.h`) and `tools/pixelica_pack/pixelica_format.py` implement
this document independently and must agree byte for byte.

## Common container

```
offset size field
0      4    magic        "P3MS" | "P3TX" | "P3CT"
4      2    version      u16 = 1
6      2    header_size  u16 = 16 + 16 * chunk_count
8      4    file_size    u32, exact size of the whole file
12     4    chunk_count  u32, 1..16
16     16*n chunk table, n entries in payload order
...         payloads
```

Chunk table entry (16 bytes):

```
0  4  type    4 ASCII bytes
4  4  offset  u32, absolute offset from the start of the file
8  4  length  u32, payload byte length
12 4  crc32   u32, CRC-32/IEEE (zlib) of the payload
```

Rules a reader must enforce (any violation rejects the whole file):

* `version == 1`, `1 <= chunk_count <= 16`, `header_size == 16 + 16 * count`,
  `file_size == actual size`.
* Payloads are tightly packed in table order: the first `offset` equals
  `header_size`, each next `offset` equals the previous `offset + length`, and
  the last end equals `file_size`. No gaps, overlaps or trailing bytes.
  Check `offset`/`length` against the file bounds **before** adding them.
* No duplicate chunk types and no unknown chunk types. Version 1 has no
  optional/skippable chunk mechanism; a new chunk requires a new version.
* CRC-32 of every payload matches.
* Writers emit chunks in the canonical order listed below; readers must accept
  any order that satisfies the rules above.

Default limits (compile-time configurable in C, fixed in Python): file size
16 MiB, vertices 65 535, indices 196 608, texture dimension 4 096.

A failed load must not modify caller storage beyond zeroing the output
struct. Validate the entire asset first, then copy. Memory loaders copy
decoded values into caller-provided storage (so input may be unaligned and
needs no particular alignment), perform no heap allocation or file IO, and
reject input/storage buffers that overlap.

## P3MS mesh (`.p3mesh`)

Canonical order: `VTX0`, `IDX0`, `COL0`, `UV00` (optional).

| Chunk | Payload |
|-------|---------|
| `VTX0` | `vertex_count * 3` binary32 (x, y, z); length multiple of 12; all finite |
| `IDX0` | `index_count` u16 triangle-list indices; every index `< vertex_count` |
| `COL0` | one u16 RGB565 per triangle (`index_count / 3` values) |
| `UV00` | optional, `vertex_count * 2` binary32 (u, v), finite, `0 <= value <= 1` |

Counts derive from chunk lengths. `vertex_count >= 1`, `index_count >= 3`,
`index_count % 3 == 0`, `COL0` length is exactly `2 * (index_count / 3)`,
`UV00` length is exactly `8 * vertex_count`. Unreferenced vertices are legal.
There is no texture path in the file; the caller binds a texture. Loaded
meshes have `PIXELICA_Mesh.color = 0xffff`; the per-triangle colors (and UVs, when
present) live in the `PIXELICA_Material`.

RGB565 packing matches the `PIXELICA_RGB565` macro and **truncates**:
`((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)`.

Winding: triangles are counter-clockwise seen from outside (the front face)
in a right-handed coordinate system.

## P3TX texture (`.p3tex`)

Canonical order: `INFO`, `DATA`.

| Chunk | Payload |
|-------|---------|
| `INFO` | 8 bytes: `width:u16`, `height:u16`, `reserved:u32 = 0` |
| `DATA` | `width * height` u16 RGB565, row-major, **top row first** |

`1 <= width, height <= 4096` and `DATA` length is exactly `width * height * 2`.
UV (0,0) is the top-left texel; the renderer samples nearest, clamped.

## P3CT cart (`.p3cart`)

Canonical order: `ENTR`, `DATA`.

`ENTR` = `count:u32` followed by `count` 48-byte records:

```
0  4  resource_id u32, nonzero
4  4  kind        "P3MS" or "P3TX" only
8  32 name        1..31 characters of [A-Za-z0-9_.-], then NUL, then zero padding
40 4  offset      u32, relative to the start of DATA
44 4  length      u32, nonzero
```

`ENTR` length is exactly `4 + 48 * count` and `1 <= count <= 1024`. The upper
bound is `PIXELICA_CART_MAX_ENTRIES` in the C loader (`PIXELICA_ERR_CAPACITY` above it) and
`MAX_CART_ENTRIES` in `tools/pixelica_pack/pixelica_format.py`; the Python encoders
(`encode_cart`, `bundle_named`, `pixelica_pack bundle`) and decoders (`decode_cart`,
`inspect`) use the same hard default, so 1024 entries are accepted and 1025 are
rejected with `cart has 1025 entries, limit is 1024`. The count is checked
before any per-entry or nested-resource work. Records are sorted
by strictly ascending `resource_id`; names are unique. `DATA` is the
concatenation of complete resource files in `ENTR` order: record 0 has
`offset == 0`, each next `offset` equals the previous end, and the final end
equals the `DATA` length (no padding, no unreferenced bytes; resources may sit
at odd addresses, so loaders must tolerate unaligned views).

Opening a cart fully validates every nested resource (its own container, CRCs
and semantics) and checks that `kind` equals the nested magic. The C API is a
zero-copy view: the input bytes must outlive the `PIXELICA_Cart`.

`pixelica_pack bundle` assigns ids `1..N` in ascending byte order of the names, so
the same set of named files always yields the same cart regardless of command
line order.

## C API summary (`include/pixelica/assets.h`, owned by the engine)

```c
PIXELICA_Status pixelica_mesh_asset_memory_required(const void *data, size_t size, size_t *out_bytes);
PIXELICA_Status pixelica_mesh_asset_load(PIXELICA_MeshAsset *out, const void *data, size_t size,
                               void *storage, size_t storage_size);
PIXELICA_Status pixelica_texture_asset_memory_required(const void *data, size_t size, size_t *out_bytes);
PIXELICA_Status pixelica_texture_asset_load(PIXELICA_Texture *out, const void *data, size_t size,
                                  void *storage, size_t storage_size);
PIXELICA_Status pixelica_cart_open(PIXELICA_Cart *out, const void *data, size_t size);
PIXELICA_Status pixelica_cart_find(const PIXELICA_Cart *cart, uint32_t id,
                         const void **out_data, size_t *out_size);
```

## pixelica_pack command line

```
python tools/pixelica_pack/pixelica_pack.py import vox INPUT.vox -o OUTPUT.p3mesh [--scale S] [--pivot floor|center]
python tools/pixelica_pack/pixelica_pack.py texture INPUT.ppm -o OUTPUT.p3tex
python tools/pixelica_pack/pixelica_pack.py bundle -o OUTPUT.p3cart NAME=PATH [NAME=PATH ...]
python tools/pixelica_pack/pixelica_pack.py inspect INPUT
```

* Exit status 0 on success, 1 for an input/limit/format error (message on
  stderr as `pixelica_pack: error: ...`), 2 for command-line usage errors.
* Every input is fully validated before anything is written. Output is
  written to a temporary file in the destination directory and renamed, so a
  failure never leaves a damaged or half-written file and never touches an
  existing output. Output may not be the same file as an input. The output
  directory must already exist.
* `inspect` prints stable, key-sorted JSON (file size, SHA-256, chunk table,
  counts, bounds, nested cart entries). It validates everything, including
  nested resources, and also accepts `.vox` files.
* `texture` accepts P6 and P3 PPM (comments allowed, `maxval` 1..65535,
  rescaled to 8 bit with rounding, then packed with the truncating RGB565
  macro). Dimensions `1..4096`; the resulting `.p3tex` must fit 16 MiB. The
  top PPM row is the top texture row. For P6, exactly one whitespace byte
  follows `maxval`; the two-byte sequence CR LF (Windows line endings) counts
  as that single delimiter. Nothing else is skipped, so a raster whose first
  byte is `0x0A`, `0x20`, `0x23` or `0x0D` is read correctly after an LF (or
  space/tab) delimiter and after CR LF. Because CR followed by LF is always read
  as CR LF, a file that ends its header with a lone CR and whose raster starts
  with `0x0A` is not supported. The raster length must match exactly (no
  truncation, no trailing bytes).
* Python 3.8 or newer is required; no third-party packages.

### Tests

```
python -I -m unittest discover -s tools/pixelica_pack/tests -v
```

Optionally verify the built-in default VOX palette against the author's
reference text (download `MagicaVoxel-file-format-vox.txt` from the
`ephtracy/voxel-model` repository yourself):

```
python tools/pixelica_pack/vox.py --check-default-palette MagicaVoxel-file-format-vox.txt
PIXELICA_PACK_VOX_REFERENCE=MagicaVoxel-file-format-vox.txt python -I -m unittest discover -s tools/pixelica_pack/tests
```

## VOX import

Supported: `VOX ` version 150 or 200 with exactly one model:

```
'VOX ' version
MAIN (content 0, children = rest of file, exact)
  [PACK = 1]
  SIZE   x y z, each 1..128
  XYZI   count (1..1 000 000), then count * (x y z i) bytes
  [RGBA] 256 colors
```

Every chunk length (including children) must be exact; the file size must
equal the sum of the parts, and is limited to 16 MiB. **Everything else is
rejected with an explicit "unsupported chunk" error** rather than being
silently dropped: scene graph chunks (`nTRN`, `nGRP`, `nSHP`), `LAYR`, `MATL`,
`MATT`, `rOBJ`, `rCAM`, `NOTE`, `IMAP`, any unknown chunk, multiple models
(`PACK != 1` or repeated `SIZE`/`XYZI`) and chunks with child chunks.

> Practical caveat: recent MagicaVoxel versions typically write
> scene-graph/layer/material chunks even for a single model, so such files
> are rejected today (not tested against real exports here). The pipeline's
> source `.vox` files in `assets/source/vox` are written by
> `tools/pixelica_pack/generate_demo_assets.py`. Supporting the scene graph is a
> deliberate later extension.

Validation also rejects: duplicate voxels, color index 0, empty models,
coordinates outside `SIZE`, truncated or excess data, and used colors whose
alpha is not 255 (the engine renders opaque colors only; unused non-opaque
palette entries are fine).

Palette: `XYZI` color index `i` (1..255) maps to `RGBA` entry `i - 1`
(entries 0..254; the last entry is ignored). Without an `RGBA` chunk the
author's default palette is used (index 0 empty; indices 1..215 the
`0xff/0xcc/0x99/0x66/0x33/0x00` color cube without its black corner;
216..255 four ten-step ramps, ending with `0xff111111`).

### Axes, scale and pivot

VOX is right handed with +Z up. The importer maps VOX `(x, y, z)` to engine
`(x, z, -y)`. This is a pure rotation (determinant +1), so handedness and
outward counter-clockwise winding are preserved. Consequences: VOX +Z is
engine up (+Y); VOX **-Y is the front** and faces engine +Z (toward the
default camera).

A voxel occupies the unit cell `[x, x+1] x [y, y+1] x [z, z+1]` before the
map. Positions are `(lattice - pivot) * scale`:

* `--scale S` (default `1/16`): world units per voxel; must be positive and
  finite. A scale that pushes any coordinate beyond binary32 is an error.
* `--pivot floor` (default): X and Z are centered on the **declared SIZE**
  (not on the occupied bounding box), Y = 0 is the bottom of SIZE.
* `--pivot center`: the center of SIZE is the origin on all three axes.

### Meshing

1. Faces between two occupied voxels are removed, even when their colors
   differ. Faces next to empty space or outside `SIZE` are kept.
2. Remaining faces are merged greedily into rectangles per direction
   (+X, -X, +Y, -Y, +Z, -Z), per plane, and **per original palette index**.
   Two different palette indices are never merged, even if they quantize to
   the same RGB565 value.
3. Vertices are shared by lattice position and numbered in order of first use
   while walking directions, planes, then rectangles in ascending order.
   Each rectangle `p0, p1, p2, p3` becomes triangles `(p0,p1,p2)` and
   `(p0,p2,p3)`, counter-clockwise from outside.
4. Each triangle color is `RGB565(palette[index])` (truncating).

The result does not depend on the order of voxels in the file. Models that
would need more than 65 535 vertices or 196 608 indices (65 536 triangles) are
rejected with a message; split them or lower the detail.

Known limitation: sharing vertices by position and merging independently per
plane leaves T-junctions (a long quad edge meets several short ones). Because
the renderer snaps screen positions to fixed point, an occasional single-pixel
crack along such an edge is possible; it has not been measured here.

## Demo assets

`python tools/pixelica_pack/generate_demo_assets.py [--output-root DIR] [--check]`
rebuilds, with the real importer/packer code paths:

* `assets/source/vox/{desk,chair,book,cup,plant}.vox` (procedural sources,
  written by `tools/pixelica_pack/demo_models.py`)
* `assets/source/textures/checker.ppm`
* `assets/compiled/{desk,chair,book,cup,plant}.p3mesh`, `checker.p3tex`,
  `room.p3cart` (resources `book=1 chair=2 checker=3 cup=4 desk=5 plant=6`)
* `assets/compiled/pixel_room_assets.h`: `static const uint8_t pixelica_asset_desk[]`,
  `pixelica_asset_chair`, `pixelica_asset_book`, `pixelica_asset_cup`, `pixelica_asset_plant`,
  `pixelica_asset_checker`, `pixelica_asset_room_cart`, each with a
  `static const size_t <symbol>_size`, plus `PIXELICA_ROOM_CART_ID_<NAME>` macros.

`--output-root` redirects all writes (for reproducibility audits); `--check`
compares in memory against disk and exits 1 on any difference. The embedded
header means the demo needs no runtime filesystem or working-directory access.
