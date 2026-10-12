# 디버그 유틸리티 (debug)

프레임 시간(FPS / 프레임 ms)을 모아서 보여 주는 아주 작은 도구입니다.
렌더러·플랫폼·UI에 의존하지 않는 공통 부분(`pixelica/debug.h`)과,
화면에 그리는 선택적 HUD(`pixelica/ui.h`)로 나뉩니다.

- 시간 측정은 **호출자가 직접** 합니다. 한 프레임이 걸린 시간을 마이크로초로 넘겨 주면 됩니다.
- 힙 할당, 전역 상태, OS 타이머 호출이 없습니다. 모든 메모리는 호출자 소유입니다.
- 디버그를 끄고 싶으면 **그냥 호출하지 않으면** 됩니다. 켜고 끄는 설정은 없습니다.

## 가장 쉬운 사용법

```c
#include "pixelica/debug.h"
#include "pixelica/ui.h"

PIXELICA_DebugTiming timing;
PIXELICA_UI_DebugHud hud;          /* 주소가 바뀌지 않는 곳에 두세요 (복사 금지) */

pixelica_debug_timing_init(&timing, 0);      /* 0 = 기본 0.5초 창 */
pixelica_ui_debug_hud_init(&hud, 4, 4);      /* 왼쪽 위 좌표 */

/* 매 프레임 */
uint32_t frame_us = /* 직접 잰 이번 프레임 시간(마이크로초) */;
pixelica_debug_timing_update(&timing, frame_us);
pixelica_ui_debug_hud_update(&hud, &timing, "tris", 4, triangle_count);
pixelica_ui_debug_hud_render_rgb565(&hud, &surface);   /* 마지막에 그리기 */
```

화면에는 다음과 같은 패널이 나옵니다.

```
FPS 60.0 | 16.7 ms | max 20.1 ms
tris: 1234
```

첫 창이 끝나기 전에는 `FPS -- | -- ms` 로 표시됩니다.
측정 항목이 필요 없으면 `pixelica_ui_debug_hud_update(&hud, &timing, NULL, 0, 0)` 로 호출하세요.

## 공통 API (`pixelica/debug.h`)

| 함수 | 설명 |
| --- | --- |
| `pixelica_debug_timing_init(&t, window_us)` | 누적기 초기화. `window_us` 는 `0`(기본 500000) 또는 `1..60000000` |
| `pixelica_debug_timing_update(&t, frame_us)` | 프레임 하나 추가. `0..10000000` |
| `pixelica_debug_format_timing(&t, buf, cap, &len, &truncated)` | `FPS .. \| .. ms \| max .. ms` 문자열 |
| `pixelica_debug_format_metric(name, name_bytes, value, buf, cap, &len, &truncated)` | `이름: 값` 문자열 |

`PIXELICA_DebugTiming` 의 `has_report`, `fps_milli`(FPS×1000), `avg_frame_us`,
`max_frame_us`, `last_frame_us` 는 읽기 전용으로 직접 읽어도 됩니다.
나머지 필드는 내부용입니다. 0으로 채운 구조체는 "초기화 안 됨"으로 거부됩니다.

### 동작 규칙

- 창(window)에 쌓인 시간이 `window_us` 이상이 되면(또는 65535 프레임이 쌓이면) 리포트를 갱신하고 새 창을 시작합니다.
  FPS는 창 전체 프레임 수 ÷ 창 전체 시간, 프레임 ms는 창 평균입니다.
- 프레임 시간 `0` 은 허용됩니다(거친 타이머 대비). 창 전체 시간이 0이면 리포트를 만들지 않아 FPS를 지어내지 않습니다.
- 프레임 시간이 `PIXELICA_DEBUG_MAX_FRAME_US`(10초)를 넘으면 `PIXELICA_ERR_INVALID_ARG` 이며 상태는 바뀌지 않습니다.
  일시정지/브레이크포인트 뒤 첫 프레임은 호출자가 건너뛰거나 잘라서 넘기세요.
- 누적 값은 `uint32_t` 안에서 넘치지 않도록 위 상한으로 보장됩니다. FPS는 `UINT32_MAX` 에서 포화합니다.
- 메트릭 이름은 1..24바이트의 출력 가능한 ASCII(0x20..0x7E)이며 NUL 종료가 필요 없습니다.
  `NULL` 이거나 길이가 0, 25 이상, 제어문자/비ASCII 포함이면 `PIXELICA_ERR_INVALID_ARG` 입니다.
- 서식 함수의 `capacity` 는 NUL 포함 크기이며 1 이상이어야 합니다.
  `PIXELICA_DEBUG_TEXT_BYTES`(96) 면 어떤 줄도 잘리지 않습니다.
  더 작은 버퍼는 끝에서 잘리고 항상 NUL 로 끝나며, `*truncated` 가 `true` 가 될 뿐 상태는 `PIXELICA_OK` 입니다.
- 실패하면 상태, 버퍼, `len`, `truncated` 모두 변경되지 않습니다(트랜잭션).
  버퍼가 입력/출력 포인터와 겹쳐도 `PIXELICA_ERR_INVALID_ARG` 입니다.

## HUD (`pixelica/ui.h`)

| 함수 | 설명 |
| --- | --- |
| `pixelica_ui_debug_hud_init(&hud, x, y)` | 패널 왼쪽 위 좌표 지정. 처음에는 비어 있어 아무것도 그리지 않음 |
| `pixelica_ui_debug_hud_update(&hud, &timing, name, name_bytes, value)` | 문자열을 만들고 패널 크기를 글꼴에 맞춤 |
| `pixelica_ui_debug_hud_render_rgb565(&hud, &surface)` | RGB565 서피스에 그림 |

- 기존 UI 위젯(PANEL)과 UI 렌더러·기본 글꼴을 그대로 사용합니다. 글꼴/래스터라이저를 따로 두지 않습니다.
- 구조체가 UI 컨텍스트와 위젯을 직접 들고 있어서, 매 프레임 큰 컨텍스트를 스택에 만들 필요가 없습니다.
  `update` 가 쓰는 스택은 약 400바이트입니다.
- `hud.ui.theme`(색, 글꼴, 패딩)만 수정할 수 있습니다. 바꾼 뒤 다음 `update` 에서 크기가 다시 계산됩니다.
- 서피스 범위를 벗어난 부분은 잘리고(clip), 줄 끝 패딩(stride - width)과 깊이 버퍼는 건드리지 않습니다.
  좌표가 서피스 밖이어도 오류가 아닙니다.
- `update` 가 실패하면 이전 텍스트가 그대로 남습니다. `render` 가 `PIXELICA_ERR_INVALID_ARG` 면 픽셀은 바뀌지 않습니다.
- 3D/하이브리드에서 프레젠터와 함께 쓰려면 UI 컨텍스트가 필요하므로,
  HUD의 `hud.ui` 를 `pixelica_ui_presenter_init` 에 넘기면 됩니다(`hud` 주소는 고정).

## 제한 사항

- OS 타이머나 프로파일러가 아닙니다. 값이 정확한지는 호출자가 넘긴 시간에 전적으로 달려 있습니다.
  CPU/GPU 구간별 시간, 메모리 사용량, 호출 스택 등은 측정하지 않습니다.
- 표시는 창 평균이므로 짧은 끊김은 `max` 로만 보입니다. 창이 길수록 반응이 느립니다.
- 텍스트는 ASCII 전용(숫자, 영문 이름)입니다. 지역화나 비ASCII 이름은 지원하지 않습니다.
- 메트릭은 이름/정수(`int32_t`) 하나뿐입니다. 더 필요하면 문자열 서식 함수를 직접 쓰거나 UI 위젯을 직접 구성하세요.
- 단일 스레드 전용입니다. `PIXELICA_DebugTiming` 과 HUD 는 스레드 간 공유하지 마세요.
- HUD 구조체는 자기 자신을 가리키므로 복사/이동하면 `PIXELICA_ERR_INVALID_ARG` 로 거부됩니다. 다시 `init` 하세요.
- 새 패키지나 의존성은 없습니다. `debug.h` 는 코어 헤더이고, HUD 는 기존 UI 모듈에 포함됩니다.
