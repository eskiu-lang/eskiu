/* Keyboard, pointer and touch navigation shared by every website page. */
(function () {
  var top = document.querySelector('.top');
  if (!top) return;
  top.classList.add('nav-js');
  var groups = Array.from(top.querySelectorAll('.nav-g'));
  var menu = top.querySelector('.menu-btn');
  function closeGroups(except) {
    groups.forEach(function (g) {
      if (g !== except) { g.classList.remove('is-open'); g.querySelector('button').setAttribute('aria-expanded', 'false'); }
    });
  }
  function openGroup(g) {
    closeGroups(g); g.classList.add('is-open'); g.querySelector('button').setAttribute('aria-expanded', 'true');
  }
  groups.forEach(function (g) {
    var b = g.querySelector('button');
    b.addEventListener('click', function () {
      if (g.classList.contains('is-open')) closeGroups(); else openGroup(g);
    });
    b.addEventListener('keydown', function (e) {
      if (e.key === 'ArrowDown') { e.preventDefault(); openGroup(g); g.querySelector('a').focus(); }
    });
    g.addEventListener('pointerenter', function (e) { if (e.pointerType !== 'touch') openGroup(g); });
    g.addEventListener('pointerleave', function () { if (!g.contains(document.activeElement)) closeGroups(); });
    g.addEventListener('focusout', function (e) { if (!g.contains(e.relatedTarget)) closeGroups(); });
  });
  function closeDrawer() {
    top.classList.remove('open');
    if (menu) {
      menu.setAttribute('aria-expanded', 'false');
      menu.setAttribute('aria-label', document.documentElement.lang === 'es' ? 'Abrir menú' : 'Open menu');
    }
  }
  if (menu) menu.addEventListener('click', function () {
    var open = top.classList.toggle('open');
    menu.setAttribute('aria-expanded', String(open));
    menu.setAttribute('aria-label', document.documentElement.lang === 'es' ? (open ? 'Cerrar menú' : 'Abrir menú') : (open ? 'Close menu' : 'Open menu'));
  });
  top.querySelectorAll('.drawer a').forEach(function (a) { a.addEventListener('click', closeDrawer); });
  document.addEventListener('click', function (e) { if (!top.contains(e.target)) { closeGroups(); closeDrawer(); } });
  document.addEventListener('keydown', function (e) {
    if (e.key !== 'Escape') return;
    var g = groups.find(function (g) { return g.classList.contains('is-open'); });
    if (g) { g.querySelector('button').focus(); closeGroups(); }
    if (top.classList.contains('open')) { closeDrawer(); if (menu) menu.focus(); }
  });
})();
