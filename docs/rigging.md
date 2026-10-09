# 선택 모듈: 스켈레톤(스키닝)과 IK

Pixelica의 기본 애니메이션(`pixelica/animation.h`: 이징, 스칼라 tween, 키프레임 샘플링, 재생 플레이어, 피벗 변환)은 그대로
**공용 라이브러리(`pixelica_common`)**에 있으며 스켈레톤이나 IK를 포함하지 않습니다. 이 문서의 두 모듈은 **선택(optional) 모듈**입니다.
켜지 않으면 헤더, 라이브러리, 심볼, CMake 타깃, 설치 파일 어느 것에도 나타나지 않습니다.

| 모듈 | 옵션 | 타깃 | 헤더 | 소스 |
|---|---|---|---|---|
| 스켈레톤 | `PIXELICA_BUILD_SKELETON` (기본 `OFF`) | `pixelica_skeleton` / `Pixelica::skeleton` | [`pixelica/skeleton.h`](../modules/skeleton/include/pixelica/skeleton.h) | [`modules/skeleton/skeleton.c`](../modules/skeleton/skeleton.c) |
| IK | `PIXELICA_BUILD_IK` (기본 `OFF`) | `pixelica_ik` / `Pixelica::ik` | [`pixelica/ik.h`](../modules/ik/include/pixelica/ik.h) | [`modules/ik/ik.c`](../modules/ik/ik.c) |

- 두 모듈은 **서로 독립**입니다. 스켈레톤만, IK만, 둘 다, 둘 다 없이 빌드할 수 있고 서로의 헤더를 포함하지 않습니다.
- 외부 의존성, 힙 할당, 호출 사이에 남는 포인터, 전역 가변 상태, 시계, `PIXELICA_Context`가 없습니다. 모든 배열은 호출자 소유이고
  호출 동안만 빌려 씁니다.
- 2D·3D 분리 이후 두 모듈은 공용 라이브러리에만 의존하며 3D 렌더러를 끈 구성에서도 사용할 수 있습니다([2D 가이드](2d.md)).
  코어 헤더(`pixelica/pixelica.h` 포함)는 선택 헤더를 포함하지 않습니다. 선택 헤더는 필요할 때 직접 `#include "pixelica/skeleton.h"` /
  `"pixelica/ik.h"` 합니다.
- 이 문서는 **계약과 사용법**을 설명합니다. 빌드·테스트 실행 결과의 전체는 담지 않으며, 검증 기록은
  `docs/RIGGING_AUDIT.md` (local-only record)에 있습니다(요약은 9장). 공용 분리 후의 호환성 검증은
  `DIMENSIONS_AUDIT.md` (local-only record)를 참고하세요.

## 1. 빌드

### 1.1 옵션

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `PIXELICA_BUILD_SKELETON` | `OFF` | `modules/skeleton`을 빌드하고 `Pixelica::skeleton` 정의, `tests/test_skeleton.c` 등록 |
| `PIXELICA_BUILD_IK` | `OFF` | `modules/ik`를 빌드하고 `Pixelica::ik` 정의, `tests/test_ik.c` 등록 |

기본값은 최상위 프로젝트일 때도 `OFF`입니다(다른 옵션과 달리 최상위 여부에 따라 바뀌지 않음). 둘 다 `ON`이면 데모
`hello_rig`도 빌드됩니다(예제 빌드와 `PIXELICA_BUILD_3D`가 켜져 있을 때).

```sh
# 선택 모듈 없음(기본값)
cmake -S . -B build-rig-none -DPIXELICA_PLATFORM=HEADLESS
# 스켈레톤만
cmake -S . -B build-rig-skeleton -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_SKELETON=ON
# IK만
cmake -S . -B build-rig-ik -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_IK=ON
# 둘 다 + 데모 hello_rig
cmake -S . -B build-rig-both -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_SKELETON=ON -DPIXELICA_BUILD_IK=ON
cmake --build build-rig-both --config Debug
ctest --test-dir build-rig-both -C Debug -R "skeleton|ik|rig" --output-on-failure
```

조합마다 **새로운 빌드 디렉터리 이름**을 쓰세요. 옵션은 캐시에 남으므로 같은 디렉터리를 다시 구성하면 이전에 켠 모듈이 계속
`ON`으로 남습니다(끄려면 `-DPIXELICA_BUILD_SKELETON=OFF`처럼 명시하거나 새 디렉터리를 쓰세요). 두 옵션의 기본값은 계속 `OFF`입니다.

`-DPIXELICA_PLATFORM=HEADLESS`는 SDL 없는 새 빌드입니다. 창 없이 테스트와 데모 스모크를 검증하는 용도이고, 눈으로 보는 대화형 미리보기는
SDL3 플랫폼(`-DPIXELICA_PLATFORM=SDL3`, [`docs/sdk.md`](sdk.md) 참고)을 씁니다.

타깃 성질(코어와 같은 방식):

- `STATIC`, `EXPORT_NAME`은 `skeleton` / `ik`, Debug 접미사 `d`, 공개(PUBLIC)로 `pixelica_common`에 링크.
- 경고·새니타이저 플래그(`pixelica_options`)는 빌드 트리 안에서만 `$<BUILD_INTERFACE:...>`로 전달되고 설치된 타깃에는 남지
  않습니다.
- 공개 include 디렉터리는 해당 모듈의 `include/` 하나뿐이며, 모듈이 꺼져 있으면 존재하지 않습니다.

### 1.2 `add_subdirectory`로 쓰기

```cmake
set(PIXELICA_BUILD_SKELETON ON CACHE BOOL "" FORCE)
set(PIXELICA_BUILD_IK ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/Pixelica)
target_link_libraries(my_game PRIVATE Pixelica::core Pixelica::skeleton Pixelica::ik)
```

`Pixelica::skeleton`/`Pixelica::ik`는 `pixelica_common`에 PUBLIC 링크하므로 수학·포즈 계산만 쓰면 3D 렌더러가 필요 없습니다.
위 예제처럼 3D 화면을 그리는 앱은 `Pixelica::core`를 직접 링크합니다. 하위 프로젝트로 쓸 때는
`PIXELICA_BUILD_TESTS`/`PIXELICA_BUILD_EXAMPLES`가 기본 `OFF`이고, 모듈 타깃은 그것과 무관하게 빌드됩니다.

## 2. 설치와 컴포넌트

설치 규칙은 컴포넌트로 나뉩니다.

| 컴포넌트 | 내용 |
|---|---|
| `common` | 공용 라이브러리·헤더와 패키지 설정. 렌더러 없는 스켈레톤·IK 앱의 기반 |
| `core` | 공용 구성까지 포함한 완전한 3D SDK: 3D 라이브러리·헤더, `PixelicaTargets.cmake`, 템플릿, `pixelica_pack` 도구 |
| `skeleton` | `libpixelica_skeleton`, `include/pixelica/skeleton.h`, `PixelicaSkeletonTargets*.cmake` (구성된 경우에만 존재) |
| `ik` | `libpixelica_ik`, `include/pixelica/ik.h`, `PixelicaIKTargets*.cmake` (구성된 경우에만 존재) |

`core` 하나로 기존 3D SDK를 설치할 수 있으며, 렌더러가 필요 없으면 `common`을 먼저 설치합니다. 선택 모듈은 **자기 export 세트**(`PixelicaSkeletonTargets`,
`PixelicaIKTargets`)를 가집니다. 코어 export는 선택 모듈에 의존하지 않습니다.

```sh
# 구성된 모든 모듈을 설치 (옵션을 켠 모듈만 포함됨)
cmake --install build --config Release --prefix /path/to/prefix

# 코어만: 선택 모듈의 바이너리/export 없이도 동작
cmake --install build --config Release --prefix /path/to/prefix --component core
# 그 위에 선택 모듈을 추가 (core 또는 common이 먼저 설치되어 있어야 함)
cmake --install build --config Release --prefix /path/to/prefix --component skeleton
cmake --install build --config Release --prefix /path/to/prefix --component ik
```

기본 설정(`OFF`)으로 새 prefix에 설치하면 선택 헤더·라이브러리·export 파일이 **하나도** 생기지 않습니다. 스켈레톤만 / IK만 /
둘 다 / 코어만 설치하는 네 경우 모두 위 명령으로 만들 수 있습니다.

### 2.1 `find_package` 컴포넌트

```cmake
find_package(Pixelica 0.1 CONFIG REQUIRED)                          # 코어만 (기본)
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS skeleton ik)   # 둘 다 필수
find_package(Pixelica 0.1 CONFIG REQUIRED OPTIONAL_COMPONENTS ik)   # IK는 있으면 사용

target_link_libraries(my_game PRIVATE Pixelica::core)
if(Pixelica_skeleton_FOUND)
  target_link_libraries(my_game PRIVATE Pixelica::skeleton)
endif()
```

규칙(`cmake/PixelicaConfig.cmake.in`):

- 컴포넌트 없이 호출하면 설치된 3D SDK의 코어를 읽습니다. 2D 전용 설치에서는 2D 렌더러, 공용만 설치했으면 공용 타깃을 읽습니다.
  `COMPONENTS skeleton ik`는 요청한 모듈과 공용 타깃만 읽으므로 3D 렌더러가 없어도 됩니다. 3D 앱은 `core`도 명시할 수 있습니다.
- 선택 export 파일은 **요청한** 컴포넌트에 대해서만 읽습니다. 요청하지 않으면 읽지 않습니다(기본 `find_package`는 추가 타깃을
  만들지 않음).
- `Pixelica_skeleton_FOUND` / `Pixelica_ik_FOUND`는 요청된 컴포넌트마다 계산됩니다. 필요한 export 파일과 공용 의존성이 있거나
  같은 이름의 타깃이 이미 있어야 합니다. 빌드 당시 옵션을 패키지 설정에 고정하지 않습니다.
- 알 수 없는 컴포넌트 이름은 `Pixelica_<이름>_FOUND`가 거짓입니다.
- 못 찾은 컴포넌트는 `REQUIRED`/`COMPONENTS`로 요청했을 때만 `check_required_components`가 오류로 만듭니다.
  `OPTIONAL_COMPONENTS`로 요청하면 조용히 거짓으로 남고 하드 실패하지 않습니다.
- 같은 프로세스에서 `find_package(Pixelica)`를 여러 번(컴포넌트를 바꿔 가며) 호출해도 타깃을 두 번 정의하지 않습니다
  (`if(NOT TARGET ...)` 가드).

## 3. 스켈레톤 모듈 (`pixelica/skeleton.h`)

계층 본의 **포즈 계산**과 **정점 위치만의 선형 블렌드 스키닝**(법선 없음, 라이트 없는 렌더러용)입니다. 클립 파일 포맷,
쿼터니언, 블렌드 트리, 임포터는 없습니다. 회전은 호출자가 만든 4x4 행렬(`pixelica_mat4_rotation_*`, `pixelica_mat4_translation` 등)로
넘깁니다.

### 3.1 행렬 규약

- `float[16]`, **열 우선(column-major)**, 열 벡터에 작용: 원소 (행 r, 열 c)는 `m[c*4 + r]`, 이동은 `m[12..14]`
  ([`pixelica/math.h`](../include/pixelica/math.h)와 같음).
- 이 모듈의 모든 행렬은 **아핀(affine)** 이어야 합니다: 16개 원소가 모두 유한하고 아래 행이 정확히 `(0,0,0,1)`
  (`m[3]==m[7]==m[11]==0`, `m[15]==1`; `-0.0`은 0으로 취급). 출력 행렬의 아래 행은 항상 `+0, +0, +0, +1`입니다.
- 행렬 배열 매개변수는 `const float (*)[16]` / `float (*)[16]`입니다. C11에서는 `float (*)[16]`을
  `const float (*)[16]`으로 암묵 변환할 수 없어 `-Wpedantic`이 경고하므로 입력 쪽은 캐스트를 둡니다
  (데모와 테스트의 `CONST_MATRICES`/`CM` 매크로).

### 3.2 좌표 공간

- 본의 **world** 행렬은 "본 → **모델** 공간"입니다. 오브젝트가 장면 어디에 놓이는지는 렌더러에 주는 **model 행렬**
  (`pixelica_draw_mesh`의 `model`)이 담당합니다. 팔레트·스킨 결과도 모델 공간입니다.
- `inverse_bind` = 그 본의 **바인드 포즈 world 행렬의 역행렬**(모델 → 바인드 시점의 본 공간). 모듈은 역행렬 여부나 바인드
  포즈와의 일치를 검사하지 않습니다.
- 팔레트 `palette[i] = world[i] * inverse_bind[i]`는 **바인드 포즈 모델 공간 정점**을 **포즈된 모델 공간 위치**로 옮깁니다.
  바인드 포즈 그대로 포즈하면 팔레트는 (반올림 오차 범위에서) 단위 행렬입니다.

### 3.3 자료 구조

```c
#define PIXELICA_SKIN_INFLUENCES 4u
#define PIXELICA_SKELETON_MAX_BONES 65536u

typedef struct PIXELICA_Bone      { int32_t parent; float inverse_bind[16]; } PIXELICA_Bone;
typedef struct PIXELICA_Skeleton  { const PIXELICA_Bone *bones; uint32_t bone_count; } PIXELICA_Skeleton;
typedef struct PIXELICA_SkinWeight{ uint16_t bones[4]; float weights[4]; } PIXELICA_SkinWeight;
```

- `parent`는 루트면 `-1`, 아니면 **자기보다 앞선** 본의 인덱스(`0 <= parent < i`)입니다. 이 엄격한 "부모 먼저" 순서가
  순환, 자기 부모, 아직 계산하지 않은 후행 본 참조를 모두 배제합니다. 루트는 여럿이어도 됩니다.
- `bone_count`는 1..65536. 빈 스켈레톤은 유효하지 않습니다.

### 3.4 `pixelica_skeleton_pose`

```c
PIXELICA_Status pixelica_skeleton_pose(const PIXELICA_Skeleton *skeleton,
                             const float (*local)[16],
                             float (*world)[16], float (*palette)[16],
                             float (*scratch)[16], size_t scratch_matrices);
```

`world[i] = local[i]`(루트) 또는 `world[parent] * local[i]`, `palette[i] = world[i] * inverse_bind[i]`.
부모 world는 부모에 대해 저장되는 **float 값**입니다. 중간 계산은 double이고 저장 값마다 한 번 float로 반올림하므로 float
행렬 곱 사슬과 반올림 수준으로 다를 수 있습니다(ulp 한계는 약속하지 않음).

소유권과 워크스페이스(N = `bone_count`):

| 배열 | 크기 | 비고 |
|---|---|---|
| `skeleton->bones` | N | 읽기 전용 |
| `local` | N | 읽기 전용, 본마다 아핀 |
| `world`, `palette` | N (각각) | 출력 |
| `scratch` | `scratch_matrices >= 2*N` | 작업 공간: 앞 N개는 world, 다음 N개는 palette로 쓰이고 호출 뒤 내용은 규정되지 않음 |

- 크기 확장(extent)은 호출자 책임입니다. 힙이나 가변 길이 스택 배열을 쓰지 않으므로 호출자가 정적/스택 배열을 줍니다.
- 함수가 끝나면 어떤 포인터도 보관하지 않습니다.
- 모든 계산과 float 범위 검사를 `scratch`에서 끝낸 **뒤에야** `world`/`palette`에 씁니다. 따라서 **어떤 실패에서도**
  `world`/`palette`는 바이트 단위로 변하지 않습니다(`scratch`는 바뀔 수 있음).

에일리어싱: `world`, `palette`, 사용되는 `scratch` 범위는 서로, 그리고 `local`·`bones` 배열·`*skeleton` 객체와 **겹치면 안
됩니다**(정확히 같은 주소도, 부분 겹침도, 주소 범위가 한 바퀴 도는 경우도 거절). 제자리(in-place) 포즈는 없습니다. 읽기 전용
입력끼리는 같은 배열을 가리켜도 됩니다. 서로 맞닿기만 한 범위는 겹침이 아닙니다.

### 3.5 `pixelica_skeleton_skin`

```c
PIXELICA_Status pixelica_skeleton_skin(const PIXELICA_Vec3 *bind_positions,
                             const PIXELICA_SkinWeight *weights, size_t vertex_count,
                             const float (*palette)[16], uint32_t bone_count,
                             PIXELICA_Vec3 *out_positions,
                             PIXELICA_Vec3 *scratch, size_t scratch_vertices);
```

정점 v의 결과 = `sum( n_k * (palette[bone_k] * bind_v) )`, 여기서 `n_k = w_k / sum(w)`.

가중치 규칙:

- 4개 슬롯 **모두** 유한하고 `[0, 1]`이어야 합니다(0 슬롯 포함).
- 가중치가 양수인 슬롯의 본 인덱스는 `< bone_count`여야 합니다. 가중치 0인 슬롯의 인덱스는 무시됩니다(아무 값이나 가능).
- 합은 양수이고 `|합 - 1| <= 1e-4`여야 합니다. 이 범위의 합은 **실제 합으로 정규화**하므로, 모든 영향 본이 같은 위치로 보내는
  점은 거의 정규화된 입력에서도 그대로 유지됩니다. 범위 밖이거나 합이 0이면 `PIXELICA_ERR_INVALID_ARG`.
- 팔레트는 **참조 여부와 무관하게** `bone_count`개 전부 아핀·유한해야 하고, 모든 바인드 위치가 유한해야 합니다.
- 계산은 double로 하고 float 범위를 확인한 뒤 한 번만 float로 반올림합니다(오버플로면 `INVALID_ARG`).

워크스페이스: `scratch_vertices >= vertex_count`(아니면 `PIXELICA_ERR_NO_MEMORY`). 결과는 `scratch`에서 만들고 모든 정점이
성공한 뒤에만 `out_positions`에 씁니다. 실패하면 `out_positions`는 변하지 않습니다.

에일리어싱: `scratch`는 다른 모든 배열과 겹치면 안 됩니다. `out_positions`는 `weights`·`palette`와 겹치면 안 되고,
`bind_positions`와는 **정확히 같은 주소**(제자리 스키닝)만 허용되며 부분 겹침은 거절됩니다.

`out_positions`의 `vertex_count`개 정점은 호출자 소유이고 그대로 `PIXELICA_Mesh.vertices`로 쓸 수 있습니다. 바인드 메시는 읽기 전용
으로 두고 매 업데이트마다 별도 배열에 스키닝하는 방식이 기본 사용법입니다(데모).

### 3.6 오류 순서

`PIXELICA_ERR_INVALID_ARG`: NULL 필수 포인터, 0 또는 너무 큰 개수, 크기 계산 오버플로, 금지된 겹침/주소 범위 wrap, 유한하지
않거나 아핀이 아니거나 범위를 벗어난 데이터, float에 담기지 않는 결과. `PIXELICA_ERR_NO_MEMORY`는 워크스페이스가 작을 때만.
검사 순서는 "포인터·개수 → 워크스페이스 크기 → 주소 범위 → 데이터"입니다. 데이터 검증이 `scratch`를 건드리기 전에 끝나는지는 API마다
다릅니다.

- `pixelica_skeleton_pose`: 입력 전체 검증을 `scratch`에 손대기 전에 끝냅니다.
- `pixelica_skeleton_skin`: 팔레트 전체를 먼저 검증한 뒤, 정점을 하나씩 검증하며 `scratch`에 계산합니다. 뒤쪽 정점이 잘못되면 그 전에
  `scratch`가 이미 바뀌어 있을 수 있습니다.

두 API 모두 어떤 실패에서도 출력 버퍼(`world`/`palette`, `out_positions`)는 바이트 단위로 변하지 않습니다. 실패 뒤 `scratch`
내용은 규정되지 않습니다(`skeleton.h`와 같음).

### 3.7 예

설치된 스켈레톤 SDK만으로 빌드되는 완결된 C11 프로그램입니다(IK 의존 없음). 본 2개, 바인드 점 `(2,0,0)`, 아래 본의 inverse bind는
`T(-1,0,0)`, 로컬 행렬은 루트 `I`, 아래 본 `T(1,0,0) * Rz(pi/2)`입니다. 점은 아래 본에만 묶여 있으므로 기대 결과는
`(1,1,0)`입니다. 모든 배열은 호출자 소유이고 동적 할당은 없으며 모든 상태를 검사합니다.

```c
#include <math.h>
#include <stdio.h>

#include "pixelica/math.h"
#include "pixelica/skeleton.h"

#define BONES 2u
#define VERTS 1u
#define HALF_PI 1.57079632679489661923f
#define CONST_MATRICES(p) ((const float (*)[16])(const void *)(p))

int main(void)
{
    PIXELICA_Bone bones[BONES];
    PIXELICA_Skeleton skeleton;
    float local[BONES][16];
    float world[BONES][16];
    float palette[BONES][16];
    float scratch[2u * BONES][16];
    float trans[16];
    float rot[16];
    PIXELICA_Vec3 bind[VERTS] = {{2.0f, 0.0f, 0.0f}};
    PIXELICA_SkinWeight weights[VERTS] = {{{1u, 0u, 0u, 0u}, {1.0f, 0.0f, 0.0f, 0.0f}}};
    PIXELICA_Vec3 skinned[VERTS];
    PIXELICA_Vec3 skin_scratch[VERTS];
    PIXELICA_Status st;

    /* 바인드 포즈: bone0 = I, bone1 = T(1,0,0). inverse_bind는 그 world의 역. */
    bones[0].parent = -1;
    pixelica_mat4_identity(bones[0].inverse_bind);
    bones[1].parent = 0;
    pixelica_mat4_translation(bones[1].inverse_bind, -1.0f, 0.0f, 0.0f);
    skeleton.bones = bones;
    skeleton.bone_count = BONES;

    /* 현재 포즈: 아래 본을 T(1,0,0) * Rz(pi/2)로 둠. */
    pixelica_mat4_identity(local[0]);
    pixelica_mat4_translation(trans, 1.0f, 0.0f, 0.0f);
    pixelica_mat4_rotation_z(rot, HALF_PI);
    pixelica_mat4_multiply(local[1], trans, rot);

    st = pixelica_skeleton_pose(&skeleton, CONST_MATRICES(local), world, palette, scratch,
                           2u * BONES);
    if (st != PIXELICA_OK) {
        fprintf(stderr, "pixelica_skeleton_pose failed: %d\n", (int)st);
        return 1;
    }
    st = pixelica_skeleton_skin(bind, weights, VERTS, CONST_MATRICES(palette), BONES, skinned,
                           skin_scratch, VERTS);
    if (st != PIXELICA_OK) {
        fprintf(stderr, "pixelica_skeleton_skin failed: %d\n", (int)st);
        return 1;
    }

    printf("skinned = (%.4f, %.4f, %.4f)\n", (double)skinned[0].x, (double)skinned[0].y,
           (double)skinned[0].z);
    if (fabsf(skinned[0].x - 1.0f) > 1e-4f || fabsf(skinned[0].y - 1.0f) > 1e-4f ||
        fabsf(skinned[0].z) > 1e-4f) {
        fprintf(stderr, "unexpected result, expected (1, 1, 0)\n");
        return 1;
    }
    /* 렌더할 때는 skinned를 그대로 PIXELICA_Mesh.vertices로 씁니다. */
    return 0;
}
```

설치된 SDK(`--component core`와 `--component skeleton`을 설치한 prefix)로 빌드하는 `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(rig_example C)

find_package(Pixelica CONFIG REQUIRED COMPONENTS skeleton)

add_executable(rig_example main.c)
target_compile_features(rig_example PRIVATE c_std_11)
target_link_libraries(rig_example PRIVATE Pixelica::skeleton)
```

```sh
cmake -S . -B build-rig-example -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build build-rig-example --config Release
```

## 4. IK 모듈 (`pixelica/ik.h`)

루트 → 관절 → 끝으로 이루어진 **두 본 체인**을 3D 해석해로 풉니다. 임의 길이 체인, 각도 제한, 늘어남(stretch), 스켈레톤
자동 수정은 없습니다. 결과는 **관절·끝 위치뿐**이고, 위치를 회전/로컬 행렬로 바꾸는 일은 호출자가 합니다.

```c
typedef struct PIXELICA_IKTwoBone {
    PIXELICA_Vec3 root, target, pole;      /* 같은 좌표 공간 (모델 또는 월드) */
    float upper_length, lower_length; /* root->joint, joint->end */
} PIXELICA_IKTwoBone;
typedef struct PIXELICA_IKTwoBoneResult { PIXELICA_Vec3 joint, end; bool clamped; } PIXELICA_IKTwoBoneResult;
PIXELICA_Status pixelica_ik_two_bone(const PIXELICA_IKTwoBone *chain, PIXELICA_IKTwoBoneResult *out);
```

계약:

- 모든 벡터 성분이 유한하고, 두 길이가 유한한 양수이며, 필수 포인터가 NULL이 아니어야 합니다. 아니면 `PIXELICA_ERR_INVALID_ARG`.
  결과(joint/end)가 float에 담기지 않는 경우도 `INVALID_ARG`입니다. 실패하면 `*out`은 변하지 않습니다. `out`이 `chain`과
  겹쳐도 됩니다(읽기를 모두 끝낸 뒤 씀).
- O(1), 힙·숨은 상태·시계 없음. 내부는 double이라 float 범위 값의 제곱이나 아주 짧은 길이에서도 넘치거나 0으로 사라지지
  않습니다.
- **거리 clamp**: `d = |target - root|`를 `[|L0 - L1|, L0 + L1]`로 자릅니다. 입력 거리가 이 닫힌 구간 **밖**이면
  `clamped = true`. clamp된 풀이도 성공(`PIXELICA_OK`)이며 각도 제한도 늘어남도 없습니다. 닿을 수 있으면 `end == target`
  (정확히), 아니면 `end = root + forward * clamped_d` (`forward`는 `target - root`의 단위 벡터).
- **pole은 점(point)** 입니다. `pole - root`를 `forward`에 수직인 평면에 투영한 방향이 관절이 꺾이는 쪽을 정합니다.
- pole이 체인 축 위(투영이 0이거나 `|pole - root|`의 `1e-12`배 이하)이거나 `pole == root`이면 **결정적 대체 방향**을
  씁니다: `forward`와 가장 덜 평행한 좌표축(성분 절댓값이 가장 작은 축, 동률이면 X, Y, Z 순)을 투영·정규화.
- `target == root`는 방향이 없어 `forward = +X`로 정합니다. 길이가 같은 체인이 완전히 접힌 경우(`d == 0`) `end == root`,
  `joint = root + bend * upper_length`.
- 그 밖에는 코사인 법칙의 수치 안정형을 씁니다: `a = 0.5*(d + (L0-L1)*(L0+L1)/d)`를 `[-L0, L0]`로 자르고
  `h = sqrt(max(0, L0² - a²))`, `joint = root + forward*a + bend*h`.
- 출력은 float로 반올림되므로 좌표가 매우 크고 길이가 매우 작으면 본 길이는 float 정밀도까지만 유지됩니다. 플랫폼 간 비트
  단위 동일 결과는 약속하지 않습니다.

예(XY 평면 체인을 본 각도로 바꾸기). 아래는 **설명용으로 줄인 조각**이며 단독으로 컴파일되지 않습니다. `target`은 호출자가 주는
값이고, 구한 각도는 게임이 소비합니다:

```c
#include <math.h>
#include "pixelica/ik.h"

PIXELICA_IKTwoBone chain = { {0,0,0}, target, {0,2,0}, 1.6f, 1.4f };   /* target: 호출자 제공. pole = root + (0,2,0) */
PIXELICA_IKTwoBoneResult r;
if (pixelica_ik_two_bone(&chain, &r) == PIXELICA_OK) {
    float a0 = atan2f(r.joint.y - chain.root.y, r.joint.x - chain.root.x);
    float a1 = atan2f(r.end.y - r.joint.y,       r.end.x - r.joint.x);
    /* local0 = T(root) * Rz(a0);  local1 = T(upper_length,0,0) * Rz(a1 - a0)  (부모 기준 상대 각도) */
    /* a0, a1, r.clamped("닿지 않음")는 게임이 소비하는 값 */
}
```

주의: pole과 `forward`가 거의 평행해지는 경로(예: 타깃이 pole 방향 축 위)는 대체 방향으로 갑자기 바뀔 수 있습니다.
데모는 타깃의 X를 항상 양수로 두어 pole `(0,2,0)`과 평행해지지 않게 합니다.

## 5. 함께 쓰기와 데모 `hello_rig`

[`examples/hello_rig/hello_rig.c`](../examples/hello_rig/hello_rig.c)는 `PIXELICA_BUILD_SKELETON=ON`과 `PIXELICA_BUILD_IK=ON`이 모두
켜졌을 때만 빌드됩니다(`examples/CMakeLists.txt`). 256x192에서:

- 읽기 전용 **바인드 메시**: X 방향 11개 열(열마다 위·아래 정점 2개, 총 22개)의 띠. 관절(x = 1.6) 주변 열은 0.75/0.25,
  0.5/0.5, 0.25/0.75로 두 본 가중치를 섞어 부드럽게 꺾입니다.
- 두 본 계층(루트 본 → 관절 본) + 역바인드 행렬. 루트는 모델 원점이고 바인드 메시도 같은 공간에서 작성되었습니다.
- 타깃은 `PIXELICA_AnimPlayer`(LOOP, 360틱)와 키프레임 트랙을 따라 움직이며 닿는 구간과 clamp 구간(너무 멂, 너무 가까움)을
  모두 지납니다. 닿으면 초록, clamp되면 빨강입니다.
- 매 고정 업데이트마다: IK → `atan2f`로 본 각도 → 로컬 행렬 → `pixelica_skeleton_pose` → `pixelica_skeleton_skin`으로 **실제로 변형된**
  띠를 상태가 가진 배열에 만들고(호출자 소유 워크스페이스 사용) 그것을 `PIXELICA_Mesh.vertices`로 그립니다. 본 선분(흰 상자), 관절(노랑),
  pole(파랑), 타깃(상자) 표시를 함께 그립니다.
- 오브젝트 배치는 model 행렬(`T(-1.4, -0.4, 0)`)이 따로 담당하고 IK·팔레트는 모두 모델 공간입니다.
- A는 일시정지/재개(배경 색조가 바뀜), START는 틱 0(정확히 초기 포즈)으로 재시작합니다. 상태 오류는 저장되어 렌더가 첫
  실패를 반환합니다. 프레임마다 할당하지 않습니다.

```sh
cmake -S . -B build-hello-rig -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_SKELETON=ON -DPIXELICA_BUILD_IK=ON
cmake --build build-hello-rig --config Debug --target hello_rig
build-hello-rig/Debug/hello_rig.exe --frames 120 --hidden     # Windows (다중 구성)
./build-hello-rig/hello_rig --frames 120 --hidden             # 단일 구성 생성기
```

CTest에는 `smoke_hello_rig`(`--frames 30 --hidden`)가 등록됩니다. 이 스모크는 종료 코드만 보며 화면 내용은 검사하지
않습니다.

## 6. 테스트

| 테스트 | 파일 | 등록 조건 |
|---|---|---|
| `test_skeleton` | [`tests/test_skeleton.c`](../tests/test_skeleton.c) | `PIXELICA_BUILD_SKELETON=ON` (아니면 루트 glob이 건너뜀) |
| `test_ik` | [`tests/test_ik.c`](../tests/test_ik.c) | `PIXELICA_BUILD_IK=ON` (아니면 건너뜀) |
| `smoke_hello_rig` | [`examples/CMakeLists.txt`](../examples/CMakeLists.txt) | 두 옵션 모두 `ON` + 예제 빌드 |

루트 CMake는 `tests/test_*.c`를 glob하되 비활성 모듈의 테스트는 건너뛰므로(없는 헤더 때문에 깨지지 않음) 옵션이 꺼진 기본
구성은 기존 테스트 집합 그대로입니다. 테스트는 `assert`가 아니라 `CHECK` 매크로를 쓰므로 Release에서도 활성입니다.
`test_skeleton`은 바인드 포즈 항등, 계층·다중 루트, 역바인드 팔레트, 2본/4본 블렌드, 거의 정규화된 가중치, 제자리 스키닝,
전체 입력 검증(참조되지 않은 팔레트·역바인드, 사슬 끝의 NaN/inf/오버플로), 부모 검증(순환·자기·후행 참조·미지의 음수), 워크스페이스
부족과 에일리어싱/부분 겹침, 실패 시 출력 바이트 보존, double 기준과의 결정적 랜덤 비교를 다룹니다.

## 7. ESP32-P4 준비 (빌드 준비만)

`firmware/esp32p4/CMakeLists.txt`에 같은 이름의 옵션 `PIXELICA_BUILD_SKELETON`, `PIXELICA_BUILD_IK`(기본 `OFF`)이 있습니다. 켠 모듈의
래퍼 컴포넌트 디렉터리만 `EXTRA_COMPONENT_DIRS`에 덧붙이므로(IDF 프로젝트 include 전에 결정) 기본 펌웨어에는 어느 모듈도
들어가지 않습니다.

```sh
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py -DPIXELICA_BUILD_SKELETON=ON -DPIXELICA_BUILD_IK=ON build
```

- 래퍼: `firmware/esp32p4/optional_components/pixelica_skeleton/CMakeLists.txt`, `.../pixelica_ik/CMakeLists.txt`. 저장소의
  `modules/*/` 소스와 `include/`를 가리키고, `REQUIRES pixelica`입니다. 모듈 소스는 `pixelica` 컴포넌트의 코어 소스
  목록과 `src/` 드리프트 검사에 들어가지 않습니다.
- 저장소 루트는 래퍼 디렉터리에서 위로 4단계(`../../../..`)이며 환경 변수 `PIXELICA_ROOT`로 바꿀 수 있습니다.
- 래퍼는 `idf_component_register()` 앞에 CMake 스크립트 모드에서도 유효한 명령만 둡니다(타깃 생성 없음).
- 사용 쪽 컴포넌트(예: `main`)의 `idf_component_register`에 `REQUIRES pixelica_skeleton` / `pixelica_ik`를 추가해야 헤더가 보입니다.
- 컴파일러 감사 기록은 `docs/RIGGING_AUDIT.md` (local-only record)에 있습니다. 공식 ESP-IDF v6.0 `esp32p4`에서 두 래퍼를
  모두 `ON`으로 컴파일했습니다. `OFF` 구성에서도 두 모듈이 제외되고 펌웨어가 다시 빌드되는 것을 확인했습니다.
  보드 실행과 성능 측정은 하지 않았습니다.

## 8. 하지 않는 것(한계)

- 클립/애니메이션 에셋 포맷, 임포터(glTF 등), 쿼터니언 API, 블렌드 트리, 이벤트: 없음. 본 회전은 호출자가 행렬로 만듭니다.
  기본 애니메이션 모듈의 키프레임 샘플링과 플레이어를 각도/위치 입력에 쓸 수 있습니다.
- 임의 길이 IK 체인, FABRIK/CCD, 각도 제한, 늘어남: 없음(두 본 해석해만).
- 법선 스키닝, 정점 단위 라이트: 없음. 렌더러가 라이트 없는 플랫 컬러이므로 위치만 스키닝합니다.
- **정점당** 4개 영향까지, 본 최대 65536개(인덱스 `uint16_t`).
- 비용은 O(N 본 + V 정점 * 4)이고 워크스페이스 메모리는 결정적입니다(호출자 배열 크기가 곧 한계). `float`=4바이트일 때:
  - 포즈: world + palette + scratch = `256 * N` 바이트(각 행렬 64바이트 * N * 4배열). 입력 `local`(`64 * N`)과 `PIXELICA_Bone`
    배열(`68 * N`)은 제외.
  - 스킨: 출력 + scratch = `24 * V` 바이트(`PIXELICA_Vec3` 12바이트 * V * 2). 바인드 위치(`12 * V`)와 가중치(보통 `24 * V`)는
    제외. 이식 가능한 배치는 `sizeof`로 계산하세요.
  이 문서는 하드웨어 시간 수치를 제시하지 않습니다.
- 두 모듈은 정적 라이브러리입니다. 공유 라이브러리/동적 로딩 경로는 없습니다.

## 9. 검증 기록

전체 절차와 결과는 `docs/RIGGING_AUDIT.md` (local-only record)에 있습니다(2026-10-10 기준). 핵심만 요약합니다.

- 테스트: Windows Debug/Release SDL 26/26, Linux SDL Release 26/26, Linux headless ASan+UBSan+leak detection 24/24,
  ARM64 QEMU 24/24.
- 설치된 SDK로 만든 독립 소비자·데모·최대 65536본 테스트 3/3: Windows Release와 Linux 새니타이저.
- 모듈 ON/OFF 네 조합 설치, `core`/`skeleton`/`ik` 컴포넌트 개별 설치, `find_package`의 required/optional/unknown/반복 호출,
  설치 위치 이동(relocation) 모두 통과.
- 코어 소스+공개 헤더 25개 체크섬과 코어 Release 아카이브는 네 조합에서 변하지 않았습니다.
- Linux에서 계측한 데모의 update/render 중 `malloc`/`calloc`/`realloc`/`free` 호출은 0회였습니다.
- Windows와 Linux의 RGB565 프레임 91장이 바이트 단위로 같았고, 이 프레임으로 90프레임/6초 GIF를 만들었습니다.
- ESP32-P4: 공식 ESP-IDF v6.0 `esp32p4`에서 두 래퍼를 모두 `ON`으로 컴파일했습니다. `OFF` 구성의 모듈 제외와 재빌드도
  확인했으며, 보드 실행과 성능 측정은 하지 않았습니다.

데모 `hello_rig`의 실제 렌더링 캡처입니다(목업이 아님):

- `verification/hello_rig.gif` (local-only record)
- `verification/hello_rig.png` (local-only record)
