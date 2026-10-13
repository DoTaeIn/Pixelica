import React, {useState} from 'react';
import BrowserOnly from '@docusaurus/BrowserOnly';
import {ReactFlow, Background, Controls, type Node, type Edge} from '@xyflow/react';
import '@xyflow/react/dist/style.css';
const descriptions: Record<string, string> = {game: 'examples/와 sdk/template/에 게임 코드가 있습니다. 게임이 렌더러와 선택 모듈을 조합합니다.', common: 'Pixelica::common: 수학, 입력, 애니메이션, 저장과 게임 수명주기를 공유합니다.', renderer: '3D 소프트웨어 래스터라이저 및 별도 2D 렌더러가 RGB565 픽셀을 생성합니다.', platform: '플랫폼 계층이 화면 출력, 시간, 입력을 제공합니다. HEADLESS와 SDL3 구현을 확인할 수 있습니다.'};
const nodes: Node[] = [{id:'game', position:{x:180,y:0}, data:{label:'Game / SDK'}}, {id:'common', position:{x:0,y:120}, data:{label:'Common modules'}}, {id:'renderer',position:{x:350,y:120},data:{label:'CPU renderer'}}, {id:'platform',position:{x:180,y:250},data:{label:'Platform / RGB565'}}];
const edges: Edge[] = [{id:'a',source:'game',target:'common',animated:true},{id:'b',source:'game',target:'renderer',animated:true},{id:'c',source:'renderer',target:'platform',animated:true}];
export default function Architecture() {
 const [selected,setSelected] = useState('game');
 return <div className="architecture"><div style={{height:350}}><BrowserOnly fallback={<p>구조도 로딩 중…</p>}>{() => <ReactFlow defaultNodes={nodes} defaultEdges={edges} fitView minZoom={0.4} onNodeClick={(_,node)=>setSelected(node.id)}><Background /><Controls showInteractive={false}/></ReactFlow>}</BrowserOnly></div><p role="status">{descriptions[selected]}</p></div>;
}
