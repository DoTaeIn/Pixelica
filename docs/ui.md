# 내장 모듈: UI (Pixelica::ui)

> 메뉴·HUD·설정 화면에 쓰는 **유지형(retained) 위젯 UI**입니다. 버튼, 체크박스, 슬라이더, 진행 막대, 라벨, 패널을 RGB565 버퍼에 직접 그립니다.
> 2D·3D 렌더러, SDL, GPU, 힙 할당, 전역 가변 상태에 의존하지 않습니다. 위젯 배열과 텍스트는 모두 **호출자 소유**입니다.
> UI는 선택 모듈이 **아닙니다**. 두 SDK(2D·3D)에 항상 포함되고, 빌드 옵션도 없습니다.

| 항목 | 값 |
|---|---|
| 옵션 | 없음 (항상 빌드, 항상 SDK에 포함) |
| 라이브러리 | `Pixelica::ui` (정적) — 헤더 [`pixelica/ui.h`](../modules/ui/include/pixelica/ui.h) |
| 의존성 | common(`pixelica/status.h`, `pixelica/platform.h`)만 |
| 구현 | `ui.c`(상태·입력·레이아웃), `ui_rgb565.c`(렌더러), `ui_font.c`(내장 5×7 글꼴), `ui_presenter.c`(PAL 래퍼) |
| 테스트 | `test_ui`(상태·입력·레이아웃), `test_ui_render`(픽셀·클리핑·글꼴·프레젠터), `test_ui_integration`(2D/3D 연결), `sdk_installed_ui`(설치) |
| 언어 | C11, 힙 사용 없음 |

## 1. CMake에서 사용하기

SDK를 설치하면 UI 패키지가 함께 들어 있습니다. 별도 옵션이나 추가 ZIP이 필요 없습니다.

```cmake
find_package(PixelicaUI CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE Pixelica::ui)
```

`Pixelica::ui`는 common만 필요로 하므로 2D SDK(`Pixelica2D`)만 설치한 프로젝트에서도, 3D SDK에서도 같은 방식으로 쓸 수 있습니다.
소스로 포함한 프로젝트에서는 `add_subdirectory(external/Pixelica)` 뒤에 같은 타깃을 링크합니다.
`find_package(Pixelica COMPONENTS ui)`가 아니라 독립 패키지 `PixelicaUI`로 찾습니다.

## 2. 최소 흐름

```c
#include "pixelica/ui.h"

static PIXELICA_UI_Widget widgets[1];
static PIXELICA_UI_Context ui;
static uint16_t pixels[320 * 180];

PIXELICA_UI_Rect r = { 20, 20, 120, 24 };
if (pixelica_ui_widget_default(PIXELICA_UI_BUTTON, 1, r, "START", 5, &widgets[0]) != PIXELICA_OK) { /* 오류 처리 */ }
if (pixelica_ui_init(&ui, widgets, 1) != PIXELICA_OK) { /* 오류 처리 */ }

/* 게임 프레임마다 정확히 한 번 */
PIXELICA_UI_Input in = pixelica_ui_input_buttons(buttons);   /* 방향키/A/START → UI 동작 */
in.pointer_x = mouse_x; in.pointer_y = mouse_y;
in.pointer_valid = true; in.pointer_down = mouse_down;
pixelica_ui_update(&ui, &in);
if (pixelica_ui_event(&ui, 1).clicked) { /* 눌림 처리 */ }

PIXELICA_UI_Surface s = { pixels, 320, 180, 320 };
pixelica_ui_render_rgb565(&ui, &s);
```

`pixelica_ui_update`는 **게임 프레임당 한 번**만 호출하세요. 고정 틱 루프에서 밀린 틱마다 여러 번 호출하는 "따라잡기 업데이트"는 하지 마세요.
이벤트(`clicked`, `changed`)는 한 번의 update 결과이고 다음 성공한 update 또는 `set_widgets`까지 유지됩니다. 틱마다 호출하면 클릭을 놓치거나 중복 처리하기 쉽습니다.

## 3. 위젯과 데이터 소유

- `PIXELICA_UI_Widget` 배열(최대 `PIXELICA_UI_MAX_WIDGETS` = 128개)과 각 위젯의 `text`는 **호출자 소유**이며 UI는 빌려 쓰기만 합니다. 수명은 호출자가 보장합니다.
- 배열 개수 0은 `widgets == NULL`일 때만 허용됩니다. `id`는 0이 아니어야 하고 컨텍스트 안에서 유일해야 합니다.
- 사각형 `rect`의 `width`, `height`는 0 이상이고 `x`, `y`는 아무 `int32_t`입니다. 끝점은 내부에서 `int64`로 계산하므로 오버플로가 없습니다. 면적 0인 위젯은 포커스·상호작용을 받지 못합니다.
- `text`는 **엄격한 UTF-8**, 길이 0..4096바이트(`PIXELICA_UI_MAX_TEXT_BYTES`)이고 NUL 종료가 필요 없습니다. `NULL`은 길이 0일 때만 가능합니다. 과다 인코딩(overlong), 서로게이트, U+10FFFF 초과, 떠돌이 연속 바이트, 잘린 시퀀스는 거부합니다.
- **유지형 항목의 문구를 바꾸려면** 프레임 사이에 호출자가 `text`/`text_bytes`(또는 `value`, `checked`, `visible`, `enabled`)를 직접 편집하면 됩니다. 매 연산(update·render)이 전체를 다시 검증하므로 편집이 곧 반영되고, 잘못된 편집은 `PIXELICA_ERR_INVALID_ARG`로 잡힙니다. 문구 버퍼는 렌더 중에도 바뀌면 안 됩니다.

| 종류 | 상호작용 | 그리기 |
|---|---|---|
| `PANEL` | 없음(입력 투명) | `theme.panel` 배경 + `theme.border` 테두리, 텍스트는 좌상단(제목) |
| `LABEL` | 없음(입력 투명) | 배경 없이 텍스트만, 좌상단 + 패딩 |
| `BUTTON` | 클릭 | 채움(기본/hover/pressed), 포커스 시 `accent` 테두리, 텍스트 가운데 정렬 |
| `CHECKBOX` | 토글 | 정사각형 상자 + 체크(`accent`) + 오른쪽 라벨 |
| `SLIDER` | 드래그·좌우 키 | 위: 라벨, 아래: 트랙 + 썸. 히트 영역은 위젯 전체 |
| `PROGRESS` | 없음(입력 투명) | 배경 + 테두리 + `active` 막대(`value` 0..1) + 가운데 텍스트 |

모달 창이나 "입력 투명" 해제 기능은 없습니다. 장식 위젯(`PANEL`, `LABEL`, `PROGRESS`)의 글자색은 항상 `theme.text`입니다. 상호작용 위젯이 `enabled == false`이면 `theme.disabled_text`를 씁니다.

## 4. 렌더링 규칙

모든 그리기는 불투명이며 카메라·깊이·통계를 건드리지 않습니다. 위젯은 **리스트 순서대로** 그려지고 뒤쪽이 위에 놓입니다. `visible == false`는 그리지 않습니다.

- 위젯 사각형과 서피스에 클리핑합니다. 화면 밖이나 `INT32` 극단값 좌표도 안전합니다.
- 글자 칸 너비 `(font.width + 1) * scale`, 줄 높이 `(font.height + 1) * scale`. 줄바꿈 `\n`을 지원합니다.
- 패딩은 `theme.padding`(0..16)입니다. 패널·라벨·슬라이더 라벨은 `(x + padding, y + padding)`에서 시작하고, 체크박스 상자는 `x + padding`에서 시작합니다. 버튼·진행 막대 텍스트는 줄마다 가운데 정렬입니다.
- 글자는 위젯 사각형 밖으로 나가지 않습니다(잘림). 완전히 잘려 나간 글리프는 콜백도 부르지 않습니다.
- `update` 전에도 그릴 수 있습니다(이벤트가 모두 중립). 사용할 수 없는 상태(숨김·비활성·면적 0)의 위젯에 남은 오래된 `hovered/pressed/focused`는 그려지지 않습니다.
- 서피스 패딩(stride - width 구간)은 건드리지 않습니다.

### 4.1 테마

`pixelica_ui_theme_default()`는 어두운 픽셀 게임풍 색과 패딩 2, 내장 글꼴을 돌려줍니다. 테마는 컨텍스트의 `theme`을 직접 편집해 바꿉니다. 색은 임의의 RGB565입니다. `padding` 0..16, 글꼴 `width` 1..16, `height` 1..32, `scale` 1..4, `glyph` 비-NULL이 아니면 연산이 거부됩니다.

### 4.2 글꼴과 한글

기본 글꼴은 5×7 비트맵(`examples/common2d/demo_art.h`의 데이터를 복사)으로 **ASCII 32..90**만 있습니다. 소문자는 대문자로 매핑하고, 나머지 모든 코드포인트(비ASCII 유니코드 포함)는 `?`로 그립니다.
**내장 한글 글꼴은 없고 TTF/OTF 래스터라이저도 없습니다.** 한글은 사용자 콜백으로 공급합니다.
`tools/pixelica_font.py`로 BDF에서 게임에 필요한 글자만 추출해 이 콜백을 자동 생성할 수 있습니다.
번역표·언어 전환·폰트 생성 예제는 [i18n.md](i18n.md)를 참고하세요.

```c
typedef bool (*PIXELICA_UI_GlyphFn)(void *userdata, uint32_t codepoint, uint32_t rows[32]);
```

- 콜백에는 **0으로 지운 32행**이 전달됩니다. 각 행의 비트 31이 왼쪽 첫 픽셀입니다(`rows[0] = 0x80000000u`이면 왼쪽 위 한 점). 고정 칸이며 `width`(1..16) 비트까지만 사용되고 그 오른쪽 비트와 `height` 이후의 행은 무시됩니다.
- `true`는 비트맵 사용, `false`는 칸 가장자리를 두른 **missing-glyph 상자**를 그립니다.
- 한글 16×16 글꼴 예: `font.width = 16; font.height = 16; font.glyph = my_hangul; font.userdata = &my_font_data;`. `"한"`(U+D55C) 같은 코드포인트로 자기 비트맵 표를 찾으면 됩니다.
- 콜백은 UI·출력 버퍼·빌린 텍스트를 바꾸면 안 되고, 다시 그리기를 호출(재귀)하면 안 되며, `rows`를 보관해서도 안 됩니다. `userdata`는 호출자가 수명을 보장합니다.
- 조합형 한글, 글자 폭이 다른 문자, 양방향·결합 문자, 자동 줄바꿈, 줄 간격 조절은 지원하지 않습니다(칸 고정, `\n`만 줄바꿈).

## 5. 입력 매핑과 포커스

`pixelica_ui_input_buttons(logical_buttons)`는 논리 버튼을 UI 동작으로 바꿉니다: UP/DOWN → `PIXELICA_UI_PREVIOUS/NEXT`, LEFT/RIGHT → `PIXELICA_UI_LEFT/RIGHT`, A 또는 START → `PIXELICA_UI_ACCEPT`. **QUIT는 CANCEL이 아닙니다.** 포인터는 처음에 무효(`pointer_valid = false`)이므로 마우스·터치는 프레임버퍼 좌표를 직접 채웁니다. `buttons`는 `PIXELICA_UI_BUTTONS` 범위 밖 비트를 가지면 거부됩니다.

| 항목 | 규칙 |
|---|---|
| 상호작용 가능 조건 | `BUTTON/CHECKBOX/SLIDER` + `visible` + `enabled` + 면적 > 0 |
| 포커스 이동 | PREVIOUS/NEXT **상승 에지**로 가능한 위젯을 순환(wrap). 포인터 캡처 중에는 무시 |
| 포커스 초기값 | `init`/`set_widgets`는 첫 번째 가능한 위젯에 포커스 |
| 호버 | 키보드 포커스를 뺏지 않음 |
| 누름 | 가장 위(리스트 역순) 히트 위젯이 캡처와 포커스를 얻음 |
| 버튼 | 캡처한 위젯 위에서 놓으면 `clicked` (놓는 순간에도 가장 위 히트여야 함) |
| 체크박스 | 같은 조건으로 놓으면 토글(`checked` 변경, `changed`/`clicked`) |
| 슬라이더(포인터) | 누름·드래그·놓기 모두 화면 어디서나 캡처 유지. 가로 위치를 위젯 폭에 비례해 min..max로 대응(폭 1이면 min), 스냅 없음 |
| 슬라이더(키) | 포커스 중 LEFT/RIGHT 상승 에지마다 `step`만큼, 자동 반복 없음. 범위를 넘지 않게 clamp |
| ACCEPT | 상승 에지로 포커스된 버튼 클릭 또는 체크박스 토글(한 번). 슬라이더는 해당 없음. 누르고 있으면 `pressed` 모양 |
| CANCEL | 상승 에지가 캡처를 취소하고 그 update의 키보드 활성화를 막음 |
| 포인터 우선 | 같은 update에서 포인터 누름·놓기·캡처가 있으면 키보드 ACCEPT는 무시(이중 토글 방지) |
| 캡처 취소 | 포인터 상실/무효 좌표, CANCEL, 위젯의 숨김·비활성·삭제 시 `clicked` 없이 취소 |
| 포커스 복구 | 숨김·비활성으로 포커스가 사라지면 다음 성공한 update에서 그 뒤의 가능한 위젯으로 순환 복구. 삭제된 ID이면 첫 위젯으로 복구 |
| `wants_pointer` | 포인터가 가장 위 상호작용 위젯 위에 있거나 캡처 중일 때 true |
| `wants_buttons` | 가능한 포커스가 있고 UI 버튼이 하나라도 눌려 있을 때 true. 게임 입력을 막는 데 쓰세요 |

`events[]`(그리고 `pixelica_ui_event(ui, id)`)는 `hovered/focused/pressed/clicked/changed`를 알려 줍니다. `clicked`와 `changed`는 update당 최대 한 번만 서고, 다음 성공한 update에서 초기화됩니다.
`set_widgets`는 메뉴를 바꾸면서 이벤트·캡처·포커스를 초기화하지만 **이전 입력은 보존**합니다. ACCEPT를 누른 채 메뉴를 열어도 새 메뉴가 바로 눌리지 않습니다.

### 5.1 컨트롤러·자작 장치와 InputManager

UI 자체에는 장치 드라이버가 없습니다. 컨트롤러 D-pad·스틱이나 GPIO·USB 장치는 기존 선택
[InputManager](input_manager.md)의 바인딩으로 논리 액션을 만든 뒤 다음처럼 연결합니다.

```c
/* 장치 submit과 manager_tick은 InputManager의 틱 규칙을 따릅니다. */
PIXELICA_UI_Input in = pixelica_ui_input_buttons(pixelica_input_manager_buttons(&manager));
/* 별도 액션 7을 장치의 뒤로 가기 버튼에 바인딩한 경우 */
if (pixelica_input_manager_action(&manager, 7u).held) in.buttons |= PIXELICA_UI_CANCEL;
if (pixelica_ui_update(&ui, &in) == PIXELICA_OK) {
    if (!ui.wants_buttons) { /* 게임 쪽 버튼 처리 */ }
    if (!ui.wants_pointer) { /* 게임 쪽 포인터 처리 */ }
}
```

UI에는 **held 상태**를 전달하세요. `pressed` 펄스를 전달하면 UI의 에지 감지와 엇갈릴 수 있습니다.
InputManager가 없어도 `in.buttons`에 UI 액션 비트를 직접 채우면 됩니다. UI는 한 프레임에 한 번,
장치 최신 상태를 샘플링한 뒤 갱신합니다. 장치 해제·창 포커스 상실 시 버튼을 0으로, 포인터를 무효로 전달합니다.
포인터는 한 개만 지원하며 터치 ID 선택과 화면 배율·레터박스 좌표 변환은 호스트가 담당합니다.
전체 메뉴를 닫는 동작은 CANCEL 상승 에지를 게임에서 처리하세요. UI의 CANCEL은 캡처 취소만 담당합니다.

## 6. 레이아웃 도우미

`pixelica_ui_layout_init(&layout, bounds, gap, horizontal)` 후 `pixelica_ui_layout_next(&layout, extent, &rect)`로 한 칸씩 받습니다. `bounds.width/height`와 `gap`은 0 이상, `extent`는 양수입니다. 좌표는 음수도 허용하고 `int64`로 계산해 결과 위치가 `int32`에 맞아야 합니다. 남은 공간이 부족하면 `PIXELICA_ERR_CAPACITY`이고 어떤 값도 바뀌지 않습니다. `layout`과 `out`이 겹치면 거부됩니다.

## 7. 메모리와 오류

- 힙 할당·전역 가변 상태·정적 버퍼가 없습니다. 컨텍스트와 위젯 배열은 호출자가 가집니다.
- update·render는 먼저 **전체 컨텍스트와 해당 입력·서피스를 검증**하고, 잘못되면 `PIXELICA_ERR_INVALID_ARG`를 돌려주며 출력(픽셀·이벤트·값·이력)을 **변경하지 않습니다**. `pixelica_ui_event`는 가벼운 조회 함수이며 컨텍스트 구조와 대상 위젯의 상호작용 가능 여부만 확인합니다.
- 주소 범위와 크기 곱셈 오버플로를 검사합니다. 컨텍스트는 위젯 배열과, 텍스트는 컨텍스트·위젯 배열과 겹치면 안 됩니다. 렌더 대상 픽셀은 서피스 디스크립터, 컨텍스트, 위젯 배열, **어떤 위젯의 텍스트와도** 겹치면 안 됩니다. 입력 구조체도 컨텍스트·위젯과 겹치면 안 됩니다.
- 서피스는 가로·세로 1..4096, `stride >= width`입니다. 실제 할당 크기와 정렬, 글리프 콜백의 `userdata` 수명은 **호출자 책임**이며 라이브러리가 검사할 수 없습니다.
- 단일 스레드용입니다.

## 8. 2D: 버퍼에 직접 그리기

2D 게임은 이미 가진 RGB565 버퍼에 장면을 그린 뒤 마지막에 UI를 그립니다. 렌더러 모듈이 필요 없습니다.

```c
/* 프레임: 입력 → update → 장면 그리기 → UI 그리기 → 표시 */
pixelica_ui_update(&ui, &in);
draw_scene(pixels);
PIXELICA_UI_Surface s = { pixels, 320, 180, 320 };
if (pixelica_ui_render_rgb565(&ui, &s) != PIXELICA_OK) { /* 설정 오류 */ }
```

## 9. 3D/하이브리드: PAL 프레젠터

3D 엔진은 프레임버퍼를 직접 내주지 않으므로, 엔진을 만들기 **전에** PAL을 감싸고 `end_frame` 전에 UI를 update합니다.

```c
static uint16_t ui_out[320 * 180];            /* 프레임 크기 이상 */
PIXELICA_UI_Presenter presenter;
pixelica_ui_presenter_init(&presenter, &real_pal, &ui, ui_out, 320 * 180);
PIXELICA_Context *ctx;
pixelica_create(&cfg, &presenter.ops, &ctx);   /* 엔진에는 presenter.ops를 준다 */
/* 프레임마다: pixelica_ui_update(&ui, &in); 장면 그리기; pixelica_end_frame(ctx); */
```

- `present_rgb565`는 엔진 프레임(읽기 전용, `const`를 버리지 않음)을 `output`에 **촘촘한 stride(= width)** 로 복사하고, UI를 그린 뒤 업스트림 `present_rgb565`를 **정확히 한 번** 부릅니다. 업스트림이 `false`를 돌려주면 그대로 전달합니다.
- `time_us`, `read_buttons`, `log`는 원래 `userdata`로 업스트림에 전달됩니다. `log`가 NULL이면 래퍼의 `log`도 NULL입니다.
- init은 업스트림의 `time_us/read_buttons/present_rgb565`, UI 유효성, `output`/`output_capacity`(양수)를 검사하고, `presenter`·`output`·`upstream`·UI·위젯·텍스트가 겹치면 거부합니다. 실패하면 `*out`은 바뀌지 않습니다.
- 매 present마다 원본 크기·stride, `output_capacity >= width * height`, 프레임·출력·프레젠터 분리, UI 전체 유효성을 **복사 전에** 검사합니다. 하나라도 실패하면 `false`를 돌려주고 출력을 건드리지 않으며 업스트림도 부르지 않습니다.
- 프레젠터, UI, 출력 버퍼는 `destroy`까지 같은 주소에 살아 있어야 합니다. 셰이더 모듈에 의존하지 않습니다.

### 9.1 선택 쉐이더와 함께 쓰기

2D에서는 `장면 → 쉐이더 적용 → UI 그리기 → 표시`로 호출하면 메뉴 글자는 선명하게 남습니다.
3D에서는 실제 PAL을 UI로 먼저 감싸고 그 결과를 쉐이더로 감쌉니다.

```c
/* 서로 겹치지 않는 별도 ui_out, effect_out, scratch 버퍼를 사용합니다. */
pixelica_ui_presenter_init(&ui_presenter, &real_pal, &ui, ui_out, width * height);
pixelica_shader_presenter_init(&effect_presenter, &ui_presenter.ops, &effect,
    effect_out, width * height, scratch, width * height);
pixelica_create(&cfg, &effect_presenter.ops, &ctx);
/* 실제 실행 순서: 엔진 → 쉐이더 → UI → 실제 PAL, 표시 호출은 한 번 */
```

각 초기화의 오류를 확인한 뒤 다음 단계를 실행하세요. 두 presenter와 버퍼 모두 엔진보다 오래 살아 있어야 합니다.
UI까지 후처리하려면 감싸는 순서를 반대로 배치합니다. 쉐이더의 선택 설치 방법은 [shaders.md](shaders.md)에 있습니다.

## 10. 예제 빌드·실행

저장소의 HEADLESS 빌드에서도 `hello_ui`는 항상 만들어집니다(예제 ON일 때).

```sh
cmake -S . -B build-ui -DPIXELICA_PLATFORM=HEADLESS
cmake --build build-ui --config Release
./build-ui/hello_ui --smoke
./build-ui/hello_ui --output menu.ppm
```

Visual Studio에서는 `build-ui/Release/hello_ui.exe`를 실행합니다. 출력 PPM은 외부 에셋 없이
UI로 직접 렌더링한 화면입니다. 자동 검사는 버튼의 한 번 클릭, 비활성 위젯 건너뛰기, 체크 토글,
슬라이더 키 조절·포인터 드래그와 두 번 렌더링한 픽셀 해시 일치를 검사합니다.
저장소를 SDL3로 빌드하고 인자 없이 실행하면 대화형 메뉴 창이 열립니다.
방향키/WASD로 이동·조절하고 Space/Enter로 선택하며 마우스로 클릭·드래그합니다. Esc는 예제를 종료합니다.

SDK에 포함된 `share/Pixelica/examples/hello_ui/` 폴더를 다른 곳에 복사해서도 실행할 수 있습니다.

```sh
cmake -S copied-hello-ui -B copied-hello-ui/build -DCMAKE_PREFIX_PATH=/absolute/path/Pixelica-0.1.0
cmake --build copied-hello-ui/build --config Release
ctest --test-dir copied-hello-ui/build -C Release --output-on-failure
```

창을 열려면 위 configure에 `-DPIXELICA_UI_DEMO_SDL3=ON -DSDL3_DIR=/path/to/SDL3/cmake`를 추가합니다.
SDL3는 기본 SDK에 포함되지 않으며 별도로 설치된 SDL3 3.4.18 이상이 필요합니다.
`--smoke`와 `--output`은 SDL 빌드에서도 창을 만들지 않습니다.
SDL의 화면 경로도 창 없이 검사하려면 `--hidden --frames 3`을 사용합니다(dummy video/software renderer).
컨트롤러 드라이버와 터치 드라이버는 이 예제에 포함되지 않습니다.

## 11. API 목록

| 함수 | 역할·실패 |
|---|---|
| `pixelica_ui_font_default`, `pixelica_ui_theme_default` | 기본값을 값으로 반환 |
| `pixelica_ui_widget_default` | 유효한 위젯 기본값. 실패 시 출력 보존 |
| `pixelica_ui_init` | 빌린 배열로 컨텍스트 초기화. 실패 시 컨텍스트 보존 |
| `pixelica_ui_set_widgets` | 메뉴 교체, 이벤트·캡처 초기화, 입력 이력 보존 |
| `pixelica_ui_update` | 프레임 입력 처리. 실패 시 이벤트·값·이력 보존 |
| `pixelica_ui_event` | ID의 이벤트 값 반환. 없는 ID·잘못된 컨텍스트 구조·비활성 대상은 중립 |
| `pixelica_ui_input_buttons` | 기존 논리 버튼에서 UI 입력 생성. 알 수 없는 논리 비트는 무시 |
| `pixelica_ui_layout_init`, `pixelica_ui_layout_next` | 가로·세로 연속 배치. 부족하면 CAPACITY, 잘못된 인자는 INVALID_ARG |
| `pixelica_ui_render_rgb565` | UI 합성. 잘못된 인자는 INVALID_ARG, 픽셀 보존 |
| `pixelica_ui_presenter_init` | PAL 래퍼 초기화. 실패 시 출력 구조체 보존 |

Status를 반환하는 함수는 성공하면 `PIXELICA_OK`입니다. 실제로 할당되지 않은 포인터나 거짓 길이까지
검출하는 메모리 안전 런타임은 아니므로 소유한 버퍼의 실제 크기를 정확히 전달해야 합니다.

## 12. 한계

- 내장 한글 글꼴·TTF·조합형 한글·양방향 텍스트·자동 줄바꿈·텍스트 편집 입력창 없음.
- 위젯 128개, 문자열 4096바이트, 서피스 4096×4096 상한.
- 모달 창, 스크롤 영역, 드롭다운, 애니메이션 없음. 알파 블렌딩 없음(모든 그리기 불투명).
- 포인터 입력은 마우스·터치를 호출자가 프레임버퍼 좌표로 변환해 넣어야 합니다. 키 반복(auto-repeat)과 시간 기반 입력은 없습니다.
- 위젯 배열을 UI와 렌더 사이에서 다른 스레드가 편집하는 것은 지원하지 않습니다.
