#!/usr/bin/env python3
"""Unit tests for server.py.  Standard library only:

    python -m unittest discover -s tools/online_server -v
    python tools/online_server/test_server.py
"""
import contextlib
import http.client
import io
import logging
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server  # noqa: E402

ZERO32 = bytes(32)


# --------------------------------------------------------------------------
# packet helpers (independent of server.py's encoders on purpose)
# --------------------------------------------------------------------------
def hdr(op, status=0):
    return b"P3ON" + bytes((1, op)) + struct.pack(">H", status)


def text(s):
    b = s.encode("utf-8") if isinstance(s, str) else s
    return bytes((len(b),)) + b


def host_pkt(game="game", name="Alice", key=ZERO32, mode=1, maxp=4):
    return hdr(1) + text(game) + text(name) + key + bytes((mode, maxp))


def join_pkt(code, game="game", name="Bob", key=ZERO32):
    if isinstance(code, str):
        code = code.encode("ascii")
    return hdr(2) + text(game) + text(name) + key + code


def sync_pkt(token, iseq=0, inp=b"", sseq=0, state=b""):
    return (hdr(3) + token + struct.pack(">IB", iseq, len(inp)) + inp
            + struct.pack(">IH", sseq, len(state)) + state)


def leave_pkt(token):
    return hdr(4) + token


def score_pkt(token, board, score, record=b""):
    return hdr(5) + token + text(board) + struct.pack(">iB", score, len(record)) + record


def board_pkt(token, board):
    return hdr(6) + token + text(board)


def split(resp):
    assert resp[:4] == b"P3ON" and resp[4] == 1, resp[:8]
    return resp[5], struct.unpack(">H", resp[6:8])[0], resp[8:]


def parse_snapshot(buf, pos=0):
    rev, host, count, mode, mx, sseq, slen = struct.unpack_from(">IBBBBIH", buf, pos)
    pos += 14
    state = buf[pos:pos + slen]
    pos += slen
    players = {}
    for _ in range(count):
        pid = buf[pos]
        nl = buf[pos + 1]
        name = buf[pos + 2:pos + 2 + nl].decode("utf-8")
        pos += 2 + nl
        iseq, ilen = struct.unpack_from(">IB", buf, pos)
        pos += 5
        players[pid] = {"name": name, "iseq": iseq, "input": buf[pos:pos + ilen]}
        pos += ilen
    snap = {"revision": rev, "host": host, "count": count, "mode": mode,
            "max": mx, "state_seq": sseq, "state": state, "players": players}
    return snap, pos


def parse_join(body):
    token, key, code = body[:32], body[32:64], body[64:70].decode("ascii")
    sid = body[70]
    snap, end = parse_snapshot(body, 71)
    assert end == len(body)
    return {"token": token, "key": key, "code": code, "sid": sid, "snap": snap}


def parse_board(body):
    nl = body[0]
    board = body[1:1 + nl].decode()
    pos = 1 + nl
    count = body[pos]
    pos += 1
    entries = []
    for _ in range(count):
        l = body[pos]
        name = body[pos + 1:pos + 1 + l].decode("utf-8")
        pos += 1 + l
        score, rl = struct.unpack_from(">iB", body, pos)
        pos += 5
        entries.append((name, score, body[pos:pos + rl]))
        pos += rl
    assert pos == len(body)
    return board, entries


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def open_config(**kw):
    """Config with the abuse limiters opened so a test only hits what it targets."""
    d = dict(db_path=":memory:", create_burst=10000, create_rate=10000,
             ip_burst=1e9, ip_rate=1e9, ip_score_burst=10000, ip_score_rate=10000)
    d.update(kw)
    return server.ServerConfig(**d)


# --------------------------------------------------------------------------
# protocol engine
# --------------------------------------------------------------------------
class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.make()

    def make(self, **kw):
        self.cfg = open_config(**kw)
        self.store = server.ScoreStore(":memory:", self.cfg.max_records)
        self.addCleanup(self.store.close)
        self.svc = server.RelayService(self.cfg, self.store, clock=self.clock)

    def call(self, pkt, ip="10.0.0.1"):
        return split(self.svc.handle(pkt, ip))

    def ok(self, pkt, ip="10.0.0.1"):
        op, status, body = self.call(pkt, ip)
        self.assertEqual(status, 0, "unexpected status %d" % status)
        return op, body

    def status(self, pkt, ip="10.0.0.1"):
        op, status, body = self.call(pkt, ip)
        if status != 0:
            self.assertEqual(body, b"", "error responses carry no body")
        return status

    def host(self, **kw):
        op, body = self.ok(host_pkt(**kw))
        self.assertEqual(op, 0x81)
        return parse_join(body)

    def join(self, code, **kw):
        op, body = self.ok(join_pkt(code, **kw))
        self.assertEqual(op, 0x82)
        return parse_join(body)

    def sync(self, who, **kw):
        op, body = self.ok(sync_pkt(who["token"], **kw))
        self.assertEqual(op, 0x83)
        snap, end = parse_snapshot(body)
        self.assertEqual(end, len(body))
        return snap

    # ---- header level -----------------------------------------------------
    def test_transport_errors(self):
        for bad in (b"", b"P3ON\x01\x01", b"XXXX\x01\x01\x00\x00",
                    b"P3ON\x02\x01\x00\x00", b"P3ON\x00\x01\x00\x00"):
            with self.assertRaises(server.TransportError):
                self.svc.handle(bad, "ip")
        with self.assertRaises(server.TransportError):
            self.svc.handle(b"P3ON\x01\x03\x00\x00" + bytes(5000), "ip")

    def test_unknown_opcode_and_nonzero_status(self):
        op, status, body = self.call(hdr(0x55))
        self.assertEqual((op, status, body), (0xD5, server.ST_BAD_REQUEST, b""))
        op, status, body = self.call(hdr(1, 7) + b"x")
        self.assertEqual((op, status), (0x81, server.ST_BAD_REQUEST))

    # ---- host / join ------------------------------------------------------
    def test_host_success(self):
        h = self.host(mode=2, maxp=6)
        self.assertEqual(len(h["token"]), 32)
        self.assertNotEqual(h["key"], ZERO32)
        self.assertEqual(len(h["code"]), 6)
        self.assertTrue(all(c in server.CODE_ALPHABET for c in h["code"]))
        self.assertEqual(h["sid"], 0)
        s = h["snap"]
        self.assertEqual((s["host"], s["count"], s["mode"], s["max"]), (0, 1, 2, 6))
        self.assertEqual(s["players"][0]["name"], "Alice")
        self.assertEqual((s["state_seq"], s["state"]), (0, b""))

    def test_host_key_is_kept_or_generated(self):
        key = bytes(range(1, 33))
        self.assertEqual(self.host(key=key)["key"], key)
        self.assertNotEqual(self.host()["key"], self.host()["key"])

    def test_host_validation(self):
        bad = [
            host_pkt(game="bad game"), host_pkt(game=""), host_pkt(game="g" * 33),
            host_pkt(name=""), host_pkt(name="n" * 33),
            host_pkt(name=b"\xff\xfe"), host_pkt(name="A\x07"), host_pkt(name="A\x7f"),
            host_pkt(name="A\u0085"),
            host_pkt(mode=0), host_pkt(mode=3), host_pkt(maxp=1), host_pkt(maxp=9),
            host_pkt() + b"\x00",            # trailing data
            host_pkt()[:-1],                 # truncated
            hdr(1),
        ]
        for pkt in bad:
            self.assertEqual(self.status(pkt), server.ST_BAD_REQUEST, pkt)

    def test_names_accept_utf8_up_to_32_bytes(self):
        self.host(name="한글이름")
        self.host(name="n" * 32)
        h = self.host(name="한" * 10)       # 30 bytes
        self.assertEqual(h["snap"]["players"][0]["name"], "한" * 10)

    def test_join_flow(self):
        h = self.host(maxp=3)
        g1 = self.join(h["code"], name="Bob")
        self.assertEqual(g1["sid"], 1)
        self.assertEqual(g1["code"], h["code"])
        self.assertEqual(g1["snap"]["count"], 2)
        g2 = self.join(h["code"], name="Cy")
        self.assertEqual(g2["sid"], 2)
        self.assertEqual(self.status(join_pkt(h["code"], name="Dee")), server.ST_FULL)
        self.assertEqual(len({h["token"], g1["token"], g2["token"]}), 3)

    def test_join_errors(self):
        h = self.host()
        self.assertEqual(self.status(join_pkt("AAAAAA")), server.ST_NOT_FOUND)
        self.assertEqual(self.status(join_pkt(h["code"], game="other")), server.ST_NOT_FOUND)
        self.assertEqual(self.status(join_pkt("abcdef")), server.ST_BAD_REQUEST)   # clients uppercase
        self.assertEqual(self.status(join_pkt("AB0DEF")), server.ST_BAD_REQUEST)  # '0' not in alphabet
        self.assertEqual(self.status(join_pkt(b"ABC")), server.ST_BAD_REQUEST)

    def test_freed_guest_id_is_reused(self):
        h = self.host(maxp=4)
        g1 = self.join(h["code"])
        self.join(h["code"])
        self.ok(leave_pkt(g1["token"]))
        self.assertEqual(self.join(h["code"])["sid"], 1)

    # ---- sync -------------------------------------------------------------
    def test_input_and_state_exchange(self):
        h = self.host()
        g = self.join(h["code"])
        snap = self.sync(g, iseq=1, inp=b"\x01\x02")
        self.assertEqual(snap["players"][1]["input"], b"\x01\x02")
        self.assertEqual(snap["players"][1]["iseq"], 1)
        snap = self.sync(h, iseq=1, inp=b"H", sseq=1, state=b"world-1")
        self.assertEqual(snap["state"], b"world-1")
        self.assertEqual(snap["players"][1]["input"], b"\x01\x02")
        gsnap = self.sync(g)
        self.assertEqual((gsnap["state_seq"], gsnap["state"]), (1, b"world-1"))
        self.assertEqual(gsnap["players"][0]["input"], b"H")

    def test_input_can_be_cleared(self):
        h = self.host()
        self.sync(h, iseq=1, inp=b"abc")
        snap = self.sync(h, iseq=2, inp=b"")
        self.assertEqual((snap["players"][0]["iseq"], snap["players"][0]["input"]), (2, b""))

    def test_nonhost_state_rejected(self):
        h = self.host()
        g = self.join(h["code"])
        self.assertEqual(self.status(sync_pkt(g["token"], sseq=1, state=b"x")), server.ST_NOT_HOST)
        self.assertEqual(self.status(sync_pkt(g["token"], sseq=5)), server.ST_NOT_HOST)
        self.assertEqual(self.sync(h)["state_seq"], 0)

    def test_size_and_seq_validation(self):
        h = self.host()
        self.assertEqual(self.status(sync_pkt(h["token"], iseq=1, inp=bytes(65))), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(sync_pkt(h["token"], sseq=1, state=bytes(2049))), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(sync_pkt(h["token"], sseq=0, state=b"x")), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(sync_pkt(h["token"], iseq=0, inp=b"x")), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(sync_pkt(h["token"]) + b"\x00"), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(sync_pkt(h["token"])[:-1]), server.ST_BAD_REQUEST)
        self.sync(h, iseq=1, inp=bytes(64), sseq=1, state=bytes(2048))   # exact maxima are fine

    def test_duplicate_seq_idempotent_and_conflict_is_atomic(self):
        h = self.host()
        self.sync(h, iseq=1, inp=b"X", sseq=1, state=b"A")
        rev = self.sync(h, iseq=1, inp=b"X", sseq=1, state=b"A")["revision"]   # idempotent
        self.assertEqual(self.sync(h)["revision"], rev)
        # new input + conflicting state in one request: nothing may change
        self.assertEqual(self.status(sync_pkt(h["token"], iseq=2, inp=b"Y", sseq=1, state=b"B")),
                         server.ST_BAD_REQUEST)
        snap = self.sync(h)
        self.assertEqual((snap["players"][0]["iseq"], snap["players"][0]["input"]), (1, b"X"))
        self.assertEqual((snap["state_seq"], snap["state"]), (1, b"A"))
        self.assertEqual(snap["revision"], rev)
        self.assertEqual(self.status(sync_pkt(h["token"], iseq=1, inp=b"Z")), server.ST_BAD_REQUEST)

    def test_sequence_wraparound_and_stale(self):
        h = self.host()
        self.sync(h, sseq=0xFFFFFFFF, state=b"A")
        snap = self.sync(h, sseq=1, state=b"B")          # wrapped, newer
        self.assertEqual((snap["state_seq"], snap["state"]), (1, b"B"))
        snap = self.sync(h, sseq=0xFFFFFFFE, state=b"old")   # stale: ignored, not an error
        self.assertEqual((snap["state_seq"], snap["state"]), (1, b"B"))
        snap = self.sync(h, sseq=0x7FFFFFF0, state=b"far")   # < 2^31 ahead: newer
        self.assertEqual(snap["state"], b"far")

    def test_host_zero_zero_keeps_published_state(self):
        h = self.host()
        self.sync(h, sseq=3, state=b"S")
        self.assertEqual(self.sync(h)["state"], b"S")

    def test_auth_errors(self):
        self.assertEqual(self.status(sync_pkt(bytes(32))), server.ST_UNAUTHORIZED)
        self.assertEqual(self.status(sync_pkt(b"\x01" * 32)), server.ST_UNAUTHORIZED)
        self.assertEqual(self.status(score_pkt(b"\x01" * 32, "b", 1)), server.ST_UNAUTHORIZED)
        self.assertEqual(self.status(board_pkt(b"\x01" * 32, "b")), server.ST_UNAUTHORIZED)
        h = self.host()
        bad = bytearray(h["token"])
        bad[31] ^= 1
        self.assertEqual(self.status(sync_pkt(bytes(bad))), server.ST_UNAUTHORIZED)
        # another room's token is simply a different session: guests cannot act as host
        g = self.join(h["code"])
        self.assertEqual(self.status(sync_pkt(g["token"], sseq=1, state=b"x")), server.ST_NOT_HOST)

    # ---- leave / expiry ---------------------------------------------------
    def test_guest_leave(self):
        h = self.host()
        g = self.join(h["code"])
        op, body = self.call(leave_pkt(g["token"]))[0::2]
        self.assertEqual((op, body), (0x84, b""))
        self.assertEqual(self.sync(h)["count"], 1)
        self.assertEqual(self.status(sync_pkt(g["token"])), server.ST_UNAUTHORIZED)
        self.assertEqual(self.status(leave_pkt(g["token"])), server.ST_NOT_FOUND)

    def test_host_leave_closes_room(self):
        h = self.host()
        g = self.join(h["code"])
        self.ok(leave_pkt(h["token"]))
        self.assertEqual(self.status(sync_pkt(g["token"])), server.ST_ROOM_CLOSED)
        self.assertEqual(self.status(sync_pkt(h["token"])), server.ST_UNAUTHORIZED)
        self.assertEqual(self.status(join_pkt(h["code"])), server.ST_NOT_FOUND)
        self.assertEqual(self.status(leave_pkt(h["token"])), server.ST_NOT_FOUND)
        self.assertEqual(self.svc.stats()["rooms"], 0)
        self.assertEqual(self.svc.stats()["sessions"], 0)

    def test_leave_unknown_not_found(self):
        self.assertEqual(self.status(leave_pkt(b"\x07" * 32)), server.ST_NOT_FOUND)

    def test_host_expiry_invalidates_guests(self):
        h = self.host()
        g = self.join(h["code"])
        self.clock.t += 31
        self.assertEqual(self.status(sync_pkt(g["token"])), server.ST_ROOM_CLOSED)
        self.assertEqual(self.status(join_pkt(h["code"])), server.ST_NOT_FOUND)
        self.assertEqual(self.svc.stats()["rooms"], 0)

    def test_guest_expiry_keeps_room(self):
        h = self.host()
        g = self.join(h["code"])
        self.clock.t += 20
        self.sync(h)
        self.clock.t += 11                       # guest idle 31 s, host idle 11 s
        self.assertEqual(self.status(sync_pkt(g["token"])), server.ST_UNAUTHORIZED)
        self.assertEqual(self.sync(h)["count"], 1)

    def test_sync_refreshes_lease(self):
        h = self.host()
        for _ in range(10):
            self.clock.t += 20
            self.sync(h)
        self.assertEqual(self.svc.stats()["rooms"], 1)

    def test_rejected_sync_does_not_refresh_lease(self):
        h = self.host()
        self.sync(h, iseq=1, inp=b"old", sseq=1, state=b"old")
        self.clock.t += 20
        self.assertEqual(self.status(sync_pkt(h["token"], iseq=2, inp=b"new",
                                              sseq=1, state=b"conflict")), server.ST_BAD_REQUEST)
        self.clock.t += 11
        self.assertEqual(self.status(sync_pkt(h["token"])), server.ST_ROOM_CLOSED)

    def test_board_and_score_refresh_lease(self):
        h = self.host()
        for _ in range(4):
            self.clock.t += 20
            self.ok(board_pkt(h["token"], "b"))
        self.clock.t += 20
        self.ok(score_pkt(h["token"], "b", 1))
        self.clock.t += 20
        self.sync(h)

    def test_janitor_sweep_and_tombstone_bound(self):
        h = self.host()
        self.join(h["code"])
        self.clock.t += 31
        self.svc.sweep()
        self.assertEqual(self.svc.stats(), {"rooms": 0, "sessions": 0, "tombstones": 2})
        self.clock.t += server.TOMBSTONE_TTL + 1
        self.svc.sweep()
        self.assertEqual(self.svc.stats()["tombstones"], 0)

    # ---- capacity / rate ---------------------------------------------------
    def test_room_capacity(self):
        self.make(max_rooms=2)
        a = self.host()
        self.host()
        self.assertEqual(self.status(host_pkt()), server.ST_CAPACITY)
        self.ok(leave_pkt(a["token"]))
        self.host()

    def test_session_capacity(self):
        self.make(max_sessions=3)
        h = self.host(maxp=8)
        self.join(h["code"])
        self.join(h["code"])
        self.assertEqual(self.status(join_pkt(h["code"])), server.ST_CAPACITY)
        self.assertEqual(self.status(host_pkt()), server.ST_CAPACITY)

    def test_frame_rate_limit(self):
        h = self.host()
        results = [self.status(sync_pkt(h["token"])) for _ in range(50)]
        self.assertEqual(results.count(0), int(server.FRAME_BURST))
        self.assertEqual(results.count(server.ST_RATE_LIMIT), 50 - int(server.FRAME_BURST))
        self.clock.t += 1.0
        self.assertEqual(self.status(sync_pkt(h["token"])), 0)

    def test_score_rate_limit(self):
        h = self.host()
        self.assertEqual(self.status(score_pkt(h["token"], "a", 1)), 0)
        self.assertEqual(self.status(score_pkt(h["token"], "b", 1)), 0)
        self.assertEqual(self.status(score_pkt(h["token"], "c", 1)), server.ST_RATE_LIMIT)
        self.clock.t += 0.5
        self.assertEqual(self.status(score_pkt(h["token"], "c", 1)), 0)

    def test_board_rate_limit(self):
        h = self.host()
        results = [self.status(board_pkt(h["token"], "a")) for _ in range(20)]
        self.assertEqual(results.count(0), int(server.BOARD_BURST))
        self.assertIn(server.ST_RATE_LIMIT, results)

    def test_ip_rate_limit_is_per_address(self):
        self.make(ip_rate=1, ip_burst=3)
        pkt = sync_pkt(b"\x01" * 32)
        res = [self.status(pkt, ip="1.1.1.1") for _ in range(6)]
        self.assertEqual(res.count(server.ST_RATE_LIMIT), 3)
        self.assertEqual(self.status(pkt, ip="2.2.2.2"), server.ST_UNAUTHORIZED)
        self.clock.t += 5
        self.assertEqual(self.status(pkt, ip="1.1.1.1"), server.ST_UNAUTHORIZED)

    def test_create_rate_limit(self):
        self.make(create_rate=1, create_burst=2)
        self.host()
        self.host()
        self.assertEqual(self.status(host_pkt()), server.ST_RATE_LIMIT)
        self.assertEqual(self.status(join_pkt("AAAAAA")), server.ST_RATE_LIMIT)
        self.assertEqual(self.status(host_pkt(), ip="9.9.9.9"), 0)

    def test_ip_score_rate_limit(self):
        self.make(ip_score_rate=1, ip_score_burst=2)
        toks = [self.host()["token"] for _ in range(3)]
        res = [self.status(score_pkt(t, "b", 1)) for t in toks]
        self.assertEqual(res, [0, 0, server.ST_RATE_LIMIT])

    def test_limiter_table_is_bounded(self):
        t = server.TokenBucketTable(rate=1.0, burst=2.0, max_entries=100)
        for i in range(5000):
            t.allow("k%d" % i, 1.0)
        self.assertLessEqual(len(t), 100)
        self.assertTrue(t.allow("x", 1.0))
        self.assertTrue(t.allow("x", 1.0))
        self.assertFalse(t.allow("x", 1.0))
        self.assertTrue(t.allow("x", 2.5))
        t.purge(1000.0)
        self.assertEqual(len(t), 0)

    # ---- boards -------------------------------------------------------------
    def test_score_and_board_flow(self):
        h = self.host(name="Alice")
        op, body = self.ok(score_pkt(h["token"], "lvl.1", 120, b"rec1"))
        self.assertEqual(op, 0x85)
        board, entries = parse_board(body)
        self.assertEqual((board, entries), ("lvl.1", [("Alice", 120, b"rec1")]))
        op, body = self.ok(board_pkt(h["token"], "lvl.1"))
        self.assertEqual(op, 0x86)
        self.assertEqual(parse_board(body)[1], [("Alice", 120, b"rec1")])
        self.assertEqual(parse_board(self.ok(board_pkt(h["token"], "empty"))[1]), ("empty", []))

    def test_negative_and_extreme_scores(self):
        h = self.host()
        g = self.join(h["code"])
        self.ok(score_pkt(h["token"], "b", -2147483648))
        self.ok(score_pkt(g["token"], "b", 2147483647))
        entries = parse_board(self.ok(board_pkt(h["token"], "b"))[1])[1]
        self.assertEqual([e[1] for e in entries], [2147483647, -2147483648])

    def test_only_improvement_replaces_name_and_record(self):
        key = b"\x42" * 32
        a = self.host(name="Alice", key=key)
        self.ok(score_pkt(a["token"], "b", 50, b"first"))
        b = self.host(name="Alicia", key=key)               # same identity, new name
        self.clock.t += 1
        entries = parse_board(self.ok(score_pkt(b["token"], "b", 50, b"tie"))[1])[1]
        self.assertEqual(entries, [("Alice", 50, b"first")])   # equal: no update
        self.clock.t += 1
        entries = parse_board(self.ok(score_pkt(b["token"], "b", 10, b"worse"))[1])[1]
        self.assertEqual(entries, [("Alice", 50, b"first")])
        self.clock.t += 1
        entries = parse_board(self.ok(score_pkt(b["token"], "b", 51, b"best"))[1])[1]
        self.assertEqual(entries, [("Alicia", 51, b"best")])      # one row per identity

    def test_ties_first_achieved_then_identity(self):
        h = self.host(name="A")
        g = self.join(h["code"], name="B")
        self.ok(score_pkt(h["token"], "b", 10))
        self.clock.t += 1
        self.ok(score_pkt(g["token"], "b", 10))
        self.clock.t += 1
        entries = parse_board(self.ok(board_pkt(h["token"], "b"))[1])[1]
        self.assertEqual([e[0] for e in entries], ["A", "B"])
        self.ok(score_pkt(g["token"], "b", 12))
        self.clock.t += 1
        self.ok(score_pkt(h["token"], "b", 12))
        self.clock.t += 1
        entries = parse_board(self.ok(board_pkt(h["token"], "b"))[1])[1]
        self.assertEqual([e[0] for e in entries], ["B", "A"])     # B reached 12 first

    def test_boards_partition_by_game_and_board(self):
        key = b"\x09" * 32
        a = self.host(game="g1", key=key)
        b = self.host(game="g2", key=key)
        self.ok(score_pkt(a["token"], "x", 5))
        self.ok(score_pkt(a["token"], "y", 6))
        self.assertEqual(parse_board(self.ok(board_pkt(b["token"], "x"))[1])[1], [])
        self.assertEqual(len(parse_board(self.ok(board_pkt(a["token"], "x"))[1])[1]), 1)
        self.assertEqual(parse_board(self.ok(board_pkt(a["token"], "y"))[1])[1][0][1], 6)

    def test_top10_limit_and_order(self):
        for i in range(12):
            t = self.host(name="P%d" % i, key=bytes((i + 1,)) * 32)["token"]
            self.ok(score_pkt(t, "b", i + 1))
            self.clock.t += 1
        entries = parse_board(self.ok(board_pkt(t, "b"))[1])[1]
        self.assertEqual([e[1] for e in entries], list(range(12, 2, -1)))

    def test_score_validation(self):
        h = self.host()
        self.assertEqual(self.status(score_pkt(h["token"], "bad board", 1)), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(score_pkt(h["token"], "", 1)), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(score_pkt(h["token"], "b" * 33, 1)), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(score_pkt(h["token"], "b", 1, bytes(129))), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(score_pkt(h["token"], "b", 1) + b"x"), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(board_pkt(h["token"], "b/c")), server.ST_BAD_REQUEST)
        self.assertEqual(self.status(score_pkt(h["token"], "b", 1, bytes(128))), 0)

    def test_record_capacity(self):
        self.make(max_records=2)
        toks = [self.host(key=bytes((i + 1,)) * 32)["token"] for i in range(3)]
        self.assertEqual(self.status(score_pkt(toks[0], "b", 1)), 0)
        self.clock.t += 1
        self.assertEqual(self.status(score_pkt(toks[1], "b", 1)), 0)
        self.clock.t += 1
        self.assertEqual(self.status(score_pkt(toks[2], "b", 1)), server.ST_CAPACITY)
        self.clock.t += 1
        self.assertEqual(self.status(score_pkt(toks[0], "b", 9)), 0)     # existing row may improve

    def test_scores_persist_across_restart(self):
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as d:
            path = os.path.join(d, "sub", "scores.sqlite3")
            store = server.ScoreStore(path, 100)
            ident = server._identity_hash(b"k" * 32)
            self.assertEqual(store.submit("g", "b", ident, "Zed", 77, b"r")[0], 0)
            store.close()
            store = server.ScoreStore(path, 100)
            try:
                self.assertEqual(store.top("g", "b"), (0, [("Zed", 77, b"r")]))
                self.assertEqual(store.submit("g", "b", ident, "Zed", 70, b"")[1],
                                 [("Zed", 77, b"r")])
            finally:
                store.close()

    def test_closed_store_gives_internal(self):
        h = self.host()
        self.store.close()
        self.assertEqual(self.status(score_pkt(h["token"], "b", 1)), server.ST_INTERNAL)

    # ---- misc ---------------------------------------------------------------
    def test_concurrent_requests_are_consistent(self):
        h = self.host(maxp=8)
        guests = [self.join(h["code"], name="G%d" % i) for i in range(7)]
        errors = []

        def worker(who, n):
            try:
                for i in range(1, 6):
                    op, status, body = split(self.svc.handle(
                        sync_pkt(who["token"], iseq=i, inp=bytes((n, i))), "10.0.0.%d" % n))
                    if status not in (0, server.ST_RATE_LIMIT):
                        errors.append(status)
            except Exception as exc:  # pragma: no cover
                errors.append(exc)

        threads = [threading.Thread(target=worker, args=(who, i))
                   for i, who in enumerate([h] + guests)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        snap = self.sync(h)
        self.assertEqual(snap["count"], 8)
        self.assertEqual(sorted(snap["players"]), list(range(8)))
        for pid, p in snap["players"].items():
            self.assertEqual(p["input"], bytes((pid, 5)))

    def test_resolve_client_key(self):
        trusted = [server.ipaddress.ip_network("172.28.0.2/32")]
        r = server.resolve_client_key
        self.assertEqual(r("8.8.8.8", ["1.2.3.4"], trusted), "8.8.8.8")        # untrusted peer
        self.assertEqual(r("172.28.0.2", ["1.2.3.4"], trusted), "1.2.3.4")
        self.assertEqual(r("172.28.0.2", ["9.9.9.9, 1.2.3.4"], trusted), "1.2.3.4")
        self.assertEqual(r("172.28.0.2", ["1.2.3.4, 172.28.0.2"], trusted), "1.2.3.4")
        self.assertEqual(r("172.28.0.2", ["garbage"], trusted), "172.28.0.2")
        self.assertEqual(r("172.28.0.2", None, trusted), "172.28.0.2")
        self.assertEqual(r("8.8.8.8", ["1.2.3.4"], []), "8.8.8.8")
        self.assertEqual(r("::ffff:1.2.3.4", None, []), "1.2.3.4")
        self.assertEqual(r("2001:db8::1", None, []), r("2001:db8::ffff", None, []))  # /64 bucket

    def test_cli_config(self):
        cfg = server.build_config([], env={})
        self.assertEqual((cfg.host, cfg.port, cfg.session_ttl, cfg.max_rooms),
                         ("127.0.0.1", 8787, 30.0, 256))
        cfg = server.build_config(["--host", "0.0.0.0", "--port", "9000", "--db", "x.db",
                                   "--session-ttl", "45", "--max-rooms", "5",
                                   "--trusted-proxy", "10.0.0.0/8,192.168.0.1"], env={})
        self.assertEqual((cfg.host, cfg.port, cfg.db_path, cfg.session_ttl, cfg.max_rooms),
                         ("0.0.0.0", 9000, "x.db", 45.0, 5))
        self.assertEqual(cfg.trusted_proxies, ("10.0.0.0/8", "192.168.0.1"))
        cfg = server.build_config([], env={"PIXELICA_PORT": "1234", "PIXELICA_TRUSTED_PROXIES": "172.28.0.2"})
        self.assertEqual((cfg.port, cfg.trusted_proxies), (1234, ("172.28.0.2",)))
        for bad in (["--port", "70000"], ["--session-ttl", "1"], ["--max-rooms", "0"],
                    ["--trusted-proxy", "nonsense"], ["--port", "x"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                server.build_config(bad, env={})


# --------------------------------------------------------------------------
# real HTTP server on an ephemeral port
# --------------------------------------------------------------------------
class _Capture(logging.Handler):
    def __init__(self):
        super().__init__(logging.DEBUG)
        self.lines = []

    def emit(self, record):
        self.lines.append(record.getMessage())


def raw_exchange(port, data, timeout=5.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall(data)
        chunks = []
        while True:
            try:
                b = s.recv(4096)
            except socket.timeout:
                break
            except ConnectionError:
                break
            if not b:
                break
            chunks.append(b)
        return b"".join(chunks)
    finally:
        s.close()


class HttpTests(unittest.TestCase):
    def start(self, **kw):
        d = dict(port=0, host="127.0.0.1")
        d.update(kw)
        srv = server.RelayServer(open_config(**d))
        srv.start()
        self.addCleanup(srv.stop)
        return srv

    def post(self, conn, pkt, ctype="application/octet-stream"):
        conn.request("POST", server.ROUTE, body=pkt, headers={"Content-Type": ctype})
        resp = conn.getresponse()
        return resp, resp.read()

    def conn(self, srv):
        c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=5)
        self.addCleanup(c.close)
        return c

    def test_health(self):
        srv = self.start()
        c = self.conn(srv)
        c.request("GET", "/health")
        r = c.getresponse()
        self.assertEqual((r.status, r.read()), (200, b'{"status":"ok"}'))
        self.assertEqual(r.getheader("Content-Type"), "application/json")

    def test_keepalive_and_application_errors_are_http_200(self):
        srv = self.start()
        c = self.conn(srv)
        r, body = self.post(c, host_pkt())
        self.assertEqual(r.status, 200)
        self.assertEqual(r.getheader("Content-Type"), "application/octet-stream")
        h = parse_join(split(body)[2])
        sock = c.sock
        for _ in range(5):
            r, body = self.post(c, sync_pkt(h["token"]))
            self.assertEqual((r.status, split(body)[1]), (200, 0))
        self.assertIs(c.sock, sock, "connection must be reused")
        r, body = self.post(c, sync_pkt(b"\x00" * 32))
        self.assertEqual((r.status, split(body)), (200, (0x83, server.ST_UNAUTHORIZED, b"")))
        self.assertIs(c.sock, sock)

    def test_two_clients_interoperate(self):
        srv = self.start()
        a, b = self.conn(srv), self.conn(srv)
        h = parse_join(split(self.post(a, host_pkt())[1])[2])
        g = parse_join(split(self.post(b, join_pkt(h["code"]))[1])[2])
        self.post(b, sync_pkt(g["token"], iseq=1, inp=b"go"))
        _, body = self.post(a, sync_pkt(h["token"], sseq=1, state=b"S"))
        snap = parse_snapshot(split(body)[2])[0]
        self.assertEqual(snap["players"][1]["input"], b"go")
        snap = parse_snapshot(split(self.post(b, sync_pkt(g["token"]))[1])[2])[0]
        self.assertEqual(snap["state"], b"S")

    def test_bad_routes_and_headers(self):
        srv = self.start()
        c = self.conn(srv)
        c.request("GET", "/nope")
        r = c.getresponse()
        self.assertEqual(r.status, 404)
        r.read()
        c.close()
        c = self.conn(srv)
        c.request("POST", "/v1/online?x=1", body=host_pkt(),
                  headers={"Content-Type": "application/octet-stream"})
        r = c.getresponse()
        self.assertEqual(r.status, 404)
        r.read()
        c.close()
        c = self.conn(srv)
        c.request("GET", server.ROUTE)
        r = c.getresponse()
        self.assertEqual(r.status, 405)
        r.read()
        c.close()
        c = self.conn(srv)
        r, _ = self.post(c, host_pkt(), ctype="text/plain")
        self.assertEqual(r.status, 415)
        c.close()
        c = self.conn(srv)
        r, _ = self.post(c, b"JUNKJUNKJUNK")                 # bad magic -> HTTP 400
        self.assertEqual(r.status, 400)
        c.close()
        c = self.conn(srv)
        r, _ = self.post(c, b"short")                         # < 8 bytes
        self.assertEqual(r.status, 400)

    def test_body_bounds_and_framing(self):
        srv = self.start()
        base = (b"POST /v1/online HTTP/1.1\r\nHost: x\r\n"
                b"Content-Type: application/octet-stream\r\n")
        out = raw_exchange(srv.port, base + b"\r\n")                       # no Content-Length
        self.assertTrue(out.startswith(b"HTTP/1.1 411"), out[:40])
        out = raw_exchange(srv.port, base + b"Content-Length: 4097\r\n\r\n" + bytes(4097))
        self.assertTrue(out.startswith(b"HTTP/1.1 413"), out[:40])
        out = raw_exchange(srv.port, base + b"Content-Length: 99999999999\r\n\r\n")
        self.assertTrue(out.startswith(b"HTTP/1.1 400"), out[:40])
        out = raw_exchange(srv.port, base + b"Content-Length: abc\r\n\r\n")
        self.assertTrue(out.startswith(b"HTTP/1.1 400"), out[:40])
        out = raw_exchange(srv.port, base + b"Content-Length: 5\r\nContent-Length: 5\r\n\r\nxxxxx")
        self.assertTrue(out.startswith(b"HTTP/1.1 400"), out[:40])
        out = raw_exchange(srv.port, base + b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
        self.assertTrue(out.startswith(b"HTTP/1.1 400"), out[:40])
        # exactly 4096 bytes is within bounds: trailing garbage -> in-protocol BAD_REQUEST (HTTP 200)
        pkt = host_pkt() + bytes(4096 - len(host_pkt()))
        out = raw_exchange(srv.port, base + b"Connection: close\r\nContent-Length: 4096\r\n\r\n" + pkt)
        self.assertTrue(out.startswith(b"HTTP/1.1 200"), out[:40])
        self.assertEqual(split(out.split(b"\r\n\r\n", 1)[1])[1], server.ST_BAD_REQUEST)
        out = raw_exchange(srv.port, b"PUT /v1/online HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertTrue(out.startswith(b"HTTP/1.1 501"), out[:40])

    def test_slow_client_is_cut_off(self):
        srv = self.start(request_timeout=0.6)
        s = socket.create_connection(("127.0.0.1", srv.port), timeout=6)
        self.addCleanup(s.close)
        s.sendall(b"POST /v1/online HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n")  # never finishes
        t0 = time.monotonic()
        try:
            data = s.recv(100)
        except ConnectionError:
            data = b""
        self.assertEqual(data, b"")
        self.assertLess(time.monotonic() - t0, 5.0)

    def test_truncated_body_is_cut_off(self):
        srv = self.start(request_timeout=0.6)
        s = socket.create_connection(("127.0.0.1", srv.port), timeout=6)
        self.addCleanup(s.close)
        s.sendall(b"POST /v1/online HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\n"
                  b"Content-Length: 100\r\n\r\nabc")
        try:
            data = s.recv(100)
        except ConnectionError:
            data = b""
        self.assertEqual(data, b"")

    def test_inflight_connection_cap(self):
        srv = self.start(max_inflight=2)
        holders = [socket.create_connection(("127.0.0.1", srv.port), timeout=5) for _ in range(2)]
        for h in holders:
            self.addCleanup(h.close)
        extra = socket.create_connection(("127.0.0.1", srv.port), timeout=5)
        self.addCleanup(extra.close)
        data = extra.recv(200)
        self.assertTrue(data.startswith(b"HTTP/1.1 503"), data)
        # freeing a slot restores service
        holders[0].close()
        deadline = time.monotonic() + 5
        while True:
            c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=3)
            try:
                c.request("GET", "/health")
                if c.getresponse().status == 200:
                    break
            except (ConnectionError, http.client.HTTPException, socket.timeout):
                pass
            finally:
                c.close()
            self.assertLess(time.monotonic(), deadline, "slot was never released")
            time.sleep(0.1)

    def test_concurrent_rooms_over_http(self):
        srv = self.start(max_inflight=64)
        errors = []

        def worker(n):
            try:
                c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
                try:
                    def call(pkt):
                        c.request("POST", server.ROUTE, body=pkt,
                                  headers={"Content-Type": "application/octet-stream"})
                        r = c.getresponse()
                        data = r.read()
                        if r.status != 200:
                            raise AssertionError("http %d" % r.status)
                        return split(data)
                    _, st, body = call(host_pkt(name="H%d" % n))
                    if st != 0:
                        raise AssertionError("host %d" % st)
                    h = parse_join(body)
                    _, st, body = call(join_pkt(h["code"], name="G%d" % n))
                    if st != 0:
                        raise AssertionError("join %d" % st)
                    g = parse_join(body)
                    for i in range(1, 15):
                        _, st, body = call(sync_pkt(h["token"], iseq=i, inp=bytes((n,)),
                                                    sseq=i, state=bytes((n, i))))
                        if st not in (0, server.ST_RATE_LIMIT):
                            raise AssertionError("sync %d" % st)
                        time.sleep(0.012)
                        _, st, body = call(sync_pkt(g["token"], iseq=i, inp=bytes((n + 100,))))
                        if st not in (0, server.ST_RATE_LIMIT):
                            raise AssertionError("gsync %d" % st)
                        if st == 0:
                            snap = parse_snapshot(body)[0]
                            if snap["state"] and snap["state"][0] != n:
                                raise AssertionError("cross-room leak")
                    _, st, body = call(score_pkt(h["token"], "b", n))
                    if st != 0:
                        raise AssertionError("score %d" % st)
                finally:
                    c.close()
            except Exception as exc:
                errors.append(repr(exc))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(12)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
        self.assertEqual(errors, [])
        self.assertEqual(srv.service.stats()["rooms"], 12)

    def test_no_secrets_in_logs_or_stderr(self):
        cap = _Capture()
        root = logging.getLogger()
        old_level = root.level
        root.addHandler(cap)
        root.setLevel(logging.DEBUG)
        err = io.StringIO()
        try:
            with contextlib.redirect_stderr(err):
                srv = self.start()
                c = self.conn(srv)
                r, body = self.post(c, host_pkt())
                h = parse_join(split(body)[2])
                self.post(c, score_pkt(h["token"], "b", 5, b"rec"))
                self.post(c, sync_pkt(b"\x01" * 32))
                raw_exchange(srv.port, b"garbage\r\n\r\n")
                self.post(c, leave_pkt(h["token"]))
                srv.stop()
        finally:
            root.removeHandler(cap)
            root.setLevel(old_level)
        blob = "\n".join(cap.lines) + err.getvalue()
        self.assertNotIn(h["token"].hex(), blob)
        self.assertNotIn(h["key"].hex(), blob)
        self.assertNotIn(h["code"], blob)

    def test_xff_ignored_without_trusted_proxy(self):
        # each distinct spoofed X-Forwarded-For must still land in the same limiter bucket
        srv = self.start(ip_rate=1, ip_burst=3)
        c = self.conn(srv)
        statuses = []
        for i in range(8):
            c.request("POST", server.ROUTE, body=sync_pkt(b"\x01" * 32),
                      headers={"Content-Type": "application/octet-stream",
                               "X-Forwarded-For": "6.6.6.%d" % i})
            r = c.getresponse()
            statuses.append(split(r.read())[1])
        self.assertIn(server.ST_RATE_LIMIT, statuses)

    def test_xff_honoured_from_trusted_proxy(self):
        srv = self.start(ip_rate=1, ip_burst=3, trusted_proxies=("127.0.0.1",))
        c = self.conn(srv)
        statuses = []
        for i in range(8):
            c.request("POST", server.ROUTE, body=sync_pkt(b"\x01" * 32),
                      headers={"Content-Type": "application/octet-stream",
                               "X-Forwarded-For": "6.6.6.%d" % i})
            statuses.append(split(c.getresponse().read())[1])
        self.assertNotIn(server.ST_RATE_LIMIT, statuses)

    def test_graceful_shutdown_closes_database(self):
        srv = self.start()
        c = self.conn(srv)
        self.post(c, host_pkt())
        self.assertFalse(srv.store.closed)
        srv.stop()
        self.assertTrue(srv.store.closed)
        srv.stop()                                  # idempotent
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", srv.port), timeout=1).close()

    def test_context_manager_and_real_expiry(self):
        cfg = open_config(port=0, session_ttl=0.5)
        with server.RelayServer(cfg) as srv:
            c = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=5)
            try:
                c.request("POST", server.ROUTE, body=host_pkt(),
                          headers={"Content-Type": "application/octet-stream"})
                h = parse_join(split(c.getresponse().read())[2])
                self.assertEqual(srv.service.stats()["rooms"], 1)
                deadline = time.monotonic() + 5
                while srv.service.stats()["rooms"] and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertEqual(srv.service.stats()["rooms"], 0, "janitor should expire the room")
                c.request("POST", server.ROUTE, body=sync_pkt(h["token"]),
                          headers={"Content-Type": "application/octet-stream"})
                self.assertEqual(split(c.getresponse().read())[1], server.ST_ROOM_CLOSED)
            finally:
                c.close()


if __name__ == "__main__":
    unittest.main()
