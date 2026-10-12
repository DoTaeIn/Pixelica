"""Tk smoke tests for the Pixelica editor UI.

Skipped (with a clear reason) when no Tk display is available. Dialog methods
(ask_*) are replaced so nothing modal opens. Rendering-dependent assertions are
skipped when the native bridge library is not built.
"""
import json
import sys
import tempfile
import tkinter as tk
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pixelica_editor import app as editor_app  # noqa: E402
from pixelica_editor.model import Project  # noqa: E402


def _make_root():
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        raise unittest.SkipTest(f"Tk 디스플레이를 사용할 수 없어 UI 테스트를 건너뜁니다: {exc}")
    root.withdraw()
    return root


class EditorAppTest(unittest.TestCase):
    def setUp(self):
        self.root = _make_root()
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.app = editor_app.EditorApp(self.root)
        self.app.ask_unsaved = lambda: "cancel"

    def tearDown(self):
        if not self.app._closed:
            self.app.ask_unsaved = lambda: "discard"
            self.app.close()
        try:
            self.root.destroy()
        except tk.TclError:
            pass
        self.tmp.cleanup()

    def _sprite_xy(self, obj):
        return obj.transform.position[0], obj.transform.position[1]

    def test_full_workflow_add_drag_edit_save_reload_play_stop(self):
        a = self.app
        count = len(a.project.objects)
        obj = a.add_object()
        self.assertIsNotNone(obj)
        self.assertEqual(len(a.project.objects), count + 1)
        self.assertEqual(a.selected_id, obj.id)
        self.assertTrue(a.is_dirty())

        # drag exactly once -> exactly one undo entry
        x, y = self._sprite_xy(obj)
        undo_before = len(a._undo)
        self.assertEqual(a.begin_drag(x + 1, y + 1), obj.id)
        a.drag_to(x + 11, y + 6)
        a.drag_to(x + 21, y + 11)
        self.assertTrue(a.end_drag())
        self.assertEqual(len(a._undo), undo_before + 1)
        self.assertEqual(self._sprite_xy(a.selected()), (x + 20, y + 10))
        self.assertTrue(a.undo())
        self.assertEqual(self._sprite_xy(a.selected()), (x, y))
        self.assertTrue(a.redo())

        # property transaction
        a.vars["name"].set("테스트 스프라이트")
        a.vars["rot"].set("30")
        a.vars["w"].set("48")
        a.vars["opacity"].set("200")
        self.assertTrue(a.apply_properties())
        o = a.selected()
        self.assertEqual(o.name, "테스트 스프라이트")
        self.assertEqual(o.transform.rotation[2], 30.0)
        self.assertEqual(o.components["sprite2d"]["width"], 48.0)
        self.assertEqual(o.components["sprite2d"]["opacity"], 200)

        # save / reload
        path = self.dir / "scene.json"
        self.assertTrue(a.save(path))
        self.assertFalse(a.is_dirty())
        saved = a.project.to_dict()
        a.add_object()
        self.assertTrue(a.is_dirty())
        a.ask_unsaved = lambda: "discard"
        self.assertTrue(a.open_project(path))
        self.assertEqual(a.project.to_dict(), saved)
        self.assertFalse(a.is_dirty())
        self.assertEqual(a.project.path, path)

        # play on an independent clone; stop restores the source untouched
        before = json.dumps(a.project.to_dict(), sort_keys=True)
        a.toggle_play()
        self.assertTrue(a.playing)
        self.assertIsNot(a.session.project, a.project)
        a.press_key("d")
        self.assertEqual(a.step_play(12), 12)
        a.toggle_play()
        self.assertTrue(a.paused)
        a.stop_play()
        self.assertFalse(a.playing)
        self.assertEqual(json.dumps(a.project.to_dict(), sort_keys=True), before)
        self.assertFalse(a.is_dirty())

    def test_invalid_property_leaves_model_unchanged(self):
        a = self.app
        a.add_object()
        before = json.dumps(a.project.to_dict(), sort_keys=True)
        undo = len(a._undo)
        a.vars["x"].set("abc")
        self.assertFalse(a.apply_properties())
        a.vars["sx"].set("0")
        self.assertFalse(a.apply_properties())
        a.vars["color"].set("red")
        self.assertFalse(a.apply_properties())
        self.assertEqual(json.dumps(a.project.to_dict(), sort_keys=True), before)
        self.assertEqual(len(a._undo), undo)
        self.assertTrue(a.insp_msg.get())

    def test_undo_is_bounded(self):
        a = self.app
        for _ in range(editor_app.UNDO_LIMIT + 10):
            if len(a.project.objects) >= 400:
                break
            a.add_object()
        self.assertLessEqual(len(a._undo), editor_app.UNDO_LIMIT)

    def test_dirty_follows_serialized_state(self):
        a = self.app
        self.assertFalse(a.is_dirty())
        a.add_object()
        self.assertTrue(a.is_dirty())
        self.assertTrue(a.undo())
        self.assertFalse(a.is_dirty())

    def test_rotated_hit_test(self):
        a = self.app
        a.project.objects.clear()
        o = a.project.add_object("회전", "sprite2d")
        o.transform.position[0], o.transform.position[1] = 100.0, 100.0
        o.transform.rotation[2] = 90.0
        sp = o.components["sprite2d"]
        sp["width"], sp["height"] = 40.0, 10.0
        # 90 degrees about the pivot: rectangle spans x 90..100, y 100..140
        self.assertEqual(a.hit_test(95, 120), o.id)
        self.assertIsNone(a.hit_test(120, 105))
        o.visible = False
        self.assertIsNone(a.hit_test(95, 120))

    def test_failed_open_preserves_project(self):
        a = self.app
        a.add_object()
        before = json.dumps(a.project.to_dict(), sort_keys=True)
        bad = self.dir / "bad.json"
        bad.write_text("{ not json", encoding="utf-8")
        self.assertFalse(a.open_project(bad))
        self.assertEqual(json.dumps(a.project.to_dict(), sort_keys=True), before)
        self.assertTrue(a.is_dirty())
        self.assertFalse(a.open_project(self.dir / "missing.json"))

    def test_cancel_prompt_blocks_destructive_transitions(self):
        a = self.app
        a.add_object()
        good = self.dir / "good.json"
        Project.from_dict(a.project.to_dict())  # sanity: serializable
        from pixelica_editor.model import save_project
        save_project(a.project, good)
        a.add_object()
        a.ask_unsaved = lambda: "cancel"
        before = len(a.project.objects)
        self.assertFalse(a.open_project(good))
        self.assertFalse(a.new())
        self.assertFalse(a.close())
        self.assertEqual(len(a.project.objects), before)
        self.assertFalse(a._closed)

    def test_close_cancels_timer_and_destroys(self):
        a = self.app
        a.ask_unsaved = lambda: "discard"
        a.add_object()
        self.assertTrue(a.close())
        self.assertTrue(a._closed)
        self.assertIsNone(a._after_id)

    def test_delete_and_duplicate(self):
        a = self.app
        obj = a.add_object()
        n = len(a.project.objects)
        dup = a.duplicate_object()
        self.assertEqual(len(a.project.objects), n + 1)
        self.assertEqual(a.selected_id, dup.id)
        self.assertTrue(a.delete_object())
        self.assertEqual(len(a.project.objects), n)
        self.assertIsNone(a.selected_id)
        self.assertIsNotNone(a.project.object_by_id(obj.id))

    def test_save_commits_pending_object_and_project_fields(self):
        from pixelica_editor.model import load_project
        a = self.app
        obj = a.add_object()
        a.vars["x"].set("123")
        a.vars["p_name"].set("펜딩 프로젝트")
        self.assertTrue(a.is_dirty())
        path = self.dir / "pending.json"
        self.assertTrue(a.save(path))
        self.assertFalse(a.is_dirty())
        loaded = load_project(path)
        self.assertEqual(loaded.name, "펜딩 프로젝트")
        self.assertEqual(loaded.object_by_id(obj.id).transform.position[0], 123.0)

    def test_invalid_pending_blocks_save_play_and_selection(self):
        a = self.app
        first = a.add_object()
        second = a.add_object()
        self.assertEqual(a.selected_id, second.id)
        a.vars["x"].set("abc")
        before = json.dumps(a.project.to_dict(), sort_keys=True)
        path = self.dir / "blocked.json"
        self.assertFalse(a.save(path))
        self.assertFalse(path.exists())
        a.toggle_play()
        self.assertFalse(a.playing)
        self.assertFalse(a.select_object(first.id))
        self.assertEqual(a.selected_id, second.id)
        self.assertEqual(a.vars["x"].get(), "abc")
        self.assertEqual(json.dumps(a.project.to_dict(), sort_keys=True), before)
        self.assertIsNone(a.begin_drag(*self._sprite_xy(first)))
        self.assertEqual(a.selected_id, second.id)
        # valid input commits on selection change
        a.vars["x"].set("55")
        self.assertTrue(a.select_object(first.id))
        self.assertEqual(a.project.object_by_id(second.id).transform.position[0], 55.0)

    def test_pending_only_input_prompts_and_discard_discards(self):
        a = self.app
        good = self.dir / "good.json"
        self.assertTrue(a.save(good))
        name = a.project.name
        a.vars["p_name"].set("Zed")
        self.assertTrue(a.is_dirty())
        a.ask_unsaved = lambda: "cancel"
        self.assertFalse(a.new())
        self.assertFalse(a.open_project(good))
        self.assertFalse(a.close())
        self.assertFalse(a._closed)
        self.assertEqual(a.vars["p_name"].get(), "Zed")
        self.assertEqual(a.project.name, name)
        a.ask_unsaved = lambda: "discard"
        self.assertTrue(a.new())
        self.assertEqual(a.vars["p_name"].get(), a.project.name)
        self.assertFalse(a.is_dirty())

    def test_project_fields_survive_object_apply(self):
        a = self.app
        a.add_object()
        name = a.project.name
        a.vars["p_name"].set("Keep")
        a.vars["x"].set("77")
        self.assertTrue(a.apply_properties())
        self.assertEqual(a.vars["p_name"].get(), "Keep")
        self.assertEqual(a.project.name, name)
        self.assertTrue(a.is_dirty())
        self.assertTrue(a.apply_project_settings())
        self.assertEqual(a.project.name, "Keep")

    def test_undo_after_pending_commit(self):
        a = self.app
        obj = a.add_object()
        x0 = obj.transform.position[0]
        a.vars["x"].set(editor_app._fmt(x0 + 10))
        self.assertTrue(a.save(self.dir / "undo.json"))
        self.assertEqual(a.selected().transform.position[0], x0 + 10)
        self.assertTrue(a.undo())
        self.assertEqual(a.selected().transform.position[0], x0)
        self.assertEqual(a.vars["x"].get(), editor_app._fmt(x0))
        self.assertTrue(a.redo())
        self.assertEqual(a.selected().transform.position[0], x0 + 10)

    def test_save_as_other_directory_clears_history_only_on_success(self):
        a = self.app
        d1, d2 = self.dir / "one", self.dir / "two"
        d1.mkdir()
        d2.mkdir()
        a.add_object()
        self.assertTrue(a.save(d1 / "p.json"))
        a.add_object()
        a.add_object()
        self.assertTrue(a.undo())
        self.assertTrue(a._undo and a._redo)
        self.assertTrue(a.save_as(d1 / "q.json"))  # same directory keeps history
        self.assertTrue(a._undo and a._redo)
        self.assertFalse(a.save_as(d2))  # a directory is not a writable target
        self.assertTrue(a._undo and a._redo)
        a.ask_save_path = lambda: None
        self.assertFalse(a.save_as())
        self.assertTrue(a._undo and a._redo)
        self.assertTrue(a.save_as(d2 / "p.json"))
        self.assertEqual((a._undo, a._redo), ([], []))
        self.assertTrue(a.project.path.is_absolute())

    def test_toolbar_fits_minimum_width(self):
        self.root.update_idletasks()
        self.assertLessEqual(self.app.toolbar.winfo_reqwidth(), 1024)
        self.assertLessEqual(self.app.asset_row.winfo_reqwidth(), 230)
        self.assertLessEqual(self.app.view_toolbar.winfo_reqwidth(), 500)

    def test_image_import_save_as_and_reopen(self):
        from PIL import Image
        a = self.app
        if a.renderer is None:
            self.skipTest(f"Native bridge not built: {a.render_error}")
        self.assertTrue(a.save(self.dir / "original" / "scene.json"))
        source = self.dir / "source.png"
        Image.new("RGBA", (4, 3), (255, 0, 0, 128)).save(source)
        self.assertTrue(a.import_image(source))
        oid = a.selected_id
        asset = a.selected().components["sprite2d"]["asset"]
        self.assertTrue((a.project.path.parent / asset).is_file())
        target = self.dir / "copy" / "scene.json"
        self.assertTrue(a.save_as(target))
        self.assertTrue((target.parent / asset).is_file())
        self.assertTrue(a.open_project(target))
        self.assertEqual(a.project.object_by_id(oid).components["sprite2d"]["asset"], asset)
        self.assertEqual(a.render_now().size, (a.project.width, a.project.height))

    # ------------------------------------------------------------ collision
    def _snap(self):
        return json.dumps(self.app.project.to_dict(), sort_keys=True)

    def test_sample_collision_fields_load_into_inspector(self):
        a = self.app
        self.assertTrue(a.select_object("sample-player"))
        self.assertTrue(a.vars["col_enabled"].get())
        self.assertFalse(a.vars["col_trigger"].get())
        self.assertTrue(a.vars["body_dynamic"].get())
        self.assertEqual(a.vars["gravity"].get(), "600")
        self.assertEqual(a.vars["jump"].get(), "220")
        self.assertFalse(a.is_dirty())
        self.assertTrue(a.select_object("sample-coin-1"))
        self.assertTrue(a.vars["col_trigger"].get())
        self.assertFalse(a.vars["body_dynamic"].get())
        # irrelevant fields are disabled until their toggle is on
        self.assertTrue(a._col_body[0].instate(["disabled"]))
        self.assertTrue(a._col_geo[0].instate(["disabled"]))  # fit sprite
        a.vars["col_fit"].set(False)
        a._sync_collision_widgets()
        self.assertFalse(a._col_geo[0].instate(["disabled"]))

    def test_presets_are_single_undoable_edits_preserving_fields(self):
        a = self.app
        o = a.add_object()
        from pixelica_editor.model import default_collider2d
        o.components["collider2d"] = dict(default_collider2d(), enabled=False, future={"k": 1})
        a._saved_key = a._key()
        a._load_inspector()
        sprite = json.dumps(o.components["sprite2d"], sort_keys=True)
        for preset, trig, dyn in (("solid", False, False), ("trigger", True, False),
                                  ("player", False, True)):
            before_undo = len(a._undo)
            before = self._snap()
            self.assertTrue(a.apply_collision_preset(preset))
            col = a.selected().components["collider2d"]
            self.assertEqual((col["enabled"], col["trigger"]), (True, trig))
            self.assertEqual(col["future"], {"k": 1})
            self.assertEqual(json.dumps(a.selected().components["sprite2d"], sort_keys=True), sprite)
            body = a.selected().components.get("body2d")
            self.assertEqual(body is not None and body["type"] == "dynamic", dyn)
            self.assertEqual(len(a._undo), before_undo + 1)
            self.assertTrue(a.undo())
            self.assertEqual(self._snap(), before)
            self.assertTrue(a.redo())
        self.assertTrue(a.selected().components["motion2d"]["controlled"])
        self.assertTrue(a.apply_collision_preset("none"))
        self.assertFalse(a.selected().components["collider2d"]["enabled"])
        self.assertFalse(a.vars["col_enabled"].get())

    def test_collision_fields_apply_validate_and_rollback(self):
        a = self.app
        o = a.add_object()
        self.assertNotIn("collider2d", o.components)
        a.vars["col_enabled"].set(True)
        a.vars["col_fit"].set(False)
        a.vars["cox"].set("2")
        a.vars["cw"].set("0")  # invalid
        a.vars["body_dynamic"].set(True)
        a.vars["gravity"].set("900")
        before, undo = self._snap(), len(a._undo)
        self.assertFalse(a.apply_properties())
        self.assertEqual(self._snap(), before)
        self.assertEqual(len(a._undo), undo)
        self.assertEqual(a.vars["gravity"].get(), "900")  # input kept
        self.assertTrue(a.is_dirty())
        self.assertFalse(a.save(self.dir / "bad.json"))
        a.vars["cw"].set("12")
        a.vars["ch"].set("8")
        self.assertTrue(a.apply_properties())
        col = a.selected().components["collider2d"]
        self.assertEqual((col["enabled"], col["fit_sprite"], col["offset"], col["size"]),
                         (True, False, [2.0, 0.0], [12.0, 8.0]))
        body = a.selected().components["body2d"]
        self.assertEqual((body["type"], body["gravity"], body["jump_speed"]),
                         ("dynamic", 900.0, 220.0))
        self.assertEqual(len(a._undo), undo + 1)
        self.assertTrue(a.undo())
        self.assertNotIn("collider2d", a.selected().components)
        self.assertNotIn("body2d", a.selected().components)

    def test_pending_collision_input_is_saved_not_discarded(self):
        from pixelica_editor.model import load_project
        a = self.app
        self.assertTrue(a.select_object("sample-player"))
        a.vars["gravity"].set("750")
        a.vars["jump"].set("260")
        self.assertTrue(a.is_dirty())
        path = self.dir / "physics.json"
        self.assertTrue(a.save(path))
        body = load_project(path).object_by_id("sample-player").components["body2d"]
        self.assertEqual((body["gravity"], body["jump_speed"]), (750.0, 260.0))
        self.assertFalse(a.is_dirty())
        # invalid pending physics blocks save/play and is kept
        a.vars["maxfall"].set("0")
        self.assertFalse(a.save(path))
        a.toggle_play()
        self.assertFalse(a.playing)
        self.assertEqual(a.vars["maxfall"].get(), "0")

    def test_legacy_object_stays_without_collision_components(self):
        a = self.app
        o = a.add_object()
        a.vars["x"].set("10")
        self.assertTrue(a.apply_properties())
        self.assertNotIn("collider2d", a.selected().components)
        self.assertNotIn("body2d", a.selected().components)
        self.assertFalse(a.vars["col_enabled"].get())

    def test_collision_overlay_solid_trigger_and_toggle(self):
        a = self.app
        a.select_object("sample-coin-1")
        a._draw_overlay()
        items = a.canvas.find_withtag("overlay")
        colors = {a.canvas.itemcget(i, "outline") for i in items}
        self.assertIn(editor_app.P["solid"], colors)
        self.assertIn(editor_app.P["trigger"], colors)
        a.collision_var.set(False)
        a._draw_overlay()
        colors = {a.canvas.itemcget(i, "outline") for i in a.canvas.find_withtag("overlay")}
        self.assertNotIn(editor_app.P["solid"], colors)
        self.assertNotIn(editor_app.P["trigger"], colors)

    def test_play_keys_and_collision_status(self):
        a = self.app
        a.toggle_play()
        if not a.playing:
            self.skipTest(f"재생을 시작할 수 없음: {a.status_var.get()}")
        a.press_key("space")
        self.assertIn("space", a.play_keys)
        a.release_key("space")
        self.assertNotIn("space", a.play_keys)
        a.press_key("up")
        a.step_play(5)
        text = a.play_collision_text()
        self.assertIn("접지:", text)
        self.assertIn("트리거:", text)
        self.assertIn("접지", a.info_var.get())
        self.assertEqual(a.session.project.object_by_id("sample-ground").transform.position[1], 148.0)

    def test_player_preset_restores_jump_and_disabled_collision_disables_fields(self):
        a = self.app
        a.add_object()
        self.assertTrue(a.apply_collision_preset("player"))
        a.vars["gravity"].set("0")
        a.vars["jump"].set("0")
        self.assertTrue(a.apply_properties())
        self.assertTrue(a.apply_collision_preset("player"))
        body = a.selected().components["body2d"]
        self.assertEqual((body["gravity"], body["jump_speed"]), (600.0, 220.0))
        a.vars["col_enabled"].set(False)
        a._sync_collision_widgets()
        self.assertTrue(all(w.instate(["disabled"]) for w in a._col_body))

    def test_play_collision_overlay_follows_runtime_clone(self):
        a = self.app
        a.toggle_play()
        a.step_play(1)
        a.press_key("space")
        a.step_play(5)
        a._draw_overlay()
        self.assertTrue(a.canvas.find_withtag("collider"))
        moved = a.session.project.object_by_id("sample-player").transform.position[1]
        self.assertLess(moved, a.project.object_by_id("sample-player").transform.position[1])
        a.collision_var.set(False)
        a._draw_overlay()
        self.assertFalse(a.canvas.find_withtag("collider"))

    def test_native_render_produces_image(self):
        a = self.app
        if a.renderer is None:
            self.skipTest(f"네이티브 브리지를 사용할 수 없음: {a.render_error}")
        img = a.render_now()
        self.assertEqual(img.mode, "RGB")
        self.assertEqual(img.size, (a.project.width, a.project.height))


if __name__ == "__main__":
    unittest.main()
