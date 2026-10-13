# Pixellica Developer Portal

Docusaurus + MDX + React/TypeScript로 구현한 정적 문서 사이트입니다. 기존 C 엔진은 저장소 이름과 API 식별자 `Pixelica`를 유지합니다. 문서는 Development 상태이며 웹 데모는 C 엔진의 웹 포팅이 아닙니다.

## 로컬 실행

Node.js 22 LTS를 권장합니다.

```sh
cd website
npm ci
npm start
```

기본 주소는 `http://localhost:3000/Pixelica/`입니다. 프로덕션 검증:

```sh
npm run sync
npm run typecheck
npm run build
npm run serve
```

검색은 정적 빌드에서 생성한 로컬 색인을 사용하므로 `npm run build` 후 `npm run serve`로 확인합니다. 검색용 별도 서버나 API 키는 없습니다. 한국어 제목은 입력 문자열 기반 검색을 확인하고 영문 C 식별자로도 검색하세요.

## 구조와 문서 작성

- `docs/`: 신규 문서, 다섯 개 핵심 섹션
- `src/components/`: Sandpack Playground, React Flow Architecture
- `examples/pixel-grid/`: 실행 가능한 HTML/CSS/JS 데모의 단일 원본
- `scripts/sync-docs.mjs`: 루트 `docs/*.md`와 실제 C 예제를 읽어 문서에 연결
- `src/generated/`, `docs/engine/`: 생성 결과, Git 제외
- `docusaurus.config.ts`, `sidebars.ts`: 사이트 설정과 탐색 구조

엔진 기술 문서는 루트 `docs/`에서 수정합니다. `npm start`와 `npm run build` 전에 자동 동기화됩니다. 실행 중 루트 문서나 C 예제를 변경했다면 `npm run sync`를 다시 실행하세요. 기존 Markdown은 MDX가 아닌 Markdown 형식으로 가져와 C 문법과 HTML 표기 충돌을 피합니다. 원문 링크는 GitHub 소스로 연결됩니다. 현재 원문에 오래된 사양 설명이 일부 있으므로 공개 헤더를 우선 확인하세요.

신규 문서는 `docs/`에 `.md` 또는 `.mdx`로 추가하고 `sidebars.ts`에 확장자를 뺀 ID를 등록합니다. 내부 링크에는 Docusaurus `Link`의 `/docs/...` 또는 상대 문서 링크를 사용해 GitHub Pages 하위 경로를 유지합니다.

## 컴포넌트 추가와 코드 공유

`src/components/`에 TypeScript React 컴포넌트를 추가하고 MDX에서 import하세요. 브라우저 전용 라이브러리는 `BrowserOnly`로 감쌉니다. Playground는 `examples/pixel-grid/`의 파일을 raw import하여 에디터와 실제 예제 코드를 공유합니다. 실제 C 예제는 `examples/hello_triangle/hello_triangle.c`를 빌드 시 읽어 표시하며 브라우저에서 실행하지 않습니다.

Sandpack은 브라우저에서 외부 CodeSandbox 서비스에 연결합니다. 인터넷 또는 해당 서비스가 차단된 환경에서는 코드 실행을 사용할 수 없습니다. 엔진 문서와 로컬 검색은 정적 파일로 제공됩니다. 다이어그램은 React Flow 노드 선택/드래그/확대와 Mermaid를 지원합니다. 애니메이션은 시스템의 reduced-motion 설정을 존중합니다.

## 버전과 코드 탭

현재는 `Development` 문서만 제공합니다. 실제 릴리스 시:

```sh
npm run sync
npm run docs:version -- 0.1.0
```

생성된 `versions.json`, `versioned_docs/`, `versioned_sidebars/`를 함께 커밋하세요. 루트 엔진 문서를 동기화한 뒤 버전을 만들어야 해당 시점 원문이 스냅샷에 포함됩니다. 코드 언어 또는 운영체제 탭은 `@theme/Tabs`, `@theme/TabItem`으로 구성하며 Getting Started에서 사용 예를 확인할 수 있습니다.

## GitHub Pages 배포

1. GitHub 저장소 Settings → Pages → Build and deployment → Source를 **GitHub Actions**로 선택합니다.
2. 사이트 파일과 `website/package-lock.json`, `.github/workflows/docs-pages.yml`을 함께 커밋하여 `main`에 push합니다.
3. Actions의 Documentation Pages 워크플로가 타입 검사와 정적 빌드를 수행한 뒤 배포합니다. PR에서는 빌드만 실행합니다.

예상 주소: https://dotaein.github.io/Pixelica/

`GITHUB_REPOSITORY`에서 소유자와 저장소 이름을 자동으로 읽습니다. 로컬 기본값은 `DoTaeIn/Pixelica`입니다. 사용자 도메인이나 루트 사이트는 `DOCS_BASE_URL=/`와 config의 `url`을 함께 설정하세요. Pages를 실제로 활성화하고 push해야 공개됩니다. 기존 엔진 CI와 별도 워크플로이며 별도 백엔드는 없습니다.

공식 참고: [Docusaurus 배포](https://docusaurus.io/docs/deployment), [Sandpack 사용법](https://sandpack.codesandbox.io/docs/getting-started/usage).
