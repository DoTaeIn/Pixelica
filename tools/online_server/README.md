# Pixelica online relay (`tools/online_server`)

A small relay for the optional `pixelica` online module: room-code **co-op**, realtime **versus**
(latest-state, about 30 Hz) and persistent **highest-score boards**.

* Python 3.10+, **standard library only** (`ThreadingHTTPServer`, `sqlite3`). One file: `server.py`.
* Binary wire protocol v1 over `POST /v1/online` (see [Wire protocol](#wire-protocol-v1)).
* In-memory rooms, SQLite score boards that survive restarts.

## Honest scope

* Suitable for **casual** real-time play where the host aggregates the latest inputs and publishes
  the latest world state. It is **not** rollback netcode and **not** UDP.
* Scores are **self-reported by clients**. The boards are casual "best score" lists. They are **not
  cheat-proof and not a ranked / anti-cheat service**; anyone who can speak the protocol can post any score.
* A player key (32 bytes) is a bearer secret: whoever holds it owns that player's board entries.
  Keys are not authenticated to a person, and a key chosen by a client with low entropy can be taken over.
* The relay speaks **plain HTTP**. For anything public use the included Caddy HTTPS proxy (below).
* Single process only: rooms live in memory. Do not run several workers/replicas against one database.

## Quick start (local)

```
python tools/online_server/server.py                    # http://127.0.0.1:8787 , ./pixelica_online.sqlite3
python tools/online_server/server.py --port 9000 --db /tmp/scores.sqlite3
curl http://127.0.0.1:8787/health                       # {"status":"ok"}
```

Client `server_url` is the base URL (the client appends `/v1/online`): `http://127.0.0.1:8787`.
Plain HTTP is accepted by clients only for localhost / explicit LAN opt-in; use HTTPS otherwise.

Run the tests (no network access beyond loopback needed):

```
python -m unittest discover -s tools/online_server -v
```

### CLI

| Option | Default | Env | Meaning |
|---|---|---|---|
| `--host` | `127.0.0.1` | `PIXELICA_HOST` | Bind address. Non-loopback logs a plain-HTTP warning. |
| `--port` | `8787` | `PIXELICA_PORT` | TCP port (`0` = ephemeral). |
| `--db` | `./pixelica_online.sqlite3` | `PIXELICA_DB` | SQLite file (parent directory is created). |
| `--session-ttl` | `30` | `PIXELICA_SESSION_TTL` | Seconds without a request before a session expires (5..3600). |
| `--max-rooms` | `256` | `PIXELICA_MAX_ROOMS` | Max live rooms. |
| `--max-sessions` | `2048` | `PIXELICA_MAX_SESSIONS` | Max live sessions (players). |
| `--max-inflight` | `64` | `PIXELICA_MAX_INFLIGHT` | Max concurrent HTTP connections (one thread each). |
| `--max-records` | `100000` | `PIXELICA_MAX_RECORDS` | Max score rows in the database. |
| `--ip-rate` | `600` | `PIXELICA_IP_RATE` | Sustained requests/s per client address (burst = 2x). |
| `--trusted-proxy CIDR` | none | `PIXELICA_TRUSTED_PROXIES` | Trust `X-Forwarded-For` only from this address/CIDR (repeat or comma list). |

Embedding / tests: `server.RelayServer(server.ServerConfig(port=0, db_path=":memory:"))` is a context
manager that serves on an ephemeral port (`srv.url`, `srv.port`) and closes the database on exit.

## Public deployment: Docker + Caddy (automatic HTTPS)

Prerequisites: a host with Docker Compose, a DNS name pointing at it, ports 80 and 443 reachable.

```
cd tools/online_server
cp .env.example .env            # edit PIXELICA_DOMAIN=relay.example.com
docker compose up -d --build
curl https://relay.example.com/health
```

Then set the client `server_url` to `https://relay.example.com`.

* `relay` publishes **no** host port; only Caddy reaches it over the private `relay_net`.
* Caddy gets/renews the certificate, accepts only `POST /v1/online` and `GET /health`, caps request
  bodies at 8 KB, sets header/body timeouts and overwrites `X-Forwarded-For` with the real client address.
* The relay trusts `X-Forwarded-For` **only** from Caddy's fixed address `172.28.77.2`
  (`PIXELICA_TRUSTED_PROXIES`). If that subnet clashes with your host, change the subnet, the `ipv4_address`
  and `PIXELICA_TRUSTED_PROXIES` together. Without a trusted proxy the header is ignored.
* Data: named volume `relay-data` (`/data/online.sqlite3`); back it up by copying the file (WAL mode;
  copy while stopped, or use `sqlite3 .backup`). Containers run read-only, non-root, with all capabilities dropped.
* Logs: no per-request logging exists; startup/shutdown and internal-error class names only. Tokens, keys and
  room codes are never logged. Caddy's own access log is off by default.

Without Docker: run `server.py` on `127.0.0.1:8787` under a service manager and point any HTTPS reverse
proxy at it, setting `--trusted-proxy` to the proxy's address.

## Limits and bounds

| Item | Limit |
|---|---|
| Request/response packet | 4096 bytes; `Content-Length` required, exact read, `Transfer-Encoding` rejected (400), 411 / 413 / 415 for missing / oversize / wrong type |
| Whole request (headers + body + reply) | 10 s wall clock, then the connection is cut; headers+body also capped at 32 KiB; idle keep-alive 30 s |
| Concurrent connections | 64 (extra connections get an immediate `503`) |
| Rooms / sessions | 256 / 2048 (`CAPACITY`) |
| Players per room | 2..8 (host chooses); `FULL` beyond |
| Input / state / record | 64 / 2048 / 128 bytes |
| Session lease | 30 s; refreshed by authenticated SYNC, BOARD, SCORE; host expiry closes the room |
| SYNC per session | 90/s sustained, burst 10 (`RATE_LIMIT`) |
| SCORE per session | 2/s, burst 2; per client address 5/s, burst 20 |
| BOARD per session | 5/s, burst 5 |
| HOST+JOIN per client address | 2/s, burst 30 (also slows room-code guessing) |
| Any request per client address | `--ip-rate` (600/s), burst 2x |
| Rate-limit tables | LRU, max 20000 addresses each, idle entries purged (IPv6 keyed by /64) |
| Leave tombstones | 8192 max, 300 s (lets guests learn `ROOM_CLOSED`) |
| Score rows | 100000 total; a new identity beyond that gets `CAPACITY`, existing rows can still improve |

## Wire protocol v1

`POST /v1/online`, `Content-Type: application/octet-stream`, one binary packet <= 4096 bytes.
All application results (including errors) are HTTP 200. Anything not answerable in-protocol
(bad magic/version, packet < 8 bytes, bad route, framing) is a plain HTTP error (400/404/405/411/413/415).

Header (8 bytes): `"P3ON"`, version `u8 = 1`, opcode `u8`, status `u16`. Requests carry status 0;
ops `1 HOST, 2 JOIN, 3 SYNC, 4 LEAVE, 5 SCORE, 6 BOARD`. Responses use `opcode | 0x80` and a status
(`0 OK, 1 BAD_REQUEST, 2 NOT_FOUND, 3 FULL, 4 UNAUTHORIZED, 5 NOT_HOST, 6 CAPACITY, 7 RATE_LIMIT,
8 ROOM_CLOSED, 9 INTERNAL`). Error responses have **no body**; lengths are exact (trailing bytes = BAD_REQUEST).
Integers are big-endian; `text` = `u8 length` + UTF-8 (no terminator).

| Op | Request body | Success response body |
|---|---|---|
| HOST | game text, name text, key[32], mode u8 (1 coop / 2 versus), max_players u8 (2..8) | token[32], key[32], code[6], self u8, SNAPSHOT |
| JOIN | game text, name text, key[32], code[6] | token[32], key[32], code[6], self u8, SNAPSHOT |
| SYNC | token[32], input_seq u32, input_len u8, input, state_seq u32, state_len u16, state | SNAPSHOT |
| LEAVE | token[32] | empty |
| SCORE | token[32], board text, score i32, record_len u8, record | BOARDRESULT |
| BOARD | token[32], board text | BOARDRESULT |

SNAPSHOT: `revision u32, host u8, count u8, mode u8, max u8, state_seq u32, state_len u16, state`,
then `count` x (`id u8, name text, input_seq u32, input_len u8, input`).
BOARDRESULT: `board text, count u8` (<= 10), then `count` x (`name text, score i32, record_len u8, record`).

Rules the server enforces:

* `game`/`board`: `[A-Za-z0-9_.-]{1,32}`. `name`: non-empty strict UTF-8, <= 32 bytes, no control characters (Unicode `Cc`).
  Codes use `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (6 chars, uppercase; clients normalise, the server is strict).
* `key` all zero = generate one (returned). Token and key come from `secrets`; tokens are stored only as SHA-256,
  identities as a domain-separated SHA-256 of the key. A key owns the score identity, **not** session authority.
* A JOIN only matches a room of the same `game`; otherwise `NOT_FOUND`.
* SYNC sequences (input and state, using `0 < ((new-old) & 0xffffffff) < 0x80000000` for "newer"):
  seq 0 with data = BAD_REQUEST; seq 0 with no data = no update; same seq + same bytes = idempotent;
  same seq + different bytes = BAD_REQUEST; older = ignored (not an error). Non-host with any state = `NOT_HOST`.
  Every check runs before any mutation, so a rejected packet changes nothing. A host sending `0/0` keeps the
  previously published state.
* Unknown or revoked token: `UNAUTHORIZED`; token whose room was closed by host leave/expiry: `ROOM_CLOSED`.
  LEAVE of an unknown session: `NOT_FOUND`. Host leave/expiry closes the room.
* Scores: one best score per (game, board, identity). Update (score, name, record) only on a **strictly higher** score;
  ties rank by who reached the score first, then identity. The name is the session name at submit time.
  SCORE answers with the refreshed top 10.

## Files

`server.py` (relay + CLI) · `test_server.py` (unittest) · `Dockerfile` · `docker-compose.yml` · `Caddyfile` · `.env.example`
