# Pixelica online starter

A standalone project that uses the **online** component of an installed Pixelica SDK
(`find_package(Pixelica CONFIG REQUIRED COMPONENTS online)` + `Pixelica::online`) and nothing else:
no renderer, no window. Copy this directory anywhere. Documentation (Korean): `docs/online.md`.

```text
CMakeLists.txt     find_package + one executable + offline CTest cases (+ optional network smoke test)
src/main.c         the whole game: "Charge" (hold a button, the host counts held ticks), co-op and versus
launch_local.py    one command: local relay + host + guest (identical to examples/hello_online/launch_local.py)
```

## Build against an installed SDK

`<prefix>` contains `lib/cmake/Pixelica` (the SDK archive extracted anywhere). The SDK must have been built
with the online module, otherwise CMake says the `online` component was not found.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

WinHTTP (Windows) or libcurl + Threads (Linux) are found by the package config, not listed here.
`-DONLINEGAME_STRICT_WARNINGS=ON` compiles with strict warnings as errors.

## Run it

```sh
# relay + two bots, verifies the result, exit status 0/1 (needs the relay's server.py, see below)
python launch_local.py --exe build/bin/online_game --smoke --mode coop

# by hand, relay on 127.0.0.1:8787 (python server.py with its default port)
build/bin/online_game --host coop --name Alice          # prints room=ABCDEF
build/bin/online_game --join ABCDEF --name Bob
```

`launch_local.py` finds `tools/online_server/server.py` when this directory still sits in the SDK
(`share/Pixelica/template_online` next to `share/Pixelica/tools/online_server`); after you copied it
elsewhere pass `--server PATH_TO/server.py` or set `PIXELICA_ONLINE_SERVER`. CMake's `Pixelica_ONLINE_SERVER`
variable (set by `find_package(Pixelica)`) holds that path. `--bot` and `--hidden` are accepted only so the
launcher works unchanged: this starter always plays itself.

`-DONLINEGAME_NETWORK_TESTS=ON` (needs Python 3.10+) adds a CTest case that runs that smoke test.

## What to change first

* `bot_hold()` -> read your real input.
* `host_step()` -> your rules (the host is the authority; guests only send inputs and draw states).
* `encode_state()` / `decode_state()` -> your state layout (max 2048 bytes; inputs max 64 bytes).
* `board_score()` and the 5-byte record -> what you want to rank and remember.

Input and state are "latest value wins" levels at about 30 Hz, not an ordered event stream: a one-tick
press can be lost, so send levels (or cumulative counters) and let the host integrate them.
