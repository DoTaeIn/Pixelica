# Pixelica

Pixelica는 GPU 없이 CPU만으로 RGB565 프레임버퍼에 그리는 2D / 2.5D / 3D 픽셀 아트 엔진입니다. 이식 가능한 C11로 작성했고,
렌더러는 SDL이나 ESP-IDF에 의존하지 않습니다. 플랫폼 계층은 완성된 픽셀을 화면에 보여 주고 시간과 입력을 제공하는 일만 합니다.

> **이름 변경 안내:** 공개 식별자(C 심볼, 헤더 경로, CMake 패키지·타깃·옵션)는 모두 Pixelica 기준으로 바뀌었으므로 이전 빌드 디렉터리를 쓰지 말고 새로 빌드하세요.

## 지원 범위

- 개발·실행 환경: Windows, Linux (MSVC / GCC / Clang)
- ESP32-P4: 컴파일 준비 단계입니다. 실제 보드가 없어 보드에서 실행해 본 적이 없으며 디스플레이·핀 설정은 보드 선택 후 채워야 합니다 ([`docs/porting.md`](docs/porting.md)).
- 범위 밖: 범용 물리 엔진, 오디오, 에디터, GPU 렌더러 (간단한 AABB 충돌은 제공)

## 모듈

| CMake 타깃 | 설명 | 켜는 옵션 | 기본값 |
|---|---|---|---|
| `Pixelica::common` | 수학, 입력, 애니메이션, 텍스처 에셋 공용 코드 | 항상 | - |
| `Pixelica::core` | 3D 소프트웨어 래스터라이저 | `PIXELICA_BUILD_3D` | ON |
| `Pixelica::renderer2d` | 스프라이트, 타일맵, 비트맵 텍스트, 아이소메트릭 | `PIXELICA_BUILD_2D` | ON (최상위) |
| `Pixelica::hybrid` | 2.5D: 3D 깊이 버퍼에 스프라이트 배치 (2D+3D 필요) | 2D와 3D 모두 ON | ON |
| `Pixelica::skeleton` | 본 계층, 스키닝 | `PIXELICA_BUILD_SKELETON` | OFF |
| `Pixelica::ik` | 2본 IK | `PIXELICA_BUILD_IK` | OFF |
| `Pixelica::input_manager` | 장치 스냅샷 → 바인딩 → 게임 액션 | `PIXELICA_BUILD_INPUT_MANAGER` | OFF |
| `Pixelica::online` | 방 코드 협동·대전, 기록 보드 | `PIXELICA_BUILD_ONLINE` | OFF |

## 요구 사항

- CMake 3.21 이상, C11 컴파일러 (Windows는 Visual Studio "C++를 사용한 데스크톱 개발")
- Python 3.8+ (표준 라이브러리만 사용, 에셋 도구와 Python 테스트용), 온라인 서버·런처는 3.10+
- SDL3 플랫폼: SDL **3.4.18** (설치되어 있거나 `-DPIXELICA_FETCH_SDL3=ON`으로 공식 릴리스 아카이브를 받아 빌드). Linux는 SDL 빌드용 X11/Wayland 개발 패키지가 필요합니다.

## 빌드, 실행, 테스트

헤드리스(SDL·창 없음) 빌드는 어느 환경에서나 동작합니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPIXELICA_PLATFORM=HEADLESS
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
./build/hello_cube --frames 10
```

Visual Studio 같은 멀티 설정 생성기에서는 `-DCMAKE_BUILD_TYPE` 대신 `--config Debug`를 쓰고 실행 파일은 `build\Debug\` 아래에 생깁니다.
`--frames N`은 고정 시간 간격으로 N프레임만 실행하고, 헤드리스에서는 마지막 프레임의 해시를 출력합니다.

### SDL3 창 실행

```sh
cmake -S . -B build-sdl -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON
cmake --build build-sdl --config Debug
```

SDL 3.4.18이 이미 설치되어 있으면 `-DPIXELICA_FETCH_SDL3=ON`을 빼세요(`-DCMAKE_PREFIX_PATH`로 위치 지정 가능). 예제는 `pixel_room`, `hello_2d`, `hello_25d` 등이며
조작은 방향키/WASD, Space(A), Enter(START), Esc(종료)입니다.

### 주요 CMake 옵션

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `PIXELICA_PLATFORM` | `SDL3` (최상위) | `SDL3` 또는 `HEADLESS` |
| `PIXELICA_FETCH_SDL3` | `OFF` | 설치된 SDL3가 없을 때 소스에서 빌드 |
| `PIXELICA_BUILD_2D` / `PIXELICA_BUILD_3D` | `ON` | 렌더러 선택 |
| `PIXELICA_BUILD_SKELETON` / `_IK` / `_INPUT_MANAGER` / `_ONLINE` | `OFF` | 선택 모듈 |
| `PIXELICA_BUILD_EXAMPLES` / `PIXELICA_BUILD_TESTS` | `ON` (최상위) | 예제, 테스트 |
| `PIXELICA_WARNINGS_AS_ERRORS` | `OFF` | 경고를 오류로 처리 |
| `PIXELICA_SANITIZE` | `OFF` | ASan(+UBSan) |

## 프로젝트에서 사용하기

**소스로 포함** — 저장소를 하위 디렉터리로 두고 타깃에 링크합니다.

```cmake
set(PIXELICA_BUILD_2D ON CACHE BOOL "Build the 2D renderer")
add_subdirectory(external/Pixelica)
target_link_libraries(my_game PRIVATE Pixelica::renderer2d)
```

하위 프로젝트로 포함하면 기본 플랫폼은 `HEADLESS`이고 예제·테스트는 꺼집니다.

**설치 후 사용** — 설치된 패키지를 `find_package`로 찾습니다.

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DPIXELICA_PLATFORM=HEADLESS \
  -DPIXELICA_BUILD_EXAMPLES=OFF -DPIXELICA_BUILD_TESTS=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix sdk-prefix
```

```cmake
find_package(Pixelica CONFIG REQUIRED COMPONENTS renderer2d)
target_link_libraries(my_game PRIVATE Pixelica::renderer2d)
```

사용할 때는 `-DCMAKE_PREFIX_PATH=<sdk-prefix>`를 지정합니다. 컴포넌트는 `common`, `core`, `renderer2d`, `hybrid`, `skeleton`, `ik`, `input_manager`, `online`입니다.
2D 전용 사용자를 위한 `Pixelica2D` 패키지는 `find_package(Pixelica2D CONFIG REQUIRED)`로 찾으며, 시작용 프로젝트는 `sdk/template`, `sdk/template2d`에 있습니다
([`docs/sdk.md`](docs/sdk.md)).

## 온라인 예제 (선택)

`PIXELICA_BUILD_ONLINE=ON`으로 빌드하면 로컬 릴레이 서버(`tools/online_server/server.py`, Python 표준 라이브러리 + SQLite)와 `hello_online`을 명령 한 줄로 함께 띄울 수 있습니다.

```sh
cmake -S . -B build-online -DCMAKE_BUILD_TYPE=Release -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_ONLINE=ON
cmake --build build-online --config Release
python examples/hello_online/launch_local.py --exe build-online/hello_online_headless --smoke --mode versus
```

Visual Studio에서는 `--exe build-online/Release/hello_online_headless.exe`를 지정합니다.
서버는 `127.0.0.1`의 빈 포트에서만 열립니다. SDL 창으로 직접 플레이하는 방법과 공개 서버 배포는 [`docs/online.md`](docs/online.md)를 참고하세요.

## 에셋 도구

`tools/pixelica_pack/pixelica_pack.py` (Python 표준 라이브러리만 사용): MagicaVoxel `.vox` → `.p3mesh`, PPM → `.p3tex`, `.p3cart` 묶음, 검사.
파일 확장자와 v1 바이너리 형식은 그대로입니다 ([`docs/asset_format.md`](docs/asset_format.md)).

```sh
python tools/pixelica_pack/pixelica_pack.py import vox model.vox -o model.p3mesh
python tools/pixelica_pack/pixelica_pack.py texture floor.ppm -o floor.p3tex
python tools/pixelica_pack/pixelica_pack.py bundle -o game.p3cart name=model.p3mesh floor=floor.p3tex
python tools/pixelica_pack/pixelica_pack.py inspect game.p3cart
```

## SDK 패키징

```sh
python tools/package_sdk.py --build-dir build-package --output-dir dist
```

`Pixelica2D`와 `Pixelica3D` ZIP 두 개를 만들며, 압축 파일의 최상위 디렉터리는 `Pixelica-0.1.0/`입니다. `--dry-run`으로 계획만 출력할 수 있습니다.

## 디렉터리 구성

```text
include/pixelica/   공개 헤더 (pixelica.h, renderer2d.h, ...)
src/                common + core
modules/            renderer2d, hybrid, skeleton, ik, input_manager, online
platforms/          sdl3, esp32p4
firmware/esp32p4/   ESP-IDF 프로젝트
examples/           예제 게임
sdk/                설치 패키지용 시작 템플릿
tools/              에셋 도구, SDK 패키저, 온라인 릴레이 서버
assets/             VOX 원본과 컴파일된 에셋
tests/              C 테스트
```

## 문서

- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md): 개발 가이드 (빌드, 구조, 규칙)
- [`docs/api.md`](docs/api.md), [`docs/architecture.md`](docs/architecture.md)
- [`docs/sdk.md`](docs/sdk.md), [`docs/porting.md`](docs/porting.md)
- [`docs/2d.md`](docs/2d.md), [`docs/animation.md`](docs/animation.md), [`docs/rigging.md`](docs/rigging.md)
- [`docs/input_manager.md`](docs/input_manager.md), [`docs/online.md`](docs/online.md)
- [`docs/asset_format.md`](docs/asset_format.md), [`docs/art_style.md`](docs/art_style.md)

