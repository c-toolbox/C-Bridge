import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// This runs in Node.js - Don't use client-side code here (browser APIs, JSX...)

const config: Config = {
  title: 'C-Bridge Documentation',
  tagline:
    'Bridge WebRTC streams to MPEG-TS multicast and NDI at scale',

  future: {
    v4: true,
  },

  // GitHub Pages serves this site from c-toolbox.github.io/C-Bridge/ (see
  // .github/workflows/pages.yml). CI can override both via DOCS_URL /
  // DOCS_BASE_URL if the site is served from a different origin or path.
  url: process.env.DOCS_URL || 'https://c-toolbox.github.io',
  baseUrl: process.env.DOCS_BASE_URL || '/C-Bridge/',
  trailingSlash: true,

  organizationName: 'c-toolbox',
  projectName: 'C-Bridge',

  onBrokenLinks: 'throw',

  markdown: {
    // future.v4 would otherwise parse .md as plain CommonMark, disabling admonitions.
    format: 'mdx',
    hooks: {
      onBrokenMarkdownLinks: 'warn',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          // Serve the docs at the site root, so pages live at /install/ etc.
          routeBasePath: '/',
          sidebarPath: './sidebars.ts',
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  plugins: [
    [
      '@cmfcmf/docusaurus-search-local',
      {
        indexDocs: true,
        indexBlog: false,
        indexPages: false,
        language: 'en',
      },
    ],
  ],

  themeConfig: {
    colorMode: {
      respectPrefersColorScheme: true,
    },
    navbar: {
      title: 'C-Bridge',
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Documentation',
        },
        {to: '/install', label: 'Install', position: 'left'},
        {to: '/configuration/overview', label: 'Configuration', position: 'left'},
        {to: '/networking/multicast', label: 'Networking', position: 'left'},
        {to: '/build/overview', label: 'Build', position: 'left'},
        {
          href: 'https://github.com/c-toolbox/C-Bridge',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Getting started',
          items: [
            {label: 'Introduction', to: '/'},
            {label: 'Install', to: '/install'},
            {label: 'MediaMTX sources', to: '/operations/mediamtx'},
          ],
        },
        {
          title: 'Reference',
          items: [
            {label: 'Configuration', to: '/configuration/overview'},
            {label: 'Sinks', to: '/configuration/sinks'},
            {label: 'Multicast networking', to: '/networking/multicast'},
          ],
        },
        {
          title: 'More',
          items: [
            {label: 'Performance', to: '/operations/performance'},
            {label: 'Troubleshooting', to: '/operations/troubleshooting'},
            {
              label: 'GitHub',
              href: 'https://github.com/c-toolbox/C-Bridge',
            },
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} C-Toolbox.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['bash', 'json', 'cmake', 'powershell', 'yaml'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
