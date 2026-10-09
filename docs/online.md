# 온라인 모듈 (`Pixelica::online`)

방 코드로 **협동(co-op)** 하고, **실시간 대전(versus)** 을 하고, **영구 최고 점수·기록 보드**를 쓰는
선택 모듈입니다. 2D, 2.5D, 3D 어디에서나 `common`만 있으면 쓸 수 있고 렌더러에 의존하지 않습니다.
이 문서는 사용자 관점의 안내서이며, 정확한 함수 계약은 `modules/online/include/pixelica/online.h`가 기준입니다.

> **검증 기준 2026-10-10.** Claude Opus 5.5와 Sonnet이 구현하고 Codex가 통합·감사했습니다.
> Windows·Linux 빌드, 8인 협동·대전, 영구 기록, 잘못된 응답 거부와 메모리 검사를 실행했습니다.
> 상세 결과와 실행하지 않은 범위는 `ONLINE_AUDIT.md` (local-only record)에 기록합니다.

## 목차

1. [무엇이고, 무엇이 아닌가](#1-무엇이고-무엇이-아닌가)
2. [빠른 시작](#2-빠른-시작)
3. [소스 트리에서 빌드](#3-소스-트리에서-빌드)
4. [SDK 설치와 복사용 템플릿](#4-sdk-설치와-복사용-템플릿)
5. [세 가지 기능과 API](#5-세-가지-기능과-api)
6. [복사해서 쓰는 최소 예제](#6-복사해서-쓰는-최소-예제)
7. [예제 게임 hello_online](#7-예제-게임-hello_online)
8. [영구 신원 (player key)](#8-영구-신원-player-key)
9. [점수와 기록 보드](#9-점수와-기록-보드)
10. [로컬 런처](#10-로컬-런처)
11. [릴레이 서버와 TLS 배포](#11-릴레이-서버와-tls-배포)
12. [제한 값 한눈에 보기](#12-제한-값-한눈에-보기)
13. [오류 코드와 문제 해결](#13-오류-코드와-문제-해결)
14. [스레드·수명·재접속 규칙](#14-스레드수명재접속-규칙)
15. [검증 상태와 미완 항목](#15-검증-상태와-미완-항목)

---

## 1. 무엇이고, 무엇이 아닌가

구성은 두 부분입니다.

* **클라이언트 라이브러리** `Pixelica::online` (`pixelica/online.h`, C11). Windows는 WinHTTP, Linux는 libcurl +
  pthread를 씁니다. 그 밖의 플랫폼은 구성 단계에서 멈춥니다. 기본값은 **꺼짐**(`PIXELICA_BUILD_ONLINE=OFF`)입니다.
* **릴레이 서버** `tools/online_server/server.py`. 파이썬 3.10+ 표준 라이브러리만 쓰고 SQLite에 점수를 저장합니다.

동작 방식: 백그라운드 워커 스레드가 `sync_hz`(기본 30 Hz)로 "각 플레이어의 최신 입력"과 "호스트의 최신
월드 상태"를 릴레이와 주고받습니다. 게임 루프는 이 요청을 **기다리지 않습니다**.

**잘 맞는 용도**: 캐주얼한 실시간 협동·대전, 방 코드로 친구 초대, 간단한 최고 점수 보드.

**아닌 것 (이 점을 꼭 알고 쓰세요)**

* 롤백/예측 넷코드가 아니고 UDP도 아닙니다. 지연을 숨기지 않습니다. 게스트는 호스트가 발행한 상태를
  네트워크 왕복 한 번 정도 늦게 봅니다.
* 입력과 상태는 **"최신 값이 이긴다"** 입니다. 중간 값이 사라질 수 있으므로 순서 있는 이벤트 스트림이
  아닙니다(한 틱짜리 버튼 눌림은 유실될 수 있음 → 누르고 있는 *상태*나 누적 카운터를 보내세요).
* 점수는 **클라이언트가 스스로 보고**합니다. 프로토콜을 아는 누구나 아무 점수나 올릴 수 있으므로 치팅 방지
  기능이 있는 랭크 서비스가 아닙니다. 캐주얼 "최고 기록 목록"입니다.
* 릴레이는 평문 HTTP를 말합니다. 공개 서비스는 반드시 HTTPS 리버스 프록시(동봉된 Caddy) 뒤에 두세요.
* 자동 재접속이 없습니다. 방은 호스트가 나가거나 만료되면 사라집니다.

## 2. 빠른 시작

### 2.1 한 줄 로컬 실행 (런처)

```sh
python examples/hello_online/launch_local.py --exe <빌드>/hello_online --mode coop
```

릴레이를 빈 포트에 띄우고, 호스트(내 창)와 게스트(봇)를 차례로 실행합니다. 창에서 방 코드와 참가자가
보이고 게스트가 들어오면 라운드가 자동으로 시작합니다. 창 없이 자동 검증만 하려면:

```sh
python examples/hello_online/launch_local.py --exe <빌드>/hello_online_headless --smoke --mode versus
```

`smoke OK`가 나오고 종료 코드가 0이면 방 생성 → 참가 → 라운드 → 점수 제출 → 보드 조회까지 한 번 돈 것입니다.
자세한 옵션은 [10장](#10-로컬-런처).

### 2.2 손으로 실행

```sh
# 터미널 1: 릴레이 (기본 127.0.0.1:8787, ./pixelica_online.sqlite3)
python tools/online_server/server.py

# 터미널 2: 호스트. 첫 줄에 room=ABCDEF 가 출력됩니다.
hello_online --host coop --name Alice --players 2

# 터미널 3: 게스트
hello_online --join ABCDEF --name Bob

# 방 없이 점수만 올리고 보드 보기 (같은 API)
hello_online --score 123 --board speedrun
```

`--server`를 생략하면 `http://127.0.0.1:8787`을 씁니다. SDL 빌드의 조작: 방향키/WASD 이동, Space 대시,
Enter 라운드 시작/다시 하기, Esc 종료.

## 3. 소스 트리에서 빌드

```sh
cmake -S . -B build -DPIXELICA_BUILD_ONLINE=ON            # 필요하면 -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_BUILD_2D=ON
cmake --build build --config Release
```

* Windows: WinHTTP(시스템 라이브러리)라 추가 설치가 없습니다. 시스템 인증서 저장소로 검증합니다.
* Linux: libcurl 개발 패키지(7.68 이상)와 Threads가 필요합니다(예: `libcurl4-openssl-dev`).
* 그 외 플랫폼에서 `PIXELICA_BUILD_ONLINE=ON`은 구성 오류입니다.

예제 소스는 `examples/hello_online/`에 있고 **자체 CMakeLists.txt는 없습니다.** 루트 쪽 CMake가 아래 표대로
연결합니다. 아래 이름은 현재 빌드에서 생성되는 실제 타깃입니다.

| 실행 파일 | 소스 (`examples/hello_online/`) | 컴파일 정의 | 링크 |
| --- | --- | --- | --- |
| `hello_online` (SDL 창) | `main.c online_session.c online_view.c online_game.c` | `HELLO_ONLINE_WITH_RENDERER2D`, `HELLO_ONLINE_SDL` | `pixelica_online pixelica_renderer2d pixelica_platform_sdl3 pixelica_common`, include 경로 `examples/common2d` |
| `hello_online_headless` (콘솔, 봇 전용) | `main.c online_session.c online_game.c` | 없음 | `pixelica_online pixelica_common` |
| `hello_online_game_test` | `online_game_test.c online_game.c` | 없음 | `pixelica_common` (libm) |

* 모든 타깃의 include 경로에 `examples/hello_online` 자신이 필요합니다.
* `online_view.c`가 `demo_art.h`(5x7 비트맵 글꼴)를 쓰기 때문에 `examples/common2d`가 include 경로에 있어야 합니다.
* 창 없는 빌드에서도 `renderer2d`가 있으면 `HELLO_ONLINE_WITH_RENDERER2D`만 정의해 매 프레임 렌더링하고
  `--capture PATH`로 PPM을 저장할 수 있습니다. 이 정의가 없으면 `--capture`는 사용 오류(종료 코드 2)입니다.
* 파이썬 단위 시험: `python examples/hello_online/test_launch_local.py` (네트워크 불필요).

## 4. SDK 설치와 복사용 템플릿

설치된 SDK에서는 패키지 설정이 플랫폼 라이브러리(WinHTTP / libcurl + Threads)를 찾아 줍니다. 사용자는 나열하지 않습니다.

```cmake
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS online)
target_link_libraries(my_game PRIVATE Pixelica::online)
```

* SDK가 online 모듈을 포함해 빌드되지 않았다면 `online` 컴포넌트를 찾지 못했다는 오류가 납니다.
* `find_package`는 `Pixelica_ONLINE_TEMPLATE_DIR`(설치된 `share/.../template_online`)과
  `Pixelica_ONLINE_SERVER`(설치된 `share/.../tools/online_server/server.py`)를 설정합니다. 해당 파일이 설치되어
  있지 않으면 빈 문자열입니다.
* Linux에서는 `find_package`가 CURL과 Threads를 다시 찾으므로 소비자 쪽에도 libcurl 개발 패키지가 필요합니다.

**복사용 템플릿** `templates/online` (설치본에서는 `share/.../template_online`):

```text
CMakeLists.txt     find_package(... COMPONENTS online) + 실행 파일 1개 + 오프라인 CTest
src/main.c         게임 전체 ("Charge": 버튼을 누르고 있으면 호스트가 틱을 세어 점수를 줌), co-op/versus
launch_local.py    examples/hello_online/launch_local.py 와 바이트 단위로 같은 런처
README.md
```

```sh
cp -r <sdk>/share/Pixelica/template_online my_online_game && cd my_online_game
cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdk> -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python launch_local.py --exe build/bin/online_game --smoke --server <sdk>/share/Pixelica/tools/online_server/server.py
```

템플릿은 `pixelica/online.h`만 씁니다(렌더러·창 없음). 게임 규칙은 `host_step()`, 입력은 `bot_hold()`, 상태
형식은 `encode_state()/decode_state()`, 보드 점수와 기록은 `board_score()`와 5바이트 `record`를 바꾸면 됩니다.
`-DONLINEGAME_NETWORK_TESTS=ON`이면 위 smoke 시험이 CTest(`online_game_smoke`, 라벨 `network`)로 추가됩니다.

## 5. 세 가지 기능과 API

모든 함수는 **클라이언트를 만든 스레드(게임 스레드)** 에서만 부르세요. 네트워크 I/O는 내부 워커가 합니다.

### 5.1 방 코드 + 모드 (co-op / versus)

```c
PIXELICA_OnlineConfig config = pixelica_online_config_default();
config.server_url  = "http://127.0.0.1:8787";   /* 공개 서버는 https://... */
config.game_id     = "my_game";                  /* [A-Za-z0-9_.-]{1,32}: 방과 보드를 나눔 */
config.player_name = "Alice";                    /* 비어 있지 않은 UTF-8, 32바이트 이하, 제어 문자 없음 */
config.player_key  = NULL;                       /* NULL = 새 신원. 8장 참고 */

PIXELICA_OnlineStatus error;
PIXELICA_Online *host  = pixelica_online_host(&config, PIXELICA_ONLINE_COOP, 4, &error);      /* 모드 + 최대 인원 2..8 */
PIXELICA_Online *guest = pixelica_online_join(&config, "ABCDEF", &error);               /* 방 코드 6자, 소문자 허용 */
const char *code  = pixelica_online_room_code(host);                               /* destroy 전까지 유효 */
```

* 방 모드는 `PIXELICA_ONLINE_COOP`(1) 또는 `PIXELICA_ONLINE_VERSUS`(2)이며 방에 저장됩니다. 참가자는
  `pixelica_online_snapshot`의 `mode`로 확인합니다. 모드의 의미(협동 점수 합산, 대전 규칙 등)는 **게임이 정합니다.**
  릴레이는 모드를 저장하고 알려줄 뿐 규칙을 강제하지 않습니다.
* 방 코드 알파벳은 `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`(6자). JOIN은 **같은 `game_id`** 의 방에만 붙습니다.
* `pixelica_online_host/join`만 네트워크를 **동기적으로** 기다립니다(`timeout_ms`, 기본 1500 ms). 실패하면 `NULL`과
  `error`를 돌려줍니다(`error`는 `NULL` 가능).
* 호스트의 플레이어 id가 0이라는 보장은 없습니다. `snapshot.host_id`/`self_id`를 쓰세요.
* `pixelica_online_destroy`는 워커를 멈추고 최선 노력으로 방을 떠난 뒤 해제합니다. `NULL`은 안전합니다.
  **호스트가 나가면 방 전체가 닫힙니다.**

### 5.2 실시간 입력/상태 교환

```c
/* 모든 플레이어: 내 최신 입력 (<= 64바이트). 복사되고 최신 값이 이김. 크기 0 = 지우기 */
pixelica_online_input(online, input_bytes, input_size);

/* 호스트만(아니면 PIXELICA_ONLINE_NOT_HOST): 권위 있는 월드 상태 (<= 2048바이트) */
pixelica_online_state(online, state_bytes, state_size);

/* 모두: 최신 검증된 스냅샷 복사 */
PIXELICA_OnlineSnapshot snap;
PIXELICA_OnlineStatus st = pixelica_online_snapshot(online, &snap);
```

`PIXELICA_OnlineSnapshot` 주요 필드: `room_code`, `self_id`, `host_id`, `player_count`, `max_players`, `mode`,
`connected`, `revision`, `state_sequence`, `state_size/state[]`, `players[0..player_count-1]`
(`id`, `name`, `input_sequence`, `input_size/input[]`).

* **호스트 권위 모델**: 게스트는 입력만 보내고, 호스트가 모든 플레이어의 최신 입력(`snap.players[i].input`)으로
  시뮬레이션해 상태를 발행하며, 게스트는 `snap.state`를 그리기만 합니다. 예제가 정확히 이렇게 합니다.
* 시퀀스 번호는 내부에서 만들며 **바이트가 바뀔 때만** 증가합니다. 매 프레임 같은 값을 보내도 됩니다.
* `connected == false`면 일시적 전송 오류이거나 치명 상태입니다. 이때 나머지 필드는 *마지막으로 받은 정상
  스냅샷*이지 새 권위 데이터가 아닙니다. 일시 오류는 워커가 지수 백오프로 재시도합니다. 방 닫힘·세션 만료·폐기
  같은 치명 상태가 되면 워커는 더 교환하지 않습니다 → 5.1의 `destroy` 후 다시 `join`.
* 구조체를 통째로 보내지 마세요. 필드마다 명시적으로 인코딩하세요(예제는 빅엔디언 `u16/u32` + `f32` 비트).
  `float`은 `memcpy`로 비트를 옮기며 IEEE binary32 빌드를 가정합니다(예제가 컴파일 시점에 확인).
* 디코드는 **검증을 전부 끝낸 뒤에** 상태를 바꿉니다(길이 정확히 일치, 열거값, 유한한 좌표 범위, id 중복 없음).
  잘못된 상태 패킷은 무시하고 이전 상태를 유지합니다.

### 5.3 점수와 기록

```c
uint8_t record[9];   /* 점수와 같이 저장되는 불투명 바이트 (<= 128) */
pixelica_online_submit_score(online, "coop", score, record, sizeof record);   /* 큐에 1건, 비차단 */
pixelica_online_request_board(online, "coop");                                 /* 조회 1건, 비차단 */

PIXELICA_OnlineLeaderboard lb;
pixelica_online_leaderboard(online, &lb);   /* 마지막으로 완료된 결과 복사 */
/* lb.generation 이 바뀌면 새 결과. 0 이면 아직 완료된 것이 없음 */
```

자세한 규칙은 [9장](#9-점수와-기록-보드)입니다. 같은 `online` 핸들(방 세션)에서 부르는 **같은 API**이고,
점수용 별도 연결이나 로그인 같은 것이 없습니다.

## 6. 복사해서 쓰는 최소 예제

호스트와 게스트가 입력을 주고받고, 끝에 점수를 올리고 상위 10개를 읽는 가장 작은 형태입니다.
(공개 헤더 이름 그대로입니다. 한 파일 `minimal.c`.)

```c
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "pixelica/online.h"

static void wait_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    (void)nanosleep(&t, NULL);
#endif
}

int main(int argc, char **argv)
{
    static PIXELICA_OnlineSnapshot snap;      /* 약 3 KB: 스택에 두지 않음 */
    static PIXELICA_OnlineLeaderboard board;
    PIXELICA_OnlineConfig config = pixelica_online_config_default();
    PIXELICA_OnlineStatus error = PIXELICA_ONLINE_OK;
    PIXELICA_Online *online;
    uint32_t frame;

    config.server_url = getenv("PIXELICA_ONLINE_SERVER_URL");
    if (config.server_url == NULL) config.server_url = "http://127.0.0.1:8787";
    config.game_id = "minimal";
    config.player_name = argc > 1 ? "Guest" : "Host";

    online = argc > 1 ? pixelica_online_join(&config, argv[1], &error)               /* minimal ABCDEF */
                      : pixelica_online_host(&config, PIXELICA_ONLINE_COOP, 4, &error);   /* minimal */
    if (online == NULL) {
        fprintf(stderr, "online: %s\n", pixelica_online_status_string(error));
        return 1;
    }
    printf("room=%s\n", pixelica_online_room_code(online));   /* 상대에게 알려 줄 코드 */
    fflush(stdout);

    for (frame = 0; frame < 600; ++frame) {              /* 약 10초짜리 설명용 루프 */
        const uint8_t input[2] = {1u, (uint8_t)(frame / 30u & 1u)};
        uint8_t state[2];

        pixelica_online_input(online, input, sizeof input);   /* 내 최신 입력 */
        pixelica_online_snapshot(online, &snap);              /* 최신 방 상태 복사 */
        if (snap.self_id == snap.host_id) {              /* 호스트만 상태를 발행 */
            state[0] = 1u;
            state[1] = snap.player_count;
            pixelica_online_state(online, state, sizeof state);
        }
        /* ... snap.players[i].input 으로 시뮬레이션, snap.state 로 그리기 ... */
        wait_ms(16u);                                  /* 실제 게임은 고정 스텝/렌더 루프 사용 */
    }

    {   /* 기록: 점수 + 9바이트 이내의 "어떻게 얻었는지" 메모 */
        const uint8_t record[3] = {1u, snap.player_count, 0u};

        if (pixelica_online_submit_score(online, "highscore", 42, record, sizeof record) != PIXELICA_ONLINE_OK) {
            pixelica_online_destroy(online);
            return 1;
        }
    }
    /* 응답(갱신된 상위 10개)은 비동기로 도착합니다: generation 이 0 보다 커질 때까지 폴링 */
    for (frame = 0; frame < 500u; ++frame) {             /* 응답 대기는 최대 약 5초 */
        pixelica_online_leaderboard(online, &board);
        if (board.generation != 0u) break;
        wait_ms(10u);
    }
    for (frame = 0; frame < board.count; ++frame) {
        printf("%u. %s %d\n", (unsigned)frame + 1u, board.entries[frame].name,
               (int)board.entries[frame].score);
    }
    if (snap.self_id == snap.host_id) wait_ms(1000u);     /* 게스트도 제출을 마칠 시간 */
    pixelica_online_destroy(online);                          /* 호스트면 방이 닫힘 */
    return board.generation != 0u && board.status == PIXELICA_ONLINE_OK ? 0 : 1;
}
```

위 코드는 단독 컴파일할 수 있습니다. 실제 게임에서는 프레임 루프 안에서 `generation` 변화를 확인하세요.
고정 스텝, 종료 처리와 두 명 자동 실행을 포함한 시작 프로젝트는 `templates/online`입니다.

## 7. 예제 게임 hello_online

**Star Catch**: 256x192 경기장에서 최대 8명이 별을 모읍니다(라운드 기본 45초).

실제 Windows 실행에서 호스트와 게스트가 함께 참가한 화면입니다.

협동:

Star Catch 협동 실행 화면 (local-only image, not in the repository)

대전:

Star Catch 대전 실행 화면 (local-only image, not in the repository)

| | CO-OP | VERSUS |
| --- | --- | --- |
| 점수 | 모두가 하나의 **팀 점수**를 공유(별 하나 = +1) | 각자의 점수 |
| 위험 | 튕겨 다니는 가시 3개: 닿으면 기절 + 팀 점수 -1 | 없음 |
| 대시 | 이동용 | 부딪히면 상대를 기절시키고 점수 1을 **빼앗음** |
| 보드 점수 | 팀 점수 (기본 보드 `coop`) | 내 점수 (기본 보드 `versus`) |

### 7.1 구조

```text
online_game.c      규칙 + 와이어 코덱 + 봇. 네트워크·렌더러·플랫폼 없음 → SDL/콘솔/단위 시험이 같은 코드 사용
online_session.c   pixelica/online.h 연결: host/join, 60 Hz tick, 입력 전송, 상태 발행, 점수 제출, 보드 폴링
online_view.c      renderer2d 로 그리기 (pixelica2d_fill_rect + pixelica2d_draw_text 만 사용, 텍스처 없음)
main.c             독립 main: CLI, 60 Hz 루프, SDL3 또는 콘솔, 키 파일, PPM 캡처
```

* **호스트가 권위**입니다. 호스트가 60 Hz 고정 틱으로 모든 플레이어의 최신 입력을 모아 시뮬레이션하고,
  30 Hz(프레임 2회마다)로 상태를 발행합니다. 호스트 자신의 입력은 네트워크를 거치지 않고 바로 씁니다.
* 게스트는 2바이트 입력을 보내고 호스트 상태를 그립니다. 그릴 때 위치를 보간해 30 Hz 계단을 줄입니다.
  예측/롤백은 없습니다.
* 호스트는 느린 게스트를 기다리지 않고 최신 입력으로 진행합니다. 강제 종료된 게스트의 마지막 입력은
  기본 세션 만료 30초까지 남을 수 있습니다. 즉시 멈춰야 하는 동작은 게임에서 별도 유효 시간을 두세요.
* 릴레이에 닿지 않으면 호스트는 시뮬레이션을 **멈추고**(혼자 진행하지 않음) "DISCONNECTED" 배너를 띄웁니다.

### 7.2 와이어 형식 (모두 빅엔디언, 구조체 직렬화 없음)

| 메시지 | 바이트 | 내용 |
| --- | --- | --- |
| 입력 (게스트 → 호스트) | 2 | `u8 version=1`, `u8 buttons` (비트 1 좌, 2 우, 4 상, 8 하, 16 대시) |
| 상태 (호스트 → 게스트) | 75~190 (인원·모드에 따라) | `u8 version=1, u8 mode, u8 phase, u16 round, u32 seed, u32 tick, u32 round_ticks, u16 team_score, u8 star_count + star*(f32 x, f32 y), u8 hazard_count + hazard*(f32 x, f32 y), u8 player_count + player*(u8 id, f32 x, f32 y, u16 score, u8 stun, u8 dash)` |
| 기록 (점수와 함께) | 9 | `u8 version=1, u8 mode, u8 players, u16 round_seconds, u32 seed` |

`phase`: 0 로비, 1 진행, 2 종료. 모든 수신 바이트는 길이·버전·열거값·좌표 범위·id 순서를 검사한 뒤에만
반영합니다(`hol_view_decode`). 점수 보드에는 위 9바이트 기록이 함께 저장되어 "몇 명이서, 몇 초 라운드로,
어떤 시드에서" 얻은 점수인지 되돌려 읽을 수 있습니다(콘솔 출력의 `mode=… players=… seconds=…`).

### 7.3 명령줄

| 옵션 | 의미 |
| --- | --- |
| `--host coop\|versus` | 방 만들기 |
| `--join CODE` | 방 코드로 참가 (소문자 허용) |
| `--server URL` | 릴레이, 기본 `http://127.0.0.1:8787` |
| `--game ID` | 게임 id (보드를 나눔), 기본 `hello_online` |
| `--name NAME` | 이름 (1~32바이트), 기본 Host / Guest / Player |
| `--players N` | 호스트: N명이 모이면 첫 라운드를 자동 시작. 봇/콘솔 빌드 기본 2, SDL 기본은 Enter로 수동 |
| `--max-players N` | 호스트: 방 정원 2~8, 기본 4 |
| `--round-seconds N` | 호스트: 라운드 길이 3~300, 기본 45 |
| `--board NAME` | 보드 이름 `[A-Za-z0-9_.-]{1,32}`, 기본 모드 이름(`coop`/`versus`) |
| `--score N` | 플레이한 점수 대신 N을 제출. `--host/--join` 없이 쓰면 **보드 전용 모드** |
| `--bot` | 키보드 대신 자동 조종 (SDL 없는 빌드는 항상 봇) |
| `--frames N` | 최대 N 프레임(60 Hz). 봇/숨김 모드 기본 10800(3분), SDL 대화형은 무제한 |
| `--exit-after-round` | 라운드 종료 + 점수 제출 + 보드 응답까지 끝나면 0으로 종료. 끝나지 않으면 1 |
| `--linger-seconds N` | 위 옵션과 함께: N초 더 머문 뒤 보드를 한 번 더 읽고 종료 (호스트 기본 3) |
| `--key-file PATH` | 영구 신원 키 파일 ([8장](#8-영구-신원-player-key)) |
| `--allow-insecure-lan` | 사설 LAN 주소로의 평문 http 허용 (루프백은 필요 없음) |
| `--timeout-ms N`, `--sync-hz N` | 요청 타임아웃 100~10000, 교환 속도 1~60 |
| `--hidden`, `--capture PATH` | SDL 더미 비디오(창 없음), 마지막 프레임을 PPM으로 저장 |

종료 코드: `0` 성공, `1` 실행 실패(연결·라운드 미종료·점수 거부), `2` 사용 오류.

**보드 전용 모드**(`--score N` 또는 `--board NAME`만 지정): 점수 API는 세션 토큰이 필요하므로, 2인 방 하나를
임시로 만들어 토큰만 얻고 점수를 제출/조회한 뒤 방을 닫습니다. 새로 조회된 보드를 출력하고 종료합니다.

### 7.4 표준 출력 규약 (런처·시험이 의존)

* 방을 연 직후 **첫 줄**에 `room=ABCDEF`만 출력하고 flush합니다.
* 나머지는 `hello_online: …` 줄입니다: `role=… mode=… self=…`, `players=2/4`, `phase=playing round=1`,
  `result round=1 mode=coop team_score=… your_score=…`, `score submitted board=… score=…`,
  `board name=coop generation=2 status=0 status_text=… count=2` 뒤에 `board 1 Alice 42 mode=coop players=2 seconds=45` 형식 행들,
  마지막에 `done frames=… round_done=1`.
* **플레이어 키는 절대 출력하지 않습니다.** (런처 smoke 시험은 출력에 64자리 16진수가 나오면 실패로 처리합니다.)

## 8. 영구 신원 (player key)

`config.player_key`가 `NULL`이면 릴레이가 새 무작위 신원을 만들어 줍니다. 그 값이
`pixelica_online_player_key(online)`(64자리 16진수)이고, **다음에 같은 값을 `player_key`에 넣으면 같은 사람으로
인식**되어 보드에서 "내 최고 기록 1개"를 이어 갑니다.

* 키는 **비밀**입니다. 가진 사람이 그 신원의 보드 항목을 소유합니다(세션 권한은 아님). 로그·화면·URL·이슈
  트래커에 올리지 마세요. 예제는 어디에도 출력하지 않습니다.
* 예제 사용법: `--key-file my.key`. 파일이 있으면 읽어서 쓰고, **없으면 첫 연결 뒤에 새로 만듭니다**
  (POSIX는 소유자만 읽기/쓰기 `0600`, 기존 파일은 덮어쓰지 않음). 파일 내용은 64자리 16진수 한 줄입니다.
  형식이 틀리면 키를 출력하지 않고 "key file … must hold exactly 64 hex characters"만 알리고 종료 코드 2입니다.
* 키를 잃으면 새 신원이 되므로 이전 기록은 이어지지 않습니다. 같은 키를 두 기기에서 쓰면 같은 사람으로 취급됩니다.
* 이름은 신원이 아닙니다. 보드의 이름은 **점수를 올린 순간의 세션 이름**이며 사용자 임의 덮어쓰기 필드가 없습니다.
* 키는 낮은 엔트로피로 직접 정하지 마세요(추측하면 탈취됩니다). 반드시 `pixelica_online_player_key`가 돌려준 값을 쓰세요.
* 앱에서 저장할 때는 사용자 전용 설정 폴더에 두고 권한을 제한하세요. 플랫폼 보안 저장소(키체인/자격 증명 관리자)가
  있으면 그쪽이 더 낫습니다. 이 모듈은 저장을 대신해 주지 않습니다.

## 9. 점수와 기록 보드

* **파티션**: `game_id` + `board`마다 독립된 보드입니다. 이름은 `[A-Za-z0-9_.-]{1,32}`.
* **키당 최고 점수 1개**: 같은 신원은 점수가 *엄격히 더 높을 때만* 점수·이름·기록이 갱신됩니다.
  시간이 짧을수록 좋은 게임은 음수 밀리초를 점수로 보내고 원래 시간을 기록 데이터에 넣을 수 있습니다.
  동점이면 먼저 달성한 쪽이 앞이고 그다음은 신원 순으로 결정적입니다.
* **상위 10개**: `PIXELICA_ONLINE_MAX_SCORES`. 각 항목은 `name`, `score`(`int32_t`), `record_size`, `record[128]`.
* **기록(record)**: 최대 128바이트의 불투명 바이트. 리플레이 시드, 사용 캐릭터, 클리어 시간 같은 것을
  게임이 정한 형식으로 넣고 읽을 때 디코드하세요(예제는 9바이트, 길이·버전 검사 포함). `record`는 `NULL`,
  `record_size`는 0이어도 됩니다.
* **비차단·단일 슬롯**: `pixelica_online_submit_score`와 `pixelica_online_request_board`는 요청을 큐에 넣고 즉시
  돌아옵니다. 점수 대기 슬롯 1개, 조회 대기 슬롯 1개이며 워커가 아직 시작하지 않은 요청은 새 호출이 대체합니다
  (최신 우선). 연속 호출이 모두 전송된다고 가정하지 마세요.
* **완료 확인**: 작업이 하나 끝날 때마다 `leaderboard().generation`이 1씩 올라갑니다. 점수 제출이 완료되면
  갱신된 상위 10개가 결과로 옵니다. 실패해도 `generation`은 올라가고 `status`에 오류가 들어가며, `board/count/entries`는
  **마지막 성공 결과를 유지**합니다. 잘못된 응답은 적용하지 않습니다.
* **속도 제한**: 릴레이는 세션당 점수 2건/초, 조회 5건/초로 제한합니다(`RATE_LIMIT`). 예제는 라운드 끝에 한 번만
  제출하고 실패하면 1초 간격으로 5번까지 재시도한 뒤 포기하며 `score not accepted: …`를 알립니다.
* 예제는 라운드가 끝나면 **각 클라이언트가 자기 점수를 제출**합니다(co-op은 팀 점수, versus는 내 점수).
  치팅 방지는 없습니다. 서버는 점수를 검증하지 못합니다.

## 10. 로컬 런처

`examples/hello_online/launch_local.py` (파이썬 3.10+, 표준 라이브러리만, 템플릿에 동일 파일 사본).

```sh
python launch_local.py --exe <hello_online> [--mode coop|versus] [--smoke] [--both-human]
                       [--round-seconds N] [--game ID] [--port N] [--db FILE]
                       [--server PATH/server.py] [--server-url URL] [--timeout SEC]
```

하는 일: 빈 `127.0.0.1` 포트에 릴레이 시작(`-E` 모드의 파이썬, 임시 SQLite) → `/health` 대기(프록시 미사용) →
호스트 실행 → 호스트가 출력한 `room=CODE`를 읽음 → 게스트를 `--join CODE`로 실행 → 끝나면 정리.

| 옵션 | 의미 |
| --- | --- |
| (기본) | 호스트 = 내 창(SDL 빌드), 게스트 = 봇. `--players 2`로 게스트가 들어오면 자동 시작 |
| `--both-human` | 게스트도 사람이 조종하는 창 |
| `--smoke` | 둘 다 봇 + 창 없음 + 짧은 라운드(기본 6초). 출력 검증 후 종료 코드 0/1 |
| `--db FILE` | 점수 DB를 **유지** (기본은 종료 시 삭제되는 임시 파일) |
| `--server-url URL` | 릴레이를 띄우지 않고 이미 실행 중인 서버(예: `https://…`) 사용 |
| `--timeout SEC` | 전체 하드 제한 (기본 600, smoke 90) |

* **수명 제한**: 모든 자식 프로세스는 `finally`에서 정리됩니다 — 클라이언트에게 먼저 스스로 종료할 시간을 주고
  (그 사이 방을 떠남), 안 되면 `terminate` 후 `kill`합니다. 릴레이는 마지막에 정리합니다. Ctrl-C와 SIGTERM도 같은
  경로입니다. Windows에서 `terminate`는 강제 종료이므로 그 경우 방은 서버 TTL(30초)로 정리됩니다.
* **비밀 미출력**: 런처는 키를 다루거나 전달하지 않고, 자식 출력에서 64자리 16진수가 보이면 smoke를 실패 처리합니다.
* **탐색 규칙**: 서버/실행 파일은 *런처 스크립트 주변*(그리고 `--server`, 환경 변수 `PIXELICA_ONLINE_SERVER`)에서만
  찾습니다. **현재 작업 디렉터리는 탐색하지 않으므로** 신뢰할 수 없는 폴더에서 실행해도 그 안의 프로그램을 시작하지 않습니다.
* smoke가 검증하는 것: 방 코드 출력, 호스트·게스트 모두 `phase=finished`/결과/점수 제출 로그, 호스트의 마지막 보드가
  올바른 이름·상태 0·**Host와 Guest 둘 다 포함**, 두 프로세스 종료 코드 0, 출력에 비밀 없음.
* 같은 런처는 CLI/표준 출력 규약([7.4](#74-표준-출력-규약-런처시험이-의존))을 따르는 다른 클라이언트(템플릿 `online_game`
  포함)에도 그대로 쓸 수 있습니다.

## 11. 릴레이 서버와 TLS 배포

### 11.1 로컬

```sh
python tools/online_server/server.py                       # 127.0.0.1:8787, ./pixelica_online.sqlite3
python tools/online_server/server.py --port 9000 --db /tmp/scores.sqlite3
curl http://127.0.0.1:8787/health                          # {"status":"ok"}
```

옵션: `--host --port --db --session-ttl --max-rooms --max-sessions --max-inflight --max-records --ip-rate
--trusted-proxy` (환경 변수 `PIXELICA_*`로도 지정). 서버 단위 시험: `python -m unittest discover -s tools/online_server -v`.
자세한 서버 설명은 `tools/online_server/README.md`.

### 11.2 클라이언트가 허용하는 URL (`server_url`)

| URL | 허용 |
| --- | --- |
| `https://host[:port][/base]` | 항상. 인증서와 호스트 이름을 플랫폼/라이브러리 기본값으로 검증 |
| `http://localhost`, `http://127.0.0.1`, `http://[::1]` | 루프백만 평문 허용 |
| `http://192.168.x.x` 같은 사설 LAN, `*.local` | `allow_insecure_lan = true`를 명시했을 때만. **암호화되지 않음** |
| 그 외 `http://…` | 거부 |

URL은 최대 512바이트이며 자격 증명(`user:pass@`), 쿼리(`?`), 프래그먼트(`#`)를 포함할 수 없습니다. **리다이렉트는 따라가지
않습니다.** 인증서 오류를 무시하는 옵션은 없습니다. 요청은 `<base>/v1/online`로 갑니다.

### 11.3 공개 배포 (Docker + Caddy, 자동 HTTPS)

사전 조건: Docker Compose가 있는 호스트, 그 호스트를 가리키는 DNS 이름, 80/443 포트 접근 가능.

```sh
cd tools/online_server
cp .env.example .env            # PIXELICA_DOMAIN=relay.example.com 로 수정
docker compose up -d --build
curl https://relay.example.com/health
```

그다음 클라이언트의 `server_url`을 `https://relay.example.com`으로 바꿉니다. 예제에서는
`--server https://relay.example.com`입니다.

서버 README가 설명하는 구성: 릴레이는 호스트 포트를 열지 않고 Caddy만 사설 네트워크로 접근하며, Caddy는 인증서를
발급·갱신하고 `POST /v1/online`과 `GET /health`만 통과시키고 본문 크기·타임아웃을 제한하고 `X-Forwarded-For`를 실제
클라이언트 주소로 덮어씁니다. 릴레이는 **지정한 프록시(`--trusted-proxy`)에서 온** `X-Forwarded-For`만 신뢰합니다.
점수 DB는 이름 있는 볼륨에 저장되며 SQLite 파일 복사나 `sqlite3 .backup`으로 백업합니다.

배포 체크리스트:

1. 공개 인터넷에는 반드시 HTTPS 프록시 뒤에 둡니다(릴레이 자체는 평문 HTTP).
2. 릴레이는 **프로세스 하나**만(방이 메모리에 있음). 같은 DB에 복제본을 붙이지 마세요.
3. 리버스 프록시를 직접 구성한다면 `--trusted-proxy`를 그 프록시 주소로만 지정하세요. 지정하지 않으면 전달 헤더를 무시합니다.
4. `/health`로 모니터링하고 DB 파일을 백업하세요.
5. 클라이언트 쪽: 사설 LAN 평문(`allow_insecure_lan`)은 개발·집 안 테스트 용도로만 쓰세요.
6. 자체 서명 인증서를 쓰면 클라이언트가 거부합니다(검증을 끌 수 없음). 신뢰할 수 있는 CA 인증서를 쓰세요.

## 12. 제한 값 한눈에 보기

**클라이언트 / 프로토콜**

| 항목 | 값 |
| --- | --- |
| 플레이어 수 / 방 | 방당 2~8명 (`PIXELICA_ONLINE_MAX_PLAYERS` = 8) |
| 입력 | 최대 64바이트 (`PIXELICA_ONLINE_MAX_INPUT`) |
| 상태 | 최대 2048바이트, 호스트만 (`PIXELICA_ONLINE_MAX_STATE`) |
| 점수 기록 | 최대 128바이트 (`PIXELICA_ONLINE_MAX_RECORD`), 보드 상위 10개 |
| game id / board / 이름 | UTF-8 32바이트 이하 (game·board는 `[A-Za-z0-9_.-]`) |
| 방 코드 | 6자, `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` |
| `timeout_ms` | 100~10000, 기본 1500 (요청당) |
| `sync_hz` | 1~60, 기본 30 |
| `server_url` | 최대 512바이트 |
| 한 패킷 | 최대 4096바이트, 항상 HTTP 200 안에 애플리케이션 오류 포함 |

**릴레이 서버 기본값**

| 항목 | 한계 |
| --- | --- |
| 방 / 세션 | 256 / 2048 (`CAPACITY`) |
| 세션 유지 | 요청이 없으면 30초 후 만료, 호스트 만료 = 방 닫힘 |
| 동기화(SYNC) | 세션당 지속 90/초 |
| 점수 제출 | 세션당 2/초, 주소당 5/초 |
| 보드 조회 | 세션당 5/초 |
| 방 만들기+참가 | 주소당 2/초(방 코드 추측도 늦춤) |
| 동시 연결 | 64, 전체 요청 10초 제한 |
| 점수 행 | 총 100000, 초과 시 새 신원은 `CAPACITY` |

**예제의 선택** (한계 아님): 상태 발행 30 Hz, 시뮬레이션 60 Hz 고정, 라운드 3~300초.

**알려진 한계**: 지연 은닉 없음(예측·롤백 없음), 게스트 입력 반영은 호스트 틱 + 왕복 지연만큼 늦음, 치팅 방지 없음,
자동 재접속 없음, WinHTTP는 전체 작업 시간 상한을 최선 노력으로만 보장, Windows에서 예제의 `Sleep` 기반 대기는
타이머 해상도에 따라 거칠 수 있으나 기한 기반으로 따라잡아 평균 60 Hz를 유지합니다.

## 13. 오류 코드와 문제 해결

`pixelica_online_status_string(status)`가 사람이 읽는 문자열을 줍니다.

| 값 | 이름 | 의미와 대처 |
| --- | --- | --- |
| 0 | `PIXELICA_ONLINE_OK` | 정상 |
| 1 | `BAD_REQUEST` | 잘못된 요청(이름·보드 규칙 위반, 같은 시퀀스에 다른 데이터 등) |
| 2 | `NOT_FOUND` | 방 코드가 없거나 `game_id`가 다름, 알 수 없는 세션 |
| 3 | `FULL` | 방이 가득 참 |
| 4 | `UNAUTHORIZED` | 세션이 폐기·만료됨 → `destroy` 후 다시 `join` |
| 5 | `NOT_HOST` | 호스트가 아닌데 `pixelica_online_state` 호출 |
| 6 | `CAPACITY` | 서버의 방/세션/점수 행 한도 |
| 7 | `RATE_LIMIT` | 너무 자주 호출 → 간격을 두고 재시도 |
| 8 | `ROOM_CLOSED` | 호스트가 나갔거나 방이 만료 → 새로 참가 |
| 9 | `INTERNAL` | 서버 내부 오류 |
| 100 | `TRANSPORT` | 네트워크, TLS, 타임아웃 (일시 오류는 자동 재시도) |
| 101 | `PROTOCOL` | 응답이 이상함. 서버 버전/주소(프록시)를 확인 |
| 102 | `NO_MEMORY` | 메모리 부족 |
| 103 | `INVALID_ARG` | 인자 오류(예: 잘못된 URL, 이름 길이, 키 형식) |
| 104 | `UNSUPPORTED` | 지원하지 않는 설정 |

자주 겪는 문제:

* **`cannot create the room: …TRANSPORT`**: 릴레이가 안 떠 있거나 주소/포트가 틀림. `curl <server>/health`로 확인.
* **HTTPS인데 `TRANSPORT`**: 인증서/호스트 이름이 맞지 않음(자체 서명 포함). 검증을 끄는 방법은 없습니다.
* **`INVALID_ARG`로 시작이 안 됨**: `http://` 원격 주소(`allow_insecure_lan` 없이), URL에 쿼리·자격 증명 포함, 이름/게임 id 규칙 위반.
* **게스트가 `NOT_FOUND`**: 방 코드 오타, 다른 `--game`, 또는 호스트가 이미 나가 방이 닫힘.
* **`DISCONNECTED` 배너**: 릴레이에 닿지 않는 동안 표시. 일시 오류면 복구되고, `ROOM_CLOSED`/`UNAUTHORIZED`면 다시 참가해야 합니다.
* **보드에 내 점수가 안 보임**: 같은 신원의 *이전 최고 점수가 더 높음*(엄격히 높을 때만 갱신), 또는 상위 10위 밖.
  `--key-file` 없이 실행할 때마다 새 신원이 생기는 점도 확인하세요.
* **Linux 링크 오류(curl)**: libcurl 개발 패키지와 Threads가 필요합니다.

## 14. 스레드·수명·재접속 규칙

* 모든 `pixelica_online_*` 호출은 생성한 스레드에서만. 내부 워커는 사용자에게 보이지 않습니다.
* 기다리는 호출은 `pixelica_online_host`, `pixelica_online_join`(요청 1회)과 `pixelica_online_destroy`(진행 중
  요청 + 떠나기 요청 1회) 뿐입니다. 나머지는 짧은 잠금 아래 메모리 복사입니다. 그래서 게임 루프에서 안전합니다.
* Linux는 요청 전체에 `timeout_ms`를 적용합니다. Windows WinHTTP는 단계별 타임아웃이므로 연결·종료가 여러 단계의 합만큼 걸릴 수 있습니다.
* `pixelica_online_room_code`, `pixelica_online_player_key`가 돌려준 포인터는 `destroy` 전까지 유효하고 바뀌지 않습니다.
* 자동 재접속은 없습니다. 연결이 치명적으로 끝나면 `destroy` → 방 코드로 `join`을 직접 호출하세요(호스트가 사라진 방은 못 붙음).
* 프로그램 종료 전에 `destroy`를 부르세요. 호출하지 못하면(강제 종료) 방은 서버 TTL로 정리됩니다.

## 15. 검증 상태와 미완 항목

실행한 검증은 `ONLINE_AUDIT.md` (local-only record)에 기록했습니다. Windows/Linux의 엄격한 컴파일,
8인 협동·대전, 잘못된 응답 거부, 서버 62개 시험, 메모리 검사, 복사한 SDK/템플릿과 점수 실패 재시도를 포함합니다.
검증 수치와 소스 스냅샷은 `docs/verification/online-results.json`, `online-source.sha256`에 보관합니다.

공개 인터넷에 서버를 배포하거나 실제 여러 기기에서 플레이하지는 않았습니다. Caddy 설정은 실제 바이너리로 검증했으나
Docker 이미지 빌드·공인 인증서 발급은 실행하지 않았습니다. ESP32-P4용 온라인 전송 계층은 구현하지 않았습니다.
캐주얼 HTTP 최신 값 교환이며 롤백·UDP·예측·자동 재참가·호스트 이전·치팅 방지가 없습니다.
강제 종료 후 마지막 입력은 기본 세션 만료 30초까지 남을 수 있습니다. 위험한 연속 동작은 게임에서 별도 시간 제한을 두세요.

Windows 타임아웃은 단계별이므로 연결/종료가 설정 시간보다 길어질 수 있습니다. 그 외 게임 프레임의 API는
짧은 잠금 아래 복사만 수행합니다. 점수 API의 성공 반환은 대기열 접수이며, 저장 성공은 `generation`과 `status`로 확인합니다.
hello_online은 실제 저장 실패를 확인한 뒤 최대 5회 재시도합니다. 독립 Charge 템플릿은 저장 실패를 종료 코드 1로 알립니다.
