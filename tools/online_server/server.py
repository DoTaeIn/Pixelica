#!/usr/bin/env python3
"""Pixelica online relay server (wire protocol v1).

Standard library only (Python 3.10+).  One process, in-memory rooms, SQLite
casual highest-score boards.

    python server.py                       # 127.0.0.1:8787, ./pixelica_online.sqlite3
    python server.py --host 0.0.0.0 --port 8080 --db /data/online.sqlite3

Honest scope: this is a casual relay for co-op / versus games that tolerate
latest-state HTTP polling (about 30 Hz).  It is NOT rollback netcode, NOT UDP,
and the score boards are self-reported by clients: they are NOT cheat-proof
and must not be presented as ranked / anti-cheat leaderboards.

Plain HTTP is spoken here.  For public use put it behind the included Caddy
HTTPS reverse proxy (see README.md); never expose the plain port to the
internet.

Secrets (session tokens, player keys) are never logged.  Per-request access
logging is disabled entirely.
"""
from __future__ import annotations

import argparse
import collections
import dataclasses
import hashlib
import io
import ipaddress
import logging
import os
import re
import secrets
import signal
import socket
import socketserver
import sqlite3
import struct
import sys
import threading
import time
import unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

log = logging.getLogger("pixelica.online")

# --------------------------------------------------------------------------
# Wire constants (frozen contract, protocol v1)
# --------------------------------------------------------------------------
MAGIC = b"P3ON"
VERSION = 1
ROUTE = "/v1/online"
HEADER_LEN = 8
MAX_PACKET = 4096

OP_HOST, OP_JOIN, OP_SYNC, OP_LEAVE, OP_SCORE, OP_BOARD = 1, 2, 3, 4, 5, 6

ST_OK = 0
ST_BAD_REQUEST = 1
ST_NOT_FOUND = 2
ST_FULL = 3
ST_UNAUTHORIZED = 4
ST_NOT_HOST = 5
ST_CAPACITY = 6
ST_RATE_LIMIT = 7
ST_ROOM_CLOSED = 8
ST_INTERNAL = 9

MODE_COOP, MODE_VERSUS = 1, 2
MAX_PLAYERS = 8
MAX_INPUT = 64
MAX_STATE = 2048
MAX_RECORD = 128
MAX_SCORES = 10
MAX_TEXT = 32

CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
CODE_LEN = 6
_CODE_BYTES = CODE_ALPHABET.encode("ascii")

MASK32 = 0xFFFFFFFF
_ID_RE = re.compile(rb"[A-Za-z0-9_.-]{1,32}")

# Per-session rate limits (token buckets: rate per second, burst capacity).
FRAME_RATE, FRAME_BURST = 90.0, 10.0   # SYNC
SCORE_RATE, SCORE_BURST = 2.0, 2.0     # SCORE
BOARD_RATE, BOARD_BURST = 5.0, 5.0     # BOARD

MAX_TOMBSTONES = 8192
TOMBSTONE_TTL = 300.0
MAX_REQUEST_BYTES = 32768              # headers + body hard cap per request


class ProtocolError(Exception):
    """Application-level error: answered with HTTP 200 and a status packet."""

    def __init__(self, status: int):
        super().__init__(status)
        self.status = status


class TransportError(Exception):
    """Packet cannot be answered in-protocol: the HTTP layer returns 400."""


# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------
@dataclasses.dataclass
class ServerConfig:
    host: str = "127.0.0.1"
    port: int = 8787
    db_path: str = "pixelica_online.sqlite3"
    session_ttl: float = 30.0
    max_rooms: int = 256
    max_sessions: int = 2048
    max_inflight: int = 64            # concurrent HTTP connections (threads)
    max_records: int = 100000         # total score rows in the database
    ip_rate: float = 600.0            # requests/s per client address
    ip_burst: float = 1200.0
    create_rate: float = 2.0          # HOST+JOIN per second per client address
    create_burst: float = 30.0
    ip_score_rate: float = 5.0        # SCORE per second per client address
    ip_score_burst: float = 20.0
    limiter_entries: int = 20000      # max tracked addresses per limiter table
    trusted_proxies: tuple = ()       # CIDRs allowed to set X-Forwarded-For
    request_timeout: float = 10.0     # whole request (headers+body+reply)
    idle_timeout: float = 30.0        # keep-alive idle / first request wait

    def validate(self) -> "ServerConfig":
        def chk(cond, what):
            if not cond:
                raise ValueError("invalid " + what)

        chk(isinstance(self.host, str) and self.host != "", "host")
        chk(isinstance(self.port, int) and 0 <= self.port <= 65535, "port")
        chk(isinstance(self.db_path, str) and self.db_path != "", "db path")
        chk(0 < self.session_ttl <= 3600, "session ttl (0..3600 s)")
        chk(1 <= self.max_rooms <= 100000, "max rooms")
        chk(1 <= self.max_sessions <= 1000000, "max sessions")
        chk(1 <= self.max_inflight <= 10000, "max inflight")
        chk(1 <= self.max_records <= 100000000, "max records")
        chk(self.ip_rate > 0 and self.ip_burst >= 1, "ip rate")
        chk(self.create_rate > 0 and self.create_burst >= 1, "create rate")
        chk(self.ip_score_rate > 0 and self.ip_score_burst >= 1, "score rate")
        chk(16 <= self.limiter_entries <= 10000000, "limiter entries")
        chk(0 < self.request_timeout <= 120, "request timeout")
        chk(0 < self.idle_timeout <= 600, "idle timeout")
        self.trusted_networks()
        return self

    def trusted_networks(self) -> list:
        nets = []
        for item in self.trusted_proxies:
            try:
                nets.append(ipaddress.ip_network(str(item).strip(), strict=False))
            except ValueError:
                raise ValueError("invalid trusted proxy: %r" % (item,)) from None
        return nets


# --------------------------------------------------------------------------
# Binary helpers
# --------------------------------------------------------------------------
class _Reader:
    """Strict big-endian reader; every shortfall is BAD_REQUEST."""

    __slots__ = ("_b", "_p")

    def __init__(self, data: bytes):
        self._b = data
        self._p = 0

    def take(self, n: int) -> bytes:
        end = self._p + n
        if n < 0 or end > len(self._b):
            raise ProtocolError(ST_BAD_REQUEST)
        out = self._b[self._p:end]
        self._p = end
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack(">H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack(">I", self.take(4))[0]

    def i32(self) -> int:
        return struct.unpack(">i", self.take(4))[0]

    def text_bytes(self) -> bytes:
        n = self.u8()
        return self.take(n)

    def done(self) -> None:
        if self._p != len(self._b):
            raise ProtocolError(ST_BAD_REQUEST)


def _header(op: int, status: int) -> bytes:
    return MAGIC + struct.pack(">BBH", VERSION, op & 0xFF, status)


def _text(s: str) -> bytes:
    b = s.encode("utf-8")
    return bytes((len(b),)) + b


def _ident(raw: bytes) -> str:
    """game / board id: [A-Za-z0-9_.-]{1,32}."""
    if _ID_RE.fullmatch(raw) is None:
        raise ProtocolError(ST_BAD_REQUEST)
    return raw.decode("ascii")


def _name(raw: bytes) -> str:
    """Player name: non-empty strict UTF-8, <= 32 bytes, no control chars."""
    if not 1 <= len(raw) <= MAX_TEXT:
        raise ProtocolError(ST_BAD_REQUEST)
    try:
        s = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise ProtocolError(ST_BAD_REQUEST) from None
    for ch in s:
        if unicodedata.category(ch) == "Cc":
            raise ProtocolError(ST_BAD_REQUEST)
    return s


def _seq_update(cur_seq: int, cur: bytes, new_seq: int, new: bytes) -> bool:
    """Decide whether (new_seq,new) replaces (cur_seq,cur).

    Returns True to apply, False for a no-op (seq 0/empty, idempotent
    duplicate, stale).  Raises BAD_REQUEST for seq 0 with data or for a
    duplicate seq carrying different bytes.  Never mutates.
    """
    if new_seq == 0:
        if new:
            raise ProtocolError(ST_BAD_REQUEST)
        return False
    if new_seq == cur_seq:
        if new != cur:
            raise ProtocolError(ST_BAD_REQUEST)
        return False
    if cur_seq == 0:
        return True
    return 0 < ((new_seq - cur_seq) & MASK32) < 0x80000000


def _identity_hash(key: bytes) -> bytes:
    return hashlib.sha256(b"pixelica-online-identity-v1\0" + key).digest()


# --------------------------------------------------------------------------
# Rate limiting
# --------------------------------------------------------------------------
class _Limiter:
    """Single token bucket (per session)."""

    __slots__ = ("rate", "cap", "tokens", "last")

    def __init__(self, rate: float, cap: float, now: float):
        self.rate = rate
        self.cap = cap
        self.tokens = cap
        self.last = now

    def allow(self, now: float) -> bool:
        if now > self.last:
            self.tokens = min(self.cap, self.tokens + (now - self.last) * self.rate)
            self.last = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False


class TokenBucketTable:
    """Keyed token buckets with hard-bounded memory.

    The table is an LRU: using a key moves it to the back; when full the
    least-recently-used key is evicted, and purge() drops keys idle long
    enough to have fully refilled (equivalent to being absent).
    """

    def __init__(self, rate: float, burst: float, max_entries: int):
        self._rate = float(rate)
        self._burst = float(burst)
        self._max = int(max_entries)
        self._d: "collections.OrderedDict[str, list]" = collections.OrderedDict()
        self._lock = threading.Lock()

    def allow(self, key: str, now: float, cost: float = 1.0) -> bool:
        with self._lock:
            ent = self._d.get(key)
            if ent is None:
                if len(self._d) >= self._max:
                    self._d.popitem(last=False)
                ent = [self._burst, now]
                self._d[key] = ent
            else:
                if now > ent[1]:
                    ent[0] = min(self._burst, ent[0] + (now - ent[1]) * self._rate)
                    ent[1] = now
                self._d.move_to_end(key)
            if ent[0] >= cost:
                ent[0] -= cost
                return True
            return False

    def purge(self, now: float) -> None:
        horizon = self._burst / self._rate
        with self._lock:
            while self._d:
                key = next(iter(self._d))
                if now - self._d[key][1] >= horizon:
                    del self._d[key]
                else:
                    break

    def __len__(self) -> int:
        with self._lock:
            return len(self._d)


def _norm_ip(text: str):
    try:
        ip = ipaddress.ip_address(text.strip().split("%", 1)[0])
    except ValueError:
        return None
    if ip.version == 6 and ip.ipv4_mapped is not None:
        ip = ip.ipv4_mapped
    return ip


def _limiter_key(ip) -> str:
    if ip.version == 6:  # one /64 is one subscriber; stops per-address rotation
        return str(ipaddress.ip_network((ip, 64), strict=False))
    return str(ip)


def resolve_client_key(peer: str, xff_values, trusted: list) -> str:
    """Limiter key for a connection.

    X-Forwarded-For is honoured ONLY when the TCP peer is inside a configured
    trusted proxy network; entries are then consumed right-to-left until the
    first address that is not itself a trusted proxy.  Anything malformed
    falls back to the last trusted hop (never to attacker-supplied text).
    """
    ip = _norm_ip(peer)
    if ip is None:
        return "unknown"
    if trusted and xff_values and any(ip in n for n in trusted):
        hops = []
        for value in xff_values:
            hops.extend(value.split(","))
        for part in reversed(hops):
            cand = _norm_ip(part)
            if cand is None:
                break
            ip = cand
            if not any(cand in n for n in trusted):
                break
    return _limiter_key(ip)


# --------------------------------------------------------------------------
# SQLite score store
# --------------------------------------------------------------------------
class ScoreStore:
    """Highest score per (game, board, identity); one process, one connection."""

    def __init__(self, path: str, max_records: int):
        if path != ":memory:":
            parent = os.path.dirname(os.path.abspath(path))
            os.makedirs(parent, exist_ok=True)
        self._lock = threading.Lock()
        self._max = int(max_records)
        self._closed = False
        self._db = sqlite3.connect(path, check_same_thread=False,
                                   isolation_level=None, timeout=5.0)
        try:
            db = self._db
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("PRAGMA synchronous=NORMAL")
            ver = db.execute("PRAGMA user_version").fetchone()[0]
            if ver not in (0, 1):
                raise RuntimeError("unsupported database schema version")
            db.execute(
                "CREATE TABLE IF NOT EXISTS scores ("
                " game TEXT NOT NULL, board TEXT NOT NULL,"
                " identity BLOB NOT NULL, name TEXT NOT NULL,"
                " score INTEGER NOT NULL, record BLOB NOT NULL,"
                " seq INTEGER NOT NULL,"
                " PRIMARY KEY (game, board, identity)) WITHOUT ROWID")
            db.execute(
                "CREATE INDEX IF NOT EXISTS scores_rank ON scores"
                " (game, board, score DESC, seq ASC, identity ASC)")
            db.execute("PRAGMA user_version=1")
            self._rows = db.execute("SELECT COUNT(*) FROM scores").fetchone()[0]
            self._seq = db.execute("SELECT COALESCE(MAX(seq),0) FROM scores").fetchone()[0]
        except BaseException:
            self._db.close()
            raise

    @property
    def closed(self) -> bool:
        return self._closed

    def close(self) -> None:
        with self._lock:
            if not self._closed:
                self._closed = True
                try:
                    self._db.close()
                except sqlite3.Error:
                    pass

    def _top_locked(self, game: str, board: str) -> list:
        rows = self._db.execute(
            "SELECT name, score, record FROM scores WHERE game=? AND board=?"
            " ORDER BY score DESC, seq ASC, identity ASC LIMIT ?",
            (game, board, MAX_SCORES)).fetchall()
        return [(n, int(s), bytes(r)) for (n, s, r) in rows]

    def top(self, game: str, board: str) -> tuple:
        with self._lock:
            try:
                return ST_OK, self._top_locked(game, board)
            except sqlite3.Error as exc:
                log.error("database error: %s", type(exc).__name__)
                return ST_INTERNAL, []

    def submit(self, game: str, board: str, identity: bytes, name: str,
               score: int, record: bytes) -> tuple:
        """Atomic: insert a new identity or raise its score; returns (status, top10)."""
        with self._lock:
            db = self._db
            try:
                db.execute("BEGIN IMMEDIATE")
                try:
                    row = db.execute(
                        "SELECT score FROM scores WHERE game=? AND board=? AND identity=?",
                        (game, board, identity)).fetchone()
                    inserted = False
                    if row is None:
                        if self._rows >= self._max:
                            db.execute("ROLLBACK")
                            return ST_CAPACITY, []
                        self._seq += 1
                        db.execute(
                            "INSERT INTO scores (game, board, identity, name, score, record, seq)"
                            " VALUES (?,?,?,?,?,?,?)",
                            (game, board, identity, name, score, record, self._seq))
                        inserted = True
                    elif score > row[0]:
                        self._seq += 1
                        db.execute(
                            "UPDATE scores SET name=?, score=?, record=?, seq=?"
                            " WHERE game=? AND board=? AND identity=?",
                            (name, score, record, self._seq, game, board, identity))
                    db.execute("COMMIT")
                    if inserted:
                        self._rows += 1
                except BaseException:
                    try:
                        db.execute("ROLLBACK")
                    except sqlite3.Error:
                        pass
                    raise
                return ST_OK, self._top_locked(game, board)
            except sqlite3.Error as exc:
                log.error("database error: %s", type(exc).__name__)
                return ST_INTERNAL, []


# --------------------------------------------------------------------------
# Relay core (transport independent, fully unit-testable)
# --------------------------------------------------------------------------
class _Session:
    __slots__ = ("digest", "sid", "name", "key_hash", "room", "last_seen",
                 "input_seq", "input", "frame_lim", "score_lim", "board_lim")


class _Room:
    __slots__ = ("code", "game", "mode", "max_players", "players", "revision",
                 "state_seq", "state")

    def __init__(self, code: str, game: str, mode: int, max_players: int):
        self.code = code
        self.game = game
        self.mode = mode
        self.max_players = max_players
        self.players: dict = {}
        self.revision = 0
        self.state_seq = 0
        self.state = b""


class RelayService:
    """Protocol engine: bytes in, bytes out.  Thread safe."""

    def __init__(self, config: ServerConfig, store: ScoreStore, clock=time.monotonic):
        self._cfg = config
        self._store = store
        self._clock = clock
        self._ttl = float(config.session_ttl)
        self._lock = threading.Lock()
        self._rooms: dict = {}
        self._sessions: dict = {}      # sha256(token) -> _Session
        self._tomb: "collections.OrderedDict[bytes, float]" = collections.OrderedDict()
        self._last_sweep = clock()
        self._sweep_interval = min(1.0, self._ttl / 4.0)
        n = config.limiter_entries
        self._ip_lim = TokenBucketTable(config.ip_rate, config.ip_burst, n)
        self._create_lim = TokenBucketTable(config.create_rate, config.create_burst, n)
        self._score_lim = TokenBucketTable(config.ip_score_rate, config.ip_score_burst, n)
        self._handlers = {
            OP_HOST: self._op_host, OP_JOIN: self._op_join, OP_SYNC: self._op_sync,
            OP_LEAVE: self._op_leave, OP_SCORE: self._op_score, OP_BOARD: self._op_board,
        }

    # ---- public ---------------------------------------------------------
    def handle(self, packet: bytes, client_key: str) -> bytes:
        """Process one request packet.  Raises TransportError if unanswerable."""
        if (len(packet) < HEADER_LEN or len(packet) > MAX_PACKET
                or packet[:4] != MAGIC or packet[4] != VERSION):
            raise TransportError()
        op = packet[5]
        status = struct.unpack(">H", packet[6:8])[0]
        resp_op = op | 0x80
        try:
            if status != 0 or op not in self._handlers:
                raise ProtocolError(ST_BAD_REQUEST)
            now = self._clock()
            if not self._ip_lim.allow(client_key, now):
                raise ProtocolError(ST_RATE_LIMIT)
            body = self._handlers[op](_Reader(packet[HEADER_LEN:]), client_key, now)
            return _header(resp_op, ST_OK) + body
        except ProtocolError as exc:
            return _header(resp_op, exc.status)
        except Exception as exc:  # never leak details; class name only
            log.error("internal error: %s", type(exc).__name__)
            return _header(resp_op, ST_INTERNAL)

    def sweep(self) -> None:
        """Expire leases now (called by the janitor thread)."""
        now = self._clock()
        with self._lock:
            self._sweep_locked(now, force=True)
        self._ip_lim.purge(now)
        self._create_lim.purge(now)
        self._score_lim.purge(now)

    def stats(self) -> dict:
        with self._lock:
            return {"rooms": len(self._rooms), "sessions": len(self._sessions),
                    "tombstones": len(self._tomb)}

    # ---- lease / room bookkeeping (call with self._lock held) -----------
    def _tombstone_locked(self, digest: bytes, now: float) -> None:
        self._tomb[digest] = now
        self._tomb.move_to_end(digest)
        while len(self._tomb) > MAX_TOMBSTONES:
            self._tomb.popitem(last=False)

    def _close_room_locked(self, room: _Room, now: float, revoked=None) -> None:
        self._rooms.pop(room.code, None)
        for s in room.players.values():
            self._sessions.pop(s.digest, None)
            if s is not revoked:
                self._tombstone_locked(s.digest, now)
        room.players.clear()

    def _remove_guest_locked(self, room: _Room, s: _Session) -> None:
        room.players.pop(s.sid, None)
        self._sessions.pop(s.digest, None)
        room.revision += 1

    def _sweep_locked(self, now: float, force: bool = False) -> None:
        if not force and now - self._last_sweep < self._sweep_interval:
            return
        self._last_sweep = now
        ttl = self._ttl
        for room in list(self._rooms.values()):
            host = room.players.get(0)
            if host is None or now - host.last_seen > ttl:
                self._close_room_locked(room, now)
                continue
            for s in [p for p in room.players.values()
                      if p.sid != 0 and now - p.last_seen > ttl]:
                self._remove_guest_locked(room, s)
        while self._tomb:
            digest = next(iter(self._tomb))
            if now - self._tomb[digest] > TOMBSTONE_TTL:
                del self._tomb[digest]
            else:
                break

    def _lookup_locked(self, token: bytes, now: float) -> _Session:
        digest = hashlib.sha256(token).digest()
        s = self._sessions.get(digest)
        if s is not None and now - s.last_seen > self._ttl:
            self._sweep_locked(now, force=True)
            s = self._sessions.get(digest)
        if s is None:
            raise ProtocolError(ST_ROOM_CLOSED if digest in self._tomb
                                else ST_UNAUTHORIZED)
        return s

    def _new_code_locked(self) -> str:
        for _ in range(32):
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self._rooms:
                return code
        raise ProtocolError(ST_CAPACITY)

    def _add_session_locked(self, room: _Room, sid: int, name: str, key: bytes,
                            now: float) -> tuple:
        token = secrets.token_bytes(32)
        s = _Session()
        s.digest = hashlib.sha256(token).digest()
        s.sid = sid
        s.name = name
        s.key_hash = _identity_hash(key)
        s.room = room
        s.last_seen = now
        s.input_seq = 0
        s.input = b""
        s.frame_lim = _Limiter(FRAME_RATE, FRAME_BURST, now)
        s.score_lim = _Limiter(SCORE_RATE, SCORE_BURST, now)
        s.board_lim = _Limiter(BOARD_RATE, BOARD_BURST, now)
        room.players[sid] = s
        self._sessions[s.digest] = s
        return s, token

    @staticmethod
    def _snapshot(room: _Room) -> bytes:
        parts = [struct.pack(">IBBBBIH", room.revision & MASK32, 0, len(room.players),
                             room.mode, room.max_players, room.state_seq,
                             len(room.state)), room.state]
        for sid in sorted(room.players):
            p = room.players[sid]
            parts.append(bytes((sid,)))
            parts.append(_text(p.name))
            parts.append(struct.pack(">IB", p.input_seq, len(p.input)))
            parts.append(p.input)
        return b"".join(parts)

    @staticmethod
    def _board_result(board: str, entries: list) -> bytes:
        parts = [_text(board), bytes((len(entries),))]
        for name, score, record in entries:
            parts.append(_text(name))
            parts.append(struct.pack(">iB", score, len(record)))
            parts.append(record)
        return b"".join(parts)

    @staticmethod
    def _resolve_key(key: bytes) -> bytes:
        return secrets.token_bytes(32) if key == bytes(32) else key

    # ---- operations -----------------------------------------------------
    def _op_host(self, r: _Reader, ip: str, now: float) -> bytes:
        game = _ident(r.text_bytes())
        name = _name(r.text_bytes())
        key = r.take(32)
        mode = r.u8()
        max_players = r.u8()
        r.done()
        if mode not in (MODE_COOP, MODE_VERSUS) or not 2 <= max_players <= MAX_PLAYERS:
            raise ProtocolError(ST_BAD_REQUEST)
        if not self._create_lim.allow(ip, now):
            raise ProtocolError(ST_RATE_LIMIT)
        key = self._resolve_key(key)
        with self._lock:
            self._sweep_locked(now)
            if (len(self._rooms) >= self._cfg.max_rooms
                    or len(self._sessions) >= self._cfg.max_sessions):
                raise ProtocolError(ST_CAPACITY)
            code = self._new_code_locked()
            room = _Room(code, game, mode, max_players)
            self._rooms[code] = room
            _, token = self._add_session_locked(room, 0, name, key, now)
            room.revision += 1
            return (token + key + code.encode("ascii") + bytes((0,))
                    + self._snapshot(room))

    def _op_join(self, r: _Reader, ip: str, now: float) -> bytes:
        game = _ident(r.text_bytes())
        name = _name(r.text_bytes())
        key = r.take(32)
        code = r.take(CODE_LEN)
        r.done()
        if code.translate(None, _CODE_BYTES):
            raise ProtocolError(ST_BAD_REQUEST)
        if not self._create_lim.allow(ip, now):
            raise ProtocolError(ST_RATE_LIMIT)
        key = self._resolve_key(key)
        code_s = code.decode("ascii")
        with self._lock:
            self._sweep_locked(now)
            room = self._rooms.get(code_s)
            if room is not None:
                host = room.players.get(0)
                if host is None or now - host.last_seen > self._ttl:
                    self._sweep_locked(now, force=True)
                    room = self._rooms.get(code_s)
            if room is None or room.game != game:
                raise ProtocolError(ST_NOT_FOUND)
            if len(room.players) >= room.max_players:
                raise ProtocolError(ST_FULL)
            if len(self._sessions) >= self._cfg.max_sessions:
                raise ProtocolError(ST_CAPACITY)
            sid = next(i for i in range(MAX_PLAYERS) if i not in room.players)
            _, token = self._add_session_locked(room, sid, name, key, now)
            room.revision += 1
            return (token + key + code + bytes((sid,)) + self._snapshot(room))

    def _op_sync(self, r: _Reader, ip: str, now: float) -> bytes:
        token = r.take(32)
        input_seq = r.u32()
        ilen = r.u8()
        if ilen > MAX_INPUT:
            raise ProtocolError(ST_BAD_REQUEST)
        inp = r.take(ilen)
        state_seq = r.u32()
        slen = r.u16()
        if slen > MAX_STATE:
            raise ProtocolError(ST_BAD_REQUEST)
        state = r.take(slen)
        r.done()
        with self._lock:
            self._sweep_locked(now)
            s = self._lookup_locked(token, now)
            if not s.frame_lim.allow(now):
                raise ProtocolError(ST_RATE_LIMIT)
            room = s.room
            is_host = s.sid == 0
            if not is_host and (state_seq != 0 or slen != 0):
                raise ProtocolError(ST_NOT_HOST)
            # Validate everything first; mutate only afterwards.
            apply_input = _seq_update(s.input_seq, s.input, input_seq, inp)
            apply_state = (is_host and
                           _seq_update(room.state_seq, room.state, state_seq, state))
            s.last_seen = now
            if apply_input:
                s.input_seq = input_seq
                s.input = inp
            if apply_state:
                room.state_seq = state_seq
                room.state = state
            if apply_input or apply_state:
                room.revision += 1
            return self._snapshot(room)

    def _op_leave(self, r: _Reader, ip: str, now: float) -> bytes:
        token = r.take(32)
        r.done()
        digest = hashlib.sha256(token).digest()
        with self._lock:
            s = self._sessions.get(digest)
            if s is not None and now - s.last_seen > self._ttl:
                self._sweep_locked(now, force=True)
                s = None
            if s is None:
                raise ProtocolError(ST_NOT_FOUND)
            if s.sid == 0:
                self._close_room_locked(s.room, now, revoked=s)
            else:
                self._remove_guest_locked(s.room, s)
            return b""

    def _op_score(self, r: _Reader, ip: str, now: float) -> bytes:
        token = r.take(32)
        board = _ident(r.text_bytes())
        score = r.i32()
        rlen = r.u8()
        if rlen > MAX_RECORD:
            raise ProtocolError(ST_BAD_REQUEST)
        record = r.take(rlen)
        r.done()
        with self._lock:
            self._sweep_locked(now)
            s = self._lookup_locked(token, now)
            s.last_seen = now
            if not s.score_lim.allow(now) or not self._score_lim.allow(ip, now):
                raise ProtocolError(ST_RATE_LIMIT)
            game, name, ident = s.room.game, s.name, s.key_hash
        status, entries = self._store.submit(game, board, ident, name, score, record)
        if status != ST_OK:
            raise ProtocolError(status)
        return self._board_result(board, entries)

    def _op_board(self, r: _Reader, ip: str, now: float) -> bytes:
        token = r.take(32)
        board = _ident(r.text_bytes())
        r.done()
        with self._lock:
            self._sweep_locked(now)
            s = self._lookup_locked(token, now)
            s.last_seen = now
            if not s.board_lim.allow(now):
                raise ProtocolError(ST_RATE_LIMIT)
            game = s.room.game
        status, entries = self._store.top(game, board)
        if status != ST_OK:
            raise ProtocolError(status)
        return self._board_result(board, entries)


# --------------------------------------------------------------------------
# HTTP layer
# --------------------------------------------------------------------------
class _Conn:
    """Per-connection deadline state, enforced by the maintenance thread."""

    __slots__ = ("sock", "busy", "deadline", "nbytes", "req_timeout", "idle_timeout")

    def __init__(self, sock, req_timeout: float, idle_timeout: float):
        self.sock = sock
        self.busy = False
        self.nbytes = 0
        self.req_timeout = req_timeout
        self.idle_timeout = idle_timeout
        self.deadline = time.monotonic() + req_timeout

    def on_bytes(self, n: int) -> None:
        if not self.busy:
            self.busy = True
            self.nbytes = 0
            self.deadline = time.monotonic() + self.req_timeout
        self.nbytes += n
        if self.nbytes > MAX_REQUEST_BYTES:
            raise ConnectionError("request too large")

    def end_request(self) -> None:
        self.busy = False
        self.nbytes = 0
        self.deadline = time.monotonic() + self.idle_timeout

    def kill(self) -> None:
        self.deadline = float("inf")
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class _TrackedRaw(io.RawIOBase):
    def __init__(self, sock, conn: _Conn):
        super().__init__()
        self._sock = sock
        self._conn = conn

    def readable(self) -> bool:
        return True

    def readinto(self, b) -> int:
        n = self._sock.recv_into(b)
        if n:
            self._conn.on_bytes(n)
        return n


class _HttpServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128
    allow_reuse_address = os.name != "nt"   # SO_REUSEADDR lets ports be stolen on Windows

    def __init__(self, addr, handler, service: RelayService, cfg: ServerConfig):
        self.service = service
        self.cfg = cfg
        self.trusted = cfg.trusted_networks()
        self._slots = threading.BoundedSemaphore(cfg.max_inflight)
        self._conns: set = set()
        self._conns_lock = threading.Lock()
        self.address_family = socket.AF_INET6 if ":" in addr[0] else socket.AF_INET
        super().__init__(addr, handler)

    def server_bind(self):  # avoid HTTPServer's reverse-DNS getfqdn()
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]

    # connection cap: one thread per connection, bounded by max_inflight
    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            try:
                request.settimeout(1.0)
                request.sendall(b"HTTP/1.1 503 Service Unavailable\r\n"
                                b"Retry-After: 1\r\nContent-Length: 0\r\n"
                                b"Connection: close\r\n\r\n")
            except OSError:
                pass
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()

    def handle_error(self, request, client_address):
        exc = sys.exc_info()[1]
        log.debug("connection error: %s", type(exc).__name__)

    def register(self, conn: _Conn) -> None:
        with self._conns_lock:
            self._conns.add(conn)

    def unregister(self, conn: _Conn) -> None:
        with self._conns_lock:
            self._conns.discard(conn)

    def active_connections(self) -> int:
        with self._conns_lock:
            return len(self._conns)

    def reap_stalled(self) -> None:
        now = time.monotonic()
        with self._conns_lock:
            stale = [c for c in self._conns if c.deadline < now]
        for c in stale:
            c.kill()

    def kill_all(self) -> None:
        with self._conns_lock:
            conns = list(self._conns)
        for c in conns:
            c.kill()


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "pixelica-online"
    sys_version = ""
    _drain = False

    def version_string(self):
        return self.server_version

    def log_message(self, format, *args):  # access logging disabled on purpose
        pass

    # -- connection lifecycle ---------------------------------------------
    def setup(self):
        cfg = self.server.cfg
        self.connection = self.request
        self.connection.settimeout(cfg.idle_timeout)
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        self.conn = _Conn(self.connection, cfg.request_timeout, cfg.idle_timeout)
        self.rfile = io.BufferedReader(_TrackedRaw(self.connection, self.conn), 8192)
        self.wfile = self.connection.makefile("wb", 0)
        self.server.register(self.conn)

    def handle_one_request(self):
        try:
            super().handle_one_request()
        finally:
            self.conn.end_request()

    def finish(self):
        try:
            if self._drain:  # let the client read our error instead of an RST
                try:
                    self.wfile.flush()
                    self.connection.shutdown(socket.SHUT_WR)
                    self.connection.settimeout(0.25)
                    for _ in range(4):
                        if not self.connection.recv(4096):
                            break
                except OSError:
                    pass
            super().finish()
        finally:
            self.server.unregister(self.conn)

    # -- responses -------------------------------------------------------
    def send_error(self, code, message=None, explain=None):
        self.close_connection = True
        self._drain = True
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def _send_bytes(self, status: int, body: bytes, ctype: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        # headers and body leave in one write
        self._headers_buffer.append(b"\r\n")
        self._headers_buffer.append(body)
        self.flush_headers()

    def _client_key(self) -> str:
        return resolve_client_key(self.client_address[0],
                                  self.headers.get_all("X-Forwarded-For"),
                                  self.server.trusted)

    # -- routes ----------------------------------------------------------
    def do_GET(self):
        if self.path == "/health":
            self._send_bytes(200, b'{"status":"ok"}', "application/json")
        elif self.path == ROUTE:
            self.send_error(405)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != ROUTE:
            self.send_error(404)
            return
        h = self.headers
        if h.get("Transfer-Encoding") is not None:
            self.send_error(400)
            return
        lengths = h.get_all("Content-Length")
        if not lengths:
            self.send_error(411)
            return
        if len(lengths) != 1:
            self.send_error(400)
            return
        raw = lengths[0].strip()
        if not (raw.isascii() and raw.isdigit()) or len(raw) > 6:
            self.send_error(400)
            return
        n = int(raw)
        if n > MAX_PACKET:
            self.send_error(413)
            return
        if n < HEADER_LEN:
            self.send_error(400)
            return
        ctype = (h.get("Content-Type") or "").split(";", 1)[0].strip().lower()
        if ctype != "application/octet-stream":
            self.send_error(415)
            return
        body = self.rfile.read(n)
        if len(body) != n:
            self.close_connection = True
            return
        try:
            resp = self.server.service.handle(body, self._client_key())
        except TransportError:
            self.send_error(400)
            return
        self._send_bytes(200, resp, "application/octet-stream")


# --------------------------------------------------------------------------
# Server wrapper
# --------------------------------------------------------------------------
class RelayServer:
    """Relay + HTTP server.  Usable as a context manager (ephemeral port: port=0).

        with RelayServer(ServerConfig(port=0, db_path=":memory:")) as srv:
            url = srv.url
    """

    def __init__(self, config: ServerConfig | None = None, clock=time.monotonic):
        self.config = (config or ServerConfig()).validate()
        self.store = ScoreStore(self.config.db_path, self.config.max_records)
        try:
            self.service = RelayService(self.config, self.store, clock)
            self.httpd = _HttpServer((self.config.host, self.config.port), _Handler,
                                     self.service, self.config)
        except BaseException:
            self.store.close()
            raise
        self._stop_evt = threading.Event()
        self._serve_thread: threading.Thread | None = None
        self._maint_thread: threading.Thread | None = None
        self._stopped = False
        self._tick = min(0.25, self.config.session_ttl / 4.0)

    @property
    def address(self) -> tuple:
        return self.httpd.server_address[:2]

    @property
    def port(self) -> int:
        return self.httpd.server_address[1]

    @property
    def url(self) -> str:
        host = self.address[0]
        if ":" in host:
            host = "[%s]" % host
        return "http://%s:%d" % (host, self.port)

    def start(self) -> "RelayServer":
        if self._serve_thread is not None:
            return self
        self._serve_thread = threading.Thread(
            target=self.httpd.serve_forever, kwargs={"poll_interval": 0.2},
            name="pixelica-online-http", daemon=True)
        self._maint_thread = threading.Thread(
            target=self._maintenance, name="pixelica-online-janitor", daemon=True)
        self._serve_thread.start()
        self._maint_thread.start()
        return self

    def _maintenance(self) -> None:
        while not self._stop_evt.wait(self._tick):
            try:
                self.service.sweep()
                self.httpd.reap_stalled()
            except Exception as exc:
                log.error("janitor error: %s", type(exc).__name__)

    def stop(self) -> None:
        if self._stopped:
            return
        self._stopped = True
        self._stop_evt.set()
        if self._serve_thread is not None:
            self.httpd.shutdown()
        self.httpd.server_close()
        self.httpd.kill_all()
        deadline = time.monotonic() + 2.0
        while self.httpd.active_connections() and time.monotonic() < deadline:
            time.sleep(0.02)
        for t in (self._serve_thread, self._maint_thread):
            if t is not None:
                t.join(timeout=2.0)
        self.store.close()

    def __enter__(self) -> "RelayServer":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def _num(kind, lo, hi):
    def conv(text):
        try:
            v = kind(text)
        except (TypeError, ValueError):
            raise argparse.ArgumentTypeError("invalid number: %r" % (text,)) from None
        if not lo <= v <= hi:
            raise argparse.ArgumentTypeError("must be in %s..%s" % (lo, hi))
        return v
    return conv


def build_config(argv=None, env=None) -> ServerConfig:
    """Parse CLI (+PIXELICA_* environment defaults) into a validated ServerConfig."""
    env = os.environ if env is None else env
    p = argparse.ArgumentParser(
        prog="server.py",
        description="Pixelica online relay (casual co-op/versus relay + score boards).")
    p.add_argument("--host", default=env.get("PIXELICA_HOST", "127.0.0.1"),
                   help="bind address (default 127.0.0.1; env PIXELICA_HOST)")
    p.add_argument("--port", type=_num(int, 0, 65535), default=env.get("PIXELICA_PORT", "8787"),
                   help="TCP port (default 8787; 0 = ephemeral; env PIXELICA_PORT)")
    p.add_argument("--db", default=env.get("PIXELICA_DB", "pixelica_online.sqlite3"),
                   help="SQLite file (default ./pixelica_online.sqlite3; env PIXELICA_DB)")
    p.add_argument("--session-ttl", type=_num(float, 5, 3600),
                   default=env.get("PIXELICA_SESSION_TTL", "30"),
                   help="seconds without a request before a session expires (default 30)")
    p.add_argument("--max-rooms", type=_num(int, 1, 100000),
                   default=env.get("PIXELICA_MAX_ROOMS", "256"), help="max live rooms (default 256)")
    p.add_argument("--max-sessions", type=_num(int, 1, 1000000),
                   default=env.get("PIXELICA_MAX_SESSIONS", "2048"),
                   help="max live sessions (default 2048)")
    p.add_argument("--max-inflight", type=_num(int, 1, 10000),
                   default=env.get("PIXELICA_MAX_INFLIGHT", "64"),
                   help="max concurrent HTTP connections (default 64)")
    p.add_argument("--max-records", type=_num(int, 1, 100000000),
                   default=env.get("PIXELICA_MAX_RECORDS", "100000"),
                   help="max score rows in the database (default 100000)")
    p.add_argument("--ip-rate", type=_num(float, 1, 1000000),
                   default=env.get("PIXELICA_IP_RATE", "600"),
                   help="sustained requests/s per client address (default 600)")
    p.add_argument("--trusted-proxy", action="append", default=None, metavar="CIDR",
                   help="trust X-Forwarded-For from this proxy address/CIDR "
                        "(repeatable, or comma list in env PIXELICA_TRUSTED_PROXIES)")
    args = p.parse_args(argv)
    trusted = args.trusted_proxy
    if trusted is None:
        trusted = [x for x in env.get("PIXELICA_TRUSTED_PROXIES", "").split(",") if x.strip()]
    else:
        trusted = [x for item in trusted for x in item.split(",") if x.strip()]
    try:
        return ServerConfig(
            host=args.host, port=args.port, db_path=args.db,
            session_ttl=args.session_ttl, max_rooms=args.max_rooms,
            max_sessions=args.max_sessions, max_inflight=args.max_inflight,
            max_records=args.max_records, ip_rate=args.ip_rate,
            ip_burst=max(1.0, args.ip_rate * 2.0),
            trusted_proxies=tuple(trusted)).validate()
    except ValueError as exc:
        p.error(str(exc))


def _is_loopback(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def main(argv=None) -> int:
    cfg = build_config(argv)
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        srv = RelayServer(cfg)
    except (OSError, sqlite3.Error, RuntimeError) as exc:
        log.error("cannot start: %s", type(exc).__name__)
        return 2
    if not _is_loopback(cfg.host):
        log.warning("listening on a non-loopback address with PLAIN HTTP; "
                    "put an HTTPS reverse proxy (Caddy) in front for public use")
    stop = threading.Event()

    def on_signal(signum, frame):
        stop.set()

    for name in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, name, None)
        if sig is not None:
            signal.signal(sig, on_signal)
    srv.start()
    log.info("pixelica online relay listening on %s (db=%s ttl=%gs rooms<=%d sessions<=%d "
             "inflight<=%d)", srv.url, cfg.db_path, cfg.session_ttl, cfg.max_rooms,
             cfg.max_sessions, cfg.max_inflight)
    try:
        while not stop.wait(0.5):
            pass
    finally:
        log.info("shutting down")
        srv.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
