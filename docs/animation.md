# Pixelica 애니메이션 모듈

공개 헤더 [`animation.h`](../include/pixelica/animation.h), 구현 [`animation.c`](../src/animation/animation.c),
테스트 [`tests/test_animation.c`](../tests/test_animation.c), 대화형 데모
[`examples/hello_animation/hello_animation.c`](../examples/hello_animation/hello_animation.c).
정확한 동작의 기준은 헤더 주석이며, 이 문서는 그것을 사용 관점에서 풀어 쓴 것입니다. 어긋나면 헤더가 맞습니다.

애니메이션 모듈은 **작고 할당 없는 C11 모듈**입니다. 컨텍스트(`PIXELICA_Context`)도, 시계도, 숨은 전역 상태도 필요 없고
`pixelica_core`(`Pixelica::core`)에 함께 들어 있으므로 별도 라이브러리나 의존성이 없습니다. `#include "pixelica/pixelica.h"` 하나로 쓸 수 있습니다.

| 구성 요소 | 하는 일 |
|---|---|
| `pixelica_anim_ease` | 정규화된 시간 `t`에 이징 곡선 적용 |
| `pixelica_anim_tween` | 두 스칼라 사이를 이징 보간 (±`FLT_MAX` 양끝에서도 오버플로 없음) |
| `PIXELICA_AnimKeyframe`, `pixelica_anim_sample` | 정수 틱 시간 위의 스칼라 키프레임 트랙 샘플링 |
| `PIXELICA_AnimPlayer`, `pixelica_anim_player_*` | 한 번 / 반복 / 왕복 재생 커서 (일시정지, 재시작, O(1) 대량 전진) |
| `PIXELICA_Transform`, `pixelica_transform_matrix` | 이동·오일러 회전·스케일·**피벗**으로 부분(part)의 로컬 4x4 행렬 만들기 |

이 모듈에 의도적으로 **없는 것**: 스키닝/스켈레톤, IK, 클립 에셋 포맷, 블렌드 트리, 씬 노드 자동 갱신.

본 단위 트랙, 쿼터니언 회전, 크로스페이드, 이벤트 콜백은 같은 공통 라이브러리의 **별도 헤더**
[`anim_clip.h`](../include/pixelica/anim_clip.h)에 있습니다 ([`anim_clips.md`](anim_clips.md)).
그쪽은 이 파일의 이징·보간 코드를 그대로 재사용하므로 두 계층의 샘플링 규칙이 어긋나지 않습니다.
리그에 붙이려면 선택 스켈레톤 모듈([`rigging.md`](rigging.md))과 함께 쓰세요.

> **범위 정정.** 이 기본(코어) 애니메이션 모듈에는 지금도 스켈레톤·스키닝·IK가 **없습니다**. 그것들은 코어와 분리된 **선택
> 모듈**(`PIXELICA_BUILD_SKELETON`, `PIXELICA_BUILD_IK`, 기본 `OFF`)로만 제공되며 [`rigging.md`](rigging.md)에 설명되어 있습니다.
> 선택 모듈을 쓸 때도 이 문서의 키프레임 샘플링(`pixelica_anim_sample`)과 플레이어(`PIXELICA_AnimPlayer`)로 각도나 타깃 좌표를
> 만들어 넘기면 됩니다(데모 `hello_rig`). 아래 "없다"는 서술은 모두 코어 기준입니다.

---

## 1. 빠른 시작

팔 하나를 어깨 피벗 둘레로 흔드는 최소 흐름입니다. 키프레임 트랙은 정적 상수 배열, 플레이어는 게임 상태 안의 값입니다.
(조각: 단독 컴파일용이 아니라 게임 콜백 안에 들어가는 코드입니다. 선언과 호출은 실제 API와 같습니다.)

```c
#include "pixelica/pixelica.h"   /* pixelica/animation.h 포함 */

/* 마지막 키 값 == 첫 키 값 이면 LOOP에서 이음매 없이 반복됩니다. */
static const PIXELICA_AnimKeyframe k_swing[3] = {
    {0u,  0.0f, PIXELICA_EASE_SMOOTHSTEP},
    {30u, 1.2f, PIXELICA_EASE_SMOOTHSTEP},
    {60u, 0.0f, PIXELICA_EASE_LINEAR},
};

typedef struct Game {
    PIXELICA_AnimPlayer player;
    float swing;               /* update에서 계산, render에서 사용 */
    PIXELICA_Status anim_status;    /* 첫 실패를 보관 */
} Game;

static bool game_init(Game *g)
{
    g->swing = 0.0f;   /* 틱 0의 키 값과 같음 */
    /* 루프 길이 = 마지막 키 틱(60). LOOP는 틱 60에서 0으로 되감깁니다. */
    g->anim_status = pixelica_anim_player_init(&g->player, 60u, PIXELICA_ANIM_LOOP);
    return g->anim_status == PIXELICA_OK;
}

/* PIXELICA_UpdateFn: 고정 60 Hz로 호출되므로 호출 1회 = 1틱 */
static void game_update(void *userdata, float dt, uint32_t buttons)
{
    Game *g = userdata;
    uint32_t tick = 0u;
    PIXELICA_Status st;

    (void)dt;
    (void)buttons;
    st = pixelica_anim_player_advance(&g->player, 1u);
    if (st == PIXELICA_OK) st = pixelica_anim_player_tick(&g->player, &tick);
    if (st == PIXELICA_OK) st = pixelica_anim_sample(k_swing, 3u, tick, &g->swing);
    if (g->anim_status == PIXELICA_OK) g->anim_status = st;   /* 오류를 숨기지 않음 */
}

static PIXELICA_Status draw_arm(PIXELICA_Context *ctx, const Game *g, const float parent_world[16],
                           const PIXELICA_Mesh *arm_mesh)
{
    PIXELICA_Transform t;
    float local[16], world[16];
    PIXELICA_Status st = g->anim_status;

    if (st != PIXELICA_OK) return st;      /* 오래된 swing을 쓰지 않음 */
    pixelica_transform_identity(&t);
    t.translation.x = 0.65f;
    t.translation.y = 0.15f;          /* 어깨 위치(0.65) - 피벗(0.5) */
    t.pivot.y = 0.5f;                 /* 팔 메시 윗면 중앙 (메시 로컬 좌표) */
    t.scale.y = 1.2f;
    t.rotation.z = g->swing;          /* 라디안 */

    st = pixelica_transform_matrix(local, &t);
    if (st != PIXELICA_OK) return st;
    pixelica_mat4_multiply(world, parent_world, local);   /* 부모 월드 * 자식 로컬 */
    return pixelica_draw_mesh(ctx, arm_mesh, world);
}
```

`game_update`는 실제로 `PIXELICA_UpdateFn` 시그니처이며 `pixelica_update(ctx, elapsed_us, game_update, game)`로 구동됩니다 (7절).

---

## 2. 계약 요약

### 2.1 오류와 "실패하면 아무것도 바뀌지 않는다"

`PIXELICA_Status`를 돌려주는 모든 함수는 다음에 `PIXELICA_ERR_INVALID_ARG`를 돌려줍니다.

- 필수 포인터가 `NULL`
- 알 수 없는 열거값 (`PIXELICA_Ease`, `PIXELICA_AnimMode`)
- 비유한(NaN, ±무한대) `float` 입력
- 모드 불변식을 깨는 플레이어 상태 (손으로 필드를 망가뜨린 경우)
- 변환 행렬 원소가 `float` 범위를 넘는 경우

**실패하면 모든 출력과 플레이어가 바이트 단위로 그대로입니다.** 따라서 `out`을 초기화해 둘 필요가 없고, 실패한 호출 뒤의 `out`을 믿지도 마세요.
`pixelica_transform_identity(NULL)`만 상태 값이 없는 `void` 함수로서 아무 일도 하지 않습니다.

### 2.2 소유권, 수명, 스레딩

- 키프레임 배열, `PIXELICA_AnimPlayer`, `PIXELICA_Transform`, 출력 버퍼는 **전부 호출자 소유**입니다. 모듈은 포인터를 호출 뒤에 보관하지 않습니다 (캐시 없음).
- 배열의 유효한 길이(`count`)는 호출자 책임입니다. 모듈이 그 범위를 검증할 방법은 없습니다.
- 할당(heap) 호출, 전역/정적 상태, 시계 읽기, 컨텍스트 의존이 없습니다.
- 가변 객체(플레이어, 출력)마다 단일 스레드입니다. 서로 다른 객체는 가변 상태를 공유하지 않으므로 다른 스레드에서 써도 됩니다.
- 샘플링 출력 `out`은 같은 트랙의 키프레임 `value`를 가리켜도 됩니다 (읽기를 모두 끝낸 뒤에 씁니다).

### 2.3 시간은 정수 틱입니다

시간 단위는 **틱**(`uint32_t`)이며, 의미는 호출자가 정합니다. 보통 고정 타임스텝의 1스텝(`PIXELICA_UPDATE_HZ` = 60 Hz, 틱 1 = 약 16.67 ms)을 씁니다.
실수 초나 `dt`를 모으지 않으므로 프레임율이 달라도 같은 틱 수는 같은 결과를 냅니다 (재현 가능). 렌더 프레임마다가 아니라 **업데이트 콜백마다** 전진시키세요.

---

## 3. 이징과 tween

### 3.1 `pixelica_anim_ease`

```c
float eased;
PIXELICA_Status st = pixelica_anim_ease(PIXELICA_EASE_QUAD_IN_OUT, 0.25f, &eased);   /* 0.125 */
```

유한한 `t`는 먼저 `[0, 1]`로 clamp됩니다 (`t = 7`이면 1, `t = -FLT_MAX`이면 0). 끝점은 **정확히** 0과 1입니다.

| `PIXELICA_Ease` | 식 (`t` ∈ [0, 1]) |
|---|---|
| `PIXELICA_EASE_STEP` | `t < 1`이면 0, 아니면 1 |
| `PIXELICA_EASE_LINEAR` | `t` |
| `PIXELICA_EASE_QUAD_IN` | `t * t` |
| `PIXELICA_EASE_QUAD_OUT` | `t * (2 - t)` |
| `PIXELICA_EASE_QUAD_IN_OUT` | `t < 0.5`이면 `2 t²`, 아니면 `1 - 2 (1 - t)²` |
| `PIXELICA_EASE_SMOOTHSTEP` | `t² (3 - 2 t)` |

결과는 항상 `[0, 1]` 안이며 STEP을 뺀 곡선은 단조 비감소입니다. `float`으로 내보낼 때 1에 아주 가까운 `t`는 `1.0f`로 반올림될 수 있습니다 (STEP은 `t < 1`이면 항상 0).

### 3.2 `pixelica_anim_tween`

```c
float x;
if (pixelica_anim_tween(0.45f, 0.95f, u, PIXELICA_EASE_SMOOTHSTEP, &x) != PIXELICA_OK) { /* u가 NaN 등 */ }
```

`x = from + (to - from) * ease(clamp(t, 0, 1))`를 계산합니다. 결과는 항상 `[min(from, to), max(from, to)]` 안이고,
이징 값이 정확히 0이면 정확히 `from`, 정확히 1이면 정확히 `to`입니다. 내부 계산은 `double`이라 `from = -FLT_MAX`, `to = FLT_MAX` 같은
반대 극단에서도 오버플로하지 않습니다. 정규화 `t`는 `(float)tick / (float)duration`처럼 직접 만들 수 있지만, `float` 변환이 반올림되므로
`long` 정수 구간(큰 틱 값)에서는 끝점이 반올림될 수 있습니다. 이런 구간은 `pixelica_anim_sample`을 권장합니다.

---

## 4. 키프레임 트랙

```c
typedef struct PIXELICA_AnimKeyframe { uint32_t tick; float value; PIXELICA_Ease ease; } PIXELICA_AnimKeyframe;
PIXELICA_Status pixelica_anim_sample(const PIXELICA_AnimKeyframe *keys, size_t count, uint32_t tick, float *out);
```

### 4.1 규칙

- 트랙 조건 (**매 호출마다** 전체를 검사, O(`count`)): `keys != NULL`, `count >= 1`, 모든 `value` 유한, 모든 `ease` 유효(마지막 키의 사용되지 않는 `ease` 포함),
  틱이 **엄격히 증가**. 첫 틱이 0일 필요는 없습니다. 키 1개는 상수 트랙입니다.
- 샘플 값:
  - `tick <=` 첫 키 틱 → 첫 값, `tick >=` 마지막 키 틱 → 마지막 값 (앞뒤 clamp).
  - 어떤 키의 틱과 정확히 같으면 → 그 키의 `value` (STEP이어도).
  - 구간 안이면 → `tween(왼쪽.value, 오른쪽.value, t, 왼쪽.ease)`, `t = (tick - 왼쪽.tick) / (오른쪽.tick - 왼쪽.tick)`.
    **구간을 이징하는 것은 구간이 시작하는 왼쪽 키의 `ease`**입니다. 오른쪽 키의 `ease`는 그 구간에 영향이 없습니다.
- 정규화는 내부적으로 `double`로 하므로 `UINT32_MAX` 길이 구간(또는 `4294967290`~`UINT32_MAX` 같은 큰 틱)에서도 내부 정규화 값은 틱마다 구분됩니다.
  다만 최종 `float` 결과는 반올림으로 서로 같아질 수 있으며, 인접한 모든 틱이 서로 다른 `float` 값을 낸다고 약속하지는 않습니다.
- 각도(라디안)도 그냥 스칼라로 **직접 보간**합니다. 각도 wrap이나 최단 호 선택이 없으므로 `-3 → 3`은 0을 거쳐 6 라디안을 돕니다.
  돌고 싶은 경로대로 키 값을 적으세요.

### 4.2 STEP과 "유지" 구간

`STEP`은 구간 동안 왼쪽 값을 유지하다가 **다음 키의 틱에서** 새 값으로 점프합니다.

```c
static const PIXELICA_AnimKeyframe k_flash[2] = {
    {0u,  1.0f, PIXELICA_EASE_STEP},
    {10u, 2.0f, PIXELICA_EASE_STEP},
};
/* 틱 0..9 -> 1.0, 틱 10 이후 -> 2.0 */
```

### 4.3 비용

`pixelica_anim_sample`은 검증 때문에 **O(`count`)**, 위치 찾기는 이분 탐색입니다. 키가 수백 개인 트랙을 프레임마다 여러 번 샘플하면 검증이 지배적일 수 있으니
트랙을 짧게 유지하거나 틱이 바뀔 때만 샘플하세요. 할당은 없습니다.

---

## 5. 플레이어

```c
PIXELICA_Status pixelica_anim_player_init(PIXELICA_AnimPlayer *player, uint32_t duration_ticks, PIXELICA_AnimMode mode);
PIXELICA_Status pixelica_anim_player_advance(PIXELICA_AnimPlayer *player, uint32_t ticks);
PIXELICA_Status pixelica_anim_player_tick(const PIXELICA_AnimPlayer *player, uint32_t *out_tick);
```

`duration_ticks >= 1`과 알려진 모드가 필요합니다. `init`은 `phase_ticks = 0`, `paused = false`, `finished = false`로 만들며, **다시 호출하면 재시작**입니다
(일시정지와 끝남도 풀림). `duration_ticks`, `phase_ticks`, `mode`, `finished`는 읽기 전용이고 바꾸려면 `init`을 쓰세요.
`paused`만 직접 토글해도 됩니다.

### 5.1 모드 (D = `duration_ticks`)

| 모드 | `phase_ticks` 범위 | 샘플 틱 (`player_tick`) | `finished` |
|---|---|---|---|
| `PIXELICA_ANIM_ONCE` | 0 ~ D, D에서 멈춤 | `phase` | `phase == D`일 때만 true |
| `PIXELICA_ANIM_LOOP` | 0 ~ D-1 (`D`가 되면 0으로 되감김) | `phase` | 항상 false |
| `PIXELICA_ANIM_PING_PONG` | 0 ~ 2D-1 (64비트 주기) | `phase <= D`이면 `phase`, 아니면 `2D - phase` | 항상 false |

- **LOOP는 틱 D를 보여주지 않습니다.** 마지막 키가 틱 D에 있어도 되감김 순간에 곧바로 틱 0이 됩니다. 이음매 없는 반복이 필요하면
  마지막 키 값을 첫 키 값과 같게 하세요 (1절, 데모의 팔 흔들기와 몸 흔들림). 마지막 키 값을 꼭 보여줘야 하면 ONCE나 PING_PONG을 쓰세요.
- **PING_PONG은 양 끝점을 모두 방문합니다** (0과 D). `D = 4`이면 틱 순서는 `0 1 2 3 4 3 2 1 0 1 …`.
- **ONCE**는 끝에서 `phase == D`, `finished = true`로 멈추고 이후 `advance`는 성공하지만 움직이지 않습니다. 마지막 키의 값이 계속 샘플됩니다.
- `advance(0)`은 허용됩니다. 일시정지 또는 끝난 플레이어에 대한 `advance`도 `PIXELICA_OK`이면서 이동은 없습니다.

### 5.2 O(1) 대량 전진

`advance`는 틱 수와 무관하게 상수 시간입니다 (`UINT32_MAX`도 반복문 없이 나머지 연산). 내부는 64비트 산술이라 `duration_ticks = UINT32_MAX`에서도
넘침이나 잘림이 없습니다. 같은 총 틱 수를 한 번에 전진하든 여러 조각으로 나눠 전진하든 결과가 같습니다 (테스트가 확인).

### 5.3 상태 검증

`advance`와 `player_tick`은 쓰기 전에 불변식을 검사합니다: `duration >= 1`, 알려진 모드, ONCE는 `phase <= D`이고 `finished == (phase == D)`,
LOOP는 `phase < D`이고 `!finished`, PING_PONG은 `phase < 2D`이고 `!finished`. 어긋나면 `INVALID_ARG`이고 플레이어는 변하지 않습니다.
(`bool` 필드에 0/1이 아닌 바이트를 넣는 것은 C에서 미정의 동작이라 감지할 수 없습니다.)

### 5.4 일시정지와 재시작 패턴

```c
PIXELICA_Status st = PIXELICA_OK;
bool restarted = false;

if (pressed & PIXELICA_BTN_START) {                 /* START가 A보다 우선 */
    st = pixelica_anim_player_init(&player, duration, mode);   /* 0틱으로, 정지 해제 */
    restarted = true;
} else if (pressed & PIXELICA_BTN_A) {
    player.paused = !player.paused;
}
if (st == PIXELICA_OK && !restarted) {
    st = pixelica_anim_player_advance(&player, 1u); /* 정지 중에는 성공하고 움직이지 않음 */
}
/* st는 1절의 anim_status처럼 호출자가 보관하고 render에서 돌려줍니다. */
```

재시작을 누른 업데이트는 `advance`를 건너뛰므로 그 프레임은 정확히 틱 0을 보여 주고, 다음 일반 업데이트부터 정상적으로 전진합니다.

---

## 6. 변환과 피벗

```c
typedef struct PIXELICA_Transform {
    PIXELICA_Vec3 translation, rotation, scale, pivot;
} PIXELICA_Transform;
void       pixelica_transform_identity(PIXELICA_Transform *transform);
PIXELICA_Status pixelica_transform_matrix(float out[16], const PIXELICA_Transform *transform);
```

`identity`는 `translation = rotation = pivot = 0`, `scale = 1`로 채웁니다 (`NULL`이면 아무것도 안 함).

### 6.1 행렬 정의

열 우선(column-major), 열 벡터, 오른손 좌표계, 라디안 ([`math.h`](../include/pixelica/math.h)와 같은 규약)이며:

```text
out = T(translation) * T(pivot) * Rz * Ry * Rx * S(scale) * T(-pivot)
```

오른쪽부터 적용됩니다. 즉 메시 정점은 **피벗이 원점에 오도록 이동 → 스케일 → X 회전 → Y 회전 → Z 회전 → 피벗을 되돌림 → 이동** 순입니다.

- **회전은 오일러 XYZ**입니다: X축을 먼저, 그다음 Y, 마지막으로 Z. 쿼터니언이 아니며 짐벌락과 순서 의존성이 있습니다 (XYZ 순서로 만든 각도는 다른 순서로 해석하면 다른 자세).
  한 프레임에 한 축만 움직이는 부품(팔, 문, 바퀴)에는 충분합니다.
- **피벗은 메시 로컬 좌표이고 스케일 전에 적용됩니다.** 회전과 스케일이 모두 피벗 둘레에서 일어나므로, 피벗 점 자체는 어떤 회전·스케일(0이거나 음수 포함)에서도
  **`translation + pivot`에 그대로 남습니다.**
- `translation`은 "피벗 이동이 아니라 **로컬 원점의 변위**"입니다 (R, S가 단위일 때 정점 `p`는 `p + translation`).
  어깨(피벗)를 부모 공간 위치 `S`에 두고 싶으면 `translation = S - pivot`.
- 스케일은 0이나 음수여도 됩니다. 음수 스케일은 삼각형 와인딩을 뒤집어 기본 back-face culling에 걸릴 수 있으니 메시를 뒤집을 일이 아니면 피하세요.
- 모든 성분이 유한해야 하고, 결과 원소가 `float` 범위를 넘으면 (`scale = FLT_MAX`에 큰 피벗 등) `INVALID_ARG`이며 `out`은 그대로입니다.
  회전 각도는 아무 크기의 유한 라디안이어도 됩니다 (wrap하지 않음).
- 렌더러에 비유한 행렬이 가면 `PIXELICA_ERR_INVALID_ARG`가 되므로, 이 함수가 그 앞에서 막아 줍니다.

### 6.2 피벗 예: 박스 팔

단위 박스(정점 ±0.5)를 `scale = (0.3, 1.2, 0.3)`로 늘려 팔로 쓰고 윗면 중앙 `(0, 0.5, 0)`을 어깨 피벗으로 잡습니다.
스케일이 피벗 둘레에서 일어나므로 윗면은 어깨에 붙은 채 팔이 아래로 1.2만큼 자랍니다. 회전하면 어깨를 중심으로 휘두릅니다.

```c
PIXELICA_Transform arm;
pixelica_transform_identity(&arm);
arm.pivot.y       = 0.5f;                   /* 메시 로컬: 윗면 중앙 */
arm.scale         = (PIXELICA_Vec3){0.3f, 1.2f, 0.3f};
arm.translation.x = 0.65f;                  /* 어깨 x = 0.65 (부모 공간) */
arm.translation.y = 0.65f - arm.pivot.y;    /* 어깨 y = 0.65 -> translation = S - pivot */
arm.rotation.z    = angle;
```

### 6.3 부모-자식 합성

계층 API는 따로 없습니다. **자식의 로컬 행렬을 부모의 월드 행렬 오른쪽에 곱하면** 됩니다. 기존 [`pixelica_mat4_multiply`](../include/pixelica/math.h)를 그대로 씁니다.

```c
float parent_world[16], arm_local[16], arm_world[16];
PIXELICA_Status st = pixelica_transform_matrix(parent_world, &body);   /* 루트 */
if (st == PIXELICA_OK) st = pixelica_transform_matrix(arm_local, &arm);
if (st == PIXELICA_OK) {
    pixelica_mat4_multiply(arm_world, parent_world, arm_local);   /* 부모 월드 * 자식 로컬 */
    st = pixelica_draw_mesh(ctx, &arm_mesh, arm_world);
}
```

부모가 **스케일을 가지면 자식도 같은 스케일로 일그러집니다** (비균등 스케일 + 회전은 전단을 만듭니다). 계층 루트에는 이동과 회전만 두고,
부위별 스케일은 각 부위의 로컬 변환에 두는 구성이 안전합니다 (데모가 그렇게 합니다). `double`로 로컬 행렬을 만드는 것은
`pixelica_transform_matrix` 내부뿐이며, 부모 합성에 쓰는 `pixelica_mat4_multiply`는 기존 `float` 연산입니다. 따라서 `pixelica_transform_matrix` 결과를
`float` `pixelica_mat4_*` 빌더를 연쇄한 결과와 비교하면 차이가 날 수 있습니다 (비트 단위 비교 대신 허용 오차로 비교하세요. 오차 상한은 약속하지 않습니다).

### 6.4 씬과 충돌과의 관계

애니메이션 모듈은 **씬을 건드리지 않습니다.** `PIXELICA_SceneNode.model`과 `bounds`는 자동 갱신되지 않으므로, 움직이는 노드는 호출자가
`pixelica_transform_matrix` 결과를 `model`에 복사하고 `bounds`(월드 공간 `PIXELICA_AABB`)도 직접 다시 계산해야 합니다 ([`DEVELOPMENT.md`](DEVELOPMENT.md) 12.1절).
`PIXELICA_AABB`는 축 정렬이라 **회전된 부품의 충돌 상자는 호출자가 정한 근사**(예: 회전 범위를 포함하는 큰 상자)여야 합니다. 이 모듈은 충돌 응답이나
스윕 이동을 제공하지 않습니다 ([`pixelica_aabb_move`](../include/pixelica/scene.h) 참고).

---

## 7. 고정 업데이트와의 통합

`pixelica_update(ctx, elapsed_us, update_fn, userdata)`는 경과 시간을 모아 **고정 60 Hz로 `update_fn`을 0~4번** 호출합니다
(`PIXELICA_MAX_UPDATES_PER_CALL`). 콜백의 `dt`는 항상 고정 스텝(`1/60` 초)이므로 애니메이션은 `dt`를 쓰지 않고 **콜백마다 1틱**씩 전진시키면 됩니다.

```text
렌더 프레임마다:  pixelica_update(ctx, elapsed_us, game_update, game)    // 0..4번 game_update
                  game_render(...)                                   // 현재 pose로 그리기
game_update:      advance(player, 1) -> player_tick -> sample/tween -> pose에 저장
```

`--frames N` 모드는 프레임마다 고정 16667 us를 먹이므로 짧은 실행도 재현 가능합니다. 한 호출에서 4스텝을 넘는 분량은 **버려지므로**
(창이 오래 멈췄다 돌아온 경우 등) 애니메이션은 밀린 만큼 따라잡지 않고 이어집니다. 벽시계와 정확히 맞춰야 하는 연출에는 이 전제가 맞지 않습니다.

**프레임당 할당은 없습니다.** 모듈 함수는 힙을 쓰지 않고, 플레이어와 트랙은 호출자의 정적/상태 메모리에 둡니다.

---

## 8. 대화형 데모 `hello_animation`

[`hello_animation.c`](../examples/hello_animation/hello_animation.c)는 256x192에서 다음을 보여 줍니다.

- 스케일한 박스 부품으로 만든 **가운데 로봇** (몸통, 머리, 바이저, 다리 2개, 팔 2개). 로봇 루트는 키프레임된 좌우 흔들림(yaw)을 가지며,
  오른팔은 **루트의 자식**입니다: `world = root_world * arm_local`, 어깨 피벗 둘레로 손 흔들기 키프레임 각도를 돌립니다.
- 별도의 **큐브**: 위치는 키프레임 트랙(좌우 스윕 + 통통 튐), 크기와 yaw는 `pixelica_anim_tween`으로 PING_PONG 플레이어의 정규화 위치 `tick / 120`에서 계산.
- 플레이어 2개(LOOP 180틱, PING_PONG 120틱)가 **업데이트 콜백마다 1틱** 전진. 상태 코드는 `update`가 보관하고 `render`가 그대로 돌려줍니다.

| 입력 | 동작 |
|---|---|
| Space (`A`) | 두 플레이어 일시정지/재개 (바닥 색조가 바뀜) |
| Enter (`START`) | 두 플레이어를 틱 0으로 재시작하고 정지 해제 (같은 업데이트에서 전진하지 않아 정확히 틱 0을 보여 줌, `A`보다 우선) |
| Escape / 창 닫기 | 종료 |

눌림 엣지로 처리하므로 키를 누르고 있어도 한 번만 토글됩니다.

hello_animation 캡처 (local-only image, not in the repository)

*256x192 RGB565, 최근접(nearest) 2배 표시, 샘플 프레임 180장을 60 Hz 기준 4틱 간격으로 캡처, 합성 주기 12초.*

### 8.1 빌드와 실행

예제는 기본으로 빌드됩니다 (`PIXELICA_BUILD_EXAMPLES=ON`). 구성 옵션은 README의 "Build, run, test"와 같습니다.

```sh
# Windows (Visual Studio 생성기)
cmake -S . -B build -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON
cmake --build build --config Debug --target hello_animation
build\Debug\hello_animation.exe
build\Debug\hello_animation.exe --frames 120 --hidden     # 창 없이 120프레임 스모크

# Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON
cmake --build build --parallel --target hello_animation
./build/hello_animation
./build/hello_animation --frames 120 --hidden

# SDL 없는 HEADLESS 빌드 (화면 대신 마지막 프레임의 FNV-1a 해시를 출력)
cmake -S . -B build-headless -DCMAKE_BUILD_TYPE=Debug -DPIXELICA_PLATFORM=HEADLESS
cmake --build build-headless --target hello_animation
./build-headless/hello_animation --frames 120
```

헤드리스 해시나 `--hidden` 스모크는 "프로그램이 시작해서 그리고 정상 종료한다"만 증명합니다. 화면이 어떻게 보이는지는 사람이 창을 직접 봐야 합니다.

---

## 9. 테스트

[`tests/test_animation.c`](../tests/test_animation.c)는 `ctest`에 `test_animation`으로 자동 등록됩니다 (`assert`를 쓰지 않아 Release에서도 동작).

```sh
ctest --test-dir build -C Debug -R "test_animation|smoke_hello_animation" --output-on-failure
```

확인하는 것: 이징 끝점·clamp·수치·단조성·대칭·잘못된 입력, tween 극단 끝점과 오버플로 없음, 키프레임 구간/모든 이징/정확한 키/앞뒤 clamp/
`UINT32_MAX` 경계/엘리어싱/검증 실패 시 불변, 플레이어 3모드의 `duration = 1`·`UINT32_MAX`·대량 전진·일시정지·재시작·잘못된 상태 거부,
**조각 전진 = 한 번에 전진** 속성(결정적 의사 난수)과 단위 스텝 참조 모델, 변환의 피벗 고정·회전 순서·스케일 순서·문서 식과의 일치·
비유한/오버플로 거부·부모-자식 합성, 그리고 피벗 회전이 실제 픽셀에 나타나는지(반 바퀴 회전이 중심 피벗에서는 같은 픽셀, 원점 피벗에서는 다른 픽셀)와 메모리 가드.
테스트 결과는 감사 기록(`REMAINING_AUDIT.md` 계열)에 실제 실행 뒤 별도로 적습니다.

---

## 10. 제한

- 스칼라 트랙만 있습니다. 위치·회전 같은 벡터는 성분마다 트랙을 따로 두거나,
  벡터·쿼터니언 트랙을 가진 [`anim_clip.h`](../include/pixelica/anim_clip.h)를 쓰세요.
- `PIXELICA_Transform`은 오일러 XYZ이며 각도를 최단 호 없이 직접 보간합니다. 쿼터니언과
  최단 호 구면 보간은 `PIXELICA_Quat`/`PIXELICA_TRS` 쪽에 있습니다([`anim_clips.md`](anim_clips.md)).
- 이음매 없는 LOOP는 마지막 키가 틱 D에서 보이지 않는 점을 고려해 설계해야 합니다 (5.1절).
  클립 플레이어도 같은 규칙입니다.
- 이 헤더 자체에는 계층, 본, 스키닝, IK, 이벤트, 블렌딩, 클립 에셋 파일이 없습니다.
  본 계층 포즈·스키닝·두 본 IK는 선택 모듈([`rigging.md`](rigging.md)),
  본별 클립·크로스페이드·이벤트는 [`anim_clips.md`](anim_clips.md)를 보세요. 이 모듈만으로
  이벤트를 흉내 내려면 `player_tick`의 틱 값을 직접 비교하면 됩니다.
- 자동으로 씬 노드나 `bounds`를 갱신하지 않습니다 (6.4절).
- 내부 계산에 `double`을 쓰므로 단정밀도 FPU만 있는 MCU에서는 비용이 더 들 수 있으나, **MCU에서의 `double` 비용은 측정하지 않았습니다 (보드 없음).**
  `pixelica_transform_matrix`는 고정 크기 O(1)이며 `sinf`/`cosf` 호출과 16개 원소의 변환·검사를 포함합니다.
- 비용 요약: `ease`/`tween`/`player_*`/`transform_*`는 O(1), `sample`은 O(`count`) 검증 + O(log `count`) 탐색. 어느 것도 할당하지 않습니다.
