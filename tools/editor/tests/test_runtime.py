"""Tests for pixelica_editor.runtime (native Renderer2D and PlaySession).

Renderer tests need the built bridge library. They are skipped when it cannot
be found, unless PIXELICA_EDITOR_REQUIRE_NATIVE=1 (launch_editor.py --test sets
it), in which case a missing library is a failure.
"""

from __future__ import annotations

import math
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from PIL import Image  # noqa: E402

from pixelica_editor import runtime  # noqa: E402
from pixelica_editor.model import Project, SceneObject, Transform  # noqa: E402

REQUIRE_NATIVE = os.environ.get("PIXELICA_EDITOR_REQUIRE_NATIVE") == "1"


def native_available() -> bool:
    try:
        runtime.find_library()
        return True
    except runtime.NativeLibraryError:
        return False


def q565(rgb: tuple[int, int, int]) -> tuple[int, int, int]:
    """Expected RGB24 after the engine's RGB565 round trip."""
    r, g, b = rgb[0] >> 3, rgb[1] >> 2, rgb[2] >> 3
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def sprite(obj_id: str, x: float, y: float, w: float = 4, h: float = 4, color: str = "#ffffff", *,
           opacity: int = 255, layer: int = 0, rot: float = 0.0, scale=(1.0, 1.0), asset=None,
           visible: bool = True, motion: dict | None = None, kind: str = "sprite2d") -> SceneObject:
    components = {"sprite2d": {"width": float(w), "height": float(h), "color": color, "asset": asset,
                               "opacity": opacity, "layer": layer}}
    if motion is not None:
        components["motion2d"] = motion
    return SceneObject(id=obj_id, name=obj_id, kind=kind,
                       transform=Transform(position=[float(x), float(y), 0.0], rotation=[0.0, 0.0, float(rot)],
                                           scale=[float(scale[0]), float(scale[1]), 1.0]),
                       visible=visible, components=components)


def project(objects, width: int = 32, height: int = 24, background: str = "#000000", path=None) -> Project:
    return Project(name="test", width=width, height=height, background=background, objects=list(objects),
                   path=path)


class LibraryDiscoveryTests(unittest.TestCase):
    def test_explicit_missing_path_raises(self):
        with self.assertRaises(runtime.NativeLibraryError):
            runtime.find_library(Path(tempfile.gettempdir()) / "definitely_missing_bridge.dll")

    def test_candidate_order(self):
        old = os.environ.pop("PIXELICA_EDITOR_LIBRARY", None)
        try:
            explicit = Path("x") / runtime.library_filename()
            self.assertEqual(runtime.library_candidates(explicit), [explicit])
            names = [p.parent.name for p in runtime.library_candidates()]
            self.assertEqual(names, ["Release", "build-editor", "native"])
            os.environ["PIXELICA_EDITOR_LIBRARY"] = str(Path("env") / "lib")
            self.assertEqual(runtime.library_candidates(), [Path("env") / "lib"])
        finally:
            os.environ.pop("PIXELICA_EDITOR_LIBRARY", None)
            if old is not None:
                os.environ["PIXELICA_EDITOR_LIBRARY"] = old

    def test_no_fallback_without_library(self):
        with self.assertRaises(runtime.NativeLibraryError):
            runtime.Renderer2D(16, 16, library_path=Path(tempfile.gettempdir()) / "missing_bridge.so")


@unittest.skipUnless(native_available() or REQUIRE_NATIVE, "native bridge not built")
class Renderer2DTests(unittest.TestCase):
    def setUp(self):
        self.renderer = runtime.Renderer2D(32, 24)
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.renderer.close()
        self.tmp.cleanup()

    def saved_project(self, objects, **kw) -> Project:
        return project(objects, path=self.root / "game.pixelica.json", **kw)

    def write_png(self, name: str, pixels: list[tuple[int, int, int, int]], size=(2, 2)) -> str:
        img = Image.new("RGBA", size)
        img.putdata(pixels)
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        img.save(target, "PNG")
        return name

    def test_background_and_output_format(self):
        img = self.renderer.render(project([], background="#17212c"))
        self.assertEqual(img.mode, "RGB")
        self.assertEqual(img.size, (32, 24))
        self.assertEqual(img.getpixel((0, 0)), q565((0x17, 0x21, 0x2C)))
        self.assertTrue(self.renderer.library_path.is_file())

    def test_rect_position_scale_and_visibility(self):
        img = self.renderer.render(project([
            sprite("a", 2, 3, 4, 2, "#00ff00", scale=(2.0, 1.5)),
            sprite("hidden", 20, 20, 4, 4, "#ff0000", visible=False),
        ]))
        self.assertEqual(img.getpixel((2, 3)), (0, 255, 0))
        self.assertEqual(img.getpixel((9, 5)), (0, 255, 0))   # 4*2 wide, 2*1.5 tall
        self.assertEqual(img.getpixel((10, 3)), (0, 0, 0))
        self.assertEqual(img.getpixel((2, 6)), (0, 0, 0))
        self.assertEqual(img.getpixel((21, 21)), (0, 0, 0))

    def test_layer_then_index_order(self):
        img = self.renderer.render(project([
            sprite("top", 0, 0, 8, 8, "#ff0000", layer=2),
            sprite("bottom", 0, 0, 8, 8, "#0000ff", layer=1),
            sprite("same1", 10, 0, 8, 8, "#00ff00", layer=0),
            sprite("same2", 10, 0, 8, 8, "#ffffff", layer=0),
        ]))
        self.assertEqual(img.getpixel((4, 4)), (255, 0, 0))
        self.assertEqual(img.getpixel((14, 4)), (255, 255, 255))

    def test_opacity_and_rotation(self):
        img = self.renderer.render(project([
            sprite("half", 0, 0, 4, 4, "#ffffff", opacity=128),
            sprite("bar", 20, 10, 10, 2, "#ff0000", rot=90.0),
            sprite("zero", 0, 10, 4, 4, "#ffffff", opacity=0),
        ]))
        self.assertEqual(img.getpixel((1, 1)), (132, 130, 132))
        self.assertEqual(img.getpixel((19, 15)), (255, 0, 0))  # rotated clockwise around top-left
        self.assertEqual(img.getpixel((25, 10)), (0, 0, 0))
        self.assertEqual(img.getpixel((1, 11)), (0, 0, 0))

    def test_invalid_values_are_rejected_cleanly(self):
        bad = [
            sprite("nan", float("nan"), 0),
            sprite("inf", 0, 0, rot=float("inf")),
            sprite("zero_scale", 0, 0, scale=(0.0, 1.0)),
            sprite("opacity", 0, 0, opacity=300),
            sprite("color", 0, 0, color="red"),
            sprite("far", 1e12, 0),
        ]
        bool_opacity = sprite("bool", 0, 0)
        bool_opacity.components["sprite2d"]["opacity"] = True
        bad.append(bool_opacity)
        huge_position = sprite("huge", 0, 0)
        huge_position.transform.position[0] = 10 ** 400
        bad.append(huge_position)
        for obj in bad:
            with self.subTest(obj=obj.id), self.assertRaises(runtime.RendererError):
                self.renderer.render(project([obj]))
        with self.assertRaises(runtime.RendererError):
            self.renderer.render(project([], background="#12345"))
        with self.assertRaises(runtime.RendererError):
            self.renderer.render(project([], width=0))
        with self.assertRaises(runtime.RendererError):
            self.renderer.render(project([], width=2048, height=1024))
        # Still usable afterwards.
        self.assertEqual(self.renderer.render(project([sprite("ok", 0, 0, color="#ff0000")])).getpixel((0, 0)),
                         (255, 0, 0))

    def test_unsupported_kind_refused_unless_hidden(self):
        with self.assertRaisesRegex(runtime.RendererError, "unsupported kind"):
            self.renderer.render(project([sprite("m", 0, 0, kind="mesh3d")]))
        self.renderer.render(project([sprite("m", 0, 0, kind="mesh3d", visible=False)]))

    def test_model_valid_large_sprite_is_renderable(self):
        p = project([sprite("large", 0, 0, 16384, 1, scale=(10, 1))]).validate()
        self.assertEqual(self.renderer.render(p).getpixel((1, 0)), (255, 255, 255))

    def test_resize_on_dimension_change(self):
        img = self.renderer.render(project([], width=64, height=16))
        self.assertEqual(img.size, (64, 16))
        self.assertEqual((self.renderer.width, self.renderer.height), (64, 16))

    def test_constructor_dimension_limits(self):
        for w, h in ((0, 10), (10, 0), (4096, 10), (2048, 1024), (True, 10)):
            with self.subTest(w=w, h=h), self.assertRaises(runtime.RendererError):
                runtime.Renderer2D(w, h)

    def test_close_is_idempotent(self):
        r = runtime.Renderer2D(8, 8)
        r.close()
        r.close()
        with self.assertRaises(runtime.RendererError):
            r.render(project([], width=8, height=8))

    # -- assets ------------------------------------------------------------
    def test_image_alpha_and_tint(self):
        name = self.write_png("assets/img.png", [(255, 255, 255, 255), (255, 255, 255, 0),
                                                 (0, 0, 255, 255), (255, 255, 255, 128)])
        p = self.saved_project([sprite("img", 0, 0, 4, 4, "#ffffff", asset=name)], background="#000000")
        img = self.renderer.render(p)
        self.assertEqual(img.getpixel((0, 0)), (255, 255, 255))
        self.assertEqual(img.getpixel((2, 0)), (0, 0, 0))     # transparent texel
        self.assertEqual(img.getpixel((0, 2)), (0, 0, 255))
        self.assertEqual(img.getpixel((2, 2)), (132, 130, 132))
        p.objects[0].components["sprite2d"]["color"] = "#ff0000"
        img = self.renderer.render(p)
        self.assertEqual(img.getpixel((0, 0)), (255, 0, 0))   # tinted
        self.assertEqual(img.getpixel((0, 2)), (0, 0, 0))     # blue * red = black
        p.objects[0].components["sprite2d"]["opacity"] = 128
        p.objects[0].components["sprite2d"]["color"] = "#ffffff"
        img = self.renderer.render(p)
        self.assertEqual(img.getpixel((0, 0)), (132, 130, 132))

    def test_asset_reload_on_change(self):
        name = self.write_png("a.png", [(255, 0, 0, 255)] * 4)
        p = self.saved_project([sprite("img", 0, 0, 2, 2, asset=name)])
        self.assertEqual(self.renderer.render(p).getpixel((0, 0)), (255, 0, 0))
        st = (self.root / name).stat()
        self.write_png("a.png", [(0, 0, 255, 255)] * 4)
        os.utime(self.root / name, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
        self.assertEqual(self.renderer.render(p).getpixel((0, 0)), (0, 0, 255))
        self.assertEqual(self.renderer.texture_count, 1)

    def test_bad_assets_are_recoverable(self):
        (self.root / "junk.png").write_bytes(b"not an image")
        Image.new("RGBA", (4097, 1)).save(self.root / "wide.png")
        outside = Path(self.tmp.name).parent / "outside_asset_test.png"
        cases = ["missing.png", "junk.png", "wide.png", "../outside_asset_test.png", str(outside),
                 "notes.txt", "C:/x.png", "/abs.png", "sub/../a.png"]
        for asset in cases:
            with self.subTest(asset=asset), self.assertRaises(runtime.AssetError):
                self.renderer.render(self.saved_project([sprite("img", 0, 0, asset=asset)]))
        with self.assertRaises(runtime.AssetError):
            self.renderer.render(project([sprite("img", 0, 0, asset="a.png")]))  # unsaved project
        self.assertEqual(self.renderer.render(project([sprite("ok", 0, 0, color="#00ff00")])).getpixel((0, 0)),
                         (0, 255, 0))

    def test_symlink_escape_rejected(self):
        outside = tempfile.TemporaryDirectory()
        self.addCleanup(outside.cleanup)
        Image.new("RGBA", (1, 1), (255, 0, 0, 255)).save(Path(outside.name) / "x.png")
        try:
            os.symlink(outside.name, self.root / "link", target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks not permitted here")
        with self.assertRaises(runtime.AssetError):
            self.renderer.render(self.saved_project([sprite("img", 0, 0, asset="link/x.png")]))

    def test_texture_cache_is_bounded(self):
        objects = []
        for i in range(runtime.TEXTURE_CACHE_ENTRIES + 6):
            name = self.write_png(f"t{i}.png", [(i % 256, 0, 0, 255)], size=(1, 1))
            objects.append(sprite(f"s{i}", 0, 0, 1, 1, asset=name))
        for obj in objects:  # render each separately so all of them pass through the cache
            self.renderer.render(self.saved_project([obj]))
        self.assertLessEqual(self.renderer.texture_count, runtime.TEXTURE_CACHE_ENTRIES)
        img = self.renderer.render(self.saved_project([objects[0]]))
        self.assertEqual(img.getpixel((0, 0)), q565((0, 0, 0)))


class PlaySessionTests(unittest.TestCase):
    def make(self):
        player = sprite("player", 10, 10, motion={"controlled": True, "speed": 60.0, "velocity": [0.0, 0.0]})
        drifter = sprite("drift", 0, 0, motion={"controlled": False, "speed": 80.0, "velocity": [30.0, -60.0]})
        still = sprite("still", 5, 5)
        return project([player, drifter, still])

    def test_does_not_mutate_source(self):
        src = self.make()
        session = runtime.PlaySession(src)
        for _ in range(60):
            session.step(runtime.FIXED_DT, {"d"})
        self.assertEqual(session.ticks, 60)
        self.assertEqual(src.objects[0].transform.position[:2], [10.0, 10.0])
        self.assertEqual(src.objects[1].transform.position[:2], [0.0, 0.0])
        self.assertIsNot(session.project, src)

    def test_velocity_and_controlled_movement(self):
        session = runtime.PlaySession(self.make())
        for _ in range(60):
            session.step(runtime.FIXED_DT, {"D"})
        player, drifter, still = session.project.objects
        self.assertAlmostEqual(player.transform.position[0], 70.0, places=6)
        self.assertAlmostEqual(player.transform.position[1], 10.0, places=6)
        self.assertAlmostEqual(drifter.transform.position[0], 30.0, places=6)
        self.assertAlmostEqual(drifter.transform.position[1], -60.0, places=6)
        self.assertEqual(still.transform.position[:2], [5.0, 5.0])

    def test_diagonal_normalized_and_arrows(self):
        session = runtime.PlaySession(self.make())
        session.step(0.1, {"Up", "Left"})
        p = session.project.objects[0].transform.position
        self.assertAlmostEqual(math.hypot(p[0] - 10.0, p[1] - 10.0), 6.0, places=6)
        self.assertLess(p[0], 10.0)
        self.assertLess(p[1], 10.0)
        dx, dy = runtime.PlaySession.direction({"w", "s"})
        self.assertEqual((dx, dy), (0.0, 0.0))

    def test_invalid_dt(self):
        session = runtime.PlaySession(self.make())
        for dt in (0.0, -1.0, float("nan"), float("inf"), 10.0, True, 10 ** 400):
            with self.subTest(dt=dt), self.assertRaises(ValueError):
                session.step(dt, set())
        self.assertEqual(session.ticks, 0)

    def test_advance_caps_catch_up(self):
        session = runtime.PlaySession(self.make())
        self.assertEqual(session.advance(runtime.FIXED_DT * 2.5, set()), 2)
        self.assertEqual(session.advance(10.0, set()), runtime.MAX_CATCH_UP_STEPS)
        self.assertEqual(session.ticks, 2 + runtime.MAX_CATCH_UP_STEPS)

    @unittest.skipUnless(native_available() or REQUIRE_NATIVE, "native bridge not built")
    def test_session_project_renders(self):
        session = runtime.PlaySession(self.make())
        session.step(runtime.FIXED_DT, {"right"})
        with runtime.Renderer2D(32, 24) as renderer:
            self.assertEqual(renderer.render(session.project).size, (32, 24))


def solid(obj_id, x, y, w, h, **kw) -> SceneObject:
    obj = sprite(obj_id, x, y, w, h, **kw)
    obj.components["collider2d"] = {"enabled": True, "trigger": False, "fit_sprite": True,
                                    "offset": [0.0, 0.0], "size": [32.0, 32.0]}
    obj.components["body2d"] = {"type": "static"}
    return obj


def trigger(obj_id, x, y, w, h, **kw) -> SceneObject:
    obj = solid(obj_id, x, y, w, h, **kw)
    obj.components["collider2d"]["trigger"] = True
    return obj


def hero(obj_id, x, y, w=10, h=10, *, speed=60.0, controlled=True, velocity=(0.0, 0.0), **body) -> SceneObject:
    obj = solid(obj_id, x, y, w, h, motion={"controlled": controlled, "speed": speed,
                                            "velocity": list(velocity)})
    obj.components["body2d"] = {"type": "dynamic", "gravity": 600.0, "jump_speed": 220.0,
                                "max_fall_speed": 600.0, **body}
    return obj


def collision_native_available() -> bool:
    try:
        runtime._load(runtime.find_library())
        return True
    except runtime.NativeLibraryError:
        return False


def pos(session, obj_id):
    return session.project.object_by_id(obj_id).transform.position


@unittest.skipUnless(collision_native_available() or REQUIRE_NATIVE, "ABI2 native bridge not built")
class CollisionPlayTests(unittest.TestCase):
    GROUND_Y = 100.0  # ground top; a 10-high hero rests at y == 90

    def world(self, *extra, player_y=90.0, **body):
        return project([solid("ground", -100, self.GROUND_Y, 400, 20), hero("player", 10, player_y, **body),
                        *extra], width=320, height=200)

    def run_ticks(self, session, n, keys=()):
        for _ in range(n):
            session.step(runtime.FIXED_DT, set(keys))

    def test_falls_and_grounds_without_mutating_source(self):
        src = self.world(player_y=40.0)
        before = src.to_dict()
        s = runtime.PlaySession(src)
        self.run_ticks(s, 90)
        self.assertAlmostEqual(pos(s, "player")[1], 90.0, places=3)
        self.assertIn("player", s.grounded)
        self.assertNotIn("ground", s.grounded)
        self.assertEqual(src.to_dict(), before)
        self.assertEqual(pos(s, "ground")[:2], [-100.0, 100.0])

    def test_first_tick_jump_from_exact_floor(self):
        s = runtime.PlaySession(self.world())
        s.step(runtime.FIXED_DT, {"space"})
        self.assertLess(pos(s, "player")[1], 90.0)
        self.assertNotIn("player", s.grounded)

    def test_held_jump_does_not_autojump_but_repress_does(self):
        s = runtime.PlaySession(self.world())
        self.run_ticks(s, 1, {"Space"})
        self.run_ticks(s, 150, {"Space"})  # airtime is ~45 ticks; then keep holding
        self.assertAlmostEqual(pos(s, "player")[1], 90.0, places=3)
        self.assertIn("player", s.grounded)
        self.run_ticks(s, 10, {"Space"})
        self.assertAlmostEqual(pos(s, "player")[1], 90.0, places=3)
        self.run_ticks(s, 1, set())
        self.run_ticks(s, 1, {"w"})
        self.assertLess(pos(s, "player")[1], 90.0)

    def test_air_press_does_not_double_jump(self):
        s = runtime.PlaySession(self.world())
        self.run_ticks(s, 1, {"up"})
        self.run_ticks(s, 10, set())
        y = pos(s, "player")[1]
        vy = s._velocity["player"][1]
        self.run_ticks(s, 1, {"up"})  # airborne press: no new impulse
        self.assertGreater(s._velocity["player"][1], vy)
        self.assertGreater(y, 0)

    def test_wall_ceiling_and_walking_off_edge(self):
        wall = solid("wall", 40, 0, 10, 100)
        ceiling = solid("ceiling", 0, 40, 30, 10)
        s = runtime.PlaySession(self.world(wall, ceiling))
        self.run_ticks(s, 120, {"d"})
        self.assertAlmostEqual(pos(s, "player")[0], 30.0, places=3)  # touches wall at x=40
        self.assertIn("player", s.grounded)
        s2 = runtime.PlaySession(project([solid("g", 0, 100, 30, 20), hero("p", 10, 90)]))
        self.run_ticks(s2, 60, {"right"})
        self.assertGreater(pos(s2, "p")[1], 90.0)  # left the edge, now falling
        self.assertNotIn("p", s2.grounded)
        c = runtime.PlaySession(project([solid("g", 0, 100, 100, 20), solid("c", 0, 70, 100, 10),
                                         hero("p", 10, 90)]))
        self.run_ticks(c, 30, {"space"})
        self.assertGreaterEqual(pos(c, "p")[1], 80.0 - 1e-3)

    def test_high_speed_does_not_tunnel(self):
        s = runtime.PlaySession(project([solid("thin", 100, 0, 1, 50),
                                         hero("fast", 0, 10, gravity=0.0, speed=0.0, velocity=(5000.0, 0.0))]))
        for _ in range(4):
            s.step(0.25, set())
        self.assertAlmostEqual(pos(s, "fast")[0], 90.0, places=3)

    def test_triggers_enter_stay_exit_hidden_and_sorted(self):
        coin = trigger("coin", 30, 90, 10, 10, visible=False)
        s = runtime.PlaySession(self.world(coin))
        phases = []
        for _ in range(120):
            s.step(runtime.FIXED_DT, {"d"})
            phases.extend(e[0] for e in s.trigger_events)
            self.assertEqual(s.trigger_events, sorted(s.trigger_events))
        self.assertIn("enter", phases)
        self.assertIn("stay", phases)
        self.assertEqual(phases.count("enter"), 1)
        self.assertEqual(phases.count("exit"), 1)
        self.assertLess(phases.index("enter"), phases.index("stay"))
        self.assertEqual(s.active_triggers, set())
        self.assertEqual(pos(s, "coin")[:2], [30.0, 90.0])  # trigger never blocks nor moves

    def test_trigger_event_payload_and_pair_order(self):
        s = runtime.PlaySession(self.world(trigger("zone", 5, 85, 30, 15)))
        s.step(runtime.FIXED_DT, set())
        self.assertIn(("enter", "zone", "player"), s.trigger_events)
        self.assertEqual(s.active_triggers, {("zone", "player")})
        self.assertNotIn("zone", s.grounded)
        both = runtime.PlaySession(project([trigger("b", 0, 0, 10, 10), trigger("a", 5, 5, 10, 10)]))
        both.step(runtime.FIXED_DT, set())
        self.assertEqual(both.trigger_events, [("enter", "a", "b")])

    def test_fast_pass_through_between_ticks_is_not_sampled(self):
        s = runtime.PlaySession(project([trigger("t", 50, 0, 2, 10),
                                         hero("p", 0, 0, gravity=0.0, speed=0.0, velocity=(1000.0, 0.0))]))
        s.step(0.25, set())  # 0 -> 250 in one tick; overlap is evaluated at tick end
        self.assertEqual(s.trigger_events, [])

    def test_triggers_use_clamped_world_position(self):
        sensor = trigger("sensor", runtime.MAX_COORD, 0, 2, 10)
        sensor.components["collider2d"].update(fit_sprite=False, offset=[8, 0], size=[2, 10])
        player = hero("player", runtime.MAX_COORD - 3, 0, speed=1000, gravity=0)
        s = runtime.PlaySession(project([sensor, player]))
        s.step(0.1, {"d"})
        self.assertEqual(pos(s, "player")[0], runtime.MAX_COORD)
        self.assertIn(("sensor", "player"), s.active_triggers)

    def test_dynamic_trigger_ignores_solids(self):
        coin = trigger("fall", 200, 0, 10, 10)
        coin.components["body2d"] = {"type": "dynamic", "gravity": 600.0}
        s = runtime.PlaySession(self.world(coin, player_y=90.0))
        s.step(runtime.FIXED_DT, set())
        self.run_ticks(s, 90)
        self.assertGreater(pos(s, "fall")[1], 100.0)

    def test_custom_offset_rotation_and_negative_scale(self):
        custom = solid("custom", 50, 0, 10, 10)
        custom.components["collider2d"].update(fit_sprite=False, offset=[8.0, 0.0], size=[4.0, 100.0])
        s = runtime.PlaySession(project([custom, hero("p", 10, 20, gravity=0.0)]))
        self.run_ticks(s, 120, {"d"})
        self.assertAlmostEqual(pos(s, "p")[0], 48.0, places=3)  # blocked by x=58..62

        bar = solid("bar", 60, 50, 4, 40, rot=90.0)  # clockwise: occupies x 20..60, y 50..54
        s = runtime.PlaySession(project([bar, hero("p", 30, 20)]))
        self.run_ticks(s, 90)
        self.assertAlmostEqual(pos(s, "p")[1], 40.0, places=3)
        self.assertIn("p", s.grounded)

        mirrored = solid("m", 100, 0, 10, 100, scale=(-1.0, 1.0))  # x 90..100
        s = runtime.PlaySession(project([mirrored, hero("p", 60, 20, gravity=0.0)]))
        self.run_ticks(s, 120, {"d"})
        self.assertAlmostEqual(pos(s, "p")[0], 80.0, places=3)

    def test_legacy_objects_keep_free_motion_and_need_no_native(self):
        free = sprite("free", 0, 0, motion={"controlled": True, "speed": 60.0, "velocity": [0.0, 0.0]})
        disabled = solid("off", 0, 0, 4, 4, motion={"controlled": True, "speed": 60.0, "velocity": [0, 0]})
        disabled.components["collider2d"]["enabled"] = False
        s = runtime.PlaySession(project([free, disabled]))
        self.run_ticks(s, 60, {"d", "s"})
        self.assertIsNone(s._lib)
        self.assertGreater(pos(s, "free")[0], 30.0)
        self.assertGreater(pos(s, "off")[1], 30.0)
        self.assertEqual((s.grounded, s.active_triggers, s.trigger_events), (set(), set(), []))

    def test_static_solid_and_motionless_trigger_motion_rules(self):
        wall = solid("wall", 20, 20, 4, 4, motion={"controlled": True, "speed": 60.0, "velocity": [30, 0]})
        mover = trigger("mover", 0, 0, 4, 4, motion={"controlled": False, "speed": 0.0, "velocity": [60, 0]})
        s = runtime.PlaySession(project([wall, mover]))
        self.run_ticks(s, 60, {"d"})
        self.assertEqual(pos(s, "wall")[:2], [20.0, 20.0])
        self.assertAlmostEqual(pos(s, "mover")[0], 60.0, places=4)

    def test_gravity_zero_free_movement_without_jump(self):
        s = runtime.PlaySession(project([hero("p", 10, 50, gravity=0.0, speed=60.0)]))
        self.run_ticks(s, 60, {"up", "d"})
        self.assertLess(pos(s, "p")[1], 50.0)
        self.assertGreater(pos(s, "p")[0], 10.0)
        y = pos(s, "p")[1]
        self.run_ticks(s, 30, set())
        self.assertAlmostEqual(pos(s, "p")[1], y, places=4)  # no gravity, no fall

    def test_body_defaults_and_fall_speed_cap(self):
        p = hero("p", 0, 0)
        p.components["body2d"] = {"type": "dynamic"}
        s = runtime.PlaySession(project([p]))
        for _ in range(300):
            s.step(runtime.FIXED_DT, set())
        self.assertLessEqual(s._velocity["p"][1], 600.0 + 1e-3)
        self.assertGreater(s._velocity["p"][1], 500.0)

    def test_failed_ticks_are_transactional(self):
        stuck = hero("stuck", 5, 105)  # starts inside the ground
        drifter = sprite("drift", 0, 0, motion={"controlled": False, "speed": 0.0, "velocity": [60.0, 0.0]})
        s = runtime.PlaySession(project([solid("ground", -100, 100, 400, 20), stuck, drifter]))
        s.active_triggers = {("x", "y")}
        s._jump_held = False
        for keys in (set(), {"space"}):
            with self.assertRaises(runtime.RendererError):
                s.step(runtime.FIXED_DT, keys)
        self.assertEqual(s.ticks, 0)
        self.assertEqual(pos(s, "drift")[:2], [0.0, 0.0])
        self.assertEqual(pos(s, "stuck")[:2], [5.0, 105.0])
        self.assertEqual(s.active_triggers, {("x", "y")})
        self.assertFalse(s._jump_held)
        self.assertEqual(s._velocity, {})

    def test_invalid_collider_geometry_fails_clearly(self):
        bad = solid("bad", 0, 0, 10, 10)
        bad.components["collider2d"].update(fit_sprite=False, size=[0.0, 5.0])
        s = runtime.PlaySession(project([bad, hero("p", 50, 0)]))
        with self.assertRaises(runtime.RendererError):
            s.step(runtime.FIXED_DT, set())
        self.assertEqual(pos(s, "p")[:2], [50.0, 0.0])
        self.assertEqual(s.ticks, 0)
        bad2 = solid("bad2", 0, 0, 10, 10)
        bad2.transform.position[0] = float("nan")
        with self.assertRaises(runtime.RendererError):
            runtime.PlaySession(project([bad2])).step(runtime.FIXED_DT, set())

    def test_clone_keeps_extra_data_and_3d_axes(self):
        p = hero("p", 10, 90)
        p.transform.position[2] = 7.0
        p.transform.rotation[:2] = [11.0, 22.0]
        p.components["collider2d"]["custom"] = {"keep": 1}
        src = self.world()
        src.objects[1] = p
        s = runtime.PlaySession(src)
        self.run_ticks(s, 5, {"d"})
        q = s.project.object_by_id("p")
        self.assertEqual(q.transform.position[2], 7.0)
        self.assertEqual(q.transform.rotation[:2], [11.0, 22.0])
        self.assertEqual(q.components["collider2d"]["custom"], {"keep": 1})
        self.assertEqual(src.objects[1].transform.position, [10.0, 90.0, 7.0])


if __name__ == "__main__":
    unittest.main()
