import React from 'react';
import Layout from '@theme/Layout';
import Link from '@docusaurus/Link';
export default function Home() {
 return <Layout title="Developer Portal" description="Pixellica C11 엔진의 문서, 구조도와 인터랙티브 예제"><main className="portal">
 <div className="hero-grid" aria-hidden="true">{Array.from({length:81},(_,i)=><i key={i} style={{opacity:0.15+((i*7)%9)/12, animationDelay:`${i*35}ms`}}/>)}</div>
 <div className="hero-copy"><div className="eyebrow"><span className="status-dot"/> DEVELOPER PORTAL · DEVELOPMENT</div><h1>Small pixels.<br/><em>Endless possibilities.</em></h1><p>작은 픽셀로 만드는 커다란 세계.<br/>C11 소프트웨어 렌더링 엔진을 이해하고, 만들고, 실험하세요.</p><div className="hero-actions"><Link className="button button--primary button--lg" to="/docs/intro">문서 시작하기 <span>→</span></Link><Link className="button button--secondary button--lg" to="/docs/examples/playground">Playground ↗</Link></div><div className="hero-meta">PORTABLE C11 <b> / </b> CPU RENDERING <b> / </b> RGB565</div></div>
 <div className="portal-bottom"><div className="section-label">YOUR NEXT BUILD STARTS HERE <span>01 — 04</span></div><div className="doc-cards">
 <Link to="/docs/getting-started/install"><span>01 / GETTING STARTED</span><h2>첫 프레임까지, 한 걸음 ↗</h2><p>로컬 환경을 준비하고 엔진을 빌드하세요.</p></Link>
 <Link to="/docs/architecture/overview"><span>02 / ARCHITECTURE</span><h2>픽셀 뒤의 구조 ↗</h2><p>인터랙티브 다이어그램으로 내부를 탐색하세요.</p></Link>
 <Link to="/docs/examples/playground"><span>03 / EXAMPLES</span><h2>바꾸고, 실행하고, 발견하기 ↗</h2><p>브라우저에서 코드를 직접 실험하세요.</p></Link>
 <Link to="/docs/api/reference"><span>04 / API REFERENCE</span><h2>소스에서 찾는 답 ↗</h2><p>공개 헤더와 실제 C 예제를 확인하세요.</p></Link>
 </div></div>
 </main></Layout>;
}
