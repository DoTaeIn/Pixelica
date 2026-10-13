import React from 'react';
import BrowserOnly from '@docusaurus/BrowserOnly';
import {useColorMode} from '@docusaurus/theme-common';
import html from '!!raw-loader!@site/examples/pixel-grid/index.html';
import css from '!!raw-loader!@site/examples/pixel-grid/style.css';
import js from '!!raw-loader!@site/examples/pixel-grid/index.js';
export default function Playground() {
 const {colorMode} = useColorMode();
 return <BrowserOnly fallback={<p>코드 에디터를 불러오는 중…</p>}>{() => {
 const {Sandpack} = require('@codesandbox/sandpack-react');
 return <Sandpack template="static" theme={colorMode} files={{'/index.html': html, '/style.css': css, '/index.js': js}} options={{activeFile: '/index.js', visibleFiles: ['/index.html', '/style.css', '/index.js'], showLineNumbers: true, showRefreshButton: true, editorHeight: 440, resizablePanels: true}} />;
 }}</BrowserOnly>;
}
