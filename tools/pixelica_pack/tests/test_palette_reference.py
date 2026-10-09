import hashlib
import itertools
import os
import struct
import unittest

import support
from support import vox_file, vox_size, vox_xyzi

import vox
from pixelica_format import rgb565

# SHA-256 of struct.pack("<256I", *words) for the 256 0xAABBGGRR words of the
# author's default_palette table (MagicaVoxel-file-format-vox.txt).
DEFAULT_PALETTE_SHA256 = (
    "cc9800abac4eea5f0dc2b29399d7d6440165026723622ca30ffd77dd71653605")

AUTHOR_REFERENCE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
    ".claude-work", "vox-author-format.txt")

LEVELS = (0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00)
RAMP = (0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11)


def expected_words():
    """The author's table, built from its layout: empty, 215 cube colors (the
    black corner is absent), then red, green, blue and gray ramps."""
    words = [0x00000000]
    for r, g, b in itertools.product(LEVELS, repeat=3):
        if (r, g, b) != (0, 0, 0):
            words.append(0xFF000000 | (b << 16) | (g << 8) | r)
    for v in RAMP:
        words.append(0xFF000000 | v)
    for v in RAMP:
        words.append(0xFF000000 | (v << 8))
    for v in RAMP:
        words.append(0xFF000000 | (v << 16))
    for v in RAMP:
        words.append(0xFF000000 | (v << 16) | (v << 8) | v)
    return words


class DefaultPaletteReferenceTests(unittest.TestCase):
    def test_full_palette_hash(self):
        payload = struct.pack("<256I", *vox.default_palette_words())
        self.assertEqual(len(payload), 1024)
        self.assertEqual(hashlib.sha256(payload).hexdigest(),
                         DEFAULT_PALETTE_SHA256)

    def test_every_entry_matches_independent_layout(self):
        expected = expected_words()
        self.assertEqual(len(expected), 256)
        self.assertEqual(vox.default_palette_words(), expected)

    def test_cube_has_215_colors_without_black(self):
        words = vox.default_palette_words()
        self.assertNotIn(0xFF000000, words)
        self.assertEqual(len(set(words[1:216])), 215)
        self.assertEqual(words[215], 0xFF330000)
        self.assertEqual(words[216], 0xFF0000EE)

    def test_explicit_indices(self):
        pal = vox.DEFAULT_PALETTE
        self.assertEqual(pal[215], (0x00, 0x00, 0x33, 255))   # 0xff330000
        self.assertEqual(pal[216], (0xEE, 0x00, 0x00, 255))   # 0xff0000ee
        self.assertEqual(pal[255], (0x11, 0x11, 0x11, 255))   # 0xff111111

    def _mesh_colors(self, index):
        data = vox_file([vox_size(1, 1, 1), vox_xyzi([(0, 0, 0, index)])])
        model = vox.parse_vox(data)
        self.assertFalse(model.custom_palette)
        return set(vox.build_mesh(model, 1.0).colors)

    def test_import_without_rgba_uses_author_colors(self):
        self.assertEqual(self._mesh_colors(215), {rgb565(0x00, 0x00, 0x33)})
        self.assertEqual(self._mesh_colors(216), {0xE800})
        self.assertEqual(self._mesh_colors(255), {rgb565(0x11, 0x11, 0x11)})

    @unittest.skipUnless(os.path.isfile(AUTHOR_REFERENCE),
                         "author reference text not present")
    def test_matches_author_reference_file(self):
        with open(AUTHOR_REFERENCE, "r", encoding="utf-8",
                  errors="replace") as handle:
            self.assertEqual(vox.check_default_palette(handle.read()), [])


if __name__ == "__main__":
    unittest.main()
