# 게임 수명주기 (`pixelica/app.h`)

초기화 → 일시정지/재개 → 중단(suspend)/재개 → 종료를 모든 플랫폼이 같은 방식으로
표현하도록 만든 작은 상태 기계입니다. 게임 코드에 플랫폼 `#ifdef`가 필요 없습니다.
힙·전역 상태·렌더러·시계·스레드가 없고, `PIXELICA_App`은 호출자 메모리이며 한
스레드에서만 씁니다. **엔진이 스스로 전환을 일으키지 않습니다**: 언제 전환할지는
드라이버(SDL3 루프, ESP-IDF 태스크, 테스트)가 정하고, 이 모듈은 어떤 전환이 합법인지,
콜백 순서가 무엇인지, 체크포인트를 언제 쓰는지를 강제합니다.

## 상태

```
                  pixelica_app_init
                         │
                         ▼
           ┌────────► RUNNING ◄──── resume ──── PAUSED ◄───┐
           │           │  │                        ▲   │   │
           └─ resume ──┘  └───────── pause ────────┘   │   │
                       │                               │   │
                   suspend                          suspend│
                       ▼                               ▼   │
                   SUSPENDED ── resume ──► (직전 상태로: RUNNING 또는 PAUSED)

   살아 있는 어떤 상태든 ── pixelica_app_shutdown ──► STOPPED
```

| 상태 | `pixelica_app_should_update()` | 뜻 |
| --- | --- | --- |
| `PIXELICA_APP_UNINITIALIZED` | false | init 전, 또는 init 실패 후 |
| `PIXELICA_APP_RUNNING` | **true** | 갱신 중 |
| `PIXELICA_APP_PAUSED` | false | 프로세스는 그대로, 갱신만 멈춤 |
| `PIXELICA_APP_SUSPENDED` | false | 백그라운드. 체크포인트가 기록됨 |
| `PIXELICA_APP_STOPPED` | false | 종료됨. 다시 init 가능 |

### pause/resume 과 suspend/resume 의 차이

| | pause / resume | suspend / resume |
| --- | --- | --- |
| 언제 | 창 포커스 상실, 일시정지 메뉴, Start 버튼 | 호스트가 게임을 백그라운드로 보냄 |
| 프로세스 | 계속 살아 있고 메모리도 그대로 | **예고 없이 종료될 수 있음** |
| 저장 | 하지 않음 (공짜) | 체크포인트를 씁니다 |
| 비용 | 싸고 자주 일어남 | 비쌈 |
| 복귀 | 항상 `RUNNING` | **중단 직전 상태**로 (paused였으면 paused) |

프로세스가 살아남으면 `resume`이 원래 상태로 되돌리고, 죽었다면 다음 실행의 평범한
`pixelica_app_init()`이 체크포인트에서 복원합니다.

엔진은 전환을 **합성하지 않습니다**. "will resign active" 다음 "did enter background"를
보내는 플랫폼은 둘 다 전달하세요(pause 후 suspend). 백그라운드 이벤트만 있는
플랫폼은 `RUNNING`에서 바로 `suspend`를 부르면 됩니다.

**멱등성**: `PAUSED`에서의 pause, `RUNNING`에서의 resume, `SUSPENDED`에서의 suspend는
`PIXELICA_OK`를 돌려주고 콜백을 실행하지 않습니다(플랫폼은 같은 이벤트를 반복해서 보냅니다).

## 체크포인트는 게임이 직렬화합니다

체크포인트는 **프로세스 스냅샷이 아닙니다**. `save_checkpoint` 콜백이 게임의 필드를
고정 순서·고정 폭으로 직접 써 넣고([`save.md`](save.md)), 그 바이트가
[`storage.md`](storage.md)를 통해 버전·CRC-32 봉투에 담깁니다. 돌아올 때는
`load_checkpoint`가 저장 당시의 schema 버전과 바이트를 받아 게임 상태를 **직접**
재구성합니다. 엔진은 payload를 들여다보거나 변환하지 않고, 엔진 객체(컨텍스트, 화면
스택, 텍스처, 포인터)는 어떤 것도 저장되지 않습니다.

## 콜백 순서

```
init      [복원: storage read + load_checkpoint] → on_init      → RUNNING
pause     on_pause                                               → PAUSED
resume    on_resume                      → RUNNING / PAUSED / 변화 없음
suspend   [체크포인트] → on_suspend                              → SUSPENDED
shutdown  [체크포인트] → on_shutdown                             → STOPPED
```

실패 상태를 돌려준 수명주기 콜백은 **init만** 중단시킵니다(앱은 UNINITIALIZED로
남습니다). pause/resume/suspend/shutdown은 이미 전환이 일어난 뒤에 그 상태를
호출자에게 보고할 뿐입니다.

### 엔진 규칙의 유일한 예외

`pixelica_app_suspend()`와 `pixelica_app_shutdown()`은 **체크포인트가 실패해도 전환합니다**.
호스트가 게임에게 거부할 기회를 주지 않기 때문입니다. 두 함수는 체크포인트의 상태를
반환하므로(= `pixelica_app_checkpoint_status()`), 어디에 있는지는 반환값이 아니라
`pixelica_app_state()`로 확인하세요. 다른 모든 API는 평소대로 "실패하면 아무것도 바꾸지 않음"입니다.

## 복원 실패는 init 실패가 아닙니다

슬롯이 비었거나(`NOT_FOUND`), 손상됐거나(`BAD_ASSET`), `load_checkpoint`가 거부하면
앱은 그냥 `RUNNING`으로 시작하고 게임은 기본값을 씁니다.
`pixelica_app_restored()`가 `false`, `pixelica_app_restore_status()`가 이유입니다.

## 재진입

콜백 안에서 init/pause/resume/suspend/shutdown/checkpoint를 호출하면
`PIXELICA_ERR_INVALID_ARG`가 나고 아무 일도 일어나지 않습니다. 게터는 언제나 허용됩니다.

## API

```c
PIXELICA_Status  pixelica_app_init(PIXELICA_App *app, const PIXELICA_AppConfig *config);
PIXELICA_Status  pixelica_app_pause(PIXELICA_App *app);
PIXELICA_Status  pixelica_app_resume(PIXELICA_App *app);
PIXELICA_Status  pixelica_app_suspend(PIXELICA_App *app);
PIXELICA_Status  pixelica_app_shutdown(PIXELICA_App *app);
PIXELICA_Status  pixelica_app_checkpoint(PIXELICA_App *app);   /* 자동 저장 */
PIXELICA_Status  pixelica_app_restore(PIXELICA_App *app);      /* "불러오기" 메뉴 */
PIXELICA_AppState pixelica_app_state(const PIXELICA_App *app);
bool             pixelica_app_should_update(const PIXELICA_App *app);
bool             pixelica_app_restored(const PIXELICA_App *app);
PIXELICA_Status  pixelica_app_restore_status(const PIXELICA_App *app);
PIXELICA_Status  pixelica_app_checkpoint_status(const PIXELICA_App *app);
PIXELICA_Status  pixelica_app_stats(const PIXELICA_App *app, PIXELICA_AppStats *out);
const char      *pixelica_app_state_name(PIXELICA_AppState state);
```

`PIXELICA_AppConfig`:

| 필드 | 뜻 |
| --- | --- |
| `callbacks` | 아래 표. 전부 선택이지만 체크포인트 옵션을 켜면 대응 콜백이 필요 |
| `storage` | 체크포인트가 갈 곳. `NULL`이면 체크포인트 기능이 꺼짐 |
| `checkpoint_slot` | `< PIXELICA_STORAGE_SLOT_MAX` |
| `scratch` / `scratch_size` | 게임 직렬화 버퍼(앱 전용, init~shutdown) |
| `restore_on_init` | init에서 슬롯을 읽을지 |
| `checkpoint_on_suspend` | suspend에서 저장할지 |
| `checkpoint_on_shutdown` | shutdown에서 저장할지 |

| 콜백 | 서명 |
| --- | --- |
| `on_init` / `on_pause` / `on_resume` / `on_suspend` / `on_shutdown` | `PIXELICA_Status (*)(void *userdata)` |
| `save_checkpoint` | `PIXELICA_Status (*)(void *ud, void *out, size_t capacity, size_t *out_size, uint32_t *out_schema_version)` |
| `load_checkpoint` | `PIXELICA_Status (*)(void *ud, uint32_t schema_version, const void *data, size_t size)` |

`pixelica_app_init`이 `INVALID_ARG`로 거부하는 설정: NULL 인자, 이미 살아 있는 앱,
`checkpoint_slot` 범위 초과, `scratch == NULL`인데 `scratch_size != 0`,
`storage` 없이 켠 체크포인트 옵션, `load_checkpoint` 없는 `restore_on_init`,
`save_checkpoint` 없는 체크포인트 옵션.

## 수명 (모두 빌림, 앱은 아무것도 소유·해제하지 않음)

- `PIXELICA_App`: 호출자 소유, init부터 shutdown까지 주소 고정. 첫 init 전에 0으로 초기화하세요(`PIXELICA_App app = {0};`).
- `PIXELICA_AppConfig`는 값으로 복사되므로 호출보다 오래 살 필요가 없습니다.
- `callbacks.userdata`는 `on_shutdown`이 반환할 때까지 유효해야 합니다.
- `config.storage`는 shutdown까지 초기화된 상태로 유지되어야 하고 다른 곳에서 건드리면 안 됩니다.
- `config.scratch`는 init~shutdown 동안 앱 전용이며, 호출 사이의 내용은 정의되지 않습니다.
- `shutdown`은 빌린 포인터를 버리지만 카운터(`pixelica_app_stats`)와 복원·체크포인트
  상태는 마지막 로그 한 줄을 위해 읽을 수 있게 남깁니다.

## 드라이버 쪽 모양

```c
/* 조각(fragment): 단독 컴파일 불가 */
while (!quit) {
    switch (platform_poll_event()) {
    case EVENT_FOCUS_LOST:   (void)pixelica_app_pause(&app);    break;
    case EVENT_FOCUS_GAINED: (void)pixelica_app_resume(&app);   break;
    case EVENT_BACKGROUND:   (void)pixelica_app_suspend(&app);  break; /* 체크포인트 */
    case EVENT_FOREGROUND:   (void)pixelica_app_resume(&app);   break;
    case EVENT_QUIT:         quit = true;                       break;
    default: break;
    }
    if (pixelica_app_should_update(&app)) {
        game_update(&game, 1.0f / 60.0f);   /* 또는 pixelica_screens_update */
        render_frame(&game);
    }
}
(void)pixelica_app_shutdown(&app);          /* 마지막 체크포인트 */
```

[`screens.md`](screens.md)의 화면 스택과는 독립입니다. 화면 스택은 "게임 안에서
어떤 화면이 위에 있는가"를, 이 모듈은 "게임 자체가 돌고 있는가"를 다룹니다. 일시정지
메뉴를 push 하면서 동시에 `pixelica_app_pause()`를 부를 필요는 없습니다.

## 실행 가능한 예제와 검증

`examples/hello_lifecycle/main.c`는 렌더러·창·플랫폼 테이블 없이 이 모듈과
[`storage.md`](storage.md)를 함께 쓰는 완전한 프로그램입니다. 한 번 실행할 때마다
한 줄을 출력하고, `--exit shutdown|suspend|kill`로 종료 방식을 고릅니다.

```sh
./build/hello_lifecycle --dir saves --steps 10 --exit suspend
# hello-lifecycle: restored=0 restore=not_found level=1 score=10 runs=1 ... state=suspended checkpoint=ok
./build/hello_lifecycle --dir saves --steps 5 --exit shutdown
# hello-lifecycle: restored=1 restore=ok level=1 score=15 runs=2 ... state=stopped checkpoint=ok
```

CTest `lifecycle_checkpoint_restart`(`cmake/PixelicaLifecycleCheck.cmake`)가 **서로 다른
프로세스**로 이 프로그램을 여러 번 돌려 확인하는 것:

1. 빈 디렉터리에서의 첫 실행은 복원하지 않고, suspend가 세이브 파일(12바이트 payload + 20바이트 봉투 = 32바이트)을 만든다
2. 두 번째 프로세스가 그 지점에서 이어서 시작한다
3. 수명주기 호출 없이 죽은 프로세스는 아무것도 저장하지 못한다
4. 그 다음 프로세스가 여전히 이전 체크포인트를 본다
5. 일시정지된 스텝은 진행을 더하지 않는다
6. 파일이 손상되면 `restore=bad_asset`으로 기본값에서 다시 시작한다
7. 적대적인 게임 ID는 디스크를 건드리기 전에 거부된다
8. 쓸 수 없는 저장 루트에서는 체크포인트가 `platform` 오류로 **보고**된다 (숨기지 않음)
9. 다른 슬롯은 같은 게임 디렉터리 안의 다른 파일이다

단위 테스트는 `tests/test_app.c`(모든 전환·재진입·I/O 실패)와
`tests/test_storage.c`(게임 ID·슬롯·봉투·어댑터)입니다.
