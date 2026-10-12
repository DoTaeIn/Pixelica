# 렌더 이펙트: 바이리니어 필터, 픽셀 훅, 월드 클립 평면

`pixelica_draw_mesh_effects()`는 `pixelica_draw_mesh_layers()`에 세 가지 선택 기능을
더한 진입점입니다. 내부적으로 같은 단일 드로우 구현을 사용하며, 기존 API의 동작과
출력은 비트 단위로 그대로입니다.

이 기능은 기본 3D 코어에 포함됩니다. 색·깊이 복사와 샘플링은 공통 라이브러리이므로
2D에서도 사용할 수 있습니다. 선택 쉐이더 패키지를 설치할 필요는 없습니다.

```c
PIXELICA_Status pixelica_draw_mesh_effects(PIXELICA_Context *ctx,
                                           const PIXELICA_Mesh *mesh,
                                           const float model[16],
                                           const PIXELICA_MaterialLayers *material,
                                           const PIXELICA_DrawOptions *options,
                                           const PIXELICA_MeshEffects *effects);
```

| 필드 | 0 값(기본) | 의미 |
| --- | --- | --- |
| `filter` | `PIXELICA_FILTER_NEAREST` | `BILINEAR`면 베이스 텍스처, `texture_alpha`, 모든 추가 레이어를 바이리니어 샘플 |
| `pixel` | `NULL` | 프래그먼트별 콜백 |
| `userdata` | — | 콜백에 그대로 전달(빌린 포인터, 엔진이 소유하지 않음) |
| `clip_planes` / `clip_plane_count` | `NULL` / 0 | 월드 공간 클립 평면 최대 `PIXELICA_MAX_WORLD_CLIP_PLANES`(4)개 |

- `effects == NULL`은 `pixelica_draw_mesh_layers(ctx, mesh, model, material, options)`와 정확히 같습니다.
- 0으로 초기화한 `PIXELICA_MeshEffects`도 같은 결과(NEAREST, 훅 없음, 클립 없음)입니다.
- 기존 `pixelica_draw_mesh*` 함수, `PIXELICA_MaterialLayers`, `PIXELICA_DrawOptions`의
  구조체 레이아웃과 기본 출력은 바뀌지 않았습니다. 프레임 아레나도 여전히
  정점당 `PIXELICA_ARENA_BYTES_PER_VERTEX`(20바이트)입니다.

## 1. 바이리니어 필터

`filter = PIXELICA_FILTER_BILINEAR`일 때 각 축(크기 `n`, 원근 보정된 좌표 `t`,
double로 계산, 비유한 값은 0)에 대해:

- **CLAMP**: `s = t*n - 0.5`, `i0 = floor(s)`, `f = s - i0`, 탭 `i0`, `i0+1`을 각각
  `[0, n-1]`로 클램프합니다(가장자리 텍셀이 반복됨).
- **REPEAT**: `t' = t - floor(t)`, `s = t'*n - 0.5`, 이후 동일하되 두 탭을 각각
  독립적으로 `[0, n-1]`로 감쌉니다. 음수 UV, 정수 UV, 아주 큰 유한 UV 모두 안전합니다.
  (예: `u = 0`이나 `u = 1`은 마지막 열과 첫 열을 반반 섞습니다.)

네 탭 `c00(i0,j0) c10(i1,j0) c01(i0,j1) c11(i1,j1)`의 RGB565 **네이티브** 채널(5/6/5)과
alpha8 각각을 double로

```
v = (c00*(1-fx) + c10*fx)*(1-fy) + (c01*(1-fx) + c11*fx)*fy
결과 = floor(v + 0.5)
```

로 섞습니다. 그 뒤 레이어 결합, 정점 RGBA 변조, 알파 계산은
[material_layers.md](material_layers.md)와 완전히 같습니다. NEAREST는 기존 경로를
그대로 사용합니다.

비용: 텍셀 4회 읽기 + 채널당 double 연산. 레이어 3개를 모두 쓰면 프래그먼트당
최대 16회 텍셀 읽기(알파 평면 포함 시 20회)입니다. FPU가 약한 MCU에서는 눈에 띄게
느려지므로 필요한 드로우에만 사용하세요.

## 2. 픽셀 훅 (`PIXELICA_PixelFn`)

```c
typedef struct PIXELICA_Fragment {
    uint16_t x, y;   /* 프레임버퍼 픽셀, row 0 = 위 */
    uint16_t depth;  /* 기록될 원시 uint16 깊이 */
    uint16_t color;  /* RGB565 */
    uint8_t alpha;
} PIXELICA_Fragment;

typedef bool (*PIXELICA_PixelFn)(void *userdata, const PIXELICA_Fragment *fragment,
                                 uint16_t *out_color, uint8_t *out_alpha);
```

호출 시점(프래그먼트 순서):

1. 커버리지 → **조기 깊이 테스트**(실패하면 샘플링도 콜백도 없음)
2. 머티리얼 색 → 레이어 → 정점 RGBA → 텍스처 알파 × 정점 알파 → opacity
3. **콜백**: `*out_color`, `*out_alpha`는 위 결과로 미리 채워져 있습니다
   (OPAQUE 모드에서 alpha는 255).
4. 알파 테스트(TEST) / 블렌드(BLEND) / 깊이+색 기록

반환값:

- `false`: 프래그먼트 폐기. 색도 깊이도 쓰지 않습니다(모든 알파 모드에서, OPAQUE 포함).
  `pixels_written`에도 세지 않습니다.
- `true`: `*out_color`를 사용. OPAQUE는 `*out_alpha`를 무시하고, TEST는 이 값으로
  cutoff를 비교하며, BLEND는 이 값으로 섞습니다(0이면 건너뜀).

### 깊이 값은 비선형입니다

`depth`는 NDC z를 `[0, 65534]`로 선형 매핑한 값으로, 원근 투영에서는 뷰 공간 거리에
대해 **비선형**입니다(가까운 쪽 해상도가 높음). 뷰 공간 거리가 필요하면 직접
복원하세요(OpenGL 스타일 투영, near `n`, far `f`):

```c
static float view_distance(uint16_t depth, float n, float f)
{
    float z_ndc = (float)depth / (float)PIXELICA_DEPTH_MAX_DRAWABLE * 2.0f - 1.0f;
    return (2.0f * n * f) / (f + n - z_ndc * (f - n));
}
```

### 콜백 제약

- 콜백이 실행되는 동안 **같은 컨텍스트**에 대한 변경/재진입은 금지입니다.
  같은 컨텍스트의 `pixelica_draw_mesh*` 호출은 아무 상태도 바꾸지 않고
  `PIXELICA_ERR_INVALID_ARG`로 거부됩니다(`last_error`도 갱신하지 않음).
  `pixelica_begin_frame`, `pixelica_end_frame`, `pixelica_set_view_projection`,
  `pixelica_set_cull_mode`, `pixelica_destroy`, `pixelica_update`도 거부됩니다.
  상태를 반환하는 함수는 `PIXELICA_ERR_INVALID_ARG`, void 함수는 아무 동작도 하지 않습니다.
  이미 바인딩한 2D 컨텍스트로 같은 버퍼에 쓰거나, 포인터를 직접 쓰거나,
  살아 있는 컨텍스트 메모리에 `pixelica_create_in_buffer`를 다시 호출하는 것은
  호출자 소유권 계약 위반이며 보호하지 않습니다.
- 같은 컨텍스트의 `pixelica_color_buffer()`/`pixelica_depth_buffer()`는 그리는
  중인 버퍼이므로 읽어도 결과가 드로우 순서에 의존합니다. 이전 패스의 결과가
  필요하면 **스냅샷**(별도 버퍼로 복사한 것)을 `userdata`로 넘기세요.
- 드로우에 넘긴 메시, 행렬, 머티리얼, 텍스처, 옵션, 이펙트 데이터를 콜백에서
  수정하지 마세요(클립 평면과 모델 행렬은 엔진이 내부 복사본을 쓰지만, 나머지는
  드로우 중에 계속 읽힙니다).
- 엔진은 타이머나 할당을 추가하지 않습니다. 콜백 안에서도 할당·블로킹 I/O는
  피하세요. 다른 독립 컨텍스트에 그리는 것은 허용됩니다.
- 비용: 깊이 테스트를 통과한 프래그먼트마다 간접 호출 1회. 320×240 화면 전체를
  덮으면 프레임당 약 7.7만 회입니다.

## 3. 월드 클립 평면

```c
typedef struct PIXELICA_WorldClipPlane {
    PIXELICA_Vec3 normal;   /* 유한, 0 벡터 불가, 정규화 불필요 */
    float distance;         /* 유한 */
} PIXELICA_WorldClipPlane;
```

월드 좌표 `p = model * vertex`가 `dot(normal, p) + distance >= 0`이면 남깁니다.

- 픽셀 단위 폐기가 아니라 **투영 전 실제 폴리곤 클리핑**입니다. UV, 레이어 UV,
  정점 RGBA는 교차점에서 위치와 같은 매개변수로 보간됩니다.
- 처리 순서: 절두체 trivial reject → 평면 trivial reject(세 정점이 한 평면 밖)
  → 면 방향 컬링 → 월드 평면 클리핑 → 절두체 클리핑.
- 통계: 평면 하나 밖에 완전히 있는 삼각형은 `triangles_rejected`, 월드 평면이나
  절두체로 실제 잘린 삼각형은 `triangles_clipped`에 한 번만 셉니다.
- 평면이 하나라도 있으면 `model`은 **아핀**이어야 합니다
  (`m[3] = m[7] = m[11] = 0`, `m[15] = 1`). 평면이 없으면 제한 없음.
- 클립된 폴리곤은 최대 3 + 4 + 7 = 14정점으로 스택 버퍼(16)에 들어갑니다.
  평면 거리는 삼각형마다 스택에서 다시 계산하므로 아레나 크기는 그대로입니다.
- 비용: 평면당 정점마다 내적 1회(삼각형당 3정점, 재계산), 잘리는 삼각형은 추가
  교차 계산.

## 4. 검증

`effects != NULL`이면 `material`이 NULL이어도 메시/모델/정점/인덱스 배열의
컨텍스트 메모리 겹침을 검사하며, 다음은 픽셀·통계·아레나에 쓰기 전에
`PIXELICA_ERR_INVALID_ARG`(기존처럼 `last_error` 기록)로 거부됩니다.

- `effects` 또는 평면 배열이 컨텍스트 소유 메모리와 겹침
- `filter`가 열거형 밖
- `clip_plane_count > 4`, 또는 `count > 0`인데 `clip_planes == NULL`
- 평면 normal/distance가 비유한, normal이 (0,0,0)
- 평면 사용 시 비아핀 모델 행렬

어떤 정점의 월드 위치나 평면 거리가 float 범위를 넘으면 픽셀·그리기 카운터를
기록하기 전에 거부합니다. 이 정점 변환 검사는 아레나 스크래치를 변경할 수 있지만
부분 드로우는 발생하지 않습니다.

## 5. 예제: 오프스크린 반사 + 클립 평면

수면(`y = 0`) 위의 장면을 거울에 비친 모습으로 오프스크린 컨텍스트에 그리고,
메인 화면의 물 메시를 그릴 때 픽셀 훅으로 그 스냅샷을 섞습니다.

```c
#include <string.h>
#include "pixelica/pixelica.h"

#define W 320
#define H 240

static uint16_t g_reflection[W * H]; /* 스냅샷: 반사 컨텍스트 결과의 복사본 */

typedef struct WaterFx {
    const uint16_t *reflection; /* W*H 스냅샷 */
    uint8_t mix;                /* 0..255, 반사 비율 */
} WaterFx;

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)
{
    uint32_t r  = (((a >> 11) & 31u) * (255u - t) + ((b >> 11) & 31u) * t + 127u) / 255u;
    uint32_t g  = (((a >> 5) & 63u) * (255u - t) + ((b >> 5) & 63u) * t + 127u) / 255u;
    uint32_t bl = ((a & 31u) * (255u - t) + (b & 31u) * t + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* 같은 카메라·같은 해상도로 그렸으므로 (x, y)를 그대로 조회한다. */
static bool water_pixel(void *userdata, const PIXELICA_Fragment *f,
                        uint16_t *out_color, uint8_t *out_alpha)
{
    const WaterFx *fx = (const WaterFx *)userdata;
    uint16_t refl = fx->reflection[(size_t)f->y * W + f->x];
    *out_color = mix565(f->color, refl, fx->mix);
    (void)out_alpha;
    return true;
}

void render_frame(PIXELICA_Context *main_ctx, PIXELICA_Context *refl_ctx,
                  const float view_proj[16], const PIXELICA_Mesh *scene,
                  const float scene_model[16], const PIXELICA_Mesh *water,
                  const float water_model[16])
{
    /* 1) 반사 패스: y를 뒤집는 아핀 행렬을 모델 앞에 곱한다. */
    static const float mirror_y[16] = {
        1, 0, 0, 0,
        0, -1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    float reflected_model[16];
    /* 뒤집힌 월드에서 원래 수면 위였던 부분은 y <= 0: -y + 0 >= 0 만 남긴다. */
    const PIXELICA_WorldClipPlane below_water = { { 0.0f, -1.0f, 0.0f }, 0.0f };
    PIXELICA_MeshEffects refl_fx;

    pixelica_mat4_multiply(reflected_model, mirror_y, scene_model);
    memset(&refl_fx, 0, sizeof refl_fx);
    refl_fx.clip_planes = &below_water;
    refl_fx.clip_plane_count = 1;

    pixelica_set_view_projection(refl_ctx, view_proj);
    pixelica_set_cull_mode(refl_ctx, PIXELICA_CULL_NONE); /* 거울은 감김 방향을 뒤집는다 */
    pixelica_begin_frame(refl_ctx, 0x0000);
    pixelica_draw_mesh_effects(refl_ctx, scene, reflected_model, NULL, NULL, &refl_fx);
    pixelica_end_frame(refl_ctx); /* headless: present 없음 */
    {
        PIXELICA_FrameView live, snapshot;
        if (pixelica_frame_view(refl_ctx, &live) != PIXELICA_OK ||
            pixelica_frame_copy(&live, g_reflection, W * H,
                                NULL, 0, &snapshot) != PIXELICA_OK) {
            return;
        }
    }

    /* 2) 메인 패스: 장면 → 물(픽셀 훅으로 반사 스냅샷 혼합). */
    {
        WaterFx water_data = { g_reflection, 128 };
        PIXELICA_MeshEffects water_fx;

        memset(&water_fx, 0, sizeof water_fx);
        water_fx.pixel = water_pixel;
        water_fx.userdata = &water_data;

        pixelica_set_view_projection(main_ctx, view_proj);
        pixelica_begin_frame(main_ctx, 0x001F);
        pixelica_draw_mesh(main_ctx, scene, scene_model);
        pixelica_draw_mesh_effects(main_ctx, water, water_model, NULL, NULL, &water_fx);
        pixelica_end_frame(main_ctx);
    }
}
```

두 컨텍스트는 같은 `W×H` 설정으로 만든다고 가정합니다. 해상도가 다르면 콜백에서
`x * refl_w / W`처럼 비율로 조회하세요. 수면 아래 물체가 반사에 섞이지 않는 것은
월드 클립 평면 덕분이며, 경계는 픽셀 폐기가 아니라 폴리곤 경계로 깔끔하게 잘립니다.

## 6. 예제: 깊이 안개 + 컷아웃 콜백

```c
typedef struct FogFx {
    uint16_t fog_color;
    float near_z, far_z, fog_start, fog_end;
    const uint8_t *mask; /* W*H 스냅샷(예: 이전 프레임에서 만든 컷아웃 마스크) */
} FogFx;

static bool fog_pixel(void *userdata, const PIXELICA_Fragment *f,
                      uint16_t *out_color, uint8_t *out_alpha)
{
    const FogFx *fx = (const FogFx *)userdata;
    float d = view_distance(f->depth, fx->near_z, fx->far_z); /* 비선형 깊이 복원 */
    float t = (d - fx->fog_start) / (fx->fog_end - fx->fog_start);

    if (fx->mask != NULL && fx->mask[(size_t)f->y * W + f->x] == 0) {
        return false; /* 색·깊이 모두 쓰지 않음 */
    }
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    *out_color = mix565(f->color, fx->fog_color, (uint32_t)(t * 255.0f + 0.5f));
    (void)out_alpha; /* OPAQUE에서는 무시, BLEND에서는 바꿔도 된다 */
    return true;
}
```

바이리니어와 함께 쓰려면 `filter = PIXELICA_FILTER_BILINEAR`만 추가하면 됩니다.

## 7. 예제: 장면 스냅샷으로 굴절 합성

불투명 장면을 그린 뒤, 물·유리를 그리기 **전에** `pixelica_frame_copy`로 색과 깊이를
복사합니다. 아래 콜백은 같은 화면·카메라의 스냅샷을 왜곡해서 기존 재질 색과 섞습니다.
`du`, `dv`는 정규화 화면 좌표의 오프셋입니다. 깊이는 같은 투영의 값끼리만 비교합니다.

```c
typedef struct RefractionFx {
    PIXELICA_FrameView scene; /* 독립 스냅샷: 색+깊이, 출력 타깃과 같은 해상도 */
    float du, dv;
    uint8_t mix;
} RefractionFx;

static bool refraction_pixel(void *userdata, const PIXELICA_Fragment *f,
                              uint16_t *out_color, uint8_t *out_alpha)
{
    const RefractionFx *fx = (const RefractionFx *)userdata;
    float u = ((float)f->x + 0.5f) / fx->scene.width + fx->du;
    float v = ((float)f->y + 0.5f) / fx->scene.height + fx->dv;
    uint16_t behind, depth;
    if (pixelica_frame_sample_depth(&fx->scene, u, v, PIXELICA_WRAP_CLAMP,
                                    PIXELICA_WRAP_CLAMP, &depth) != PIXELICA_OK ||
        depth < f->depth) {
        return true; /* 왜곡된 위치가 물 앞 물체면 원래 재질 색 유지 */
    }
    if (pixelica_frame_sample_color(&fx->scene, u, v, PIXELICA_WRAP_CLAMP,
                                    PIXELICA_WRAP_CLAMP, PIXELICA_FILTER_BILINEAR,
                                    &behind) == PIXELICA_OK) {
        *out_color = mix565(f->color, behind, fx->mix); /* 위 예제의 mix565 재사용 */
    }
    (void)out_alpha;
    return true;
}
```

`effects.pixel = refraction_pixel`, `effects.userdata = &fx`로 연결합니다.
움직이는 왜곡은 게임이 시간에 따라 오프셋이나 왜곡 맵을 계산해 전달하면 됩니다.
화면 전체의 물리 기반 굴절·반사 계산은 이 범용 API를 사용해 게임에서 구성합니다.

## 8. 제한 요약

- 클립 평면 최대 4개, 평면 사용 시 아핀 모델 필수.
- 콜백은 같은 컨텍스트를 변경하거나 다시 그릴 수 없음(거부됨). 스냅샷을 사용.
- 콜백 좌표는 프레임버퍼 픽셀, 깊이는 원시 비선형 uint16.
- 조기 깊이 테스트에 실패한 프래그먼트에는 콜백이 호출되지 않음.
- 기본 API와 기존 출력은 변경 없음.
