# 텍스처 옵션: REPEAT와 텍셀 단위 알파

기존 RGB565 에셋과 기존 그리기 API는 **변경되지 않습니다**. 이 문서의 기능은 모두 opt-in이며,
공유 헤더 `pixelica/draw_options.h`의 `PIXELICA_DrawOptions`로 선택합니다. 새 로더나 새 에셋
형식은 없습니다. RGB565 텍스처 + 별도의 8비트 알파 평면(plane)을 호출자가 직접 제공합니다.

## API

```c
/* 3D */
PIXELICA_DrawOptions options = pixelica_draw_options_default();
pixelica_draw_mesh_options(ctx, mesh, model, material, &options);

/* 2D */
PIXELICA_Status pixelica2d_draw_sprite_ex(PIXELICA2D_Context *ctx,
                                          const PIXELICA2D_Sprite *sprite,
                                          const PIXELICA_DrawOptions *options);
```

`pixelica_draw_options_default()`는 `CLAMP/CLAMP`, `OPAQUE`, `opacity=255`, `alpha_cutoff=128`,
알파 평면 없음(`NULL`, 0바이트)을 돌려줍니다. 2D에서 `options == NULL`이면 기존
`pixelica2d_draw_sprite`와 완전히 같습니다(소스 사각형 검증, 컬러 키, opacity, depth 포함).

## 래핑 규칙 (2D)

- 소스 사각형의 크기는 여전히 `width, height > 0` 이어야 합니다. `x, y`는 부호 있는 정수입니다.
- 샘플링은 정규화 UV가 아니라 **절대 소스 텍셀 좌표** `source.x + floor(u)`,
  `source.y + floor(v)`로 합니다. 소스 크기를 UV 범위로 재해석하지 않습니다.
- U, V 축은 독립적으로 `CLAMP` 또는 `REPEAT`입니다.
  - `REPEAT`: 전체 텍스처 크기 기준 floor modulo. 음수(`-1 -> width-1`)와 2의 거듭제곱이 아닌
    크기도 정확히 동작하며 오버플로가 없습니다.
  - `CLAMP`: 좌표를 `0..size-1`로 자릅니다.
- 따라서 `pixelica2d_draw_sprite_ex`에 `options`를 주면 소스 사각형이 텍스처 밖에 있어도
  허용됩니다(CLAMP 축은 가장자리 텍셀로 잘림). 이는 **ex 전용**이며 기존 API는 계속 거부합니다.
- 큰 좌표/배율/회전은 기존의 double 변환과 클리핑을 그대로 사용하므로 안전합니다.

## 래핑 규칙 (3D)

3D는 원근 보정된 정규화 UV를 사용합니다. `CLAMP`는 `[0,1]`로 제한하고 1은 마지막 텍셀을 선택합니다.
`REPEAT`는 `t - floor(t)`를 적용하므로 1·2·-1은 0으로, -0.25는 0.75로 반복합니다.
U/V를 독립적으로 설정하며 2의 거듭제곱이 아닌 텍스처도 지원합니다.
알파 평면은 RGB565 색과 같은 UV·같은 텍셀 위치에서 샘플링됩니다.
- 알파 평면은 `width*height` 바이트, 행 우선, 패딩 없음이며 색 텍셀과 같은 위치를 읽습니다.

## 알파 모드

2D 유효 알파 = `round(round(map_or_255 * options.opacity / 255) * sprite.opacity / 255)`
(`round(x/255)` = `(x + 127) / 255`).
3D 유효 알파는 `round(map_or_255 * options.opacity / 255)`이며, 텍스처 없는 단색 메시도 전역 opacity로 처리합니다.
3D `OPAQUE`는 옵션의 알파를 무시하고, `TEST`는 통과한 픽셀의 색·깊이를 기록하고,
`BLEND`는 기존 불투명 깊이와 LESS 검사만 수행하며 깊이를 기록하지 않습니다.

| 모드 | 동작 |
|---|---|
| `OPAQUE` | `options.opacity`와 알파 평면을 무시. 기존 `sprite.opacity`/컬러 키/깊이 동작 유지 |
| `TEST` | 유효 알파 `< alpha_cutoff`이면 버림. 통과한 픽셀은 불투명 복사, `depth_test`/`depth_write` 존중 |
| `BLEND` | 유효 알파 0이면 깊이까지 건너뜀. RGB565 반올림 source-over, `depth_test`만 적용, 깊이는 **쓰지 않음** |

BLEND는 순서 의존적이며 자동 OIT는 없습니다. 불투명/TEST를 먼저 그리고, BLEND는 호출자가
뒤에서 앞으로(back-to-front) 정렬해 그리세요. 3D도 동일한 규칙입니다.

## 기존 2D opacity 특이사항

- `sprite.opacity == 0`은 OPAQUE에서 아무것도(깊이도) 쓰지 않습니다.
- opacity < 255의 OPAQUE는 기존처럼 채널별 블렌드를 하며 깊이는 `depth_write`면 씁니다.
- TEST에서 `alpha_cutoff == 0`이면 유효 알파 0도 통과합니다(조기 종료 없음).
- 컬러 키(`color_key_enabled`)는 모든 모드에서 먼저 적용됩니다.

## 검증, 메모리, 수명

- 모든 옵션은 그리기 전에 검증되며 실패 시 `PIXELICA_ERR_INVALID_ARG`, 색/깊이 버퍼는 그대로입니다.
  2D는 stats도 그대로이며, 3D는 기존 API처럼 `last_error`에 실패 코드를 기록합니다.
- 거부 조건: 범위 밖/음수 enum, `texture_alpha == NULL`인데 bytes != 0,
  `texture_alpha_bytes < width*height`, 옵션 구조체나 알파 평면이 색/깊이 버퍼와 겹침.
- 그리기 중 할당하지 않고 입력 포인터를 보관하지 않습니다. `options`와 알파 평면은 호출 동안만
  빌려 쓰며 출력 버퍼와 겹치면 안 됩니다. 알파 평면은 텍스처 크기만큼 추가 메모리(1바이트/텍셀)입니다.
- 텍스처 크기 제한은 기존 `PIXELICA_MAX_TEXTURE_DIMENSION`을 따릅니다.
- 3D 컨텍스트는 투명 픽셀에도 정확한 커버리지 통계를 유지하도록 `ceil(width*height/8)` 바이트의
  비트맵과 가드를 생성 시 확보합니다. 호출자 버퍼는 최신 `pixelica_memory_requirement()`로 크기를 구하세요.
- tilemap/text/fill_rect/draw_list의 동작은 바뀌지 않습니다(draw_list는 기존 sprite 검증 사용).

## 예제

```c
/* 가로 타일링 바닥: 소스 8x8을 64픽셀 폭으로 반복 */
PIXELICA_DrawOptions o = pixelica_draw_options_default();
o.wrap_u = PIXELICA_WRAP_REPEAT;
PIXELICA2D_Sprite s;
pixelica2d_sprite_init(&s, &tex);
s.source = (PIXELICA2D_Rect){0, 0, 64, 8};   /* 텍스처가 8x8이어도 허용 */
pixelica2d_draw_sprite_ex(&ctx, &s, &o);

/* 알파 컷아웃 + 깊이 */
o = pixelica_draw_options_default();
o.alpha_mode = PIXELICA_ALPHA_TEST;
o.texture_alpha = alpha_plane;               /* tex.width * tex.height 바이트 */
o.texture_alpha_bytes = (size_t)tex.width * tex.height;
s.depth = 10; s.depth_test = true; s.depth_write = true;
pixelica2d_draw_sprite_ex(&ctx, &s, &o);

/* 반투명: 뒤에서 앞으로 */
o.alpha_mode = PIXELICA_ALPHA_BLEND;
o.opacity = 160;
pixelica2d_draw_sprite_ex(&ctx, &s, &o);
```
