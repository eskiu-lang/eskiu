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

Requires: markdown. Install it in a virtual environment and run the script with
its Python (a system Python may refuse pip installs):
        python3 -m venv ~/.venvs/eskiu-site
        ~/.venvs/eskiu-site/bin/pip install markdown
        ~/.venvs/eskiu-site/bin/python site/build-docs.py
Re-run whenever the Markdown sources change, then commit site/docs/.
"""

import hashlib
import html
import os
import re
import sys

try:
    import markdown
except ImportError:
    sys.exit("build-docs.py needs Python's markdown module: create a virtual environment\n"
             "(python3 -m venv ~/.venvs/eskiu-site), install it there\n"
             "(~/.venvs/eskiu-site/bin/pip install markdown) and run this script with that\n"
             "environment's python.")

from docs_config import ROOT, VERSION, GH
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
    ("internals.html", "Internals"),
]
NAV_DEV = [
    ("index.html", "Language docs"),
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

# VERSION (bumped per release) and GH are in docs_config.py.

from docs_theme import CSS, SHELL

# Sidebar groups (desktop aside and mobile <details>). Hrefs are relative to site/docs/.
SIDEBAR = [
    ("Language", [(h, l) for h, l in NAV_LANG if h != "internals.html"]),
    ("Compiler internals", [(h, l) for h, l in NAV_DEV if h != "index.html"]),
]

# Header nav: (href relative to site/, label, key used to mark the current section)
TOP_LINKS = [
    ("quickstart.html", "Get started", "learn"),
    ("index.html#playground", "Playground", "playground"),
    ("the-book-of-eskiu.html", "Book", "book"),
    ("docs/index.html", "Docs", "docs"),
    ("packages.html", "Packages", "packages"),
    ("platforms.html", "Platforms", "platforms"),
    ("showcase.html", "Showcase", "showcase"),
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
        if re.fullmatch(r"-{3,}", rest):
            # A section with no entries yet (a fresh [Unreleased]): just its rule.
            block += rest + "\n\n"
            rest = ""
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


def make_rewriter(srcdir, prefix=""):
    """Return a regex callback that rewrites .md hrefs relative to srcdir; prefix is the
    path from the output page to site/docs/."""

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
            return f'href="{prefix}{PATHMAP[resolved]}{anchor}"'
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


from packages_page import build_packages
from site_nav import stamp_assets


def main():
    if "--stamp" in sys.argv:
        stamp_assets()
        return
    if "--packages" in sys.argv:
        build_packages()
        stamp_assets()
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
            title=f"{title} / Eskiu docs",
            desc=first_paragraph(text),
            canon=f"docs/{out}",
            p="../", d="", a="../../assets/",
            current="docs", active=out,
            summary=f"{section_of(out)} / {title}",
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
    cl_body = re.sub(r'href="([^"]+)"', make_rewriter("", "docs/"), cl_body)
    cl_body = cl_body.replace("<table>", '<div class="tablewrap"><table>').replace(
        "</table>", "</table></div>"
    )
    cl_page = render(
        title="Changelog / Eskiu",
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
    stamp_assets()

    print(f"Done. {len(PAGES)} docs pages + changelog.html")


if __name__ == "__main__":
    main()
