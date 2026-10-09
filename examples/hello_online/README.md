# hello_online

Runnable multiplayer minigame ("Star Catch", up to 8 players, 256x192) for the optional
Pixelica **online** module. One executable, three online features, **co-op and versus**:

| Feature | API used |
| --- | --- |
| room code | `pixelica_online_host` / `pixelica_online_join` |
| realtime exchange | `pixelica_online_input` (guest -> host, 2 bytes), `pixelica_online_state` (host -> guests, ~190 bytes) |
| scores and records | `pixelica_online_submit_score`, `pixelica_online_request_board`, `pixelica_online_leaderboard` (+ a 9-byte record) |

Full documentation (Korean): [`docs/online.md`](../../docs/online.md). The smallest copyable
starter project: [`templates/online`](../../templates/online).

## Files

```text
online_game.h/.c       game: wire codec, host simulation, bot       (no network, no renderer)
online_game_test.c     unit test of the above                       (no network)
online_session.h/.c    glue to pixelica/online.h: host/join, 60 Hz tick, score submit, board polling
online_view.h/.c       drawing with pixelica/renderer2d.h                (only with the renderer2d component)
main.c                 standalone main: CLI, 60 Hz loop, SDL3 or headless, key file, capture
launch_local.py        one command: relay + host + guest, bounded lifetime, cleanup
test_launch_local.py   stdlib unittest for the launcher's parsers/verdict (no network)
```

## Build flavours (the parent CMake chooses; this directory has no CMakeLists.txt)

| Executable | Sources | Definitions | Link |
| --- | --- | --- | --- |
| `hello_online` (SDL window) | `main.c online_session.c online_view.c` + `online_game` | `HELLO_ONLINE_WITH_RENDERER2D HELLO_ONLINE_SDL` | `pixelica_online pixelica_renderer2d pixelica_platform_sdl3 pixelica_common`, include dir `examples/common2d` (font `demo_art.h`) |
| `hello_online_headless` (console, bots only) | `main.c online_session.c` + `online_game` | none | `pixelica_online pixelica_common` |
| `hello_online_game_test` | `online_game_test.c` + `online_game.c` | none | `pixelica_common` (libm) |

`online_game.c` can be its own static library (`hello_online_game`, include dir = this directory).
Without SDL the client is always an autopilot bot; `--capture PATH` needs `HELLO_ONLINE_WITH_RENDERER2D`.

## Quick start

```sh
# 1. one command: relay + host window + guest bot (needs an SDL build)
python launch_local.py --exe <build>/hello_online --mode coop

# 2. no window, verifies room / round / scores / board and exits 0 or 1
python launch_local.py --exe <build>/hello_online_headless --smoke --mode versus

# 3. by hand (python tools/online_server/server.py running on its default 127.0.0.1:8787)
hello_online --host coop --name Alice          # prints room=ABCDEF first
hello_online --join ABCDEF --name Bob
hello_online --score 123 --board speedrun      # board only: no game, same API
```

Keys (SDL): arrows/WASD move, Space dash, Enter start round / play again, Escape quit.
`--help` lists every option. Exit status: 0 ok, 1 runtime failure, 2 usage error.

## Honest limits

Casual realtime over an HTTP relay at about 30 Hz: no rollback, no client prediction, a guest sees the
host's state with one network round trip of delay. Scores are self-reported and **not** cheat-proof.
The secret player key is never printed; `--key-file PATH` stores it for a stable ranking identity.
