"""site_nav.py: the shared navigation, call-to-action bands and asset stamps of every page
(used by build-docs.py)."""

import hashlib
import html
import os
import re

from docs_config import ROOT, GH

# Every page names its stylesheets and scripts with ?v=<hash of the file>, so a browser
# that cached an old copy fetches the new one when the file changes, and only then.
ASSETS = ("page.css", "home.css", "home.js")


# The main navigation of every page, defined once: apply_nav rewrites each page's header
# nav (grouped menus) and mobile drawer from it. A target is relative to site/; "@" marks a
# page that exists only in English (the Spanish pages link to it with "../").
NAV = [
    ("language", "Language", "Lenguaje", [
        ("index.html#language", "The language", "Memory, types, errors and C interop",
         "El lenguaje", "Memoria, tipos, errores e interoperabilidad con C"),
        ("index.html#compare", "How it compares", "The choices behind Eskiu",
         "Cómo se compara", "Las decisiones de Eskiu"),
        ("index.html#status", "Stability and maturity", "What to expect before 1.0",
         "Estabilidad y madurez", "Qué esperar antes de 1.0"),
    ]),
    ("learn", "Learn", "Aprende", [
        ("quickstart.html", "Get started", "Install it and run a first program",
         "Empieza", "Instálalo y corre un primer programa"),
        ("index.html#playground", "Playground", "Run code in the browser",
         "Playground", "Corre código en el navegador"),
        ("@the-book-of-eskiu.html", "The Book of Eskiu", "The language, chapter by chapter",
         "El libro de Eskiu", "El lenguaje, capítulo por capítulo"),
        ("agents.html", "For agents", "Install, drive and test Eskiu from an agent",
         "Para agentes", "Instala, maneja y prueba Eskiu desde un agente"),
    ]),
    ("docs", "Documentation", "Documentación", [
        ("@docs/index.html", "Reference", "The language, standard library and compiler",
         "Referencia", "El lenguaje, la biblioteca estándar y el compilador"),
        ("platforms.html", "Platforms", "Targets, toolchains and requirements",
         "Plataformas", "Destinos de compilación, herramientas y requisitos"),
        ("@docs/stability.html", "Stability policy", "Compatibility between releases",
         "Política de estabilidad", "Compatibilidad entre versiones"),
        ("@changelog.html", "Release notes", "What changed in each version",
         "Notas de versión", "Qué cambió en cada versión"),
    ]),
    ("ecosystem", "Ecosystem", "Ecosistema", [
        ("packages.html", "Packages", "Libraries that extend the language",
         "Paquetes", "Bibliotecas que amplían el lenguaje"),
        ("showcase.html", "Built with Eskiu", "Projects and case studies",
         "Hecho con Eskiu", "Proyectos y casos de uso"),
    ]),
]
NAV_CURRENT = {"quickstart.html": "learn", "agents.html": "learn", "docs/": "docs",
               "the-book-of-eskiu.html": "learn", "changelog.html": "docs",
               "platforms.html": "docs", "packages.html": "ecosystem",
               "vision.html": "ecosystem", "gpu.html": "ecosystem", "showcase.html": "ecosystem",
               "case-study-reactvision.html": "ecosystem", "case-study-3ds.html": "ecosystem",
               "case-study-identity.html": "ecosystem", "ui.html": "ecosystem",
               "ui-components.html": "ecosystem"}
CHEVRON = ('<svg viewBox="0 0 10 10" width="10" height="10" fill="none" stroke="currentColor" '
           'stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
           '<path d="M2 3.5 5 6.5 8 3.5"/></svg>')


def nav_html(rel):
    """The header nav and the drawer for the page at site/<rel>."""
    es = rel.startswith("es/")
    up = "../" if es or rel.startswith("docs/") else ""
    name = os.path.basename(rel)
    home = name == "index.html" and "/" not in rel.replace("es/", "", 1)
    cur_group = NAV_CURRENT.get("docs/" if rel.startswith("docs/") else name)

    def href(target):
        en_only = target.startswith("@")
        t = target.lstrip("@")
        if home and t.startswith("index.html#"):
            return t[len("index.html"):]
        if rel.startswith("docs/") or (es and en_only):
            return "../" + t
        return t

    nav, drawer = [], []
    for key, en_label, es_label, items in NAV:
        label = es_label if es else en_label
        links = []
        for target, en_t, en_d, es_t, es_d in items:
            title, desc = (es_t, es_d) if es else (en_t, en_d)
            here = target.lstrip("@") == ("docs/index.html" if rel == "docs/index.html" else
                                          (name if not rel.startswith("docs/") else ""))
            cur = ' aria-current="page"' if here else ""
            links.append((href(target), html.escape(title), html.escape(desc), cur))
        gcls = "nav-g cur" if key == cur_group else "nav-g"
        nav.append(f'<div class="{gcls}"><button type="button" class="nav-t">{label}{CHEVRON}</button>'
                   '<div class="nav-m">' +
                   "".join(f'<a href="{h}"{c}><b>{t}</b><span>{d}</span></a>' for h, t, d, c in links) +
                   "</div></div>")
        drawer.append(f'<div class="dr-g"><span class="dr-h">{label}</span>' +
                      "".join(f'<a href="{h}"{c}>{t}</a>' for h, t, d, c in links) + "</div>")
    nav.append(f'<a class="nav-t" href="{GH}">GitHub</a>')
    drawer.append(f'<a class="dr-gh" href="{GH}">GitHub</a>')
    return "".join(nav), "".join(drawer)


NAV_RE = re.compile(r'(<nav class="(nav|drawer)"[^>]*>)(.*?)(</nav>)', re.S)


def apply_nav():
    site = os.path.join(ROOT, "site")
    for dirpath, _, files in os.walk(site):
        for fn in files:
            if not fn.endswith(".html"):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, site).replace(os.sep, "/")
            with open(path, encoding="utf-8") as f:
                text = f.read()
            if '<nav class="nav"' not in text:
                continue
            nav, drawer = nav_html(rel)
            new = NAV_RE.sub(lambda m: m.group(1) + (nav if m.group(2) == "nav" else drawer) + m.group(4), text)
            if new != text:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(new)


# The closing call to action of the article pages, between the article and the footer.
CTA_PAGES = ("showcase.html", "platforms.html", "packages.html", "vision.html",
             "case-study-reactvision.html", "case-study-3ds.html", "case-study-identity.html")
CTA_BAND = {
    "en": """  <section class="cta-band" aria-label="Get started">
    <div class="bar cta-in">
      <div class="cta-txt">
        <h2>Start in a minute.</h2>
        <p>Install the compiler with one command, or run a program in the browser first.</p>
      </div>
      <div class="cta-btns">
        <a class="cta-dark" href="quickstart.html">Get started</a>
        <a class="cta-line" href="index.html#playground">Try it in the browser</a>
      </div>
    </div>
  </section>
""",
    "es": """  <section class="cta-band" aria-label="Empieza">
    <div class="bar cta-in">
      <div class="cta-txt">
        <h2>Empieza en un minuto.</h2>
        <p>Instala el compilador con un comando, o corre un programa en el navegador primero.</p>
      </div>
      <div class="cta-btns">
        <a class="cta-dark" href="quickstart.html">Empieza</a>
        <a class="cta-line" href="index.html#playground">Pruébalo en el navegador</a>
      </div>
    </div>
  </section>
""",
}


def add_cta_bands():
    site = os.path.join(ROOT, "site")
    for lang, sub in (("en", ""), ("es", "es")):
        for name in CTA_PAGES:
            path = os.path.join(site, sub, name)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            if 'class="cta-band"' in text or '<footer class="foot">' not in text:
                continue
            text = text.replace('<footer class="foot">', CTA_BAND[lang].lstrip() + '  <footer class="foot">', 1)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)


def stamp_assets():
    add_cta_bands()
    apply_nav()
    site = os.path.join(ROOT, "site")
    stamps = {}
    for name in ASSETS:
        with open(os.path.join(site, name), "rb") as f:
            stamps[name] = hashlib.sha256(f.read()).hexdigest()[:10]
    pattern = re.compile(r'((?:href|src)="(?:\.\./)*)(' + "|".join(re.escape(n) for n in ASSETS) +
                         r')(?:\?v=[0-9a-f]*)?"')
    for dirpath, _, files in os.walk(site):
        for fn in files:
            if not fn.endswith(".html"):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            new = pattern.sub(lambda m: f'{m.group(1)}{m.group(2)}?v={stamps[m.group(2)]}"', text)
            if new != text:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(new)
