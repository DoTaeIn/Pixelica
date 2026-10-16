# 저장소 (`pixelica/storage.h`)

게임 ID와 번호가 붙은 슬롯, 그리고 플랫폼 콜백 한 묶음으로 세이브를 읽고 씁니다.
[`save.md`](save.md)의 **봉투(envelope)를 그대로 재사용**하므로 magic·schema 버전·CRC-32
검사는 PC든 ESP32든 동일합니다. 힙·전역 상태·렌더러·시계·스레드가 없고 버퍼는 모두
호출자 소유이며, 하나의 `PIXELICA_Storage`는 한 스레드에서만 씁니다.

이 모듈이 하지 **않는** 일: 스키마 마이그레이션(게임이 버전을 보고 직접 분기),
암호화, 클라우드 동기화, 압축, 모르는 게임 ID 열거, 디렉터리 나열, 프로세스 간 잠금.

```
게임 payload ──► pixelica_storage_save ──► [봉투 인코딩: scratch] ──► ops.write(game_id, slot, bytes)
게임 payload ◄── pixelica_storage_load ◄── [봉투 검증: scratch] ◄──── ops.read(game_id, slot, ...)
```

## 게임 ID 규칙

`pixelica_storage_game_id_valid()`가 판정하며, 실패하면 `pixelica_storage_init`은
`INVALID_ARG`입니다. 어댑터는 **콜백마다 다시 검사**하므로 `PIXELICA_StorageOps`를
직접 호출해도 디렉터리를 벗어날 수 없습니다.

| 규칙 | 내용 |
| --- | --- |
| 길이 | 1 ~ `PIXELICA_STORAGE_GAME_ID_MAX`(32) 바이트 |
| 문자 | `a`–`z`, `0`–`9`, `-`, `_` 만 |
| 양 끝 | `-`·`_`로 시작하거나 끝날 수 없음 |
| 예약 이름 | `con` `prn` `aux` `nul` `com0`~`com9` `lpt0`~`lpt9` (Windows 장치명) |

따라서 `..`, `../../etc/passwd`, `..\..\windows`, `/abs`, `C:`, `a/b`, `.hidden`,
`Game`(대문자), 빈 문자열은 모두 거부되고 **경로 구성요소가 될 수 없습니다**.
`com`·`lpt`·`console`처럼 숫자가 없는 이름은 평범한 이름이라 허용됩니다.

## 슬롯과 scratch 버퍼

- 슬롯은 `0` ~ `PIXELICA_STORAGE_SLOT_MAX - 1`(0~15). 벗어나면 `INVALID_ARG`.
- `pixelica_storage_init`에 넘기는 scratch 버퍼 하나가 **인코딩된 봉투 전체**를 담습니다.
  필요한 크기는 `payload + PIXELICA_SAVE_OVERHEAD_BYTES`(20)이며
  `pixelica_storage_required_scratch()`로 계산하고 `pixelica_storage_max_payload()`로
  지금 버퍼가 허용하는 payload 크기를 확인합니다.
- scratch는 init부터 shutdown까지 유효해야 하고, 호출 사이의 내용은 정의되지 않습니다.
- **여유를 두세요.** 저장된 슬롯이 scratch보다 크면 검증 전에 `NO_MEMORY`가 나서
  "손상"인지 "버퍼 부족"인지 구분할 수 없습니다. 게임이 앞으로 쓸 최대 payload 기준으로
  잡는 것이 좋습니다 (예제 `examples/hello_lifecycle/main.c`의 `GAME_MAX_PAYLOAD`).

## API

```c
bool            pixelica_storage_game_id_valid(const char *game_id);
PIXELICA_Status pixelica_storage_init(PIXELICA_Storage *s, const char *game_id,
                                      const PIXELICA_StorageOps *ops,
                                      void *scratch, size_t scratch_size);
PIXELICA_Status pixelica_storage_shutdown(PIXELICA_Storage *s);
const char     *pixelica_storage_game_id(const PIXELICA_Storage *s);
PIXELICA_Status pixelica_storage_max_payload(const PIXELICA_Storage *s, size_t *out);
PIXELICA_Status pixelica_storage_required_scratch(size_t payload_size, size_t *out);
PIXELICA_Status pixelica_storage_save(PIXELICA_Storage *s, uint32_t slot,
                                      uint32_t schema_version,
                                      const void *payload, size_t payload_size);
PIXELICA_Status pixelica_storage_load(PIXELICA_Storage *s, uint32_t slot,
                                      uint32_t *out_schema_version, void *out_payload,
                                      size_t payload_capacity, size_t *out_payload_size);
PIXELICA_Status pixelica_storage_inspect(PIXELICA_Storage *s, uint32_t slot,
                                         uint32_t *out_schema_version,
                                         size_t *out_payload_size);
PIXELICA_Status pixelica_storage_erase(PIXELICA_Storage *s, uint32_t slot);
PIXELICA_Status pixelica_storage_exists(PIXELICA_Storage *s, uint32_t slot, bool *out);
PIXELICA_Status pixelica_storage_fs_init(PIXELICA_StorageFs *fs, const char *root,
                                         PIXELICA_StorageOps *out_ops);
```

- `inspect`는 복사 없이 완전 검증 후 schema 버전과 payload 크기만 돌려줍니다(슬롯 목록·버퍼 크기 결정용).
- `exists`는 "내용이 있는가"만 답합니다. 유효한지는 `inspect`로 확인하세요.
- schema 버전은 검사하지 않습니다. 받은 값으로 직접 분기·변환하세요([`save.md`](save.md)).
- 실패한 호출은 호출자 버퍼를 바꾸지 않고, 실패한 save는 (어댑터가 원자적인 만큼) 기존 슬롯을 보존합니다.

| 오류 | 상황 |
| --- | --- |
| `INVALID_ARG` | NULL, 초기화 안 됨/이미 shutdown, 잘못된 게임 ID, 슬롯 범위 초과, 필수 콜백 없음, 없는 선택 콜백 호출(`erase`) |
| `NO_MEMORY` | scratch 또는 `out_payload`가 작음 |
| `CAPACITY` | payload가 봉투/플랫폼 한도 초과 |
| `BAD_ASSET` | 저장된 바이트 손상 (magic·길이·CRC) |
| `PLATFORM` | 플랫폼 콜백이 실패를 보고함 |
| `NOT_FOUND` | 슬롯이 비어 있음 |

## 플랫폼 콜백 (`PIXELICA_StorageOps`)

플랫폼은 **바이트 덩어리 저장소**만 제공하면 됩니다. 형식은 모듈이 처리합니다.

| 콜백 | 필수 | 계약 |
| --- | --- | --- |
| `write(ud, game_id, slot, data, size)` | ✅ | `data[0..size)`를 통째로 저장. 원자적이어야 함(실패 시 기존 내용 보존) |
| `read(ud, game_id, slot, out, capacity, *out_size)` | ✅ | 비었으면 `NOT_FOUND`. 아니면 `*out_size`에 저장 크기를 넣고, 들어가면 복사 후 `OK`, 모자라면 **복사하지 않고** `NO_MEMORY` |
| `erase(ud, game_id, slot)` | — | 없으면 `pixelica_storage_erase`가 `INVALID_ARG` |
| `size(ud, game_id, slot, *out_size)` | — | 없으면 `exists`가 capacity 0짜리 `read`로 대신 확인 |

`userdata`는 빌려주는 것이며 저장소가 쓰이는 동안 유효해야 합니다. 콜백은 자신을
호출한 `PIXELICA_Storage`를 다시 호출하면 안 됩니다.

임베디드(NVS/SD) 어댑터도 이 네 개만 구현하면 봉투·CRC·버전 처리를 그대로 물려받습니다.

## PC 파일시스템 어댑터

```c
PIXELICA_StorageFs fs;
PIXELICA_StorageOps ops;
PIXELICA_Storage storage;
uint8_t scratch[32 + PIXELICA_SAVE_OVERHEAD_BYTES];

pixelica_storage_fs_init(&fs, "saves", &ops);           /* 파일시스템은 아직 건드리지 않음 */
pixelica_storage_init(&storage, "my-game", &ops, scratch, sizeof scratch);
```

- 경로는 `<root>/<게임 ID>/save<NN>.pxsv` (`NN`은 두 자리 10진수).
- 쓰기는 `pixelica_save_file_write`와 **같은 원자적 경로**(`.tmp` 기록 → 교체)를 씁니다.
  실패하면 임시 파일을 지우고 기존 세이브는 그대로입니다. 읽기는 봉투를 완전히 검증한
  뒤에만 복사합니다.
- 첫 쓰기에서 `<root>`와 `<root>/<게임 ID>` **두 단계만** 생성합니다. `<root>`의 상위
  디렉터리는 미리 있어야 하고, 디렉터리는 절대 삭제하지 않습니다.
- `fs`는 호출자 소유이며 주소가 고정되어야 하고 이를 쓰는 저장소보다 오래 살아야 합니다.
- 경로가 `PIXELICA_STORAGE_PATH_MAX`(512)에 맞지 않으면 `pixelica_storage_fs_init`이 `INVALID_ARG`.
- 경로는 narrow(`char`) 문자열이라 Windows에서는 ANSI 코드 페이지를 따릅니다.
  슬롯당 writer는 하나이며, 같은 슬롯에 대한 스레드 안전성과 정전 내구성은 보장하지 않습니다.

## 예제

실행 가능한 전체 예제는 [`lifecycle.md`](lifecycle.md)와
`examples/hello_lifecycle/main.c`에 있습니다. 체크포인트를 쓰고 프로세스를 끝낸 뒤
새 프로세스가 복원하는 것까지 CTest `lifecycle_checkpoint_restart`가 검증합니다.
