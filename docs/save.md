# 세이브 / 설정 저장 (`pixelica/save.h`)

게임이 만든 바이트 payload를 작은 버전 포함 바이너리 봉투(envelope)로 감쌉니다.
힙 없는 `encode`/`decode`는 어느 플랫폼에서나 쓸 수 있고(ESP32는 이걸로 NVS/SD에
직접 저장), PC에서는 같은 소스의 파일 헬퍼를 쓰면 됩니다.

## 형식 (모두 little-endian)

| 오프셋 | 크기 | 필드 |
|---|---|---|
| 0 | 4 | magic `"PXSV"` |
| 4 | 2 | 컨테이너 형식 버전 (`PIXELICA_SAVE_FORMAT_VERSION` = 1) |
| 6 | 2 | 예약, 반드시 0 |
| 8 | 4 | schema 버전 (게임이 정함) |
| 12 | 4 | payload 길이 N |
| 16 | N | payload |
| 16+N | 4 | CRC-32 (IEEE), 0..16+N-1 바이트 전체(헤더 포함) |

전체 크기 = N + `PIXELICA_SAVE_OVERHEAD_BYTES`(20). 길이가 정확히 맞지 않거나
(잘림/뒤에 남는 바이트), magic·형식·예약 필드·CRC 중 하나라도 틀리면
`PIXELICA_ERR_BAD_ASSET`입니다. 실패하면 출력과 대상 버퍼는 바이트 단위로 그대로 유지됩니다.

## API

```c
PIXELICA_Status pixelica_save_encoded_size(size_t payload_size, size_t *out_size);
PIXELICA_Status pixelica_save_encode(uint32_t schema_version, const void *payload,
                                     size_t payload_size, void *out,
                                     size_t out_capacity, size_t *out_size);
PIXELICA_Status pixelica_save_inspect(const void *data, size_t data_size,
                                      uint32_t *out_schema_version,
                                      size_t *out_payload_size);
PIXELICA_Status pixelica_save_decode(const void *data, size_t data_size,
                                     uint32_t *out_schema_version,
                                     void *out_payload, size_t payload_capacity,
                                     size_t *out_payload_size);
PIXELICA_Status pixelica_save_file_write(const char *path, uint32_t schema_version,
                                         const void *payload, size_t payload_size);
PIXELICA_Status pixelica_save_file_read(const char *path, uint32_t *out_schema_version,
                                        void *out_payload, size_t payload_capacity,
                                        size_t *out_payload_size);
```

- 버퍼는 모두 호출자 소유입니다. 크기 0이면 포인터는 NULL이어도 됩니다.
- `encode`의 `payload`와 `out`, `decode`의 `data`와 `out_payload`는 겹쳐도 됩니다.
- `inspect`는 복사 없이 전체 검증 후 schema/payload 크기만 알려 줍니다(버퍼 크기 결정용).
- schema 버전은 검사하지 않습니다. 받은 값을 보고 직접 분기/변환하세요.
- 오류: `INVALID_ARG`(NULL 등), `NO_MEMORY`(버퍼 부족), `CAPACITY`(형식/파일 한도 초과),
  `BAD_ASSET`(손상), `PLATFORM`(파일 I/O·교체 실패), `NOT_FOUND`(읽을 파일 없음).

## struct를 그대로 저장하지 마세요

`fwrite(&settings, sizeof settings, ...)`는 padding, endian, `bool`/enum 크기가
컴파일러·CPU마다 달라 호환이 깨집니다. 필드를 하나씩 고정 순서·고정 바이트로 쓰세요.

## 예제 (설정 + 진행도)

```c
#include <stdint.h>
#include <stdio.h>

#include "pixelica/save.h"

#define SETTINGS_SCHEMA 2u /* v1: volume, fullscreen / v2: + best_score */

typedef struct Settings {
    uint8_t volume;     /* 0..100 */
    uint8_t fullscreen; /* 0 or 1 */
    uint32_t best_score;
} Settings;

static size_t settings_pack(const Settings *s, uint8_t out[6])
{
    out[0] = s->volume;
    out[1] = s->fullscreen;
    out[2] = (uint8_t)(s->best_score & 0xFFu);
    out[3] = (uint8_t)((s->best_score >> 8) & 0xFFu);
    out[4] = (uint8_t)((s->best_score >> 16) & 0xFFu);
    out[5] = (uint8_t)(s->best_score >> 24);
    return 6;
}

/* 0 on success; *s is changed only on success. */
static int settings_unpack(uint32_t schema, const uint8_t *p, size_t n, Settings *s)
{
    Settings t;

    if (schema == 1u && n == 2u) {
        t.best_score = 0; /* v1에는 없던 필드: 기본값 */
    } else if (schema == 2u && n == 6u) {
        t.best_score = (uint32_t)p[2] | ((uint32_t)p[3] << 8) |
                       ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
    } else {
        return -1;
    }
    if (p[0] > 100u || p[1] > 1u) {
        return -1;
    }
    t.volume = p[0];
    t.fullscreen = p[1];
    *s = t;
    return 0;
}

int main(void)
{
    Settings s = { 80, 1, 12345u };
    uint8_t buf[16];
    size_t n;
    uint32_t schema;

    n = settings_pack(&s, buf);
    if (pixelica_save_file_write("settings.bin", SETTINGS_SCHEMA, buf, n) != PIXELICA_OK) {
        return 1;
    }
    s.volume = 0;
    if (pixelica_save_file_read("settings.bin", &schema, buf, sizeof buf, &n) != PIXELICA_OK ||
        settings_unpack(schema, buf, n, &s) != 0) {
        return 1; /* 파일 없음/손상: 기본 설정 사용 */
    }
    printf("volume=%u best=%lu\n", (unsigned)s.volume, (unsigned long)s.best_score);
    return 0;
}
```

임베디드(NVS/SD)에서는 `pixelica_save_encode`로 만든 바이트를 그대로 저장하고, 읽은
바이트를 `pixelica_save_decode`에 넘기면 됩니다.

## 파일 헬퍼 동작과 한계

- 쓰기: `"<path>.tmp"`(같은 디렉터리)에 스트리밍 기록 → 모든 `fwrite`/`fflush`/`fclose`
  확인 → Windows는 `MoveFileExA(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`,
  그 외는 `rename()`으로 교체. 기존 파일을 먼저 지우지 않으며, 실패하면 임시 파일을
  지우고 기존 파일은 그대로입니다. `<path>.tmp`가 이미 있으면 덮어쓰지 않고 실패합니다.
  이전 실행이 중단돼 임시 파일이 남았다면, 해당 파일이 불필요한지 확인한 뒤 제거하고 다시 저장하세요.
- 읽기: 파일 전체를 임시 힙 버퍼 하나에 읽어 완전히 검증한 뒤에만 대상 버퍼에 복사합니다.
  힙 사용은 이 버퍼와 쓰기 시 임시 경로 문자열뿐입니다.
- 파일 크기 상한 `PIXELICA_SAVE_FILE_MAX_BYTES`(기본 16 MiB, `save.c` 컴파일 시 재정의
  가능). 넘으면 쓰기/읽기 모두 `CAPACITY`.
- 경로는 narrow(`char`) 문자열이라 Windows에서는 ANSI 코드 페이지를 따릅니다(비ASCII
  경로 주의).
- 경로당 writer는 하나만(동시 쓰기·스레드 안전 없음).
- 정전/크래시 시 내구성(fsync 등)은 보장하지 않습니다. 교체 전 파일 또는 새 파일 중
  하나가 남는 것을 기대할 뿐이며, 읽기 시 CRC로 손상을 감지합니다.
- 마이그레이션 시스템, 암호화, 클라우드 동기화는 없습니다(schema 분기는 직접 구현).
