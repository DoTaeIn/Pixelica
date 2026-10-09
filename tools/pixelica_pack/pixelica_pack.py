#!/usr/bin/env python3
"""pixelica_pack - Pixelica offline asset tool (Python standard library only).

    pixelica_pack.py import vox INPUT.vox -o OUTPUT.p3mesh [--scale S] [--pivot floor|center]
    pixelica_pack.py texture INPUT.ppm -o OUTPUT.p3tex
    pixelica_pack.py bundle -o OUTPUT.p3cart NAME=PATH [NAME=PATH ...]
    pixelica_pack.py inspect INPUT

Outputs are deterministic and written atomically after full validation; on any
error nothing is written and the exit status is non-zero.
"""

import argparse
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pixelica_format  # noqa: E402
import ppm  # noqa: E402
import vox  # noqa: E402
from pixelica_format import PixelicaPackError  # noqa: E402

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_USAGE = 2


def _positive_scale(text):
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError("not a number: %r" % text)
    if not (math.isfinite(value) and value > 0.0):
        raise argparse.ArgumentTypeError("must be a positive finite number")
    return value


def _check_distinct(inputs, output):
    for path in inputs:
        if os.path.exists(output) and os.path.exists(path) \
                and os.path.samefile(path, output):
            raise PixelicaPackError("output would overwrite the input file %s" % path)


def cmd_import_vox(args):
    _check_distinct([args.input], args.output)
    data = pixelica_format.read_limited(args.input)
    blob, mesh = vox.import_vox(data, args.scale, args.pivot)
    pixelica_format.atomic_write(args.output, blob)
    print("wrote %s: %d vertices, %d triangles, %d bytes"
          % (args.output, len(mesh.positions), len(mesh.indices) // 3, len(blob)))


def cmd_texture(args):
    _check_distinct([args.input], args.output)
    data = pixelica_format.read_limited(args.input, ppm.MAX_PPM_BYTES)
    blob = ppm.ppm_to_p3tex(data)
    pixelica_format.atomic_write(args.output, blob)
    print("wrote %s: %d bytes" % (args.output, len(blob)))


def cmd_bundle(args):
    named = []
    paths = []
    for item in args.resources:
        name, sep, path = item.partition("=")
        if not sep or not path:
            raise PixelicaPackError("resource %r must look like NAME=PATH" % item)
        named.append((name, pixelica_format.read_limited(path)))
        paths.append(path)
    _check_distinct(paths, args.output)
    blob = pixelica_format.bundle_named(named)
    pixelica_format.atomic_write(args.output, blob)
    print("wrote %s: %d resources, %d bytes" % (args.output, len(named), len(blob)))


def cmd_inspect(args):
    data = pixelica_format.read_limited(args.input)
    if data[:4] == b"VOX ":
        info = vox.describe_vox(data)
    else:
        info = pixelica_format.inspect_asset(data)
    print(json.dumps(info, indent=2, sort_keys=True))


def build_parser():
    parser = argparse.ArgumentParser(
        prog="pixelica_pack", description="Pixelica offline asset tool.")
    sub = parser.add_subparsers(dest="command", required=True)

    imp = sub.add_parser("import", help="convert an editing format to a runtime asset")
    imp_sub = imp.add_subparsers(dest="source", required=True)
    v = imp_sub.add_parser("vox", help="MagicaVoxel .vox -> .p3mesh")
    v.add_argument("input")
    v.add_argument("-o", "--output", required=True)
    v.add_argument("--scale", type=_positive_scale, default=vox.DEFAULT_SCALE,
                   help="world units per voxel (default 1/16)")
    v.add_argument("--pivot", choices=vox.PIVOTS, default="floor",
                   help="floor: horizontal center of SIZE, y=0 (default); "
                        "center: center of SIZE")
    v.set_defaults(func=cmd_import_vox)

    tex = sub.add_parser("texture", help="PPM (P6/P3) -> .p3tex RGB565")
    tex.add_argument("input")
    tex.add_argument("-o", "--output", required=True)
    tex.set_defaults(func=cmd_texture)

    bun = sub.add_parser("bundle", help="pack .p3mesh/.p3tex files into a .p3cart")
    bun.add_argument("-o", "--output", required=True)
    bun.add_argument("resources", nargs="+", metavar="NAME=PATH")
    bun.set_defaults(func=cmd_bundle)

    ins = sub.add_parser("inspect", help="print validated metadata as JSON")
    ins.add_argument("input")
    ins.set_defaults(func=cmd_inspect)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except PixelicaPackError as exc:
        print("pixelica_pack: error: %s" % exc, file=sys.stderr)
        return EXIT_ERROR
    except MemoryError:
        print("pixelica_pack: error: out of memory", file=sys.stderr)
        return EXIT_ERROR
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
