#!/usr/bin/env python3
"""
Generate the site's docs pages from the Markdown sources in docs/.

The repo is private, so the public can't read the docs on GitHub. This renders
the language docs (docs/lang/*) and the contributor docs (docs/dev/*) into self-contained, styled HTML
under site/docs/, in the same Editorial look as the homepage (site/index.html).

Usage:  python3 site/build-docs.py        (run from anywhere)

Requires: markdown  (pip install --user markdown)
Re-run whenever the Markdown sources change, then commit site/docs/.
"""

import html
import os
import re

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


def main():
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

    print(f"Done. {len(PAGES)} docs pages + changelog.html")


if __name__ == "__main__":
    main()
