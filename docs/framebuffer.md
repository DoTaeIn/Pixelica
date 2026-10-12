# 프레임버퍼 스냅샷 · 샘플링

헤더: `pixelica/framebuffer.h` (공통 라이브러리, 3D 헤더 불필요).
3D 컨텍스트 어댑터 `pixelica_frame_view`, `pixelica_frame_texture`는 3D 코어에 있습니다.

## PIXELICA_FrameView

```c
typedef struct PIXELICA_FrameView {
    const uint16_t *color, *depth; /* 둘 중 하나는 NULL 가능, 둘 다 NULL은 오류 */
    uint16_t width, height;        /* 1..4096 */
    size_t stride;                 /* 픽셀 단위, stride >= width */
} PIXELICA_FrameView;
```

- 행 우선, 0번 행이 **위쪽**. 픽셀 `(x, y)` = `plane[y * stride + x]`.
- 뷰는 **빌린(borrowed) 읽기 전용** 설명자입니다. 메모리는 소유하지 않습니다.
- `depth`는 3D 렌더러의 **raw depth16**입니다 (NDC 기반, 지운 값 = 65535).
  원근 투영에서는 거리에 대해 비선형이며, 거리(미터 등)로 바로 해석하지 마세요.

## 레시피 1: 스냅샷

```c
PIXELICA_FrameView live, snap;
static uint16_t color[160 * 120], depth[160 * 120];

pixelica_frame_view(ctx, &live);                 /* 소유하지 않는 뷰 */
if (pixelica_frame_copy(&live, color, 160 * 120,
                        depth, 160 * 120, &snap) == PIXELICA_OK) {
    /* snap.stride == 160, color/depth는 이제 독립 사본 */
}
```

- 출력 버퍼 용량은 픽셀 수 기준 `>= width * height`. 용량 부족은 `PIXELICA_ERR_CAPACITY`.
- 생략할 평면은 버퍼 `NULL` + 용량 `0`. 적어도 하나는 요청해야 합니다.
  버퍼를 주면 소스에 해당 평면이 있어야 합니다 (color 전용/depth 전용 소스 가능).
- 출력 뷰(`out`)는 항상 stride == width로 빽빽하게 채워집니다.
- 모든 검증 후에만 씁니다. 실패 시 출력 버퍼와 `out`은 그대로입니다.
- 출력끼리, 출력과 입력(소스 설명자·평면)이 겹치면 `PIXELICA_ERR_INVALID_ARG`.

## 레시피 2: 샘플링

```c
uint16_t px, d;
pixelica_frame_sample_color(&snap, 0.5f, 0.5f, PIXELICA_WRAP_CLAMP,
                            PIXELICA_WRAP_CLAMP, PIXELICA_FILTER_BILINEAR, &px);
pixelica_frame_sample_depth(&snap, 0.5f, 0.5f, PIXELICA_WRAP_CLAMP,
                            PIXELICA_WRAP_CLAMP, &d);
```

- `(u, v) = (0, 0)`은 왼쪽 위. 유한한 모든 값(음수, 매우 큰 값)이 안전합니다. NaN/Inf는 오류.
- NEAREST: `floor(u * width)`; CLAMP는 가장자리 고정, REPEAT는 순환.
- BILINEAR: 텍셀 중심 `u * width - 0.5`, RGB565 고유 5/6/5 채널을 double로 섞고 가장 가까운 정수로 반올림.
  stride 패딩 열은 읽지 않습니다. REPEAT는 이음새에서 반대쪽 텍셀과 섞습니다.
- depth는 항상 nearest raw 값 (보간 없음).
- `out`이 뷰 설명자나 평면과 겹치면 오류.

## 레시피 3: 오프스크린 렌더 → 텍스처 → 메인 렌더

오프스크린은 이미 지원됩니다: `pixelica_create(&cfg, NULL, &off)` (플랫폼 `NULL`, 또는
`pixelica_create_in_buffer`)로 해상도가 다른 독립 컨텍스트를 만듭니다.

```c
PIXELICA_Config cfg = {160, 120, 64, 256, 256 * PIXELICA_ARENA_BYTES_PER_VERTEX};
PIXELICA_Context *off = NULL;
if (pixelica_create(&cfg, NULL, &off) != PIXELICA_OK) {
    /* 메모리 부족 등 오류 처리 */
}
/* off 사용을 마치면 pixelica_destroy(off). */
```

위 3D 코드는 `pixelica/pixelica.h`를 포함합니다. 2D는 별도 `PIXELICA2D_Surface`에
그린 후 `{surface.pixels, NULL, surface.width, surface.height, surface.stride}` 뷰를
만들면 공통 복사·색 샘플링 API를 그대로 쓸 수 있습니다. 2D 스프라이트 그리기의
필터는 기존 NEAREST이며, 이번 BILINEAR 재질 경로는 3D 메시용입니다.

```c
pixelica_begin_frame(off, 0);
pixelica_draw_mesh(off, &scene_mesh, model);
pixelica_end_frame(off);            /* PAL이 NULL이면 present 없이 렌더만 */

PIXELICA_Texture tex;
pixelica_frame_texture(off, &tex);  /* off의 RGB565 색 버퍼를 그대로 빌림 */

PIXELICA_Material mat = { uvs, NULL, &tex };
pixelica_begin_frame(main_ctx, 0);
pixelica_draw_mesh_ex(main_ctx, &quad, model, &mat);
pixelica_end_frame(main_ctx);
```

전체 예: `tests/test_framebuffer3d.c`.

## 소유권 · 수명

| 대상 | 소유자 | 수명 |
| --- | --- | --- |
| `pixelica_frame_view`/`pixelica_frame_texture` 결과 | 컨텍스트 | `pixelica_destroy`까지. 내용은 다음 `pixelica_begin_frame`에서 바뀜 |
| `pixelica_frame_copy` 출력 | 호출자 | 호출자가 해제할 때까지. 이후 프레임과 무관 |
| 어댑터 `out` 구조체 | 호출자 | 컨텍스트 메모리와 겹치면 `PIXELICA_ERR_INVALID_ARG` |

- 라이브 뷰/텍스처는 컨텍스트가 다시 그릴 때마다 바뀝니다. 유지하려면 `pixelica_frame_copy`로 스냅샷을 뜨세요.
- 자기 자신의 색 버퍼를 같은 컨텍스트에서 텍스처로 그리지 마세요 (읽기/쓰기 겹침; 레이어 API는 겹침을 거부). 별도 컨텍스트나 스냅샷을 쓰세요.

## 복사 시점 · 비용 · 비동기

- 복사는 프레임 중 읽기 전용이며 동기식입니다. 그리는 도중의 미완성 결과가 보이므로, 보통 `pixelica_end_frame` 후에 뜨세요.
- 비동기 기능은 없습니다. 호출이 끝나면 복사도 끝납니다. 스레드 안전하지 않습니다 (컨텍스트당 단일 스레드).
- 메모리: 스냅샷 한 장 = `W*H*2` 바이트(color) + `W*H*2` 바이트(depth). 예: 160x120 = 약 37.5 KiB + 37.5 KiB.
  오프스크린 컨텍스트는 color/depth/arena/coverage 전체 비용이 듭니다 (`pixelica_memory_requirement`).
- 스냅샷·샘플링 API 자체에는 힙 할당·전역 상태가 없습니다.
