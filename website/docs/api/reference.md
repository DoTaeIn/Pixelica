---
title: API Reference
---

# 공개 C API 탐색

이 페이지는 실제 공개 헤더로 연결하는 탐색용 색인입니다. REST API 또는 JavaScript SDK를 제공한다는 의미가 아닙니다.

| 기능 | 기준 소스 |
| --- | --- |
| 컨텍스트와 프레임 | [pixelica.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/pixelica.h) |
| 메시와 재질 | [renderer.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/renderer.h) |
| 플랫폼 계약 | [platform.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/platform.h) |
| 수명주기 | [app.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/app.h) |
| 저장소 | [storage.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/storage.h) |

## 정확한 사양 확인

현재 체크아웃의 `include/pixelica/` 및 선택 모듈의 `include/pixelica/`를 우선하세요. 아직 main에 반영되지 않은 로컬 변경은 GitHub 링크에 나타나지 않을 수 있습니다.

기존 API 원문은 왼쪽 **엔진 기술 문서 · 원문**에 수록되어 있습니다. 원문의 일부 지원 범위 설명은 최신 README와 다를 수 있습니다.

## 최근 추가된 API

| 기능 | 공개 헤더 | 사용 가이드 |
| --- | --- | --- |
| 클립·전환·이벤트 | [anim_clip.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/anim_clip.h) | [애니메이션 클립](../engine/anim_clips.md) |
| 노멀 조명 | [renderer.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/renderer.h) | [조명](../engine/lighting.md) |
| 화면 가산 합성 | [draw_options.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/draw_options.h) | [블렌딩](../engine/blending.md) |
| 빛 패치·접촉 그림자·블룸 | [light.h](https://github.com/DoTaeIn/Pixelica/blob/main/include/pixelica/light.h) | [픽셀 조명 도구](../engine/pixel_light.md) |
| 수면·부력 | [water.h](https://github.com/DoTaeIn/Pixelica/blob/main/modules/water/include/pixelica/water.h) | [물 모듈](../engine/water.md) |
| 수면 3D 어댑터 | [water3d.h](https://github.com/DoTaeIn/Pixelica/blob/main/modules/water/include/pixelica/water3d.h) | [애드온 설치](../guides/water.mdx) |

수명주기와 저장소는 [게임 기능 가이드](../guides/game-features.mdx)에서 시작하세요. 선택 모듈 헤더는 해당 옵션을 켠 SDK·애드온에만 설치됩니다.
