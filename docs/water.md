# 선택 모듈: 물결과 부력 (Water)

바다 표면을 **해석적 높이장(heightfield)** 으로 계산하고, 그 표면에 뜬 강체 하나에
작용하는 **힘과 토크**를 돌려주는 선택 모듈입니다. 기본값은 OFF이며, 켜지 않은 빌드·설치·
패키지에는 이 모듈의 헤더도 라이브러리도 심볼도 들어가지 않습니다.

| CMake 타깃 | 내용 | 의존 |
|---|---|---|
| `Pixelica::water` | 표면 샘플링 + 부력/항력 하중 + 단일 강체 적분기 | `Pixelica::common` 만 |
| `Pixelica::water3d` | 표면을 삼각형 격자로 만들어 그리는 어댑터 | `Pixelica::water` + `Pixelica::core` |

핵심은 **렌더링과 물리가 같은 `pixelica_water_sample()` 을 같은 시각으로 호출한다**는 점입니다.
그려지는 물과 뗏목을 밀어 올리는 물이 구조적으로 동일합니다.

```sh
cmake -S . -B build -DPIXELICA_PLATFORM=HEADLESS -DPIXELICA_BUILD_WATER=ON
cmake --build build --target hello_water
./build/hello_water --frames 420 --out-dir output/water --scale 2
```

## 1. 이 모듈이 아닌 것

과장하지 않기 위해 먼저 적습니다. **유체 솔버가 아닙니다.** 압력 투영, 자유표면 추적,
파도 부서짐, 물보라가 없습니다. 높이장은 `(x, z)` 에 대한 단일값 함수이므로 **말리거나
겹칠 수 없습니다**(오버행 불가). 파동의 **궤도 속도장도 없습니다**(5절).

`pixelica_water_load()` 는 **적분하지 않고** 하중만 보고합니다.
`pixelica_water_body_step()` 은 명시적 단일 강체 적분기이며, 접촉·구속·적층·충돌 응답이
없고 **무조건 안정이 아닙니다**(6절).

## 2. 표면

```c
typedef struct PIXELICA_WaterWave {
    float dir_x, dir_z; /* 월드 XZ 진행 방향(내부에서 정규화, 0 불가) */
    float amplitude;    /* m */
    float wavelength;   /* m */
    float speed;        /* 위상 속도 m/s, 음수면 반대로 진행 */
    float phase;        /* t = 0에서의 위상 rad */
} PIXELICA_WaterWave;

typedef struct PIXELICA_WaterSurface {
    PIXELICA_WaterWave waves[PIXELICA_WATER_MAX_WAVES]; /* 최대 8개 */
    uint32_t wave_count;   /* 0이면 평면 */
    float base_height;     /* 정수면 월드 Y */
    float current_x, current_z; /* 균일 수평 유속 m/s */
} PIXELICA_WaterSurface;
```

파동 하나의 기여는 다음과 같습니다.

```
k     = 2*pi / wavelength
omega = k * speed
theta = k * (dhat . (x, z)) - omega * t + phase
h    += amplitude * sin(theta)
```

`- omega * t` 의 **부호가 진행 방향을 결정합니다**. 이 부호 덕분에 다음 **진행파 항등식**이
성립하며, `tests/test_water.c` 가 이를 검증합니다.

```
h(p + dhat * speed * dt,  t + dt)  ==  h(p, t)
```

### 2.1 시간은 `double` 입니다

표면·하중·격자 생성 진입점은 모두 `double time_seconds` 를 받고, 내부에서 `float` 으로
좁히지 않습니다. `float` 시계는 긴 세션에서 죽습니다. `t = 1e6` 에서는 `t + 1/60` 을
표현조차 못 해 `(float)(1.0e6f + 1.0f/60.0f) == 1.0e6f` 이고, 바다가 그대로 멈춥니다.

내부에서는 각 파동의 위상을 `fmod` 로 `2*pi` 로 **환산한 뒤** `sin`/`cos` 을 호출하므로
정확도가 시간과 함께 무너지지 않습니다. 남는 오차는 `omega * t` 의 반올림
(약 `|omega * t| * 2^-53`)뿐이며, `omega = 1.6`, `t = 1e9` 에서 약 `2e-7` rad 입니다.
**시간 상한은 없습니다**(유한한 `double` 이면 됩니다). 질의 위치만
`|x|, |z| <= PIXELICA_WATER_MAX_COORD`(1e6 m)로 제한됩니다.

## 3. 샘플 하나가 알려주는 것

```c
PIXELICA_WaterSample s;
pixelica_water_sample(&surface, x, z, time_seconds, &s);
```

| 필드 | 의미 |
|---|---|
| `height` | 그 XZ의 표면 월드 Y (m) |
| `normal` | 해석적 단위 법선. `normalize(-slope_x, 1, -slope_z)`, 항상 `normal.y > 0` |
| `slope_x`, `slope_z` | `dh/dx`, `dh/dz` (무차원) |
| `eulerian_dhdt` | **고정된 (x, z)** 에서의 `∂h/∂t` (m/s) |
| `surface_velocity_y` | `Dh/Dt = ∂h/∂t + current·∇h` (m/s) |
| `velocity` | `(current_x, surface_velocity_y, current_z)` — 항력이 쓰는 물 속도 |

법선·도함수는 유한차분과 일치하도록 검증되어 있습니다(`test_water.c`).

## 4. 부력

**샘플점 배수(displacement) 모델**입니다. 프로브는 선체의 한 조각을 대신하는 작은 수평판입니다.

```c
typedef struct PIXELICA_WaterProbe {
    PIXELICA_Vec3 offset;  /* 질량중심 기준 보디 좌표 (m) */
    float area;            /* 이 프로브가 대표하는 수평 투영 면적 (m^2) */
    float max_depth;       /* 배수 체적이 포화되는 깊이 (m) */
    float drag_normal;      /* 선형 감쇠, 젖은 면적 m^2당 (m/s)당 N = kg*m^-2*s^-1 */
    float drag_tangential;
} PIXELICA_WaterProbe;
```

프로브마다:

```
r      = R * offset                          (보디 -> 월드)
p      = position + r
depth  = (p.x, p.z)의 표면 높이 - p.y        > 0이면 잠김
f      = min(depth, max_depth) / max_depth   잠김 비율 0..1
V      = area * max_depth * f                배수 체적 m^3
F_b    = (0, density * gravity * V, 0)       아르키메데스, 엄격히 수직
v_p    = velocity + omega x r                프로브 "점"의 속도
v_rel  = v_p - sample.velocity               물 기준 상대 속도
F_d    = -(area * f) * (drag_normal * v_rel.y * yhat
                        + drag_tangential * (v_rel.x, 0, v_rel.z))
torque += r x (F_b + F_d)
```

마지막에 `force += (0, -mass * gravity, 0)`.

의도적인 선택 세 가지:

- **부력은 +Y 고정**입니다. 수평 투영 면적에 작용하는 정수압은 수직이므로, 법선 방향으로
  기울이면 틀립니다. 그래도 물체는 기웁니다 — 네 프로브의 깊이가 달라 `r x F` 가 토크를
  만들기 때문입니다. **기울기 휴리스틱이 아니라 그게 실제 메커니즘입니다.**
- `max_depth` 는 **프로브별 체적 상한**입니다. 깊이 밀어넣어도 `area * max_depth` 이상은
  배수하지 않으므로 하중이 발산하지 않습니다.
- `omega x r` 이 프로브 속도에 포함되므로 회전도 같은 계수로 감쇠합니다.

### 4.1 평형 흘수 (draft)

```c
pixelica_water_equilibrium_draft(&rig, mass, &draft);
```

**정의**: 평면 수면에서 **똑바로 선**(orientation = identity) 물체의 **질량중심이 정수면
아래로 내려간 깊이**. 질량중심을 `base_height - draft` 에 놓으면 균형 상태로 시작합니다.

```
V(d) = sum_i area_i * clamp(d - offset_i.y, 0, max_depth_i)
V(draft) = mass / density
```

`V` 는 연속·구간선형·비감소이므로 **정확히** 풉니다. 중단점(`offset_i.y` 와
`offset_i.y + max_depth_i`)을 정렬하고 해가 걸친 구간의 직선에서 읽습니다. 따라서
**프로브별 포화와 보디 좌표 Y 오프셋이 모두 반영됩니다**.

> `draft` 는 **음수일 수 있고 보통 음수입니다.** 선체 바닥이 아니라 질량중심의 깊이이기
> 때문입니다. 프로브가 `offset.y = -0.175` 에 있고 0.14 m 잠겨 뜨는 널판은 질량중심이
> 수면 **위** 0.035 m 에 있어 `draft = -0.035` 입니다. 프로브의 잠김 깊이는 항상
> `draft - offset_i.y` 입니다.

`PIXELICA_ERR_CAPACITY` 는 **정말 못 뜰 때만** 반환합니다. 즉
`mass / density > sum_i area_i * max_depth_i` 일 때뿐입니다. 포화 깊이가 서로 다른
프로브가 섞여 있어도 총량이 충분하면 뜹니다. 예: `(area 1, max_depth 1)` 과
`(area 1, max_depth 3)`, `density 1000` 은 2500 kg 을 **흘수 1.5 m** 로 띄우고,
4000 kg 을 넘을 때만 `PIXELICA_ERR_CAPACITY` 입니다.

## 5. 물 속도의 범위 (중요)

같은 이름으로 다른 값을 돌려주지 않기 위해 **수직 속도를 두 개** 노출합니다.

- `eulerian_dhdt` 는 순수한 오일러 장량입니다. **고정된 월드 좌표**에서 수면이 올라가는
  속도이며, 어떤 물 입자의 속도도 아니고 유속을 무시합니다.
- `surface_velocity_y = Dh/Dt = ∂h/∂t + current·∇h` 는 **물질 표면점**의 수직 속도입니다.
  수평 속도 `(current_x, current_z)` 와 운동학적으로 **정합하는** 짝이며, 항력이 기준으로
  삼는 값입니다. `velocity` 가 이 값을 담습니다.

**솔직한 한계**: 이것은 *균일 유속에 실려 가는 높이장*에 대한 정합적 운동학 근사이고,
일반적인 유체 속도장이 **아닙니다**. 특히 **파동 궤도 운동이 없습니다.** 파도가 지나가면
물체는 올라갔다 내려가지만(옳음), 파봉 아래에서 앞뒤로 밀리지는 않습니다(실제 파도는 밀립니다).
`current` 가 0이면(기본값) 수평 성분은 0이고 `surface_velocity_y` 는 `eulerian_dhdt` 와
정확히 같아집니다.

## 6. 적분기와 안정성 (과장 없음)

```c
pixelica_water_body_step(&body, &load, dt);  /* 반암시적(심플렉틱) 오일러 1스텝 */
```

보디 좌표에서 대각 관성으로 각운동을 적분하며 오일러 자이로스코픽 항 `omega x (I omega)`
까지 포함합니다. 자세는 쿼터니언으로 적분·정규화합니다.

**항력 "힘"은 소산적입니다**: 물 기준에서 `F_d · v_rel <= 0` 이 항상 성립하므로 순간
기계적 일률이 양수가 될 수 없습니다(`test_water.c` 가 검증). 이것은 **힘의 성질**이며
**적분기의 안정성 보장이 아닙니다.**

명시적 적분기는 **조건부 안정**이고, 그 조건은 호출자의 숫자에 달려 있습니다. 질량 `m`,
총 면적 `A`, 수직 항력 계수 `c_n` 의 히브(heave)는 감쇠 진동자이고

```
omega_n = sqrt(rho * g * A / m)
dt < 2 / omega_n        그리고        dt < 2 * m / (c_n * A_wet)
```

가 대략의 조건입니다. 이 헤더가 허용하는 파라미터 범위는 60 Hz에서 두 조건을 **깨뜨릴
수 있을 만큼 넓습니다.** 범위를 밀어붙이는 호출자는 `dt` 를 줄이거나 서브스텝을 써야
합니다. "안전한" 값으로 클램프하지 않는 이유는, 어떤 리그에는 거짓말이고 다른 리그에는
족쇄이기 때문입니다.

보장하는 것은 다른 것입니다: **못 밟는 스텝은 보고되고, 조용히 NaN 이 되지 않습니다.**
새 상태는 지역 복사본에 조립해 전부 유한하고 범위 안일 때만 커밋하므로, 발산하는 스텝은
`PIXELICA_ERR_INVALID_ARG` 를 반환하고 `*body` 는 **비트 단위로 그대로**입니다.

## 7. 3D 어댑터

```c
PIXELICA_WaterPatch patch = { .size_x = 24, .size_z = 24,
                              .cells_x = 48, .cells_z = 48,
                              .color = PIXELICA_RGB565(46, 118, 168) };
PIXELICA_WaterMesh mesh = { g_vertices, g_normals, g_indices, VERTS, INDICES, 0, 0 };

pixelica_water_mesh_build(&surface, &patch, t, &mesh);   /* 같은 t */
pixelica_water_draw(ctx, &mesh, &patch, &lighting, NULL);
```

버퍼는 전부 **호출자 소유**이고 용량을 선언하며, 넘칠 일은 `PIXELICA_ERR_CAPACITY` 로
거부합니다(힙 할당·전역 상태 없음). 격자는 셀당 삼각형 2개로, **위(+Y)에서 봤을 때
반시계** 방향 — 엔진의 다른 메시와 같은 규약이라 기본 `PIXELICA_CULL_BACK` 에서 위에서
보입니다. 아래에서도 봐야 하면 `PIXELICA_CULL_NONE` 을 쓰세요.

`pixelica_water_mesh_height()` 는 **그려진** 표면의 높이입니다. 셀을 찾고, 그 셀의 두
삼각형 중 점이 속한 쪽의 평면에서 보간하므로 꼭짓점·셀 경계·대각선에서 모두 정확합니다.
해석적 표면과의 차이는 **격자 이산화 오차**이며, 이것이 렌더/물리 간 실제 간극입니다.
추측하지 말고 이 함수로 측정하세요 — `hello_water` 가 뗏목 네 모서리에서 매 프레임
측정하고 한계를 넘으면 실패합니다.

## 8. 예제: 파도를 타는 뗏목

`examples/hello_water/hello_water.c` 는 창도 플랫폼도 없이 자체 컨텍스트와 **게임이
주도하는 고정 스텝**(1/60 s) 루프를 돕니다. 매 프레임 순서는
`mesh_build` → `load` → `body_step` → 물 그리기 → 뗏목 그리기입니다.

- 뗏목: 2 × 2 m, 두께 0.35 m, 800 kg. 네 모서리 프로브(각 1 m², `max_depth` 0.35 m).
- 흘수: `800 / (1000 * 4) = 0.20 m`. 프로브가 물을 벗어나기까지 0.20 m, 포화까지
  0.15 m 여유입니다. 기본 바다는 ±0.153 m 이고 뗏목을 최대 0.08 rad 기울여 모서리를
  `0.08 × 0.8 = 0.06 m` 더 들어 올리는데, 0.20 m 흘수가 둘을 모두 덮으므로
  **설득력 있게 떠 있습니다**(파도 구간 전 프레임에서 네 프로브 모두 잠김).
- 1단계(평온): 파동 0개. 평형 흘수보다 0.30 m 위에서 놓아 가라앉혀 정착시킵니다.
- 2단계(파도): 기본 두 파동 바다로 전환.

측정값(`--frames 420`, 이 저장소의 macOS arm64 Release 빌드):

```
draft=0.0250 m (probe depth 0.2000 m of 0.350 m)
calm draft_error=0.000000 m tilt=0.000000 rad depth_error=0.000000 m drop_min_submerged=0
waves roll_max=0.0737 rad pitch_max=0.0813 rad heave_max=0.0958 m
probe depth mean=0.1978 min=0.1193 max=0.2196 m (equilibrium 0.2000, saturates at 0.350)
                                                               submerged=4..4
render_physics_gap_max=0.00418 m (limit 0.020, cell 0.500 m)
ms/frame mesh=0.074 physics=0.001 draw=0.481 (48x48 grid, 4 probes)
```

비용은 48 × 48 셀(2401 정점) 격자 기준입니다. **물리는 사실상 공짜**(프로브 4개에
0.001 ms)이고, 표면 샘플링 + 격자 생성이 0.074 ms, 나머지는 래스터라이저가 256 × 192
화면을 채우는 비용입니다. 격자 해상도를 낮추면 `mesh` 와 `draw` 가 함께 줄고,
`render_physics_gap_max` 만 커집니다.

프로그램이 스스로 검사해 `ok=1` 일 때만 성공합니다: 평온 시 예측 흘수로 정착하고 수평을
유지하는지, 파도가 **두 축 모두** 기울이는지, 전 구간 네 프로브가 물에 잠겨 있는지,
모서리마다 그려진 표면과 샘플한 표면이 일치하는지, 세 캡처가 서로 다른 프레임인지,
물리만 돌린 재생이 **동일한 상태**에 도달하는지.

> 더 가벼운 선체는 골에서 **물을 벗어납니다.** 같은 선체가 560 kg이면(흘수 0.14 m)
> 가장 가파른 골에서 모서리 하나가 들리고, 240 kg이면(흘수 0.06 m) 통째로 떠오릅니다.
> 결함이 아니라 가벼운 선체의 실제 거동이고, 모듈은 `submerged_probes` 로 그대로
> 보고합니다. 이 예제는 그 상황을 보여주는 게 목적이 아니라서 그렇게 잡지 않았습니다.

`--out-dir DIR` 를 주면 프레임버퍼를 그대로 **바이너리 PPM**(P6, 256×192)으로 세 장
씁니다: `calm.ppm`(평온 정착), `waves_early.ppm`, `waves_late.ppm`(시간이 떨어진 두 장).
OS 스크린샷이 아니라 렌더 결과입니다. `--scale N` 으로 확대 저장할 수 있습니다.

## 9. 설치

### 9.1 바이너리 추가 ZIP

받은 베이스 SDK와 **같은 부모 디렉터리**에 풀면 됩니다. 두 압축 모두
`Pixelica-<version>/` 루트를 쓰므로 하나의 prefix 로 합쳐지고, 이 추가분은 베이스 파일을
**하나도 덮지 않습니다**(패키저가 그런 아카이브 작성을 거부합니다).

```cmake
find_package(PixelicaWater CONFIG REQUIRED)                    # Pixelica::water
find_package(PixelicaWater CONFIG REQUIRED COMPONENTS water3d) # + Pixelica::water3d
```

`water3d` 는 같은 prefix 에 3D 코어가 있어야 합니다. 없으면 패키지는 그대로 로드되고
`water3d` 만 "not found" 로 보고합니다(전체 실패가 아닙니다).

### 9.2 소스 ZIP

설치된 베이스 SDK에 기대 빌드합니다. 플랫폼 제약이 없습니다.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Pixelica-0.1.0 \
      -DPIXELICA_WATER_BUILD_EXAMPLE=ON
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/Pixelica-0.1.0
```

### 9.3 아카이브 만들기

```sh
python tools/package_water.py --build-dir build-water-pkg --output-dir dist/water
```

Debug + Release 바이너리 ZIP, 이식 가능한 소스 ZIP, `SHA256SUMS.txt` 를 만들고
바이너리 쪽에는 파일별 SHA256이 담긴 `share/Pixelica/water-manifest.json` 을 넣습니다.
기존 아카이브를 덮지 않고, 아무것도 삭제하지 않으며, 네트워크를 쓰지 않습니다.

## 10. 한계 요약

- 유체 솔버가 아닙니다. 오버행·부서짐·물보라 없음. 파동 궤도 속도 없음(5절).
- 부력은 수직 고정, 항력은 **선형** 감쇠(이차 형상항력 아님).
- 적분기는 단일 강체 전용이고 **조건부 안정**입니다(6절). 접촉·구속·충돌 없음.
- 관성은 보디 좌표 **대각**만 지원합니다(주축 정렬 가정).
- 파동 최대 8개, 프로브 최대 32개, 격자 최대 64 × 64 셀.
- 표면이 전체로 되풀이되는지는 호출자의 산수입니다. 파동별 주기는
  `pixelica_water_wave_period()` 가 알려줍니다.
