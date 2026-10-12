# 레이어 재질: 텍스처 × 정점 색 + 다중 UV 합성

3D 메시 전용 opt-in 기능입니다(2.5D의 3D 경로 포함, 2D 스프라이트는 **변경 없음**).
기존 `PIXELICA_Mesh`, `PIXELICA_Material`, `PIXELICA_DrawOptions`의 레이아웃·API·출력은 그대로입니다.

```c
PIXELICA_Status pixelica_draw_mesh_layers(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                                          const float model[16],
                                          const PIXELICA_MaterialLayers *material,
                                          const PIXELICA_DrawOptions *options);
```

| 타입 | 필드 |
|---|---|
| `PIXELICA_ColorRGBA8` | `r, g, b, a` (각 0..255) |
| `PIXELICA_TextureLayer` | `texture`, `uvs`(정점마다 1개), `wrap_u/wrap_v`, `combine`, `opacity` |
| `PIXELICA_MaterialLayers` | `base`(기존 `PIXELICA_Material`), `vertex_colors`, `layers`, `layer_count` (0..`PIXELICA_MAX_TEXTURE_LAYERS`=3) |

- `material == NULL` → `pixelica_draw_mesh_options(ctx, mesh, model, NULL, options)`와 동일(단색).
- `{0}`으로 초기화한 재질 → 레이어·정점 색 없음, `base`만 사용 = 기존 결과와 비트 단위 동일.

## 합성 순서 (프래그먼트마다)

1. **base**: `base.texture`(DrawOptions의 wrap 사용) → 없으면 `base.triangle_colors` → 없으면 `mesh->color`.
2. **layers[0..layer_count)** 순서대로. 레이어 텍셀 `t`(자신의 UV·wrap, nearest, RGB만)와 이전 색 `p`를
   RGB565 채널별(최대값 `m` = 31/63/31) 정수 연산:

   | combine | 결과 `r` |
   |---|---|
   | `PIXELICA_TEXTURE_MULTIPLY` | `(p*t + m/2) / m` (반올림) |
   | `PIXELICA_TEXTURE_ADD` | `min(p + t, m)` (포화) |
   | `PIXELICA_TEXTURE_LERP` | `t` |

   그 뒤 레이어 opacity `a`로 혼합: `(r*a + p*(255-a) + 127) / 255`. (`a=0` 효과 없음, `255` 완전 적용)
3. **정점 색**(`vertex_colors`, NULL = 불투명 흰색 = 변화 없음): r,g,b,a를 클리핑과 함께 보간하고
   원근 보정(double)으로 보간한 값 `c`(0..255로 clamp, **정수로 반올림하지 않음**)로
   각 RGB565 채널을 `floor(channel * c / 255 + 0.5)`로 곱합니다. RGB565로 미리 양자화하지 않으며,
   `255`는 정확한 항등, `0`은 0입니다.
4. **알파**(TEST/BLEND만): `s` = base 텍스처 알파(`options.texture_alpha`, 없으면 255. 추가 레이어는 알파 없음)
   → `s = floor(s * c_a / 255 + 0.5)` → `s = (s * options.opacity + 127) / 255`.
   이후 기존 규칙 그대로: `TEST`는 `s < alpha_cutoff`면 버림(통과 시 색+깊이 기록),
   `BLEND`는 `s == 0` 건너뜀, 깊이는 **쓰지 않음**. `OPAQUE`는 base 알파처럼 정점 알파도 무시합니다.

UV·색 속성은 모두 원근 나눗셈 **전에** 위치와 같은 매개변수로 클리핑되고, `1/w`와 함께
원근 보정 보간됩니다. 정점 변환 arena(`PIXELICA_ARENA_BYTES_PER_VERTEX` = 20)는 변하지 않습니다.

## 검증 (`PIXELICA_ERR_INVALID_ARG`, 픽셀·통계 카운터·arena에 쓰기 전)

base와 옵션에는 기존 규칙이 그대로 적용되고, 추가로 다음을 거부합니다(`last_error`에는 기록):
- `layer_count > 3`, `layer_count > 0`인데 `layers == NULL`
- 레이어: `texture` NULL/무효(픽셀 NULL, 크기 0 또는 4096 초과), `vertex_count > 0`인데 `uvs == NULL`,
  비유한 UV, `wrap_u/wrap_v/combine`이 enum 범위 밖
- 재질 구조체, 레이어 배열, 텍스처 구조체·텍셀, UV, `triangle_colors`, `vertex_colors`가
  컨텍스트 소유 메모리(컨텍스트, 색/깊이, arena, 커버리지)와 겹침
- 확장 재질을 지정한 경우 메시·모델 행렬·정점·인덱스 배열의 같은 메모리 겹침도 거부합니다.

힙 할당이 없고 포인터를 보관하지 않습니다. 모든 배열은 호출 동안만 빌려 씁니다.

## 예제: UV 2벌 + 정점 색 + 알파

```c
/* 사각형 4정점: 확산 텍스처(UV0) + 라이트맵(UV1, 별도 좌표) */
static const PIXELICA_Vec2 uv0[4] = {{0, 1}, {4, 1}, {4, 0}, {0, 0}}; /* 4회 타일링 */
static const PIXELICA_Vec2 uv1[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
static const PIXELICA_ColorRGBA8 vcol[4] = {
    {255, 255, 255, 255}, {255, 128, 128, 255},   /* 오른쪽 아래를 붉게 */
    {255, 255, 255, 128}, {255, 255, 255, 0}};    /* 위쪽으로 투명해짐 */

PIXELICA_TextureLayer lightmap = {0};
lightmap.texture = &lightmap_tex;
lightmap.uvs = uv1;
lightmap.wrap_u = PIXELICA_WRAP_CLAMP;
lightmap.wrap_v = PIXELICA_WRAP_CLAMP;
lightmap.combine = PIXELICA_TEXTURE_MULTIPLY;
lightmap.opacity = 255;

PIXELICA_MaterialLayers mat = {0};
mat.base.texture = &diffuse_tex;     /* base = 1번째 UV */
mat.base.uvs = uv0;
mat.layers = &lightmap;              /* 2번째 UV */
mat.layer_count = 1;
mat.vertex_colors = vcol;

PIXELICA_DrawOptions o = pixelica_draw_options_default();
o.wrap_u = PIXELICA_WRAP_REPEAT;     /* base 텍스처에만 적용 */
o.wrap_v = PIXELICA_WRAP_REPEAT;
o.alpha_mode = PIXELICA_ALPHA_BLEND; /* 정점 알파 × base 알파 × opacity */
o.opacity = 255;

/* 불투명/TEST 먼저, BLEND는 뒤에서 앞으로 */
pixelica_draw_mesh_layers(ctx, &quad, model, &mat, &o);
```

정점 색만 쓰려면 `mat.vertex_colors`만 채우고 나머지는 `{0}`으로 두면 됩니다(색 = `mesh->color` × 정점 색).

## 비용

- 레이어/정점 색이 없으면 기존 래스터 경로를 그대로 탑니다.
- 레이어당 프래그먼트마다 nearest 샘플 1회, 정점 색은 double 보간 4채널.
- 스택의 클립 다각형/화면 정점이 정점당 속성 10개만큼 커졌습니다(합계 약 3 KiB).
