"""Tests for pixelica_editor.model (stdlib unittest, temp dirs only)."""
import copy
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pixelica_editor import model  # noqa: E402
from pixelica_editor.model import (Project, ProjectError, import_asset,  # noqa: E402
                                   load_project, new_project, save_project)

PNG = b"\x89PNG\r\n\x1a\n" + b"fakepngdata"


def minimal(objects=None, **kw):
    d = {"format": "pixelica.project", "version": 1, "name": "t",
         "width": 64, "height": 64, "background": "#000000",
         "objects": [] if objects is None else objects}
    d.update(kw)
    return d


def obj(oid="a", **kw):
    d = {"id": oid, "name": oid, "kind": "sprite2d"}
    d.update(kw)
    return d


class TempCase(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.tmp = Path(self._td.name).resolve()

    def write_png(self, name="src.png", data=PNG):
        p = self.tmp / "srcdir" / name
        p.parent.mkdir(exist_ok=True)
        p.write_bytes(data)
        return p


class ModelTests(unittest.TestCase):
    def test_validate_does_not_coerce_invalid_transform_types(self):
        for value in (True, "12", 10**400):
            p = new_project()
            p.objects[0].transform.position[0] = value
            with self.subTest(value=type(value).__name__), self.assertRaises(ProjectError):
                p.validate()

    def test_sample_valid_roundtrip_and_stable_ids(self):
        p = new_project()
        self.assertEqual(Project.from_dict(p.to_dict()).to_dict(), p.to_dict())
        self.assertEqual(new_project().to_dict(), p.to_dict())
        ids = [o.id for o in p.objects]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIsNotNone(p.object_by_id("sample-player"))
        self.assertTrue(all(o.components["sprite2d"]["asset"] is None for o in p.objects))
        self.assertNotIn("path", p.to_dict())

    def test_preserves_3d_axes_unknown_components_and_extras(self):
        d = minimal([obj(transform={"position": [1, 2, 3], "rotation": [10, 20, 30],
                                    "scale": [1, 2, 3]},
                         components={"mesh3d": {"verts": [[0, 0, 1]], "nested": {"k": None}},
                                     "sprite2d": {"width": 5, "height": 6, "custom": "keep"}},
                         future="x")], future_root=[1, 2])
        p = Project.from_dict(d)
        out = p.to_dict()
        o = out["objects"][0]
        self.assertEqual(o["transform"]["position"], [1.0, 2.0, 3.0])
        self.assertEqual(o["transform"]["rotation"], [10.0, 20.0, 30.0])
        self.assertEqual(o["transform"]["scale"], [1.0, 2.0, 3.0])
        self.assertEqual(o["components"]["mesh3d"]["verts"], [[0, 0, 1]])
        self.assertEqual(o["components"]["sprite2d"]["custom"], "keep")
        self.assertEqual(o["future"], "x")
        self.assertEqual(out["future_root"], [1, 2])

    def test_unsupported_kind_loads(self):
        p = Project.from_dict(minimal([obj(kind="mesh3d")]))
        self.assertEqual(p.objects[0].kind, "mesh3d")
        self.assertNotIn("sprite2d", p.objects[0].components)

    def test_rejects_invalid(self):
        bad = {
            "format": minimal(format="other"),
            "version": minimal(version=2),
            "bool version": minimal(version=True),
            "width zero": minimal(width=0),
            "width bool": minimal(width=True),
            "width big": minimal(width=2049),
            "pixels": minimal(width=2048, height=2048),
            "bg": minimal(background="red"),
            "dup ids": minimal([obj("a"), obj("a")]),
            "bad id": minimal([obj("../x")]),
            "scale zero": minimal([obj(transform={"scale": [1, 0, 1]})]),
            "pos 2 axes": minimal([obj(transform={"position": [1, 2]})]),
            "pos bool": minimal([obj(transform={"position": [True, 0, 0]})]),
            "pos huge": minimal([obj(transform={"position": [1e12, 0, 0]})]),
            "pos overflowing int": minimal([obj(transform={"position": [10**400, 0, 0]})]),
            "sprite extent underflow": minimal([obj(transform={"scale": [1e-300, 1, 1]})]),
            "sprite extent too large": minimal([obj(transform={"scale": [10000, 1, 1]},
                components={"sprite2d": {"width": 10000}})]),
            "transform extra": minimal([obj(transform={"skew": [0, 0, 0]})]),
            "opacity": minimal([obj(components={"sprite2d": {"opacity": 256}})]),
            "opacity bool": minimal([obj(components={"sprite2d": {"opacity": True}})]),
            "width neg": minimal([obj(components={"sprite2d": {"width": -1}})]),
            "color": minimal([obj(components={"sprite2d": {"color": "#12"}})]),
            "visible": minimal([obj(visible=1)]),
            "controlled": minimal([obj(components={"motion2d": {"controlled": 1}})]),
            "velocity": minimal([obj(components={"motion2d": {"velocity": [0]}})]),
            "components type": minimal([obj(components=[])]),
            "objects type": minimal(objects={}),
            "too many": minimal([obj(f"o{i}") for i in range(513)]),
        }
        for asset in ("/abs.png", "../x.png", "a/../../x.png", "C:/x.png", "a\\b.png",
                      "a//b.png", "x.exe", "", "./x.png", 5):
            bad[f"asset {asset!r}"] = minimal(
                [obj(components={"sprite2d": {"asset": asset}})])
        for name, data in bad.items():
            with self.subTest(name), self.assertRaises(ProjectError):
                Project.from_dict(data)

    def test_edit_methods(self):
        p = Project(name="x", width=100, height=50)
        a = p.add_object()
        self.assertEqual(a.transform.position[:2], [34.0, 9.0])
        b = p.duplicate_object(a.id)
        self.assertNotEqual(a.id, b.id)
        self.assertEqual(b.transform.position[:2], [42.0, 17.0])
        b.components["sprite2d"]["color"] = "#000000"
        self.assertEqual(a.components["sprite2d"]["color"], "#edbd52")
        self.assertTrue(p.remove_object(a.id))
        self.assertFalse(p.remove_object(a.id))
        self.assertIsNone(p.object_by_id(a.id))
        with self.assertRaises(ProjectError):
            p.duplicate_object("missing")
        # id of a removed object may be reused but never collides
        c = p.add_object()
        self.assertEqual(len({o.id for o in p.objects}), len(p.objects))
        self.assertNotEqual(c.id, b.id)

    def test_clone_is_independent_and_keeps_path(self):
        p = new_project()
        p.path = Path("somewhere/p.json")
        c = p.clone()
        c.objects[0].transform.position[0] = 999.0
        c.objects[0].components["sprite2d"]["color"] = "#ffffff"
        self.assertNotEqual(p.objects[0].transform.position[0], 999.0)
        self.assertEqual(c.path, p.path)

    def test_object_limit_on_add(self):
        p = Project()
        for _ in range(512):
            p.add_object()
        with self.assertRaises(ProjectError):
            p.add_object()


class CollisionModelTests(unittest.TestCase):
    def _o(self, **comps):
        c = {"sprite2d": {"width": 20, "height": 10}}
        c.update(comps)
        return Project.from_dict(minimal([obj(components=c)])).objects[0]

    def test_legacy_objects_have_no_collision_components(self):
        o = Project.from_dict(minimal([obj()])).objects[0]
        self.assertNotIn("collider2d", o.components)
        self.assertNotIn("body2d", o.components)
        self.assertIsNone(model.collider_bounds(o))

    def test_defaults_filled_and_unknown_keys_preserved(self):
        o = self._o(collider2d={"future": [1]}, body2d={"x": "y"})
        self.assertEqual(o.components["collider2d"],
                         dict(model.default_collider2d(), future=[1]))
        self.assertEqual(o.components["body2d"], dict(model.default_body2d(), x="y"))
        d = Project.from_dict(minimal([o.to_dict()])).to_dict()
        self.assertEqual(d["objects"][0]["components"]["collider2d"]["future"], [1])

    def test_rejects_invalid_collision_values(self):
        bad = [
            ("collider2d", {"enabled": 1}), ("collider2d", {"trigger": "no"}),
            ("collider2d", {"fit_sprite": None}), ("collider2d", {"offset": [0]}),
            ("collider2d", {"offset": [20000, 0]}), ("collider2d", {"size": [0, 5]}),
            ("collider2d", {"size": [True, 5]}), ("collider2d", {"size": [1e9, 5]}),
            ("collider2d", []),
            ("body2d", {"type": "kinematic"}), ("body2d", {"type": 3}),
            ("body2d", {"gravity": -1}), ("body2d", {"jump_speed": 10001}),
            ("body2d", {"max_fall_speed": 0}), ("body2d", {"gravity": True}),
            ("body2d", "dynamic"),
        ]
        for name, val in bad:
            with self.subTest(name=name, val=val), self.assertRaises(ProjectError):
                self._o(**{name: val})

    def test_bounds_fit_offset_scale_rotation(self):
        o = self._o(collider2d={})
        o.transform.position[:2] = [100.0, 50.0]
        self.assertEqual(model.collider_bounds(o), (100.0, 50.0, 120.0, 60.0))
        o.components["collider2d"].update(fit_sprite=False, offset=[2.0, 3.0], size=[4.0, 5.0])
        self.assertEqual(model.collider_bounds(o), (102.0, 53.0, 106.0, 58.0))
        o.transform.scale[:2] = [-2.0, 1.0]
        self.assertEqual(model.collider_bounds(o), (88.0, 53.0, 96.0, 58.0))
        o.transform.scale[:2] = [1.0, 1.0]
        o.components["collider2d"]["fit_sprite"] = True
        o.transform.rotation[2] = 90.0  # clockwise: (x, y) -> (-y, x)
        b = model.collider_bounds(o)
        for got, want in zip(b, (90.0, 50.0, 100.0, 70.0)):
            self.assertAlmostEqual(got, want, places=6)

    def test_disabled_collider_has_no_bounds_and_invalid_enabled_raises(self):
        o = self._o(collider2d={"enabled": False})
        self.assertIsNone(model.collider_bounds(o))
        o.components["collider2d"]["enabled"] = True
        o.transform.scale[0] = 1e-9
        with self.assertRaises(ProjectError):
            model.collider_bounds(o)
        with self.assertRaises(ProjectError):  # also rejected at the load boundary
            Project.from_dict(minimal([obj(
                transform={"scale": [0.01, 1, 1]},
                components={"collider2d": {"fit_sprite": False, "size": [0.001, 1]},
                            "sprite2d": {"width": 1, "height": 1}})]))
        with self.assertRaises(ProjectError):  # fit_sprite requires a sprite
            Project.from_dict(minimal([obj(kind="mesh3d", components={"collider2d": {}})]))

    def test_sample_demonstrates_solids_player_and_triggers(self):
        p = new_project()
        col = lambda i: p.object_by_id(i).components.get("collider2d")
        for i in ("sample-ground", "sample-platform-1", "sample-platform-2"):
            self.assertTrue(col(i)["enabled"] and not col(i)["trigger"])
            self.assertEqual(p.object_by_id(i).components["body2d"]["type"], "static")
        for i in ("sample-grass", "sample-moon", "sample-hill-far"):
            self.assertIsNone(col(i))
        for i in ("sample-coin-1", "sample-coin-2", "sample-coin-3"):
            self.assertTrue(col(i)["enabled"] and col(i)["trigger"])
        player = p.object_by_id("sample-player")
        body = player.components["body2d"]
        self.assertEqual((body["type"], body["gravity"], body["jump_speed"]), ("dynamic", 600.0, 220.0))
        self.assertTrue(player.components["motion2d"]["controlled"])
        pb = model.collider_bounds(player)
        gb = model.collider_bounds(p.object_by_id("sample-ground"))
        self.assertEqual(pb[3], gb[1])  # standing exactly on the ground
        boxes = {o.id: model.collider_bounds(o) for o in p.objects if model.collider_bounds(o)}
        pid = "sample-player"
        for oid, b in boxes.items():
            if oid != pid:
                overlap = pb[0] < b[2] and b[0] < pb[2] and pb[1] < b[3] and b[1] < pb[3]
                self.assertFalse(overlap, oid)


class FileTests(TempCase):
    def test_save_load_roundtrip(self):
        p = new_project()
        f = self.tmp / "proj" / "p.json"
        save_project(p, f)
        self.assertEqual(p.path, f)
        q = load_project(f)
        self.assertEqual(q.path, f)
        self.assertEqual(q.to_dict(), p.to_dict())
        self.assertNotIn("path", json.loads(f.read_text("utf-8")))
        self.assertEqual([x.name for x in f.parent.iterdir()], ["p.json"])

    def test_load_rejects_bad_files(self):
        cases = {
            "nan.json": '{"format":"pixelica.project","version":1,"width":NaN}',
            "dupkey.json": '{"format":"pixelica.project","version":1,"name":"a","name":"b"}',
            "syntax.json": "{nope",
            "list.json": "[]",
        }
        for name, text in cases.items():
            f = self.tmp / name
            f.write_text(text, "utf-8")
            with self.subTest(name), self.assertRaises(ProjectError):
                load_project(f)
        big = self.tmp / "big.json"
        big.write_bytes(b" " * (model.MAX_JSON_BYTES + 1))
        with self.assertRaises(ProjectError):
            load_project(big)
        binary = self.tmp / "bin.json"
        binary.write_bytes(b"\xff\xfe\x00")
        with self.assertRaises(ProjectError):
            load_project(binary)
        with self.assertRaises(OSError):
            load_project(self.tmp / "missing.json")

    def test_save_failure_keeps_previous_file_and_path(self):
        f = self.tmp / "p.json"
        p = new_project()
        save_project(p, f)
        before = f.read_bytes()
        p.name = "changed"
        with mock.patch.object(model.os, "replace", side_effect=OSError("boom")):
            with self.assertRaises(OSError):
                save_project(p, f)
        self.assertEqual(f.read_bytes(), before)
        self.assertEqual([x.name for x in self.tmp.iterdir()], ["p.json"])

    def test_invalid_project_not_written(self):
        f = self.tmp / "p.json"
        p = new_project()
        p.objects[0].transform.scale[0] = 0.0
        with self.assertRaises(ProjectError):
            save_project(p, f)
        self.assertFalse(f.exists())
        self.assertIsNone(p.path)

    def test_import_asset_digest_dedupe_and_validation(self):
        f = self.tmp / "proj" / "p.json"
        p = new_project()
        with self.assertRaises(ProjectError):
            import_asset(p, self.write_png())  # unsaved project
        save_project(p, f)
        src = self.write_png()
        rel = import_asset(p, src)
        self.assertTrue(rel.startswith("assets/") and rel.endswith(".png"))
        self.assertEqual((f.parent / rel).read_bytes(), PNG)
        self.assertEqual(import_asset(p, src), rel)
        other = self.write_png("same-name.png", PNG + b"2")
        rel2 = import_asset(p, other)
        self.assertNotEqual(rel, rel2)
        self.assertEqual((f.parent / rel).read_bytes(), PNG)  # not overwritten
        txt = self.write_png("x.png", b"not a png")
        with self.assertRaises(ProjectError):
            import_asset(p, txt)
        exe = self.write_png("x.exe", PNG)
        with self.assertRaises(ProjectError):
            import_asset(p, exe)
        with self.assertRaises(ProjectError):
            import_asset(p, self.tmp / "nope.png")
        self.assertEqual(rel, model.normalize_asset_path(rel))

    def test_save_as_copies_assets_and_keeps_old(self):
        old = self.tmp / "old" / "p.json"
        p = new_project()
        save_project(p, old)
        rel = import_asset(p, self.write_png())
        p.objects[0].components["sprite2d"]["asset"] = rel
        save_project(p, old)
        new = self.tmp / "new" / "q.json"
        save_project(p, new)
        self.assertEqual(p.path, new)
        self.assertEqual((new.parent / rel).read_bytes(), PNG)
        self.assertTrue((old.parent / rel).is_file())
        self.assertEqual(load_project(new).objects[0].components["sprite2d"]["asset"], rel)

    def test_save_as_failure_rolls_back_copies_and_path(self):
        old = self.tmp / "old" / "p.json"
        p = new_project()
        save_project(p, old)
        rel = import_asset(p, self.write_png())
        p.objects[0].components["sprite2d"]["asset"] = rel
        save_project(p, old)
        new = self.tmp / "new" / "q.json"
        real = os.replace

        def fail_json(src, dst):
            if str(dst).endswith("q.json"):
                raise OSError("disk full")
            return real(src, dst)

        with mock.patch.object(model.os, "replace", side_effect=fail_json):
            with self.assertRaises(OSError):
                save_project(p, new)
        self.assertEqual(p.path, old)
        self.assertFalse(new.exists())
        self.assertFalse((new.parent / rel).exists())
        self.assertTrue((old.parent / rel).is_file())

    def test_save_as_refuses_to_overwrite_different_asset(self):
        old = self.tmp / "old" / "p.json"
        p = new_project()
        save_project(p, old)
        rel = import_asset(p, self.write_png())
        p.objects[0].components["sprite2d"]["asset"] = rel
        victim = self.tmp / "new" / rel
        victim.parent.mkdir(parents=True)
        victim.write_bytes(b"precious")
        with self.assertRaises(ProjectError):
            save_project(p, self.tmp / "new" / "q.json")
        self.assertEqual(victim.read_bytes(), b"precious")
        self.assertEqual(p.path, old)

    def test_symlink_escape_rejected(self):
        outside = self.tmp / "outside"
        outside.mkdir()
        (outside / "secret.png").write_bytes(PNG)
        proj = self.tmp / "proj"
        proj.mkdir()
        try:
            os.symlink(outside, proj / "assets", target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks unavailable")
        d = minimal([obj(components={"sprite2d": {"asset": "assets/secret.png"}})])
        f = proj / "p.json"
        f.write_text(json.dumps(d), "utf-8")
        with self.assertRaises(ProjectError):
            load_project(f)
        p = Project.from_dict(d)
        p.path = f
        with self.assertRaises(ProjectError):
            import_asset(p, self.write_png())
        self.assertEqual([x.name for x in outside.iterdir()], ["secret.png"])


if __name__ == "__main__":
    unittest.main()
