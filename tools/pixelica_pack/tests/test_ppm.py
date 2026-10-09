import struct
import unittest

import support  # noqa: F401  (sets sys.path)

import pixelica_format
import ppm
from pixelica_format import PixelicaPackError


def p6(width, height, samples, maxval=255):
    head = ("P6\n%d %d\n%d\n" % (width, height, maxval)).encode("ascii")
    return head + bytes(samples)


class PpmTests(unittest.TestCase):
    def test_p6_quantizes_like_the_c_macro(self):
        tex = ppm.parse_ppm(p6(3, 1, [255, 0, 0, 0, 255, 0, 8, 4, 8]))
        self.assertEqual((tex.width, tex.height), (3, 1))
        self.assertEqual(list(tex.pixels), [0xF800, 0x07E0, 0x0821])

    def test_top_row_first(self):
        tex = ppm.parse_ppm(p6(1, 2, [255, 0, 0, 0, 0, 255]))
        self.assertEqual(list(tex.pixels), [0xF800, 0x001F])

    def test_p3_with_comments_matches_p6(self):
        ascii_ppm = (b"P3 # magic\n2 # width\n1\n255\n"
                     b"255 0 0\n0 0 # inline comment\n255\n")
        self.assertEqual(list(ppm.parse_ppm(ascii_ppm).pixels),
                         list(ppm.parse_ppm(p6(2, 1, [255, 0, 0, 0, 0, 255])).pixels))

    def test_maxval_scaling(self):
        tex = ppm.parse_ppm(p6(1, 1, [1, 0, 1], maxval=1))
        self.assertEqual(list(tex.pixels), [0xF81F])
        tex = ppm.parse_ppm(p6(1, 1, [100, 50, 0], maxval=100))
        self.assertEqual(list(tex.pixels)[0] >> 11, 31)

    def test_sixteen_bit_samples(self):
        raster = struct.pack(">HHH", 65535, 0, 65535)
        tex = ppm.parse_ppm(b"P6\n1 1\n65535\n" + raster)
        self.assertEqual(list(tex.pixels), [0xF81F])

    def test_ppm_to_p3tex_roundtrip(self):
        blob = ppm.ppm_to_p3tex(p6(2, 1, [255, 0, 0, 0, 0, 255]))
        tex = pixelica_format.decode_texture(blob)
        self.assertEqual(list(tex.pixels), [0xF800, 0x001F])
        self.assertEqual(blob, ppm.ppm_to_p3tex(p6(2, 1, [255, 0, 0, 0, 0, 255])))

    def reject(self, data, text=None):
        with self.assertRaises(PixelicaPackError) as ctx:
            ppm.parse_ppm(data)
        if text:
            self.assertIn(text, str(ctx.exception))

    def test_bad_magic(self):
        for data in (b"", b"P5\n1 1\n255\n\0", b"P7\n", b"hello"):
            self.reject(data, "magic")

    def test_bad_dimensions(self):
        for head in (b"P6\n0 1\n255\n", b"P6\n1 0\n255\n", b"P6\n4097 1\n255\n",
                     b"P6\n1 4097\n255\n", b"P6\n-1 1\n255\n", b"P6\nabc 1\n255\n",
                     b"P6\n1\n", b"P6\n99999999999 1\n255\n"):
            self.reject(head + bytes(64))

    def test_bad_maxval(self):
        for maxval in (b"0", b"65536", b"-1", b"x"):
            self.reject(b"P6\n1 1\n" + maxval + b"\n\0\0\0")

    def test_output_size_limit_checked_before_reading_pixels(self):
        self.reject(b"P6\n4096 4096\n255\n", "limit")
        self.reject(b"P6\n4096 4000\n255\n", "limit")

    def test_truncated_and_excess_p6(self):
        self.reject(p6(2, 1, [0] * 5), "truncated")
        self.reject(p6(2, 1, [0] * 7), "trailing")
        self.reject(b"P6\n1 1\n255", "whitespace")
        self.reject(b"P6\n1 1\n255\n", "truncated")

    def test_p6_sample_above_maxval(self):
        self.reject(p6(1, 1, [101, 0, 0], maxval=100), "exceeds maxval")

    def test_p3_errors(self):
        self.reject(b"P3\n1 1\n255\n1 2\n", "truncated")
        self.reject(b"P3\n1 1\n255\n1 2 3 4\n", "extra")
        self.reject(b"P3\n1 1\n255\n1 2 300\n", "exceeds maxval")
        for token in (b"+5", b"1_0", b"0x1", b"-1", b"1.0"):
            self.reject(b"P3\n1 1\n255\n1 2 " + token + b"\n", "invalid sample")

    def test_every_truncation_is_rejected(self):
        data = p6(2, 2, list(range(12)))
        for cut in range(len(data)):
            with self.assertRaises(PixelicaPackError):
                ppm.parse_ppm(data[:cut])

    # ---- CRLF headers (Windows tools) and whitespace-valued raster bytes ----

    def test_p6_crlf_header(self):
        tex = ppm.parse_ppm(b"P6\r\n1 1\r\n255\r\n" + bytes([255, 0, 0]))
        self.assertEqual(list(tex.pixels), [0xF800])

    def test_p6_crlf_header_with_comments_and_16_bit(self):
        head = b"P6\r\n# made on windows\r\n2 1\r\n# more\r\n65535\r\n"
        raster = struct.pack(">HHHHHH", 65535, 0, 0, 0, 0, 65535)
        tex = ppm.parse_ppm(head + raster)
        self.assertEqual(list(tex.pixels), [0xF800, 0x001F])

    def test_p3_crlf(self):
        tex = ppm.parse_ppm(b"P3\r\n# c\r\n1 1\r\n255\r\n255 0 0\r\n")
        self.assertEqual(list(tex.pixels), [0xF800])

    def test_lf_header_keeps_whitespace_valued_first_raster_byte(self):
        # Only the single LF delimiter is consumed; the raster byte stays.
        for first in (0x0A, 0x20, 0x23, 0x0D, 0x09):
            with self.subTest(first=first):
                tex = ppm.parse_ppm(b"P6\n1 1\n255\n" + bytes([first, 0, 0]))
                self.assertEqual(list(tex.pixels),
                                 [pixelica_format.rgb565(first, 0, 0)])
        # CR then LF as raster *content* after a lone LF delimiter.
        tex = ppm.parse_ppm(b"P6\n1 1\n255\n" + bytes([0x0D, 0x0A, 0x20]))
        self.assertEqual(list(tex.pixels), [pixelica_format.rgb565(0x0D, 0x0A, 0x20)])

    def test_crlf_header_keeps_whitespace_valued_first_raster_byte(self):
        # CRLF is one delimiter; the following raster byte is never skipped.
        for first in (0x0A, 0x20, 0x23, 0x0D, 0x09):
            with self.subTest(first=first):
                tex = ppm.parse_ppm(b"P6\r\n1 1\r\n255\r\n"
                                    + bytes([first, 0, 0]))
                self.assertEqual(list(tex.pixels),
                                 [pixelica_format.rgb565(first, 0, 0)])
        tex = ppm.parse_ppm(b"P6\r\n1 1\r\n255\r\n" + bytes([0x0D, 0x0A, 0x20]))
        self.assertEqual(list(tex.pixels), [pixelica_format.rgb565(0x0D, 0x0A, 0x20)])

    def test_other_single_whitespace_delimiters_consume_one_byte(self):
        for ws in (b" ", b"\t", b"\r", b"\n", b"\x0b", b"\x0c"):
            with self.subTest(ws=ws):
                tex = ppm.parse_ppm(b"P6\n1 1\n255" + ws + bytes([0x20, 8, 8]))
                self.assertEqual(list(tex.pixels),
                                 [pixelica_format.rgb565(0x20, 8, 8)])

    def test_two_whitespace_bytes_other_than_crlf_are_not_swallowed(self):
        # LF LF: the second LF is raster data, so a 3-byte raster is 4 long.
        self.reject(b"P6\n1 1\n255\n\n" + bytes([1, 2, 3]), "trailing")
        self.reject(b"P6\n1 1\n255 \n" + bytes([1, 2, 3]), "trailing")
        self.reject(b"P6\n1 1\n255\n\r\n" + bytes([1, 2, 3]), "trailing")

    def test_crlf_raster_validation_stays_exact(self):
        head = b"P6\r\n2 1\r\n255\r\n"
        good = head + bytes(range(6))
        ppm.parse_ppm(good)
        self.reject(good + b"\0", "trailing")
        self.reject(head + bytes(5), "truncated")
        self.reject(head + bytes(7), "trailing")
        self.reject(head + b"\n" + bytes(6), "trailing")
        self.reject(head[:-2], "whitespace")
        for cut in range(len(good)):
            with self.assertRaises(PixelicaPackError, msg="cut=%d" % cut):
                ppm.parse_ppm(good[:cut])

    def test_crlf_and_lf_files_give_identical_textures(self):
        raster = bytes([0x0A, 0x20, 0x23, 0x0D, 0x00, 0xFF])
        lf = ppm.parse_ppm(b"P6\n2 1\n255\n" + raster)
        crlf = ppm.parse_ppm(b"P6\r\n2 1\r\n255\r\n" + raster)
        self.assertEqual(list(lf.pixels), list(crlf.pixels))
        self.assertEqual(ppm.ppm_to_p3tex(b"P6\n2 1\n255\n" + raster),
                         ppm.ppm_to_p3tex(b"P6\r\n2 1\r\n255\r\n" + raster))


if __name__ == "__main__":
    unittest.main()
