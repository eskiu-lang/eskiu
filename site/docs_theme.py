"""docs_theme.py: the docs pages' stylesheet and HTML shell (used by build-docs.py)."""

CSS = r"""
*, *::before, *::after { box-sizing: border-box; }
:root {
  --paper: #fafafa; --surface: #ffffff; --ink: #0a0a0a; --ink2: #3a3a3a; --muted: #5f5f5f; --faint: #8a8a8a;
  --rule: #e2e2e5; --rule-hover: #bdbdc2; --purple: #4b33c4; --lilac: #efecfb; --lilac-2: #f5f3fd; --lilac-3: #d6cff3;
  --on-accent: #ffffff; --code-bg: #f6f8fa; --head-bg: rgba(250, 250, 250, 0.88);
  --serif: "Inter", -apple-system, "Segoe UI", sans-serif;
  --sans: "Inter", -apple-system, "Segoe UI", sans-serif;
  --mono: "JetBrains Mono", ui-monospace, "SF Mono", Menlo, Consolas, monospace;
  --gutter: 24px;
}
@media (prefers-color-scheme: dark) {
  :root {
    --paper: #0d0d0d; --surface: #141414; --ink: #ffffff; --ink2: #d4d4d4; --muted: #9a9a9a; --faint: #7a7a7a;
    --rule: #262626; --rule-hover: #444444; --purple: #a594ff; --lilac: rgba(165, 148, 255, 0.14);
    --lilac-2: rgba(165, 148, 255, 0.08); --lilac-3: rgba(165, 148, 255, 0.35); --on-accent: #0d0d0d;
    --code-bg: #0a0a0a; --head-bg: rgba(13, 13, 13, 0.85);
  }
}
@media (min-width: 900px) { :root { --gutter: 48px; } }
html { scroll-behavior: smooth; -webkit-text-size-adjust: 100%; text-size-adjust: 100%; }
body { margin: 0; background: var(--paper); color: var(--ink2); font-family: var(--sans); font-size: 17px;
       line-height: 1.7; -webkit-font-smoothing: antialiased; }
a { color: var(--purple); text-decoration: none; }
a:hover { color: var(--ink); }
.bar { max-width: 1440px; margin: 0 auto; padding-inline: var(--gutter); }

/* Header (shared look with the homepage) */
.top { position: sticky; top: 0; z-index: 30; background: var(--head-bg);
       backdrop-filter: saturate(1.4) blur(8px); border-bottom: 1px solid var(--rule); }
.top-in { height: 72px; display: flex; align-items: center; gap: 40px; }
.brand { display: flex; align-items: center; gap: 10px; font-family: var(--serif); font-weight: 800;
         font-size: 24px; color: var(--ink); }
.brand img { width: 34px; height: 34px; }
.brand .v { font-family: var(--mono); font-weight: 400; font-size: 12px; color: var(--faint); margin-left: 2px; }
.nav { display: none; gap: 26px; font-weight: 500; font-size: 16px; }
.nav a { color: var(--ink); }
.nav a:hover, .nav a[aria-current] { color: var(--purple); }
.nav a[aria-current] { text-decoration: underline; text-underline-offset: 6px; text-decoration-thickness: 1.5px; }
.top-right { margin-left: auto; display: flex; align-items: center; gap: 14px; }
.btn-install { display: none; background: var(--ink); color: var(--paper); font-weight: 700; font-size: 15px;
               padding: 12px 20px; border-radius: 12px; }
.btn-install:hover { background: var(--purple); color: var(--on-accent); }
.menu-btn { width: 48px; height: 48px; border: 1px solid var(--rule); background: var(--surface); border-radius: 14px;
            display: flex; align-items: center; justify-content: center; padding: 0; cursor: pointer; color: var(--ink); }
.drawer { display: none; border-top: 1px solid var(--rule); padding: 8px var(--gutter) 20px; }
.drawer a { display: block; padding-block: 14px; font-size: 18px; font-weight: 500; color: var(--ink);
            border-bottom: 1px solid var(--rule); }
.top.open .drawer { display: block; }
@media (min-width: 900px) { .btn-install { display: flex; } }
@media (min-width: 1280px) {
  .nav { display: flex; }
  .menu-btn, .drawer, .top.open .drawer { display: none; }
}

/* Layout: sidebar | content | on this page */
.layout { padding-block: 32px 88px; }
.side { display: none; }
.side .grp, .side-m .grp, .toc .grp { display: block; font-family: var(--mono); font-size: 12px;
  letter-spacing: 0.06em; text-transform: uppercase; color: var(--purple); margin: 0 0 8px; }
.side nav + .grp, .side-m nav + .grp { margin-top: 28px; }
.side a, .side-m a { display: block; font-size: 15px; color: var(--ink2); padding: 7px 12px; border-radius: 10px; }
.side a:hover, .side-m a:hover { color: var(--ink); background: var(--lilac-2); }
.side a[aria-current], .side-m a[aria-current] { color: var(--ink); background: var(--lilac); font-weight: 700; }
.side-m { margin: 0 0 36px; background: var(--surface); border: 1px solid var(--rule); border-radius: 14px; }
.side-m summary { min-height: 52px; display: flex; align-items: center; justify-content: space-between; gap: 12px;
  padding: 0 18px; font-weight: 700; color: var(--ink); cursor: pointer; list-style: none; }
.side-m summary::-webkit-details-marker { display: none; }
.side-m summary::after { content: "+"; font-family: var(--mono); font-size: 20px; color: var(--purple); }
.side-m[open] summary::after { content: "–"; }
.side-m .inner { padding: 4px 8px 16px; border-top: 1px solid var(--rule); }
.side-m .grp { padding: 14px 12px 0; }
.toc { display: none; }
@media (min-width: 900px) {
  .layout { display: grid; grid-template-columns: 220px minmax(0, 1fr); gap: 56px; padding-block: 48px 104px; }
  .side { display: block; position: sticky; top: 104px; align-self: start; max-height: calc(100vh - 128px); overflow: auto; }
  .side-m { display: none; }
}
@media (min-width: 1240px) {
  .layout.has-toc { grid-template-columns: 220px minmax(0, 780px) 200px; justify-content: space-between; }
  .toc { display: block; position: sticky; top: 104px; align-self: start; max-height: calc(100vh - 128px);
         overflow: auto; border-left: 1px solid var(--rule); padding-left: 16px; }
  .toc a { display: block; font-size: 14px; line-height: 1.4; color: var(--muted); padding: 5px 0; }
  .toc a:hover { color: var(--purple); }
}

/* Content */
.content { min-width: 0; max-width: 780px; overflow-wrap: break-word; }
.content h1, .content h2, .content h3, .content h4 { color: var(--ink); scroll-margin-top: 96px; }
.content h1 { font-family: var(--serif); font-weight: 800; font-size: clamp(40px, 5vw, 56px);
              letter-spacing: -0.03em; line-height: 1.04; margin: 0 0 24px; }
.content h2 { font-family: var(--serif); font-weight: 600; font-size: clamp(26px, 3vw, 32px);
              letter-spacing: -0.02em; line-height: 1.15; margin: 56px 0 16px; }
.content h3 { font-size: 20px; font-weight: 700; line-height: 1.3; margin: 36px 0 10px; }
.content h4 { font-size: 16px; font-weight: 700; margin: 28px 0 8px; }
.content h1 code, .content h2 code, .content h3 code { font-size: 0.85em; }
.content p { margin: 0 0 18px; }
.content ul, .content ol { margin: 0 0 18px; padding-left: 1.3em; }
.content li { margin-bottom: 6px; }
.content li > ul, .content li > ol { margin: 6px 0 0; }
.content strong { font-weight: 700; color: var(--ink); }
.content hr { border: none; border-top: 1px solid var(--rule); margin: 48px 0; }
.content p a, .content li a, .content td a { text-decoration: underline; text-decoration-color: var(--lilac-3);
  text-decoration-thickness: 1.5px; text-underline-offset: 3px; }
.content p a:hover, .content li a:hover, .content td a:hover { text-decoration-color: var(--purple); }
.content blockquote { margin: 0 0 24px; background: var(--lilac-2); border-radius: 14px; padding: 16px 20px; }
.content blockquote p:last-child { margin-bottom: 0; }
.content code { font-family: var(--mono); font-size: 0.88em; color: var(--ink); background: var(--lilac-2);
                padding: 0.12em 0.38em; border-radius: 6px; }
.content pre { margin: 0 6px 28px 0; background: var(--surface); border: 1px solid var(--rule); border-radius: 14px;
               padding: 18px 20px; overflow-x: auto; font-family: var(--mono); font-size: 13.5px; line-height: 1.7;
               color: var(--ink); box-shadow: 0 1px 2px rgba(0, 0, 0, 0.04); }
.content :not(pre) > code { overflow-wrap: anywhere; }
.content pre code { background: none; padding: 0; font-size: inherit; border-radius: 0; }
.tablewrap { overflow-x: auto; margin: 0 0 28px; background: var(--surface); border: 1px solid var(--rule); border-radius: 14px; }
.content table { width: 100%; border-collapse: collapse; font-size: 15px; line-height: 1.55; }
.content th, .content td { text-align: left; padding: 10px 14px; border-bottom: 1px solid var(--rule); vertical-align: top; }
.content th { font-family: var(--mono); font-size: 12px; font-weight: 500; text-transform: uppercase; letter-spacing: 0.05em;
              color: var(--muted); background: var(--paper); border-bottom: 1px solid var(--rule); white-space: nowrap; }
.content tbody tr:last-child td { border-bottom: none; }
.content td code { white-space: nowrap; }
@media (min-width: 900px) { .content pre { font-size: 14px; padding: 20px 24px; } }
@media (max-width: 600px) { .content th, .content td { padding: 8px 10px; white-space: normal; } .content table { font-size: 14px; } }

/* Footer (shared look with the homepage) */
.foot { border-top: 1px solid var(--rule); }
.foot-in { padding-block: 40px 48px; display: flex; flex-direction: column; gap: 22px; font-size: 16px; color: var(--ink2); }
.foot .brand { font-size: 26px; }
.foot-links { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0 20px; }
.foot-links a { color: var(--ink); padding-block: 12px; }
.foot-links a:hover { color: var(--purple); }
@media (min-width: 900px) {
  .foot-in { flex-direction: row; align-items: center; gap: 12px 32px; padding-block: 36px; }
  .foot .brand { font-size: 22px; }
  .foot-links { display: flex; gap: 26px; margin-left: auto; }
}
details.cl-full { margin: 4px 0 28px; border: 1px solid var(--rule); border-radius: 12px; padding: 0 16px; }
details.cl-full > summary { cursor: pointer; padding: 10px 0; font-family: var(--mono); font-size: 13px; color: var(--purple); }
details.cl-full[open] { padding-bottom: 8px; }

/* Main navigation: grouped menus (generated by build-docs.py, apply_nav) */
.nav { align-items: center; gap: 4px; }
.nav-g { position: relative; }
.nav-t { display: inline-flex; align-items: center; gap: 7px; height: 40px; padding: 0 12px; border: 0; border-radius: 10px; background: none; font-family: var(--sans); font-weight: 500; font-size: 16px; color: var(--ink); cursor: pointer; }
.nav-t:hover, .nav-g:hover > .nav-t, .nav-g:focus-within > .nav-t { background: var(--lilac-2); color: var(--ink); }
.nav-t svg { transition: transform 0.15s; }
.nav-g:hover > .nav-t svg, .nav-g:focus-within > .nav-t svg { transform: rotate(180deg); }
.nav-g.cur > .nav-t { color: var(--purple); }
.nav-m { position: absolute; top: 100%; left: -6px; z-index: 40; display: none; flex-direction: column; gap: 2px; width: 320px; margin-top: 8px; padding: 8px; background: var(--surface); border: 1px solid var(--rule); border-radius: 16px; box-shadow: 0 1px 2px rgba(0, 0, 0, 0.04); }
.nav-m::before { content: ""; position: absolute; left: 0; right: 0; top: -12px; height: 12px; }
.nav-g:hover > .nav-m, .nav-g:focus-within > .nav-m { display: flex; }
.nav .nav-m a { display: flex; flex-direction: column; gap: 2px; padding: 10px 12px; border-radius: 10px; color: var(--ink); text-decoration: none; }
.nav .nav-m a:hover { background: var(--lilac-2); color: var(--ink); }
.nav-m b { font-weight: 700; font-size: 15px; }
.nav-m span { font-size: 13.5px; line-height: 1.4; color: var(--muted); }
.nav .nav-m a[aria-current] b { color: var(--purple); }
.dr-g { padding-block: 10px; border-bottom: 1px solid var(--rule); }
.dr-h { display: block; padding-block: 6px; font-family: var(--mono); font-size: 12px; letter-spacing: 0.06em; text-transform: uppercase; color: var(--faint); }
.drawer .dr-g a { border-bottom: 0; padding-block: 9px; font-size: 17px; }
.drawer .dr-g a[aria-current] { color: var(--purple); }
.drawer .dr-gh { border-bottom: 0; }
@media (min-width: 1100px) {
  .nav { display: flex; }
  .menu-btn, .drawer, .top.open .drawer { display: none; }
}

/* ReactVision-style refinements: sans headings, gray labels, pill buttons, quiet cards */
h1, h2, h3, .brand { letter-spacing: -0.02em; }
.article h2 { font-weight: 700; }
.eyebrow { font-family: var(--sans); font-weight: 600; font-size: 12px; letter-spacing: 0.12em; color: var(--muted); }
.btn-install, .cta-btns a, .chips a { border-radius: 9999px; }
.btn-install { padding: 10px 18px; font-size: 14px; font-weight: 600; }
.btn-install:hover { background: var(--ink); opacity: 0.85; color: var(--paper); }
.menu-btn { border-radius: 9999px; border: 1px solid var(--rule); }
.btn-w { border-radius: 14px; }
.btn-w:hover { background: var(--surface); border-color: var(--rule-hover); color: var(--ink); }
.chips a:hover { background: var(--surface); border-color: var(--rule-hover); color: var(--ink); }
pre { background: var(--code-bg); }
code { background: var(--lilac-2); }
figure img { border-radius: 14px; }
.cta-band { background: var(--surface); color: var(--ink); border-top: 1px solid var(--rule); border-bottom: 1px solid var(--rule); }
.cta-band h2 { color: var(--ink); }
.cta-band p { color: var(--muted); }
.cta-btns a { border: 1px solid var(--rule); height: 44px; font-size: 15px; font-weight: 600; }
.cta-dark { background: var(--ink); color: var(--paper); border-color: var(--ink) !important; }
.cta-dark:hover { opacity: 0.85; background: var(--ink); color: var(--paper); }
.cta-line { color: var(--ink); background: var(--surface); }
.cta-line:hover { background: var(--surface); color: var(--ink); border-color: var(--rule-hover); }
.nav-m { border-radius: 14px; box-shadow: 0 16px 40px -20px rgba(0, 0, 0, 0.25); }
.pull { border: 1px solid var(--rule); background: var(--surface); border-radius: 14px; }
@media (prefers-color-scheme: dark) { img[src$="reactvision.png"], .user img { filter: invert(1); mix-blend-mode: normal; } }
.side .grp, .side-m .grp, .toc .grp, .dr-h { font-family: var(--sans); font-weight: 600; font-size: 12px; letter-spacing: 0.12em; color: var(--muted); }
.content th { font-family: var(--sans); font-weight: 600; letter-spacing: 0.08em; }
""".strip()

SHELL = """<!doctype html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>{title}</title>
  <meta name="description" content="{desc}" />
  <meta name="theme-color" content="#fbf8f3" />
  <link rel="canonical" href="https://eskiu-lang.org/{canon}" />
  <meta property="og:type" content="website" />
  <meta property="og:site_name" content="Eskiu" />
  <meta property="og:title" content="{title}" />
  <meta property="og:description" content="{desc}" />
  <meta property="og:url" content="https://eskiu-lang.org/{canon}" />
  <meta property="og:image" content="https://eskiu-lang.org/assets/og.png" />
  <meta property="og:image:width" content="1200" />
  <meta property="og:image:height" content="630" />
  <meta name="twitter:card" content="summary_large_image" />
  <link rel="icon" type="image/png" href="{a}logo.png" />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&amp;family=JetBrains+Mono:wght@400;500&amp;display=swap" rel="stylesheet" />
  <style>{css}</style>
</head>
<body>
  <header class="top">
    <div class="bar top-in">
      <a class="brand" href="{p}index.html"><img src="{a}logo.png" alt="" />eskiu<span class="v">{version}</span></a>
      <nav class="nav" aria-label="Main">{topnav}</nav>
      <div class="top-right">
        <a class="btn-install" href="{p}index.html#install">Install {vnum}</a>
        <button class="menu-btn" type="button" aria-label="Open menu" aria-expanded="false" aria-controls="drawer">
          <svg width="20" height="20" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><line x1="3" y1="6" x2="17" y2="6"/><line x1="3" y1="10" x2="17" y2="10"/><line x1="3" y1="14" x2="17" y2="14"/></svg>
        </button>
      </div>
    </div>
    <nav class="drawer" id="drawer" aria-label="Main">{topnav}</nav>
  </header>
  <div class="bar layout{layoutcls}">
    <aside class="side" aria-label="Documentation">{side}</aside>
    <main class="content">
      <details class="side-m"><summary>{summary}</summary><div class="inner">{side}</div></details>
{body}
    </main>
{toc}
  </div>
  <footer class="foot">
    <div class="bar foot-in">
      <span class="brand">eskiu</span>
      <span>Part of the <a href="https://reactvision.xyz">ReactVision</a> family · MIT</span>
      <nav class="foot-links" aria-label="Footer">
        <a href="{d}index.html">Docs</a>
        <a href="{p}the-book-of-eskiu.html">Book</a>
        <a href="{d}internals.html">Internals</a>
        <a href="{p}changelog.html">Changelog</a>
        <a href="{p}llms.txt">llms.txt</a>
        <a href="{gh}">GitHub</a>
        <a href="https://reactvision.xyz">ReactVision</a>
      </nav>
    </div>
  </footer>
  <script>(function(){{var t=document.querySelector(".top"),b=document.querySelector(".menu-btn");if(!t||!b)return;b.addEventListener("click",function(){{var o=t.classList.toggle("open");b.setAttribute("aria-expanded",o?"true":"false");}});}})();</script>
</body>
</html>
"""
