#!/usr/bin/env python3
"""
Generate the site's docs pages from the Markdown sources in docs/.

The repo is private, so the public can't read the docs on GitHub. This renders
the language docs (docs/lang/*) and the contributor docs (docs/dev/*) into self-contained, styled HTML
under site/docs/, in the same Editorial look as the homepage (site/index.html).

It also writes the package catalog (site/packages.html, site/es/packages.html)
from the READMEs of the eskiu-lang packages at their released tag, fetched from
raw.githubusercontent.com.

Usage:  python3 site/build-docs.py                 (run from anywhere)
        python3 site/build-docs.py --packages      (only the package catalog)
        python3 site/build-docs.py --no-packages   (offline: skip the catalog)

Requires: markdown  (pip install --user markdown)
Re-run whenever the Markdown sources change, then commit site/docs/.
"""

import html
import os
import re
import sys

import markdown

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "site", "docs")

# source (relative to repo root) -> (output basename, page title, section)
PAGES = [
    ("docs/lang/index.md",           "index.html",           "Documentation",        "lang"),
    ("docs/lang/getting-started.md", "getting-started.html", "Getting started",      "lang"),
    ("docs/lang/spec.md",            "spec.html",            "Language spec",        "lang"),
    ("docs/lang/grammar.md",         "grammar.html",         "Grammar",              "lang"),
    ("docs/lang/build.md",           "build.html",           "Building & tooling",   "lang"),
    ("docs/lang/stability.md",       "stability.html",       "Stability",            "lang"),
    ("docs/dev/api.md",              "api.html",             "Compiler C++ API",     "dev"),
    ("docs/lang/glossary.md",        "glossary.html",        "Glossary",             "lang"),
    ("docs/dev/roadmap.md",          "roadmap.html",         "Roadmap",              "dev"),
    ("docs/dev/index.md",            "internals.html",       "Compiler internals",   "dev"),
    ("docs/dev/architecture.md",     "architecture.html",    "Architecture",         "dev"),
    ("docs/dev/design.md",           "design.html",          "Design",               "dev"),
    ("docs/dev/abi.md",              "abi.html",             "ABI",                  "dev"),
    ("docs/dev/async-design.md",     "async-design.html",    "Async design",         "dev"),
    ("docs/dev/cross-compile.md",    "cross-compile.html",   "Cross-compiling",      "dev"),
    ("docs/dev/self-hosting.md",     "self-hosting.html",    "Self-hosting",         "dev"),
    ("docs/dev/http2-design.md",     "http2-design.html",    "HTTP/2 design",        "dev"),
    ("docs/dev/debugging.md",        "debugging.html",       "Debugging",            "dev"),
    ("docs/dev/contributing.md",     "contributing.html",    "Contributing",         "dev"),
]

# repo-relative source path (normalized) -> output html, for link rewriting
PATHMAP = {os.path.normpath(src): out for src, out, _, _ in PAGES}

# top nav, per section
NAV_LANG = [
    ("index.html", "Overview"),
    ("getting-started.html", "Getting started"),
    ("spec.html", "Spec"),
    ("grammar.html", "Grammar"),
    ("build.html", "Tooling"),
    ("stability.html", "Stability"),
    ("glossary.html", "Glossary"),
    ("internals.html", "Internals →"),
]
NAV_DEV = [
    ("index.html", "← Language docs"),
    ("internals.html", "Overview"),
    ("architecture.html", "Architecture"),
    ("design.html", "Design"),
    ("abi.html", "ABI"),
    ("async-design.html", "Async"),
    ("self-hosting.html", "Self-hosting"),
    ("http2-design.html", "HTTP/2"),
    ("api.html", "C++ API"),
    ("debugging.html", "Debugging"),
    ("contributing.html", "Contributing"),
    ("roadmap.html", "Roadmap"),
]
NAVS = {"lang": NAV_LANG, "dev": NAV_DEV}

# un-ported docs fall back to the GitHub source
GH_BLOB = "https://github.com/eskiu-lang/eskiu/blob/main/"

# Shown in the shared top bar (kept in step with the homepage). Bump per release.
VERSION = "v0.12.0"
GH = "https://github.com/eskiu-lang/eskiu"

CSS = r"""
*, *::before, *::after { box-sizing: border-box; }
:root {
  --paper: #fbf8f3; --ink: #1c1830; --ink2: #4a4560; --muted: #6b6580; --faint: #8b8699;
  --rule: #e6e0d6; --purple: #3b2a8c; --lilac: #e9e1f4; --lilac-2: #f1ecf8; --lilac-3: #d9d1f0;
  --serif: "Fraunces", Georgia, serif;
  --sans: "DM Sans", -apple-system, "Segoe UI", sans-serif;
  --mono: "DM Mono", ui-monospace, "SF Mono", Menlo, Consolas, monospace;
  --gutter: 24px;
}
@media (min-width: 900px) { :root { --gutter: 48px; } }
html { scroll-behavior: smooth; -webkit-text-size-adjust: 100%; text-size-adjust: 100%; }
body { margin: 0; background: var(--paper); color: var(--ink2); font-family: var(--sans); font-size: 17px;
       line-height: 1.7; -webkit-font-smoothing: antialiased; }
a { color: var(--purple); text-decoration: none; }
a:hover { color: var(--ink); }
.bar { max-width: 1440px; margin: 0 auto; padding-inline: var(--gutter); }

/* Header (shared look with the homepage) */
.top { position: sticky; top: 0; z-index: 30; background: rgba(251, 248, 243, 0.94);
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
.btn-install:hover { background: var(--purple); color: #fff; }
.menu-btn { width: 48px; height: 48px; border: 1.5px solid var(--ink); background: #fff; border-radius: 14px;
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
.side-m { margin: 0 0 36px; background: #fff; border: 1.5px solid var(--ink); border-radius: 14px; }
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
         overflow: auto; border-left: 1.5px solid var(--rule); padding-left: 16px; }
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
.content hr { border: none; border-top: 1.5px solid var(--rule); margin: 48px 0; }
.content p a, .content li a, .content td a { text-decoration: underline; text-decoration-color: var(--lilac-3);
  text-decoration-thickness: 1.5px; text-underline-offset: 3px; }
.content p a:hover, .content li a:hover, .content td a:hover { text-decoration-color: var(--purple); }
.content blockquote { margin: 0 0 24px; background: var(--lilac-2); border-radius: 14px; padding: 16px 20px; }
.content blockquote p:last-child { margin-bottom: 0; }
.content code { font-family: var(--mono); font-size: 0.88em; color: var(--ink); background: var(--lilac-2);
                padding: 0.12em 0.38em; border-radius: 6px; }
.content pre { margin: 0 6px 28px 0; background: #fff; border: 1.5px solid var(--ink); border-radius: 14px;
               padding: 18px 20px; overflow-x: auto; font-family: var(--mono); font-size: 13.5px; line-height: 1.7;
               color: var(--ink); box-shadow: 5px 5px 0 var(--lilac); }
.content :not(pre) > code { overflow-wrap: anywhere; }
.content pre code { background: none; padding: 0; font-size: inherit; border-radius: 0; }
.tablewrap { overflow-x: auto; margin: 0 0 28px; background: #fff; border: 1.5px solid var(--ink); border-radius: 14px; }
.content table { width: 100%; border-collapse: collapse; font-size: 15px; line-height: 1.55; }
.content th, .content td { text-align: left; padding: 10px 14px; border-bottom: 1px solid var(--rule); vertical-align: top; }
.content th { font-family: var(--mono); font-size: 12px; font-weight: 500; text-transform: uppercase; letter-spacing: 0.05em;
              color: var(--muted); background: var(--paper); border-bottom: 1.5px solid var(--ink); white-space: nowrap; }
.content tbody tr:last-child td { border-bottom: none; }
.content td code { white-space: nowrap; }
@media (min-width: 900px) { .content pre { font-size: 14px; padding: 20px 24px; } }
@media (max-width: 600px) { .content th, .content td { padding: 8px 10px; white-space: normal; } .content table { font-size: 14px; } }

/* Footer (shared look with the homepage) */
.foot { border-top: 1.5px solid var(--ink); }
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
  <link rel="icon" type="image/png" href="{a}logo.png" />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=Fraunces:opsz,wght@9..144,600;9..144,800&amp;family=DM+Sans:wght@400;500;700&amp;family=DM+Mono:wght@400;500&amp;display=swap" rel="stylesheet" />
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
        <a href="{gh}">GitHub</a>
        <a href="https://reactvision.xyz">ReactVision</a>
      </nav>
    </div>
  </footer>
  <script>(function(){{var t=document.querySelector(".top"),b=document.querySelector(".menu-btn");if(!t||!b)return;b.addEventListener("click",function(){{var o=t.classList.toggle("open");b.setAttribute("aria-expanded",o?"true":"false");}});}})();</script>
</body>
</html>
"""

# Sidebar groups (desktop aside and mobile <details>). Hrefs are relative to site/docs/.
SIDEBAR = [
    ("Language", [(h, l) for h, l in NAV_LANG if h != "internals.html"]),
    ("Compiler internals", [(h, l) for h, l in NAV_DEV if h != "index.html"]),
]

# Header nav: (href relative to site/, label, key used to mark the current section)
TOP_LINKS = [
    ("quickstart.html", "Learn", "learn"),
    ("docs/index.html", "Docs", "docs"),
    ("index.html#playground", "Playground", "pg"),
    ("the-book-of-eskiu.html", "Book", "book"),
    ("changelog.html", "Changelog", "changelog"),
    (GH, "GitHub", "gh"),
]


def build_topnav(p, current):
    out = []
    for href, label, key in TOP_LINKS:
        url = href if "://" in href else p + href
        cur = ' aria-current="page"' if key == current else ""
        out.append(f'<a href="{url}"{cur}>{html.escape(label)}</a>')
    return "".join(out)


def build_sidebar(active, d):
    out = []
    for group, links in SIDEBAR:
        out.append(f'<span class="grp">{html.escape(group)}</span><nav>')
        for href, label in links:
            label = label.replace(" →", "")
            cur = ' aria-current="page"' if href == active else ""
            out.append(f'<a href="{d}{href}"{cur}>{html.escape(label)}</a>')
        out.append("</nav>")
    return "".join(out)


def build_toc(tokens):
    """'On this page' list from the level-2 headings the toc extension found."""
    items = []

    def walk(ts):
        for t in ts:
            if t["level"] == 2:
                items.append(t)
            walk(t.get("children", []))

    walk(tokens)
    if len(items) < 3:
        return "", ""
    links = "".join(
        f'<a href="#{t["id"]}">{html.escape(html.unescape(re.sub(r"<[^>]+>", "", t["name"])))}</a>'
        for t in items
    )
    return f'    <nav class="toc" aria-label="On this page"><span class="grp">On this page</span>{links}</nav>', " has-toc"


def section_of(out):
    for group, links in SIDEBAR:
        for href, label in links:
            if href == out:
                return group
    return "Docs"


def collapse_changelog(text):
    """Each release shows its `### Highlights` and keeps the full entry in a collapsed
    <details>; a release without highlights is collapsed whole. CHANGELOG.md itself
    stays complete."""
    parts = re.split(r"(?m)^(?=## \[)", text)
    out = [parts[0]]
    for part in parts[1:]:
        head, _, rest = part.partition("\n")
        hl = ""
        m = re.search(r"(?ms)^### Highlights\n(.*?)(?=^### |\Z)", rest)
        if m:
            hl = m.group(1).strip()
            rest = rest[:m.start()] + rest[m.end():]
        rest = rest.strip()
        block = head + "\n\n"
        if hl:
            block += hl + "\n\n"
        if rest:
            label = "Full details" if hl else "Details"
            block += ('<details class="cl-full" markdown="1">\n<summary>' + label +
                      "</summary>\n\n" + rest + "\n\n</details>\n\n")
        out.append(block)
    return "".join(out)


def render(*, title, desc, canon, p, d, a, current, active, summary, body, toc_tokens):
    toc, layoutcls = build_toc(toc_tokens)
    return SHELL.format(
        title=html.escape(title), desc=html.escape(desc), canon=canon, a=a, p=p, d=d,
        css=CSS, version=VERSION, vnum=VERSION.lstrip("v"), gh=GH,
        topnav=build_topnav(p, current), side=build_sidebar(active, d),
        summary=html.escape(summary), body=body, toc=toc, layoutcls=layoutcls,
    )


def make_rewriter(srcdir):
    """Return a regex callback that rewrites .md hrefs relative to srcdir."""

    def rewrite(m):
        target = m.group(1)
        anchor = ""
        if "#" in target:
            target, anchor = target.split("#", 1)
            anchor = "#" + anchor
        if not target:  # pure in-page anchor
            return f'href="{anchor}"'
        if "://" in target:  # already absolute
            return m.group(0)
        # resolve relative to the source file's directory, against repo root
        resolved = os.path.normpath(os.path.join(srcdir, target))
        if resolved in PATHMAP:
            return f'href="{PATHMAP[resolved]}{anchor}"'
        if target.endswith(".md"):  # un-ported doc -> GitHub source
            return f'href="{GH_BLOB}{resolved}{anchor}"'
        return m.group(0)

    return rewrite


def first_paragraph(md_text):
    for line in md_text.splitlines():
        s = line.strip()
        if s and not s.startswith("#") and not s.startswith("```"):
            return re.sub(r"[`*\[\]]", "", s)[:155]
    return "Eskiu language documentation."


# ---------------------------------------------------------------------------
# Package catalog: site/packages.html and site/es/packages.html.
#
# Each package's tagline and code example come from its README at the released
# tag (raw.githubusercontent.com, so the catalog matches what `eskiuc add` gets).
# The table below adds what a README does not carry: the README section the
# example comes from, a short note and the requirements, in English and Spanish,
# and the Spanish tagline.

PKG_ORG = "eskiu-lang"
PKG_REV = "v0.1.0"

PACKAGES_CATALOG = [
    {
        "name": "gpu",
        "section": "Models",
        "tag_es": "Cómputo en GPU, renderizado, modelos 3D y machine learning para Eskiu, sobre WebGPU.",
        "note_en": "One API runs on Metal (macOS), Vulkan (Linux) and DirectX 12 (Windows) through "
                   "wgpu-native, with shaders in WGSL. It draws glTF and OBJ models with physically "
                   "based shading, runs Llama models from GGUF or safetensors files (Llama 2 7B Q4_0 "
                   "at 45 tokens per second on an M4 Pro; llama.cpp does 58 on the same machine) and "
                   "trains small networks (97% test accuracy on MNIST).",
        "note_es": "Una sola API corre sobre Metal (macOS), Vulkan (Linux) y DirectX 12 (Windows) a "
                   "través de wgpu-native, con shaders en WGSL. Dibuja modelos glTF y OBJ con "
                   "sombreado físico (PBR), corre modelos Llama desde archivos GGUF o safetensors "
                   "(Llama 2 7B Q4_0 a 45 tokens por segundo en un M4 Pro; llama.cpp da 58 en la "
                   "misma máquina) y entrena redes pequeñas (97% de precisión en MNIST).",
        "needs_en": "Needs wgpu-native 29, and SDL3 for windows: <code>brew install wgpu-native sdl3</code> "
                    "on macOS, the release archives on Linux and Windows.",
        "needs_es": "Necesita wgpu-native 29, y SDL3 para abrir ventanas: <code>brew install wgpu-native sdl3</code> "
                    "en macOS, los archivos de cada release en Linux y Windows.",
        "setup": [],
        "build": ["eskiuc build -L/opt/homebrew/lib"],
    },
    {
        "name": "onnx",
        "section": "Inference",
        "tag_es": "Inferencia de modelos ONNX para Eskiu, sobre ONNX Runtime.",
        "note_en": "A model exported from PyTorch, TensorFlow or scikit-learn runs on the CPU "
                   "everywhere, and on CoreML, CUDA, DirectML or WebGPU when the ONNX Runtime build "
                   "has them. Wrong input names, types or shapes come back as error messages.",
        "note_es": "Un modelo exportado desde PyTorch, TensorFlow o scikit-learn corre en el CPU en "
                   "cualquier sistema, y en CoreML, CUDA, DirectML o WebGPU cuando el build de ONNX "
                   "Runtime los trae. Un nombre, tipo o shape de entrada equivocado regresa como "
                   "mensaje de error.",
        "needs_en": "Needs ONNX Runtime 1.31 or newer: <code>brew install onnxruntime</code> on macOS, "
                    "the release archives on Linux and Windows.",
        "needs_es": "Necesita ONNX Runtime 1.31 o más nuevo: <code>brew install onnxruntime</code> en "
                    "macOS, los archivos de cada release en Linux y Windows.",
        "setup": [],
        "build": ["eskiuc build -L/opt/homebrew/lib"],
    },
    {
        "name": "tflite",
        "section": "Inference",
        "tag_es": "Inferencia de modelos de TensorFlow Lite para Eskiu.",
        "note_en": "A <code>.tflite</code> model converted from TensorFlow, Keras, JAX or PyTorch runs "
                   "on the CPU with XNNPACK, and on the GPU through the Metal delegate (macOS) or the "
                   "OpenCL/OpenGL delegate (Linux). Quantized inputs and outputs are converted for you.",
        "note_es": "Un modelo <code>.tflite</code> convertido desde TensorFlow, Keras, JAX o PyTorch "
                   "corre en el CPU con XNNPACK, y en la GPU con el delegado de Metal (macOS) o el de "
                   "OpenCL/OpenGL (Linux). Las entradas y salidas cuantizadas se convierten solas.",
        "needs_en": "Needs <code>libtensorflowlite_c</code> 2.21, which Google does not publish for "
                    "desktops; the package's <code>tools/get_tflite.sh</code> downloads a build for "
                    "your platform.",
        "needs_es": "Necesita <code>libtensorflowlite_c</code> 2.21, que Google no publica para "
                    "escritorio; el script <code>tools/get_tflite.sh</code> del paquete descarga un "
                    "build para tu plataforma.",
        "setup": ["curl -fsSL https://raw.githubusercontent.com/eskiu-lang/tflite/v0.1.0/tools/get_tflite.sh | sh -s -- ~/tflite",
                  "export TFL=~/tflite"],
        "build": ["eskiuc build -L$TFL/lib --link-arg=-Wl,-rpath,$TFL/lib"],
    },
    {
        "name": "compress",
        "section": "ZIP archives",
        "tag_es": "zlib, gzip, DEFLATE crudo y archivos ZIP para Eskiu, escrito en Eskiu.",
        "note_en": "DEFLATE at levels 0 to 9, zlib and gzip in both directions, and ZIP archives "
                   "with ZIP64. Every read is bounds checked and the output is capped by a size "
                   "limit, so a small file cannot expand into all of memory.",
        "note_es": "DEFLATE en niveles del 0 al 9, zlib y gzip en los dos sentidos, y archivos ZIP "
                   "con ZIP64. Cada lectura revisa sus límites y la salida tiene un tope de tamaño, "
                   "así que un archivo chico no puede inflarse hasta llenar la memoria.",
        "needs_en": "Nothing to install: no zlib and no C code.",
        "needs_es": "No hay que instalar nada: no usa zlib ni código en C.",
        "setup": [],
        "build": [],
    },
    {
        "name": "audio",
        "section": "Playing a sound",
        "tag_es": "Sonido para Eskiu: archivos WAV, un mezclador y salida con SDL3.",
        "note_en": "Reads WAV files in every common encoding (PCM, float, IMA and Microsoft ADPCM) "
                   "as ffmpeg decodes them, mixes any number of voices with volume, pan, pitch and "
                   "loops, and sends the mix to the default output device.",
        "note_es": "Lee archivos WAV en todas las codificaciones comunes (PCM, float, ADPCM de IMA y "
                   "de Microsoft) igual que los decodifica ffmpeg, mezcla todas las voces que "
                   "quieras con volumen, paneo, tono y loops, y manda la mezcla a la salida de audio "
                   "predeterminada.",
        "needs_en": "<code>&lt;audio/wav&gt;</code> and <code>&lt;audio/mixer&gt;</code> are pure "
                    "Eskiu; <code>&lt;audio/device&gt;</code> needs SDL3 (<code>brew install sdl3</code> "
                    "on macOS).",
        "needs_es": "<code>&lt;audio/wav&gt;</code> y <code>&lt;audio/mixer&gt;</code> son Eskiu puro; "
                    "<code>&lt;audio/device&gt;</code> necesita SDL3 (<code>brew install sdl3</code> en "
                    "macOS).",
        "setup": [],
        "build": ["eskiuc build -L/opt/homebrew/lib"],
    },
    {
        "name": "font",
        "section": "Drawing text",
        "tag_es": "Fuentes TrueType para Eskiu: contornos de glifos, renderizado con antialiasing, "
                  "acomodo de texto y atlas.",
        "note_en": "Reads <code>.ttf</code> files, renders glyphs into antialiased coverage bitmaps, "
                   "lays out lines of UTF-8 text with kerning, and packs a character set into one "
                   "texture for text on the GPU.",
        "note_es": "Lee archivos <code>.ttf</code>, convierte los glifos en bitmaps de cobertura con "
                   "antialiasing, acomoda líneas de texto UTF-8 con kerning y empaca un conjunto de "
                   "caracteres en una sola textura para dibujar texto en la GPU.",
        "needs_en": "Nothing to install.",
        "needs_es": "No hay que instalar nada.",
        "setup": [],
        "build": [],
    },
    {
        "name": "nds",
        "section": "Archives and ROMs",
        "tag_es": "Formatos de archivo de la Nintendo DS para Eskiu: ROMs, archivos NARC, sonido, "
                  "gráficos 2D y texturas 3D.",
        "note_en": "ROM file systems and overlays, NARC archives, LZ compression, SDAT sound with a "
                   "synthesizer of the DS sound driver, 2D graphics and 3D textures, in pure Eskiu. "
                   "It was ported from the tools and engine of Tallgrass and checked against them.",
        "note_es": "Sistemas de archivos y overlays de ROMs, archivos NARC, compresión LZ, sonido "
                   "SDAT con un sintetizador del driver de sonido de la DS, gráficos 2D y texturas 3D, "
                   "todo en Eskiu puro. Se portó de las herramientas y el motor de Tallgrass y se "
                   "comparó contra ellos.",
        "needs_en": "Nothing to install.",
        "needs_es": "No hay que instalar nada.",
        "setup": [],
        "build": [],
    },
]


def fetch_readme(name):
    import urllib.request
    url = f"https://raw.githubusercontent.com/{PKG_ORG}/{name}/{PKG_REV}/README.md"
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode("utf-8")


def readme_tagline(text, name):
    m = re.search(r'<h2 align="center">[^<]*</h2>\s*<p align="center">(.*?)</p>', text, re.S)
    if not m:
        raise SystemExit(f"packages: no tagline in the {name} README")
    return html.unescape(re.sub(r"<[^>]+>", "", m.group(1))).strip()


def readme_example(text, name, section):
    m = re.search(r"(?m)^#{2,3} " + re.escape(section) + r"\s*$", text)
    if not m:
        raise SystemExit(f"packages: no '{section}' section in the {name} README")
    b = re.search(r"(?ms)^```eskiu\n(.*?)^```", text[m.end():])
    if not b:
        raise SystemExit(f"packages: no eskiu code under '{section}' in the {name} README")
    return b.group(1).rstrip("\n")


def highlight_comments(code):
    """Escape Eskiu source and wrap each // comment (outside string and char literals)."""
    out = []
    for line in code.split("\n"):
        quote, i, cut = None, 0, -1
        while i < len(line):
            c = line[i]
            if quote:
                if c == "\\":
                    i += 1
                elif c == quote:
                    quote = None
            elif c in "\"'":
                quote = c
            elif line.startswith("//", i):
                cut = i
                break
            i += 1
        if cut < 0:
            out.append(html.escape(line, quote=False))
        else:
            out.append(html.escape(line[:cut], quote=False) +
                       '<span class="c">' + html.escape(line[cut:], quote=False) + "</span>")
    return "\n".join(out)


PKG_TEXT = {
    "en": {
        "lang": "en", "title": "Packages · Eskiu", "eyebrow": "Packages", "h1": "Official packages",
        "desc": "The official Eskiu packages: gpu (compute, rendering, glTF and OBJ models, Llama "
                "inference and training over WebGPU), onnx, tflite, compress, audio, font and nds, "
                "each added with one eskiuc add.",
        "lede": "Libraries from the <a href=\"https://github.com/eskiu-lang\">eskiu-lang</a> "
                "organization on GitHub. Each one is a git repository with an "
                "<code>eskiu.toml</code>, added to a project with one command.",
        "how_h": "Using a package",
        "how_p": "<code>eskiuc add</code> writes the dependency into <code>eskiu.toml</code>, fetches "
                 "it with git and records its commit in <code>eskiu.lock</code>, so later builds "
                 "reuse it without touching the network. There is no registry: a dependency is a "
                 "git URL with a rev (a tag, branch or commit) or a local path.",
        "how_cmd": ["eskiuc new myapp", "cd myapp",
                    "eskiuc add compress --git https://github.com/eskiu-lang/compress --rev v0.1.0",
                    "eskiuc run"],
        "how_p2": "A package's modules are imported by the package name and the file name, as in "
                  "<code>import &lt;gpu/model&gt;;</code>. Chapter 13 of the "
                  "<a href=\"{p}the-book-of-eskiu.html#sec-13-5\">Book</a> covers manifests, locks "
                  "and <code>eskiuc test</code>.",
        "chips": "Packages", "install": "Install", "example": "From the README",
        "repo": "Repository", "ci": "CI status",
        "outro": "Every package needs Eskiu 0.12 or newer. Its README has the full module list, "
                 "more examples and the details for each operating system.",
    },
    "es": {
        "lang": "es", "title": "Paquetes · Eskiu", "eyebrow": "Paquetes", "h1": "Paquetes oficiales",
        "desc": "Los paquetes oficiales de Eskiu: gpu (cómputo, renderizado, modelos glTF y OBJ, "
                "inferencia y entrenamiento de Llama sobre WebGPU), onnx, tflite, compress, audio, "
                "font y nds, cada uno se agrega con un eskiuc add.",
        "lede": "Bibliotecas de la organización <a href=\"https://github.com/eskiu-lang\">eskiu-lang</a> "
                "en GitHub. Cada una es un repositorio de git con un <code>eskiu.toml</code>, y se "
                "agrega a un proyecto con un solo comando.",
        "how_h": "Cómo usar un paquete",
        "how_p": "<code>eskiuc add</code> escribe la dependencia en <code>eskiu.toml</code>, la "
                 "descarga con git y guarda su commit en <code>eskiu.lock</code>, así que los builds "
                 "siguientes la reutilizan sin tocar la red. No hay un registro central: una "
                 "dependencia es una URL de git con un rev (un tag, una rama o un commit) o una ruta "
                 "local.",
        "how_cmd": ["eskiuc new myapp", "cd myapp",
                    "eskiuc add compress --git https://github.com/eskiu-lang/compress --rev v0.1.0",
                    "eskiuc run"],
        "how_p2": "Los módulos de un paquete se importan con el nombre del paquete y el del archivo, "
                  "por ejemplo <code>import &lt;gpu/model&gt;;</code>. El capítulo 13 del "
                  "<a href=\"{p}the-book-of-eskiu.html#sec-13-5\">libro</a> explica los manifiestos, "
                  "el lock y <code>eskiuc test</code>.",
        "chips": "Paquetes", "install": "Instalación", "example": "Del README",
        "repo": "Repositorio", "ci": "Estado de CI",
        "outro": "Todos los paquetes necesitan Eskiu 0.12 o más nuevo. En el README de cada uno "
                 "están la lista completa de módulos, más ejemplos y los detalles para cada sistema "
                 "operativo (en inglés).",
    },
}

# Header nav of the hand-written and generated top-level pages, per language:
# (href relative to the page's directory, label, key)
SITE_NAV = {
    "en": [("quickstart.html", "Learn", "learn"), ("docs/index.html", "Docs", "docs"),
           ("packages.html", "Packages", "packages"), ("platforms.html", "Platforms", "platforms"),
           ("showcase.html", "Showcase", "showcase"), ("the-book-of-eskiu.html", "Book", "book"),
           ("changelog.html", "Changelog", "changelog"), (GH, "GitHub", "gh")],
    "es": [("quickstart.html", "Aprende", "learn"), ("../docs/index.html", "Docs", "docs"),
           ("packages.html", "Paquetes", "packages"), ("platforms.html", "Plataformas", "platforms"),
           ("showcase.html", "Proyectos", "showcase"), ("../the-book-of-eskiu.html", "Libro", "book"),
           ("../changelog.html", "Cambios", "changelog"), (GH, "GitHub", "gh")],
}

PAGE_SHELL = """<!doctype html>
<html lang="{lang}">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
{redirect}  <title>{title}</title>
  <meta name="description" content="{desc}" />
  <link rel="canonical" href="https://eskiu-lang.org/{canon}" />
  <link rel="alternate" hreflang="en" href="https://eskiu-lang.org/{page}" />
  <link rel="alternate" hreflang="es" href="https://eskiu-lang.org/es/{page}" />
  <link rel="alternate" hreflang="x-default" href="https://eskiu-lang.org/{page}" />
  <link rel="icon" type="image/png" href="{a}logo.png" />
  <meta name="theme-color" content="#fbf8f3" />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=Fraunces:opsz,wght@9..144,400;9..144,600;9..144,800&amp;family=DM+Sans:wght@400;500;700&amp;family=DM+Mono:wght@400;500&amp;display=swap" rel="stylesheet" />
  <link rel="stylesheet" href="{p}page.css" />
</head>
<body>
  <header class="top">
    <div class="bar top-in">
      <a class="brand" href="index.html"><img src="{a}logo.png" alt="" />eskiu<span class="v">{version}</span></a>
      <nav class="nav" aria-label="{navlabel}">{nav}</nav>
      <div class="top-right">
        <span class="lang">{switch}</span>
        <a class="btn-install" href="index.html#install">{installbtn} {vnum}</a>
        <button class="menu-btn" type="button" aria-label="{menulabel}" aria-expanded="false" aria-controls="drawer"><svg width="20" height="20" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><line x1="3" y1="6" x2="17" y2="6"/><line x1="3" y1="10" x2="17" y2="10"/><line x1="3" y1="14" x2="17" y2="14"/></svg></button>
      </div>
    </div>
    <nav class="drawer" id="drawer" aria-label="{navlabel}">{nav}</nav>
  </header>
  <main class="bar article">
{body}
  </main>
  <footer class="foot">
    <div class="bar foot-in">
      <span class="brand">eskiu</span>
      <span>{family}</span>
      <nav class="foot-links" aria-label="{footlabel}"><a href="{p}docs/index.html">Docs</a><a href="{p}the-book-of-eskiu.html">{book}</a><a href="{p}docs/internals.html">Internals</a><a href="{p}changelog.html">{changelog}</a><a href="{gh}">GitHub</a><a href="https://reactvision.xyz">ReactVision</a></nav>
    </div>
  </footer>
  <script>(function(){{var t=document.querySelector(".top"),b=document.querySelector(".menu-btn");if(!t||!b)return;b.addEventListener("click",function(){{var o=t.classList.toggle("open");b.setAttribute("aria-expanded",o?"true":"false");}});}})();</script>
</body>
</html>
"""


def site_nav(lang, current):
    return "".join(
        f'<a href="{h}"{" aria-current=" + chr(34) + "page" + chr(34) if k == current else ""}>{html.escape(l)}</a>'
        for h, l, k in SITE_NAV[lang]
    )


def render_site_page(lang, page, current, title, desc, body):
    en = lang == "en"
    if en:
        redirect = ('  <script>try{if(localStorage.getItem("eskiu-lang")==="es")location.replace("es/'
                    + page + '"+location.hash);}catch(e){}</script>\n')
        switch = ('<span aria-current="page">EN</span> / <a href="es/' + page +
                  '" onclick="try{localStorage.setItem(\'eskiu-lang\',\'es\')}catch(e){}">ES</a>')
    else:
        redirect = ""
        switch = ('<a href="../' + page + '" onclick="try{localStorage.setItem(\'eskiu-lang\',\'en\')}'
                  'catch(e){}">EN</a> / <span aria-current="page">ES</span>')
    return PAGE_SHELL.format(
        lang=lang, redirect=redirect, title=html.escape(title), desc=html.escape(desc),
        canon=page if en else "es/" + page, page=page,
        a="../assets/" if en else "../../assets/", p="" if en else "../",
        version=VERSION, vnum=VERSION.lstrip("v"), gh=GH,
        navlabel="Main" if en else "Principal", nav=site_nav(lang, current), switch=switch,
        installbtn="Install" if en else "Instalar", menulabel="Open menu" if en else "Abrir menú",
        family=("Part of the <a href=\"https://reactvision.xyz\">ReactVision</a> family · MIT" if en else
                "Parte de la familia <a href=\"https://reactvision.xyz\">ReactVision</a> · MIT"),
        footlabel="Footer" if en else "Pie de página", book="Book" if en else "Libro",
        changelog="Changelog" if en else "Cambios", body=body,
    )


def build_packages():
    data = []
    for pkg in PACKAGES_CATALOG:
        text = fetch_readme(pkg["name"])
        data.append((pkg, readme_tagline(text, pkg["name"]),
                     readme_example(text, pkg["name"], pkg["section"])))
    for lang in ("en", "es"):
        T = PKG_TEXT[lang]
        p = "" if lang == "en" else "../"
        b = [f'    <div class="eyebrow">{T["eyebrow"]}</div>',
             f'    <h1>{T["h1"]}</h1>',
             f'    <p class="lede">{T["lede"]}</p>',
             f'    <nav class="chips" aria-label="{T["chips"]}">' +
             "".join(f'<a href="#{pkg["name"]}">{pkg["name"]}</a>' for pkg, _, _ in data) + "</nav>",
             f'    <h2 id="using">{T["how_h"]}</h2>',
             f'    <p>{T["how_p"]}</p>',
             "    <pre><code>" + "\n".join(html.escape(c) for c in T["how_cmd"]) + "</code></pre>",
             f'    <p>{T["how_p2"].format(p=p)}</p>']
        for pkg, tagline, example in data:
            n = pkg["name"]
            repo = f"https://github.com/{PKG_ORG}/{n}"
            install = pkg["setup"] + [f"eskiuc add {n} --git {repo} --rev {PKG_REV}"] + pkg["build"]
            b += [f'    <section class="pkg" id="{n}">',
                  f'      <h2>{n}</h2>',
                  '      <div class="pkg-meta">'
                  f'<span class="tag">{PKG_REV}</span>'
                  f'<a href="{repo}/actions/workflows/ci.yml"><img src="{repo}/actions/workflows/ci.yml/badge.svg" '
                  f'alt="{T["ci"]}: {n}" width="90" height="20" loading="lazy" /></a>'
                  f'<a href="{repo}">github.com/{PKG_ORG}/{n}</a></div>',
                  f'      <p><strong>{html.escape(tagline if lang == "en" else pkg["tag_es"])}</strong></p>',
                  f'      <p>{pkg["note_" + lang]}</p>',
                  f'      <p class="note">{pkg["needs_" + lang]}</p>',
                  f'      <h3>{T["install"]}</h3>',
                  "      <pre><code>" + "\n".join(html.escape(c, quote=False) for c in install) + "</code></pre>",
                  f'      <h3>{T["example"]}</h3>',
                  "      <pre><code>" + highlight_comments(example) + "</code></pre>",
                  "    </section>"]
        b.append(f'    <p>{T["outro"]}</p>')
        page = render_site_page(lang, "packages.html", "packages", T["title"], T["desc"], "\n".join(b))
        out = os.path.join(ROOT, "site", "packages.html" if lang == "en" else "es/packages.html")
        with open(out, "w", encoding="utf-8") as f:
            f.write(page)
        print(f"  READMEs at {PKG_REV}  ->  site/{os.path.relpath(out, os.path.join(ROOT, 'site'))}")


def main():
    if "--packages" in sys.argv:
        build_packages()
        return
    os.makedirs(OUT, exist_ok=True)
    md = markdown.Markdown(
        extensions=["extra", "sane_lists", "toc"],
        output_format="html5",
    )
    for src, out, title, section in PAGES:
        srcdir = os.path.dirname(src)
        with open(os.path.join(ROOT, src), encoding="utf-8") as f:
            text = f.read()
        md.reset()
        body = md.convert(text)
        body = re.sub(r'href="([^"]+)"', make_rewriter(srcdir), body)
        body = body.replace("<table>", '<div class="tablewrap"><table>').replace(
            "</table>", "</table></div>"
        )
        page = render(
            title=f"{title} · Eskiu docs",
            desc=first_paragraph(text),
            canon=f"docs/{out}",
            p="../", d="", a="../../assets/",
            current="docs", active=out,
            summary=f"{section_of(out)} · {title}",
            body=body,
            toc_tokens=md.toc_tokens,
        )
        with open(os.path.join(OUT, out), "w", encoding="utf-8") as f:
            f.write(page)
        print(f"  {src}  ->  site/docs/{out}")

    # Top-level: mirror the CHANGELOG so the site never links the (private) GitHub.
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        cl_text = f.read()
    md.reset()
    cl_body = md.convert(collapse_changelog(cl_text))
    cl_body = cl_body.replace("<table>", '<div class="tablewrap"><table>').replace(
        "</table>", "</table></div>"
    )
    cl_page = render(
        title="Changelog · Eskiu",
        desc=first_paragraph(cl_text),
        canon="changelog.html",
        p="", d="docs/", a="../assets/",
        current="changelog", active=None,
        summary="Documentation",
        body=cl_body,
        toc_tokens=md.toc_tokens,
    )
    with open(os.path.join(ROOT, "site", "changelog.html"), "w", encoding="utf-8") as f:
        f.write(cl_page)
    print("  CHANGELOG.md  ->  site/changelog.html")

    if "--no-packages" not in sys.argv:
        build_packages()

    print(f"Done. {len(PAGES)} docs pages + changelog.html")


if __name__ == "__main__":
    main()
