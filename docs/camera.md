# 카메라 헬퍼

`pixelica/camera.h` — 렌더러와 무관한 작은 카메라 계산 함수 모음입니다(공통 라이브러리).
힙·전역 상태·전역 난수가 없고 모든 값은 호출자 소유입니다. 카메라 "프레임워크"가
아니라, 매 프레임 호출하는 순수 계산 함수입니다.

| 함수 | 용도 |
| --- | --- |
| `pixelica_camera_follow_vec3` | 임의의 Vec3 위치를 목표로 부드럽게 이동 (3D 카메라 eye/target 등) |
| `pixelica_camera2d_follow` | 2D 카메라가 목표를 화면 중앙에 두도록 추적, 선택적 경계 |
| `pixelica_camera2d_world_to_screen` / `screen_to_world` | 줌을 고려한 좌표 변환 |
| `pixelica_camera_look_at` | 검사되는 3D look-at 뷰 행렬 |
| `pixelica_camera_shake_start` / `_update` | 결정적(재현 가능한) 화면 흔들림 |

## 규약

**2D** (2D 렌더러와 동일):

```
screen = (world - camera.position) * camera.zoom      // 화면 (0,0)=좌상단, +Y 아래
```

`camera.position`은 **뷰포트 원점(화면 좌상단에 보이는 월드 좌표)** 이며, 화면
중앙이 아닙니다. 보이는 월드 영역은 `position .. position + viewport / zoom`입니다.
`PIXELICA_Camera2D`는 `PIXELICA2D_Camera`와 같은 멤버(`position`, `zoom`)를 가지므로
필드별로 복사해 `pixelica2d_set_camera()`에 넘기면 됩니다.

**3D** (`math.h`와 동일): 오른손 좌표계, +Y 위, 카메라는 -Z를 바라봄, 열 우선(column-major)
`float[16]`, 열 벡터. `clip = P * V * M * p`의 `V`로 사용합니다.

**스무딩**: `half_life`(초) 기반 지수 감쇠입니다. `half_life`초마다 남은 거리의 절반을
이동하며, dt를 어떻게 나눠도(0.5초 두 번 = 1초 한 번) 결과가 같습니다(반올림 오차 제외).
`half_life == 0`이면 즉시 이동(snap), 그 외에는 `dt == 0`이면 그대로입니다. 권장값 0.05(빠름)
~ 0.3(느슨함). 결과는 항상 현재 위치와 목표 사이에 있어 오버슈트·오버플로가 없습니다.

**오류**: 모든 함수는 입력을 전부 검사한 뒤에만 씁니다. `PIXELICA_OK`가 아니면 출력과
입·출력 인자는 그대로입니다. NULL, NaN/Inf, 범위 밖 값(zoom ≤ 0, viewport ≤ 0, dt < 0,
min > max 경계), 퇴화 구성, 유한 float로 표현할 수 없는 결과는 모두
`PIXELICA_ERR_INVALID_ARG`입니다. 중간 계산은 double입니다.

## 2D 예제

```c
#include "pixelica/camera.h"
#include "pixelica/renderer2d.h"

static PIXELICA_Camera2D g_cam = {{0.0f, 0.0f}, 2.0f};   /* 줌 2 */
static PIXELICA_CameraShake g_shake;                       /* 0 초기화 = 정지 */

void on_player_hit(void)
{
    /* 진폭 4 월드 단위, 0.3초, 초당 30 샘플, 시드 고정 → 리플레이에서도 동일 */
    pixelica_camera_shake_start(&g_shake, 4.0f, 0.3f, 30.0f, 1234u);
}

void update_and_draw(PIXELICA2D_Context *ctx, PIXELICA_Vec2 player, float dt)
{
    const PIXELICA_Vec2 viewport = {320.0f, 240.0f};          /* 화면 픽셀 크기 */
    const PIXELICA_CameraBounds2D map = {{0.0f, 0.0f}, {2048.0f, 1024.0f}};
    PIXELICA_Vec2 shake;
    PIXELICA2D_Camera draw_cam;

    /* 플레이어를 화면 중앙에, 맵 밖은 보이지 않게 (줌 반영) */
    pixelica_camera2d_follow(&g_cam, viewport, player, 0.15f, dt, &map);
    pixelica_camera_shake_update(&g_shake, dt, &shake);

    /* 흔들림은 그리기용 복사본에만 더합니다 (추적 상태 오염 방지) */
    draw_cam.position.x = g_cam.position.x + shake.x;
    draw_cam.position.y = g_cam.position.y + shake.y;
    draw_cam.zoom = g_cam.zoom;
    pixelica2d_set_camera(ctx, &draw_cam);
}

/* 마우스/터치 좌표(화면 픽셀) → 월드 좌표 */
PIXELICA_Vec2 pick(PIXELICA_Vec2 mouse)
{
    PIXELICA_Vec2 world = {0.0f, 0.0f};
    pixelica_camera2d_screen_to_world(&g_cam, mouse, &world);
    return world;
}
```

경계 동작:

- 목표 위치와 최종 위치 모두 `[min, max - viewport/zoom]`으로 제한됩니다. 줌을 줄여
  기존 위치가 경계를 벗어나도 다음 호출에서 바로 안으로 들어옵니다.
- 어떤 축에서 경계가 보이는 영역보다 작으면 그 축은 경계의 중앙에 고정됩니다.
- 경계는 `min <= max`이면 폭 0도 허용합니다.

## 3D 예제

```c
#include "pixelica/camera.h"
#include "pixelica/pixelica.h"

static PIXELICA_Vec3 g_eye = {0.0f, 3.0f, 8.0f};
static PIXELICA_Vec3 g_look = {0.0f, 0.0f, 0.0f};

void update_camera(PIXELICA_Vec3 player, float dt, float view[16])
{
    PIXELICA_Vec3 want_eye = {player.x, player.y + 3.0f, player.z + 8.0f};
    const PIXELICA_Vec3 up = {0.0f, 1.0f, 0.0f};

    pixelica_camera_follow_vec3(&g_eye, want_eye, 0.25f, dt);  /* 위치는 느슨하게 */
    pixelica_camera_follow_vec3(&g_look, player, 0.08f, dt);   /* 시선은 빠르게 */

    if (pixelica_camera_look_at(view, g_eye, g_look, up) != PIXELICA_OK) {
        /* eye == target 이거나 up이 시선과 평행: view는 이전 값 그대로 */
    }
}
```

`pixelica_camera_look_at`은 다음을 거부합니다: `eye == target`, `up`이 0 벡터,
`up`이 시선 방향과 평행/반평행(사인 ≤ 1e-6), NaN/Inf, 그리고 이동 성분이 float 범위를
넘는 경우. 바로 위/아래를 볼 수 있다면 그때만 `up`을 `(0,0,-1)` 등으로 바꿔 주세요.
3D 흔들림은 `shake_update`의 오프셋을 eye와 target에 같은 값으로 더하면 됩니다.

## 화면 흔들림

- `PIXELICA_CameraShake`를 0으로 초기화하면 정지 상태이며 오프셋은 `(0,0)`입니다.
- 오프셋은 `(seed, elapsed)`만의 순수 함수입니다. 같은 시드와 같은 경과 시간이면 dt
  분할과 무관하게 같은 값이며 전역 RNG를 쓰지 않습니다. 다만 float 시간 누적의 반올림 때문에
  서로 다른 dt 분할에서 경과 시간이 미세하게 달라질 수 있으므로 리플레이에는 같은 시간 스텝을 사용하세요.
- 축마다 `[-amplitude, amplitude]` 범위의 부드러운 값 노이즈에 `(1 - elapsed/duration)²`
  감쇠를 곱하므로 끝에서 정확히 `(0,0)`이 됩니다. `elapsed`는 `duration`에서 멈춥니다.
- 진폭 단위는 월드 단위입니다. 픽셀 단위로 흔들려면 `amplitude / zoom`을 넘기세요.
- 손상된 상태(`elapsed > duration`, 활성 상태인데 `frequency <= 0` 등)는 거부됩니다.
