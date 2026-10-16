# 스켈레탈 애니메이션 클립 (`pixelica/anim_clip.h`)

본(bone)마다 변환 트랙을 가진 클립, 재생 커서, 크로스페이드, 시간 이벤트입니다.
**공통 라이브러리(`Pixelica::common`)** 에 있으므로 3D 렌더러도 2D 렌더러도 선택 스켈레톤
모듈도 필요 없습니다. 클립은 본 **인덱스**마다 `PIXELICA_TRS`를 하나 내놓을 뿐이고, 그
인덱스가 무엇을 뜻하는지는 호출자가 정합니다. 스켈레톤 모듈과 함께 쓸 때는 각 `TRS`를
`pixelica_trs_matrix()`로 로컬 행렬로 바꿔 `pixelica_skeleton_pose()`에 넘기면 됩니다
([`rigging.md`](rigging.md)).

`animation.h`와 같은 규칙입니다: 할당 없음, 시계 없음, 숨은 전역 상태 없음, 모든 배열과
플레이어는 호출자 소유, 시간은 호출자가 고르는 정수 **틱**(고정 60 Hz 스텝마다 1틱이
의도된 사용법)입니다.

## 타입

```c
typedef struct PIXELICA_TRS {        /* 클립의 출력 단위 */
    PIXELICA_Vec3 translation;
    PIXELICA_Quat rotation;          /* 쿼터니언: 짐벌락 없음, 보간이 잘 정의됨 */
    PIXELICA_Vec3 scale;
} PIXELICA_TRS;

typedef struct PIXELICA_ClipBoneTrack {
    const PIXELICA_ClipVec3Key *translation; size_t translation_count;
    const PIXELICA_ClipQuatKey *rotation;    size_t rotation_count;
    const PIXELICA_ClipVec3Key *scale;       size_t scale_count;
} PIXELICA_ClipBoneTrack;

typedef struct PIXELICA_AnimClip {
    const PIXELICA_ClipBoneTrack *tracks;  /* bone_count개 */
    uint32_t bone_count;                   /* 1..65536 */
    uint32_t duration_ticks;               /* >= 1 */
    const PIXELICA_ClipEvent *events;      /* tick 비감소 정렬 */
    size_t event_count;                    /* 0..256 */
} PIXELICA_AnimClip;
```

`PIXELICA_Transform`(animation.h)은 오일러 각 + 피벗을 가진 **파트** 변환이고,
`PIXELICA_TRS`는 쿼터니언을 가진 **본** 변환입니다. 바인드 포즈가 이미 본의 원점을
놓아 주므로 피벗이 없습니다.

## 쿼터니언 정책 (`pixelica/math.h`)

`PIXELICA_Quat`은 `(x, y, z, w)`이고 **w가 스칼라**입니다. 손 방향과 부호는
`pixelica_mat4_rotation_x/y/z`와 같습니다(+축에서 원점을 볼 때 반시계). 단위 축 `a`를
중심으로 `angle`만큼 도는 회전은 `(sin(angle/2) * a, cos(angle/2))`입니다.

- **방향만 의미가 있습니다.** 입력은 단위 길이일 필요가 없고 내부에서 정규화합니다.
  길이 0이거나 비유한이면 `PIXELICA_ERR_INVALID_ARG`입니다.
- **q와 -q는 같은 회전**입니다. 그래서 모든 보간은 **최단 호(shortest arc)** 를 택합니다:
  `pixelica_quat_slerp`는 내적이 음수면 두 번째 입력을 부호 반전하므로 절대로 먼 쪽으로
  돌지 않습니다.
- 출력은 항상 단위 길이입니다(부동소수 반올림 범위 내).
- 거의 평행한 입력은 정규화 선형 보간으로 떨어집니다(그 영역에서는 float 반올림보다
  정확).

제공 함수: `pixelica_quat_identity`, `_normalize`, `_from_axis_angle`, `_slerp`,
`_to_mat4`.

## 트랙 샘플링

빈 트랙(`NULL` + count 0)은 그 성분의 **항등**으로 샘플됩니다(이동 0, 회전 항등, 스케일 1).
바인드 포즈가 아닙니다 — 클립은 바인드 포즈를 모릅니다. 클립 전체에서 항등이 아닌 값을
유지하려면 상수 키 하나를 넣으세요.

비어 있지 않은 트랙은 `pixelica_anim_sample`과 **같은 규칙**입니다(같은 이징·보간 코드를
공유합니다): 첫 키 이전은 첫 값, 마지막 키 이후는 마지막 값, 키 위는 그 키의 값(STEP
포함), 그 사이는 왼쪽 키의 `ease`로 `t = (T - left.tick) / (right.tick - left.tick)`
(double)에서 보간. 벡터 키는 세 성분이 **같은 이징 파라미터**로 함께 움직이고, 회전 키는
slerp 됩니다.

보간은 항상 **양 끝 키 값 사이로 클램프**되므로 어떤 이징도 오버슛할 수 없고,
`±FLT_MAX` 키조차 유한하고 범위 안에 머뭅니다.

## 재생

```c
PIXELICA_ClipPlayer p = {0};                       /* 0 초기화 계약 (아래) */
pixelica_clip_player_init(&p, &clip, /*loop=*/true);
...
pixelica_clip_player_advance(&p, 1u, on_event, userdata);
pixelica_clip_player_sample(&p, pose, bone_count, scratch, bone_count);
```

- **LOOP**: 위치는 `0 .. duration_ticks - 1`. `duration_ticks`라는 위치는 **존재하지
  않습니다** (그것은 다음 반복의 틱 0입니다). `repetition`이 올라가고 절대 끝나지
  않습니다.
- **ONCE**: 위치는 `0 .. duration_ticks`이고 정확히 `duration_ticks`에서 멈추며
  `finished = true`가 됩니다.
- `paused`는 직접 토글해도 됩니다. 그 밖의 필드는 읽기 전용입니다.
- 위치는 클립 로컬 위상 + 반복 카운터로 보관하므로 아무리 오래 돌려도 넘치지 않습니다.

### 0 초기화 계약

`pixelica_clip_player_init`에 넘기는 객체는 **0으로 초기화**되어 있거나(`= {0}` 또는
`memset`) 이전에 `init`이 성공한 플레이어여야 합니다. 그래야 `init`도 이벤트 콜백
재진입을 거부할 수 있습니다(아래). 0 초기화된 저장소 자체는 아직 사용 가능한
플레이어가 아닙니다 — 반드시 `init`이 성공해야 합니다.

## 이벤트: 스윕 (previous, current] 의미론

플레이어는 한 번에 **정수 틱 위치 하나**를 차지합니다. N틱 전진은 다음 N개의 위치를
순서대로 **방문**합니다(루프면 `duration_ticks - 1` 다음에 0으로 감김, 아니면
`duration_ticks`에서 멈춤). 이벤트는 자기 틱이 방문된 위치 중 하나일 때 정확히 한 번
발생합니다. 즉 관례적인 좌개구간 `(이전, 현재]` 윈도우입니다.

- 틱 0에서 2만큼 전진하면 틱 1, 2를 방문하므로 **틱 2의 이벤트가 지금** 발생합니다.
  한 스텝 늦지 않습니다.
- 같은 틱의 여러 이벤트는 배열 순서로 발생합니다.
- 루프 클립이 `duration_ticks - 1`에서 감기면 다음 반복의 틱 0에 **착지**합니다:
  `PIXELICA_CLIP_EVENT_LOOP`가 먼저, 그다음 그 반복의 틱 0 이벤트들.
- 비루프 클립이 `duration_ticks`에 착지하면 그 틱의 이벤트들 → `PIXELICA_CLIP_EVENT_END`
  순서로, 각각 **한 번만**. 재초기화 전까지 다시 발생하지 않습니다.

**시작 포즈.** 갓 초기화된 플레이어는 틱 0을 "방문"한 적 없이 그 위에 있습니다. 그래서
틱 0 이벤트는 플레이어를 **실제로 움직인 첫 전진**에서 한 번 보고되고(그 스텝 자신의
이벤트들과 함께 틱 순서로), 이후 `started`가 참이 되어 루프 감김에서만 다시
발생합니다. 0틱 전진이나 일시정지 중의 전진은 이것을 소비하지 않습니다.

**분할 등가성(partition equivalence).** 전진을 쪼개도 결과가 같습니다. 10틱 루프 클립을
100틱 한 번에 전진시키면 1틱씩 100번 전진시킨 것과 **정확히 같은 이벤트가 같은 순서로**
(LOOP 마커 10개 포함) 보고됩니다. `tests/test_anim_clip.c`가 스텝 크기 1~5로 이를
고정합니다.

**루프 클립의 틱 `duration_ticks`.** 그 위치는 존재하지 않으므로 거기 둔 사용자 이벤트는
절대 발생하지 않습니다. 틱 0에 두거나 `PIXELICA_CLIP_EVENT_LOOP`를 들으세요. 비루프
클립은 `duration_ticks`에서 발생합니다.

**큰 dt.** 한 번의 전진이 `PIXELICA_CLIP_MAX_EVENT_REPETITIONS`(16)개를 넘는 반복을
가로지르면 **가장 오래된** 반복들의 이벤트가 버려지고 최근 것만 보고되며, 버린 개수가
`player.skipped_repetitions`에 누적됩니다. 최종 재생 위치는 언제나 정확합니다. 콜백이
`NULL`이면 아무것도 보고하지 않고 아무것도 "버렸다"고 세지 않습니다.

### 콜백 수명과 재진입 정책

- `pixelica_clip_player_advance`에서 **동기적으로**, 그 호출이 반환하기 전에만
  불립니다.
- `info`와 그것이 가리키는 모든 것은 호출 동안만 빌려 줍니다. 보관하려면 복사하세요.
- 콜백이 돌 때 플레이어는 **이미 그 전진의 최종 위치**로 옮겨져 있습니다. 한 전진의 모든
  콜백이 같은 일관된 상태를 봅니다. 그 순간의 정보는 `info`에 있습니다. 콜백 안에서
  플레이어를 샘플링해도 되며 그 최종 포즈가 나옵니다.
- 콜백은 플레이어를 다시 구동하면 **안 됩니다**. `_init`, `_crossfade`, `_advance`
  **셋 다** 같은 플레이어에 대해 `PIXELICA_ERR_INVALID_ARG`를 돌려주고 아무것도 바꾸지
  않습니다. 다른 플레이어·다른 클립·읽기 전용 호출(`_sample`, `_weight`)은 자유입니다.
- 한 전진의 콜백들은 시간 순서로 실행됩니다.

## 크로스페이드

```c
pixelica_clip_player_crossfade(&p, &run_clip, /*loop=*/true,
                               /*fade_ticks=*/30u, PIXELICA_EASE_SMOOTHSTEP);
```

- 가중치는 `w = ease(fade_elapsed / fade_ticks)`. 호출 순간 `w == 0`이므로 보이는 포즈가
  튀지 않습니다.
- 페이드 중 **두 클립 모두** 자기 loop 플래그대로 계속 전진합니다. 어느 쪽도 얼지
  않지만, **들어오는 클립만** 이벤트를 보고합니다.
- 페이드는 **경과 틱만으로** 굴러갑니다. 그래서 비루프 들어오는 클립이 이미
  `finished`가 되어도 페이드는 끝까지 갑니다: 그 클립의 포즈는 마지막 틱에 고정된 채
  가중치만 1까지 올라가고, `END`는 여전히 한 번만 발생합니다.
- `fade_ticks == 0`은 즉시 전환이며 `init`과 같습니다(카운터도 리셋).
- 들어오는 클립의 `bone_count`는 현재 클립과 같아야 합니다.

### 페이드 중단 정책 (실제 불연속)

이미 페이드가 진행 중일 때 새 크로스페이드를 시작하면, **더 오래된 나가는 클립이
버려지고**, 그때 들어오고 있던 클립이 현재 위치 그대로 새 "나가는" 포즈가 됩니다.
페이드는 절대 체인되지 않으므로 비용과 메모리가 일정하게 유지됩니다. 그 대신 버려진
클립이 아직 보이는 포즈의 `(1 - w)`만큼을 차지하고 있었으므로 **그 순간 포즈가
점프합니다**.

이것은 보장이 아니라 명시된 한계입니다. 페이드가 없을 때 시작한 크로스페이드만
점프가 없습니다. 피하려면 페이드를 짧게 하거나 `fade_from`이 `NULL`이 될 때까지
기다렸다가 다음 페이드를 시작하세요. `tests/test_anim_clip.c`의
`test_crossfade_interrupted_policy`가 이 동작을 고정합니다.

## 샘플링과 앨리어싱

```c
PIXELICA_Status pixelica_clip_player_sample(const PIXELICA_ClipPlayer *player,
                                            PIXELICA_TRS *out, size_t out_count,
                                            PIXELICA_TRS *scratch, size_t scratch_count);
```

`scratch`는 나가는 포즈를 섞는 동안 담아 둡니다. 페이드가 **없으면** `NULL` + 0이어도
되지만, 페이드 중이면 `bone_count`개 이상이어야 하고 아니면 `PIXELICA_ERR_NO_MEMORY`
입니다. 호출 뒤 내용은 불특정입니다.

`out`과 쓰이는 `scratch` 구간은 서로, 그리고 **양쪽 클립의 객체·트랙 배열·이벤트
배열·모든 키 배열**과 겹치면 안 됩니다. 클립을 **읽으면서** `out`을 쓰기 때문이고,
겹침은 검출되어 `PIXELICA_ERR_INVALID_ARG`가 됩니다(`pixelica_clip_sample`도 동일).

## 검증 비용과 실패 시 보존

클립은 `pixelica_clip_validate`, `pixelica_clip_sample`, `_player_init`,
`_player_crossfade`, `_player_sample`에서 **전부(O(전체 키 수))** 검증됩니다. 잘못된
데이터를 유효한 것처럼 읽는 일이 없습니다. `pixelica_clip_player_advance`는 키를 읽지
않으므로 이벤트 목록만 다시 확인합니다(O(이벤트 수)).

실패할 수 있는 모든 것(구조·유한성·순서·크기·앨리어싱)을 **먼저** 검사하고, 그 검사를
통과한 클립의 샘플링은 실패할 수 없으므로, 실패한 호출은 출력을 **바이트 단위로 그대로**
둡니다.

## 테스트

`tests/test_anim_clip.c` — TRS 헬퍼와 행렬 합성 순서, 블렌드 끝점/클램프/인플레이스,
클립 검증 전 경로, 샘플링(정확한 키·보간·클램프·STEP·모든 이징에서 `±FLT_MAX` 안전성),
ONCE/LOOP 위치와 거대 스텝, 스윕 윈도우, 루프 경계 순서, 분할 등가성, 반복 상한과
`skipped_repetitions`, 콜백 재진입 거부, 크로스페이드 가중치·완주·중단 정책, 샘플링
오류와 앨리어싱 거부.
