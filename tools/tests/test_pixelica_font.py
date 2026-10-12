"""BDF -> PIXELICA_UI_Font header conversion with a tiny in-memory font."""
import contextlib
import importlib.util
import io
import os
import pathlib
import re
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = pathlib.Path(os.environ.get("PIXELICA_TOOLS_DIR") or pathlib.Path(__file__).resolve().parent.parent)
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("pixelica_font", TOOLS / "pixelica_font.py")
pf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pf)

HANGUL = "한"  # U+D55C
KANA = "あ"  # U+3042

# 8x8 cell, bbox y offset -2 => baseline row (ascent) 6.
BDF = """STARTFONT 2.1
COMMENT tiny test font
FONT -test-tiny-medium-r-normal--8-80-75-75-c-80-iso10646-1
SIZE 8 75 75
FONTBOUNDINGBOX 8 8 0 -2
STARTPROPERTIES 2
FONT_ASCENT 6
FONT_DESCENT 2
ENDPROPERTIES
CHARS 4
STARTCHAR space
ENCODING 32
SWIDTH 500 0
DWIDTH 8 0
BBX 0 0 0 0
BITMAP
ENDCHAR
STARTCHAR A
ENCODING 65
SWIDTH 500 0
DWIDTH 8 0
BBX 2 2 0 -2
BITMAP
C0
C0
ENDCHAR
STARTCHAR hangul_han
ENCODING 54620
SWIDTH 500 0
DWIDTH 8 0
BBX 4 3 1 0
BITMAP
F0
90
F0
ENDCHAR
STARTCHAR hiragana_a
ENCODING 12354
SWIDTH 500 0
DWIDTH 8 0
BBX 2 1 0 3
BITMAP
80
ENDCHAR
ENDFONT
"""


def b(text=BDF):
    return text.encode("latin-1")


def tables(header):
    """Extract (codepoints, {cp: rows}) from generated header text."""
    cps = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{4})u", header.split("_rows")[0])]
    rows = {}
    for line in re.findall(r"\{(0x[^}]*)\}, /\* U\+([0-9A-F]+) \*/", header):
        rows[int(line[1], 16)] = [int(v[2:10], 16) for v in line[0].split(", ")]
    return cps, rows


class ParseTests(unittest.TestCase):
    def test_valid_font(self):
        font = pf.parse_bdf(b())
        self.assertEqual(font.bbox, (8, 8, 0, -2))
        self.assertEqual(sorted(font.glyphs), [32, 65, 12354, 54620])

    def test_unencoded_glyph_ignored(self):
        font = pf.parse_bdf(b(BDF.replace("ENCODING 32", "ENCODING -1 5")))
        self.assertNotIn(32, font.glyphs)

    def test_crlf_accepted(self):
        pf.parse_bdf(b(BDF.replace("\n", "\r\n")))

    def test_rejects_malformed(self):
        cases = {
            "version": BDF.replace("STARTFONT 2.1", "STARTFONT 3.0"),
            "chars too many": BDF.replace("CHARS 4", "CHARS 5"),
            "chars too few": BDF.replace("CHARS 4", "CHARS 3"),
            "duplicate encoding": BDF.replace("ENCODING 65", "ENCODING 32"),
            "bad hex": BDF.replace("C0\nC0", "C0\nZZ"),
            "short row": BDF.replace("C0\nC0", "C0\nC"),
            "long row": BDF.replace("C0\nC0", "C0\nC000"),
            "missing row": BDF.replace("C0\nC0\nENDCHAR", "C0\nENDCHAR"),
            "extra row": BDF.replace("C0\nC0\nENDCHAR", "C0\nC0\nC0\nENDCHAR"),
            "padding bits": BDF.replace("C0\nC0", "C1\nC0"),
            "antialiased": BDF.replace("SIZE 8 75 75", "SIZE 8 75 75 4"),
            "vertical metrics": BDF.replace("SIZE 8 75 75", "SIZE 8 75 75\nMETRICSSET 2"),
            "dwidth1": BDF.replace("BBX 2 2 0 -2", "DWIDTH1 8 0\nBBX 2 2 0 -2"),
            "vertical dwidth": BDF.replace("DWIDTH 8 0\nBBX 2 2", "DWIDTH 8 1\nBBX 2 2"),
            "unknown header": BDF.replace("SIZE 8 75 75", "SIZE 8 75 75\nBOGUS 1"),
            "duplicate header": BDF.replace("SIZE 8 75 75", "SIZE 8 75 75\nSIZE 8 75 75"),
            "after endfont": BDF + "STARTCHAR x\n",
            "no endfont": BDF.replace("ENDFONT\n", ""),
            "no bbox": BDF.replace("FONTBOUNDINGBOX 8 8 0 -2\n", ""),
            "no font": BDF.replace("FONT -test-tiny-medium-r-normal--8-80-75-75-c-80-iso10646-1\n", ""),
            "no bbx": BDF.replace("BBX 2 2 0 -2\n", ""),
            "no encoding": BDF.replace("ENCODING 65\n", ""),
            "no dwidth": BDF.replace("DWIDTH 8 0\nBBX 2 2", "BBX 2 2"),
            "half-empty bbx": BDF.replace("BBX 2 2 0 -2", "BBX 0 2 0 -2"),
            "huge bbx": BDF.replace("BBX 2 2 0 -2", "BBX 129 2 0 -2"),
            "surrogate": BDF.replace("ENCODING 65", "ENCODING 55296"),
            "beyond unicode": BDF.replace("ENCODING 65", "ENCODING 1114112"),
            "non-integer": BDF.replace("ENCODING 65", "ENCODING 6x5"),
            "property count": BDF.replace("STARTPROPERTIES 2", "STARTPROPERTIES 3"),
            "property short": BDF.replace("STARTPROPERTIES 2", "STARTPROPERTIES 1"),
            "unterminated glyph": BDF.replace("ENDCHAR\nSTARTCHAR A", "STARTCHAR A"),
            "empty": "",
        }
        for label, text in cases.items():
            with self.subTest(label):
                with self.assertRaises(pf.FontError):
                    pf.parse_bdf(b(text))

    def test_rejects_nul_and_oversize(self):
        with self.assertRaises(pf.FontError):
            pf.parse_bdf(b(BDF) + b"\0")
        with mock.patch.object(pf, "MAX_BDF_BYTES", 10):
            with self.assertRaises(pf.FontError):
                pf.parse_bdf(b())
        with self.assertRaises(pf.FontError):
            pf.parse_bdf(b("STARTFONT 2.1\nFONT " + "x" * 5000 + "\n"))


class ConvertTests(unittest.TestCase):
    def test_used_glyphs_only_with_offsets(self):
        header = pf.convert(b(), "A %s" % HANGUL, "game_font", "tiny.bdf")
        cps, rows = tables(header)
        self.assertEqual(cps, [0x20, 0x41, 0xD55C])  # KANA is unused, so absent
        self.assertEqual(rows[0x20], [0] * 8)
        self.assertEqual(rows[0x41], [0, 0, 0, 0, 0, 0, 0xC0000000, 0xC0000000])
        # x offset 1 shifts the 4-pixel glyph right; y offset 0 sits above the baseline.
        self.assertEqual(rows[0xD55C], [0, 0, 0, 0x78000000, 0x48000000, 0x78000000, 0, 0])
        self.assertNotIn("U+3042", header)

    def test_positive_y_offset_japanese(self):
        _, rows = tables(pf.convert(b(), KANA, "ja_font"))
        self.assertEqual(rows[0x3042], [0, 0, 0x80000000, 0, 0, 0, 0, 0])

    def test_header_api_and_names(self):
        header = pf.convert(b(), "A", "game_font", "ti*/ny.bdf")
        for text in ("#ifndef PIXELICA_FONT_GAME_FONT_H", '#include "pixelica/ui.h"',
                     "#define GAME_FONT_GLYPH_COUNT 1u", "#define GAME_FONT_WIDTH 8u",
                     "#define GAME_FONT_HEIGHT 8u",
                     "static inline bool game_font_glyph(void *userdata, uint32_t codepoint, uint32_t rows[32])",
                     "static inline PIXELICA_UI_Font game_font_font(uint8_t scale)",
                     "font.glyph = game_font_glyph;", "Source font: ti__ny.bdf."):
            self.assertIn(text, header)

    def test_deterministic_and_order_independent(self):
        a = pf.convert(b(), "A%s%s" % (HANGUL, KANA), "f")
        self.assertEqual(a, pf.convert(b(), "%s%sA\n\nA" % (KANA, HANGUL), "f"))
        self.assertEqual(a, pf.convert(b(), "A%s%s" % (HANGUL, KANA), "f"))
        self.assertEqual(tables(a)[0], [0x41, 0x3042, 0xD55C])

    def test_cell_and_ascent_overrides(self):
        with self.assertRaises(pf.FontError):  # Hangul pixel at x=4 does not fit 3 wide
            pf.convert(b(), HANGUL, "f", cell=(3, 8))
        _, rows = tables(pf.convert(b(), HANGUL, "f", cell=(8, 4), ascent=3))
        self.assertEqual(rows[0xD55C], [0x78000000, 0x48000000, 0x78000000, 0])
        with self.assertRaises(pf.FontError):  # ascent 2 pushes the glyph above the cell
            pf.convert(b(), HANGUL, "f", ascent=2)
        _, rows = tables(pf.convert(b(), HANGUL, "f", left=1))
        self.assertEqual(rows[0xD55C][3], 0xF0000000)

    def test_cell_limits(self):
        wide = BDF.replace("FONTBOUNDINGBOX 8 8 0 -2", "FONTBOUNDINGBOX 20 8 0 -2")
        with self.assertRaises(pf.FontError):
            pf.convert(b(wide), "A", "f")
        pf.convert(b(wide), "A", "f", cell=(16, 8))
        tall = BDF.replace("FONTBOUNDINGBOX 8 8 0 -2", "FONTBOUNDINGBOX 8 33 0 -2")
        with self.assertRaises(pf.FontError):
            pf.convert(b(tall), "A", "f")
        with self.assertRaises(pf.FontError):
            pf.convert(b(), "A", "f", cell=(8, 33))
        with self.assertRaises(pf.FontError):
            pf.convert(b(), "A", "f", cell=(0, 8))

    def test_missing_glyphs(self):
        with self.assertRaises(pf.FontError) as ctx:
            pf.convert(b(), "A語", "f")
        self.assertIn("U+8A9E", str(ctx.exception))
        cps, _ = tables(pf.convert(b(), "A語", "f", allow_missing=True))
        self.assertEqual(cps, [0x41])
        with self.assertRaises(pf.FontError):  # nothing left to emit
            pf.convert(b(), "語", "f", allow_missing=True)

    def test_include_ascii(self):
        with self.assertRaises(pf.FontError):
            pf.convert(b(), "A", "f", include_ascii=True)
        cps, _ = tables(pf.convert(b(), "", "f", include_ascii=True, allow_missing=True))
        self.assertEqual(cps, [0x20, 0x41])

    def test_text_validation(self):
        for bad in ("a\tb", "a\x00b", "a\x7fb", "a\x85b"):
            with self.subTest(bad=repr(bad)):
                with self.assertRaises(pf.FontError):
                    pf.convert(b(), bad, "f", allow_missing=True)
        with self.assertRaises(pf.FontError):
            pf.convert(b(), "\n\r\n", "f")

    def test_bad_names(self):
        for name in ("", "Game", "1font", "a-b", "a__b", "a" * 49, "_x", "a b"):
            with self.subTest(name=name):
                with self.assertRaises(pf.FontError):
                    pf.convert(b(), "A", name)


class CliTests(unittest.TestCase):
    def run_cli(self, *argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = pf.main([str(a) for a in argv])
        return code, err.getvalue()

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = pathlib.Path(self._tmp.name)
        self.bdf = self.dir / "tiny.bdf"
        self.bdf.write_bytes(b())
        self.text = self.dir / "strings.txt"
        self.text.write_text("A %s\n%s\n" % (HANGUL, KANA), encoding="utf-8")
        self.out = self.dir / "font.h"

    def args(self, **over):
        values = {"bdf": self.bdf, "text": self.text, "output": self.out, "name": "game_font"}
        values.update(over)
        return ["--%s=%s" % (k, v) for k, v in values.items()]

    def test_success_is_deterministic(self):
        self.assertEqual(self.run_cli(*self.args())[0], 0)
        first = self.out.read_bytes()
        self.assertEqual(first.decode("utf-8"),
                         pf.convert(b(), "A %s\n%s\n\n" % (HANGUL, KANA), "game_font", "tiny.bdf"))
        self.assertEqual(self.run_cli(*self.args())[0], 0)
        self.assertEqual(self.out.read_bytes(), first)
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()),
                         ["font.h", "strings.txt", "tiny.bdf"])

    def test_bom_text_and_multiple_text_files(self):
        other = self.dir / "more.txt"
        other.write_bytes(b"\xef\xbb\xbfA")
        self.text.write_bytes(b"\xef\xbb\xbf" + HANGUL.encode("utf-8"))
        code, _ = self.run_cli(*self.args(), "--text=%s" % other)
        self.assertEqual(code, 0)
        self.assertEqual(tables(self.out.read_text("utf-8"))[0], [0x41, 0xD55C])

    def test_errors_preserve_existing_output(self):
        self.out.write_text("keep me\n")
        broken = self.dir / "broken.bdf"
        broken.write_bytes(b(BDF.replace("CHARS 4", "CHARS 5")))
        bad_text = self.dir / "bad.txt"
        bad_text.write_bytes(b"\xff\xfe")
        missing = self.dir / "missing.txt"
        missing.write_text("語", encoding="utf-8")
        for label, over in (("bdf", {"bdf": broken}), ("utf8", {"text": bad_text}),
                            ("glyph", {"text": missing}), ("name", {"name": "Bad"}),
                            ("no bdf", {"bdf": self.dir / "nope.bdf"}),
                            ("no text", {"text": self.dir / "nope.txt"})):
            with self.subTest(label):
                code, err = self.run_cli(*self.args(**over))
                self.assertEqual(code, 1)
                self.assertIn("error:", err)
                self.assertEqual(self.out.read_text(), "keep me\n")
        self.assertEqual(len(list(self.dir.glob(".pixelica_font.*"))), 0)

    def test_output_path_errors(self):
        code, err = self.run_cli(*self.args(output=self.dir / "no_such_dir" / "font.h"))
        self.assertEqual(code, 1)
        self.assertIn("does not exist", err)
        (self.dir / "isdir").mkdir()
        code, err = self.run_cli(*self.args(output=self.dir / "isdir"))
        self.assertEqual(code, 1)
        self.assertIn("directory", err)
        for target in (self.bdf, self.text):
            before = target.read_bytes()
            code, err = self.run_cli(*self.args(output=target))
            self.assertEqual(code, 1)
            self.assertIn("also an input", err)
            self.assertEqual(target.read_bytes(), before)

    def test_write_failure_keeps_output_and_cleans_temp(self):
        self.out.write_text("keep me\n")
        with mock.patch.object(pf.os, "replace", side_effect=OSError("boom")):
            code, err = self.run_cli(*self.args())
        self.assertEqual(code, 1)
        self.assertIn("boom", err)
        self.assertEqual(self.out.read_text(), "keep me\n")
        self.assertEqual(len(list(self.dir.glob(".pixelica_font.*"))), 0)

    def test_flags(self):
        self.text.write_text("A語", encoding="utf-8")
        self.assertEqual(self.run_cli(*self.args())[0], 1)
        self.assertEqual(self.run_cli(*self.args(), "--allow-missing", "--include-ascii",
                                      "--cell=8x8", "--ascent=6", "--left=0")[0], 0)
        self.assertEqual(tables(self.out.read_text("utf-8"))[0], [0x20, 0x41])


if __name__ == "__main__":
    unittest.main()
