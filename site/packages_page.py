"""packages_page.py: the package catalog pages, site/packages.html and site/es/packages.html
(used by build-docs.py)."""

import html
import os
import re

from docs_config import ROOT, VERSION, GH
from packages_catalog import PKG_ORG, PKG_REV, PACKAGES_CATALOG


def fetch_readme(name, rev):
    import urllib.request
    url = f"https://raw.githubusercontent.com/{PKG_ORG}/{name}/{rev}/README.md"
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode("utf-8")


INDEX_GIT = "https://github.com/eskiu-lang/index"


def fetch_index():
    """The index's entries (eskiu-lang/index, packages/NAME.toml), each with its latest vX.Y.Z tag."""
    import subprocess
    import tempfile
    import tomllib
    entries = []
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "clone", "--quiet", "--depth", "1", INDEX_GIT, tmp], check=True, timeout=120)
        folder = os.path.join(tmp, "packages")
        for file in sorted(os.listdir(folder)):
            if file.endswith(".toml"):
                with open(os.path.join(folder, file), "rb") as f:
                    entries.append(tomllib.load(f))
    return entries


def latest_tag(url):
    import subprocess
    out = subprocess.run(["git", "ls-remote", "--tags", "--refs", url], capture_output=True, text=True,
                         timeout=120)
    tags = []
    for line in out.stdout.splitlines():
        m = re.match(r"^v(\d+)\.(\d+)\.(\d+)$", line.split("\t")[-1].removeprefix("refs/tags/"))
        if m:
            tags.append((tuple(int(x) for x in m.groups()), m.group(0)))
    return max(tags)[1] if tags else None


def readme_tagline(text, name):
    m = re.search(r'<h2 align="center">[^<]*</h2>\s*<p align="center">(.*?)</p>', text, re.S)
    if not m:
        raise SystemExit(f"packages: no tagline in the {name} README")
    return html.unescape(re.sub(r"<[^>]+>", "", m.group(1))).strip()


def readme_example(text, name, section):
    """The first eskiu code block under `section`, or in the whole README when None."""
    start = 0
    if section is not None:
        m = re.search(r"(?m)^#{2,3} " + re.escape(section) + r"\s*$", text)
        if not m:
            raise SystemExit(f"packages: no '{section}' section in the {name} README")
        start = m.end()
    b = re.search(r"(?ms)^```eskiu\n(.*?)^```", text[start:])
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
        "lang": "en", "title": "Packages · Eskiu", "eyebrow": "Packages", "h1": "Packages",
        "desc": "Eskiu packages: gpu (compute, rendering, glTF and OBJ models, Llama "
                "inference and training over WebGPU), onnx, tflite, compress, audio, font, jni, quickjs and nds, "
                "each added with one eskiuc add.",
        "lede": "The packages the project maintains, in the "
                "<a href=\"https://github.com/eskiu-packages\">eskiu-packages</a> organization, and "
                "every package anyone has published in the "
                "<a href=\"https://github.com/eskiu-lang/index\">package index</a>. Each one is a git "
                "repository with an <code>eskiu.toml</code>, added to a project with one command.",
        "how_h": "Using a package",
        "how_p": "<code>eskiuc add</code> writes the dependency into <code>eskiu.toml</code>, fetches "
                 "it with git and records its commit in <code>eskiu.lock</code>, so later builds "
                 "reuse it without touching the network. A dependency is a git URL with a rev (a tag, "
                 "branch or commit) or a local path, from any repository; the index only helps people "
                 "find packages.",
        "how_cmd": ["eskiuc new myapp", "cd myapp",
                    "eskiuc add compress --git https://github.com/eskiu-packages/compress --rev v0.1.0",
                    "eskiuc run"],
        "how_p2": "A package's modules are imported by the package name and the file name, as in "
                  "<code>import &lt;gpu/model&gt;;</code>. Chapter 13 of the "
                  "<a href=\"{p}the-book-of-eskiu.html#sec-13-5\">Book</a> covers manifests, locks "
                  "and <code>eskiuc test</code>.",
        "chips": "Packages", "install": "Install", "example": "Example from the README",
        "soon_tag": "private for now",
        "vision_line": "Computer vision, bit for bit with C++.",
        "soon_p": "Corners, optical flow, multi-view geometry and planes, in pure Eskiu. It comes from "
                  "tinyvio, the tracker behind ViroReact's web AR, and computes the same bits as its "
                  "C++. The repository is private for now. "
                  "<a href=\"{p}vision.html\">What it does and how it was checked →</a>",
        "repo": "Repository", "ci": "CI status",
        "index_h": "From the index",
        "index_p": "Packages published by their own maintainers. They are not reviewed by the Eskiu "
                   "project: read a package before you depend on it.",
        "index_none": "None yet besides the ones above.",
        "publish_h": "Publishing a package",
        "publish_p": "Make a public git repository with an <code>eskiu.toml</code> (<code>eskiuc new "
                     "NAME</code> writes one), tag a release (<code>v0.1.0</code>), and open a pull request "
                     "on <a href=\"https://github.com/eskiu-lang/index\">eskiu-lang/index</a> that adds "
                     "<code>packages/NAME.toml</code> with its name, repository, a one-line description "
                     "and its license. Its README has the rules.",
        "outro": "Every package needs Eskiu 0.13 or newer. Its README has the full module list, "
                 "more examples and the details for each operating system.",
    },
    "es": {
        "lang": "es", "title": "Paquetes · Eskiu", "eyebrow": "Paquetes", "h1": "Paquetes",
        "desc": "Paquetes de Eskiu: gpu (cómputo, renderizado, modelos glTF y OBJ, "
                "inferencia y entrenamiento de Llama sobre WebGPU), onnx, tflite, compress, audio, "
                "font, jni, quickjs y nds, cada uno se agrega con un eskiuc add.",
        "lede": "Los paquetes que mantiene el proyecto, en la organización "
                "<a href=\"https://github.com/eskiu-packages\">eskiu-packages</a>, y cada paquete que "
                "alguien haya publicado en el "
                "<a href=\"https://github.com/eskiu-lang/index\">índice de paquetes</a>. Cada uno es un "
                "repositorio de git con un <code>eskiu.toml</code>, y se agrega a un proyecto con un solo "
                "comando.",
        "how_h": "Cómo usar un paquete",
        "how_p": "<code>eskiuc add</code> escribe la dependencia en <code>eskiu.toml</code>, la "
                 "descarga con git y guarda su commit en <code>eskiu.lock</code>, así que los builds "
                 "siguientes la reutilizan sin tocar la red. Una dependencia es una URL de git con un rev "
                 "(un tag, una rama o un commit) o una ruta local, de cualquier repositorio; el índice "
                 "solo ayuda a encontrar paquetes.",
        "how_cmd": ["eskiuc new myapp", "cd myapp",
                    "eskiuc add compress --git https://github.com/eskiu-packages/compress --rev v0.1.0",
                    "eskiuc run"],
        "how_p2": "Los módulos de un paquete se importan con el nombre del paquete y el del archivo, "
                  "por ejemplo <code>import &lt;gpu/model&gt;;</code>. El capítulo 13 del "
                  "<a href=\"{p}the-book-of-eskiu.html#sec-13-5\">libro</a> explica los manifiestos, "
                  "el lock y <code>eskiuc test</code>.",
        "chips": "Paquetes", "install": "Instalación", "example": "Ejemplo del README",
        "soon_tag": "privado por ahora",
        "vision_line": "Visión por computadora, bit por bit con C++.",
        "soon_p": "Esquinas, flujo óptico, geometría multivista y planos, en Eskiu puro. Sale de tinyvio, "
                  "el tracker detrás del AR web de ViroReact, y calcula los mismos bits que su C++. El "
                  "repositorio es privado por ahora. "
                  "<a href=\"vision.html\">Qué hace y cómo se verificó →</a>",
        "repo": "Repositorio", "ci": "Estado de CI",
        "index_h": "Del índice",
        "index_p": "Paquetes publicados por quienes los mantienen. El proyecto Eskiu no los revisa: "
                   "lee un paquete antes de depender de él.",
        "index_none": "Por ahora ninguno además de los de arriba.",
        "publish_h": "Publicar un paquete",
        "publish_p": "Crea un repositorio público de git con un <code>eskiu.toml</code> (<code>eskiuc "
                     "new NOMBRE</code> escribe uno), etiqueta un release (<code>v0.1.0</code>) y abre un "
                     "pull request en <a href=\"https://github.com/eskiu-lang/index\">eskiu-lang/index</a> "
                     "que agregue <code>packages/NOMBRE.toml</code> con su nombre, repositorio, una "
                     "descripción de una línea y su licencia. Su README tiene las reglas (en inglés).",
        "outro": "Todos los paquetes necesitan Eskiu 0.13 o más nuevo. En el README de cada uno "
                 "están la lista completa de módulos, más ejemplos y los detalles para cada sistema "
                 "operativo (en inglés).",
    },
}

# Header nav of the hand-written and generated top-level pages, per language:
# (href relative to the page's directory, label, key)
SITE_NAV = {
    "en": [("quickstart.html", "Get started", "learn"), ("index.html#playground", "Playground", "playground"),
           ("the-book-of-eskiu.html", "Book", "book"), ("docs/index.html", "Docs", "docs"),
           ("packages.html", "Packages", "packages"), ("platforms.html", "Platforms", "platforms"),
           ("showcase.html", "Showcase", "showcase"), (GH, "GitHub", "gh")],
    "es": [("quickstart.html", "Empieza", "learn"), ("index.html#playground", "Playground", "playground"),
           ("../the-book-of-eskiu.html", "Libro", "book"), ("../docs/index.html", "Docs", "docs"),
           ("packages.html", "Paquetes", "packages"), ("platforms.html", "Plataformas", "platforms"),
           ("showcase.html", "Proyectos", "showcase"), (GH, "GitHub", "gh")],
}

PAGE_SHELL = """<!doctype html>
<html lang="{lang}">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
{redirect}  <title>{title}</title>
  <meta name="description" content="{desc}" />
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
  <link rel="alternate" hreflang="en" href="https://eskiu-lang.org/{page}" />
  <link rel="alternate" hreflang="es" href="https://eskiu-lang.org/es/{page}" />
  <link rel="alternate" hreflang="x-default" href="https://eskiu-lang.org/{page}" />
  <link rel="icon" type="image/png" href="{a}logo.png" />
  <meta name="theme-color" content="#fbf8f3" />
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&amp;family=JetBrains+Mono:wght@400;500&amp;display=swap" rel="stylesheet" />
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
      <nav class="foot-links" aria-label="{footlabel}"><a href="{p}docs/index.html">Docs</a><a href="{p}the-book-of-eskiu.html">{book}</a><a href="{p}docs/internals.html">Internals</a><a href="{p}changelog.html">{changelog}</a><a href="{p}llms.txt">llms.txt</a><a href="{gh}">GitHub</a><a href="https://reactvision.xyz">ReactVision</a></nav>
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
        text = fetch_readme(pkg.get("repo", pkg["name"]), pkg.get("rev", PKG_REV))
        data.append((pkg, readme_tagline(text, pkg["name"]),
                     readme_example(text, pkg["name"], pkg["section"])))
    # The index's packages that have no section of their own here (anyone's).
    curated = {pkg["name"] for pkg in PACKAGES_CATALOG} | {"vision"}
    others = []
    for e in fetch_index():
        if e["name"] not in curated:
            others.append((e, latest_tag(e["git"])))
    for lang in ("en", "es"):
        T = PKG_TEXT[lang]
        p = "" if lang == "en" else "../"
        b = [f'    <div class="eyebrow">{T["eyebrow"]}</div>',
             f'    <h1>{T["h1"]}</h1>',
             f'    <p class="lede">{T["lede"]}</p>',
             # A card per package, to its section: the name, its line, and "ui" for the
             # packages of ui apps (their repositories are ui-NAME).
             f'    <nav class="pkg-grid" aria-label="{T["chips"]}">' +
             "".join(f'<a href="#{pkg["name"]}"><b>{pkg["name"]}</b>'
                     f'<span>{html.escape(tagline if lang == "en" else pkg["tag_es"])}</span>'
                     + (f'<em>ui</em>' if pkg.get("repo", "").startswith("ui-") or pkg.get("ui") else "") + '</a>'
                     for pkg, tagline, _ in data) +
             f'<a class="pkg-grid-soon" href="#vision"><b>vision</b><span>{T["vision_line"]}</span>'
             f'<em>{T["soon_tag"]}</em></a></nav>',
             f'    <h2 id="using">{T["how_h"]}</h2>',
             f'    <p>{T["how_p"]}</p>',
             "    <pre><code>" + "\n".join(html.escape(c) for c in T["how_cmd"]) + "</code></pre>",
             f'    <p>{T["how_p2"].format(p=p)}</p>']
        for pkg, tagline, example in data:
            n = pkg["name"]
            # The repository's name, when it is not the package's (ui-location holds location).
            repo = f"https://github.com/{PKG_ORG}/{pkg.get('repo', n)}"
            rev = pkg.get("rev", PKG_REV)
            install = pkg["setup"] + [f"eskiuc add {n} --git {repo} --rev {rev}"] + pkg["build"]
            b += [f'    <section class="pkg" id="{n}">',
                  f'      <h2>{n}</h2>',
                  '      <div class="pkg-meta">'
                  f'<span class="tag">{rev}</span>'
                  f'<a href="{repo}/actions/workflows/ci.yml"><img src="{repo}/actions/workflows/ci.yml/badge.svg" '
                  f'alt="{T["ci"]}: {n}" width="90" height="20" loading="lazy" /></a>'
                  f'<a href="{repo}">github.com/{PKG_ORG}/{pkg.get("repo", n)}</a></div>',
                  f'      <p><strong>{html.escape(tagline if lang == "en" else pkg["tag_es"])}</strong></p>',
                  f'      <p>{pkg["note_" + lang]}</p>',
                  f'      <p class="note">{pkg["needs_" + lang]}</p>',
                  f'      <h3>{T["install"]}</h3>',
                  "      <pre><code>" + "\n".join(html.escape(c, quote=False) for c in install) + "</code></pre>",
                  f'      <details class="pkg-example"><summary>{T["example"]}</summary>',
                  "      <pre><code>" + highlight_comments(example) + "</code></pre>",
                  "      </details>",
                  "    </section>"]
        b += [f'    <section class="pkg pkg-soon" id="vision">',
              '      <h2>vision</h2>',
              f'      <div class="pkg-meta"><span class="tag">{T["soon_tag"]}</span></div>',
              f'      <p>{T["soon_p"].format(p=p)}</p>',
              "    </section>"]
        b.append(f'    <h2 id="index">{T["index_h"]}</h2>')
        if others:
            b.append(f'    <p>{T["index_p"]}</p>')
            for e, tag in others:
                cmd = f'eskiuc add {e["name"]} --git {e["git"]}' + (f" --rev {tag}" if tag else "")
                who = ", ".join(e.get("maintainers", []))
                b += [f'    <section class="pkg" id="{html.escape(e["name"])}">',
                      f'      <h3>{html.escape(e["name"])}</h3>',
                      '      <div class="pkg-meta">' + (f'<span class="tag">{html.escape(tag)}</span>' if tag else "")
                      + f'<a href="{html.escape(e["git"])}">{html.escape(e["git"].removeprefix("https://"))}</a>'
                      + (f'<span>{html.escape(who)}</span>' if who else "")
                      + f'<span>{html.escape(e["license"])}</span></div>',
                      f'      <p>{html.escape(e["description"])}</p>',
                      "      <pre><code>" + html.escape(cmd, quote=False) + "</code></pre>",
                      "    </section>"]
        else:
            b.append(f'    <p>{T["index_none"]}</p>')
        b += [f'    <h2 id="publish">{T["publish_h"]}</h2>', f'    <p>{T["publish_p"]}</p>']
        b.append(f'    <p>{T["outro"]}</p>')
        page = render_site_page(lang, "packages.html", "packages", T["title"], T["desc"], "\n".join(b))
        out = os.path.join(ROOT, "site", "packages.html" if lang == "en" else "es/packages.html")
        with open(out, "w", encoding="utf-8") as f:
            f.write(page)
        print(f"  READMEs at their releases  ->  site/{os.path.relpath(out, os.path.join(ROOT, 'site'))}")
