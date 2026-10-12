# 화면(Screen) 관리자

`pixelica/screens.h` — 타이틀 / 플레이 / 일시정지 / 결과 같은 **게임 화면 상태**를
관리하는 아주 작은 스택입니다. 3D 장면 객체가 아닙니다. 힙·SDL·렌더러·시계·전역
상태가 없고, 모든 메모리는 호출자 소유입니다(스레드 비안전, 한 관리자는 한 스레드).

## 콜백

각 화면(`PIXELICA_Screen`)은 선택적 콜백 6개(`NULL`이면 아무 일도 안 함)와
`userdata`, `flags`를 가집니다. 콜백은 모두 `void`를 반환합니다.

| 콜백 | 시점 |
| --- | --- |
| `enter` | 화면이 설치될 때 (init / change / push) |
| `leave` | 화면이 제거될 때 (change / pop / shutdown) |
| `pause` | 그 위에 다른 화면이 push 되었을 때 |
| `resume` | 위 화면이 pop 되어 다시 최상단이 되었을 때 |
| `update(dt)` | `pixelica_screens_update()` 당 1회, **최상단 화면만** |
| `draw(target)` | `pixelica_screens_draw()` 당 1회 (아래 그리기 정책) |

호출 순서:

```
init(s)     s.enter
change(n)   top.leave, n.enter            (pause/resume 없음)
push(n)     top.pause, n.enter            (top은 스택에 남음)
pop()       top.leave, 아래.resume
shutdown()  최상단부터 모든 화면의 leave
```

## 정책

- **update**: 최상단 화면만 갱신. 일시정지 오버레이 아래의 `play`는 `resume`
  되기 전까지 갱신되지 않습니다.
- **draw**: 최상단에서 시작해, 현재 화면이 `PIXELICA_SCREEN_TRANSPARENT`이고 아래에
  화면이 있으면 한 칸 내려갑니다. 멈춘 화면부터 최상단까지 **아래→위** 순서로 그립니다.
  플래그가 없는 화면은 불투명이라 그 아래를 가립니다. 즉 일시정지 메뉴에
  `TRANSPARENT`를 주면 `play` 위에 겹쳐 그려지고, `result`는 단독으로 그려집니다.
- **전환 큐**: `change/push/pop`은 어떤 콜백 안에서 호출해도 **즉시 적용되지 않고**
  크기 `PIXELICA_SCREEN_QUEUE_MAX`(4)의 FIFO 큐에 쌓입니다. 적용 시점(안전 경계)은
  `update` 시작·끝, `draw` 끝, `init` 끝, 그리고 콜백 밖에서 호출한 `change/push/pop`
  직후입니다. 경계마다 **그 시점에 쌓여 있던 만큼만(한 배치)** 적용하고, 적용 중
  콜백(예: `enter`)이 새로 요청한 전환은 **다음 경계**로 넘어갑니다. 그래서 서로를
  계속 바꾸는 화면이 있어도 무한 루프나 재귀가 생기지 않습니다.
- 요청은 "큐의 모든 항목이 실행된 뒤의 스택"을 기준으로 미리 검사하므로, 수락된
  요청은 나중에 실패하지 않습니다.

| 상황 | 반환 |
| --- | --- |
| NULL 관리자/화면, 모르는 `flags` 비트, 초기화 안 됨, 이미 shutdown | `INVALID_ARG` |
| 마지막 화면 pop (큐에 쌓인 pop 포함) | `INVALID_ARG` |
| 콜백 안에서 init/update/draw/shutdown 호출 | `INVALID_ARG` |
| shutdown 중(leave 콜백) 전환 요청 | `INVALID_ARG` (남은 큐는 버려짐) |
| `dt`가 NaN/무한대/음수 | `INVALID_ARG` (콜백 실행 없음) |
| 스택이 `PIXELICA_SCREEN_STACK_MAX`(8) 초과가 되는 push | `CAPACITY` |
| 큐가 가득 참 | `CAPACITY` |

실패한 호출은 아무것도 바꾸지 않습니다.

## 수명 (모두 빌림, 관리자는 아무것도 해제하지 않음)

- 관리자: 호출자 소유, init~shutdown 동안 주소 고정. 0으로 초기화하거나 shutdown 한
  뒤에 `init` 하세요. 필드는 내부용입니다.
- `PIXELICA_Screen` 구조체는 **값으로 복사**되므로 호출 뒤 사라져도 됩니다.
- `userdata`는 그 화면의 `leave`가 반환될 때까지 유효해야 합니다.
- `draw`의 `target`은 불투명 포인터(NULL 가능)로 그 호출 동안만 쓰입니다.

## 간단한 게임 루프

```c
#include "pixelica/screens.h"
#include "pixelica/input.h"

typedef struct Game {
    PIXELICA_ScreenManager screens;
    PIXELICA_Screen title, play, pause, result;
    uint32_t buttons;   /* 이번 프레임 입력, 루프가 채움 */
    uint32_t pressed;   /* 이번 프레임에 새로 눌린 버튼 */
    int score;
    bool quit;
} Game;

static void title_update(PIXELICA_ScreenManager *s, void *ud, float dt)
{
    Game *g = ud; (void)dt;
    if (g->pressed & PIXELICA_BTN_START) pixelica_screens_change(s, &g->play);
    if (g->pressed & PIXELICA_BTN_QUIT)  g->quit = true;
}
static void play_update(PIXELICA_ScreenManager *s, void *ud, float dt)
{
    Game *g = ud; (void)dt;
    if (g->pressed & PIXELICA_BTN_START) pixelica_screens_push(s, &g->pause);
    if (g->score >= 10)                  pixelica_screens_change(s, &g->result);
}
static void pause_update(PIXELICA_ScreenManager *s, void *ud, float dt)
{
    Game *g = ud; (void)dt;
    if (g->pressed & PIXELICA_BTN_START) pixelica_screens_pop(s);     /* 재개 */
    if (g->pressed & PIXELICA_BTN_A) {           /* 그만두고 타이틀로 */
        pixelica_screens_pop(s);                 /* pause 제거, play가 최상단이 됨 */
        pixelica_screens_change(s, &g->title);   /* play를 title로 교체 */
    }
}
```

전환은 큐에 쌓인 순서대로 적용되므로 한 콜백에서 `pop` 뒤에 `change`를 이어 요청해도
안전합니다(`pause.leave, play.resume, play.leave, title.enter`).

```c
static void game_setup(Game *g)
{
    memset(g, 0, sizeof *g);
    g->title  = (PIXELICA_Screen){ .update = title_update,  .draw = title_draw,  .userdata = g };
    g->play   = (PIXELICA_Screen){ .update = play_update,   .draw = play_draw,   .userdata = g };
    g->pause  = (PIXELICA_Screen){ .update = pause_update,  .draw = pause_draw,  .userdata = g,
                                   .flags = PIXELICA_SCREEN_TRANSPARENT };
    g->result = (PIXELICA_Screen){ .update = result_update, .draw = result_draw, .userdata = g };
    pixelica_screens_init(&g->screens, &g->title);
}

/* 프레임 루프 (플랫폼이 buttons / dt / framebuffer를 제공) */
uint32_t prev = 0;
while (!g.quit) {
    g.buttons = read_buttons();                    /* PIXELICA_BTN_* 비트마스크 */
    g.pressed = g.buttons & ~prev;  prev = g.buttons;

    pixelica_screens_update(&g.screens, dt);       /* 최상단 화면만 갱신 */
    pixelica_screens_draw(&g.screens, &framebuffer);
    present(&framebuffer);
}
pixelica_screens_shutdown(&g.screens);             /* 모든 leave 호출 */
```

`update`/`draw`가 끝날 때 전환이 적용되므로, 화면을 바꾼 프레임에도 `draw`는
항상 일관된 스택을 그립니다.

## UI 버튼과 화면 전환 연결

`pixelica/ui.h`의 버튼은 `clicked` 이벤트를 주므로, 화면의 `update` 안에서 위젯 id를
전환으로 매핑하면 됩니다. UI 갱신과 화면 갱신이 같은 콜백 안에 있으므로 전환은 큐에
쌓였다가 `update`가 끝날 때 적용됩니다.

```c
enum { ID_START = 1, ID_RESUME, ID_QUIT };

typedef struct TitleUI {
    Game *game;
    PIXELICA_UI_Widget widgets[2];
    PIXELICA_UI_Context ui;
} TitleUI;

static void title_ui_enter(PIXELICA_ScreenManager *s, void *ud)
{
    TitleUI *t = ud; (void)s;
    PIXELICA_UI_Rect r1 = {40, 60, 120, 20}, r2 = {40, 90, 120, 20};
    pixelica_ui_widget_default(PIXELICA_UI_BUTTON, ID_START, r1, "START", 5, &t->widgets[0]);
    pixelica_ui_widget_default(PIXELICA_UI_BUTTON, ID_QUIT,  r2, "QUIT",  4, &t->widgets[1]);
    pixelica_ui_init(&t->ui, t->widgets, 2);
}

static void title_ui_update(PIXELICA_ScreenManager *s, void *ud, float dt)
{
    TitleUI *t = ud; (void)dt;
    PIXELICA_UI_Input in = pixelica_ui_input_buttons(t->game->buttons);
    /* 마우스/터치라면 in.pointer_x/y, pointer_valid, pointer_down 도 채웁니다. */
    if (pixelica_ui_update(&t->ui, &in) != PIXELICA_OK) return;

    if (pixelica_ui_event(&t->ui, ID_START).clicked)
        pixelica_screens_change(s, &t->game->play);   /* 큐에 쌓임 */
    if (pixelica_ui_event(&t->ui, ID_QUIT).clicked)
        t->game->quit = true;
}
```

- `PIXELICA_BTN_UP/DOWN` → UI 이전/다음, `A`/`START` → UI 수락(`clicked`)이 되므로,
  `pixelica_ui_input_buttons()`의 결과를 그대로 `pixelica_ui_update()`에 넘기세요.
- 일시정지 메뉴는 `TRANSPARENT` 화면 + 자체 `PIXELICA_UI_Context`로 만들고
  `ID_RESUME` 클릭 시 `pixelica_screens_pop()`을 호출합니다. 최상단만 `update` 되므로
  게임 화면은 입력을 받지 않습니다.
- 화면 `draw`에서는 `pixelica_ui_render_rgb565(&t->ui, &surface)`를 호출합니다
  (`target`을 `PIXELICA_UI_Surface*`로 해석).
- UI 위젯은 화면 `userdata`(위 `TitleUI`)에 두어 `leave`까지 유효하게 유지하세요.
