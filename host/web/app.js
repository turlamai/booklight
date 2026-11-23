// Booklight product web UI (#75 S2): library, reader chrome/tabs, EPUB
// surface; PDF rides the S0-verified pdf.js render-only layer (page surgery
// and selection/pending-edit bars land in S3). All business goes through the
// typed RPC surface; strings come from the shared/i18n single source served
// by the carrier; tokens come from shared.css. No business logic here.
"use strict";

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const params = new URLSearchParams(location.search);
const e2e = params.get("e2e") === "1";

let I18N = { zh: {}, en: {} };
let lang = localStorage.getItem("booklight-lang") || "zh";
let appTheme = localStorage.getItem("booklight-theme") || "light";
let settings = null;
let docs = [];            // open documents: {path, kind, title, view, chapter, page}
let active = null;        // active doc
let sideTab = "toc";
let sideVisible = true;
let readTheme = "system";
let fontPermille = 1000;
let pdfjsReady = null;

function t(key, ...args) {
  let s = (I18N[lang] && I18N[lang][key]) || (I18N.zh && I18N.zh[key]) || key;
  args.forEach((a, i) => { s = s.split("{" + i + "}").join(String(a)); });
  return s;
}
function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function b64ToBytes(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}
function b64ToText(b64) { return new TextDecoder("utf-8").decode(b64ToBytes(b64)); }
function fileName(p) { return p.split(/[\\/]/).pop(); }
function dirName(p) { const i = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\")); return i < 0 ? "" : p.slice(0, i + 1); }
function kindOf(p) { return /\.epub$/i.test(p) ? "epub" : "pdf"; }

async function rpc(name, args) {
  const res = await fetch("/api/rpc/" + name, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify(args || {}),
  });
  const text = await res.text();
  let json = null; try { json = JSON.parse(text); } catch (_) {}
  if (!res.ok) {
    const msg = (json && (json.error || json.message)) || text || ("HTTP " + res.status);
    throw new Error(msg);
  }
  return json;
}
async function post(path, body) {
  const res = await fetch(path, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body || {}),
  });
  return { status: res.status, json: await res.json().catch(() => null) };
}
async function getJson(path) {
  const res = await fetch(path);
  return { status: res.status, json: await res.json().catch(() => null) };
}

function announce(msg) {
  $("#live").textContent = msg;
  const n = $("#toast"); n.textContent = msg; n.hidden = false;
  clearTimeout(announce._t); announce._t = setTimeout(() => { n.hidden = true; }, 3500);
}
function applyI18n() {
  document.documentElement.lang = lang === "zh" ? "zh" : "en";
  $$("[data-i18n]").forEach(el => { el.textContent = t(el.dataset.i18n); });
  $$("[data-i18n-title]").forEach(el => { el.title = t(el.dataset.i18nTitle); });
  $("#prevChapter").textContent = t("ui.menu.prev-page");
  $("#nextChapter").textContent = t("ui.menu.next-page");
  $("#fontLabel").textContent = Math.round(fontPermille / 10) + "%";
}
function applyTheme() {
  $("#app").classList.toggle("dark", appTheme === "dark");
  $("#themeBtn").title = appTheme === "dark" ? t("ui.toolbar.theme-light") : t("ui.toolbar.theme-dark");
  const r = $("#reader");
  r.className = "reader " + (readTheme === "system" ? "system" : readTheme);
  r.style.setProperty("--read-size", (17 * fontPermille / 1000) + "px");
}

// ---- library -------------------------------------------------------------
function renderLibrary() {
  $("#view-library").hidden = false;
  $("#view-reader").hidden = true;
  $("#tabbar").hidden = docs.length === 0;
  $("#docTitle").textContent = t("app.name");
  $("#docSub").textContent = "";
  $("#sidebar").hidden = true;
  const rec = (settings && settings.recent) || [];
  const list = $("#recList");
  list.innerHTML = "";
  if (!rec.length) {
    list.innerHTML = '<div class="empty"><div class="et"></div><div class="eh"></div></div>';
    $(".et", list).textContent = t("ui.welcome.no-recent");
    $(".eh", list).textContent = t("ui.welcome.no-recent-hint");
  } else {
    rec.forEach(r => {
      const b = document.createElement("button");
      b.className = "rrow"; b.dataset.path = r.path;
      b.innerHTML = '<span class="ficon"></span><span class="fmeta"><span class="fname"></span><span class="fpath"></span></span><span class="resume"></span><span class="chev">›</span>';
      $(".ficon", b).textContent = kindOf(r.path).toUpperCase();
      $(".fname", b).textContent = fileName(r.path);
      $(".fpath", b).textContent = dirName(r.path);
      $(".resume", b).textContent = kindOf(r.path) === "epub" ? t("ui.epub.chapter-n", r.page) : t("ui.welcome.resume", r.page);
      b.addEventListener("click", () => openPath(r.path));
      list.appendChild(b);
    });
  }
  $("#statusText").textContent = t("app.name");
  $("#statusSave").textContent = "";
}

// ---- tabs ----------------------------------------------------------------
function renderTabs() {
  const bar = $("#tabbar");
  bar.hidden = docs.length === 0;
  bar.innerHTML = "";
  docs.forEach(d => {
    const el = document.createElement("div");
    el.className = "doctab" + (d === active ? " active" : "");
    el.innerHTML = '<span class="tl"></span><button class="x" aria-label="' + esc(t("ui.menu.close-tab")) + '">✕</button>';
    $(".tl", el).textContent = d.title || fileName(d.path);
    el.addEventListener("click", e => { if (!e.target.closest(".x")) { active = d; renderReader(); } });
    $(".x", el).addEventListener("click", e => { e.stopPropagation(); closeDoc(d); });
    bar.appendChild(el);
  });
}
function closeDoc(d) {
  docs = docs.filter(x => x !== d);
  if (active === d) active = docs[docs.length - 1] || null;
  if (active) renderReader(); else renderLibrary();
}

// ---- open ----------------------------------------------------------------
async function openViaDialog() {
  const r = await post("/api/dialog/open", {
    title: t("ui.dialog.open"),
    filters: [[t("ui.filter.documents"), "*.epub", "*.pdf"]],
  });
  const p = r.json && r.json.path;
  if (p) await openPath(p);
}
async function openPath(p) {
  const kind = kindOf(p);
  try {
    let d = docs.find(x => x.path === p);
    if (!d) {
      d = { path: p, kind, title: fileName(p), chapter: 1, page: 1, view: null };
      if (kind === "epub") {
        d.view = await rpc("epub-open", { path: p });
        d.title = d.view.title || fileName(p);
        const rec = (settings && settings.recent || []).find(x => x.path === p);
        d.chapter = rec ? Math.max(1, rec.page) : 1;
      } else {
        const bytes = await post("/api/host/file", { path: p });
        if (!bytes.json || !bytes.json.data) throw new Error(t("ui.open-failed", p));
        d.bytes = b64ToBytes(bytes.json.data);
      }
      docs.push(d);
    }
    active = d;
    await rpc("update-recents", { path: p, page: kind === "epub" ? d.chapter : d.page, scrollRatioScaled: 0 });
    settings = await rpc("get-settings", {});
    await renderReader();
  } catch (err) {
    announce(kind === "epub" ? t("ui.epub.open-failed", err.message) : t("ui.open-failed", err.message));
  }
}

// ---- reader --------------------------------------------------------------
async function renderReader() {
  if (!active) return renderLibrary();
  $("#view-library").hidden = true;
  $("#view-reader").hidden = false;
  $("#sidebar").hidden = !sideVisible;
  renderTabs();
  $("#docTitle").textContent = active.title;
  if (active.kind === "epub") {
    $("#docSub").textContent = t("ui.viewer.epub-subtitle", active.chapter, active.view.chapters);
    $("#chapterStatus").textContent = t("ui.viewer.chapter-status", active.chapter, active.view.chapters);
    $("#prevChapter").disabled = active.chapter <= 1;
    $("#nextChapter").disabled = active.chapter >= active.view.chapters;
    $("#readTheme").disabled = false; $("#fontMinus").disabled = false; $("#fontPlus").disabled = false;
    await renderChapter();
  } else {
    $("#docSub").textContent = t("ui.viewer.pdf-info", active.pageCount || "…");
    $("#chapterStatus").textContent = t("ui.viewer.page-status", active.page, active.pageCount || 0);
    $("#prevChapter").disabled = active.page <= 1;
    $("#nextChapter").disabled = active.page >= (active.pageCount || 1);
    $("#readTheme").disabled = true; $("#fontMinus").disabled = true; $("#fontPlus").disabled = true;
    await renderPdf();
  }
  renderSide();
  applyTheme();
}
async function renderChapter() {
  const html = await rpc("epub-chapter-html", { path: active.path, index: active.chapter });
  const doc = new DOMParser().parseFromString(b64ToText(html), "text/html");
  doc.querySelectorAll("script,style,link,iframe,object").forEach(n => n.remove());
  const prose = $("#prose");
  prose.innerHTML = "";
  [...doc.body.childNodes].forEach(n => prose.appendChild(document.importNode(n, true)));
  $("#reader").scrollTop = 0;
}
async function ensurePdfjs() {
  if (pdfjsReady) return pdfjsReady;
  pdfjsReady = new Promise((resolve, reject) => {
    const s = document.createElement("script");
    s.src = "/pdfjs/legacy/build/pdf.min.js";
    s.onload = () => {
      window.pdfjsLib.GlobalWorkerOptions.workerSrc = "/pdfjs/legacy/build/pdf.worker.min.js";
      resolve(window.pdfjsLib);
    };
    s.onerror = () => reject(new Error("pdf.js load failed"));
    document.head.appendChild(s);
  });
  return pdfjsReady;
}
async function renderPdf() {
  const lib = await ensurePdfjs();
  if (!active.pdf) {
    active.pdf = await lib.getDocument({ data: active.bytes.slice(0) }).promise;
    active.pageCount = active.pdf.numPages;
  }
  const page = await active.pdf.getPage(active.page);
  const viewport = page.getViewport({ scale: 1.2 });
  const prose = $("#prose");
  prose.innerHTML = '<canvas id="pdfCanvas"></canvas>';
  const canvas = $("#pdfCanvas");
  canvas.width = viewport.width; canvas.height = viewport.height;
  canvas.style.maxWidth = "100%";
  await page.render({ canvasContext: canvas.getContext("2d"), viewport }).promise;
  $("#docSub").textContent = t("ui.viewer.pdf-info", active.pageCount);
  $("#chapterStatus").textContent = t("ui.viewer.page-status", active.page, active.pageCount);
  $("#nextChapter").disabled = active.page >= active.pageCount;
}
async function gotoChapter(n) {
  if (!active || active.kind !== "epub") return;
  active.chapter = Math.min(Math.max(1, n), active.view.chapters);
  await rpc("update-recents", { path: active.path, page: active.chapter, scrollRatioScaled: 0 });
  await renderReader();
}
async function gotoPage(n) {
  if (!active || active.kind !== "pdf") return;
  active.page = Math.min(Math.max(1, n), active.pageCount || 1);
  await rpc("update-recents", { path: active.path, page: active.page, scrollRatioScaled: 0 });
  await renderReader();
}

// ---- sidebar (toc / notes tabs) -----------------------------------------
function renderSide() {
  $$("#vswitch .v").forEach(v => v.classList.toggle("active", v.dataset.tab === sideTab));
  const list = $("#sideList"); list.innerHTML = "";
  if (!active) { $("#sideSect").textContent = ""; $("#sideFoot").textContent = ""; return; }
  if (sideTab === "toc") {
    if (active.kind === "epub") {
      const toc = (active.view && active.view.toc) || [];
      $("#sideSect").textContent = t("ui.viewer.outline");
      if (!toc.length) { list.innerHTML = '<div class="empty"></div>'; $(".empty", list).textContent = t("ui.epub.toc-empty"); }
      toc.forEach(item => {
        const b = document.createElement("button");
        b.className = "side-item lvl" + Math.min(2, Math.max(0, item.level)) + (item.chapter === active.chapter ? " active" : "");
        b.innerHTML = "<small></small><span></span>";
        $("small", b).textContent = t("ui.epub.chapter-n", item.chapter);
        $("span", b).textContent = item.title;
        b.addEventListener("click", () => gotoChapter(item.chapter));
        list.appendChild(b);
      });
    } else {
      $("#sideSect").textContent = t("ui.viewer.thumbs");
      for (let n = 1; n <= (active.pageCount || 0); n++) {
        const b = document.createElement("button");
        b.className = "side-item" + (n === active.page ? " active" : "");
        b.textContent = t("ui.viewer.page-status", n, active.pageCount);
        b.addEventListener("click", () => gotoPage(n));
        list.appendChild(b);
      }
    }
    $("#sideFoot").textContent = "";
  } else {
    $("#sideSect").textContent = t("ui.sidebar.annotations");
    renderNotes(list);
  }
}
async function renderNotes(list) {
  list.innerHTML = '<div class="empty"></div>';
  try {
    const sidecar = !!(settings && settings.annotationSidecar);
    const doc = await rpc("load-annotations", { pdfPath: active.path, sidecar });
    $("#sideFoot").textContent = doc.recoveryPending ? t("ui.notes.recovery-pending") : "";
    const data = doc.data ? JSON.parse(b64ToText(doc.data)) : null;
    const items = (data && (data.annotations || data.items)) || [];
    list.innerHTML = "";
    if (!items.length) { list.innerHTML = '<div class="empty"></div>'; $(".empty", list).textContent = t("ui.notes.empty"); return; }
    items.forEach(a => {
      const row = document.createElement("div");
      row.className = "arow";
      row.innerHTML = '<span class="adot"></span><span class="abody"><span class="aloc"></span><span class="aex"></span><span class="anote"></span></span>';
      const color = a.color === "blue" ? "var(--hl-blue)" : a.color === "green" ? "var(--hl-green)" : "var(--hl-yellow)";
      $(".adot", row).style.background = color;
      $(".aloc", row).textContent = a.loc || (a.page ? t("ui.notes.page", a.page) : "");
      $(".aex", row).textContent = a.text || a.exact || "";
      $(".anote", row).textContent = a.note || "";
      list.appendChild(row);
    });
  } catch (err) {
    $(".empty", list).textContent = t("ui.notes.empty");
    $("#sideFoot").textContent = "";
  }
}

// ---- prefs ---------------------------------------------------------------
async function setReadTheme(v) {
  readTheme = v; applyTheme();
  settings = await rpc("save-reading-prefs", { theme: v, fontScalePermille: fontPermille });
}
async function setFont(delta) {
  fontPermille = Math.min(2000, Math.max(700, fontPermille + delta));
  applyTheme(); applyI18n();
  settings = await rpc("save-reading-prefs", { theme: readTheme, fontScalePermille: fontPermille });
}

// ---- boot ----------------------------------------------------------------
async function boot() {
  const i18n = await getJson("/api/host/i18n");
  if (i18n.json) I18N = i18n.json;
  await rpc("initialize", {});
  settings = await rpc("get-settings", {});
  readTheme = (settings && settings.readingTheme) || "system";
  fontPermille = (settings && settings.readingFontScalePermille) || 1000;
  $("#readTheme").value = readTheme;
  applyI18n(); applyTheme(); renderLibrary();
  wire();
  if (e2e) {
    const gate = await post("/api/host/e2e", {});
    if (gate.json && gate.json.enabled) runJourney();
  }
}
function wire() {
  $("#openBtn").addEventListener("click", openViaDialog);
  $("#openCta").addEventListener("click", openViaDialog);
  $("#sidebarBtn").addEventListener("click", () => { sideVisible = !sideVisible; if (active) renderReader(); else $("#sidebar").hidden = !sideVisible; });
  $("#themeBtn").addEventListener("click", () => { appTheme = appTheme === "dark" ? "light" : "dark"; localStorage.setItem("booklight-theme", appTheme); applyTheme(); });
  $("#langBtn").addEventListener("click", async () => {
    lang = lang === "zh" ? "en" : "zh";
    localStorage.setItem("booklight-lang", lang);
    await rpc("set-locale", { code: lang });
    applyI18n(); if (active) renderReader(); else renderLibrary();
  });
  $$("#vswitch .v").forEach(v => v.addEventListener("click", () => { sideTab = v.dataset.tab; renderSide(); }));
  $("#prevChapter").addEventListener("click", () => active && (active.kind === "epub" ? gotoChapter(active.chapter - 1) : gotoPage(active.page - 1)));
  $("#nextChapter").addEventListener("click", () => active && (active.kind === "epub" ? gotoChapter(active.chapter + 1) : gotoPage(active.page + 1)));
  $("#readTheme").addEventListener("change", e => setReadTheme(e.target.value));
  $("#fontMinus").addEventListener("click", () => setFont(-100));
  $("#fontPlus").addEventListener("click", () => setFont(100));
}

// ---- S2 e2e journey (harness only; ?e2e=1 + BOOKLIGHT_E2E=1) --------------
async function runJourney() {
  const steps = {};
  const step = async (id, fn) => {
    try { const detail = await fn(); steps[id] = { ok: true, detail: String(detail ?? "") }; }
    catch (err) { steps[id] = { ok: false, detail: err.message }; }
  };
  await step("i18n", () => { if (!I18N.zh["ui.viewer.recents"]) throw new Error("i18n missing"); return Object.keys(I18N.zh).length; });
  await step("library", () => { renderLibrary(); return $("#recList").children.length; });
  const fx = await post("/api/host/e2e_fixtures", {});
  const epub = fx.json && fx.json.epub, pdf = fx.json && fx.json.pdf;
  await step("open-epub", async () => { await openPath(epub); if (!active || active.kind !== "epub") throw new Error("not epub"); return active.view.chapters; });
  await step("toc", () => { const n = $$("#sideList .side-item").length; if (!n) throw new Error("empty toc"); return n; });
  await step("chapter-html", () => { const n = $("#prose").querySelectorAll("p").length; if (!n) throw new Error("no prose"); return n; });
  await step("chapter-next", async () => { const before = active.chapter; await gotoChapter(before + 1); if (active.chapter !== before + 1) throw new Error("nav failed"); return active.chapter; });
  await step("notes-tab", async () => { sideTab = "notes"; renderSide(); await new Promise(r => setTimeout(r, 300)); return $$("#sideList .arow, #sideList .empty").length; });
  await step("read-theme", async () => { await setReadTheme("sepia"); if (!$("#reader").classList.contains("sepia")) throw new Error("theme not applied"); return readTheme; });
  await step("font-scale", async () => { const before = fontPermille; await setFont(100); if (fontPermille !== before + 100) throw new Error("font not applied"); return fontPermille; });
  await step("open-pdf", async () => { await openPath(pdf); if (!active || active.kind !== "pdf") throw new Error("not pdf"); await new Promise(r => setTimeout(r, 500)); return active.pageCount; });
  await step("pdf-canvas", () => { const c = $("#pdfCanvas"); if (!c || !c.width) throw new Error("no canvas"); const ctx = c.getContext("2d"); const d = ctx.getImageData(0, 0, c.width, c.height).data; let nonBlank = 0; for (let i = 3; i < d.length; i += 40) if (d[i] > 0) nonBlank++; if (!nonBlank) throw new Error("blank canvas"); return nonBlank; });
  await step("tabs", () => { const n = $$("#tabbar .doctab").length; if (n !== 2) throw new Error("tabs=" + n); return n; });
  await step("tab-close", async () => { const d = docs.find(x => x.kind === "pdf"); closeDoc(d); if (docs.length !== 1) throw new Error("close failed"); return docs.length; });
  const ok = Object.values(steps).every(s => s.ok);
  await post("/api/host/e2e_report", { payload: { phase: "done", ok, stepCount: Object.keys(steps).length, steps } });
}

boot();