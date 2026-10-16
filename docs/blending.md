# 프레임버퍼 가산 블렌딩 (`PIXELICA_ALPHA_ADD`)

`PIXELICA_AlphaMode`에 네 번째 값 `PIXELICA_ALPHA_ADD`가 추가되었습니다. 3D와 2D가 같은
`pixelica/draw_options.h`를 공유하므로 **두 경로 모두** 같은 규칙으로 동작합니다. 0으로
초기화된 `PIXELICA_DrawOptions`는 여전히 `OPAQUE`이고,
`pixelica_draw_options_default()`도 그대로이므로 기존 코드는 영향을 받지 않습니다.

```c
PIXELICA_DrawOptions o = pixelica_draw_options_default();
o.alpha_mode = PIXELICA_ALPHA_ADD;
o.opacity = 90u;

pixelica_draw_mesh_options(ctx, &mesh, model, NULL, &o);   /* 3D */
pixelica2d_draw_sprite_ex(&ctx2d, &sprite, &o);            /* 2D */
```

## 머티리얼 텍스처 ADD와 혼동하지 말 것

이름이 비슷한 두 "ADD"는 **파이프라인의 다른 단계**입니다.

| | `PIXELICA_TEXTURE_ADD` | `PIXELICA_ALPHA_ADD` |
| --- | --- | --- |
| 어디서 | `PIXELICA_TextureLayer.combine` (머티리얼 레이어) | `PIXELICA_DrawOptions.alpha_mode` (목적지 블렌드) |
| 무엇에 더하나 | **프래그먼트 색**에 레이어 텍셀을 더함 | **이미 프레임버퍼에 있는 픽셀**에 프래그먼트 색을 더함 |
| 언제 | 셰이딩 중(정점 색·알파보다 먼저) | 셰이딩이 전부 끝난 뒤, 쓰기 직전 |
| 깊이 | 모드에 따름(OPAQUE면 깊이를 **씀**) | 깊이를 **절대 쓰지 않음** |
| 목적지 | 결과가 기존 픽셀을 **대체** (OPAQUE일 때) | 결과가 기존 픽셀에 **누적** |

`tests/test_blend_add.c`의 `test_add_is_not_texture_add`가 같은 레이어드 머티리얼을
두 설정으로 그려 두 결과가 다름을 고정합니다.

## 색 연산

유효 알파 `a`를 구한 뒤, 네이티브 5/6/5 채널마다:

```
BLEND  dst = (src*a + dst*(255-a) + 127) / 255
ADD    dst = min(dst + (src*a + 127) / 255, 채널 최대값)      /* 31 / 63 / 31 */
```

- **포화(saturate)** 합니다. 감싸돌지 않으므로 과다 노출이 어두운 색으로 뒤집히지
  않습니다. 31/63/31에 도달한 채널에 더 더해도 그대로입니다.
- 채널은 **독립적으로** 포화합니다. 빨강만 먼저 차고 초록은 계속 밝아질 수 있습니다.

## 유효 알파

두 블렌드 모드가 완전히 같은 알파를 씁니다. `a == 0`이면 **아무것도 쓰지 않습니다**
(색도 깊이도 없고 `pixels_written`도 늘지 않습니다).

- **3D**: `a = round(texture_alpha[텍셀] * opacity / 255)`. 알파 평면이 없으면 255에서
  시작합니다. 레이어드 머티리얼의 정점 알파는 그 앞에서 곱해지고(`material_layers.md`),
  이펙트 픽셀 훅은 그 뒤에서 알파를 바꿀 수 있습니다.
- **2D**: `a = round(round(round(plane) * options.opacity / 255) * sprite.opacity / 255)`.
  컬러 키는 알파보다 **먼저** 적용되므로, 키에 걸린 텍셀은 가산되지 않습니다.

`OPAQUE`는 지금까지처럼 `opacity`와 알파 평면을 **무시**합니다(검증은 여전히 합니다).

## 깊이 의미 (명시)

`ADD`는 `BLEND`와 똑같이:

- 깊이 **테스트**를 정상적으로 받습니다(3D는 strict-less, 2D는 `depth_test`가 켜져 있을
  때). 즉 불투명 지오메트리 **뒤에 있는** 가산 지오메트리는 가려집니다.
- 깊이를 **절대 쓰지 않습니다**. 3D에서는 모드 자체가 깊이 쓰기를 건너뛰고, 2D에서는
  `pixelica2d_draw_sprite_ex`가 `sprite.depth_write`를 강제로 꺼 버립니다.

결과적으로:

- 겹치는 반투명/가산 표면끼리는 서로를 가리지 않습니다.
- 가산 레이어 **뒤에** 나중에 그린 불투명 지오메트리는 가산 레이어에 가려지지 않고
  그대로 그려집니다. 이것은 버그가 아니라 의도된 동작이며, 그래서 가산 지오메트리는
  마지막에 그려야 합니다.

## 정렬

`ADD`는 포화를 제외하면 **교환법칙이 성립**하므로 정렬이 필요 없습니다. `BLEND`는
필요합니다. 권장 순서:

1. 불투명 + `TEST` 지오메트리 (깊이를 씀)
2. `BLEND` 지오메트리, 뒤에서 앞으로
3. `ADD` 지오메트리, 임의 순서

`tests/test_blend_add.c`와 `tests/test_blend_add2d.c`가 두 가산 드로우의 순서를 바꿔도
같은 픽셀이 나오고, 같은 상황에서 `BLEND`는 달라진다는 것을 확인합니다.

## 검증

열거형 범위 밖의 `alpha_mode`(이제 `PIXELICA_ALPHA_ADD + 1` 이상, 또는 음수)는 3D와 2D
모두에서 `PIXELICA_ERR_INVALID_ARG`이고 아무것도 바뀌지 않습니다. 알파 평면 포인터/크기
불일치, 컨텍스트 소유 메모리와의 겹침 등 기존 검증은 `ADD`에서도 전부 그대로입니다.

## 쓰임새

광원 플레어, 스파크·불꽃 파티클, 레이저, 마법 효과, 화면 플래시처럼 "빛을 더하는" 것들.
어두운 배경 위에서 특히 자연스럽고, RGB565의 좁은 계조에서도 포화 덕분에 깨지지
않습니다. `examples/hello_lighting/`이 두 장의 겹치는 글로우로 포화와 순서 무관성을
보여 줍니다.
