# 픽셀 조명 도구 (`pixelica/light.h`)

창문 모양 **광 패치**, 부드러운 **접지 그림자**, **선택적 블룸** 세 가지를 재사용 가능한
도구로 제공합니다. 전부 **opt-in**이며, 호출하지 않으면 아무 비용도 없고 기존 드로우
경로의 출력은 비트 단위로 그대로입니다.

- 헤더 `pixelica/light.h`는 **공통 라이브러리**(`Pixelica::common`)에 있습니다. 렌더러에
  의존하지 않으므로 2D 전용·3D 전용·하이브리드 SDK 모두에서 쓸 수 있습니다.
- 3D 드로우 헬퍼 `pixelica_draw_light` / `pixelica_draw_contact_shadow`와 이미시브 마스크
  훅은 3D 코어(`pixelica/pixelica.h`, `pixelica/renderer.h`)에 있습니다.
- 힙 할당·전역 상태·숨은 큐가 없습니다. 모든 버퍼는 **호출자 소유**이고 호출 동안만
  빌려 씁니다. 모든 함수는 **쓰기 전에 전부 검증**하므로, 실패하면 출력은 그대로입니다.

> **물리 섀도잉이 아닙니다.** 광 패치는 호출자가 배치하는 지오메트리입니다. 가시성
> 계산·레이 추적이 없고, 개구부와 바닥 사이에 물체가 있어도 패치에 구멍이 뚫리지
> 않습니다. 접지 그림자도 실루엣이 아니라 물체 발밑의 **블롭**입니다. 다만 패치와
> 그림자는 **깊이 테스트를 받으므로** 더 가까운 지오메트리에 가려집니다.

## 1. 모양 마스크와 틴트

```c
PIXELICA_LightMaskDesc desc;
PIXELICA_LightMask mask;
PIXELICA_Texture tint;
static uint8_t alpha[48 * 32];
static uint16_t texels[48 * 32];

pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &desc); /* 창문 기본값 */
desc.panes_x = 2u; desc.panes_y = 3u; desc.bar = 0.13f;
pixelica_light_mask_build(&desc, alpha, sizeof alpha, &mask);
pixelica_light_tint_fill(PIXELICA_RGB565(255, 196, 120), 48, 32, texels,
                         48 * 32, &tint);
```

- `RECT`는 부드러운 가장자리 + 선택적 **창살**(`panes_x/y`, `bar`), `ELLIPSE`는 부드러운
  블롭입니다. `softness`는 반 폭 대비 페이드 비율, `fade_v`는 아래 행으로 갈수록 추가로
  어두워지는 비율, `peak`는 최대 알파입니다. 정확한 식은 헤더 주석에 있습니다.
- 마스크는 **tight**(stride == width)하게 채워집니다. 두 렌더러의 `texture_alpha` 평면이
  요구하는 형식이 그대로입니다.
- 색은 **평평한 틴트 이미지**이고 모양은 전부 마스크에 있습니다. 두 이미지는 크기가 같아야
  합니다(렌더러가 같은 텍셀에서 색과 알파를 읽기 때문).
- 모든 필드는 모양과 무관하게 범위 검사합니다. 0으로 초기화한 구조체는 유효하지 않으므로
  `pixelica_light_mask_default()`에서 시작하세요.

## 2. 수신면(receiver) 쿼드

| 함수 | 쓰임 |
| --- | --- |
| `pixelica_light_quad` | 평면 위에 직접 놓기 (원점 + 두 축) |
| `pixelica_light_quad_centered` | 중심 + 반지름 두 개 (블롭 그림자) |
| `pixelica_light_quad_project` | 개구부를 **빛의 진행 방향으로 투영** (창문 → 바닥) |

```c
PIXELICA_LightPlacement opening = {0};
opening.origin = (PIXELICA_Vec3){0.5f, 1.0f, -3.9f};   /* 창 아래 모서리 */
opening.u_axis = (PIXELICA_Vec3){2.0f, 0.0f, 0.0f};    /* 창 너비 */
opening.v_axis = (PIXELICA_Vec3){0.0f, 1.3f, 0.0f};    /* 창 높이 */
opening.normal = (PIXELICA_Vec3){0.0f, 0.0f, 1.0f};

PIXELICA_LightQuad quad;
pixelica_light_quad_project(&opening, sun_direction,
                            (PIXELICA_Vec3){0, 0, 0},   /* 바닥 위의 한 점 */
                            (PIXELICA_Vec3){0, 1, 0},   /* 바닥 법선 */
                            0.03f, &quad);              /* 깊이 바이어스 */
```

- `direction`은 빛이 **진행하는** 방향입니다(`PIXELICA_DirectionalLight.direction`과 같은
  규약). 수직 창에 수직으로 내리꽂는 빛처럼 투영 결과가 선으로 **퇴화**하거나, 평면이
  모서리 앞에 없으면(`t <= 0`) `PIXELICA_ERR_INVALID_ARG`입니다. 가짜 결과를 만들지
  않습니다.
- 인덱스 순서는 `normal` **+쪽에서 볼 때 앞면**(CCW)이 되도록 정해지므로 기본
  `PIXELICA_CULL_BACK` 그대로 쓸 수 있습니다.
- **`bias`는 수신면에서 띄우는 거리**입니다. 깊이 버퍼가 비선형이라 필요한 값은 거리에
  따라 다릅니다. 예제 장면(near 1, far 40, 카메라 거리 13.5)에서는 깊이 1단계가 약
  **0.003 월드 단위**이므로 `0.03`(약 10단계)을 씁니다. 너무 작으면 그림자가 바닥과
  싸워서 픽셀이 듬성듬성 사라지고, 너무 크면 떠 보입니다.

## 3. 그리기 (3D)

```c
PIXELICA_LightDraw patch = {0};
patch.quad = quad;
patch.tint = tint;          /* 평평한 색 */
patch.mask = mask;          /* 모양, tint와 같은 크기 + tight */
patch.mode = PIXELICA_ALPHA_ADD;   /* 빛 = 가산, 그림자 = BLEND */
patch.opacity = 235u;
pixelica_draw_light(ctx, &patch, &effects);   /* effects는 NULL 가능 */

PIXELICA_ContactShadow blob = {0};
blob.center = ...; blob.normal = (PIXELICA_Vec3){0,1,0};
blob.u_axis = light_ground_direction;
blob.radius_u = 0.6f; blob.radius_v = 0.45f; blob.bias = 0.03f;
blob.tint = dark_tint; blob.mask = ellipse_mask; blob.opacity = 190u;
pixelica_draw_contact_shadow(ctx, &blob, NULL);   /* 항상 BLEND */
```

둘 다 **기존 `pixelica_draw_mesh_effects` 경로**로 쿼드 하나를 그릴 뿐입니다. 따라서
래스터라이저에 새 코드가 없고 다음이 자동으로 따라옵니다.

- **깊이 테스트는 받지만 깊이를 쓰지 않습니다**(`blending.md`의 ADD/BLEND 규칙 그대로).
  패치끼리, 그리고 패치와 이후 지오메트리는 서로 가리지 않습니다.
- 한 번 호출 = **드로우 커맨드 1개**. `max_draw_commands` 예산에 포함됩니다.
- `effects`는 평범한 `PIXELICA_MeshEffects`입니다. `PIXELICA_FILTER_BILINEAR`로 마스크
  가장자리를 부드럽게 하거나 픽셀 훅을 걸 수 있습니다.
- `mode`는 `ADD`/`BLEND`만 받습니다. 그리는 순서는 **불투명 → 그림자(BLEND) → 빛(ADD)**.

**2D에서는** 새 API가 필요 없습니다. 같은 마스크·틴트를 `PIXELICA2D_Sprite`의 텍스처와
`PIXELICA_DrawOptions.texture_alpha`로 넘겨 `pixelica2d_draw_sprite_ex`를
`PIXELICA_ALPHA_ADD`(빛) 또는 `PIXELICA_ALPHA_BLEND`(그림자)로 호출하면 됩니다
([`texture_options.md`](texture_options.md)).

## 4. 선택적 블룸

```c
PIXELICA_LightSurface target = {pixels, w, h, stride};   /* 쓰기 가능한 RGB565 */
PIXELICA_LightMask selection = {emissive_bytes, w, h, w};
PIXELICA_BloomDesc bloom;

pixelica_bloom_default(&bloom);
bloom.mask = &selection;          /* 또는/그리고 bloom.emissive = &frame_view */
bloom.downsample = 4u; bloom.radius = 3u; bloom.passes = 2u; bloom.intensity = 190u;
need = pixelica_bloom_scratch_pixels(&target, &bloom);
pixelica_bloom_apply(&target, &bloom, scratch, need);
```

- **무엇이 빛나는지는 명시적으로 고릅니다.** 8비트 이미시브 마스크, 별도로 렌더한 이미시브
  이미지, 또는 둘 다. 둘 다 없으면 거부합니다. `threshold`는 **선택 안에서만** 동작하는
  보조 게이트이며 기본값 0(끔)입니다. 완성된 프레임의 밝기만으로 고르는 방식은
  제공하지 않습니다.
- 파이프라인: 선택 + 박스 다운샘플 → 분리형 박스 블러(`passes`회, 가로/세로, 가장자리
  클램프) → 바이리니어 업샘플 → **가산 합성**. 합성이 더하기만 하므로 글로우가 닿지 않은
  픽셀은 **값이 정확히 그대로**이고, 픽셀아트가 뭉개지지 않습니다.
- 스크래치는 `6 * ceil(w/f) * ceil(h/f)` 픽셀(핑퐁 2면 × 채널 3개)입니다. 가로세로가
  `downsample`로 나눠떨어지지 않아도 되고, 1x1 프레임도 됩니다.
- 비용은 `O(w*h + dw*dh*radius*passes)`이고 힙을 쓰지 않습니다. `intensity == 0`이면
  검증만 하고 아무 일도 하지 않습니다.

### 정밀도와 측정된 한계

중간 평면은 채널당 **8비트**를 유지합니다(패스마다 RGB565로 다시 양자화하지 않음).
합성은 반올림을 한 번만 하므로, 픽셀을 바꾸는 최소 글로우는 RGB8 기준 약 `128/m`
(빨강·파랑 5, 초록 3)입니다. 그래도 **평균화 자체는 물리적**입니다. `test_bloom`이
고정해 둔 측정값(배경 `0x2104`, 이미시브 텍셀 하나 `0x8410`, intensity 255):

| downsample | radius | 결과 |
| --- | --- | --- |
| 8 | 8 | 이웃 픽셀이 **전혀 바뀌지 않음** (1픽셀 발광체가 64픽셀 블록 + 17탭에 희석) |
| 1 | 2 | 이웃 픽셀이 눈에 띄게 밝아짐 |

작고 어두운 발광체를 살리려면 `downsample`/`radius`를 줄이거나, 이미시브 색을 더 밝게
하거나, `intensity`를 올리세요.

## 5. 이미시브 마스크: 숨은 발광체 처리

```c
PIXELICA_EmissiveMask emissive = {0};
emissive.alpha = bytes; emissive.depth = depths;   /* depth는 선택 */
emissive.width = w; emissive.height = h; emissive.stride = w;

pixelica_emissive_mask_clear(&emissive);
effects.pixel = pixelica_emissive_mask_pixel;
effects.userdata = &emissive;
pixelica_draw_light(ctx, &glow, &effects);         /* 커버리지를 기록 */
...
pixelica_end_frame(ctx);
pixelica_frame_view(ctx, &frame);
pixelica_emissive_mask_resolve(&emissive, &frame, &removed);  /* 가려진 발광체 제거 */
```

훅은 이미지에 **아무 영향도 주지 않습니다**(항상 픽셀을 유지). 주의할 두 가지:

1. 훅은 **알파 단계보다 먼저** 실행됩니다. 알파 0인 프래그먼트는 어떤 모드에서도 보이지
   않으므로 **절대 기록하지 않습니다**. `PIXELICA_ALPHA_TEST`를 쓴다면 그 드로우의
   `alpha_cutoff`를 `min_alpha`에 넣어, 테스트에서 버려질 프래그먼트가 빛나지 않게
   하세요.
2. 훅은 **그 시점의** 깊이 테스트를 통과한 프래그먼트를 봅니다. 나중에 불투명
   지오메트리가 덮으면 가려진 발광체가 빛날 수 있습니다. 이미시브를 **마지막에** 그리거나,
   `depth` 평면을 주고 `pixelica_emissive_mask_resolve()`로 최종 깊이 버퍼와 비교해
   가려진 픽셀을 지우세요 (`test_light_draw`가 두 경우를 모두 확인합니다).

`alpha`/`depth` 평면은 드로우가 읽는 메모리(마스크·텍스처·정점)나 컨텍스트 소유 버퍼와
겹치면 안 됩니다. 훅은 이를 검사할 수 없습니다.

## 6. 메모리 겹침 규칙

주소 범위로 검사하고 위반은 `PIXELICA_ERR_INVALID_ARG`입니다.

- `pixelica_light_mask_build` / `_tint_fill`: 쓰는 범위에 `desc`나 출력 구조체가 들어
  있으면 거부.
- `pixelica_bloom_apply`: `scratch`는 타깃 픽셀·마스크·이미시브·모든 서술자와 겹칠 수
  없습니다. **서술자(`target`/`desc`/`mask`/`emissive`)가 타깃 픽셀 범위 안에 있어도
  거부**합니다(합성이 그 구조체들을 읽으면서 타깃에 쓰기 때문). 반대로 읽기 전용
  **마스크 바이트와 이미시브 픽셀은 타깃과 겹쳐도 됩니다**. 다운샘플 패스가 모든 소스
  픽셀을 먼저 소비한 뒤에야 합성이 타깃에 쓰기 때문이고, "방금 그린 프레임을 마스크로
  블룸한다"가 바로 이 경우입니다.
- 패치/그림자 드로우의 마스크·틴트가 컨텍스트 소유 메모리와 겹치면 기존 드로우 경로가
  거부합니다.

## 7. 실행 예제와 캡처

`examples/hello_pixel_light/`가 **Pixel Room 장면**을 그대로 쓰면서 세 도구를 모두
보여 줍니다(Pixel Room 자체는 수정하지 않았습니다). 자체 컨텍스트와 프레임 루프를
가지므로 패치는 프레임 안에서, 블룸은 호출자 소유 스냅샷에서 합성됩니다.

구성: 창 개구부를 바닥에 투영한 광 패치 1개, 창 주변 벽 워시 1개, 프롭 5개와 큐브의
접지 그림자 6개, 그리고 **유리 안에 그려진 해**에만 걸린 작은 가산 글로우 1개.
글로우가 유리 전체가 아니라 해에만 걸려 있는 것이 핵심입니다. 하늘·구름·창살 픽셀은
원본 그대로 남고, 정말 밝은 지점 하나만 번집니다. 발광체가 작으므로 블룸은
`downsample 2, radius 2, passes 2`를 씁니다(4로 내리면 §4 표대로 사라집니다).

```sh
./build/hello_pixel_light --frames 8 --effects both \
    --ppm lit.ppm --ppm-base base.ppm --scale 3
```

- `--effects on|off|both`로 효과를 켜고 끄고 비교합니다. `--ppm` / `--ppm-base`가 실제
  RGB565 버퍼를 PPM으로 씁니다.
- 출력은 단계별 `ms/frame`(scene / light / bloom)과 결정적인 요약 한 줄입니다. 프로그램이
  스스로 불변식을 확인하고 `ok=1`만 통과로 인정합니다: 효과를 켠 프레임이 끈 프레임과
  다를 것, 모든 패치·그림자·글로우 드로우가 수락될 것, 이미시브 마스크가 비어 있지 않을
  것, **블룸이 어떤 채널도 줄이지 않을 것**.

## 8. 테스트

- `tests/test_light_mask.c` — 마스크 수식(가장자리·창살·fade·1x1), 틴트 채우기, 세 가지
  쿼드 생성기의 수치와 와인딩, 퇴화·비유한·오버플로 거부, 실패 시 출력 보존, 겹침 거부.
- `tests/test_bloom.c` — 선택성(마스크 0이면 아무 일도 안 함, 선명한 가장자리 보존,
  채널이 줄지 않음), 이미시브 이미지와 `threshold`, 소스-타깃 에일리어싱 허용,
  **서술자가 타깃 안에 있을 때 거부**, 스크래치 용량·겹침, 나눠떨어지지 않는 크기,
  1x1, stride 패딩, 포화, 정밀도 한계.
- `tests/test_light_draw.c` — ADD/BLEND 산술, **깊이 가림**과 깊이 미기록, 접지 그림자의
  부드러움과 불투명도, 모든 거부 경로에서 색·깊이·통계 보존, 컨텍스트 메모리 에일리어싱
  거부, 드로우 예산, 이미시브 기록(알파 0·`min_alpha`·상수 값)과 `_resolve`.
- `smoke_hello_pixel_light`, `smoke_hello_pixel_light_tiny` — 예제의 자체 검증.
- 설치된 SDK 검사(`sdk_installed_template`)의 2D·3D 소비자가 각각 마스크/블룸과
  `pixelica_draw_light`/이미시브 훅을 실제로 호출합니다.
