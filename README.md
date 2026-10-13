# Pixelica

Pixelica는 GPU 없이 CPU만으로 RGB565 프레임버퍼에 그리는 2D / 2.5D / 3D 픽셀 아트 엔진입니다. 이식 가능한 C11로 작성했고,
렌더러는 SDL이나 ESP-IDF에 의존하지 않습니다. 플랫폼 계층은 완성된 픽셀을 화면에 보여 주고 시간과 입력을 제공하는 일만 합니다.

> **이름 변경 안내:** 공개 식별자(C 심볼, 헤더 경로, CMake 패키지·타깃·옵션)는 모두 Pixelica 기준으로 바뀌었으므로 이전 빌드 디렉터리를 쓰지 말고 새로 빌드하세요.

## 지원 범위

- 개발·실행 환경: Windows, Linux (MSVC / GCC / Clang)
- ESP32-P4: 컴파일 준비 단계입니다. 실제 보드가 없어 보드에서 실행해 본 적이 없으며 디스플레이·핀 설정은 보드 선택 후 채워야 합니다 ([`docs/porting.md`](docs/porting.md)).
- [2D 에디터](tools/editor/README.md): 배치·속성 편집·저장·실행·충돌 영역·중력·점프를 지원하는 별도 데스크톱 도구.
- 범위 밖: 범용 물리 엔진, GPU 렌더러 (간단한 AABB 충돌은 제공)

## 모듈

| CMake 타깃 | 설명 | 켜는 옵션 | 기본값 |
|---|---|---|---|
| `Pixelica::common` | 수학, 입력, 애니메이션, 텍스처 에셋, 저장, 화면 전환, 다국어, 카메라 보조, 디버그, 청크 관리 | 항상 | - |
| `Pixelica::ui` | 메뉴·HUD: 버튼, 체크박스, 슬라이더, 진행 막대, 텍스트·패널 | 항상, 두 SDK에 기본 포함 | ON |
| `Pixelica::core` | 3D 소프트웨어 래스터라이저 | `PIXELICA_BUILD_3D` | ON |
| `Pixelica::renderer2d` | 스프라이트, 타일맵, 비트맵 텍스트, 아이소메트릭 | `PIXELICA_BUILD_2D` | ON (최상위) |
| `Pixelica::hybrid` | 2.5D: 3D 깊이 버퍼에 스프라이트 배치 (2D+3D 필요) | 2D와 3D 모두 ON | ON |
| `Pixelica::skeleton` | 본 계층, 스키닝 | `PIXELICA_BUILD_SKELETON` | OFF |
| `Pixelica::ik` | 2본 IK | `PIXELICA_BUILD_IK` | OFF |
| `Pixelica::input_manager` | 장치 스냅샷 → 바인딩 → 게임 액션 | `PIXELICA_BUILD_INPUT_MANAGER` | OFF |
| `Pixelica::online` | 방 코드 협동·대전, 기록 보드 | `PIXELICA_BUILD_ONLINE` | OFF |
| `Pixelica::audio` | WAV/PCM 효과음·배경음, 믹싱·볼륨·팬·페이드 (렌더러 독립) | `PIXELICA_BUILD_AUDIO` | OFF |
| `Pixelica::audio_sdl3` | PC 오디오 출력·WAV 파일 로딩 | 오디오 ON + 플랫폼 SDL3 | OFF |
| `Pixelica::particles` | 고정 풀 파티클·불꽃·연기·비 (렌더러 독립) | `PIXELICA_BUILD_PARTICLES` | OFF |
| `Pixelica::particles2d` / `::particles25d` | 2D 스프라이트·3D 빌보드 출력 | 파티클 ON + 해당 렌더러 | OFF |
| `Pixelica::shaders` | CPU 픽셀 효과·색 보정·팔레트·외곽선·블러 | `PIXELICA_BUILD_SHADERS` | OFF |

## 요구 사항

- CMake 3.21 이상, C11 컴파일러 (Windows는 Visual Studio "C++를 사용한 데스크톱 개발")
- Python 3.8+ (표준 라이브러리만 사용, 에셋 도구와 Python 테스트용), 온라인 서버·런처는 3.10+
- 에디터: Python 3.10+ / Tk / Pillow. 게임 SDK에는 이 의존성이 추가되지 않습니다.
- SDL3 플랫폼: SDL **3.4.18** (설치되어 있거나 `-DPIXELICA_FETCH_SDL3=ON`으로 공식 릴리스 아카이브를 받아 빌드). Linux는 SDL 빌드용 X11/Wayland 개발 패키지가 필요합니다.

## 빌드, 실행, 테스트

### 2D 에디터

```sh
python -m pip install -r tools/editor/requirements.txt
python tools/editor/launch_editor.py
```

Windows는 `tools/editor/Pixelica Editor.cmd`를 더블 클릭해도 됩니다.
첫 실행은 CMake와 C 컴파일러로 네이티브 렌더러 브리지를 빌드합니다.
[조작·실행 안내](tools/editor/README.md) · [구조·저장 형식](docs/editor.md)

### C 라이브러리

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
| `PIXELICA_BUILD_SKELETON` / `_IK` / `_INPUT_MANAGER` / `_ONLINE` / `_AUDIO` / `_PARTICLES` / `_SHADERS` | `OFF` | 선택 모듈 |
| `PIXELICA_BUILD_EXAMPLES` / `PIXELICA_BUILD_TESTS` | `ON` (최상위) | 예제, 테스트 |
| `PIXELICA_WARNINGS_AS_ERRORS` | `OFF` | 경고를 오류로 처리 |
| `PIXELICA_SANITIZE` | `OFF` | ASan(+UBSan) |

### 텍스처 반복·투명도

`pixelica/draw_options.h`의 공통 옵션으로 U/V별 `CLAMP`·`REPEAT`, 알파 테스트·RGB565 블렌딩을 설정합니다.
3D는 `pixelica_draw_mesh_options()`, 2D 스프라이트는 `pixelica2d_draw_sprite_ex()`를 사용합니다.
기존 텍스처 에셋과 그리기 API는 그대로 사용할 수 있습니다. [사용법·깊이·그리기 순서](docs/texture_options.md)를 참고하세요.

### 카메라·디버그 (기본 포함)

- [렌더 타깃·색/깊이 스냅샷](docs/framebuffer.md): 별도 해상도 렌더링, 독립 복사, 색·깊이 샘플링.
- [재질 픽셀 처리·Bilinear·월드 클립 평면](docs/render_effects.md): 기존 재질 결과에 커스텀 처리를 적용하고 반사·단면·포털에 필요한 영역을 잘라냅니다.

3D 메시·2.5D의 3D 재질은 [텍스처×정점 RGBA·다중 UV 합성](docs/material_layers.md)을 지원합니다.
기본 텍스처와 최대 3개 보조 텍스처를 독립 UV로 합성하며, 기존 재질 API는 그대로 사용할 수 있습니다.

- [카메라 보조](docs/camera.md): 2D 추적·좌표 변환, 3D 시점 행렬, 화면 흔들림.
- [디버그](docs/debug.md): FPS·프레임 시간·사용자 수치를 UI로 표시. 호출할 때만 동작합니다.
- [청크 관리](docs/chunks.md): 주변 청크 선택, 중복 로딩 방지, 로딩·해제 콜백과 프레임당 처리량 제한. 청크 크기·입출력·범위는 게임에서 정합니다.
- 타이틀·플레이 화면 전환은 기존 ScreenManager를 사용합니다. 비동기 스트리밍과 렌더링 가시성 선별은 게임에서 구현합니다.

### 저장·화면 전환·다국어 (기본 포함)

두 SDK의 `Pixelica::common`에 포함되며 별도 다운로드나 활성화 옵션이 없습니다.

- [저장·설정](docs/save.md): 버전·CRC 검사와 PC 파일 저장. 임베디드는 같은 데이터를 NVS·SD 등에 저장할 수 있습니다.
- [게임 화면](docs/screens.md): 타이틀·플레이·일시정지·결과 화면, 콜백 중 안전한 전환 요청.
- [다국어](docs/i18n.md): UTF-8 번역표, 언어 전환·대체 언어, 게임에서 쓰는 글자만 추출하는 BDF 폰트 도구.

### UI 시작하기 (기본 포함)

메뉴와 설정 화면에 필요한 UI는 **2D·3D SDK에 항상 함께 들어 있습니다**. 별도 다운로드나 활성화 옵션은 없습니다.
독립된 정적 라이브러리여서 게임에서 사용하지 않으면 실행 파일에 UI 코드가 연결되지 않습니다.

```cmake
find_package(PixelicaUI CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE Pixelica::ui)
```

`hello_ui`는 키보드와 마우스로 조작하는 메뉴 예제입니다. SDL3 빌드에서는 창을 열고,
HEADLESS 또는 `--smoke`에서는 입력 동작을 자동 검사합니다. `--output menu.ppm`으로 화면도 저장합니다.
컨트롤러·자작 장치의 버튼, 터치 좌표도 같은 입력 구조체로 연결합니다.
[설치·사용법·2D/3D 연결·한글 글꼴 안내](docs/ui.md)를 참고하세요.

### 오디오 시작하기

```sh
cmake -S . -B build-audio -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON -DPIXELICA_BUILD_AUDIO=ON
cmake --build build-audio --config Release
./build-audio/hello_audio
```

Visual Studio에서는 `build-audio/Release/hello_audio.exe`를 실행하세요. WAV를 로딩해 효과음과 반복 배경음을 재생할 수 있고,
전체·효과음·배경음 볼륨, 일시정지, 좌우 팬, 페이드를 제공합니다. 2D·2.5D·3D 모두 같은 API를 씁니다.
HEADLESS에서도 믹서와 예제가 동작하며, 스피커 없이 PCM 출력을 검사합니다. 자세한 사용법과 파일 수명 규칙은 [`docs/audio.md`](docs/audio.md)를 보세요.
기존 2D/3D 기본 SDK ZIP에는 선택 오디오 모듈이 포함되지 않습니다.

### 파티클 시작하기 (별도 선택 설치)

불꽃·연기·비 프리셋, 버스트·연속 발생, 중력, 수명에 따른 크기·색상·투명도를 제공합니다.
기본 빌드와 2D/3D 기본 SDK ZIP에는 포함되지 않습니다. `PIXELICA_BUILD_PARTICLES=ON`으로 켜거나,
별도 `PixelicaParticles` ZIP만 추가로 받으면 됩니다.

```sh
cmake -S . -B build-particles -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_PARTICLES=ON
cmake --build build-particles --config Release
```

`Pixelica::particles`는 렌더러 없이 동작하는 계산 모듈, `::particles2d`는 2D 그리기,
`::particles25d`는 3D 공간의 깊이·투명도를 처리하는 빌보드 그리기입니다.
빌보드는 기존 hybrid를 사용하므로 2D·3D 기본 SDK가 모두 필요합니다.
[설치·예제·메모리 관리 안내](docs/particles.md)를 참고하세요.

### 쉐이더 시작하기 (별도 선택 설치)

`PixelicaShaders-0.1.0-<platform>.zip`을 기존 기본 SDK와 같은 부모 폴더에 풀면 추가됩니다.
2D·2.5D·3D가 같은 모듈을 사용하며 기본 파일을 교체하지 않습니다.

```cmake
find_package(PixelicaShaders CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE Pixelica::shaders)
```

소스 빌드는 `-DPIXELICA_BUILD_SHADERS=ON`으로 선택합니다. RGB565 CPU 후처리 모듈로,
GLSL/HLSL 실행기는 제공하지 않습니다. [효과·예제·설치 안내](docs/shaders.md)를 참고하세요.

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

사용할 때는 `-DCMAKE_PREFIX_PATH=<sdk-prefix>`를 지정합니다. 컴포넌트는 `common`, `core`, `renderer2d`, `hybrid`, `skeleton`, `ik`, `input_manager`, `online`, `audio`, `audio_sdl3`, `particles`, `particles2d`, `particles25d`입니다.
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
두 ZIP 모두 UI의 Debug/Release 라이브러리, 헤더, CMake 설정, 문서와 독립 실행 예제를 포함합니다.

파티클은 `python tools/package_particles.py --build-dir build-package-particles --output-dir dist-particles`로
별도 추가 ZIP과 소스 ZIP을 만듭니다. 기본 SDK와 같은 폴더에 추가 ZIP을 마지막으로 풀어 설치하세요.

쉐이더는 `python tools/package_shaders.py --build-dir build-package-shaders --output-dir dist-shaders`로
별도 추가 ZIP(Debug/Release)과 소스 ZIP을 만듭니다. 설치 패키지 이름은 `PixelicaShaders`입니다.

## 디렉터리 구성

```text
include/pixelica/   공개 헤더 (pixelica.h, renderer2d.h, ...)
src/                common + core
modules/            ui (기본), renderer2d, hybrid, skeleton, ik, input_manager, online, audio, particles, shaders
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
- [`docs/editor.md`](docs/editor.md): 2D 에디터 구조·저장·실행·3D 확장 지점
- [`docs/collision2d.md`](docs/collision2d.md): 2D 충돌·중력·점프 API와 에디터 설정
- [`docs/api.md`](docs/api.md), [`docs/architecture.md`](docs/architecture.md)
- [`docs/sdk.md`](docs/sdk.md), [`docs/porting.md`](docs/porting.md)
- [`docs/2d.md`](docs/2d.md), [`docs/animation.md`](docs/animation.md), [`docs/rigging.md`](docs/rigging.md)
- [`docs/input_manager.md`](docs/input_manager.md), [`docs/online.md`](docs/online.md)
- [`docs/ui.md`](docs/ui.md): 기본 UI, 입력·포커스·레이아웃·사용자 글꼴·설치 예제
- [`docs/audio.md`](docs/audio.md): 선택 오디오 모듈, PC 출력과 임베디드 포트 준비
- [`docs/particles.md`](docs/particles.md): 선택 파티클 모듈, 별도 다운로드·설치·프리셋
- [`docs/shaders.md`](docs/shaders.md): 선택 CPU 쉐이더, 별도 다운로드·설치·사용자 픽셀 효과
- [`docs/asset_format.md`](docs/asset_format.md), [`docs/art_style.md`](docs/art_style.md)

## 개발자 문서 웹사이트

`website/`에 Docusaurus 기반 Pixellica 개발자 포털이 있습니다. 3단 문서 레이아웃, 라이트/다크 모드, 로컬 검색, Sandpack 웹 데모와 인터랙티브 구조도를 제공합니다. 기존 `docs/` 원문과 실제 C 예제는 사이트 빌드 시 자동으로 가져옵니다.

```sh
cd website
npm ci
npm start
```

문서 작성, 컴포넌트 추가, 버전 관리와 GitHub Pages 배포는 [website/README.md](website/README.md)를 참고하세요.
