// Shared theme toggle for static desktop prototypes. The interactive shell
// uses the same dedicated preference key without rewriting document state.
// Icon = the theme you are in; accessible name and tooltip = the theme a click
// switches to. Each page keeps its own theme mechanism: data-theme-toggle is a
// selector for the element that carries the .dark class.
(() => {
  'use strict';
  const SUN = '<circle cx="12" cy="12" r="3.5"></circle><path d="M12 2.5v2M12 19.5v2M4.6 4.6 6 6M18 18l1.4 1.4M2.5 12h2M19.5 12h2M4.6 19.4 6 18M18 6l1.4-1.4"></path>';
  const MOON = '<path d="M20 15.2A8.2 8.2 0 0 1 8.8 4a8.4 8.4 0 1 0 11.2 11.2Z"></path>';
  // Key = the theme a click switches to.
  const NAME = { dark: '切换深色主题', light: '切换浅色主题' };
  const KEY = 'booklight.prototype.theme';
  let fallback = null;
  const media = window.matchMedia('(prefers-color-scheme: dark)');
  function mode() {
    if (fallback) return fallback;
    try { return localStorage.getItem(KEY); } catch { return null; }
  }
  function darkMode() { const m = mode(); return m === 'dark' || (m !== 'light' && media.matches); }
  const targets = new Set();
  function apply() { targets.forEach(t => t.classList.toggle('dark', darkMode())); }
  function save(value) {
    try { localStorage.setItem(KEY, value); fallback = null; }
    catch { fallback = value; }
    apply();
  }
  window.addEventListener('storage', e => {
    if (e.storageArea === localStorage && (e.key === KEY || e.key === null)) { fallback = null; apply(); }
  });
  media.addEventListener('change', apply);
  for (const btn of document.querySelectorAll('[data-theme-toggle]')) {
    const target = document.querySelector(btn.dataset.themeToggle || 'html') || document.documentElement;
    targets.add(target);
    const sync = () => {
      const dark = target.classList.contains('dark');
      const name = dark ? NAME.light : NAME.dark;
      btn.setAttribute('aria-label', name);
      btn.title = name;
      btn.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true">${dark ? MOON : SUN}</svg>`;
    };
    btn.addEventListener('click', () => { save(target.classList.contains('dark') ? 'light' : 'dark'); });
    // Sync as well when the class is set by other means (render harness, devtools).
    new MutationObserver(sync).observe(target, { attributes: true, attributeFilter: ['class'] });
    sync();
  }
  apply();
})();
