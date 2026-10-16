# Bundled GalmuriMono11 bitmap data

Copyright (c) 2019–2025 Lee Minseo (quiple@quiple.dev).
Licensed under SIL Open Font License 1.1; see OFL.txt.
Upstream: https://github.com/quiple/galmuri
Pinned commit: 71e1cacf1437a11220307120e63e30bc275312d4
Source: dist/GalmuriMono11.bdf (font version 2.404).

The original BDF and its SHA-256 are retained here. Generated fixed-cell
bitmap data is in src/game/font_korean_data.inc and keeps the OFL license.
Regenerate with `python3 tools/generate_builtin_font.py`. The generator fixes
upstream's CHARS count (excludes the final unencoded .notdef) in memory only.
No glyph design is changed. Baseline 14, left -4, cell 16x16.

20,324 encoded glyphs include all 11,172 precomposed modern Hangul syllables,
ASCII, kana and a subset of CJK. This is not universal Unicode coverage.
Combining marks, shaping and normalization are not provided by the engine.
Redistribute this notice and OFL.txt with games/SDKs using the bitmap data.
