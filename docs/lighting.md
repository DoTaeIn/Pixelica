# 노멀 기반 조명 (`pixelica_draw_mesh_lit`)

렌더러의 기본값은 지금까지와 똑같이 **unlit**입니다. 조명은 전적으로 opt-in이며,
`PIXELICA_Lighting`을 넘기는 새 진입점 하나에서만 동작합니다. 기존 `pixelica_draw_mesh`,
`_ex`, `_options`, `_layers`, `_effects`의 출력은 **비트 단위로 그대로**이고, 노멀이 없는
기존 메시·에셋도 그대로 쓸 수 있습니다.

```c
PIXELICA_Status pixelica_draw_mesh_lit(PIXELICA_Context *ctx,
                                       const PIXELICA_Mesh *mesh,
                                       const float model[16],
                                       const PIXELICA_MaterialLayers *material,
                                       const PIXELICA_DrawOptions *options,
                                       const PIXELICA_MeshEffects *effects,
                                       const PIXELICA_Lighting *lighting);
```

`lighting == NULL`이면 `pixelica_draw_mesh_effects(...)`와 정확히 같습니다. 즉 이 함수는
기존 경로의 상위 집합이며, 조명만 추가로 켤 수 있는 입구입니다.

```c
typedef struct PIXELICA_DirectionalLight {
    PIXELICA_Vec3 direction; /* 빛이 "진행하는" 방향(월드), 0이 아니어야 함 */
    uint8_t r, g, b;         /* 빛의 색·세기, 255 = 최대 */
} PIXELICA_DirectionalLight;

typedef struct PIXELICA_Lighting {
    const PIXELICA_Vec3 *normals;            /* vertex_count개 오브젝트 공간 노멀 */
    const PIXELICA_DirectionalLight *lights; /* light_count == 0이면 NULL 가능 */
    size_t light_count;                      /* 0..PIXELICA_MAX_DIRECTIONAL_LIGHTS (4) */
    uint8_t ambient_r, ambient_g, ambient_b; /* 상수항 */
    bool two_sided;                          /* |dot| 사용(양면 조명) */
} PIXELICA_Lighting;
```

## 계산 모델

**정점마다(Gouraud)** 다음을 계산합니다.

```
N   = normalize(normal_matrix * n)        n = 그 정점의 오브젝트 공간 노멀
L_k = normalize(-lights[k].direction)     빛을 "향하는" 방향
d_k = max(0, dot(N, L_k))                 two_sided면 |dot(N, L_k)|
f_c = min(255, ambient_c + Σ_k round(light_k.c * d_k))     c = r, g, b 각각 독립
```

`f`는 삼각형 위에서 **원근 보정 보간**되고, 프래그먼트 색을 정점 색과 똑같은 방식으로
**변조(modulate)** 합니다. 5/6/5 네이티브 채널마다

```
channel = floor(channel * f_c / 255 + 0.5)
```

따라서

- `f = 255`는 **정확한 항등**입니다. `ambient = (255,255,255)` + 광원 0개는 unlit 이미지를
  비트 단위로 재현합니다 (`test_lighting`이 해시로 확인).
- `f = 0`은 검정입니다.
- 조명은 표면을 자기 색보다 **밝게 만들지 않습니다**. 채널 합은 255에서 포화하며
  감싸돌지(wrap) 않습니다. 빛을 더하고 싶으면 프레임버퍼 가산 블렌딩
  ([`blending.md`](blending.md))을 쓰세요.
- `PIXELICA_MaterialLayers.vertex_colors`가 같이 있으면 두 정점별 계수를 **하나로 곱합니다**
  (`vertex_color_c * f_c / 255`, 보간 전에 double로). 흰 정점 색은 아무것도 바꾸지 않습니다.
- 조명은 알파를 건드리지 않습니다. 정점 알파는 그대로 적용됩니다.
- 조명은 이펙트 픽셀 훅보다 **먼저** 실행되므로, 훅은 이미 조명이 적용된 색을 봅니다.

정점 단위이므로 면마다 노멀이 하나인 **플랫 셰이딩 메시는 정확**하고, 부드러운 메시는
Gouraud 보간이 됩니다. 픽셀마다 노멀을 복원하지는 않습니다 (MCU 타깃에서 픽셀당 sqrt를
피하기 위한 의도적 선택입니다).

## 노멀 행렬: 왜 역전치인가

노멀은 **방향**이므로 모델 행렬로 그냥 변환하면 안 됩니다. 비균등(non-uniform) 스케일이나
전단(shear)에서는 표면이 기울어지는 방향과 반대로 기울어 버립니다. 렌더러는 모델 행렬
좌상단 3x3의 **역전치**를 쓰고, 이는 공개 API로도 노출되어 있습니다.

```c
PIXELICA_Status pixelica_mat4_normal_matrix(float out[16], const float m[16]);
```

- 결과의 좌상단 3x3이 역전치이고, 나머지는 항등입니다(이동은 무시).
- 정규화는 하지 않습니다. 호출자(조명)가 정규화합니다.
- 행렬식이 음수인 **거울(mirroring)** 행렬은 노멀을 뒤집습니다. 같은 행렬이 지오메트리의
  와인딩도 뒤집으므로 일관됩니다.
- **특이(singular)** 3x3(예: 한 축 스케일 0)은 노멀 행렬이 존재하지 않으므로
  `PIXELICA_ERR_INVALID_ARG`이고, 그 모델로 조명을 건 드로우도 거부됩니다.

예: X를 1/4로 눌러 붙인 모델에서 노멀 `(1,0,1)`은 `(4,0,1)`을 정규화한 값이 되어야 하며
(표면은 +Z 쪽으로 눕고 노멀은 +X 쪽으로 섭니다), 모델 행렬을 그대로 쓴 `(0.25,0,1)`과는
완전히 다릅니다. `test_lighting`과 `test_math`가 두 경우를 모두 비교합니다.

## 검증 (전부 픽셀을 쓰기 전에)

`PIXELICA_ERR_INVALID_ARG`가 되고 색·깊이·통계가 전혀 바뀌지 않는 경우:

| 조건 |
| --- |
| `vertex_count > 0`인데 `normals == NULL` |
| 비유한(non-finite) 노멀, 또는 길이 0 노멀 |
| `light_count > PIXELICA_MAX_DIRECTIONAL_LIGHTS` |
| `light_count > 0`인데 `lights == NULL` |
| 비유한 또는 길이 0인 광원 방향 |
| 모델 행렬 좌상단 3x3이 특이 |
| `lighting` 구조체·`normals`·`lights`가 컨텍스트 소유 메모리와 겹침 |

광원 방향과 노멀은 **단위 길이일 필요가 없습니다**. 내부에서 정규화하므로 길이를 바꿔도
결과가 같습니다.

## 메모리와 비용

새 힙 할당이 없고 프레임 아레나도 커지지 않습니다. 노멀 행렬과 정규화된 광원 방향은
드로우마다 한 번 계산하고, 정점별 계수는 월드 클립 평면 거리와 같은 방식으로 **삼각형마다
다시 계산**합니다(정점당 아레나 바이트 수 `PIXELICA_ARENA_BYTES_PER_VERTEX`는 그대로 20).
래스터라이저는 전혀 바뀌지 않습니다: 조명은 기존 정점 색 변조 슬롯을 타고 들어갑니다.

## 스키닝된 메시의 노멀

선택 모듈 `Pixelica::skeleton`이 위치와 짝이 되는 노멀 스키닝을 제공합니다
([`rigging.md`](rigging.md)):

```c
pixelica_skeleton_normal_palette(palette, bone_count, normal_palette, scratch, n);
pixelica_skeleton_skin_normals(bind_normals, weights, vertex_count,
                               normal_palette, bone_count,
                               out_normals, scratch, n);
```

결과는 단위 길이이므로 그대로 `PIXELICA_Lighting.normals`에 넣을 수 있습니다. 본이 회전·
이동·**균등** 스케일뿐이면 `normal_palette` 단계는 건너뛰고 일반 팔레트를 바로 넘겨도
같은 결과가 나옵니다(차이는 본마다의 양수 배율뿐이고 마지막 정규화가 지웁니다).

## 실행 예제

`examples/hello_lighting/`이 비균등 스케일이 걸린 조명 큐브 + 가산 글로우 + 애니메이션
클립을 한 프로그램에서 보여 줍니다. 헤드리스로 돌아갑니다:

```sh
./build/hello_lighting --frames 260 --hidden
```

## 테스트

- `tests/test_lighting.c` — unlit 비트 동일성, ambient/directional 산술, 포화, 비균등
  스케일과 거울 변환, 정규화, two_sided, Gouraud 그라디언트, 정점 색 합성, 모든 거부 경로.
- `tests/test_math.c` — `pixelica_mat4_normal_matrix`의 수학적 성질
  (`transpose(M) * N == I`, 수직성 보존, 거울, 특이 행렬 거부).
- `tests/test_skeleton.c` — 노멀 팔레트와 노멀 스키닝.
