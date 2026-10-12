# 선택 모듈: 오디오 (Audio)

> 효과음과 배경음을 섞는 **작고 선택적인 PCM 믹서**입니다. 2D·3D·헤드리스 어떤 렌더 프로필에서도 똑같이 쓰며 기본값은 `OFF`입니다.
> 이 문서는 **계약과 사용법**을 설명하며 자동 검증 결과는 10장에 정리했습니다.
> 실제 스피커에서의 청감 확인과 보드 출력 검증은 아직 하지 않았습니다(9장).

| 항목 | 값 |
|---|---|
| 옵션 | `PIXELICA_BUILD_AUDIO` (기본 `OFF`) |
| 포터블 믹서 | `pixelica_audio` / `Pixelica::audio` — 헤더 [`pixelica/audio.h`](../modules/audio/include/pixelica/audio.h), SDL 의존성 없음, 힙 사용 없음, 파일 입출력 없음 |
| PC 출력(선택) | `pixelica_audio_sdl3` / `Pixelica::audio_sdl3` — 헤더 `pixelica/audio_sdl3.h`, `PIXELICA_PLATFORM=SDL3`일 때만 |
| 설치 컴포넌트 | `audio`(`PixelicaAudioTargets.cmake`), `audio_sdl3`(`PixelicaAudioSDL3Targets.cmake`) |
| 예제 | `hello_audio` (`examples/hello_audio/main.c`) |

## 1. 빠른 빌드

포터블 믹서만 (SDL 없이, 오프라인 렌더링과 보드 이식용):

```bash
cmake -S . -B build -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_AUDIO=ON
cmake --build build
./build/hello_audio --offline
```

PC 스피커 출력까지 (SDL3 필요):

```bash
cmake -S . -B build-sdl -DPIXELICA_PLATFORM=SDL3 -DPIXELICA_FETCH_SDL3=ON -DPIXELICA_BUILD_AUDIO=ON
cmake --build build-sdl
./build-sdl/hello_audio             # 약 8초 재생, 스스로 종료
./build-sdl/hello_audio --smoke     # 약 1초, 테스트용
```

`hello_audio`는 소리 파일 없이 코드에서 만든 작은 WAV 두 개(반복 BGM, 효과음)를 씁니다. 항상 오프라인 믹스 체크섬과 0이 아닌 샘플 수를 출력하고
(SDL 없이도 동작), SDL 어댑터가 있으면 이어서 기본 출력 장치로 재생합니다. 콘솔 입력을 기다리지 않습니다. 스피커 없는 환경은
`SDL_AUDIO_DRIVER=dummy`(`SDL_AUDIODRIVER=dummy`도 인식)로 실행하세요.

Windows Visual Studio에서는 `cmake --build build-sdl --config Release`로 빌드하고
`.\build-sdl\Release\hello_audio.exe`를 실행합니다. 헤드리스도 같은 방식으로 `build\Release\`에서 실행하세요.

## 2. 최소 API

아래 코드는 호출 순서를 보여 주는 조각입니다. 실제 게임은 각 `PIXELICA_Status` 반환값을 확인한 뒤 다음 단계로 진행하세요.
오류 처리와 종료까지 포함한 실행 가능한 예제는 [`hello_audio/main.c`](../examples/hello_audio/main.c)입니다.

### 포터블 믹서

```c
#include "pixelica/audio.h"

static PIXELICA_AudioMixer mixer;          /* 호출자가 소유, 힙 없음 */
PIXELICA_AudioClip clip;
PIXELICA_AudioVoice voice = 0;

pixelica_audio_init(&mixer, 48000);                          /* 8000..96000 Hz */
pixelica_audio_clip_wav(&clip, wav_bytes, wav_size);         /* WAV 파싱, 복사 없음 */
pixelica_audio_play(&mixer, &clip, NULL, &voice);            /* NULL = 기본 옵션 */
pixelica_audio_play_music(&mixer, &bgm, &music);             /* 반복, MUSIC 버스, 이전 음악 교체 */

/* 오디오 장치의 요청 또는 게임 루프마다 */
float out[512 * 2];                                          /* 인터리브 스테레오 */
pixelica_audio_render(&mixer, out, 512);
```

- 음성(voice)은 최대 `PIXELICA_AUDIO_MAX_VOICES`(16)개입니다. 가득 차면 `PIXELICA_ERR_CAPACITY`이고 기존 음성을 빼앗지 않습니다.
- `PIXELICA_AudioVoice`는 세대가 들어 있는 64비트 핸들입니다. `0`은 항상 무효이고, 같은 초기화 수명 안에서 끝나거나 멈춘 음성의 핸들은 재사용되지 않습니다.
  핸들은 해당 믹서에서만 사용하세요. `pixelica_audio_init`을 다시 호출하면 모든 이전 핸들을 버려야 합니다. 재생을 모두 멈출 때는 `stop_all`을 사용합니다.
- `pixelica_audio_default_options()`: SFX 버스, 반복 없음, 볼륨 1, 팬 0.
- 출력 `render`는 항상 믹서 샘플레이트의 스테레오 `float`이며 합계를 `[-1, 1]`로 자릅니다. 대상 버퍼는 매번 덮어씁니다(무음도 0으로 채움).
- 모든 함수는 NULL, 범위 밖, 유한하지 않은 값(NaN, 무한대)을 오류로 처리합니다. 믹서는 **단일 스레드**용입니다.

### SDL3 출력

```c
#include "pixelica/audio_sdl3.h"

PIXELICA_AudioSDL3 audio = {0};
PIXELICA_AudioWav  boom  = {0};
PIXELICA_AudioVoice v = 0;

pixelica_audio_sdl3_open(&audio, 48000);
pixelica_audio_sdl3_load_wav(&boom, "boom.wav");
pixelica_audio_play(pixelica_audio_sdl3_mixer(&audio), &boom.clip, NULL, &v);

while (running) {
    /* 게임 로직 ... */
    pixelica_audio_sdl3_update(&audio);   /* 매 프레임, 게임이 일시정지여도 */
}

pixelica_audio_sdl3_unload_wav(&audio, &boom);
pixelica_audio_sdl3_close(&audio);
```

CMake 연결 (소스 트리 안): `target_link_libraries(my_game PRIVATE pixelica_audio_sdl3)`. 설치 패키지에서는 5장을 보세요.

## 3. 메모리 소유권

- **믹서**(`PIXELICA_AudioMixer`)와 SDL 래퍼(`PIXELICA_AudioSDL3`)는 호출자가 만든 구조체입니다. 포터블 믹서는 힙을 쓰지 않으며 SDL 장치·파일 로딩은 메모리를 할당합니다.
  내부 필드는 구현 저장소일 뿐이며 레이아웃, 직렬화, ABI를 약속하지 않습니다.
- `PIXELICA_AudioSDL3` 안에 믹서가 들어 있습니다. `{0}`으로 초기화한 뒤 `open`하고, 연 뒤에는 **복사하거나 옮기지 마세요**.
- **클립**(`PIXELICA_AudioClip`)은 PCM 바이트를 *빌려* 씁니다. `play` 때 클립 구조체(메타데이터)는 복사되지만 `samples`가 가리키는 바이트는 복사되지 않습니다.
  그 바이트는 해당 클립을 쓰는 모든 음성이 멈출 때까지 살아 있고 바뀌지 않아야 합니다.
- 바이트를 해제하기 전에 `pixelica_audio_stop_clip(&mixer, &clip)`으로 그 클립을 쓰는 음성을 멈추세요. `samples` 포인터가 같은 음성이 모두 멈춥니다.
- `pixelica_audio_clip_wav`는 복사도 할당도 하지 않습니다. `out->samples`는 `bytes` 안을 가리킵니다. 실패하면 `*out`은 그대로입니다.
- `PIXELICA_AudioWav`(SDL3 전용)는 `SDL_LoadFile`로 읽은 바이트를 **소유**합니다. 반드시 `pixelica_audio_sdl3_unload_wav`로 해제하세요.
  이미 불러온 `PIXELICA_AudioWav`에 다시 `load_wav`하면 누수 대신 `PIXELICA_ERR_INVALID_ARG`로 거부합니다. 실패한 로드는 구조체를 바꾸지 않습니다.
- `unload_wav(&audio, &wav)`는 먼저 `audio`의 믹서에서 그 클립을 쓰는 음성을 멈추고 해제합니다. **다른 믹서**에서도 같은 클립을 재생 중이면
  그 믹서에서 먼저 `pixelica_audio_stop_clip`을 호출해야 합니다.

## 4. 지원 형식과 내보내기 팁

| 항목 | 지원 |
|---|---|
| 컨테이너 | RIFF/WAVE, 포맷 태그 1 (비압축 PCM) |
| 샘플 | unsigned 8-bit (`PIXELICA_AUDIO_U8`), signed 16-bit little-endian (`PIXELICA_AUDIO_S16LE`) |
| 채널 | 모노, 스테레오 |
| 샘플레이트 | 8000 ~ 96000 Hz (믹서 레이트와 달라도 선형 보간으로 변환) |

파서는 `fmt `/`data` 청크를 경계 검사하며 읽습니다. RIFF가 선언한 범위를 따르고, 청크 패딩과 모르는 청크를 처리합니다.
일부 WAV 작성기와의 호환을 위해 RIFF 마지막의 홀수 길이 청크에 패딩 바이트가 없는 경우도 허용합니다.
잘리거나 지원하지 않는 포맷, 산술 오버플로, 중복된 `fmt `/`data`, 프레임 경계에 맞지 않는 데이터, 샘플 0개는 `PIXELICA_ERR_BAD_ASSET`입니다.

내보내기 팁 (Audacity 등):

- **WAV (Microsoft) signed 16-bit PCM**으로 내보내세요. 압축 WAV(ADPCM), float 32, 24-bit, MP3/OGG는 지원하지 않습니다.
- 효과음은 **모노**가 메모리를 절반 쓰며 팬이 잘 동작합니다. 스테레오 음악은 스테레오 그대로 둡니다.
- 믹서 레이트와 같은 샘플레이트로 맞추면 보간 비용이 없습니다. 임베디드에서는 22050 Hz 모노, 8-bit나 16-bit가 현실적입니다.
- 반복 음악은 시작과 끝 값이 이어지게(제로 크로싱 근처에서 자르기) 만들면 이음매 소리가 줄어듭니다.
- 소리 파일을 코드에 넣을 때는 `const uint8_t[]` 배열로 만들고 `pixelica_audio_clip_wav`에 넘깁니다(정렬 요구 없음, 구조체 캐스팅 없음).

## 5. 설치 컴포넌트

기본 설치와 기존 SDK 압축 파일에는 오디오가 **들어가지 않습니다**.
소스 빌드에서 오디오를 켜고 일반 `cmake --install`을 실행하면 빌드된 오디오도 설치됩니다.
컴포넌트별로 설치하려면 다음처럼 공통 코드와 오디오를 같은 경로에 설치하세요.

```sh
cmake --install build-sdl --config Release --prefix out/audio-sdk --component common
cmake --install build-sdl --config Release --prefix out/audio-sdk --component audio
cmake --install build-sdl --config Release --prefix out/audio-sdk --component audio_sdl3
```

HEADLESS에서는 마지막 `audio_sdl3` 단계를 빼세요. 소비자 프로젝트를 구성할 때는
`-DCMAKE_PREFIX_PATH=<out/audio-sdk의 절대 경로>`를 지정합니다.

| 컴포넌트 | 내용 | 조건 |
|---|---|---|
| `audio` | `pixelica/audio.h`, 정적 라이브러리, `Pixelica::audio`, 이 문서 | `PIXELICA_BUILD_AUDIO=ON` |
| `audio_sdl3` | `pixelica/audio_sdl3.h`, `Pixelica::audio_sdl3` | 위에 더해 `PIXELICA_PLATFORM=SDL3` |

```cmake
# 순수 믹서만 (SDL 불필요)
find_package(Pixelica CONFIG REQUIRED COMPONENTS audio)
target_link_libraries(my_game PRIVATE Pixelica::audio)

# PC 출력 (SDL3 3.4.18 이상을 이 컴포넌트를 요청했을 때만 찾습니다)
find_package(Pixelica CONFIG REQUIRED COMPONENTS audio audio_sdl3)
target_link_libraries(my_game PRIVATE Pixelica::audio_sdl3)
```

`audio` 컴포넌트는 SDL을 찾지 않습니다. `audio_sdl3`는 소비자 프로젝트에서 설치된 SDL3 3.4.18 이상을 찾습니다.
Pixelica 빌드에서 `PIXELICA_FETCH_SDL3=ON`으로 받은 SDL은 소비자에게 자동 설치되거나 번들되지 않습니다.
SDL을 설치하고 `SDL3_DIR` 또는 `CMAKE_PREFIX_PATH`로 위치를 지정하세요. 게임별 소스 포함 방식에서는 이미 빌드한 SDL 타깃을 그대로 씁니다.

## 6. 일시정지, 페이드, 게인

모든 제어 함수는 잘못된 값이면 `PIXELICA_ERR_INVALID_ARG`, 살아 있지 않은 핸들이면 `PIXELICA_ERR_NOT_FOUND`를 돌려줍니다.

| 함수 | 의미 |
|---|---|
| `pixelica_audio_pause(m, v, paused)` | 일시정지/재개. 정지된 음성도 `is_playing`은 `true`이고 위치와 진행 중인 페이드가 멈춥니다. |
| `pixelica_audio_set_volume(m, v, vol)` | 음성 볼륨 `[0,1]`. 진행 중인 페이드를 취소합니다. |
| `pixelica_audio_set_pan(m, v, pan)` | 팬 `[-1,1]`, 선형 밸런스. 0은 양쪽 채널을 그대로 둡니다(스테레오 중앙 보존). -1은 오른쪽, +1은 왼쪽을 끕니다. |
| `pixelica_audio_fade(m, v, target, seconds, stop_when_done)` | 현재 볼륨에서 `target`까지 선형 램프. **샘플 단위 시간**(렌더된 프레임 수 기준)이라 믹서가 렌더되는 만큼만 진행합니다. `seconds=0`이면 즉시 적용. `stop_when_done`이면 끝에 음성을 멈춥니다. |
| `pixelica_audio_set_master_volume(m, vol)` | 전체 게인 `[0,1]`. |
| `pixelica_audio_set_bus_volume(m, bus, vol)` | `PIXELICA_AUDIO_SFX` / `PIXELICA_AUDIO_MUSIC` 버스 게인 `[0,1]`. |

최종 샘플은 `clamp(master * sum(음성 볼륨 * 버스 * 팬), -1, 1)`입니다. 많이 겹치면 합이 잘리므로(클리핑) 음성 볼륨을 낮추세요.
`pixelica_audio_play_music`은 이전 음악 음성을 같은 자리에서 교체합니다. 클립이 잘못되었거나 믹서가 가득 차 실패하면 이전 음악이 그대로 남습니다.

SDL3 출력에서 이 제어는 **이후에 믹스되는 소리**에만 영향을 줍니다(다음 7장).

## 7. SDL3 출력 펌프의 의미

- 콜백이나 믹스 스레드가 **없습니다**. 믹서의 모든 호출과 `pixelica_audio_sdl3_update`는 같은 스레드(보통 게임 루프)에서 해야 합니다.
- `open`은 `SDL_InitSubSystem(SDL_INIT_AUDIO)` 참조 하나를 가져가고 기본 재생 장치에 f32 스테레오 스트림(믹서 레이트)을 엽니다. SDL이 장치 포맷으로 변환합니다.
  `close`는 음성을 모두 멈추고 스트림을 파괴하며 **자기 참조만** 돌려줍니다. 비디오 등 다른 서브시스템과 `SDL_Quit`는 건드리지 않습니다.
  `close`는 0으로 초기화된 구조체나 이미 닫힌 구조체에도 안전합니다.
- `update`는 스트림에 쌓인 바이트를 확인해 약 **50 ms**보다 적으면 부족한 만큼만 최대 512프레임 블록으로 렌더해 넣습니다.
  한 번 호출에 최대 8블록이므로 오래 멈췄다 돌아와도 한꺼번에 따라잡지 않습니다. 큐가 차 있으면 거의 비용이 없습니다.
- 큐에는 약 **50 ms 분량의 원본 PCM**을 유지하며 장치·운영체제 버퍼가 추가 지연을 만들 수 있습니다. 총 출력 지연의 상한은 보장하지 않습니다.
  볼륨, 팬, 정지, 페이드 같은 제어는 이미 큐에 들어간 소리를 되돌릴 수 없어 큐와 장치 버퍼만큼 늦게 들릴 수 있습니다.
  `unload_wav`나 `stop`을 해도 이미 큐에 렌더된 샘플은 끝까지 재생됩니다(원본 바이트가 아니라 복사본이라 메모리 안전은 유지됩니다).
- `update`는 **게임이 일시정지 중이어도 매 프레임** 호출하세요. 호출이 약 50 ms 넘게 끊기면 장치가 비어 소리가 끊깁니다(언더런). 스트리밍과 콜백 출력은 아직 제공하지 않습니다.
- 오류: `PIXELICA_ERR_INVALID_ARG`(NULL이거나 열리지 않음), `PIXELICA_ERR_PLATFORM`(SDL이 상태를 못 읽거나 데이터를 거부, 예: 장치 분리, `SDL_GetError()` 참고).
  `open`은 장치를 열지 못해도 SDL 참조를 균형 있게 돌려놓습니다.

## 8. 보드 이식 준비 (예: ESP32, pull 방식)

믹서는 SDL을 모르는 포터블 C입니다. 보드에서는 오디오 하드웨어가 샘플을 **당겨가는(pull)** 구조로 어댑터를 쓰면 됩니다.
`pixelica_audio_render_s16`은 할당 없이 네이티브 엔디언 `int16_t` 인터리브 스테레오를 채웁니다.

```c
static PIXELICA_AudioMixer g_mixer;          /* pixelica_audio_init 으로 보드의 출력 레이트 지정 */
static int16_t g_block[256 * 2];             /* 드라이버가 요구하는 블록 크기에 맞출 것 */

/* 보드 드라이버 루프(태스크 하나): 블록을 렌더해서 드라이버에 쓴다. */
void audio_task_step(void)
{
    pixelica_audio_render_s16(&g_mixer, g_block, 256);
    /* board_i2s_write(g_block, sizeof(g_block));   <- 보드/드라이버별 호출, 여기서 정하지 않음 */
}
```

- 이 저장소는 **핀 배치, I2S 설정, DAC/앰프 종류, 드라이버 호출을 정하지 않으며** 실제 보드에서 검증하지도 않았습니다. 위 `board_i2s_write`는 이름만 가정한 자리 표시입니다.
- 믹서는 단일 스레드입니다. 게임 로직 태스크가 `play`/`pause`/`fade`를 부르고 오디오 태스크가 `render_s16`을 부르려면
  호출자가 하나의 스레드로 합치거나 직접 잠금/명령 큐를 두어야 합니다. 라이브러리는 잠금을 제공하지 않습니다.
- 모노 하드웨어는 스테레오 출력을 어댑터에서 평균해 쓰세요. 8-bit 출력은 `(s + 32768) >> 8` 같은 변환을 어댑터에서 합니다.
- 클립은 플래시(`const` 배열)에 둘 수 있습니다. 파서와 믹서는 정렬되지 않은 바이트를 안전하게 읽습니다.
- 렌더링 비용은 활성 음성 수와 클립 레이트 변환에 비례합니다. 블록은 오디오 지연이 허용하는 한 크게, 태스크 우선순위는 렌더 시간이 블록 시간보다 짧게 잡으세요.

## 9. 제한사항

- 지원 형식은 비압축 PCM(8/16-bit, 모노/스테레오)뿐입니다. MP3/OGG 등 압축 코덱, 디스크 스트리밍, 효과(DSP), 3D 음향, 하드웨어별 I2S 구현은 **없습니다**.
- 동시 음성 16개 고정입니다. 가득 차면 새 음성이 거부됩니다(`PIXELICA_ERR_CAPACITY`).
- 샘플레이트 변환은 선형 보간이라 고음에서 앨리어싱이 생길 수 있습니다. 가능하면 믹서와 같은 레이트로 내보내세요.
- 스레드에 안전하지 않습니다. 콜백이나 별도 믹스 스레드 모델은 아직 없습니다.
- SDL 출력은 기본 재생 장치 하나만 엽니다. 장치 선택, 장치 변경 감지, 재연결은 구현하지 않았습니다.
- 위 설명은 설계 계약입니다. 이 문서는 실제 스피커, 헤드폰, 보드에서 소리가 나는지 확인했다고 주장하지 않습니다. 더미 오디오 드라이버(`SDL_AUDIO_DRIVER=dummy`)는
  재생 경로가 오류 없이 도는지만 볼 수 있고 소리가 들리는지는 알려주지 않습니다.


## 10. 자동 검증 (2026-10-10)

최종 코드에서 관찰한 로컬 결과입니다. MSVC/GCC 경고를 오류로 처리했으며 Linux sanitizer 구성은
ASan·UBSan·누수 검사를 켰습니다. SDL 자체는 별도 빌드한 3.4.18 라이브러리이며 sanitizer 계측 대상은 Pixelica 코드입니다.

| 구성 | CTest 결과 |
|---|---|
| Windows x64 MSVC Release, HEADLESS, 2D+3D 및 오디오·입력 매니저·스켈레톤·IK | 38/38 |
| Windows x64 MSVC Release, SDL3, 2D+오디오 | 19/19 |
| Linux x64 GCC Debug, HEADLESS, 위 공통 모듈 + ASan/UBSan/누수 검사 | 35/35 |
| Linux x64 GCC Debug, SDL3, 2D+오디오 + ASan/UBSan/누수 검사 | 17/17 |
| Linux x64 GCC Release, 두 렌더러 OFF, 공통+오디오 | 13/13 |

모든 구성에서 오디오 단위 검사 6,029개가 통과했습니다. WAV 파싱·잘린 데이터·무작위 입력,
PCM 변환, 리샘플링·반복, 스테레오·팬·클리핑, 용량·음악 교체, 오래된 ID, 일시정지와 긴 페이드 진행 등을 검사합니다.
SDL dummy 장치 검사는 Windows/Linux에서 각각 122개가 통과했고 장치 열기·닫기·참조 수명,
출력 큐, 실제 파일 로딩과 해제를 확인했습니다. 실제 스피커 출력이나 청감 품질 검사는 아닙니다.

설치 파일만 사용하는 별도 소비자 프로그램도 검증했습니다. Windows에서는 순수 믹서와 SDL 출력 컴포넌트,
Linux 렌더러 없는 구성에서는 순수 믹서가 설치된 패키지로 빌드·실행됐습니다.
오디오 OFF 구성은 10/10 검사가 통과했고 설치 결과에 오디오 헤더·라이브러리·내보내기가 없음을 확인했습니다.
합성 예제의 1초 PCM 결과는 모든 환경에서 체크섬 `0x11CD8E9B`, 0이 아닌 샘플 86,751개, 최대 절댓값 19,331로 같았습니다.

감사에서 새 믹서 메모리를 초기화 전에 읽는 문제, ID 카운터 재사용, 긴 페이드의 누적 오차를 수정하고 회귀 검사를 추가했습니다.
핸들은 믹서 초기화 수명 안에서만 유효합니다. 기존 SDK ZIP의 재생성, GitHub Actions 실행·업로드,
실제 스피커와 ESP32 보드 출력은 이 결과에 포함되지 않습니다.

SDL을 소스에서 포함하는 구성도 로컬 소스로 구성·내보내기를 확인했습니다(추가 다운로드 없음).
해당 내보내기 연결 수정 후 Windows의 설치 오디오·설치 2D·패키지 선택 검사를 다시 실행해 3/3 통과했습니다.
