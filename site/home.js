/* eskiu-lang.org homepage script. Shared by index.html and es/index.html. */

/* ── UI: install tabs, copy ─────────────────────────────────── */
(function () {
  var ES = document.documentElement.lang === "es";
  var T = ES ? { copied: "Copiado" } : { copied: "Copied" };

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
})();
