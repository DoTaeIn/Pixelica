import {readdir, readFile, mkdir, writeFile, rm} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
const root = fileURLToPath(new URL('../../', import.meta.url));
const target = new URL('../docs/engine/', import.meta.url);
await rm(target, {recursive:true, force:true}); await mkdir(target, {recursive:true});
for (const name of (await readdir(root + 'docs')).filter(n => n.endsWith('.md'))) {
 let content = await readFile(root + 'docs/' + name, 'utf8');
 content = content.replace(/\]\((?!https?:|#|mailto:)([^)]+)\)/g, (_, path) => `](https://github.com/DoTaeIn/Pixelica/blob/main/${path.startsWith('docs/') ? path : 'docs/' + path})`);
 await writeFile(new URL(name, target), `---
mdx:
  format: md
---

> 저장소 원문을 빌드할 때 동기화한 문서입니다. 일부 내용은 최신 구현과 다를 수 있으므로 공개 헤더를 우선 확인하세요.

${content}`);
}
await mkdir(new URL('../src/generated/', import.meta.url), {recursive:true});
await writeFile(new URL('../src/generated/native.ts', import.meta.url), 'export default ' + JSON.stringify(await readFile(root + 'examples/hello_triangle/hello_triangle.c', 'utf8')) + ';\n');
