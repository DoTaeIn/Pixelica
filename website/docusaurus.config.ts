import type {Config} from '@docusaurus/types';
import {themes} from 'prism-react-renderer';
const repository = process.env.GITHUB_REPOSITORY || 'DoTaeIn/Pixelica';
const [owner, repo] = repository.split('/');
const config: Config = {
  title: 'Pixellica', tagline: 'Small pixels. Endless possibilities.',
  favicon: 'img/logo.svg', url: `https://${owner}.github.io`, baseUrl: process.env.DOCS_BASE_URL || `/${repo}/`,
  organizationName: owner, projectName: repo, trailingSlash: true,
  onBrokenLinks: 'throw', i18n: {defaultLocale: 'ko', locales: ['ko']},
  markdown: {mermaid: true}, themes: ['@docusaurus/theme-mermaid', ['@easyops-cn/docusaurus-search-local', {hashed: 'filename', language: ['en'], indexBlog: false, docsRouteBasePath: 'docs'}]],
  presets: [['classic', {docs: {sidebarPath: './sidebars.ts', editUrl: `https://github.com/${repository}/edit/main/website/`, lastVersion: 'current', versions: {current: {label: 'Development'}}}, blog: false, theme: {customCss: './src/css/custom.css'}}]],
  themeConfig: {
    image: 'img/logo.svg', colorMode: {defaultMode: 'dark', respectPrefersColorScheme: true},
    navbar: {title: 'pixellica', logo: {alt: '', src: 'img/logo.svg'}, items: [{to: '/docs/intro', label: '문서', position: 'left'}, {to: '/docs/examples/playground', label: 'Playground', position: 'left'}, {type: 'docsVersionDropdown', position: 'right'}, {href: `https://github.com/${repository}`, label: 'GitHub ↗', position: 'right'}]},
    footer: {style: 'dark', copyright: 'Pixellica · Built for curious developers. C API identifiers use Pixelica.'},
    prism: {theme: themes.github, darkTheme: themes.vsDark, additionalLanguages: ['c', 'cmake', 'bash']},
  },
};
export default config;
