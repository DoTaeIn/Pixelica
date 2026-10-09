#!/usr/bin/env python3
"""One-command local launcher for hello_online (Python 3.10+, standard library only).

    python launch_local.py --exe path/to/hello_online --mode coop
    python launch_local.py --exe path/to/hello_online_headless --smoke

It starts the local relay (tools/online_server/server.py) on a free 127.0.0.1
port with a throw-away SQLite file, starts the HOST client, reads the room code
the host prints ("room=ABCDEF"), starts a second GUEST client with --join, and
cleans everything up when the host exits, on Ctrl-C, or when --timeout expires.

  * default      host = you (SDL window, keyboard), guest = autopilot bot;
                 --both-human makes the guest a second window too;
  * --smoke      both clients are bots without a window, a short round, then
                 the output is checked (room code, finished round, both scores on
                 the board, no 64-hex secret in any output) -> exit status 0/1.

Process lifetime is bounded: every child is terminated (then killed) in a
finally block, and --timeout is a hard overall limit. The launcher never
handles player keys; it passes none and prints none. It only talks to
127.0.0.1 unless you pass --server-url (an already running relay, e.g. https://).

Exit status: 0 success, 1 failure, 2 usage / file not found.

NOTE: this file exists twice, byte for byte: examples/hello_online/launch_local.py
and templates/online/launch_local.py (the template must be copyable on its own).
Any client that follows the same command line and stdout protocol works with it
(see docs/online.md, "로컬 런처").
"""
from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOM_RE = re.compile(r"^room=([A-HJ-NP-Z2-9]{6})\s*$")
SECRET_RE = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{64}(?![0-9A-Fa-f])")
BOARD_RE = re.compile(r"^hello_online: board name=(\S+) generation=(\d+) status=(\d+) .*count=(\d+)")
BOARD_ROW_RE = re.compile(r"^board (\d+) (.+) (-?\d+)(?: .*)?$")
MAX_STORED_LINES = 5000

_print_lock = threading.Lock()


def emit(text: str) -> None:
    with _print_lock:
        print(text, flush=True)


def exe_name(base: str) -> str:
    return base + (".exe" if os.name == "nt" else "")


class Child:
    """A subprocess whose stdout/stderr is echoed with a prefix and kept for checks."""

    def __init__(self, label: str, argv: list[str], echo: bool = True) -> None:
        self.label = label
        self.argv = argv
        self.echo = echo
        self.lines: list[str] = []
        self._cond = threading.Condition()
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self._thread = threading.Thread(target=self._pump, name=f"pump-{label}", daemon=True)
        self._thread.start()

    def _pump(self) -> None:
        try:
            for raw in self.proc.stdout:  # type: ignore[union-attr]
                line = raw.rstrip("\r\n")
                with self._cond:
                    if len(self.lines) < MAX_STORED_LINES:
                        self.lines.append(line)
                    self._cond.notify_all()
                if self.echo:
                    emit(f"[{self.label}] {line}")
        except ValueError:
            pass  # pipe closed while terminating
        finally:
            with self._cond:
                self._cond.notify_all()

    def wait_for(self, pattern: re.Pattern[str], timeout: float) -> re.Match[str] | None:
        """First output line matching `pattern`, or None (timeout / process ended)."""
        end = time.monotonic() + timeout
        seen = 0
        with self._cond:
            while True:
                while seen < len(self.lines):
                    match = pattern.search(self.lines[seen])
                    seen += 1
                    if match:
                        return match
                if self.proc.poll() is not None and not self._thread.is_alive():
                    return None
                remaining = end - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(min(remaining, 0.2))

    def finished(self) -> bool:
        return self.proc.poll() is not None

    def collect(self) -> list[str]:
        self._thread.join(timeout=2.0)
        with self._cond:
            return list(self.lines)

    def stop(self, grace: float) -> None:
        """Let it exit by itself for `grace` seconds, then terminate, then kill."""
        if self.proc.poll() is None:
            try:
                self.proc.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._thread.join(timeout=2.0)
        if self.proc.stdout is not None:
            self.proc.stdout.close()


def candidate_roots() -> list[Path]:
    """Folders around THIS script. The current directory is deliberately not searched,
    so running the launcher inside an untrusted folder never starts a program from it."""
    here = Path(__file__).resolve().parent
    return [here, *list(here.parents)[:5]]


def find_server_script(explicit: str | None) -> Path | None:
    candidates: list[Path] = []
    for value in (explicit, os.environ.get("PIXELICA_ONLINE_SERVER")):
        if value:
            candidates.append(Path(value))
    for root in candidate_roots():
        candidates += [
            root / "tools" / "online_server" / "server.py",
            root / "online_server" / "server.py",
            root / "share" / "Pixelica" / "online_server" / "server.py",
        ]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    return None


def find_client_exe(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path.resolve() if path.is_file() else None
    names = [exe_name("hello_online"), exe_name("hello_online_headless"), exe_name("online_game")]
    subdirs = ["", "build", "build/Release", "build/Debug", "build/bin", "build/bin/Release", "bin"]
    for root in candidate_roots():
        for sub in subdirs:
            for name in names:
                path = root / sub / name
                if path.is_file():
                    return path.resolve()
    return None


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_health(url: str, server: Child | None, timeout: float) -> bool:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # never via a system proxy
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if server is not None and server.finished():
            return False
        try:
            with opener.open(url.rstrip("/") + "/health", timeout=1.0) as response:
                if response.status == 200:
                    return True
        except (urllib.error.URLError, OSError, ValueError):
            pass
        time.sleep(0.15)
    return False


def check_smoke(mode: str, host_lines: list[str], guest_lines: list[str]) -> list[str]:
    """Problems found in the bots' output (empty list = the smoke test passed)."""
    problems: list[str] = []
    joined = "\n".join(host_lines + guest_lines)
    if SECRET_RE.search(joined):
        problems.append("a 64-character hex string (possible player key) appeared in the output")
    for label, lines in (("host", host_lines), ("guest", guest_lines)):
        text = "\n".join(lines)
        if "phase=finished" not in text:
            problems.append(f"{label}: the round never finished")
        if "score submitted" not in text:
            problems.append(f"{label}: no score was submitted")
        if "result round=1" not in text:
            problems.append(f"{label}: no result line")
    if not any(f"mode={mode}" in line for line in host_lines):
        problems.append(f"host: mode={mode} not reported")
    # last board report of the host: after the lingering re-fetch it must hold both players
    last_header = None
    rows: list[tuple[str, int]] = []
    for line in host_lines:
        header = BOARD_RE.match(line)
        if header:
            last_header = header
            rows = []
            continue
        row = BOARD_ROW_RE.match(line)
        if row and last_header is not None:
            rows.append((row.group(2), int(row.group(3))))
    if last_header is None:
        problems.append("host: no board was reported")
    else:
        if last_header.group(1) != mode:
            problems.append(f"host: board name is {last_header.group(1)!r}, expected {mode!r}")
        if last_header.group(3) != "0":
            problems.append(f"host: board status code {last_header.group(3)}")
        names = {name for name, _ in rows}
        if not {"Host", "Guest"} <= names:
            problems.append(f"host: board should hold Host and Guest, saw {sorted(names)}")
    return problems


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Start a local relay and two hello_online clients (host + guest).")
    parser.add_argument("--exe", help="hello_online executable (default: searched in the usual build dirs)")
    parser.add_argument("--server", help="path of tools/online_server/server.py (default: searched; "
                                         "or env PIXELICA_ONLINE_SERVER)")
    parser.add_argument("--server-url", help="use this already running relay instead of starting one")
    parser.add_argument("--mode", choices=("coop", "versus"), default="coop")
    parser.add_argument("--smoke", action="store_true",
                        help="two windowless bots, short round, verify the output, exit 0/1")
    parser.add_argument("--both-human", action="store_true", help="the guest is a window controlled by you too")
    parser.add_argument("--round-seconds", type=int, help="round length (default 45, smoke 6)")
    parser.add_argument("--port", type=int, default=0, help="relay port (default: a free one)")
    parser.add_argument("--db", help="SQLite file to KEEP between runs (default: a deleted temp file)")
    parser.add_argument("--game", default="hello_online", help="game id (boards are per game id)")
    parser.add_argument("--timeout", type=float, help="hard limit in seconds (default 600, smoke 90)")
    args = parser.parse_args(argv)
    if args.port < 0 or args.port > 65535:
        parser.error("--port must be 0..65535")
    if args.round_seconds is not None and not 3 <= args.round_seconds <= 300:
        parser.error("--round-seconds must be 3..300")
    if args.timeout is not None and not 5 <= args.timeout <= 86400:
        parser.error("--timeout must be 5..86400")
    return args


def run(args: argparse.Namespace) -> int:
    exe = find_client_exe(args.exe)
    if exe is None:
        emit("error: hello_online executable not found; pass --exe PATH")
        return 2
    server_script: Path | None = None
    if not args.server_url:
        server_script = find_server_script(args.server)
        if server_script is None:
            emit("error: tools/online_server/server.py not found; pass --server PATH or --server-url URL")
            return 2

    round_seconds = args.round_seconds or (6 if args.smoke else 45)
    timeout = args.timeout or (90.0 if args.smoke else 600.0)
    deadline = time.monotonic() + timeout

    children: list[Child] = []
    temp_dir: tempfile.TemporaryDirectory[str] | None = None
    status = 1
    try:
        server: Child | None = None
        if args.server_url:
            url = args.server_url.rstrip("/")
        else:
            port = args.port or free_port()
            url = f"http://127.0.0.1:{port}"
            if args.db:
                db_path = Path(args.db).resolve()
            else:
                temp_dir = tempfile.TemporaryDirectory(prefix="hello_online_")
                db_path = Path(temp_dir.name) / "online.sqlite3"
            assert server_script is not None
            # -E: ignore PYTHON* environment variables; the relay is stdlib only.
            server = Child("server", [sys.executable, "-E", str(server_script), "--host", "127.0.0.1",
                                      "--port", str(port), "--db", str(db_path)])
            children.append(server)
        if not wait_for_health(url, server, min(20.0, max(1.0, deadline - time.monotonic()))):
            emit("error: the relay did not become healthy")
            return 1
        emit(f"relay ready at {url}")

        common = ["--server", url, "--game", args.game]
        frames = (round_seconds + 30) * 60
        host_argv = [str(exe), *common, "--host", args.mode, "--name", "Host", "--players", "2",
                     "--round-seconds", str(round_seconds)]
        guest_bot = args.smoke or not args.both_human
        if args.smoke:
            host_argv += ["--bot", "--hidden", "--exit-after-round", "--frames", str(frames),
                          "--linger-seconds", "3"]
        host = Child("host", host_argv)
        children.append(host)
        match = host.wait_for(ROOM_RE, min(20.0, max(1.0, deadline - time.monotonic())))
        if match is None:
            emit("error: the host did not print a room code")
            return 1
        room = match.group(1)
        emit(f"room code {room}: starting the guest")

        guest_argv = [str(exe), *common, "--join", room, "--name", "Guest"]
        if guest_bot:
            guest_argv.append("--bot")
        if args.smoke:
            guest_argv += ["--hidden", "--exit-after-round", "--frames", str(frames)]
        guest = Child("guest", guest_argv)
        children.append(guest)

        while time.monotonic() < deadline:
            if host.finished() and (guest.finished() or not args.smoke):
                break
            if server is not None and server.finished():
                emit("error: the relay stopped unexpectedly")
                return 1
            time.sleep(0.2)
        else:
            emit(f"error: timeout after {timeout:.0f} s")
            return 1

        host.stop(grace=1.0)
        guest.stop(grace=5.0 if args.smoke else 1.0)
        host_rc = host.proc.returncode
        guest_rc = guest.proc.returncode if args.smoke else 0
        if args.smoke:
            problems = check_smoke(args.mode, host.collect(), guest.collect())
            if host_rc != 0:
                problems.append(f"host exited with status {host_rc}")
            if guest_rc != 0:
                problems.append(f"guest exited with status {guest_rc}")
            if problems:
                for problem in problems:
                    emit(f"smoke FAILED: {problem}")
                return 1
            emit("smoke OK: room, round, scores and board verified")
            status = 0
        else:
            status = 0 if host_rc in (0, None) else 1
        return status
    except KeyboardInterrupt:
        emit("interrupted")
        return 130
    finally:
        # clients first so they can leave their room, the relay last
        for child in reversed(children):
            child.stop(grace=3.0 if child.label != "server" else 0.0)
        if temp_dir is not None:
            temp_dir.cleanup()


def main(argv: list[str]) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    if sys.version_info < (3, 10):
        print("Python 3.10 or newer is required", file=sys.stderr)
        return 2
    args = parse_args(argv)

    def on_sigterm(signum: int, frame: object) -> None:
        raise KeyboardInterrupt  # runs the same cleanup as Ctrl-C

    signal.signal(signal.SIGTERM, on_sigterm)
    return run(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
