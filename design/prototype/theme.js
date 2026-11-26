// Shared theme toggle for the static desktop prototype pages and the nav page
// (design/index.html). The interactive app shell (agent.html + app.js) keeps its
// own state-driven toggle.
// Icon = the theme you are in; accessible name and tooltip = the theme a click
// switches to. Each page keeps its own theme mechanism: data-theme-toggle is a
// selector for the element that carries the .dark class.
(() => {
  'use strict';
  const SUN = '<circle cx="12" cy="12" r="3.5"></circle><path d="M12 2.5v2M12 19.5v2M4.6 4.6 6 6M18 18l1.4 1.4M2.5 12h2M19.5 12h2M4.6 19.4 6 18M18 6l1.4-1.4"></path>';
  const MOON = '<path d="M20 15.2A8.2 8.2 0 0 1 8.8 4a8.4 8.4 0 1 0 11.2 11.2Z"></path>';
  // Key = the theme a click switches to.
  const NAME = { dark: '切换深色主题', light: '切换浅色主题' };
  for (const btn of document.querySelectorAll('[data-theme-toggle]')) {
    const target = document.querySelector(btn.dataset.themeToggle || 'html') || document.documentElement;
    const sync = () => {
      const dark = target.classList.contains('dark');
      const name = dark ? NAME.light : NAME.dark;
      btn.setAttribute('aria-label', name);
      btn.title = name;
      btn.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true">${dark ? MOON : SUN}</svg>`;
    };
    btn.addEventListener('click', () => { target.classList.toggle('dark'); });
    // Sync as well when the class is set by other means (render harness, devtools).
    new MutationObserver(sync).observe(target, { attributes: true, attributeFilter: ['class'] });
    sync();
  }
})();
