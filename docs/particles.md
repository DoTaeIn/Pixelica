# 선택 모듈: 파티클 (Particles)

> 불꽃·연기·비 같은 효과를 만드는 **메모리를 할당하지 않는 고정 풀 파티클 시뮬레이션**과, 이를 2D 스프라이트 또는 3D 위치의 빌보드로 그리는 **선택 어댑터**입니다.
> 기본값은 `OFF`이며 기본 SDK ZIP에는 들어 있지 않습니다. 이 문서는 **계약과 사용법**을 설명합니다.
> 검증 구성은 마지막 절을 참고하세요. 실제 보드 실행은 아직 확인하지 않았습니다.

| 항목 | 값 |
|---|---|
| 옵션 | `PIXELICA_BUILD_PARTICLES` (기본 `OFF`) |
| 시뮬레이션 | `pixelica_particles` / `Pixelica::particles` — 헤더 [`pixelica/particles.h`](../modules/particles/include/pixelica/particles.h). common만 필요, 렌더러 헤더·의존성 없음, 힙 사용 없음 |
| 2D 어댑터 | `Pixelica::particles2d` — 헤더 [`pixelica/particles2d.h`](../modules/particles/include/pixelica/particles2d.h). 2D 렌더러(`Pixelica::renderer2d`)가 있을 때만 만들어집니다 |
| 2.5D 어댑터 | `Pixelica::particles25d` — 헤더 [`pixelica/particles25d.h`](../modules/particles/include/pixelica/particles25d.h). **기본 3D SDK와 2D SDK가 모두** 있어야 합니다(hybrid 모듈 기반) |
| 설치 컴포넌트 | `particles`, `particles2d`, `particles25d` (각각 별도 export) |
| 예제 | `hello_particles` ([`examples/hello_particles/main.c`](../examples/hello_particles/main.c)), 창·SDL 없이 오프스크린 |

## 1. 빠른 시작

### 1.1 예제 실행

```bash
cmake -S . -B build-particles -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_PARTICLES=ON
cmake --build build-particles --config Release
./build-particles/hello_particles --output frame.ppm
```

`hello_particles`는 160×120 화면에 불꽃(색 있는 사각형), 연기(코드로 만든 8×8 스프라이트), 비를 그리고
마지막 프레임을 PPM(P6)으로 저장합니다. 같은 시뮬레이션을 두 번 돌려 두 프레임의 체크섬·생존 개수·탈락 개수가 같은지 확인합니다.
`--ticks N`(1~3600, 기본 90)으로 시뮬레이션 길이를 바꿉니다. 체크섬은 같은 빌드(컴파일러·옵션·플랫폼)끼리만 비교하세요.
Visual Studio에서는 `.\build-particles\Release\hello_particles.exe`처럼 구성 폴더 안에서 실행해야 할 수 있습니다.

### 1.2 최소 코드 (2D)

아래는 호출 순서를 보여 주는 조각입니다. 실제 게임은 각 `PIXELICA_Status`를 확인하세요. 전체 예는 `hello_particles/main.c`입니다.

```c
#include "pixelica/particles2d.h"            /* particles.h와 renderer2d.h를 함께 가져옵니다 */

static PIXELICA_Particle storage[128];        /* 호출자가 소유하는 고정 풀 */
static PIXELICA_ParticleEmitter sparks;

PIXELICA_ParticleConfig config;
pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, true, &config);   /* true = 2D 화면(+Y 아래) */
config.position.x = 80.0f;
config.position.y = 100.0f;
pixelica_particles_init(&sparks, storage, 128, &config, 1234u);         /* 마지막 인자: 난수 시드 */

/* 매 프레임 */
pixelica_particles_update(&sparks, 1.0f / 60.0f);
pixelica2d_particles_draw(&ctx, &sparks, NULL);                         /* NULL = 색 있는 사각형 */

/* 한 번에 터뜨리기 */
pixelica_particles_burst(&sparks, 24, NULL);
```

### 1.3 최소 코드 (2.5D, 3D 장면 안)

```c
#include "pixelica/particles25d.h"

static PIXELICA25D_ParticleSortItem scratch[128];   /* 풀 크기 이상, 호출자 소유 */

pixelica_begin_frame(ctx, clear_color);
pixelica_set_view_projection(ctx, vp);
/* ... 불투명 3D 메쉬를 먼저 그립니다 ... */
pixelica2d_bind_3d(&canvas, ctx);
pixelica25d_particles_draw(&canvas, vp, &emitter, NULL, scratch, 128);
pixelica_end_frame(ctx);
```

## 2. 시뮬레이션 모델

- **고정 풀**: 호출자가 `PIXELICA_Particle` 배열(용량 1~4096)을 주고 에미터가 그 안만 씁니다. 풀이 가득 차면 새 파티클을 **버리고** `dropped`에 셉니다.
  살아 있는 파티클을 덮어쓰지 않으며 이때도 오류가 아니라 성공입니다. 필요한 최대 동시 개수는 대략 `rate × lifetime_max`이므로 그 값에 여유를 더해 풀 크기를 정하세요.
- **살아 있는 파티클은 `particles[0 .. count)`에 빽빽하게** 있습니다. 수명이 끝나면 마지막 파티클이 빈자리로 옮겨 오는(swap removal) 방식이라 **순서가 일정하지 않습니다**.
- **결정성**: 에미터가 자체 시드 생성기를 가지며 전역 `rand`를 쓰지 않습니다. 같은 컴파일러·옵션·플랫폼에서 같은 시드, 설정, **같은 `dt` 순서의 같은 호출**이면 결과가 같습니다.
  `dt`를 다르게 쪼개면 결과도 달라집니다(방출이 틱 단위, 프레임레이트 독립 아님). 시드 `0`도 사용할 수 있습니다.
- **`dt`**: `update`는 `[0, 1]`초의 유한한 값만 받습니다. 1초보다 긴 간격(예: 일시정지 후 복귀)은 **직접 여러 번 나눠서** 호출하세요.
  틱마다 먼저 기존 파티클을 적분하고 만료된 것을 제거한 뒤, **틱 끝에서** `rate × dt` 만큼(소수 부분은 다음 틱으로 이월) 새로 만듭니다. 새로 만든 파티클은 그 틱에서 이동하지 않습니다.
- **단위**: 위치·속도·크기는 호출자 세계의 단위입니다(2D는 세계 픽셀, 3D는 세계 단위). 시간은 초입니다. 좌표는 XYZ이며 2D 어댑터는 X/Y만 사용합니다.
- **소유권**: 에미터 구조체와 저장소는 호출자 것입니다. 공개 필드는 읽기 전용이며 `emitting`만 직접 바꿀 수 있습니다(방출 켜기/끄기). 위치는 `pixelica_particles_move`로 옮기고,
  이미 살아 있는 파티클은 따라 움직이지 않습니다. 단일 스레드용입니다.
- **오류**: 잘못된 인자는 `PIXELICA_ERR_INVALID_ARG`이며 출력과 에미터를 건드리지 않습니다. 설정은 모든 값이 유한하고 한계 안이어야 합니다
  (위치·속도·중력·크기·회전 ≤ `PIXELICA_PARTICLES_MAX_VALUE`(1e6), 수명 ≤ 3600초, `rate` ≤ 10000/초).

| 함수 | 역할 |
|---|---|
| `pixelica_particles_config_default` | 중립 설정으로 채움 |
| `pixelica_particles_preset` | `SPARKS` / `SMOKE` / `RAIN` 설정 (`y_down`으로 Y 방향 선택) |
| `pixelica_particles_init` | 저장소·설정·시드 연결 (`out`은 초기화되지 않은 메모리여도 됨) |
| `pixelica_particles_move` | 앞으로의 생성 위치 변경 |
| `pixelica_particles_burst` | 즉시 N개 생성 (`emitting`·`rate`와 무관). 못 만든 수는 `dropped`에 더함 |
| `pixelica_particles_update` | 한 틱 진행 |
| `pixelica_particles_clear` | 살아 있는 파티클과 이월된 방출량 제거 |

## 3. 프리셋과 좌표 방향

프리셋은 **2D 세계 픽셀 크기**에 맞춰져 있습니다. 3D 세계에서는 속도·중력·크기를 환경에 맞게 곱해 쓰세요.
값은 `y_down = false`(+Y 위, 3D 규약) 기준이며 `y_down = true`(2D 화면 규약)는 Y를 거울상으로 뒤집습니다
(속도 Y 범위는 `[-max.y, -min.y]`, 중력 Y는 부호 반전). 위치는 0입니다.

| 프리셋 | 속도 (min ~ max) | 중력 Y | rate | 수명(초) | 크기 | 색 / 불투명도 |
|---|---|---|---|---|---|---|
| `SPARKS` | (-80,20,-80) ~ (80,120,80) | -240 | 60 | 0.4~0.9 | 2 → 1 | 노랑 `0xFFE0` → 빨강 `0xF800` / 255 → 0 |
| `SMOKE` | (-10,15,-10) ~ (10,35,10) | 6 | 12 | 1.5~3 | 4 → 14, 회전 0~2π, 각속도 0.5 | 회색 `0x8410` → 밝은 회색 `0xC618` / 160 → 0 |
| `RAIN` | (-30,-260,-30) ~ (30,-200,30) | -60 | 120 | 0.6~1 | 1 → 1 | 파랑 `0x9DFF` → `0x5C1F` / 200 → 120 |

(위 표는 `y_down = false` 값입니다.) 2D 화면에서 불꽃과 연기는 위로, 비는 아래로 움직여야 하므로 2D에서는 `y_down = true`를 쓰세요.
알 수 없는 프리셋 값이나 `NULL` 출력은 `PIXELICA_ERR_INVALID_ARG`입니다.

## 4. 2D 어댑터 (`particles2d`)

```c
PIXELICA_Status pixelica2d_particles_draw(PIXELICA2D_Context *ctx,
    const PIXELICA_ParticleEmitter *emitter, const PIXELICA2D_Sprite *appearance);
```

- 파티클 하나가 `pixelica2d_draw_sprite` 한 번입니다. X/Y는 **2D 세계 픽셀**이며 컨텍스트 카메라(위치·확대)와 클립이 그대로 적용됩니다. Z는 무시합니다. 확대하면 파티클 크기도 같이 커집니다.
- `size`는 **세계 픽셀 단위 한 변의 길이**입니다. `rotation`은 라디안이고 양수가 화면에서 시계 방향이며 파티클 위치를 중심으로 돕니다.
- `appearance == NULL`이면 파티클마다 현재 색(RGB565)의 1×1 텍셀을 `size × size`로 늘린 **색 있는 사각형**을 그립니다. 중심이 파티클 위치입니다.
- `appearance`를 주면 그 스프라이트의 텍스처·`source`·`pivot`·컬러 키·`depth`·`depth_test`·`depth_write`를 씁니다.
  위치, 크기, 불투명도는 파티클마다 바뀌고 `scale`은 **부호만** 사용합니다(음수 = 반전). `source`는 `size × size`로 늘어나므로 정사각형 프레임을 권장합니다.
  `appearance.rotation`은 파티클 회전에 더해지고 `appearance.opacity`는 파티클 불투명도에 곱해집니다(`(a×b+127)/255`).
  `pivot`은 `source` 안 텍셀 좌표이므로 가운데에 놓으려면 프레임 중심을 직접 지정하세요.
- **파티클 색은 색 있는 사각형에만 쓰입니다.** 텍스처에 색을 입히는(tint) API는 없습니다.
- `size`가 0이거나 최종 불투명도가 0인 파티클은 그리지 않으며 draw call도 세지 않습니다. 그린 파티클은 `ctx->stats.draw_calls`를 하나씩 늘립니다.
- 그리기 순서는 **풀 순서**입니다. 수명이 끝나면 순서가 바뀌므로 서로 겹치는 반투명 파티클의 위아래가 프레임마다 달라질 수 있습니다.
  정렬이 필요하면 2.5D 어댑터를 쓰거나, 불투명/드문 파티클로 설계하세요. 이 어댑터는 임시 메모리를 받지 않습니다.
- 모든 입력(에미터 크기·개수, 각 파티클의 위치·회전·크기, `appearance`의 텍스처·`source`·`scale`·`pivot`·`rotation`·깊이 값, 깊이 플래그에 대한 깊이 버퍼)을
  **그리기 전에** 검사합니다. 잘못되면 `PIXELICA_ERR_INVALID_ARG`이고 색·깊이·통계가 그대로입니다.
  변조된 컨텍스트는 첫 그리기에서 발견됩니다(그릴 파티클이 없으면 검사하지 않습니다).

## 5. 2.5D 어댑터 (`particles25d`)

> **기본 3D SDK와 2D SDK가 모두 필요합니다.** 이 어댑터는 `pixelica/hybrid.h`를 쓰므로 2D 전용·3D 전용 SDK에는 들어 있지 않습니다.
> 시뮬레이션 코어(`Pixelica::particles`)는 어느 쪽에도 의존하지 않습니다.

```c
typedef struct PIXELICA25D_ParticleOptions {
    const PIXELICA2D_Sprite *appearance; /* NULL = 색 있는 사각형 */
    bool depth_write;                    /* 기본 false */
} PIXELICA25D_ParticleOptions;

PIXELICA_Status pixelica25d_particles_draw(PIXELICA2D_Context *canvas, const float view_projection[16],
    const PIXELICA_ParticleEmitter *emitter, const PIXELICA25D_ParticleOptions *options,
    PIXELICA25D_ParticleSortItem *scratch, size_t scratch_count);
```

- 프레임 순서는 `hybrid.h`와 같습니다: `pixelica_begin_frame` → 불투명 3D 메쉬 → `pixelica2d_bind_3d` → 파티클 → HUD → `pixelica_end_frame`.
  `canvas`는 `pixelica2d_bind_3d`로 만든 뷰이거나 깊이 버퍼가 있는 2D 컨텍스트여야 합니다. `options`가 `NULL`이거나 0으로 초기화되어 있으면 기본값입니다.
- 위치는 **3D 세계 단위**(+Y 위, 카메라는 -Z를 봄)이며 3D 컨텍스트에 준 것과 같은 `view_projection`으로 투영합니다.
  `size`는 세계 단위 한 변의 길이이고 투영 시 화면 픽셀로 바뀝니다. 회전은 **화면 공간** 라디안(양수 = 시계 방향)입니다. 캔버스 카메라는 무시하고 클립은 적용합니다.
- 카메라 뒤쪽이거나 near/far 평면 밖인 파티클은 조용히 건너뜁니다(성공, draw call 없음).
- **깊이**: 빌보드는 **항상 깊이 검사**를 합니다(`hybrid.h`가 강제). 불투명 메쉬 뒤의 파티클은 가려집니다. 비교는 16비트 깊이 버퍼에 대한 엄격한 `<`이므로
  표면과 거의 같은 거리의 파티클은 깜빡일 수 있습니다. 눈에 띄게 간격을 두세요. 반투명 파티클에 맞게 **깊이는 기본으로 쓰지 않습니다**
  (`depth_write = false`). 불투명 파티클이 뒤에 그려지는 것을 가려야 하면 `options->depth_write = true`로 켜세요(`appearance.depth_write`보다 우선).
- **정렬**: 한 번의 호출 안에서 파티클을 투영 깊이(16비트로 양자화) 기준 **먼 것부터 가까운 것 순서**로 그립니다. 같은 깊이는 풀 순서입니다.
  정렬은 힙 정렬(O(n log n))이며 호출자가 주는 `scratch`(파티클 수 이상 개수의 `PIXELICA25D_ParticleSortItem`)를 씁니다. 할당하지 않습니다.
  **에미터끼리, 또는 다른 빌보드·스프라이트와는 정렬하지 않습니다.** 에미터마다 한 번씩 먼 것부터 직접 호출하세요.
- `scratch`가 작거나 `NULL`이면 `PIXELICA_ERR_NO_MEMORY`입니다(살아 있는 파티클이 없으면 `NULL`도 됩니다). `scratch`가 에미터·풀·캔버스·행렬·옵션·텍스처 같은 입력 데이터와 겹치면 `PIXELICA_ERR_INVALID_ARG`입니다.
- `appearance`는 2D와 같게 쓰입니다(`source`는 투영된 `size × size`로 늘어남, `scale`은 부호만, 회전 가산, 불투명도 곱). 위치·깊이·`depth_test`·`depth_write`는 무시합니다.
- 입력은 투영까지 모두 **그리기 전에** 검사합니다. 비정상 값(NaN 위치, 범위 밖 크기, 비유한 행렬, 깊이 버퍼 없는 캔버스, 잘못된 `appearance`)이나
  투영된 크기가 `PIXELICA25D_PARTICLES_MAX_PROJECTED_SIZE`(1e9 픽셀)를 넘는 파티클이 하나라도 있으면 `PIXELICA_ERR_INVALID_ARG`이고 아무것도 그리지 않습니다.

## 6. 재질·렌더링 한계 (정확히 알아 두세요)

| 항목 | 지원 여부 |
|---|---|
| 블렌딩 | 렌더러의 일반 알파 블렌드만. 가산(additive)·곱(multiply) 모드 **없음**. 8비트 불투명도를 RGB565 채널마다 정수로 계산하므로 부드러운 페이드에 계단이 보임 |
| 색 | 파티클 색은 **색 있는 사각형에만** 적용. 텍스처 tint **없음** |
| 텍스처 | 텍셀 단위 알파 **없음**(컬러 키 + 파티클 전체 불투명도만). 최근접 샘플링, 필터링 없음. 1픽셀보다 작은 사각형은 픽셀 중심 사이로 빠져 **아무것도 안 그려질 수 있음** |
| 회전 | 2D는 평면 회전. 2.5D는 화면 공간 회전만(빌보드는 항상 카메라를 향함, 3D 방향·원근 기울임 없음) |
| 정렬 | 2D: 없음(풀 순서, 불안정). 2.5D: 호출 한 번 안에서만 먼 것 → 가까운 것 |
| 3D 상호작용 | 깊이 검사만(딱딱한 경계). 조명, 소프트 파티클(메쉬와 부드럽게 겹침), 그림자 **없음** |
| 크기 | 2D는 세계 픽셀(확대에 비례), 2.5D는 세계 단위(투영 픽셀/단위로 환산) |
| 입자 수 | 풀 크기(최대 4096)가 상한. 상한은 비용이 아니라 개수의 제한 |

## 7. 성능: CPU 오버드로우

모든 그리기는 CPU 소프트웨어 래스터화입니다. 파티클 하나가 덮는 **모든 화면 픽셀**을 그 파티클마다 다시 그리므로,
크고 많이 겹치는 반투명 파티클(연기 같은)이 프레임 시간을 지배합니다. 풀 크기는 개수만 제한할 뿐 비용을 제한하지 않습니다.
2.5D에서는 카메라에 가까운 파티클이 특히 비쌉니다(투영 크기가 커짐). 큰 효과는 개수를 줄이거나 크기·불투명 구간을 줄여 조절하고,
클립 사각형을 좁히면 래스터 범위도 줄어듭니다. 정확한 비용은 대상 하드웨어에서 직접 측정하세요.

## 8. 설치

### 8.1 이미 받은 SDK에 추가 ZIP 풀기 (Windows)

기본 SDK는 파티클을 포함하지 않습니다(항상 `OFF`로 빌드). 파티클은 별도 **추가 ZIP**입니다
(`PixelicaParticles-<버전>-windows-AMD64.zip`, MSVC 2022 x64, Debug/Release 포함).

1. 기본 SDK(`Pixelica-<버전>/`)를 먼저 풉니다. 2D 어댑터는 2D SDK, 2.5D 어댑터는 2D와 3D가 합쳐진 SDK가 필요합니다.
2. 추가 ZIP을 **같은 상위 폴더에 마지막으로** 풀고 `PixelicaPackage.cmake` 교체를 허용합니다. 공통·렌더러 라이브러리는 교체되지 않습니다.
3. 컴파일러·아키텍처·Debug/Release 설정이 기본 SDK와 같아야 합니다.

배포 담당자가 직접 만들 때는 `tools/package_particles.py`를 사용합니다(문서 `docs/sdk.md`의 파티클 패키지 절). 빌드·설치 산출물은
`out/pixelica-particles-release` 아래에 생성됩니다. 패키지는 이 문서를 작성한 시점에 이 작업 안에서 만들거나 실행해 보지 않았습니다.

### 8.2 CMake 연결

```cmake
find_package(Pixelica CONFIG REQUIRED COMPONENTS particles2d)       # 2D 어댑터 (particles 시뮬레이션 포함)
target_link_libraries(my_game PRIVATE Pixelica::particles2d)

find_package(Pixelica CONFIG REQUIRED COMPONENTS particles25d)      # 2.5D 어댑터 (2D + 3D SDK 필요)
target_link_libraries(my_game PRIVATE Pixelica::particles25d)

find_package(Pixelica CONFIG REQUIRED COMPONENTS particles)         # 시뮬레이션만 (렌더러 없이)
target_link_libraries(my_game PRIVATE Pixelica::particles)
```

사용할 때는 `-DCMAKE_PREFIX_PATH=<sdk-prefix>`를 지정합니다. 요청한 컴포넌트만 연결되므로 받은 파일이 더 많아도 필요한 기능만 쓰입니다.

### 8.3 소스 ZIP

다른 컴파일러·플랫폼에서는 `PixelicaParticles-<버전>-source.zip`을 풀고 설치된 기본 SDK를 지정해 빌드합니다.
소스 ZIP은 파티클 코드·문서·오프스크린 예제·독립 CMake 프로젝트만 담습니다. 기본 엔진 소스나 SDL을 다운로드하지 않습니다.

```sh
cmake -S PixelicaParticles-0.1.0-source -B build-addon \
  -DCMAKE_PREFIX_PATH=/path/to/Pixelica-0.1.0 \
  -DPIXELICA_PARTICLES_BUILD_2D=ON -DPIXELICA_PARTICLES_BUILD_EXAMPLE=ON
cmake --build build-addon --config Release
cmake --install build-addon --config Release --prefix /path/to/Pixelica-0.1.0
```

기본값은 시뮬레이션만 빌드합니다. `PIXELICA_PARTICLES_BUILD_2D=ON`은 2D 출력,
`PIXELICA_PARTICLES_BUILD_25D=ON`은 3D 공간 빌보드를 추가합니다. 후자는 먼저 두 기본 SDK를 합쳐야 합니다.
`PIXELICA_PARTICLES_BUILD_EXAMPLE=ON`은 `hello_particles`를 만듭니다(2D 옵션 필요).
게임에서 소스로 포함하려면 기본 SDK를 찾을 수 있게 `CMAKE_PREFIX_PATH`를 지정한 뒤 이 디렉터리를 `add_subdirectory` 하세요.
기본 SDK와 컴파일러·아키텍처·Debug/Release 설정을 맞추고, MSVC에서는 예제를 `build-addon/Release/hello_particles.exe`로 실행합니다.

### 8.4 소스 트리에서 빌드

```bash
cmake -S . -B build-particles -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_PARTICLES=ON
cmake --build build-particles --config Release
```

`Pixelica::particles2d`는 2D 렌더러가 켜져 있을 때(`PIXELICA_BUILD_2D=ON`), `Pixelica::particles25d`는 3D·2D가 모두 켜져 hybrid가 만들어질 때만 생깁니다.

## 9. 사용 팁

- 풀 크기: `rate × lifetime_max`보다 넉넉하게. 가득 차는 것이 이상하면 `emitter.dropped`가 늘어나는지 보세요.
- 효과를 켜고 끄기: 방출은 `emitter.emitting = false`로 멈추고(살아 있는 파티클은 계속 사라질 때까지 진행), 한 번에 터뜨릴 때는 `burst`를 씁니다.
- 일시정지: `update`를 부르지 않으면 정지합니다. 긴 공백 뒤에는 `dt`를 1초 이하로 나눠 호출하세요.
- 플레이어 위치 따라가기: 매 틱 `pixelica_particles_move`를 부릅니다. 이미 생긴 파티클은 따라오지 않고 새 파티클만 새 위치에서 시작합니다.
- 결정성이 필요하면 고정 `dt`(예: 1/60)로 `update`를 부르고 시드를 고정하세요. 재현은 같은 빌드에서만 보장됩니다.
- 3D 세계 규모에서는 프리셋 값을 그대로 쓰지 말고 속도·중력·크기를 세계 단위에 맞춰 스케일하세요.

## 10. 검증 범위와 한계

- `tests/test_particles_render.c`는 2D 어댑터(사각형 위치, 카메라·확대·클립, 회전, 불투명도, 텍스처·반전·컬러 키·불투명도 곱, 깊이 옵션, 풀 순서, 입력 오류 시 무변경, 프리셋 값과 결정성)를
  검사합니다. 2.5D 어댑터는 해당 타깃이 빌드되어 연결될 때만 추가로 검사합니다(깊이 가림, 원근 크기, 먼 것부터 정렬, `depth_write`, 범위 밖 건너뜀, scratch 오류, 입력 오류 시 무변경).
- 이 문서의 수치(프리셋 값 등)는 헤더의 주석과 같습니다. 실제 실행·성능·플랫폼별 결과는 이 문서가 보증하지 않습니다.
- 실제 보드·GPU 경로는 없습니다. 모든 그리기는 CPU입니다.


### 로컬 검증 결과 (2026-10-10)

| 환경·구성 | 결과 |
|---|---|
| Windows / MSVC 2022 x64 / Release / 2D+3D, 오디오·입력·Skeleton·IK·파티클 | CTest 43/43 |
| Linux / GCC 13.3 / Debug / 같은 구성 / ASan+UBSan | CTest 39/39 |
| Linux / Debug / 2D 전용 파티클 / ASan+UBSan | CTest 17/17 |
| Linux / Release / 3D+공통 파티클, 2D 제외 | CTest 19/19 (설치된 모듈 소비자 포함) |
| Linux / Debug / 렌더러 없는 공통 파티클 / ASan+UBSan | CTest 11/11 |
| Windows / Release / 기본 파티클 OFF, 렌더러 없음 | CTest 10/10; 설치에 파티클 헤더·라이브러리·export 없음 |

Windows 직접 실행에서 코어 4,449개, 렌더링 929개, 감사 회귀 16개 검사를 통과했습니다.
감사 회귀는 회전 합산 오버플로로 오류가 발생한 뒤 일부 화면이 남는 문제와, 정렬 scratch가 에미터 자체를 덮어쓰는 문제를
수정 전 재현하고 수정 후 통과한 검사입니다. 초기화 시 에미터와 풀의 중첩도 거부합니다.
예제는 90틱에서 생존 158개·탈락 0개, 프레임 체크섬 `0x7e8713170b0d2e59`를 출력했습니다(Windows 빌드 기준).
CI 구성에도 파티클을 추가했지만 GitHub Actions 실행 결과는 여기에 포함하지 않습니다. 실보드·실시간 성능 벤치마크는 수행하지 않았습니다.
