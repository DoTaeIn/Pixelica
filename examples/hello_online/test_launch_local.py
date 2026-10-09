"""Unit tests for launch_local.py that need no relay, no client and no network.

    python test_launch_local.py

Covers the pure parts the smoke test relies on: the room-code and board
parsers, the smoke verdict (including the "no 64-hex secret in the output"
rule) and the search order (the current directory is never searched).
"""
from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_launcher():
    spec = importlib.util.spec_from_file_location("launch_local", HERE / "launch_local.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["launch_local"] = module
    spec.loader.exec_module(module)  # type: ignore[union-attr]
    return module


launcher = load_launcher()

GOOD_HOST = [
    "room=ABCD23",
    "hello_online: role=host mode=coop self=0 max_players=4 board=coop game=hello_online",
    "hello_online: phase=finished round=1",
    "hello_online: result round=1 mode=coop team_score=9 your_score=5 players=2",
    "hello_online: score submitted board=coop score=9",
    "hello_online: board name=coop generation=2 status=0 status_text=OK count=1",
    "board 1 Host 9 mode=coop players=2 seconds=6",
    "hello_online: board name=coop generation=3 status=0 status_text=OK count=2",
    "board 1 Host 9 mode=coop players=2 seconds=6",
    "board 2 Guest 9 mode=coop players=2 seconds=6",
    "hello_online: done frames=500 round_done=1",
]
GOOD_GUEST = [
    "room=ABCD23",
    "hello_online: phase=finished round=1",
    "hello_online: result round=1 mode=coop team_score=9 your_score=4 players=2",
    "hello_online: score submitted board=coop score=9",
]


class ParserTests(unittest.TestCase):
    def test_room_code_line(self) -> None:
        self.assertEqual(launcher.ROOM_RE.match("room=ABCD23").group(1), "ABCD23")
        self.assertIsNone(launcher.ROOM_RE.match("room=ABCDO3"))  # O is not in the alphabet
        self.assertIsNone(launcher.ROOM_RE.match("room=ABCD2"))
        self.assertIsNone(launcher.ROOM_RE.match("hello_online: room=ABCD23"))

    def test_board_header_and_rows(self) -> None:
        header = launcher.BOARD_RE.match(GOOD_HOST[5])
        self.assertEqual((header.group(1), header.group(3), header.group(4)), ("coop", "0", "1"))
        row = launcher.BOARD_ROW_RE.match("board 2 Two Words -7 mode=coop players=2 seconds=6")
        self.assertEqual((row.group(2), row.group(3)), ("Two Words", "-7"))

    def test_secret_regex(self) -> None:
        self.assertIsNotNone(launcher.SECRET_RE.search("key " + "ab" * 32))
        self.assertIsNone(launcher.SECRET_RE.search("seed 12345678"))
        self.assertIsNone(launcher.SECRET_RE.search("a" * 65))  # longer run is not a 64-hex token


class SmokeVerdictTests(unittest.TestCase):
    def test_good_run_passes(self) -> None:
        self.assertEqual(launcher.check_smoke("coop", GOOD_HOST, GOOD_GUEST), [])

    def test_secret_in_output_fails(self) -> None:
        leaked = GOOD_GUEST + ["debug " + "0f" * 32]
        problems = launcher.check_smoke("coop", GOOD_HOST, leaked)
        self.assertTrue(any("64-character" in p for p in problems))

    def test_missing_guest_on_board_fails(self) -> None:
        host = [line for line in GOOD_HOST if "Guest" not in line]
        host = [line.replace("count=2", "count=1") for line in host]
        problems = launcher.check_smoke("coop", host, GOOD_GUEST)
        self.assertTrue(any("Host and Guest" in p for p in problems))

    def test_unfinished_round_and_wrong_board_fail(self) -> None:
        problems = launcher.check_smoke("versus", GOOD_HOST, ["room=ABCD23"])
        text = "\n".join(problems)
        self.assertIn("guest: the round never finished", text)
        self.assertIn("mode=versus not reported", text)
        self.assertIn("expected 'versus'", text)

    def test_board_error_status_fails(self) -> None:
        host = [line.replace("status=0 status_text=OK", "status=7 status_text=RATE_LIMIT") for line in GOOD_HOST]
        problems = launcher.check_smoke("coop", host, GOOD_GUEST)
        self.assertTrue(any("board status code 7" in p for p in problems))


class SearchTests(unittest.TestCase):
    def test_explicit_paths_win_and_missing_is_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            server = Path(tmp) / "server.py"
            server.write_text("# fake", encoding="utf-8")
            self.assertEqual(launcher.find_server_script(str(server)), server.resolve())
            self.assertIsNone(launcher.find_client_exe(str(Path(tmp) / "nope")))

    def test_current_directory_is_never_searched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            planted = Path(tmp) / "tools" / "online_server"
            planted.mkdir(parents=True)
            (planted / "server.py").write_text("raise SystemExit('planted')", encoding="utf-8")
            previous = os.getcwd()
            os.chdir(tmp)
            try:
                found = launcher.find_server_script(None)
            finally:
                os.chdir(previous)
            self.assertNotEqual(found, (planted / "server.py").resolve())

    def test_free_port_is_usable(self) -> None:
        port = launcher.free_port()
        self.assertTrue(1024 <= port <= 65535)


if __name__ == "__main__":
    unittest.main()
