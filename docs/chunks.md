# 청크(Chunk) 관리자

`pixelica/chunks.h`는 정수 청크 좌표 `(x, y, z)`의 집합을 **원하는 범위**와 같아지도록
유지하는 작은 모듈입니다. 실제 작업은 게임이 넘긴 `load` / `unload` 콜백이 합니다.
힙·파일시스템·렌더러·스레드·전역 상태가 없고, 메모리는 모두 호출자 소유입니다.
2D·2.5D·3D 모두 같은 API를 쓰며, **2D는 z를 항상 0**으로 둡니다.

이 모듈이 하지 **않는** 일: 청크 데이터 저장, 파일 읽기, 그리기, 컬링, ECS, 비동기
로딩. 청크 크기, 저장소, I/O, 프레임당 작업량(budget), 모든 리소스는 게임이
정하고 소유합니다.

## 타입

관리자는 첫 초기화 전에 `PIXELICA_Chunks chunks = {0};`처럼 0으로 초기화하세요.
작업 횟수 출력 포인터는 관리자나 슬롯 배열과 겹치면 안 됩니다.
처리량 제한은 콜백 호출 횟수이며 시간 제한이 아닙니다. 콜백 하나가 오래 걸리면 해당 프레임도 오래 걸립니다.
실패한 로딩 콜백은 자신이 부분적으로 할당한 자원을 직접 정리한 뒤 오류를 반환해야 합니다.

| 타입 | 의미 |
| --- | --- |
| `PIXELICA_ChunkCoord { int32_t x, y, z; }` | 청크 좌표 |
| `PIXELICA_ChunkRange { min, max }` | **양 끝 포함** 범위 (`min.x <= x <= max.x`, y·z 동일) |
| `PIXELICA_ChunkSlot { coord, loaded }` | 호출자가 빌려주는 슬롯 배열의 원소 |
| `PIXELICA_Chunks` | 관리자 상태 (호출자 소유, `count` = 로드된 청크 수) |

콜백:

```c
PIXELICA_Status load(void *userdata, PIXELICA_ChunkCoord coord);   /* OK = 준비 완료 */
void unload(void *userdata, PIXELICA_ChunkCoord coord);            /* 실패 불가 */
```

두 콜백 모두 **필수**입니다. 할 일이 없으면 빈 함수를 넘기세요.

## 함수

| 함수 | 설명 |
| --- | --- |
| `pixelica_chunks_init(out, slots, capacity, load, unload, userdata)` | 모든 검사 후에만 슬롯과 상태를 0으로 초기화. 콜백 없음 |
| `pixelica_chunk_coord(position, chunk_size, &out)` | 축별 `floor(position / size)`. 음수 위치 지원(-0.5/1 → -1), 축마다 다른 양수 크기 허용, int32 범위 검사 |
| `pixelica_chunk_range(center, radius, &out)` | `[center - radius, center + radius]`. radius >= 0, int32 범위 검사 |
| `pixelica_chunks_set_range(c, range)` | 원하는 범위를 설정/교체. **콜백 없음**. 부피가 capacity 초과면 `CAPACITY` |
| `pixelica_chunks_update(c, max_ops, &ops)` | 실제 로드/언로드 수행 (아래 순서) |
| `pixelica_chunks_contains(c, coord)` | 로드되어 있으면 `true` |
| `pixelica_chunks_clear(c)` | 전부 언로드하고 원하는 범위를 잊음. 이후 다시 `set_range` 또는 `init` 가능 |

### update 순서 (결정적)

1. 범위 **밖**의 로드된 슬롯을 슬롯 인덱스 순서로 언로드합니다.
2. 1단계가 끝난 경우에만, 범위 안의 빠진 좌표를 **z → y → x 순서**(x가 가장 빠르게
   변함)로 로드합니다.

- 성공한 콜백 하나가 작업 1회입니다. `max_ops == 0`이면 무제한입니다. budget을 다
  쓰면 `OK`를 반환하고 다음 `update`에서 이어갑니다.
- `load`가 실패하면 즉시 멈추고 **그 상태 코드를 그대로** 반환합니다. 그 호출에서
  앞서 성공한 작업은 유효하게 남고, 실패한 좌표는 비어 있다가 다음 `update`에서 다시
  시도됩니다. `ops`에는 실패 시에도 성공한 횟수가 들어갑니다(`NULL` 허용).
- `set_range`를 한 번도 호출하지 않았으면(또는 `clear` 후) `update`는 아무것도 안 합니다.
- 작업량은 좌표 공간이 아니라 capacity에 묶입니다. 조회가 선형이라 최악 O(capacity²)
  이므로 capacity는 수백 개 이하로 두세요.

## 오류: 잘못된 입력 vs 부분 진행

| 상황 | 반환 | 변화 |
| --- | --- | --- |
| NULL, 초기화 안 된 관리자, `min > max`, radius < 0, NaN/무한대, 크기 <= 0, int32 넘침 | `INVALID_ARG` | 없음 |
| 콜백 안에서 같은 관리자에 `init/set_range/update/clear` | `INVALID_ARG` | 없음 |
| 로드된 청크가 남은 관리자에 다시 `init` | `INVALID_ARG` | 없음 (먼저 `clear`) |
| 범위 부피 > capacity | `CAPACITY` | 없음 (이전 목표 유지) |
| `update` 중 `load` 실패 | 콜백의 상태 | **부분 진행** (위 설명) |

잘못된 입력은 트랜잭션처럼 동작합니다: 아무것도 바뀌거나 쓰이지 않고 콜백도 실행되지
않습니다. 오류를 반환하면서 부분 진행이 남는 경우는 `load` 실패뿐입니다.

## 소유권과 수명 (모두 빌림, 모듈은 아무것도 해제하지 않음)

- `PIXELICA_Chunks`: 호출자 소유, `init`부터 사용이 끝날 때까지 **주소가 고정**되어야
  합니다. 한 스레드에서만 사용합니다. 필드는 읽어도 되지만(`count`) 수정하면 안 됩니다.
- `slots`: `capacity`개짜리 배열을 빌려주며, `init` 이후에는 관리자가 **독점**합니다.
  읽기는 괜찮지만(`loaded == true`인 원소 = 로드된 청크) 쓰면 안 됩니다.
- `load` / `unload` / `userdata`: 로드된 청크가 있는 동안 유효해야 합니다.
- 콜백은 **동기식**입니다. `update`/`clear` 안에서 실행되고, 반환 시 작업이 끝나 있어야
  합니다. 내장 비동기는 없습니다. 백그라운드 로딩이 필요하면 게임이 모듈 밖에 자기
  큐와 준비 상태·취소 처리를 만드세요. 단순히 요청만 넣고 `OK`를 반환하면 이 관리기는
  이미 로드 완료된 청크로 간주하므로, 준비 완료와 같은 뜻으로 사용하면 안 됩니다.
- 콜백 안에서는 `pixelica_chunks_contains()`와 필드 읽기만 허용됩니다.

## 예제: 2D (z = 0, 카메라 줌에 따른 보이는 범위)

`camera.h` 규칙: 보이는 월드 사각형은 `position .. position + viewport / zoom`입니다.
그 두 모서리의 청크를 구해 범위로 쓰면 됩니다. 줌아웃으로 범위가 capacity를 넘으면
`set_range`가 `CAPACITY`를 반환하고 이전 목표가 유지되므로, 줌을 제한하거나 capacity를
키우세요.

```c
#include <stdio.h>
#include "pixelica/camera.h"
#include "pixelica/chunks.h"

typedef struct Game { int loaded_tiles; } Game;

static PIXELICA_Status load_tiles(void *userdata, PIXELICA_ChunkCoord c)
{
    Game *g = (Game *)userdata;
    g->loaded_tiles++;           /* 실제로는 타일맵을 채우거나 파일에서 읽음 */
    printf("load %d,%d\n", (int)c.x, (int)c.y);
    return PIXELICA_OK;
}

static void unload_tiles(void *userdata, PIXELICA_ChunkCoord c)
{
    Game *g = (Game *)userdata;
    g->loaded_tiles--;
    printf("unload %d,%d\n", (int)c.x, (int)c.y);
}

/* 보이는 영역 -> 청크 범위. 2D이므로 z = 0. */
static PIXELICA_Status visible_range(const PIXELICA_Camera2D *cam, float view_w, float view_h,
                                     PIXELICA_ChunkRange *out)
{
    const PIXELICA_Vec3 size = { 256.0f, 256.0f, 1.0f }; /* 16x16 타일 * 16px */
    PIXELICA_Vec3 lo = { cam->position.x, cam->position.y, 0.0f };
    PIXELICA_Vec3 hi = { cam->position.x + view_w / cam->zoom,
                         cam->position.y + view_h / cam->zoom, 0.0f };
    PIXELICA_Status st = pixelica_chunk_coord(lo, size, &out->min);

    if (st == PIXELICA_OK) {
        st = pixelica_chunk_coord(hi, size, &out->max);
    }
    return st;
}

int main(void)
{
    static PIXELICA_ChunkSlot slots[64];
    static PIXELICA_Chunks chunks; /* 0으로 초기화된 새 관리자 */
    PIXELICA_Camera2D cam = { { -100.0f, 40.0f }, 2.0f };
    PIXELICA_ChunkRange range;
    Game game = { 0 };
    size_t ops = 0;

    if (pixelica_chunks_init(&chunks, slots, 64, load_tiles, unload_tiles, &game) != PIXELICA_OK) {
        return 1;
    }
    /* 매 프레임 */
    if (visible_range(&cam, 640.0f, 360.0f, &range) == PIXELICA_OK &&
        pixelica_chunks_set_range(&chunks, range) == PIXELICA_OK) {
        pixelica_chunks_update(&chunks, 4, &ops); /* 프레임당 최대 4개 */
    }
    printf("loaded=%zu ops=%zu\n", chunks.count, ops);

    pixelica_chunks_clear(&chunks); /* 종료: 전부 unload */
    return 0;
}
```

## 예제: 3D (플레이어 주변 반경)

```c
#include "pixelica/chunks.h"

static PIXELICA_Status load_voxels(void *userdata, PIXELICA_ChunkCoord c)
{
    (void)userdata; (void)c;   /* 게임의 복셀 풀에서 할당/생성 */
    return PIXELICA_OK;
}

static void unload_voxels(void *userdata, PIXELICA_ChunkCoord c)
{
    (void)userdata; (void)c;   /* 풀에 반납 */
}

int stream_world(PIXELICA_Chunks *chunks, PIXELICA_Vec3 player)
{
    const PIXELICA_Vec3 size = { 32.0f, 16.0f, 32.0f };   /* 축마다 달라도 됨 */
    const PIXELICA_ChunkCoord radius = { 2, 1, 2 };       /* 5 x 3 x 5 = 75 청크 */
    PIXELICA_ChunkCoord center;
    PIXELICA_ChunkRange range;

    if (pixelica_chunk_coord(player, size, &center) != PIXELICA_OK ||
        pixelica_chunk_range(center, radius, &range) != PIXELICA_OK ||
        pixelica_chunks_set_range(chunks, range) != PIXELICA_OK) {
        return -1;
    }
    return pixelica_chunks_update(chunks, 8, NULL) == PIXELICA_OK ? 0 : -1;
}

int main(void)
{
    static PIXELICA_ChunkSlot slots[128];
    static PIXELICA_Chunks chunks; /* static = 0으로 초기화됨 */
    PIXELICA_Vec3 player = { -10.0f, 3.0f, 70.0f };
    int frame;

    if (pixelica_chunks_init(&chunks, slots, 128, load_voxels, unload_voxels, NULL) !=
        PIXELICA_OK) {
        return 1;
    }
    for (frame = 0; frame < 20; ++frame) {   /* budget 8이라 몇 프레임에 걸쳐 채워짐 */
        player.x += 4.0f;
        if (stream_world(&chunks, player) != 0) {
            break;
        }
    }
    pixelica_chunks_clear(&chunks);
    return 0;
}
```

2.5D(아이소메트릭 등)는 2D와 같이 z = 0을 쓰거나, 층이 있으면 z를 층 번호로 씁니다.
