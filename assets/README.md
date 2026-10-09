# assets

Everything below is *generated* by `tools/pixelica_pack/generate_demo_assets.py`
(Python standard library only). Run it from any directory:

```
python tools/pixelica_pack/generate_demo_assets.py            # write into this repository
python tools/pixelica_pack/generate_demo_assets.py --check    # compare, write nothing
python tools/pixelica_pack/generate_demo_assets.py --output-root /tmp/audit   # elsewhere
```

| Path | Content |
|------|---------|
| `source/vox/*.vox` | procedural VOX sources: desk, chair, book, cup, plant |
| `source/textures/checker.ppm` | procedural floor texture (P6) |
| `compiled/*.p3mesh`, `checker.p3tex`, `room.p3cart` | outputs of the real `pixelica_pack` importer/bundler code |
| `compiled/pixel_room_assets.h` | the same bytes as C arrays (`pixelica_asset_*`, `pixelica_asset_*_size`) |

Until the generator has been run these files do not exist. Formats and
conventions: `docs/asset_format.md`, `docs/art_style.md`.
