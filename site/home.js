/* eskiu-lang.org homepage script. Shared by index.html and es/index.html. */
const PLAYGROUND_API = "https://eskiu-playground-4cb1.orvex.cloud";
      const EXAMPLES = {
        hello: `import <io>;

int add(int a, int b) {
    return a + b;
}

int main() {
    println("Hello from Eskiu!");
    printf("2 + 3 = %d\\n", add(2, 3));
    return 0;
}
`,
        defer: `import <io>;
import <libc>;

int main() {
    {
        *int value = alloc<int>(1);
        defer { free(value); println("Resource released."); }
        value[0] = 42;
        printf("value = %d\\n", value[0]);
    }
    println("Block finished.");
    return 0;
}
`,
        basics: `import <io>;

// A counter that remembers its state across calls (static local).
int next_id() {
    static int id = 0;
    id++;                        // increment operator
    return id;
}

int main() {
    // Array-literal init; a short list zero-fills the rest.
    int[5] fib = {0, 1, 1, 2, 3};

    // do/while runs the body at least once.
    int i = 0;
    do {
        int even = fib[i] % 2 == 0 ? 1 : 0;      // ternary
        printf("fib[%d]=%d (%s)\\n", i, fib[i], even ? "even" : "odd");
        i++;
    } while (i < 5);

    // Multidimensional array in C order: rows of columns.
    int[2][3] grid = { {1, 2, 3}, {4, 5, 6} };
    printf("grid[1][2] = %d\\n", grid[1][2]);     // 6

    printf("ids: %d %d %d\\n", next_id(), next_id(), next_id());  // 1 2 3
    return 0;
}
`,
        match: `import <io>;
import <libc>;   // atoi

enum ParseResult {
    Port(int),
    InvalidRange(int),
    NotANumber,
}

ParseResult parse_port(string s) {
    let n: int = atoi(s);
    if (n == 0) { return NotANumber; }
    if (n < 1 || n > 65535) { return InvalidRange(n); }
    return Port(n);
}

int main() {
    match parse_port("8080") {
        Port(p)         -> printf("Listening on :%d\\n", p);
        InvalidRange(n) -> printf("%d out of range\\n", n);
        NotANumber      -> printf("not a number\\n");
    }
    return 0;
}
`,
        generics: `import <io>;

interface Ord { int cmp(Ord* o); }

struct Coin {
    int value;
    int cmp(Coin* o) { if (self.value < o.value) { return -1; } return 1; }
}

T max<T: Ord>(T* a, T* b) {
    if (a.cmp(b) > 0) { return a[0]; }
    return b[0];
}

int main() {
    let x: Coin = Coin{value: 7};
    let y: Coin = Coin{value: 3};
    let hi: Coin = max<Coin>(&x, &y);
    printf("max = %d\\n", hi.value);   // max = 7
    return 0;
}
`,
        operators: `import <io>;

struct V3 { float x; float y; float z; }

V3 operator +(V3 a, V3 b) {
    return V3{a.x + b.x, a.y + b.y, a.z + b.z};
}

V3 operator *(V3 v, float s) {
    return V3{v.x * s, v.y * s, v.z * s};
}

bool operator ==(V3 a, V3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

int main() {
    let a: V3 = V3{1.0, 2.0, 3.0};
    let b: V3 = V3{3.0, 4.0, 5.0};

    let mid: V3 = (a + b) * 0.5;                 // + and * resolve to the overloads
    printf("mid = %.0f %.0f %.0f\\n", (double)mid.x, (double)mid.y, (double)mid.z);

    printf("mid == a? %d\\n", mid == a);          // 0
    return 0;
}
`,
        async: `import <io>;
import <future>;
import <channel>;

async int sum_squares(Chan<int>* ch) {
    let a: int = await Chan_recv(ch);
    let b: int = await Chan_recv(ch);
    let c: int = await Chan_recv(ch);
    return a + b + c;
}

int main() {
    Chan<int>* ch = chan_new<int>(8);
    for (i in 1..4) { Chan_send(ch, i * i); }   // 1, 4, 9
    Future<int>* f = sum_squares(ch);
    Future_poll<int>(f, void() {});
    printf("sum of squares = %d\\n", f.value);   // 14
    Future_free<int>(f);  Chan_free<int>(ch);
    return 0;
}
`,
      };

/* ── UI: menu, copy, playground ─────────────────────────────── */
(function () {
  var ES = document.documentElement.lang === "es";
  var T = ES
    ? { empty: "No hay nada que ejecutar. Escribe código primero.", running: "Ejecutando", compiling: "Compilando y ejecutando…", noout: "(sin salida)", timeout: "tiempo agotado", trunc: "salida truncada", fail: "La solicitud falló.", offline: "No se pudo contactar al servidor del playground.", hint: "Revisa tu conexión o ejecútalo localmente; consulta el inicio rápido.", run: "Ejecutar", copied: "Copiado", idle: "La salida aparece aquí. Presiona Ejecutar (o ⌘/Ctrl + Enter)." }
    : { empty: "Nothing to run. Write some code first.", running: "Running", compiling: "Compiling &amp; running…", noout: "(no output)", timeout: "timed out", trunc: "output truncated", fail: "Request failed.", offline: "Could not reach the playground server.", hint: "Check your connection or run locally; see the quickstart.", run: "Run", copied: "Copied", idle: "Output appears here. Press Run (or ⌘/Ctrl + Enter)." };

  /* Copy buttons */
  // The install box's tabs: the command for each system, Windows first on Windows.
  document.querySelectorAll("[data-os-tabs]").forEach(function (tabs) {
    var box = document.getElementById("install");
    if (!box) return;
    var buttons = tabs.querySelectorAll("button");
    function pick(t) {
      buttons.forEach(function (o) {
        o.setAttribute("aria-selected", o === t ? "true" : "false");
        o.tabIndex = o === t ? 0 : -1;
      });
      box.setAttribute("aria-labelledby", t.id);
      box.querySelector(".cmd").textContent = t.getAttribute("data-cmd");
      box.querySelector(".lbl").textContent = t.getAttribute("data-lbl");
      box.querySelector("[data-copy]").setAttribute("data-copy", t.getAttribute("data-cmd"));
    }
    buttons.forEach(function (t, i) {
      t.id = "install-os-" + i;
      t.setAttribute("aria-controls", box.id);
      t.tabIndex = i === 0 ? 0 : -1;
      t.addEventListener("click", function () { pick(t); });
      t.addEventListener("keydown", function (e) {
        var next;
        if (e.key === "ArrowRight") next = (i + 1) % buttons.length;
        else if (e.key === "ArrowLeft") next = (i + buttons.length - 1) % buttons.length;
        else if (e.key === "Home") next = 0;
        else if (e.key === "End") next = buttons.length - 1;
        else return;
        e.preventDefault(); pick(buttons[next]); buttons[next].focus();
      });
    });
    box.setAttribute("role", "tabpanel");
    box.setAttribute("aria-labelledby", buttons[0].id);
    if (/Windows/.test(navigator.userAgent) && buttons.length > 1) pick(buttons[1]);
  });

  document.querySelectorAll("[data-copy]").forEach(function (b) {
    b.addEventListener("click", function () {
      var label = b.textContent;
      function failed() {
        b.textContent = ES ? "Copia el comando" : "Select and copy";
        var selection = window.getSelection();
        var range = document.createRange();
        range.selectNodeContents(document.querySelector(".install-box .cmd"));
        selection.removeAllRanges(); selection.addRange(range);
        setTimeout(function () { b.textContent = label; }, 2400);
      }
      if (!navigator.clipboard) { failed(); return; }
      navigator.clipboard.writeText(b.getAttribute("data-copy")).then(function () {
        b.textContent = T.copied;
        setTimeout(function () { b.textContent = label; }, 1600);
      }, failed);
    });
  });

  /* Playground */
  var ta = document.getElementById("pg-code");
  var res = document.getElementById("pg-res");
  var btn = document.getElementById("pg-run");
  if (!ta || !res || !btn) return;
  var exButtons = Array.prototype.slice.call(document.querySelectorAll(".pg-ex button"));

  function esc(s) { return s.replace(/[&<>]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]; }); }
  function idle() { res.innerHTML = '<span class="hint">' + T.idle + "</span>"; }
  function load(name) {
    ta.value = EXAMPLES[name];
    exButtons.forEach(function (b) { b.setAttribute("aria-pressed", b.getAttribute("data-ex") === name ? "true" : "false"); });
    idle();
  }
  var runLabel = btn.innerHTML;

  async function run() {
    if (btn.disabled) return;
    var code = ta.value;
    if (!code.trim()) { res.innerHTML = '<span class="err">' + T.empty + "</span>"; return; }
    btn.disabled = true;
    btn.innerHTML = '<span class="spin"></span> ' + T.running;
    res.innerHTML = '<span class="meta">$ eskiuc run main.esk</span>\n<span class="meta">' + T.compiling + "</span>";
    var controller = new AbortController();
    var timeout = setTimeout(function () { controller.abort(); }, 30000);
    res.setAttribute("aria-busy", "true");
    try {
      var r = await fetch(PLAYGROUND_API + "/run", { method: "POST", headers: { "Content-Type": "application/json" }, signal: controller.signal, body: JSON.stringify({ code: code }) });
      var data = await r.json();
      if (!data.ok) { res.innerHTML = '<span class="err">' + esc(data.error || T.fail) + "</span>"; return; }
      var html = '<span class="meta">$ eskiuc run main.esk</span>\n';
      if (data.stdout) html += '<span class="res">' + esc(data.stdout) + "</span>";
      if (data.stderr) html += '<span class="err">' + esc(data.stderr) + "</span>";
      if (!data.stdout && !data.stderr) html += '<span class="meta">' + T.noout + "</span>";
      var parts = [];
      if (data.timed_out) parts.push('<span class="err">' + T.timeout + "</span>");
      else if (data.exit_code === 0) parts.push('<span class="ok">exit 0</span>');
      else parts.push('<span class="err">exit ' + data.exit_code + "</span>");
      if (typeof data.duration_ms === "number") parts.push(data.duration_ms + "ms");
      if (data.truncated) parts.push(T.trunc);
      html += '\n\n<span class="meta">' + parts.join(" / ") + "</span>";
      res.innerHTML = html;
    } catch (e) {
      res.innerHTML = '<span class="err">' + T.offline + '</span>\n<span class="hint">' + T.hint + '</span>\n<a href="quickstart.html">' + (ES ? "Inicio rápido" : "Quickstart") + '</a>';
    } finally {
      clearTimeout(timeout);
      res.setAttribute("aria-busy", "false");
      btn.disabled = false;
      btn.innerHTML = runLabel;
    }
  }

  exButtons.forEach(function (b) { b.addEventListener("click", function () { load(b.getAttribute("data-ex")); }); });
  btn.addEventListener("click", run);
  ta.addEventListener("keydown", function (e) {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); run(); }
  });
  var requested = new URLSearchParams(window.location.search).get("example");
  load(Object.prototype.hasOwnProperty.call(EXAMPLES, requested) ? requested : "hello");
})();
