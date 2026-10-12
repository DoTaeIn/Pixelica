# 선택 모듈: 셰이더 (Shaders)

> RGB565 프레임버퍼를 **CPU에서 후처리**하는 선택 모듈입니다. GLSL·HLSL·SPIR-V 같은 셰이더 언어도, GPU도 쓰지 않습니다.
> "셰이더"는 화면의 모든 픽셀에 같은 C 함수를 적용하는 후처리 효과라는 뜻입니다. 할당(`malloc`)과 전역 변수 없이 호출자가 준 버퍼만 사용합니다.
> 기본값은 `OFF`이며 기본 SDK ZIP에는 들어 있지 않습니다. 이 문서는 **계약과 사용법**을 설명합니다.

| 항목 | 값 |
|---|---|
| 옵션 | `PIXELICA_BUILD_SHADERS` (기본 `OFF`) |
| 라이브러리 | `Pixelica::shaders` — 헤더 [`pixelica/shaders.h`](../modules/shaders/include/pixelica/shaders.h), 구현 `modules/shaders/shaders.c` |
| 의존성 | common(`pixelica/status.h`, `pixelica/platform.h`)만. 2D·3D 렌더러, SDL에 의존하지 않음 |
| 언어 | C11, 힙 사용 없음, 전역 상태 없음 |
| 예제 | `hello_shaders` ([`examples/hello_shaders/main.c`](../examples/hello_shaders/main.c)), 창·SDL·렌더러 없이 오프스크린 |
| 테스트 | `tests/test_shaders_presenter.c`(가짜 PAL로 프레젠터 검증). 효과 자체의 테스트는 코어 테스트가 맡습니다 |
| 배포 파일 | `PixelicaShaders-0.1.0-windows-AMD64.zip`, `PixelicaShaders-0.1.0-source.zip` (`out/pixelica-shaders-release`) |

## 1. 빠른 시작

### 1.1 예제 실행

```bash
cmake -S . -B build-shaders -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_SHADERS=ON
cmake --build build-shaders --config Release
./build-shaders/hello_shaders --output sheet.ppm
```

`hello_shaders`는 코드로 만든 160×120 장면(그라데이션 하늘과 해, 색 막대 여섯 개, 체크무늬 바닥)에 열 가지 효과를 **각각 따로** 적용합니다.
결과를 2×2 RGB8 평균으로 줄여 5×2 비교 시트(400×120)에 붙이고, 시트를 두 번 처음부터 만들어 해시가 같은지 확인합니다.
홀수 줄도 평균에 포함하므로 스캔라인 효과가 축소 과정에서 사라지지 않습니다.
또한 모든 패널이 원본과 다르고 서로 다른지, 사용자 정의 콜백이 정확히 픽셀 수만큼 불렸는지 검사합니다.
`--output PATH`를 주면 시트를 바이너리 PPM(P6)으로 저장합니다. 해시는 같은 빌드(컴파일러·옵션·플랫폼)끼리만 비교하세요.
Visual Studio에서는 `.\build-shaders\Release\hello_shaders.exe`처럼 구성 폴더 안에서 실행해야 할 수 있습니다.

### 1.2 최소 코드

아래는 호출 순서를 보여 주는 조각입니다. 실제 코드는 각 `PIXELICA_Status`를 확인하세요.

```c
#include "pixelica/shaders.h"

static uint16_t pixels[160 * 120];   /* 이미 그려 둔 RGB565 화면 */

PIXELICA_ShaderSurface surface = { pixels, 160, 120, 160 };   /* pixels, width, height, stride */
PIXELICA_Shader shader;

pixelica_shader_default(PIXELICA_SHADER_VIGNETTE, &shader);   /* 종류별 기본값 */
shader.amount = 0.6f;
pixelica_shader_apply(&surface, &shader, NULL, 0);            /* VIGNETTE는 scratch가 필요 없음 */
```

## 2. 자료형

```c
typedef struct PIXELICA_ShaderSurface { uint16_t *pixels; uint16_t width, height; uint32_t stride; } PIXELICA_ShaderSurface;
typedef struct PIXELICA_ShaderImage   { const uint16_t *pixels; uint16_t width, height; uint32_t stride; } PIXELICA_ShaderImage;

typedef struct PIXELICA_ShaderPixel {
    const PIXELICA_ShaderImage *source;   /* 효과 적용 전의 불변 스냅샷 */
    uint16_t x, y, color;                 /* 위치와 현재 색 */
} PIXELICA_ShaderPixel;

typedef uint16_t (*PIXELICA_PixelShader)(const PIXELICA_ShaderPixel *pixel, void *userdata);

typedef struct PIXELICA_Shader {
    PIXELICA_ShaderKind kind;
    float amount;                 /* 0..1 */
    uint16_t color;               /* RGB565 */
    uint8_t levels, radius;
    const uint16_t *palette; uint16_t palette_count;
    PIXELICA_PixelShader callback; void *userdata;
} PIXELICA_Shader;
```

| 함수 | 역할 |
|---|---|
| `pixelica_shader_default(kind, &out)` | 종류별 기본값으로 채움 |
| `pixelica_shader_scratch_pixels(&surface, &shader)` | 필요한 scratch **픽셀 수**. 필요 없거나 입력이 잘못되면 0 (판정은 `apply`가 최종) |
| `pixelica_shader_apply(&surface, &shader, scratch, scratch_count)` | 표면 전체에 효과를 제자리 적용 |
| `pixelica_shader_sample(&image, x, y)` | 좌표를 가장자리로 고정(clamp)해서 읽음. 잘못된 이미지면 0 |
| `pixelica_shader_presenter_init(...)` | PAL을 감싸는 프레젠터 초기화 (8절) |

## 3. 공통 규칙

- **표면**: `pixels`는 필수이고 `width`, `height`는 1~4096, `stride >= width`입니다. 행 `y`는 `pixels[y * stride]`에서 시작하며, **행 사이 패딩은 읽지도 쓰지도 않습니다**.
  크기와 범위 계산은 오버플로를 검사합니다.
- **먼저 검사, 나중에 쓰기**: 모든 효과가 출력·scratch를 건드리기 **전에** 입력을 검사합니다. 잘못된 인자는 `PIXELICA_ERR_INVALID_ARG`,
  버퍼가 모자라면 `PIXELICA_ERR_CAPACITY`이고 이때 표면은 그대로입니다.
- **겹침 거부**: 출력 표면이 셰이더 구조체·표면 구조체·팔레트와 겹치면 거부합니다. 필요한 scratch가 출력·셰이더·표면·팔레트와 겹쳐도 거부합니다.
  관련된 필드만 검사하며, 사용자 `userdata`·콜백이 가리키는 메모리와 할당 크기가 맞는지는 호출자 책임입니다(임의 포인터의 유효성은 확인할 수 없습니다).
- **단일 스레드**: 같은 표면·scratch·프레젠터를 여러 스레드에서 동시에 쓰지 마세요.

## 4. 열 가지 효과

공통: RGB565 → RGB8 확장은 비트 복제(`r8 = (r5 << 3) | (r5 >> 2)`, `g8 = (g6 << 2) | (g6 >> 4)`, `b8`은 `r8`과 같은 방식)이고, RGB8 → RGB565 압축은 **상위 비트**를 취합니다.
"기본값"은 `pixelica_shader_default`가 채우는 값입니다. 공통 기본값: `amount = 1`, `color = 흰색`, `levels = 4`, `radius = 1`, `palette = NULL`, `palette_count = 0`, `callback = NULL`
(OUTLINE만 `amount = 0.2`, `color = 검정`). 팔레트와 콜백은 직접 지정하기 전에는 비어 있어 `apply`가 거부합니다.

| 종류 | 설명 | 쓰는 필드 | scratch |
|---|---|---|---|
| `PIXELICA_SHADER_TINT` | 원본과 `color`를 `amount`만큼 섞음 | `amount`, `color` | 없음 |
| `PIXELICA_SHADER_GRAYSCALE` | 가중 정수 밝기로 회색을 만들고 `amount`만큼 섞음 | `amount` | 없음 |
| `PIXELICA_SHADER_INVERT` | 색 반전과 원본을 `amount`만큼 섞음 | `amount` | 없음 |
| `PIXELICA_SHADER_POSTERIZE` | 채널당 단계 수를 줄임 | `levels` (2~32) | 없음 |
| `PIXELICA_SHADER_PALETTE` | 가장 가까운 팔레트 색으로 바꿈 | `palette`, `palette_count` (1~256) | 없음 |
| `PIXELICA_SHADER_SCANLINES` | 홀수 행을 어둡게 | `amount` | 없음 |
| `PIXELICA_SHADER_VIGNETTE` | 가장자리를 검정으로 어둡게 | `amount` | 없음 |
| `PIXELICA_SHADER_BLUR` | 상자(box) 평균 흐림 | `radius` (1~4), `amount`는 **무시** | `width*height` |
| `PIXELICA_SHADER_OUTLINE` | 밝기 차이가 큰 곳을 `color`로 칠함 | `amount`, `color` | `width*height` |
| `PIXELICA_SHADER_CUSTOM` | 픽셀마다 사용자 콜백 호출 | `callback`, `userdata` | `width*height` |

`TINT`, `GRAYSCALE`, `INVERT`, `SCANLINES`, `VIGNETTE`, `OUTLINE`의 `amount`는 **유한한 값이고 0~1**이어야 합니다(NaN·무한대·범위 밖은 `PIXELICA_ERR_INVALID_ARG`).

### 4.1 amount와 반올림

- **섞기(mix)**: 채널마다 RGB8에서 `원본 + (대상 - 원본) × amount`를 **가장 가까운 정수로 반올림**한 뒤 RGB565로 압축합니다. `amount = 0`이면 원본 그대로(RGB565 → RGB8 → RGB565 왕복은 손실이 없으므로 같은 값), `amount = 1`이면 대상 색입니다.
- **GRAYSCALE**: 밝기 `Y = (77R + 150G + 29B + 128) / 256`(정수, RGB8)을 만들고, 이를 회색 (Y, Y, Y)로 보고 `amount`만큼 섞습니다.
- **INVERT**: 대상은 (255-R, 255-G, 255-B)입니다.
- **POSTERIZE**: 채널(RGB8)을 `levels`개의 균등한 값으로 **가장 가까운 단계**에 맞춥니다. 두 단계가 같은 거리면 어느 쪽인지는 정수 반올림 규칙을 따릅니다.
  `levels`가 2~32 밖이면 거부합니다. 결과는 RGB565로 압축되므로 R·B는 5비트, G는 6비트 해상도로 다시 줄어듭니다.
- **PALETTE**: 팔레트는 RGB565 배열이며 **빌려 쓰기**만 합니다(복사하지 않음). 각 색을 RGB8로 확장해 원본 픽셀과의 **제곱 거리**(`dR² + dG² + dB²`)가 가장 작은 색을 고릅니다. 같으면 **먼저 나온 색**입니다.
- **SCANLINES**: 홀수 행(`y`가 홀수)만 검정 쪽으로 `amount`만큼 섞습니다. 짝수 행은 그대로입니다.
- **VIGNETTE**: `nx = (2x - (w-1)) / (w-1)`, `ny`도 같은 식(높이 기준), 가중치 `t = clamp(nx² + ny², 0, 1)`로 검정과 `amount × t`만큼 섞습니다.
  가로나 세로가 1픽셀이면 그 축의 값은 0으로 둡니다. 화면 가장자리 모서리에서 가장 어둡습니다.
- **BLUR**: 반지름 `radius`(1~4)의 정사각형 창을 RGB8 채널별로 평균하고 정수로 반올림합니다. 가장자리 밖은 가장자리 픽셀로 **고정(clamp)**합니다. `amount`는 쓰지 않습니다.
- **OUTLINE**: 현재 픽셀과 상·하·좌·우 이웃(가장자리는 clamp)의 밝기(GRAYSCALE과 같은 식)를 비교하여, 차이의 절댓값이 하나라도 `round(amount × 255)`보다 **크면** 그 픽셀을 `color`로 바꿉니다.
  `amount`가 작을수록 더 많은 선이 생깁니다. 이웃은 **적용 전 스냅샷**에서 읽으므로 이미 칠한 픽셀이 영향을 주지 않습니다.
- **CUSTOM**: `callback`이 필수입니다. 콜백은 모든 픽셀에서 정확히 한 번 불리고, 반환값이 새 색입니다. 읽기는 불변 스냅샷이므로 다른 픽셀을 읽어도 이미 바뀐 값을 보지 않습니다.

### 4.2 scratch 개수·소유권·용량·겹침

- `BLUR`, `OUTLINE`, `CUSTOM`은 적용 전 화면을 보존할 **`width × height`개의 `uint16_t`** 스냅샷이 필요합니다. 행 간격 없이 빽빽한(tight) 배열입니다.
  나머지 효과는 `scratch = NULL`, `scratch_count = 0`으로 호출해도 됩니다.
- `pixelica_shader_scratch_pixels`가 필요한 개수를 돌려줍니다(필요 없거나 입력이 잘못되면 0). 이 값이 0이어도 `apply`가 거부할 수 있으므로 **성공 여부는 항상 `apply`의 반환값**으로 판단하세요.
- **소유권**: scratch는 호출자 것입니다. 모듈은 호출 중에만 쓰고, 끝난 뒤의 내용은 의미가 없습니다. 호출 사이에 재사용해도 됩니다.
- **용량**: `scratch_count`가 필요한 개수보다 작으면 `PIXELICA_ERR_CAPACITY`이며 표면은 그대로입니다.
- **겹침(alias)**: scratch가 출력 표면, 셰이더 구조체, 표면 구조체, 팔레트와 겹치면 `PIXELICA_ERR_INVALID_ARG`입니다.

## 5. 2D 캔버스에서 바로 쓰기 (direct buffer)

2D 렌더러(`PIXELICA2D_Context`)의 색 버퍼는 호출자가 소유한 RGB565 표면입니다. 장면을 다 그린 뒤 **같은 버퍼**를 `PIXELICA_ShaderSurface`로 넘기면 복사 없이 후처리됩니다.
셰이더 모듈은 렌더러 헤더를 포함하지 않으므로 필드를 직접 옮깁니다.

```c
#include "pixelica/renderer2d.h"
#include "pixelica/shaders.h"

static uint16_t color[160 * 120];
static uint16_t scratch[160 * 120];

PIXELICA2D_Surface target = { color, 160, 120, 160, NULL, 0 };
PIXELICA2D_Context ctx;
pixelica2d_init(&ctx, &target);

/* ... pixelica2d_clear / draw_sprite 등으로 장면을 그림 ... */

PIXELICA_ShaderSurface surface = { ctx.surface.pixels, ctx.surface.width, ctx.surface.height, ctx.surface.stride };
PIXELICA_Shader blur;
pixelica_shader_default(PIXELICA_SHADER_BLUR, &blur);
pixelica_shader_apply(&surface, &blur, scratch, 160 * 120);
```

- 깊이 버퍼(`depth`)는 건드리지 않습니다. 후처리 뒤에 같은 프레임에 다시 그리면 깊이 검사 결과가 후처리된 색과 어긋날 수 있으니 **프레임의 마지막 단계**로 적용하세요.
- scratch는 색 버퍼와 별도 메모리여야 합니다(겹치면 거부).
- 행 패딩(`stride > width`)이 있어도 안전합니다.

## 6. 효과 연결 (수동 체이닝)

한 번의 `apply`는 효과 하나만 적용합니다. 여러 효과는 **순서대로 직접 호출**합니다. 각 단계가 끝나면 다음 단계가 그 결과를 입력으로 받습니다.

```c
PIXELICA_Shader chain[3];
pixelica_shader_default(PIXELICA_SHADER_BLUR, &chain[0]);
pixelica_shader_default(PIXELICA_SHADER_POSTERIZE, &chain[1]);
pixelica_shader_default(PIXELICA_SHADER_SCANLINES, &chain[2]);
chain[2].amount = 0.4f;

for (int i = 0; i < 3; ++i) {
    size_t need = pixelica_shader_scratch_pixels(&surface, &chain[i]);
    if (pixelica_shader_apply(&surface, &chain[i], need != 0 ? scratch : NULL, need) != PIXELICA_OK) {
        break;   /* 실패한 단계는 표면을 바꾸지 않음. 앞 단계 결과는 남아 있음 */
    }
}
```

순서에 따라 결과가 다릅니다(예: 흐림 뒤 포스터화와 포스터화 뒤 흐림). 프레젠터(8절)는 셰이더를 하나만 가지므로, 연결이 필요하면 직접 `apply`를 부르는 래퍼 PAL을 만들거나 프레젠터를 하나 더 감싸세요.

## 7. 사용자 정의 콜백 샘플

콜백은 `(x, y)`의 현재 색 `pixel->color`와, **적용 전 불변 스냅샷** `pixel->source`를 받습니다. 스냅샷은 `pixelica_shader_sample`로 읽습니다(좌표는 가장자리로 고정되고 음수도 허용).

```c
typedef struct Smear { uint32_t calls; } Smear;

/* 초록은 그대로, 빨강·파랑은 왼쪽 3픽셀의 색에서 가져옴 */
static uint16_t smear(const PIXELICA_ShaderPixel *pixel, void *userdata)
{
    Smear *state = (Smear *)userdata;
    uint16_t left = pixelica_shader_sample(pixel->source, (int32_t)pixel->x - 3, (int32_t)pixel->y);

    ++state->calls;
    return (uint16_t)((pixel->color & 0x07E0u) | (left & 0xF81Fu));
}

Smear state = {0};
PIXELICA_Shader shader;
pixelica_shader_default(PIXELICA_SHADER_CUSTOM, &shader);
shader.callback = smear;
shader.userdata = &state;
pixelica_shader_apply(&surface, &shader, scratch, 160 * 120);   /* CUSTOM은 scratch 필요 */
```

- 콜백은 **표면에 직접 쓰지 마세요**. 반환값만이 결과입니다.
- `userdata`가 가리키는 메모리의 수명·유효성은 호출자 책임입니다. 콜백이 불러야 할 횟수는 `width × height`입니다.
- 콜백이 느리면 프레임 시간이 그대로 늘어납니다(10절).
- 이 절의 코드는 호출 순서를 보여 주는 조각입니다.

## 8. PAL 프레젠터: 3D에서 쓰기 (pixelica_create 전에 감싸기)

3D 엔진은 `pixelica_end_frame` 안에서 PAL의 `present_rgb565`를 **프레임당 정확히 한 번** 부릅니다. 프레젠터는 진짜 PAL을 감싸는 새 `PIXELICA_PlatformOps`를 만들어,
그 안에서 프레임을 복사하고 셰이더를 적용한 뒤 진짜 PAL로 넘깁니다. 렌더러에 의존하지 않으므로 3D, 2.5D(hybrid), 2D 어디서나 같습니다.

```c
typedef struct PIXELICA_ShaderPresenter {
    PIXELICA_PlatformOps upstream, ops;   /* 진짜 PAL의 복사본, 래퍼 */
    PIXELICA_Shader shader;               /* 프레임 사이에 바꿀 수 있음 */
    uint16_t *output, *scratch;
    size_t output_capacity, scratch_capacity;
} PIXELICA_ShaderPresenter;

PIXELICA_Status pixelica_shader_presenter_init(PIXELICA_ShaderPresenter *out, const PIXELICA_PlatformOps *upstream,
    const PIXELICA_Shader *shader, uint16_t *output, size_t output_capacity,
    uint16_t *scratch, size_t scratch_capacity);
```

```c
#define W 320
#define H 240
static uint16_t output[W * H];
static uint16_t scratch[W * H];            /* BLUR/OUTLINE/CUSTOM이 아니면 NULL, 0 */
static PIXELICA_ShaderPresenter presenter; /* 컨텍스트보다 오래 살아야 함 */

PIXELICA_PlatformOps real = /* 진짜 SDL/보드 PAL */;
PIXELICA_Shader shader;
pixelica_shader_default(PIXELICA_SHADER_SCANLINES, &shader);
pixelica_shader_presenter_init(&presenter, &real, &shader, output, W * H, scratch, W * H);

PIXELICA_Config cfg = { W, H, 64, 256, 256u * PIXELICA_ARENA_BYTES_PER_VERTEX };
PIXELICA_Context *ctx;
pixelica_create(&cfg, &presenter.ops, &ctx);   /* 진짜 PAL 대신 presenter.ops 를 넘김 */
```

- **반드시 `pixelica_create`(또는 `pixelica_create_in_buffer`) 전에** 초기화하고 `presenter.ops`를 넘깁니다. 엔진은 테이블을 값으로 복사하지만 `userdata`(= 프레젠터 주소)는 `pixelica_destroy`까지 유효해야 합니다.
  프레젠터를 스택에 두었다가 함수가 끝나면 안 됩니다.
- **전달 규칙**: `time_us`, `read_buttons`, `log`는 그대로 진짜 PAL의 `userdata`로 전달합니다. 진짜 PAL은 `time_us`, `read_buttons`, `present_rgb565`가 모두 있어야 합니다(`log`는 선택).
- **present 동작**: 들어온 프레임이 올바른지(크기·stride·포인터), 출력·scratch 용량이 충분한지, 겹치지 않는지를 **매번** 검사한 뒤, 원본 프레임은 **바꾸지 않고** 빽빽한(tight, `stride == width`) `output`에 복사해 셰이더를 적용하고 진짜 PAL을 **한 번만** 호출합니다.
  진짜 PAL에는 `output` 포인터와 `stride = width`가 전달됩니다.
- **실패**: 잘못된 프레임·효과·용량·겹침이면 `false`를 돌려주고 **진짜 PAL을 부르지 않으며 `output`도 바꾸지 않습니다**(엔진은 `PIXELICA_ERR_PLATFORM`을 받음). 진짜 PAL이 `false`를 돌려주면 그 값을 그대로 전달합니다.
- **프레임 사이에 셰이더 바꾸기**: `presenter.shader`는 공개 필드이며 매 프레임 다시 검사합니다. 별칭·효과·팔레트·scratch 포인터도 마찬가지입니다. 초기화 때는 치수를 알 수 없어서 `present` 때 처음으로 치수를 검사합니다.
- **입력 프레임**은 프레젠터가 소유한 `output`, `scratch`, 프레젠터 자체와 겹치면 안 됩니다.
- **제공한 배열 전체 검사**: 초기화·present 모두 `output_capacity`, `scratch_capacity` 전체 범위를 검사합니다.
  현재 효과가 scratch를 쓰지 않아도 제공된 scratch는 다른 버퍼·팔레트와 겹치면 안 됩니다. 필요 없으면 `NULL, 0`을 넘기세요.
- **용량**: `output_capacity >= width × height`. BLUR·OUTLINE·CUSTOM은 `scratch_capacity >= width × height`가 추가로 필요합니다. 초기화에서는 출력 용량이 비어 있지 않은지와, 포인터·개수 계산 오버플로, 출력/업스트림/셰이더/scratch/팔레트의 겹침을 검사하며, 실패하면 `out`을 바꾸지 않습니다.
- **단일 스레드**: `present` 호출은 엔진을 호출하는 스레드에서만 일어납니다. 프레젠터와 진짜 PAL의 수명은 컨텍스트보다 길어야 합니다.

2.5D(hybrid)에서는 `pixelica2d_bind_3d`가 만든 2D 뷰를 쓰는 HUD까지 그린 뒤 `pixelica_end_frame`이 한 번 present하므로, 위 방식이 HUD와 3D 장면 전체에 적용됩니다.

## 9. 설치

### 9.1 이미 받은 SDK에 추가 ZIP 풀기 (Windows)

기본 SDK는 셰이더를 포함하지 않습니다. 셰이더는 별도 **추가 ZIP**입니다.

1. 기본 SDK(`Pixelica-<버전>/`, common 포함)를 먼저 풉니다.
2. `PixelicaShaders-0.1.0-windows-AMD64.zip`을 같은 상위 폴더에 풉니다(MSVC 2022 x64). 기본 SDK와 컴파일러·아키텍처·Debug/Release 설정이 같아야 합니다.
3. 사용할 프로젝트에서 SDK 접두사를 지정합니다.

```cmake
find_package(PixelicaShaders CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE Pixelica::shaders)
```

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/path/to/Pixelica-0.1.0
```

배포 담당자가 만든 파일은 `out/pixelica-shaders-release`에 `PixelicaShaders-0.1.0-windows-AMD64.zip`과 `PixelicaShaders-0.1.0-source.zip`으로 놓입니다.

### 9.2 소스 ZIP

다른 컴파일러·플랫폼에서는 `PixelicaShaders-0.1.0-source.zip`을 풀고 설치된 기본 SDK를 지정해 빌드합니다. 소스 ZIP의 독립 CMake 프로젝트는
`find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS common)`으로 **common 라이브러리**를 찾습니다. 기본 엔진 소스나 SDL은 받지 않습니다.

```bash
cmake -S PixelicaShaders-0.1.0-source -B build-shaders -DCMAKE_PREFIX_PATH=/path/to/Pixelica-0.1.0
cmake --build build-shaders --config Release
cmake --install build-shaders --config Release --prefix /path/to/Pixelica-0.1.0
```

`-DPIXELICA_SHADERS_BUILD_EXAMPLE=ON`으로 소스 ZIP의 예제도 빌드할 수 있습니다.
컴파일러·아키텍처·Debug/Release 설정은 기본 SDK와 맞춰야 합니다. 별도 CMake 패키지 `PixelicaShaders`를
사용하므로 `find_package(Pixelica COMPONENTS shaders)`로 요청하지 않습니다. 기존 파티클 ZIP과 설치 순서를 바꾸어도 기본 로더를 교체하지 않습니다.

### 9.3 소스 트리에서 빌드

```bash
cmake -S . -B build-shaders -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_SHADERS=ON
cmake --build build-shaders --config Release
ctest --test-dir build-shaders -C Release --output-on-failure
```

`PIXELICA_BUILD_SHADERS`의 기본값은 `OFF`입니다. 다른 선택 모듈(오디오, 파티클)과 독립적이며 서로의 설정에 영향을 주지 않습니다.

## 10. 성능과 한계

- **전부 CPU**입니다. GPU, SIMD 전용 경로, 멀티스레딩은 없습니다. `GLSL`/`HLSL`/`SPIR-V` 같은 셰이더 언어를 읽거나 컴파일하지도 않습니다.
- 비용은 대략 **픽셀 수에 비례**합니다. 해상도가 두 배(가로·세로)면 일은 4배입니다. 효과마다 픽셀당 비용이 다릅니다.
  - 가벼움: `TINT`, `GRAYSCALE`, `INVERT`, `POSTERIZE`, `SCANLINES`, `VIGNETTE`.
  - `PALETTE`: 팔레트 크기(최대 256)에 비례(픽셀마다 전체 팔레트와 거리 계산).
  - `BLUR`: `(2·radius+1)²`에 비례하며 `radius = 4`면 픽셀당 81번 읽습니다. 스냅샷 복사(`width × height` 쓰기)가 추가됩니다.
  - `OUTLINE`: 픽셀당 이웃 4개를 읽습니다. 스냅샷 복사가 추가됩니다.
  - `CUSTOM`: 콜백의 비용이 지배합니다(함수 포인터 호출이 픽셀마다 한 번).
- 프레젠터는 프레임을 `output`으로 **한 번 복사**한 뒤 효과를 적용하므로, `amount=0`인 색 효과도 프레임당 `width × height` 복사 비용이 듭니다.
- 마이크로컨트롤러 같은 작은 보드에서는 `output`(과 scratch)이 각각 `width × height × 2`바이트의 추가 RAM입니다. 프로파일링으로 프레임 예산 안에 들어가는지 **대상 하드웨어에서 직접 측정**하세요. 이 문서는 속도를 보증하지 않습니다.
- 색 정밀도: 내부 계산은 RGB8이지만 결과는 다시 RGB565로 줄어듭니다. 효과를 여러 번 연결하면 단계마다 반올림 오차가 쌓이고, 느슨한 그라데이션에서 줄무늬(밴딩)가 보일 수 있습니다.
- 입력과 출력이 **같은 표면**인 제자리 처리만 지원합니다(`BLUR`/`OUTLINE`/`CUSTOM`은 스냅샷으로 정확성을 확보). 입력/출력 이미지를 따로 주는 API는 없습니다.
- 최대 크기는 4096×4096입니다. 알파·가산 혼합·텍스처 필터링·3D 인식(깊이 기반 효과)은 없습니다.
- 부동소수점(`amount`)이 들어가는 효과의 비트 단위 결과는 컴파일러·플랫폼에 따라 다를 수 있으므로 해시는 같은 빌드끼리만 비교하세요.

## 11. 예제·검증 범위

- `hello_shaders`: 160×120 장면에 열 효과를 적용한 5×2 시트를 두 번 만들어 해시가 같은지, 각 패널이 원본·다른 패널과 다른지, 콜백 호출 횟수가 픽셀 수와 같은지 검사합니다. 렌더러·SDL이 필요 없습니다.
- `tests/test_shaders_presenter.c`: 가짜 PAL로 `userdata`·시간·버튼·로그 전달, present 정확히 한 번, 원본 프레임 보존, 빽빽한 출력 stride,
  용량·효과·프레임 오류 시 무변경·무전달, 업스트림 `false` 전달, 프레임 사이 셰이더 변경과 팔레트·scratch 겹침 거부, 사용자 정의 콜백의 불변 스냅샷을 검사합니다.
- 실제 보드 검증과 GPU 경로는 없습니다. PC 검증 결과는 아래에 기록합니다.

### 11.1 로컬 검증 (2026-10-10)

| 환경 | 구성 | 결과 |
|---|---|---|
| Windows x64, MSVC 19.44 | Release, 2D+3D 및 기존 오디오·파티클·리깅·입력 모듈, 경고를 오류로 처리 | CTest 48/48 |
| Linux x64, GCC 13.3 | Debug, 같은 모듈, ASan+UBSan, 경고를 오류로 처리 | CTest 43/43 |
| Linux, ASan+UBSan | 2D만 / 3D만 / 렌더러 없음 + 쉐이더 | 17/17 · 28/28 · 14/14 |
| Linux, 기본 옵션 OFF | 렌더러·선택 모듈 없음, 설치 파일 검사 | 10/10, 쉐이더 헤더·라이브러리·설정 없음 |
| Python 3.10 | 패키징 단위 검사 전체 | 44개 중 42개 성공, 환경 선택 검사 2개 건너뜀 |

버퍼 전체 범위·주소 오버플로 회귀 검사는 추가 후 Windows Release와 Linux ASan+UBSan에서 다시 통과했습니다.
예제의 로컬 비교 시트 해시는 `0x06efd59ecfb722a2`입니다. 고정 호환성 보증이나 성능 측정값으로 사용하지 마세요.
GitHub Actions 실행, ESP32-P4 실보드 속도·메모리 검증은 포함하지 않습니다.
