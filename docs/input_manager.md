# 선택 모듈: 입력 매니저 (Input Manager)

> **2026-10-10 추가.** 입력 소스(키보드, 컨트롤러, 사용자 정의 하드웨어)가 올려 주는 **정규화된 스냅샷**을 **게임 액션**으로 바꿔 주는 선택 모듈입니다.
> 2D·3D 렌더러가 똑같이 씁니다. 기본값은 `OFF`이며 기본 빌드·기본 설치·기존 SDK 압축 파일에는 헤더, 라이브러리, 심볼이 하나도 들어가지 않습니다.
> 이 문서는 **계약과 사용법**을 설명합니다. 빌드·테스트 실행 결과, 개수, 해시는 `INPUT_MANAGER_AUDIT.md` (local-only record)(감사 기록)가 기준이며
> 이 문서는 그런 수치를 적지 않습니다. 실물 키보드, 컨트롤러, 보드에서 검증했다는 뜻이 **아닙니다**(11장, 14장).

| 항목 | 값 |
|---|---|
| 옵션 | `PIXELICA_BUILD_INPUT_MANAGER` (기본 `OFF`, 최상위 프로젝트일 때도 `OFF`) |
| 타깃 | `pixelica_input_manager` / `Pixelica::input_manager` (STATIC, Debug 접미사 `d`) |
| 의존성 | `Pixelica::common`만 (렌더러, SDL, 시계, 힙 없음) |
| 헤더 | [`pixelica/input_manager.h`](../modules/input_manager/include/pixelica/input_manager.h) (현재 공개 API 계약) |
| 소스 | [`modules/input_manager/input_manager.c`](../modules/input_manager/input_manager.c) |
| 설치 컴포넌트 | `input_manager` (export 파일 `PixelicaInputManagerTargets.cmake`) |
| 선택적 SDL3 어댑터 | `Pixelica::input_sdl3` — **소스 트리 전용, 설치되지 않음** (12장) |

## 목차

1. [목적과 데이터 흐름](#1-목적과-데이터-흐름)
2. [빌드와 설치](#2-빌드와-설치)
3. [고정 한계와 메모리](#3-고정-한계와-메모리)
4. [장치 스냅샷, 액션, 바인딩](#4-장치-스냅샷-액션-바인딩)
5. [API 목록과 오류 의미](#5-api-목록과-오류-의미)
6. [값 계산 규칙](#6-값-계산-규칙)
7. [틱과 엣지(pressed / held / released)](#7-틱과-엣지pressed--held--released)
8. [연결 끊김, 포커스 상실, 재바인딩](#8-연결-끊김-포커스-상실-재바인딩)
9. [레거시 브리지와 P3IN](#9-레거시-브리지와-p3in)
10. [하드웨어 어댑터 작성 지침](#10-하드웨어-어댑터-작성-지침)
11. [완전한 예제](#11-완전한-예제)
12. [SDL3 어댑터 (소스 트리 전용)](#12-sdl3-어댑터-소스-트리-전용)
13. [기존 PAL·예제와 함께 쓸 때의 주의](#13-기존-pal예제와-함께-쓸-때의-주의)
14. [범위, 확장 경계, 단계 계획](#14-범위-확장-경계-단계-계획)

---

## 1. 목적과 데이터 흐름

기존 엔진은 입력을 논리 버튼 7비트(`PIXELICA_BTN_*`, [`input.h`](../include/pixelica/input.h))로만 다룹니다. 입력 매니저는 그 앞단에 다음 층을 더합니다.

```text
조각(fragment): 데이터 흐름
입력 소스 (어댑터가 소유)            매니저 (호출자가 구조체 소유)               게임
 source 0  키보드류 스냅샷   ┐
 source 1  패드 스냅샷       ├─ submit ─> devices[4]
 source 2  사용자 정의 HW    │              │ 바인딩 (최대 64)
 source 3  ...               ┘              v
                                   tick: 액션 값 합산, 임계값, 엣지
                                            v
                                   pixelica_input_manager_action(id) ──> 게임 로직
                                   pixelica_input_manager_buttons()  ──> 레거시 7비트 마스크
```

- **소스(source)** 는 최대 4개의 장치 슬롯(인덱스 0..3)입니다. 슬롯에 무엇이 연결되는지는 어댑터가 정합니다. 매니저는 SDL, GPIO, ADC, 시계를 모릅니다.
- **스냅샷**은 한 시점의 `connected`, 64비트 버튼 마스크, 정규화된 축 8개입니다. 어댑터가 `submit`으로 올립니다.
- **바인딩**은 "어느 소스의 어느 버튼/축 → 어느 액션, 크기·데드존"을 적은 표입니다. 게임이 설정합니다.
- **액션**은 게임이 읽는 논리 입력(32개)입니다. 값(`value`)과 `held`/`pressed`/`released`를 가집니다.
- 게임은 렌더러와 무관하게 같은 액션을 읽습니다. 렌더러(2D, 3D, 하이브리드)를 모두 꺼도 이 모듈은 빌드됩니다.

이 모듈이 **하는 일**: 스냅샷 검증과 보관, 바인딩 검증과 보관, 틱마다 액션 계산, 엣지 계산, 레거시 7비트 마스크 제공.
**하지 않는 일**: 이벤트 큐, 창/포커스 감지, 장치 열거, 키 이름·텍스트 입력(IME), 마우스·터치, 진동, 바인딩 편집 UI, 입력 전송(네트워크) 드라이버,
플레이어↔장치 할당 UI. 필요하면 어댑터나 게임이 만듭니다(14장).

## 2. 빌드와 설치

옵션은 다른 선택 모듈(`PIXELICA_BUILD_SKELETON`, `PIXELICA_BUILD_IK`, [`rigging.md`](rigging.md))과 같은 방식입니다. 켜지 않으면 `modules/input_manager`가 추가되지 않습니다.

### 2.1 소스 트리 빌드 (Windows / Linux)

렌더러를 전부 끈 최소 구성(공용 라이브러리 + 입력 매니저 + 테스트)입니다. 구성마다 **새 빌드 디렉터리**를 쓰세요(옵션은 캐시에 남습니다).

```sh
# Windows PowerShell (Visual Studio 생성기). Debug 와 Release 를 각각 빌드·테스트하세요.
cmake -S . -B build-input -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_3D=OFF -DPIXELICA_BUILD_2D=OFF -DPIXELICA_BUILD_INPUT_MANAGER=ON -DPIXELICA_BUILD_EXAMPLES=OFF
cmake --build build-input --config Debug
ctest --test-dir build-input -C Debug -R input_manager --output-on-failure
cmake --build build-input --config Release
ctest --test-dir build-input -C Release -R input_manager --output-on-failure
```

```sh
# Linux (단일 구성 생성기): 구성마다 빌드 디렉터리를 나눕니다.
cmake -S . -B build-input-debug -DCMAKE_BUILD_TYPE=Debug -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_3D=OFF -DPIXELICA_BUILD_2D=OFF -DPIXELICA_BUILD_INPUT_MANAGER=ON -DPIXELICA_BUILD_EXAMPLES=OFF
cmake --build build-input-debug --parallel
ctest --test-dir build-input-debug -R input_manager --output-on-failure
cmake -S . -B build-input-release -DCMAKE_BUILD_TYPE=Release -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_3D=OFF -DPIXELICA_BUILD_2D=OFF -DPIXELICA_BUILD_INPUT_MANAGER=ON -DPIXELICA_BUILD_EXAMPLES=OFF
cmake --build build-input-release --parallel
ctest --test-dir build-input-release -R input_manager --output-on-failure
```

Debug와 Release를 둘 다 확인하는 이유: 이 저장소의 테스트는 `assert`가 아니라 `CHECK`를 쓰므로 Release에서도 검사가 살아 있어야 하고, 이 문서의 예제도 같은 규칙을 따릅니다(11장). 경고를 오류로 보려면 `-DPIXELICA_WARNINGS_AS_ERRORS=ON`을 더합니다.
렌더러를 켠 일반 빌드에 추가하려면 해당 구성에 `-DPIXELICA_BUILD_INPUT_MANAGER=ON`만 더하면 됩니다.

### 2.2 `add_subdirectory`로 쓰기

```text
조각(fragment): 단독 CMakeLists.txt 가 아님
set(PIXELICA_BUILD_INPUT_MANAGER ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/Pixelica)
target_link_libraries(my_game PRIVATE Pixelica::input_manager)   # 렌더러가 필요하면 Pixelica::core / Pixelica2D::core 도 링크
```

`Pixelica::input_manager`는 `pixelica_common`에 PUBLIC 링크하므로 렌더러 없는 앱(헤드리스 도구, 입력 전용 테스트)에서도 쓸 수 있습니다. 헤더는 `#include "pixelica/input_manager.h"`로
직접 포함하며, `pixelica/pixelica.h`는 이 헤더를 포함하지 않습니다.

### 2.3 설치와 `find_package`

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_INPUT_MANAGER=ON -DPIXELICA_BUILD_EXAMPLES=OFF -DPIXELICA_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/prefix                              # 구성된 모든 것
cmake --install build --config Release --prefix /path/to/prefix --component common           # 공용 (입력 매니저의 의존성)
cmake --install build --config Release --prefix /path/to/prefix --component input_manager    # 입력 매니저만 추가
# Visual Studio 같은 다중 구성 생성기: 같은 prefix 에 --config Debug 로 다시 빌드·설치하면 `d` 접미사 라이브러리가 더해집니다.
```

```text
조각(fragment): 게임의 CMakeLists.txt 에서
find_package(Pixelica 0.1 CONFIG REQUIRED COMPONENTS input_manager)   # 또는 OPTIONAL_COMPONENTS
target_link_libraries(my_game PRIVATE Pixelica::input_manager)       # 렌더러를 쓰면 Pixelica::core 도
```

- 요청한 컴포넌트만 `PixelicaInputManagerTargets.cmake`를 읽습니다. 기본 `core`/`common` export는 입력 매니저에 의존하지 않습니다.
- 입력 매니저는 `common`에만 의존합니다. `common`이 설치되어 있어야 하며, 설치된 패키지 설정이 연결합니다.
- 설치된 CMake 파일에는 빌드·소스 경로와 `pixelica_options`(경고 플래그)가 남지 않습니다. Pixelica2D 패키지로 쓰는 경우에도 같은 `Pixelica::input_manager` 타깃을 명시적으로 요청할 수 있습니다.
- **현재 상태 (중요).** 이미 만들어 둔 2D·3D SDK 압축 파일(`Pixelica2D-*.zip`, `Pixelica3D-*.zip`, `out/sdk-release`)은 이 모듈을 넣어 **다시 빌드하지 않았습니다**.
  기존 압축 파일에는 입력 매니저가 **들어 있지 않습니다**. 지금 가능한 방법은 위의 소스 트리 빌드, 직접 설치, 소스 `add_subdirectory` 추가입니다.
  `tools/package_sdk.py`의 기본 구성도 바꾸지 않았습니다(기본 `OFF`).

## 3. 고정 한계와 메모리

| 상수 | 값 | 의미 |
|---|---|---|
| `PIXELICA_INPUT_MAX_DEVICES` | 4 | 소스(장치) 슬롯, 인덱스 0..3 |
| `PIXELICA_INPUT_MAX_BUTTONS` | 64 | 소스당 버튼 비트 (`uint64_t`) |
| `PIXELICA_INPUT_MAX_AXES` | 8 | 소스당 축 |
| `PIXELICA_INPUT_MAX_ACTIONS` | 32 | 액션 ID 0..31 |
| `PIXELICA_INPUT_MAX_BINDINGS` | 64 | 바인딩 항목 |

- `PIXELICA_InputManager`는 **호출자가 소유하는 구조체**이고(스택, 정적, 게임 상태 안 어디든) 배열이 구조체 안에 들어 있으므로 힙 할당이 없습니다.
  `pixelica_input_manager_init`로 먼저 초기화해야 하며, 초기화하지 않은 구조체를 쓰면 동작이 정의되지 않습니다.
- 바이트 크기는 ABI(패딩, 정렬)에 따라 다르므로 이 문서는 숫자를 적지 않습니다. 필요하면 `sizeof(PIXELICA_InputManager)`로 직접 재세요(11장 예제가 출력합니다).
- 구조체의 필드는 "구현 저장소"입니다. 읽기 전용으로 들여다보는 것은 가능하지만 `set_bindings`/`submit`을 거치지 않고 쓰면 검증을 우회합니다. 코어는 `tick`에서 저장된 바인딩을 다시 검사하고
  `binding_count`를 64로 제한하지만(손상 방어), 이것은 지원하는 사용법이 아닙니다.
- 단일 스레드 계약입니다. 인터럽트나 다른 스레드의 입력은 어댑터가 넘겨받은 뒤(10장) 게임 스레드에서 `submit`합니다. 잠금도 원자 연산도 없습니다.

## 4. 장치 스냅샷, 액션, 바인딩

### 4.1 장치 스냅샷 `PIXELICA_InputDeviceState`

| 필드 | 규칙 |
|---|---|
| `connected` | `false`면 장치가 없는 것으로 보고 **중립**(`buttons`=0, 축=0)으로 저장합니다. 이때 `buttons`/`axes` 내용은 무시됩니다(쓰레기 값이어도 됨). |
| `buttons` | 64비트 마스크. 비트 *n* = 버튼 *n*. 어떤 값이든 유효합니다. |
| `axes[8]` | 정규화된 `float`, **유한하고 `[-1, 1]`**. 트리거는 `[0, 1]`을 써도 됩니다. `connected`가 `true`일 때 하나라도 NaN/무한대/범위 밖이면 제출 전체가 거부됩니다. |

원시 ADC 값, 핀 극성, 보정은 **어댑터의 일**입니다. 매니저는 이미 정규화된 값만 받습니다(10장).

### 4.2 액션 ID

액션 ID `0..6`은 기존 `PIXELICA_BTN_*` 비트 위치와 같습니다. `7..31`은 게임이 자유롭게 씁니다(25개).

| ID | 열거자 | 대응하는 옛 버튼 |
|---|---|---|
| 0 | `PIXELICA_INPUT_LEFT` | `PIXELICA_BTN_LEFT` (`1<<0`) |
| 1 | `PIXELICA_INPUT_RIGHT` | `PIXELICA_BTN_RIGHT` (`1<<1`) |
| 2 | `PIXELICA_INPUT_UP` | `PIXELICA_BTN_UP` (`1<<2`) |
| 3 | `PIXELICA_INPUT_DOWN` | `PIXELICA_BTN_DOWN` (`1<<3`) |
| 4 | `PIXELICA_INPUT_A` | `PIXELICA_BTN_A` (`1<<4`) |
| 5 | `PIXELICA_INPUT_START` | `PIXELICA_BTN_START` (`1<<5`) |
| 6 | `PIXELICA_INPUT_QUIT` | `PIXELICA_BTN_QUIT` (`1<<6`) |
| 7..31 | 사용자 정의 | 레거시 마스크에 나타나지 않음 |

### 4.3 바인딩 `PIXELICA_InputBinding`

| 필드 | 규칙 |
|---|---|
| `device` | 소스 인덱스 `< 4` |
| `control` | `kind`가 버튼이면 `< 64`, 축 종류면 `< 8` |
| `action` | 액션 ID `< 32` |
| `kind` (`PIXELICA_InputControl`) | `PIXELICA_INPUT_BUTTON`, `PIXELICA_INPUT_AXIS`(부호 있는 축), `PIXELICA_INPUT_AXIS_POSITIVE`(양의 절반), `PIXELICA_INPUT_AXIS_NEGATIVE`(음의 절반) |
| `scale` | 유한한 `[-1, 1]`. 음수면 방향이 뒤집힙니다. 0도 허용(기여 0). |
| `deadzone` | 유한한 `[0, 1)`. 축 종류에만 적용, 버튼은 무시. |

헤더의 필드 이름이 `kind`(타입 `PIXELICA_InputControl`)와 `control`(인덱스)로 나뉘어 있는 점에 주의하세요. 같은 액션에 바인딩을 여러 개 걸 수 있습니다(여러 소스, 키와 축 혼합).

## 5. API 목록과 오류 의미

모든 API의 이름, 시그니처와 구조체 배치는 `pixelica/input_manager.h`가 기준입니다. 버전 0.1의 현재 계약이며, 향후 ABI 변경이 없는 영구 고정 API라는 뜻은 아닙니다.

| 함수 | 동작 | 실패/특수 입력 |
|---|---|---|
| `void pixelica_input_manager_init(m)` | 장치(연결 해제, 중립), 바인딩(0개), 모든 액션 값과 엣지를 초기화 | `NULL`이면 아무것도 안 함 |
| `PIXELICA_Status pixelica_input_manager_set_bindings(m, bindings, count)` | 바인딩 표를 **통째로 교체**(복사). `count==0`이면 모두 지움(`bindings`는 `NULL` 가능) | `m==NULL` → `INVALID_ARG`; `count>64` → `CAPACITY`; `count>0 && bindings==NULL` → `INVALID_ARG`; 항목 하나라도 4.3절 규칙 위반 → `INVALID_ARG` |
| `PIXELICA_Status pixelica_input_manager_submit(m, device, state)` | 해당 소스의 스냅샷을 **교체**("최신이 이긴다") | `m`/`state`가 `NULL`, `device>=4` → `INVALID_ARG`; `connected==true`인데 축이 유한하지 않거나 `[-1,1]` 밖 → `INVALID_ARG` |
| `void pixelica_input_manager_tick(m)` | 모든 바인딩으로 액션 값과 엣지를 계산 | `NULL`이면 아무것도 안 함 |
| `PIXELICA_InputActionState pixelica_input_manager_action(m, id)` | 액션 상태를 값으로 반환(엣지를 소비하지 않음) | `m==NULL` 또는 `id>=32` → 모두 0/`false`인 **중립** 값 |
| `uint32_t pixelica_input_manager_buttons(m)` | 액션 0..6의 `held`를 `PIXELICA_BTN_*` 마스크로 | `NULL` → 0 |
| `uint32_t pixelica_input_manager_read_buttons(void *userdata)` | 위와 같음. `PIXELICA_PlatformOps.read_buttons` 모양 | `userdata`는 **매니저 포인터**여야 함. `NULL` → 0 |

### 5.1 트랜잭션

`set_bindings`는 **먼저 전부 검증한 뒤** 쓰기 때문에 실패하면 이전 바인딩이 그대로 남습니다. `submit`도 같아서 실패한 제출은 이전 스냅샷을 건드리지 않습니다.
`set_bindings`는 액션 상태(값, 엣지)를 바꾸지 않습니다. 다음 `tick`에서 새 표가 반영됩니다(8.3절).

### 5.2 앨리어싱

- `set_bindings(m, m->bindings, n)`처럼 매니저 자신의 저장소를 원본으로 줘도 됩니다(검증 후 `memmove`).
- `submit(m, d, &m->devices[k])`처럼 자기 슬롯을 원본으로 줘도 됩니다(먼저 복사한 뒤 검증).
- 그 밖의 겹침은 의미가 없으므로 지원 목록에 없습니다.

### 5.3 오류 처리 습관

```text
조각(fragment): 반환 코드는 무시하지 않는다
PIXELICA_Status st = pixelica_input_manager_submit(&g->input, SOURCE_PAD, &snapshot);
if (st != PIXELICA_OK) {
    /* 어댑터의 버그: 이 소스를 중립으로 만들고 로그를 남긴다. 이전 스냅샷은 그대로 남아 있다. */
    PIXELICA_InputDeviceState gone = {0};          /* connected = false */
    (void)pixelica_input_manager_submit(&g->input, SOURCE_PAD, &gone);
}
```

## 6. 값 계산 규칙

`tick`은 바인딩마다 다음 순서로 **기여값**을 구하고, 액션별로 합산합니다(내부는 `double`).

1. 소스가 `connected`가 아니면 기여 0.
2. **버튼**: 비트가 켜져 있으면 1, 아니면 0. 데드존은 무시합니다.
3. **축**: 값을 `[-1,1]`로 놓고
   - `AXIS_POSITIVE`는 음수 쪽을 0으로 버리고, `AXIS_NEGATIVE`는 양수 쪽을 0으로 버린 뒤 **크기**(양수)를 사용합니다. 방향 게이트가 **먼저**이므로 각 절반이 데드존을 온전히 받습니다. `AXIS_NEGATIVE`의 기여는 양수이므로, 음의 값이 필요한 액션에는 `scale = -1`을 쓰세요.
   - `|x| <= deadzone`이면 0, 아니면 `sign(x) * (|x| - deadzone) / (1 - deadzone)`로 **재조정(rescale)** 합니다.
4. 기여값에 `scale`을 곱합니다.
5. 같은 액션의 기여값을 모두 더하고 `[-1, 1]`로 **clamp**한 것이 `value`입니다.
6. `held = fabs(value) >= 0.5`.

따라서:

- **합산 후 clamp.** 키보드 오른쪽(+1)과 아날로그 스틱(+0.6)이 같은 액션이면 1.6이 아니라 1.0입니다. 반대로 +1과 -0.6이 만나면 0.4가 되어 `held`가 **아닙니다**.
- **반대 방향 상쇄.** 왼쪽(`scale=-1`)과 오른쪽(`scale=+1`)을 한 액션에 걸고 둘 다 누르면 0입니다.
- **축 임계값이 데드존만큼 이동합니다.** `scale=1` 단일 축이면 `held`가 되는 원시 크기는 `0.5 + deadzone/2` 이상입니다(데드존 0.25 → 0.625, 0.2 → 0.6). 임계값은 원시 축이 아니라 **재조정된 값**에 걸립니다.
- **성분별(component-wise) 재조정이며 방사형(radial) 데드존이 아닙니다.** 스틱의 X축과 Y축은 각자의 바인딩으로 따로 처리됩니다. 대각선 입력이 원형으로 잘리지 않으며,
  원형 데드존/정규화가 필요하면 어댑터가 두 축을 합쳐 계산해서 올리세요.
- 같은 액션에 버튼 여러 개가 걸려 있으면 **마지막 버튼을 뗄 때까지** `released`가 나오지 않습니다(기여가 줄지 않기 때문).
- 부동소수점 경계값(예: 정확히 임계값)은 피하고 시험할 때는 경계에서 약간 벗어난 값을 쓰세요.

## 7. 틱과 엣지(pressed / held / released)

`PIXELICA_InputActionState`: `value`, `held`, `pressed`, `released`.

- `pressed` = 이번 틱에 `held`가 되었고 직전 틱에는 아니었음. `released` = 이번 틱에 `held`가 아니고 직전 틱에는 `held`.
- 엣지는 **틱 사이에서만** 바뀝니다. `action()`을 몇 번 불러도 소비되지 않고(**비소비 상태**), 다음 `tick` 전까지 같은 값입니다.
- 매니저는 **스냅샷 API**입니다. 이벤트 큐가 없으므로 두 샘플 사이에 눌렀다 뗀 **아주 짧은 탭은 보관되지 않습니다**(`submit`은 최신 값만 남김). 짧은 입력을 놓치면 안 된다면 어댑터가 틱까지 눌림을 유지하는 정책을 구현해야 합니다(기존
  [`DEVELOPMENT.md` 11.2절](DEVELOPMENT.md#112-기록과-재생)의 같은 한계 참고).

### 7.1 고정 스텝 안에서 틱하기

`tick`은 **시뮬레이션 스텝마다 정확히 한 번**, 입력 샘플링(`submit`) 뒤에 호출합니다. 렌더 프레임 속도와 무관해야 하므로 `pixelica_update`의 고정 스텝 콜백 안에서 부르는 것이 가장 안전합니다.

```text
조각(fragment): 단독 컴파일 불가
/* 프레임당 한 번 (루프 바깥): 소스를 읽어 올린다. 같은 스냅샷이 이 호출의 모든 스텝에서 재사용된다. */
poll_sources_and_submit(&g->input);
pixelica_update(ctx, elapsed_us, on_step, g);

static void on_step(void *ud, float dt, uint32_t legacy_buttons)
{
    Game *g = (Game *)ud;
    (void)legacy_buttons;                       /* PAL 마스크는 쓰지 않는다 (13장) */
    pixelica_input_manager_tick(&g->input);          /* 스텝마다 정확히 한 번 */
    if (pixelica_input_manager_action(&g->input, ACTION_JUMP).pressed) { /* 이 스텝에서만 true */ }
    game_step(g, dt);
}
```

- `pixelica_update`는 한 호출에서 최대 4스텝을 같은 입력으로 실행합니다([`DEVELOPMENT.md` 10장](DEVELOPMENT.md#10-고정-타임스텝)). 콜백마다 `tick`을 부르면 첫 스텝에서 `pressed`가 나오고 나머지 스텝에서는 `false`라 **엣지가 정확히 한 번** 보입니다.
- 렌더 프레임당 한 번만 `tick`을 부르면 한 프레임에 스텝이 0개 또는 2개 이상일 때 엣지가 사라지거나 스텝과 어긋납니다. 엣지를 쓰는 게임은 **반드시 스텝마다** 틱하세요.
- 업데이트가 0스텝인 호출에서는 `tick`을 부르지 않습니다(콜백이 안 불림). 그 호출의 `submit`은 다음 `submit`이 덮어쓸 수 있으므로 거기서 끝난 짧은 탭은 보관되지 않습니다.
- `pixelica_update`가 4스텝을 넘겨 버린 시간에는 틱도 일어나지 않습니다. 시뮬레이션 시간과 같은 방식으로 건너뜁니다.

## 8. 연결 끊김, 포커스 상실, 재바인딩

### 8.1 연결 끊김과 포커스 상실

- `connected=false`로 제출하면 그 소스는 중립이 됩니다. 다음 `tick`에서 그 소스가 누르고 있던 액션은 `released`가 됩니다(다른 소스가 여전히 누르고 있으면 `held`가 유지됩니다).
- 창 포커스를 잃거나 패드가 뽑히면 **어댑터가 중립(또는 `connected=false`)을 제출해야 합니다.** 매니저는 포커스를 알지 못합니다.
- 장치를 교체할 때 이전 사용자의 눌림이 새 장치에 상속되는 것을 피하려면 어댑터가 중립 스냅샷을 먼저 보여 준 뒤 새 장치를 열도록 하세요(12.2절의 SDL3 어댑터는 슬롯을 비운 같은 `poll`에서 새 장치를 열지 않습니다).

### 8.2 제출하지 않은 소스는 마지막 스냅샷을 유지합니다

`submit`이 없는 틱에서도 마지막 스냅샷이 계속 쓰입니다. 즉 어댑터가 어떤 틱에 샘플을 못 올리면 **눌림이 그대로 남습니다.** 어댑터는 오류나 타임아웃일 때 반드시 중립을 올려야 입력이 끈적이지(sticky) 않습니다.

### 8.3 재바인딩과 눌림 유지

`set_bindings`는 액션 상태를 즉시 바꾸지 않습니다. 다음 `tick`이 **새 표를 현재의 실제 장치 상태에 적용**하고 그 결과를 직전 `held`와 비교합니다.

- 눌려 있는 키가 새 표에서도 같은 액션에 매핑되면 엣지가 없습니다(누르고 있는 상태가 이어짐).
- 새 표에서 그 액션을 누르는 컨트롤이 없으면 다음 틱에 `released`가 나옵니다.
- 이미 눌려 있는 컨트롤을 아직 `held`가 아닌 액션에 새로 연결하면 다음 틱에 `pressed`가 나옵니다. 키 재할당 화면에서 확인 키(Enter 등)로 바인딩을 바꾸면 그 키가 즉시 새 액션의
  `pressed`가 될 수 있으므로, 재바인딩 UI는 확인 키를 뗄 때까지 기다리거나 입력 처리를 한 틱 건너뛰세요.
- 실패한 `set_bindings`는 이전 표를 유지합니다(5.1절).

## 9. 레거시 브리지와 P3IN

- `pixelica_input_manager_buttons(m)`은 액션 0..6의 `held`만 `PIXELICA_BTN_*` 비트로 모은 마스크입니다(`PIXELICA_BTN_ALL` 안쪽만). 사용자 액션 7..31과 아날로그 값은 나오지 않습니다.
- `pixelica_input_manager_read_buttons(void *userdata)`는 같은 마스크를 `PIXELICA_PlatformOps.read_buttons` 모양으로 돌려주지만 `userdata`는 **매니저 자신**입니다. 기존 PAL은
  한 개의 `userdata`를 시계, 버튼, 화면 출력이 공유하므로 단순히 바꿔 끼우면 안 됩니다(13장).
- 입력 기록·재생(`P3IN` v1, [`input.h`](../include/pixelica/input.h), [`DEVELOPMENT.md` 11.2·11.3절](DEVELOPMENT.md#112-기록과-재생))은 **7비트 마스크만** 저장합니다. 추가 액션, 축 값, 바인딩 설정은
  기록되지 않습니다. 이 요청에서 파일 형식은 **바뀌지 않았고 업그레이드도 하지 않았습니다.**
- 모든 액션과 축을 재현해야 한다면 현재 형식 밖에서 **직접 만든 기록기**를 쓰세요. 예를 들어 틱마다 각 소스에 제출한 `PIXELICA_InputDeviceState`와 바인딩 표를 자기 형식으로 저장하고, 재생할 때 같은 순서로
  `set_bindings` → `submit` → `tick` 하는 방식입니다. 이 모듈은 그런 기록기를 제공하지 않으며, 다른 컴파일러·CPU 사이의 비트 단위 재현성은 검증하지 않았습니다.

## 10. 하드웨어 어댑터 작성 지침

"하드웨어 어댑터"는 실제 입력 장치의 값을 읽어 **정규화된 스냅샷**으로 바꿔 `submit`하는 코드입니다. 매니저와 이 문서는 어떤 핀, 보드, 프로토콜도 정하지 않습니다.

어댑터가 **코어 밖에서** 책임지는 것:

| 항목 | 어댑터가 할 일 |
|---|---|
| GPIO 활성 레벨 | 풀업/풀다운, active-low 여부를 흡수해 "눌림 = 비트 1"로 만듭니다 |
| 디바운스 | 접점 튕김을 어댑터에서 걸러 낸 뒤 올립니다 (매니저에 디바운스 없음) |
| ADC | 중심값과 범위로 `[-1,1]`(트리거는 `[0,1]`)로 정규화하고 클램프합니다. 센서 보정 데이터도 어댑터 소관 |
| 타임아웃, 오류 | 읽기 실패나 응답 없음이면 중립 또는 `connected=false`를 제출합니다 (8.2절) |
| IRQ·스레드 | ISR/다른 스레드는 자체 버퍼에 쓰고, **게임 스레드가 샘플 시점에 넘겨받아** `submit`합니다. 매니저를 인터럽트에서 직접 호출하면 안 됩니다 |
| 원형 데드존 | 필요하면 두 축을 합쳐 계산한 뒤 올립니다 (6장) |

**현재 상태.** 사용자 정의 소스(예: source 2)로 스냅샷을 올리는 일반 경로는 코어에 준비되어 있습니다. 반면 **검증된 실제 보드 드라이버는 없습니다.** ESP32-P4 포트의 GPIO 버튼 경로와 board 훅은
[`porting.md`](porting.md)에 쓰인 대로 보드 데이터가 비어 있고 실행해 본 적이 없습니다. 아래 11장의 "사용자 정의 하드웨어"는 **합성(synthetic) 입력**이며 실제 핀이나 드라이버를 가정하지 않습니다.

## 11. 완전한 예제

다음 C 코드 블록(문서 전체에서 유일한 `c` 펜스 블록)은 **하나의 완전한 C11 프로그램**입니다. SDL, 렌더러 없이 `Pixelica::input_manager`(와 그것이 쓰는 `Pixelica::common`)만 링크합니다.
검사는 `assert`가 아니라 함수 반환값으로 하므로 Release에서도 그대로 실행됩니다. 실패하면 stderr에 실패한 검사의 설명을 쓰고 종료 코드 1을 돌려줍니다.

다루는 것: source 0(키보드류 스냅샷)과 source 2(합성 사용자 정의 하드웨어)가 같은 액션 `A`에 매핑됨, 부호 있는 축 액션 7(`ACTION_STEER`),
단계별 눌림/다중 장치 유지/떼기, 데드존과 임계값, 합산 clamp·반대 방향 상쇄, 오류와 트랜잭션, 포커스 상실, 짧은 탭, 재바인딩, 레거시 7비트 브리지와 `P3IN` 기록·인코딩·디코딩·재생.

```c
/* input_manager_sample.c - portable C11, no SDL, no renderer.
 * Links only Pixelica::input_manager (and through it Pixelica::common). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pixelica/input.h"
#include "pixelica/input_manager.h"
#include "pixelica/status.h"

/* Source slots: 0 is a keyboard-like snapshot source, 2 is "custom hardware". */
enum { SOURCE_KEYBOARD = 0, SOURCE_CUSTOM = 2 };
/* Button indices inside each source's 64-bit mask. */
enum { KEY_LEFT = 0, KEY_RIGHT = 1, KEY_SPACE = 2, KEY_ALT = 5 };
enum { CUSTOM_FIRE_BUTTON = 3, CUSTOM_DIAL_AXIS = 0, CUSTOM_LEVER_AXIS = 1 };
/* User actions start at 7; 0..6 are the legacy PIXELICA_INPUT_* ids. */
enum { ACTION_STEER = 7, ACTION_THROTTLE = 8, ACTION_BRAKE = 9 };

#define BIT(n) (UINT64_C(1) << (n))
#define ADC_CENTER 2048
#define ADC_HALF_RANGE 2047
#define ADC_MAX 4095

/* The same A action is driven by the keyboard Space bit and the custom fire
 * button; STEER sums two keys and a signed dial; THROTTLE uses the positive
 * half of the lever; BRAKE uses the negative half of the dial. */
static const PIXELICA_InputBinding k_bindings[] = {
    { SOURCE_KEYBOARD, KEY_LEFT, PIXELICA_INPUT_LEFT, PIXELICA_INPUT_BUTTON, 1.0f, 0.0f },
    { SOURCE_KEYBOARD, KEY_RIGHT, PIXELICA_INPUT_RIGHT, PIXELICA_INPUT_BUTTON, 1.0f, 0.0f },
    { SOURCE_KEYBOARD, KEY_SPACE, PIXELICA_INPUT_A, PIXELICA_INPUT_BUTTON, 1.0f, 0.0f },
    { SOURCE_CUSTOM, CUSTOM_FIRE_BUTTON, PIXELICA_INPUT_A, PIXELICA_INPUT_BUTTON, 1.0f, 0.0f },
    { SOURCE_KEYBOARD, KEY_LEFT, ACTION_STEER, PIXELICA_INPUT_BUTTON, -1.0f, 0.0f },
    { SOURCE_KEYBOARD, KEY_RIGHT, ACTION_STEER, PIXELICA_INPUT_BUTTON, 1.0f, 0.0f },
    { SOURCE_CUSTOM, CUSTOM_DIAL_AXIS, ACTION_STEER, PIXELICA_INPUT_AXIS, 1.0f, 0.25f },
    { SOURCE_CUSTOM, CUSTOM_LEVER_AXIS, ACTION_THROTTLE, PIXELICA_INPUT_AXIS_POSITIVE, 1.0f, 0.2f },
    { SOURCE_CUSTOM, CUSTOM_DIAL_AXIS, ACTION_BRAKE, PIXELICA_INPUT_AXIS_NEGATIVE, 1.0f, 0.25f }
};
#define BINDING_COUNT (sizeof k_bindings / sizeof k_bindings[0])

/* Stand-in for what a hardware driver would read: raw numbers, not real pins. */
typedef struct SyntheticBoard {
    bool fire;
    int dial_raw;  /* 0..ADC_MAX, ADC_CENTER at rest */
    int lever_raw; /* 0..ADC_MAX, 0 at rest */
} SyntheticBoard;

static bool expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "check failed: %s\n", what);
    }
    return ok;
}

static float clamp_unit(float v, float lo)
{
    if (v < lo) {
        return lo;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* The adapter's job: raw ADC counts -> normalized floats (the manager never sees raw data). */
static float normalize_centered(int raw)
{
    return clamp_unit((float)(raw - ADC_CENTER) / (float)ADC_HALF_RANGE, -1.0f);
}

static float normalize_unipolar(int raw)
{
    return clamp_unit((float)raw / (float)ADC_MAX, 0.0f);
}

static PIXELICA_InputDeviceState board_snapshot(const SyntheticBoard *board)
{
    PIXELICA_InputDeviceState state;

    memset(&state, 0, sizeof state);
    state.connected = true;
    if (board->fire) {
        state.buttons |= BIT(CUSTOM_FIRE_BUTTON);
    }
    state.axes[CUSTOM_DIAL_AXIS] = normalize_centered(board->dial_raw);
    state.axes[CUSTOM_LEVER_AXIS] = normalize_unipolar(board->lever_raw);
    return state;
}

static bool submit_state(PIXELICA_InputManager *m, uint8_t source,
                         const PIXELICA_InputDeviceState *state, const char *what)
{
    return expect(pixelica_input_manager_submit(m, source, state) == PIXELICA_OK, what);
}

static bool submit_keys(PIXELICA_InputManager *m, uint64_t buttons)
{
    PIXELICA_InputDeviceState state;

    memset(&state, 0, sizeof state);
    state.connected = true;
    state.buttons = buttons;
    return submit_state(m, SOURCE_KEYBOARD, &state, "keyboard snapshot");
}

static bool submit_board(PIXELICA_InputManager *m, bool fire, int dial_raw, int lever_raw)
{
    SyntheticBoard board;
    PIXELICA_InputDeviceState state;

    board.fire = fire;
    board.dial_raw = dial_raw;
    board.lever_raw = lever_raw;
    state = board_snapshot(&board);
    return submit_state(m, SOURCE_CUSTOM, &state, "board snapshot");
}

/* Unplug / focus loss: the payload is ignored when connected == false. */
static bool submit_lost(PIXELICA_InputManager *m, uint8_t source)
{
    PIXELICA_InputDeviceState state;

    memset(&state, 0, sizeof state);
    state.connected = false;
    state.buttons = ~UINT64_C(0);
    state.axes[0] = 0.75f;
    return submit_state(m, source, &state, "disconnect");
}

static bool action_is(const PIXELICA_InputManager *m, uint8_t action, bool held,
                      bool pressed, bool released, const char *what)
{
    PIXELICA_InputActionState a = pixelica_input_manager_action(m, action);

    return expect(a.held == held && a.pressed == pressed && a.released == released, what);
}

static bool value_near(const PIXELICA_InputManager *m, uint8_t action, float expected,
                       const char *what)
{
    float d = pixelica_input_manager_action(m, action).value - expected;

    return expect(d < 0.001f && d > -0.001f, what);
}

static bool phase_neutral(PIXELICA_InputManager *m)
{
    pixelica_input_manager_init(m);
    if (!expect(pixelica_input_manager_set_bindings(m, k_bindings, BINDING_COUNT) == PIXELICA_OK,
                "set_bindings")) {
        return false;
    }
    pixelica_input_manager_tick(m); /* nothing was ever submitted: every source is disconnected */
    return action_is(m, PIXELICA_INPUT_A, false, false, false, "A neutral before any submit")
        && value_near(m, ACTION_STEER, 0.0f, "steer neutral")
        && expect(pixelica_input_manager_buttons(m) == 0u, "no legacy bits");
}

/* Phases: press, multi-device hold, release. */
static bool phase_press_hold_release(PIXELICA_InputManager *m)
{
    if (!submit_keys(m, BIT(KEY_SPACE))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, true, false, "A pressed edge")
        || !action_is(m, PIXELICA_INPUT_A, true, true, false, "queries do not consume the edge")
        || !expect(pixelica_input_manager_buttons(m) == PIXELICA_BTN_A, "legacy mask is A only")) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, false, false, "A held, edge gone on the next tick")) {
        return false;
    }

    /* The custom source presses the same action: still no new edge. */
    if (!submit_board(m, true, ADC_CENTER, 0)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, false, false, "second device joins, no new edge")) {
        return false;
    }

    /* Keyboard releases Space, but the board still holds A: no release yet. */
    if (!submit_keys(m, 0u)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, false, false, "A held by the other device")) {
        return false;
    }

    /* Board unplugged (payload still says "fire"): the last holder lets go. */
    if (!submit_lost(m, SOURCE_CUSTOM)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    return action_is(m, PIXELICA_INPUT_A, false, false, true, "A released after the last holder is gone");
}

/* Signed axis, half axes, deadzone rescale, threshold shift, sum and clamp. */
static bool phase_axes(PIXELICA_InputManager *m)
{
    /* dial 0.20 is inside the 0.25 deadzone; lever 0 */
    if (!submit_board(m, false, ADC_CENTER + 410, 0)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, 0.0f, "inside deadzone")
        || !action_is(m, ACTION_STEER, false, false, false, "steer idle")) {
        return false;
    }

    /* dial +0.70 -> (0.70 - 0.25) / 0.75 = 0.6; lever 0.59 -> (0.59 - 0.2) / 0.8 = 0.4875 (< 0.5) */
    if (!submit_board(m, false, 3481, 2416)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, 0.6f, "steer rescaled")
        || !action_is(m, ACTION_STEER, true, true, false, "steer pressed")
        || !value_near(m, ACTION_THROTTLE, 0.4875f, "throttle just under the threshold")
        || !action_is(m, ACTION_THROTTLE, false, false, false, "throttle not held")) {
        return false;
    }

    /* lever 0.61 -> 0.5125: the threshold moved from 0.5 to 0.6 of the raw axis */
    if (!submit_board(m, false, 3481, 2498)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, ACTION_THROTTLE, true, true, false, "throttle pressed")
        || !action_is(m, ACTION_STEER, true, false, false, "steer stays held")) {
        return false;
    }

    /* dial -0.70: steer flips sign without a new press; the negative half drives BRAKE */
    if (!submit_board(m, false, 615, 0)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, -0.6f, "steer negative")
        || !action_is(m, ACTION_STEER, true, false, false, "negative steer is still held")
        || !action_is(m, ACTION_BRAKE, true, true, false, "negative half pressed")
        || !action_is(m, ACTION_THROTTLE, false, false, true, "throttle released")) {
        return false;
    }

    /* Right key (+1) plus dial (-0.6) sums to 0.4: below the threshold. */
    if (!submit_keys(m, BIT(KEY_RIGHT))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, 0.4f, "contributions are summed")
        || !action_is(m, ACTION_STEER, false, false, true, "sum fell under the threshold")) {
        return false;
    }

    /* Opposing keys cancel (dial centered). LEFT and RIGHT are still separate actions;
     * RIGHT has been held since the previous step, so only LEFT is a new press. */
    if (!submit_board(m, false, ADC_CENTER, 0) || !submit_keys(m, BIT(KEY_LEFT) | BIT(KEY_RIGHT))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, 0.0f, "opposing keys cancel")
        || !action_is(m, PIXELICA_INPUT_LEFT, true, true, false, "left still reads")
        || !action_is(m, PIXELICA_INPUT_RIGHT, true, false, false, "right keeps reading")) {
        return false;
    }

    /* Right key (+1) plus dial (+0.6) = 1.6, clamped to 1. */
    if (!submit_board(m, false, 3481, 2498) || !submit_keys(m, BIT(KEY_RIGHT))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    return value_near(m, ACTION_STEER, 1.0f, "sum is clamped to 1")
        && action_is(m, ACTION_STEER, true, true, false, "steer pressed again");
}

/* Invalid input is rejected without touching the old state. */
static bool phase_errors(PIXELICA_InputManager *m)
{
    PIXELICA_InputDeviceState bad;
    PIXELICA_InputBinding edited[BINDING_COUNT];
    PIXELICA_InputBinding many[PIXELICA_INPUT_MAX_BINDINGS + 1u];
    PIXELICA_InputActionState neutral;
    size_t i;

    memset(&bad, 0, sizeof bad);
    bad.connected = true;
    bad.axes[0] = 2.0f; /* outside [-1, 1] */
    if (!expect(pixelica_input_manager_submit(m, SOURCE_CUSTOM, &bad) == PIXELICA_ERR_INVALID_ARG,
                "out-of-range axis is rejected")
        || !expect(pixelica_input_manager_submit(m, PIXELICA_INPUT_MAX_DEVICES, &bad) == PIXELICA_ERR_INVALID_ARG,
                   "device 4 does not exist")
        || !expect(pixelica_input_manager_submit(m, SOURCE_CUSTOM, NULL) == PIXELICA_ERR_INVALID_ARG,
                   "NULL state")
        || !expect(pixelica_input_manager_submit(NULL, SOURCE_CUSTOM, &bad) == PIXELICA_ERR_INVALID_ARG,
                   "NULL manager")) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, 1.0f, "rejected submit left the old snapshot")) {
        return false;
    }

    memcpy(edited, k_bindings, sizeof edited);
    edited[6].deadzone = 1.0f; /* deadzone must be < 1 */
    if (!expect(pixelica_input_manager_set_bindings(m, edited, BINDING_COUNT) == PIXELICA_ERR_INVALID_ARG,
                "deadzone 1.0 is rejected")) {
        return false;
    }
    edited[6].deadzone = 0.25f;
    edited[8].scale = 2.0f; /* scale must be in [-1, 1] */
    if (!expect(pixelica_input_manager_set_bindings(m, edited, BINDING_COUNT) == PIXELICA_ERR_INVALID_ARG,
                "scale 2.0 is rejected")
        || !expect(pixelica_input_manager_set_bindings(m, NULL, 1u) == PIXELICA_ERR_INVALID_ARG,
                   "NULL bindings with a count")
        || !expect(pixelica_input_manager_set_bindings(NULL, k_bindings, BINDING_COUNT) == PIXELICA_ERR_INVALID_ARG,
                   "NULL manager")) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!value_near(m, ACTION_STEER, 1.0f, "rejected set_bindings left the old table")) {
        return false;
    }

    for (i = 0; i < PIXELICA_INPUT_MAX_BINDINGS + 1u; ++i) {
        many[i] = k_bindings[0];
    }
    if (!expect(pixelica_input_manager_set_bindings(m, many, PIXELICA_INPUT_MAX_BINDINGS + 1u) == PIXELICA_ERR_CAPACITY,
                "65 bindings exceed the capacity")
        || !expect(pixelica_input_manager_set_bindings(m, many, PIXELICA_INPUT_MAX_BINDINGS) == PIXELICA_OK,
                   "64 bindings fit")
        || !expect(pixelica_input_manager_set_bindings(m, k_bindings, BINDING_COUNT) == PIXELICA_OK,
                   "restore the table")) {
        return false;
    }

    neutral = pixelica_input_manager_action(NULL, PIXELICA_INPUT_A);
    if (!expect(!neutral.held && !neutral.pressed && !neutral.released, "NULL manager reads neutral")) {
        return false;
    }
    neutral = pixelica_input_manager_action(m, (uint8_t)PIXELICA_INPUT_MAX_ACTIONS);
    return expect(!neutral.held && !neutral.pressed && !neutral.released, "action 32 reads neutral");
}

/* Focus loss, retained snapshots and taps that fall between two samples. */
static bool phase_focus_and_retention(PIXELICA_InputManager *m)
{
    if (!submit_lost(m, SOURCE_KEYBOARD) || !submit_lost(m, SOURCE_CUSTOM)) {
        return false;
    }
    pixelica_input_manager_tick(m);

    /* Down and up between two ticks: the manager only sees the latest snapshot. */
    if (!submit_keys(m, BIT(KEY_SPACE)) || !submit_keys(m, 0u)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, false, false, false, "a tap between samples is not retained")) {
        return false;
    }

    /* No new submit: the last snapshot stays in force. */
    if (!submit_keys(m, BIT(KEY_SPACE))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, false, false, "unsubmitted source keeps its last snapshot")) {
        return false;
    }

    /* The adapter reports focus loss: neutral snapshot, release edge. */
    if (!submit_lost(m, SOURCE_KEYBOARD)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    return action_is(m, PIXELICA_INPUT_A, false, false, true, "focus loss releases A");
}

/* A rebinding takes effect on the next tick and is compared with the live device state. */
static bool phase_rebinding(PIXELICA_InputManager *m)
{
    static const PIXELICA_InputBinding alt_only[] = {
        { SOURCE_KEYBOARD, KEY_ALT, PIXELICA_INPUT_A, PIXELICA_INPUT_BUTTON, 1.0f, 0.0f }
    };

    if (!submit_keys(m, BIT(KEY_SPACE))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, true, false, "A pressed before rebinding")
        || !expect(pixelica_input_manager_set_bindings(m, alt_only, 1u) == PIXELICA_OK, "rebind A to Alt")
        || !action_is(m, PIXELICA_INPUT_A, true, true, false, "set_bindings does not touch action state")) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, false, false, true, "Space no longer feeds A: released")) {
        return false;
    }
    if (!submit_keys(m, BIT(KEY_SPACE) | BIT(KEY_ALT))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, true, false, "Alt is held: pressed")) {
        return false;
    }
    /* Back to the original table while Space is still down: held continues, no new edge. */
    if (!expect(pixelica_input_manager_set_bindings(m, k_bindings, BINDING_COUNT) == PIXELICA_OK, "restore table")) {
        return false;
    }
    pixelica_input_manager_tick(m);
    return action_is(m, PIXELICA_INPUT_A, true, false, false, "live held state is compared, no spurious edge");
}

/* The seven-bit bridge and the existing P3IN recorder; extra actions are not recorded. */
static bool phase_legacy_bridge(PIXELICA_InputManager *m)
{
    PIXELICA_InputSpan spans[4];
    PIXELICA_InputSpan decoded[4];
    PIXELICA_InputRecorder recorder;
    PIXELICA_InputPlayer player;
    uint8_t bytes[64];
    size_t need = 0;
    size_t written = 0;
    size_t decoded_count = 0;
    uint32_t mask = 0;
    uint32_t expected_mask = PIXELICA_BTN_LEFT | PIXELICA_BTN_A;

    if (!submit_lost(m, SOURCE_KEYBOARD)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    pixelica_input_recorder_init(&recorder, spans, 4u);

    /* Tick 1: Left + Space. STEER (action 7) is also held but has no legacy bit. */
    if (!submit_keys(m, BIT(KEY_LEFT) | BIT(KEY_SPACE))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    mask = pixelica_input_manager_read_buttons(m); /* userdata is the manager itself */
    if (!expect(mask == expected_mask, "legacy mask is LEFT | A")
        || !expect((mask & ~PIXELICA_BTN_ALL) == 0u, "only the seven legacy bits")
        || !action_is(m, ACTION_STEER, true, true, false, "extra action is held but not in the mask")
        || !expect(pixelica_input_record(&recorder, mask) == PIXELICA_OK, "record tick 1")) {
        return false;
    }

    /* Tick 2: unchanged input. */
    pixelica_input_manager_tick(m);
    if (!expect(pixelica_input_manager_buttons(m) == expected_mask, "same mask on tick 2")
        || !expect(pixelica_input_record(&recorder, pixelica_input_manager_buttons(m)) == PIXELICA_OK, "record tick 2")) {
        return false;
    }

    /* Tick 3: everything released. */
    if (!submit_keys(m, 0u)) {
        return false;
    }
    pixelica_input_manager_tick(m);
    mask = pixelica_input_manager_buttons(m);
    if (!expect(mask == 0u, "empty mask on tick 3")
        || !expect(pixelica_input_record(&recorder, mask) == PIXELICA_OK, "record tick 3")
        || !expect(recorder.count == 2u && recorder.total_ticks == 3u, "run-length spans")
        || !expect(pixelica_input_manager_read_buttons(NULL) == 0u, "NULL userdata reads 0")) {
        return false;
    }

    /* Existing P3IN v1 encode / decode / replay round trip. */
    if (!expect(pixelica_input_encoded_size(recorder.count, &need) == PIXELICA_OK, "encoded size")
        || !expect(need == PIXELICA_INPUT_HEADER_BYTES + PIXELICA_INPUT_SPAN_BYTES * recorder.count && need <= sizeof bytes,
                   "16 + 8 * spans")
        || !expect(pixelica_input_encode(recorder.spans, recorder.count, bytes, sizeof bytes, &written) == PIXELICA_OK
                   && written == need, "encode")
        || !expect(pixelica_input_decode(bytes, written, decoded, 4u, &decoded_count) == PIXELICA_OK
                   && decoded_count == recorder.count, "decode")) {
        return false;
    }
    pixelica_input_player_init(&player, decoded, decoded_count);
    return expect(pixelica_input_player_next(&player, &mask) && mask == expected_mask, "replay tick 1")
        && expect(pixelica_input_player_next(&player, &mask) && mask == expected_mask, "replay tick 2")
        && expect(pixelica_input_player_next(&player, &mask) && mask == 0u, "replay tick 3")
        && expect(!pixelica_input_player_next(&player, &mask), "recording is finished");
}

static bool phase_clear_bindings(PIXELICA_InputManager *m)
{
    if (!submit_keys(m, BIT(KEY_SPACE))) {
        return false;
    }
    pixelica_input_manager_tick(m);
    if (!action_is(m, PIXELICA_INPUT_A, true, true, false, "A pressed before clearing")
        || !expect(pixelica_input_manager_set_bindings(m, NULL, 0u) == PIXELICA_OK, "count 0 clears the table")) {
        return false;
    }
    pixelica_input_manager_tick(m);
    return action_is(m, PIXELICA_INPUT_A, false, false, true, "no bindings: A released")
        && expect(pixelica_input_manager_buttons(m) == 0u, "no legacy bits after clearing");
}

int main(void)
{
    PIXELICA_InputManager manager;
    bool ok;

    ok = phase_neutral(&manager)
        && phase_press_hold_release(&manager)
        && phase_axes(&manager)
        && phase_errors(&manager)
        && phase_focus_and_retention(&manager)
        && phase_rebinding(&manager)
        && phase_legacy_bridge(&manager)
        && phase_clear_bindings(&manager);
    if (!ok) {
        fprintf(stderr, "input_manager sample: FAILED\n");
        return 1;
    }
    printf("input_manager sample: all checks passed (sizeof(PIXELICA_InputManager) = %zu bytes on this build)\n",
           sizeof manager);
    return 0;
}
```

예제 CMake는 이렇게 연결합니다(조각).

```text
조각(fragment): 소스 트리에서 한 줄 실행 파일 추가 (렌더러 전부 OFF 가능)
add_executable(input_manager_sample input_manager_sample.c)
target_link_libraries(input_manager_sample PRIVATE Pixelica::input_manager)
```

이 예제는 합성 입력으로 계약을 확인할 뿐입니다. 물리 키보드, 컨트롤러, 실제 보드를 검증한 것이 아니며, 예제 실행 결과 기록은 `INPUT_MANAGER_AUDIT.md` (local-only record)에 있습니다.

## 12. SDL3 어댑터 (소스 트리 전용)

SDL3 키보드와 게임패드를 매니저 소스로 올려 주는 어댑터입니다. 계약은 [`platforms/sdl3/input_sdl3.h`](../platforms/sdl3/input_sdl3.h)의 주석이 기준입니다.

| 항목 | 내용 |
|---|---|
| 타깃 | `pixelica_input_sdl3` / `Pixelica::input_sdl3` (STATIC) |
| 만들어지는 조건 | `PIXELICA_BUILD_INPUT_MANAGER=ON` **그리고** `PIXELICA_PLATFORM=SDL3` (`platforms/sdl3/CMakeLists.txt`) |
| 링크 | `pixelica_input_manager`와 SDL3를 PUBLIC (헤더가 `<SDL3/SDL.h>`를 포함) |
| 설치 | **안 됨.** export도 설치 규칙도 없는 소스 트리 전용입니다. 기존 SDL PAL(`pixelica_platform_sdl3`)이 SDK에 들어가지 않는 것과 같고, SDK 압축 파일은 SDL을 싣지 않습니다([`sdk.md`](sdk.md)) |
| 테스트 | CTest `pixelica_input_sdl3` (매니저 ON, SDL3 플랫폼, `BUILD_TESTING`일 때 등록). 결과는 `INPUT_MANAGER_AUDIT.md` (local-only record) |

```sh
cmake -S . -B build-input-sdl -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON -DPIXELICA_BUILD_INPUT_MANAGER=ON
cmake --build build-input-sdl --config Debug
ctest --test-dir build-input-sdl -C Debug -R "input_manager|input_sdl3" --output-on-failure
```

SDL이 설치되어 있으면 `-DPIXELICA_FETCH_SDL3=ON`은 필요 없습니다(README의 "SDL3 version" 참고). HEADLESS 빌드에는 이 어댑터가 없습니다.

### 12.1 소스 배치

`pixelica_input_sdl3_init(adapter, manager, gamepad_count)`의 `gamepad_count`는 **0..3**입니다.

| 소스 | 주인 |
|---|---|
| 0 | 키보드 (버튼 0..63 = **재매핑 가능한 키 슬롯 64개**, 축 없음) |
| 1 .. `gamepad_count` | 게임패드 슬롯 |
| `gamepad_count`+1 .. 3 | **어댑터가 건드리지 않음.** 사용자 정의 하드웨어용(10장). 어댑터는 핀이나 프로토콜을 정하지 않습니다 |

예: `gamepad_count = 1`이면 소스 0(키보드), 1(패드)을 어댑터가 쓰고 소스 2, 3은 자유입니다. 11장의 예제가 쓰는 사용자 정의 소스 2는 `gamepad_count <= 1`일 때만 충돌하지 않습니다.

- **키보드 슬롯**: 기본 매핑은 버튼 0 `LEFT`, 1 `RIGHT`, 2 `UP`, 3 `DOWN`, 4 `SPACE`, 5 `RETURN`, 6 `ESCAPE`, 7 `A`, 8 `D`, 9 `W`, 10 `S`, 11 `KP_ENTER`이고 12..63은 `SDL_SCANCODE_UNKNOWN`(미매핑)입니다.
  이 번호는 **키보드 장치의 버튼 인덱스**이며 액션 ID가 아닙니다. `pixelica_input_sdl3_set_key(adapter, button, scancode)`로 어떤 슬롯이든 SDL이 보고할 수 있는 스캔코드(또는 `SDL_SCANCODE_UNKNOWN`=해제)로 바꿀 수 있고,
  같은 스캔코드를 여러 슬롯에 걸어도 됩니다. 잘못된 인자는 `PIXELICA_ERR_INVALID_ARG`이며 매핑은 그대로입니다.
- **게임패드**: 버튼 비트 *N* = `SDL_GamepadButton` *N*, `axes[N]` = `SDL_GamepadAxis` *N*(0..5: 왼/오른 스틱 X·Y, 왼/오른 트리거). `axes[6]`, `axes[7]`은 항상 0입니다.
  스틱은 음수를 32768, 양수를 32767로 나눠 `[-1, 1]`, 트리거는 `[0, 1]`입니다. **어댑터는 데드존을 적용하지 않습니다**(바인딩의 `deadzone`이 합니다). 기본 바인딩은 없으므로 `control`에 `SDL_GAMEPAD_BUTTON_SOUTH`, `SDL_GAMEPAD_AXIS_LEFTX` 같은 SDL 열거자를 그대로 씁니다.

옛 키 배치를 새 경로로 재현하려면 바인딩을 이렇게 거세요(키보드 소스 0).

| 액션 | 키보드 버튼 인덱스 (기본 키) |
|---|---|
| `PIXELICA_INPUT_LEFT` | 0 (←), 7 (**A 키**) |
| `PIXELICA_INPUT_RIGHT` | 1 (→), 8 (D) |
| `PIXELICA_INPUT_UP` | 2 (↑), 9 (W) |
| `PIXELICA_INPUT_DOWN` | 3 (↓), 10 (S) |
| `PIXELICA_INPUT_A` | 4 (Space) |
| `PIXELICA_INPUT_START` | 5 (Return), 11 (Keypad Enter) |
| `PIXELICA_INPUT_QUIT` | 6 (Escape) |

```text
조각(fragment): 단독 컴파일 불가. 패드(소스 1)의 바인딩 예. control 은 SDL 열거자 값 그대로
{ 1, (uint8_t)SDL_GAMEPAD_BUTTON_SOUTH,     PIXELICA_INPUT_A,     PIXELICA_INPUT_BUTTON,        1.0f, 0.0f },
{ 1, (uint8_t)SDL_GAMEPAD_BUTTON_DPAD_LEFT, PIXELICA_INPUT_LEFT,  PIXELICA_INPUT_BUTTON,        1.0f, 0.0f },
{ 1, (uint8_t)SDL_GAMEPAD_AXIS_LEFTX,       PIXELICA_INPUT_RIGHT, PIXELICA_INPUT_AXIS_POSITIVE, 1.0f, 0.2f },
{ 1, (uint8_t)SDL_GAMEPAD_AXIS_LEFTX,       PIXELICA_INPUT_LEFT,  PIXELICA_INPUT_AXIS_NEGATIVE, 1.0f, 0.2f },  /* 크기(양수)가 나옴 */
```

### 12.2 사용 순서

```text
조각(fragment): 단독 컴파일 불가. 창과 이벤트 루프는 응용이 소유한다
PIXELICA_InputManager manager;
PIXELICA_InputSdl3 sdl_input = {0};                       /* 처음 init 전에 0 초기화 */

pixelica_input_manager_init(&manager);
/* set_bindings 의 반환값을 확인한다 */
if (pixelica_input_sdl3_init(&sdl_input, &manager, 1u) != PIXELICA_OK) { /* 실패: SDL_GetError() */ }

while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { /* 응용이 이벤트를 소비한다: 종료, SDL_EVENT_WINDOW_FOCUS_LOST/GAINED 로 has_focus 갱신 */ }
    PIXELICA_Status st = pixelica_input_sdl3_poll(&sdl_input, has_focus);   /* SDL 이벤트 스레드에서, 스텝 전에 */
    if (st != PIXELICA_OK) { /* 로그. 어댑터가 소유 소스를 중립으로 만들었다 */ }
    pixelica_update(ctx, elapsed_us, on_step, &game);     /* on_step 안에서 pixelica_input_manager_tick (7.1절) */
}
pixelica_input_sdl3_shutdown(&sdl_input);                 /* 매니저가 살아 있는 동안, SDL_Quit 보다 먼저 */
```

- **`poll`은 `SDL_PollEvent`를 부르지 않습니다.** `SDL_PumpEvents()`와 `SDL_UpdateGamepads()`만 호출하고 응용의 이벤트를 소비하지 않습니다. 창, 종료, 게임패드 이벤트는 큐에 남으므로 응용이 계속 비워야 합니다(큐가 차면 SDL이 이벤트를 버립니다).
  창과 이벤트 루프의 주인은 **응용**입니다. `poll`은 SDL 이벤트 루프를 도는(창을 만든) 스레드에서 호출합니다.
- **`poll`은 `tick`을 부르지 않습니다.** 스텝마다 한 번 직접 부르세요. 샘플은 프레임당 한 번(스텝 바깥)이어도 되고, 그러면 그 호출의 모든 캐치업 스텝이 같은 스냅샷을 씁니다. **엣지가 맞으려면 틱은 스텝마다**입니다(7.1절).
- `accept_input=false`(포커스 없음, 일시정지 화면 등)이면 핫플러그 처리는 계속하되 소유한 모든 소스에 중립(`connected=false`)을 올립니다. 다음 틱에 눌려 있던 것들이 `released`가 되고, `true`로 다시 부르면 정상 샘플링이 재개됩니다.
- **게임패드 스캔과 핫플러그.** 슬롯은 안정적입니다. 연 `SDL_Gamepad`와 인스턴스 ID를 장치가 사라질 때까지 유지하고, `SDL_GetGamepads()`로 확인해 사라진 슬롯은 닫고 중립을 올립니다. 새 장치는 비어 있는 가장 낮은 슬롯에 열립니다.
  **슬롯이 비워진 같은 `poll`에서는 새 장치를 열지 않으므로** 이전/새 장치 사이에 중립 스냅샷을 한 번 제출합니다. 그 사이에 `tick`이 실행되어야 해제 엣지로 관측됩니다. 열 수 없는 장치는 건너뛰며 `poll`을 실패시키지 않습니다.

### 12.3 초기화·종료와 메모리 수명

- `PIXELICA_InputSdl3`는 호출자 소유 구조체입니다. `manager` 포인터는 소유하지 않으며 어댑터보다 오래 살아야 합니다. 한 매니저에 어댑터는 하나만 붙이세요(소스를 서로 덮어씁니다). 서로 다른 매니저에 여러 어댑터를 쓰는 것은 괜찮습니다.
- `init`은 SDL 게임패드 하위 시스템(`SDL_INIT_GAMEPAD`, 조이스틱·이벤트를 함께 올림) 참조를 하나 얻고, `shutdown`이 정확히 하나 돌려놓습니다(균형). `SDL_Quit`은 부르지 않고 힌트·이벤트·다른 하위 시스템은 그대로 둡니다.
  `init`은 비디오를 초기화하지 않으므로 키보드 샘플은 응용이 SDL 비디오와 창을 이미 만들었다고 가정합니다.
- `shutdown`은 소유한 소스에 중립을 올리고(그래서 **매니저가 아직 유효해야** 함), 열어 둔 게임패드를 닫고, 구조체를 0 상태로 되돌립니다. `NULL`이나 초기화되지 않은 구조체에도 안전합니다. 이미 초기화된 어댑터에 `init`을 다시 부르면 `PIXELICA_ERR_INVALID_ARG`입니다.
- **할당.** 매니저는 힙을 쓰지 않지만 SDL 쪽은 다릅니다. `SDL_GetGamepads()`가 ID 목록을 할당하고(어댑터가 `poll`이 끝나기 전에 해제), `SDL_OpenGamepad`도 SDL 내부에서 할당할 수 있습니다. 목록 할당이 실패하면 어댑터가 소유한 소스를 **먼저 중립으로 만든 뒤**
  `PIXELICA_ERR_NO_MEMORY`를 돌려줍니다(입력이 끈적이지 않음). 열려 있던 패드는 유지되고 다음 `poll`이 성공하면 재개됩니다.
- 오류 코드: `init`은 인자 오류(NULL, `gamepad_count > 3`, 이미 초기화됨) `INVALID_ARG`, SDL 초기화 실패 `PIXELICA_ERR_PLATFORM`(어댑터는 초기화되지 않은 채로 남음). `poll`은 NULL/미초기화 `INVALID_ARG`, 매니저의 `submit` 오류는 모든 소스를 시도한 뒤 반환합니다.

### 12.4 한계

- SDL이 게임패드로 인식하는 장치를 다룹니다. 일반 조이스틱이나 자체 USB 장치는 SDL 게임패드 매핑을 제공하거나 별도 스냅샷 어댑터로 연결해야 합니다.
- SDL 3.4.18은 게임패드를 열 때 트리거의 영점을 초기화합니다. 연결 순간 이미 누른 트리거는 장치가 다음 상태를 보고할 때까지 중립으로 보일 수 있습니다. 어댑터는 SDL이 제공한 상태를 사용합니다.
- 키보드와 게임패드만 다룹니다. 마우스, 터치, IME/텍스트 입력, 진동, 장치 선택/플레이어 할당 UI는 없습니다.
- 물리 키보드나 실제 컨트롤러를 사람이 써 본 검증이 아닙니다. 어댑터의 자동 테스트([`platforms/sdl3/input_sdl3_test.c`](../platforms/sdl3/input_sdl3_test.c))는 SDL 가상 게임패드를 쓰며 물리 장치를 요구하지 않습니다. 실행 결과와 범위는 감사 기록에 있습니다.
  키보드는 OS 입력을 주입해 확인하지 않고 키 매핑 검증 위주입니다.
- 한 번에 게임패드 3대까지입니다(소스 슬롯이 4개이고 키보드가 0번이기 때문).

## 13. 기존 PAL·예제와 함께 쓸 때의 주의

새 입력 경로는 **명시적 옵트인**입니다. 기존 기본 PAL(`platforms/sdl3/platform_sdl3.c`)과 예제의 키 처리를 자동으로 바꾸지 않았습니다.

- **Escape 종료는 되돌릴 수 없습니다.** 기존 `pixelica_sdl3_pump_events`는 Escape `KEY_DOWN`이나 창 닫기를 보면 `quit_requested`를 `true`로 **고정**하고, `sdl3_read_buttons`가 그 뒤로 계속 `PIXELICA_BTN_QUIT`를 돌려줍니다.
  따라서 키 재할당 어댑터에서 Escape를 다른 액션에 매핑해도 기존 pump를 쓰는 한 Escape는 여전히 종료입니다. Escape를 재할당하려면 `pixelica_sdl3_pump_events`를 쓰지 않는 **직접 SDL 루프**가 필요합니다.
- **PAL의 `userdata`는 하나입니다.** `pixelica_sdl3_ops`는 시계, 버튼 읽기, 화면 출력 콜백이 모두 같은 `PIXELICA_Sdl3Platform *` `userdata`를 쓰도록 만듭니다.
  `pixelica_input_manager_read_buttons`의 `userdata`는 **매니저**여야 하므로, `ops.read_buttons`만 그것으로 바꾸고 `userdata`를 바꾸면 시계·화면 출력이 깨지고, `userdata`를 그대로 두면 잘못된 포인터를 읽습니다.
  단순 교체를 하지 마세요.
- **권장 1 (가장 단순).** PAL의 `buttons` 인자를 쓰지 않고, 고정 스텝 콜백 안에서 `tick` 한 뒤 매니저를 직접 읽습니다(7.1절).
- **권장 2.** 응용 컨텍스트를 `userdata`로 갖는 자체 `PIXELICA_PlatformOps`를 만들어 `time_us`, `present_rgb565`는 기존 ops로 전달하고 `read_buttons`만 매니저로 연결합니다.

```text
조각(fragment): 단독 컴파일 불가
typedef struct App {
    PIXELICA_PlatformOps inner;       /* pixelica_sdl3_ops(platform) 의 복사본 */
    PIXELICA_InputManager input;
} App;

static uint64_t app_time_us(void *ud) { App *a = ud; return a->inner.time_us(a->inner.userdata); }
static uint32_t app_read_buttons(void *ud) { App *a = ud; return pixelica_input_manager_read_buttons(&a->input); }
static bool app_present(void *ud, const uint16_t *px, uint16_t w, uint16_t h, uint32_t stride)
{ App *a = ud; return a->inner.present_rgb565(a->inner.userdata, px, w, h, stride); }
/* log 도 같은 방식으로 전달하거나 NULL. ops.userdata = &app; 그 App 은 pixelica_destroy 까지 살아 있어야 한다 */
```

- 주의: `pixelica_update`는 호출당 한 번, **스텝 전에** `read_buttons`를 부릅니다. 이때 매니저가 돌려주는 마스크는 직전 틱의 상태입니다. 스텝 콜백 안에서 `tick` 한 직후 `pixelica_input_manager_buttons`나 `action`을 읽는 쪽이 최신입니다.
- 이 요청은 기존 `platform_sdl3.c/.h`, `pixelica_sdl3_ops`, 예제의 기본 키 경로를 바꾸지 않았습니다.

## 14. 범위, 확장 경계, 단계 계획

### 14.1 만들지 않은 것

IME·텍스트 입력, 마우스, 터치, 진동(rumble), 바인딩 편집기·키 재할당 화면, 입력 전송(네트워크) 드라이버, 플레이어 할당 UI, 특정 보드용 GPIO/ADC 드라이버는 **만들지 않았습니다.**
다중 플레이어는 "소스 슬롯 4개 + 액션 ID 32개"로 구성할 수 있다는 뜻일 뿐입니다. 소스는 플레이어가 아니고, 소스를 플레이어에 배정하는 일은 게임의 몫입니다(예: 플레이어별 액션 ID 범위).

### 14.2 준비됨 / 검증됨의 구분

| 항목 | 상태 |
|---|---|
| 일반 사용자 정의 소스 경로 (`submit` + 바인딩) | 코어에 준비됨. 합성 입력으로 계약 확인 |
| 검증된 실제 보드 드라이버 | **없음** |
| 키보드, 컨트롤러 실물 검증 | 이 문서는 주장하지 않음 (감사 기록 참고) |
| 기존 SDK 압축 파일 | 입력 매니저 **미포함** (재빌드하지 않음) |
| `P3IN` 형식 | 7비트 그대로, 변경 없음 |

### 14.3 단계 계획과 확장 경계

1. **소스 트리 사용**: 옵션을 켜고 2장대로 빌드. 게임은 `Pixelica::input_manager`를 링크하고 액션 ID를 열거형으로 정의합니다.
2. **어댑터 연결**: 소스마다 스냅샷을 올립니다(키보드 슬롯 0 등). 자체 하드웨어는 10장 지침으로 비어 있는 슬롯에.
3. **고정 스텝 통합**: 7.1절 형태로 `tick`을 스텝 콜백에 넣고, 게임 로직은 `action()`을 읽습니다. 기존 `buttons` 인자를 쓰는 코드는 레거시 브리지(9장)로 단계적으로 옮깁니다.
4. **필요할 때 확장**: 아래 경계를 지켜 새 코드를 어댑터나 게임 쪽에 둡니다.

확장 경계:

- 장치별 기능은 어댑터(스냅샷 생성)나 게임(바인딩 표, 재할당 UI, 기록기)에서 확장합니다. 한계 숫자를 늘리면 공개 구조체 배치와 ABI가 바뀌므로 라이브러리와 소비자를 함께 다시 빌드해야 합니다.
- 코어는 SDL, ESP-IDF, 시계, 힙, 스레드를 모릅니다. 플랫폼 의존 코드는 `platforms/` 또는 게임에 둡니다([`architecture.md`](architecture.md)의 경계 규칙).
- 새 어댑터를 저장소에 넣는다면 설치 대상이 아닌 빌드 트리 라이브러리로 시작하고, 설치할 때는 별도 컴포넌트와 export 세트를 둡니다(선택 모듈 규칙, [`rigging.md`](rigging.md)).

## 관련 문서

- 검증 기록: `INPUT_MANAGER_AUDIT.md` (local-only record)
- SDK 설치·컴포넌트: [`sdk.md`](sdk.md), 구조: [`architecture.md`](architecture.md), 개발자 핸드북: [`DEVELOPMENT.md`](DEVELOPMENT.md) (28장)
- 입력 기록·재생과 `P3IN`: [`DEVELOPMENT.md` 11장](DEVELOPMENT.md#11-입력과-기록-재생), 고정 스텝: [`DEVELOPMENT.md` 10장](DEVELOPMENT.md#10-고정-타임스텝)
- ESP32-P4 현재 상태(GPIO·보드 훅): [`porting.md`](porting.md)
