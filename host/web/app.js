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
  $$("[data-i18n-ph]").forEach(el => { el.placeholder = t(el.dataset.i18nPh); });
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
  $("#view-settings").hidden = true;
  $("#view-research").hidden = true;
  $("#inspector").hidden = true;
  $("#aiBtn").hidden = true;
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
        d.originalBytes = d.bytes;
        d.undo = []; d.dirty = false;
        d.annotations = []; d.bookmarks = [];
      }
      docs.push(d);
    }
    active = d;
    if (kind === "pdf") { try { await loadAnnotations(); } catch (_) { active.annotations = []; } }
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
  $("#view-settings").hidden = true;
  $("#view-research").hidden = true;
  $("#aiBtn").hidden = false;
  $("#sidebar").hidden = !sideVisible;
  renderTabs();
  $("#docTitle").textContent = active.title;
  if (active.kind === "epub") {
    $("#docSub").textContent = t("ui.viewer.epub-subtitle", active.chapter, active.view.chapters);
    $("#chapterStatus").textContent = t("ui.viewer.chapter-status", active.chapter, active.view.chapters);
    $("#prevChapter").disabled = active.chapter <= 1;
    $("#nextChapter").disabled = active.chapter >= active.view.chapters;
    $("#readTheme").disabled = false; $("#fontMinus").disabled = false; $("#fontPlus").disabled = false;
    $("#pdfMenuBtn").hidden = true; $("#pendingbar").hidden = true; hideSelbar();
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
const PDF_FONTS = { cMapUrl: "/pdfjs/cmaps/", cMapPacked: true, standardFontDataUrl: "/pdfjs/standard_fonts/" };
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
function bytesToB64(bytes) {
  let bin = "";
  for (let i = 0; i < bytes.length; i += 0x8000)
    bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(bin);
}
// PDF user-space (points, bottom-left, unrotated) <-> viewport px.
function rectToPdf(viewport, r, base) {
  const [x1, y1] = viewport.convertToPdfPoint(r.left - base.left, r.bottom - base.top);
  const [x2, y2] = viewport.convertToPdfPoint(r.right - base.left, r.top - base.top);
  return { x: Math.min(x1, x2), y: Math.min(y1, y2), width: Math.abs(x2 - x1), height: Math.abs(y2 - y1) };
}
function rectToView(viewport, r) {
  const [x1, y1] = viewport.convertToViewportPoint(r.x, r.y);
  const [x2, y2] = viewport.convertToViewportPoint(r.x + r.width, r.y + r.height);
  return { left: Math.min(x1, x2), top: Math.min(y1, y2), width: Math.abs(x2 - x1), height: Math.abs(y2 - y1) };
}
async function renderPdf() {
  const lib = await ensurePdfjs();
  if (!active.pdf) {
    active.pdf = await lib.getDocument(Object.assign({ data: active.bytes.slice(0) }, PDF_FONTS)).promise;
    active.pageCount = active.pdf.numPages;
  }
  const page = await active.pdf.getPage(active.page);
  const viewport = page.getViewport({ scale: 1.5 });
  const prose = $("#prose");
  prose.innerHTML = "";
  const wrap = document.createElement("div");
  wrap.className = "pdf-page";
  wrap.style.width = viewport.width + "px";
  const canvas = document.createElement("canvas");
  canvas.width = viewport.width; canvas.height = viewport.height;
  canvas.style.width = viewport.width + "px"; canvas.style.height = viewport.height + "px";
  const tl = document.createElement("div");
  tl.className = "textLayer";
  tl.style.width = viewport.width + "px"; tl.style.height = viewport.height + "px";
  tl.style.setProperty("--scale-factor", viewport.scale);
  const hl = document.createElement("div");
  hl.className = "hl-layer";
  wrap.append(canvas, tl, hl);
  prose.appendChild(wrap);
  active.shell = { wrap, canvas, tl, hl, viewport, page };
  await Promise.all([
    page.render({ canvasContext: canvas.getContext("2d"), viewport }).promise,
    lib.renderTextLayer({ textContentSource: page.streamTextContent(), container: tl, viewport }).promise,
  ]);
  paintAnnotations();
  $("#docSub").textContent = t("ui.viewer.pdf-info", active.pageCount);
  $("#chapterStatus").textContent = t("ui.viewer.page-status", active.page, active.pageCount);
  $("#nextChapter").disabled = active.page >= active.pageCount;
  $("#pdfMenuBtn").hidden = false;
  renderPendingBar();
}
function paintAnnotations() {
  if (!active || active.kind !== "pdf" || !active.shell) return;
  const { hl, viewport } = active.shell;
  hl.innerHTML = "";
  (active.annotations || []).filter(a => a.page === active.page && (a.rects || []).length).forEach(a => {
    a.rects.forEach(r => {
      const v = rectToView(viewport, r);
      const div = document.createElement("div");
      div.className = "hl " + (a.kind || "highlight") + " " + (a.color || "yellow");
      div.style.left = v.left + "px"; div.style.top = v.top + "px";
      div.style.width = v.width + "px"; div.style.height = v.height + "px";
      hl.appendChild(div);
    });
  });
}
async function loadAnnotations() {
  const sidecar = !!(settings && settings.annotationSidecar);
  const doc = await rpc("load-annotations", { pdfPath: active.path, sidecar });
  const data = doc.data ? JSON.parse(b64ToText(doc.data)) : {};
  active.annotations = data.annotations || [];
  active.bookmarks = data.bookmarks || [];
  active.recoveryPending = !!doc.recoveryPending;
}
async function saveAnnotations() {
  const sidecar = !!(settings && settings.annotationSidecar);
  const payload = { annotations: active.annotations || [], bookmarks: active.bookmarks || [] };
  const b64 = bytesToB64(new TextEncoder().encode(JSON.stringify(payload)));
  await rpc("set-annotations", { pdfPath: active.path, sidecar, data: b64 });
  active.recoveryPending = false;
}
function showSelbar(range) {
  const bar = $("#selbar");
  const reader = $("#reader");
  const rb = reader.getBoundingClientRect();
  const r = range.getBoundingClientRect();
  bar.hidden = false;
  bar.style.left = Math.max(8, (r.left - rb.left) + (r.width / 2) - 90) + "px";
  bar.style.top = Math.max(8, (r.top - rb.top) - 34 + reader.scrollTop) + "px";
}
function hideSelbar() { $("#selbar").hidden = true; pendingSelection = null; }
let pendingSelection = null;
function onPdfSelect() {
  if (!active || active.kind !== "pdf" || !active.shell) return;
  const sel = window.getSelection();
  if (!sel || sel.isCollapsed || !sel.rangeCount) { hideSelbar(); return; }
  const range = sel.getRangeAt(0);
  if (!active.shell.tl.contains(range.commonAncestorContainer)) { hideSelbar(); return; }
  const base = active.shell.wrap.getBoundingClientRect();
  const rects = [...range.getClientRects()].filter(r => r.width > 1 && r.height > 1)
    .map(r => rectToPdf(active.shell.viewport, r, base));
  if (!rects.length) { hideSelbar(); return; }
  pendingSelection = { page: active.page, rects, excerpt: sel.toString().trim().slice(0, 800) };
  showSelbar(range);
}
async function addAnnotation(color, kind) {
  if (!pendingSelection) return;
  const a = Object.assign({ id: "a-" + Date.now() + "-" + Math.floor(Math.random() * 10000), color, kind, note: "", created: Math.floor(Date.now() / 1000) }, pendingSelection);
  active.annotations = [...(active.annotations || []), a];
  try { await saveAnnotations(); $("#statusSave").textContent = ""; }
  catch (err) { $("#statusSave").textContent = t("ui.notes.save-failed", ""); }
  window.getSelection().removeAllRanges();
  hideSelbar(); paintAnnotations(); renderSide();
}
// ---- pending page edits (preview bytes; source untouched until save-as) --
function renderPendingBar() {
  const bar = $("#pendingbar");
  const dirty = active && active.kind === "pdf" && active.dirty;
  bar.hidden = !dirty;
  if (dirty) {
    $("#pendingText").textContent = t("ui.pdf.pending-body");
    $("#editUndo").textContent = t("ui.undo");
    $("#editDiscard").textContent = t("ui.pdf.discard");
    $("#editSave").textContent = t("ui.pdf.save-as");
    $("#editUndo").disabled = !(active.undo || []).length;
  }
}
async function scratchPath() {
  const r = await post("/api/host/scratch", { data: bytesToB64(active.bytes) });
  return r.json && r.json.path;
}
async function applyEdit(kind) {
  if (!active || active.kind !== "pdf") return;
  const old = active.bytes;
  let next = null;
  const p = await scratchPath();
  if (kind === "rotate") next = await rpc("edit-rotate-pages", { path: p, pages: [active.page], delta: 90 });
  else if (kind === "delete") { if (active.pageCount <= 1) return announce(t("ui.pages.edit-failed")); next = await rpc("edit-delete-pages", { path: p, pages: [active.page] }); }
  else if (kind === "blank") next = await rpc("edit-insert-blank-after", { path: p, page: active.page });
  else if (kind === "append") {
    const r = await post("/api/dialog/open", { title: t("ui.pages.merge"), filters: [[t("ui.filter.pdf"), "*.pdf"]] });
    const other = r.json && r.json.path; if (!other) return;
    next = await rpc("edit-append-doc", { path: p, otherPath: other });
  } else if (kind === "extract") {
    const bytes = await rpc("edit-extract-pages", { path: p, pages: [active.page] });
    return exportBytes(bytes, "extract.pdf");
  } else if (kind === "bake") {
    next = await rpc("edit-bake-text", { path: p, watermarkText: active.title, watermarkSize: 24, watermarkOpacityMilli: 300, boxes: [] });
  }
  if (!next) return;
  active.undo = [...(active.undo || []), old];
  active.bytes = b64ToBytes(next);
  active.pdf = null; active.dirty = true;
  active.page = 1;
  await renderReader();
}
async function exportBytes(b64, name) {
  const r = await post("/api/dialog/save", { defaultName: name });
  const dest = r.json && r.json.path; if (!dest) return;
  await post("/api/host/write", { path: dest, data: b64 });
  announce(t("ui.settings.saved"));
}
async function undoEdit() {
  if (!active.undo || !active.undo.length) return;
  active.bytes = active.undo.pop();
  active.pdf = null; active.dirty = active.undo.length > 0; active.page = 1;
  await renderReader();
}
async function discardEdit() {
  active.bytes = active.originalBytes; active.undo = []; active.pdf = null; active.dirty = false; active.page = 1;
  await renderReader();
}
async function saveEdit() {
  const r = await post("/api/dialog/save", { defaultName: fileName(active.path) });
  const dest = r.json && r.json.path; if (!dest) return;
  await post("/api/host/write", { path: dest, data: bytesToB64(active.bytes) });
  active.dirty = false; active.undo = [];
  renderPendingBar(); announce(t("ui.settings.saved"));
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
    if (active.kind === "pdf" && active.annotations) {
      // cached in-session doc (includes unsaved edits); refresh recovery flag
      const sidecar = !!(settings && settings.annotationSidecar);
      const pend = await rpc("annotation-recovery-pending", { pdfPath: active.path, sidecar });
      active.recoveryPending = !!pend;
    } else {
      await loadAnnotations();
    }
    $("#sideFoot").textContent = active.recoveryPending ? t("ui.notes.recovery-pending") : "";
    const items = active.annotations || [];
    list.innerHTML = "";
    if (!items.length) { list.innerHTML = '<div class="empty"></div>'; $(".empty", list).textContent = t("ui.notes.empty"); return; }
    items.forEach(a => {
      const row = document.createElement("div");
      row.className = "arow";
      row.innerHTML = '<span class="adot"></span><span class="abody"><span class="aloc"></span><span class="aex"></span><span class="anote"></span></span>';
      const color = a.color === "blue" ? "var(--hl-blue)" : a.color === "green" ? "var(--hl-green)" : "var(--hl-yellow)";
      $(".adot", row).style.background = a.kind === "highlight" ? color : "var(--mute)";
      $(".aloc", row).textContent = a.page ? t("ui.notes.page", a.page) : (a.loc || "");
      $(".aex", row).textContent = a.excerpt || a.text || "";
      $(".anote", row).textContent = a.note || "";
      list.appendChild(row);
    });
    if (active.recoveryPending) {
      const retry = document.createElement("button");
      retry.className = "btn"; retry.textContent = t("ui.notes.retry-save");
      retry.addEventListener("click", async () => { await saveAnnotations(); renderSide(); });
      list.appendChild(retry);
    }
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
  wire(); wireS4(); wireDrop();
  if (e2e) {
    const gate = await post("/api/host/e2e", {});
    if (gate.json && gate.json.enabled) {
      if (params.get("stage") === "pdf") runPdfStage();
      else if (params.get("stage") === "s4") runS4Stage();
      else if (params.get("stage") === "s5") runS5Stage();
      else if (params.get("stage") === "drop") runDropStage();
      else runJourney();
    }
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
  // S3 PDF surface
  document.addEventListener("mouseup", () => setTimeout(onPdfSelect, 0));
  $$("#selbar .swatch").forEach(sw => sw.addEventListener("click", () => addAnnotation(sw.dataset.color, "highlight")));
  $$("#selbar .act[data-kind]").forEach(ac => ac.addEventListener("click", () => addAnnotation("", ac.dataset.kind)));
  $("#selCopy").addEventListener("click", () => {
    if (pendingSelection) navigator.clipboard && navigator.clipboard.writeText(pendingSelection.excerpt);
    hideSelbar();
  });
  $("#pdfMenuBtn").addEventListener("click", () => {
    $("#pdfMenuTitle").textContent = t("ui.viewer.page-status", active.page, active.pageCount);
    $("#pdfMenu").showModal();
  });
  $("#pdfMenuClose").addEventListener("click", () => $("#pdfMenu").close());
  $$("#pdfMenu [data-edit]").forEach(b => b.addEventListener("click", async () => {
    $("#pdfMenu").close();
    try { await applyEdit(b.dataset.edit); }
    catch (err) { announce(t("ui.pages.edit-failed") + " " + err.message); }
  }));
  $("#editUndo").addEventListener("click", undoEdit);
  $("#editDiscard").addEventListener("click", discardEdit);
  $("#editSave").addEventListener("click", saveEdit);
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
  await step("pdf-canvas", () => { const c = $("#prose canvas"); if (!c || !c.width) throw new Error("no canvas"); const ctx = c.getContext("2d"); const d = ctx.getImageData(0, 0, c.width, c.height).data; let nonBlank = 0; for (let i = 3; i < d.length; i += 40) if (d[i] > 0) nonBlank++; if (!nonBlank) throw new Error("blank canvas"); return nonBlank; });
  const selectSpans = (a, b) => {
    const spans = active.shell.tl.querySelectorAll("span");
    if (spans.length < 2) throw new Error("text layer <2 spans");
    const range = document.createRange();
    range.setStart(spans[a], 0); range.setEnd(spans[b], spans[b].childNodes.length || 1);
    const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
    onPdfSelect();
    return sel;
  };
  await step("sel-highlight", async () => {
    selectSpans(0, 1);
    if (!pendingSelection) throw new Error("no pending selection");
    await addAnnotation("yellow", "highlight");
    if ((active.annotations || []).length !== 1) throw new Error("annotation not added");
    return $$("#prose .hl-layer .hl").length;
  });
  await step("sel-underline", async () => {
    selectSpans(1, Math.min(2, active.shell.tl.querySelectorAll("span").length - 1));
    await addAnnotation("", "underline");
    if (!active.annotations.some(a => a.kind === "underline")) throw new Error("underline missing");
    return active.annotations.length;
  });
  await step("ann-persist", async () => {
    const sidecar = !!(settings && settings.annotationSidecar);
    const doc = await rpc("load-annotations", { pdfPath: active.path, sidecar });
    const data = JSON.parse(b64ToText(doc.data));
    if ((data.annotations || []).length < 2) throw new Error("not persisted");
    return data.annotations.length;
  });
  await step("edit-rotate", async () => {
    await applyEdit("rotate");
    if (!active.dirty) throw new Error("not dirty");
    if (active.shell.viewport.rotation !== 90) throw new Error("rotation " + active.shell.viewport.rotation);
    return active.shell.viewport.rotation;
  });
  await step("edit-undo", async () => {
    await undoEdit();
    if (active.dirty) throw new Error("still dirty");
    if (active.shell.viewport.rotation !== 0) throw new Error("rotation not restored");
    return active.shell.viewport.rotation;
  });
  await step("edit-delete", async () => {
    const before = active.pageCount;
    await applyEdit("delete");
    if (active.pageCount !== before - 1) throw new Error("page not deleted");
    return active.pageCount;
  });
  await step("edit-save", async () => {
    const sc = await post("/api/host/scratch", { data: bytesToB64(active.bytes) });
    const dest = sc.json.path;
    await post("/api/host/write", { path: dest, data: bytesToB64(active.bytes) });
    const back = await post("/api/host/file", { path: dest });
    if (b64ToBytes(back.json.data).length !== active.bytes.length) throw new Error("save roundtrip mismatch");
    return active.bytes.length;
  });
  await step("edit-discard", async () => {
    await discardEdit();
    if (active.dirty) throw new Error("still dirty after discard");
    if (active.bytes.length !== active.originalBytes.length) throw new Error("source not restored");
    return active.bytes.length;
  });
  await runS4Steps(step);
  await step("tabs", () => { const n = $$("#tabbar .doctab").length; if (n !== 2) throw new Error("tabs=" + n); return n; });
  await step("tab-close", async () => { const d = docs.find(x => x.kind === "pdf"); closeDoc(d); if (docs.length !== 1) throw new Error("close failed"); return docs.length; });
  const ok = Object.values(steps).every(s => s.ok);
  await post("/api/host/e2e_report", { payload: { phase: "done", ok, stepCount: Object.keys(steps).length, steps } });
}

// Pixel-evidence stage: leave the PDF surface on screen with highlight ink
// and the pending-edit bar visible, then report (verify captures after).
async function runPdfStage() {
  const steps = {};
  const step = async (id, fn) => {
    try { const detail = await fn(); steps[id] = { ok: true, detail: String(detail ?? "") }; }
    catch (err) { steps[id] = { ok: false, detail: err.message }; }
  };
  const fx = await post("/api/host/e2e_fixtures", {});
  await step("open-pdf", async () => { await openPath(fx.json.pdf); return active.pageCount; });
  await step("ink", async () => {
    const spans = active.shell.tl.querySelectorAll("span");
    const range = document.createRange();
    range.setStart(spans[0], 0); range.setEnd(spans[1], spans[1].childNodes.length || 1);
    const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
    onPdfSelect(); await addAnnotation("yellow", "highlight");
    const r2 = document.createRange();
    r2.setStart(spans[1], 0); r2.setEnd(spans[Math.min(2, spans.length - 1)], 1);
    sel.removeAllRanges(); sel.addRange(r2);
    onPdfSelect(); await addAnnotation("", "underline");
    window.getSelection().removeAllRanges();
    return $$("#prose .hl-layer .hl").length;
  });
  await step("pending", async () => { await applyEdit("rotate"); return active.dirty; });
  const ok = Object.values(steps).every(s => s.ok);
  await post("/api/host/e2e_report", { payload: { phase: "done", ok, stepCount: Object.keys(steps).length, steps,
    state: { readerHidden: $("#view-reader").hidden, libHidden: $("#view-library").hidden, active: active && active.kind, dirty: active && active.dirty, hl: $$("#prose .hl-layer .hl").length } } });
}

// ---- S4: settings / research workbench / AI inspector ---------------------
let presets = [];
let aiMode = "chat";
let aiScope = "page";
let aiHistory = [];
let aiStreamId = null;
let aiBubble = null;
let aiPending = null;
let sse = null;
const pdfTextCache = new Map();

function showOnly(view) {
  ["library", "reader", "settings", "research"].forEach(v => { $("#view-" + v).hidden = v !== view; });
  $("#inspector").hidden = true;
  $("#aiBtn").hidden = view !== "reader";
  $("#sidebar").hidden = view !== "reader" || !sideVisible;
}

async function showSettings() {
  showOnly("settings");
  $("#docTitle").textContent = t("ui.tab.settings");
  $("#docSub").textContent = "";
  settings = await rpc("get-settings", {});
  presets = await rpc("list-presets", {});
  const ps = $("#setPreset"); ps.innerHTML = "";
  presets.forEach(p => { const o = document.createElement("option"); o.value = p.id; o.textContent = p.label; ps.appendChild(o); });
  ps.value = settings.provider;
  $("#setBaseUrl").value = settings.baseUrl;
  $("#setModel").value = settings.model;
  $("#setKey").value = "";
  $("#setKey").placeholder = settings.hasApiKey ? t("ui.settings.api-key-saved") : "";
  $("#setTarget").innerHTML = "";
  ["zh", "zh-hant", "en", "ja", "ko", "fr", "de", "es"].forEach(c => {
    const o = document.createElement("option"); o.value = c; o.textContent = c; $("#setTarget").appendChild(o);
  });
  $("#setTarget").value = settings.targetLanguage;
  $("#setLocale").value = lang;
  $("#setSidecar").checked = !!settings.annotationSidecar;
  $$("#setTheme button").forEach(b => b.classList.toggle("on", b.dataset.v === readTheme));
  $$("#setView button").forEach(b => b.classList.toggle("on", b.dataset.v === settings.viewMode));
  $("#setSuggest").innerHTML = "";
  $("#testMsg").hidden = true; $("#savedMsg").hidden = true;
  $("#statusText").textContent = t("ui.settings.privacy");
}

function settingsUpdate() {
  return {
    provider: $("#setPreset").value,
    baseUrl: $("#setBaseUrl").value,
    model: $("#setModel").value,
    targetLanguage: $("#setTarget").value,
    viewMode: ($$("#setView button").find(b => b.classList.contains("on")) || {}).dataset?.v || "single",
    annotationSidecar: $("#setSidecar").checked,
  };
}
async function saveSettings() {
  settings = await rpc("save-settings", { update: settingsUpdate() });
  $("#savedMsg").hidden = false;
  announce(t("ui.settings.saved"));
}
async function saveKey() {
  const k = $("#setKey").value;
  if (!k) { announce(t("ui.settings.need-input")); return; }
  await rpc("save-api-key", { key: k });
  $("#setKey").value = "";
  settings = await rpc("get-settings", {});
  $("#setKey").placeholder = t("ui.settings.api-key-saved");
  announce(t("ui.settings.api-key-saved"));
}
async function testConnection() {
  $("#testMsg").hidden = false; $("#testMsg").textContent = t("ui.settings.fetching");
  try {
    const r = await rpc("test-connection", { baseUrl: $("#setBaseUrl").value, apiKey: $("#setKey").value, model: $("#setModel").value });
    if (!r.ok) throw new Error(r.error || t("ui.settings.test"));
    const models = r.models || [];
    $("#testMsg").textContent = models.length ? t("ui.settings.test-ok", models.length) : t("ui.settings.test-ok-no-list");
    const sg = $("#setSuggest"); sg.innerHTML = "";
    models.slice(0, 6).forEach(m => {
      const s = document.createElement("span"); s.className = "sg"; s.textContent = m;
      s.addEventListener("click", () => { $("#setModel").value = m; });
      sg.appendChild(s);
    });
  } catch (err) { $("#testMsg").textContent = err.message; }
}

// ---- research ------------------------------------------------------------
async function pdfDocFor(path) {
  if (pdfTextCache.has(path)) return pdfTextCache.get(path);
  await ensurePdfjs();
  const f = await post("/api/host/file", { path });
  const doc = await pdfjsLib.getDocument({ data: b64ToBytes(f.json.data) }).promise;
  pdfTextCache.set(path, doc);
  return doc;
}
async function docUnits(d) {
  const units = [];
  if (d.kind === "epub") {
    for (let c = 1; c <= d.view.chapters; c++) {
      const text = await rpc("epub-chapter-text", { path: d.path, index: c });
      units.push({ loc: t("ui.epub.chapter-n", c), page: c, text });
    }
  } else {
    const doc = await pdfDocFor(d.path);
    for (let p = 1; p <= doc.numPages; p++) {
      const tc = await doc.getPage(p).then(pg => pg.getTextContent());
      units.push({ loc: t("ui.notes.page", p), page: p, text: tc.items.map(i => i.str).join(" ") });
    }
  }
  return units;
}
let resHits = [];
async function runSearch() {
  const q = $("#resQuery").value.trim();
  const terms = q.split(/\s+/).filter(Boolean);
  const box = $("#resResults"); box.innerHTML = "";
  resHits = [];
  if (!docs.length) {
    box.innerHTML = '<div class="noresult"><div class="nr-t"></div><div class="nr-h"></div></div>';
    $(".nr-t", box).textContent = t("ui.research.no-docs");
    $(".nr-h", box).textContent = t("ui.research.no-docs-hint");
    $("#statusText").textContent = t("ui.research.footer");
    return 0;
  }
  if (!terms.length) return 0;
  const t0 = performance.now();
  for (const d of docs) {
    const units = await docUnits(d);
    const mine = [];
    for (const u of units) {
      for (const term of terms) {
        const i = u.text.indexOf(term);
        if (i < 0) continue;
        const a = Math.max(0, i - 30), b = Math.min(u.text.length, i + term.length + 40);
        mine.push({ path: d.path, title: d.title, kind: d.kind, loc: u.loc, page: u.page,
          excerpt: u.text.slice(a, b), term });
        break;
      }
      if (mine.length >= 8) break;
    }
    if (!mine.length) continue;
    const head = document.createElement("div"); head.className = "rdoc";
    head.textContent = t("ui.research.hits", d.title, mine.length);
    box.appendChild(head);
    for (const h of mine) {
      resHits.push(h);
      const card = document.createElement("div"); card.className = "rcard";
      const ex = document.createElement("div"); ex.className = "ex";
      const pre = h.excerpt.slice(0, h.excerpt.indexOf(h.term));
      const post = h.excerpt.slice(h.excerpt.indexOf(h.term) + h.term.length);
      ex.append("…" + pre);
      const mk = document.createElement("mark"); mk.textContent = h.term; ex.appendChild(mk);
      ex.append(post + "…");
      card.appendChild(ex);
      const src = document.createElement("span"); src.className = "src"; src.textContent = h.loc + " ↗";
      src.addEventListener("click", () => jumpHit(h));
      card.appendChild(src);
      const acts = document.createElement("div"); acts.className = "acts";
      const jump = document.createElement("button"); jump.className = "btn"; jump.textContent = t("ui.research.jump");
      jump.addEventListener("click", () => jumpHit(h));
      const copy = document.createElement("button"); copy.className = "btn"; copy.textContent = t("ui.research.cite");
      copy.addEventListener("click", () => { navigator.clipboard && navigator.clipboard.writeText(citeText(h)); announce(t("ui.card.copied")); });
      const pin = document.createElement("button"); pin.className = "btn"; pin.textContent = t("ui.research.pin");
      pin.addEventListener("click", async () => { await pinHit(h); pin.classList.add("pinned"); pin.textContent = t("ui.research.pinned"); });
      acts.append(jump, copy, pin);
      card.appendChild(acts);
      box.appendChild(card);
    }
  }
  if (!resHits.length) {
    box.innerHTML = '<div class="noresult"><div class="nr-t"></div><div class="nr-h"></div></div>';
    $(".nr-t", box).textContent = t("ui.research.no-result-title");
    $(".nr-h", box).textContent = t("ui.research.no-result-hint");
  }
  const secs = ((performance.now() - t0) / 1000).toFixed(1);
  $("#statusText").textContent = t("ui.research.stats", docs.length, resHits.length, secs);
  return resHits.length;
}
function citeText(h) { return t("ui.research.cite-format", h.title, h.loc, h.excerpt); }
async function jumpHit(h) {
  await openPath(h.path);
  if (active.kind === "epub") await gotoChapter(h.page); else await gotoPage(h.page);
}
async function pinHit(h) {
  const sidecar = !!(settings && settings.annotationSidecar);
  const doc = await rpc("load-annotations", { pdfPath: h.path, sidecar });
  const data = JSON.parse(b64ToText(doc.data));
  data.annotations = data.annotations || [];
  data.annotations.push({ id: "cite-" + Date.now() + "-" + Math.random().toString(36).slice(2, 6),
    page: h.page, rects: [], excerpt: h.excerpt, color: "", kind: "cite", note: "", created: new Date().toISOString() });
  await rpc("set-annotations", { pdfPath: h.path, sidecar, data: bytesToB64(new TextEncoder().encode(JSON.stringify(data))) });
  const d = docs.find(x => x.path === h.path);
  if (d) d.annotations = data.annotations;
  await renderDeck();
}
async function renderDeck() {
  const list = $("#deckList"); list.innerHTML = "";
  let n = 0;
  const sidecar = !!(settings && settings.annotationSidecar);
  for (const d of docs) {
    const doc = await rpc("load-annotations", { pdfPath: d.path, sidecar });
    const data = JSON.parse(b64ToText(doc.data));
    for (const a of (data.annotations || []).filter(x => x.kind === "cite")) {
      n++;
      const c = document.createElement("div"); c.className = "ccard";
      const q = document.createElement("div"); q.className = "cq"; q.textContent = "「" + a.excerpt + "」";
      const src = document.createElement("div"); src.className = "csrc";
      src.append("《" + d.title + "》· " + (d.kind === "epub" ? t("ui.epub.chapter-n", a.page) : t("ui.notes.page", a.page)));
      const j = document.createElement("span"); j.className = "jump"; j.textContent = t("ui.research.jump-back");
      j.addEventListener("click", () => jumpHit({ path: d.path, page: a.page }));
      src.appendChild(j);
      c.append(q, src);
      list.appendChild(c);
    }
  }
  $("#deckCount").textContent = t("ui.research.deck-count", n);
  return n;
}
async function showResearch() {
  showOnly("research");
  $("#docTitle").textContent = t("ui.research.title");
  $("#docSub").textContent = t("ui.research.subtitle");
  $("#statusText").textContent = t("ui.research.footer");
  await renderDeck();
}

// ---- AI inspector ---------------------------------------------------------
async function contextFor(scope) {
  if (!active) return { text: "", label: "" };
  if (scope === "selection") {
    const s = (pendingSelection && pendingSelection.excerpt) || String(window.getSelection());
    return { text: s, label: t("ui.chat.scope.selection") };
  }
  if (active.kind === "epub") {
    if (scope === "document") return { text: await rpc("epub-doc-text", { path: active.path, maxChapters: 64, capChars: 200000 }), label: t("ui.chat.scope.doc") };
    return { text: await rpc("epub-chapter-text", { path: active.path, index: active.chapter }), label: t("ui.epub.chapter-n", active.chapter) };
  }
  const doc = await pdfDocFor(active.path);
  if (scope === "document") {
    let all = "";
    for (let p = 1; p <= doc.numPages; p++) all += (await doc.getPage(p).then(pg => pg.getTextContent())).items.map(i => i.str).join(" ") + "\n";
    return { text: all, label: t("ui.chat.scope.doc") };
  }
  const tc = await doc.getPage(active.page).then(pg => pg.getTextContent());
  return { text: tc.items.map(i => i.str).join(" "), label: t("ui.notes.page", active.page) };
}
function addBubble(who, text, cls) {
  const wrap = document.createElement("div"); wrap.className = "bub " + cls;
  const w = document.createElement("div"); w.className = "who"; w.textContent = who;
  const body = document.createElement("div"); body.textContent = text;
  wrap.append(w, body);
  $("#aiChat").appendChild(wrap);
  $("#aiChat").scrollTop = $("#aiChat").scrollHeight;
  return body;
}
function ensureSse() {
  if (sse) return;
  sse = new EventSource("/glaze/events");
  sse.addEventListener("stream-chunk", ev => {
    const d = JSON.parse(ev.data);
    if (d.requestId !== aiStreamId || !aiBubble) return;
    aiBubble.textContent += d.delta;
    $("#aiChat").scrollTop = $("#aiChat").scrollHeight;
  });
  sse.addEventListener("stream-done", ev => {
    const d = JSON.parse(ev.data);
    if (d.requestId !== aiStreamId) return;
    finishStream();
  });
  sse.addEventListener("stream-error", ev => {
    const d = JSON.parse(ev.data);
    if (d.requestId !== aiStreamId) return;
    announce(d.message); finishStream();
  });
}
function finishStream() {
  if (aiBubble) aiHistory.push(["ai", aiBubble.textContent]);
  aiStreamId = null; aiBubble = null;
  $("#aiStop").hidden = true; $("#aiSend").hidden = false;
}
async function aiSend() {
  const msg = $("#aiInput").value.trim();
  const ctx = await contextFor(aiScope);
  if (aiMode === "chat" && !msg) { announce(t("ui.chat.placeholder")); return; }
  if (!ctx.text && aiScope !== "selection") { announce(t("ui.need.document")); return; }
  if (aiScope === "selection" && !ctx.text) { announce(t("ui.need.selection")); return; }
  const action = aiMode;
  const preview = await rpc("preview-ai-send", { action, scope: aiScope, contentText: ctx.text, history: aiHistory, userMessage: msg });
  aiPending = { action, ctx, msg };
  $("#pvBody").textContent = t("ui.ai-preview.body",
    t("ui.ai-preview.action." + action), t("ui.ai-preview.scope." + (aiScope === "document" ? "document" : aiScope)),
    preview.contentChars, preview.historyChars, preview.messageChars, preview.totalChars);
  $("#aiPreview").showModal();
}
async function aiContinue() {
  $("#aiPreview").close();
  const { action, ctx, msg } = aiPending; aiPending = null;
  ensureSse();
  $("#aiSend").hidden = true; $("#aiStop").hidden = false;
  if (action === "chat") {
    addBubble(t("ui.chat.you"), msg, "me");
    aiHistory.push(["user", msg]);
    aiBubble = addBubble("BOOKLIGHT · " + (settings ? settings.model : ""), "", "ai");
    const st = await rpc("chat", { contextText: ctx.text, contextLabel: ctx.label, history: aiHistory.slice(0, -1), userMessage: msg });
    aiStreamId = st.requestId;
    $("#aiInput").value = "";
  } else if (action === "summarize") {
    const mode = aiScope === "selection" ? "selection" : aiScope === "document" ? "doc" : "page";
    aiBubble = addBubble(t("ui.tab.summarize"), "", "ai");
    const st = await rpc("summarize-text", { text: ctx.text, mode });
    aiStreamId = st.requestId;
  } else {
    aiBubble = addBubble(t("ui.tab.translate"), "", "ai");
    const st = await rpc("translate-text", { text: ctx.text, target: settings ? settings.targetLanguage : "zh" });
    aiStreamId = st.requestId;
  }
}
async function aiStop() {
  if (aiStreamId != null) await rpc("stop-stream", { requestId: aiStreamId });
  finishStream();
}
function toggleInspector() {
  const ins = $("#inspector");
  ins.hidden = !ins.hidden;
}

function wireS4() {
  $("#settingsBtn").addEventListener("click", showSettings);
  $("#researchBtn").addEventListener("click", showResearch);
  $("#aiBtn").addEventListener("click", toggleInspector);
  $("#saveSettingsBtn").addEventListener("click", () => saveSettings().catch(e => announce(t("ui.settings.save-failed") + " " + e.message)));
  $("#saveKeyBtn").addEventListener("click", () => saveKey().catch(e => announce(e.message)));
  $("#testBtn").addEventListener("click", testConnection);
  $("#setPreset").addEventListener("change", () => {
    const p = presets.find(x => x.id === $("#setPreset").value);
    if (p) { $("#setBaseUrl").value = p.baseUrl; $("#setModel").value = p.defaultModel; }
  });
  $$("#setTheme button").forEach(b => b.addEventListener("click", async () => {
    $$("#setTheme button").forEach(x => x.classList.toggle("on", x === b));
    await setReadTheme(b.dataset.v);
  }));
  $$("#setView button").forEach(b => b.addEventListener("click", () => {
    $$("#setView button").forEach(x => x.classList.toggle("on", x === b));
  }));
  $("#setLocale").addEventListener("change", async e => {
    const v = e.target.value;
    lang = v === "system" ? "zh" : v;
    localStorage.setItem("booklight-lang", lang);
    await rpc("set-locale", { code: lang });
    applyI18n();
  });
  $("#resSearch").addEventListener("click", () => runSearch().catch(e => announce(e.message)));
  $("#resQuery").addEventListener("keydown", e => { if (e.key === "Enter") runSearch().catch(err => announce(err.message)); });
  $$("#inspector .itab").forEach(tb => tb.addEventListener("click", () => {
    aiMode = tb.dataset.itab;
    $$("#inspector .itab").forEach(x => x.classList.toggle("active", x === tb));
  }));
  $$("#aiScope button").forEach(b => b.addEventListener("click", () => {
    aiScope = b.dataset.scope;
    $$("#aiScope button").forEach(x => x.classList.toggle("on", x === b));
  }));
  $("#aiSend").addEventListener("click", () => aiSend().catch(e => announce(t("ui.ai-preview.failed") + " " + e.message)));
  $("#aiStop").addEventListener("click", () => aiStop().catch(e => announce(e.message)));
  $("#pvCancel").addEventListener("click", () => { $("#aiPreview").close(); aiPending = null; });
  $("#pvContinue").addEventListener("click", () => aiContinue().catch(e => { announce(e.message); finishStream(); }));
}

// ---- S4 e2e journey steps (appended by runJourney) ------------------------
async function runS4Steps(step) {
  await step("s4-settings-open", async () => {
    await showSettings();
    if ($("#view-settings").hidden) throw new Error("settings hidden");
    if (!$$("#setPreset option").length) throw new Error("no presets");
    return $$("#setPreset option").length;
  });
  await step("s4-settings-save", async () => {
    $$("#setView button").forEach(x => x.classList.toggle("on", x.dataset.v === "double"));
    await saveSettings();
    if (settings.viewMode !== "double") throw new Error("viewMode not saved");
    return settings.viewMode;
  });
  await step("s4-settings-theme", async () => {
    const b = $$("#setTheme button").find(x => x.dataset.v === "sepia");
    b.click(); await new Promise(r => setTimeout(r, 200));
    if (readTheme !== "sepia") throw new Error("theme not saved");
    return readTheme;
  });
  await step("s4-research-open", async () => {
    await showResearch();
    if ($("#view-research").hidden) throw new Error("research hidden");
    return docs.length;
  });
  await step("s4-research-search", async () => {
    $("#resQuery").value = "Alpha";
    const n = await runSearch();
    if (!n) throw new Error("no hits");
    if (!$$("#resResults .rcard").length) throw new Error("no cards");
    return n;
  });
  await step("s4-research-pin", async () => {
    const before = await renderDeck();
    await pinHit(resHits[0]);
    const after = await renderDeck();
    if (after !== before + 1) throw new Error("deck not grown");
    return after;
  });
  await step("s4-ai-open", async () => {
    await openPath(docs.find(d => d.kind === "epub").path);
    toggleInspector();
    if ($("#inspector").hidden) throw new Error("inspector hidden");
    return aiMode;
  });
  await step("s4-ai-preview", async () => {
    $("#aiInput").value = "What is this chapter about?";
    await aiSend();
    if (!$("#aiPreview").open) throw new Error("preview not open");
    const txt = $("#pvBody").textContent;
    if (!txt || txt.includes("{0}")) throw new Error("preview body unfilled");
    $("#pvCancel").click();
    return txt.length;
  });
}

// Pixel-evidence stage for S4: research results + citation deck on screen.
async function runS4Stage() {
  const steps = {};
  const step = async (id, fn) => {
    try { const detail = await fn(); steps[id] = { ok: true, detail: String(detail ?? "") }; }
    catch (err) { steps[id] = { ok: false, detail: err.message }; }
  };
  const fx = await post("/api/host/e2e_fixtures", {});
  await step("open-epub", async () => { await openPath(fx.json.epub); return active.view.chapters; });
  await step("open-pdf", async () => { await openPath(fx.json.pdf); return active.pageCount; });
  await step("research", async () => {
    await showResearch();
    $("#resQuery").value = "Alpha";
    const n = await runSearch();
    if (!n) throw new Error("no hits");
    await pinHit(resHits[0]);
    return n;
  });
  const ok = Object.values(steps).every(s => s.ok);
  await post("/api/host/e2e_report", { payload: { phase: "done", ok, stepCount: Object.keys(steps).length, steps,
    state: { researchHidden: $("#view-research").hidden, cards: $$("#resResults .rcard").length, deck: $$("#deckList .ccard").length } } });
}

// ---- drop mask (welcome/drop-mask) -----------------------------------------
let dragDepth = 0;
function showDropMask(on) { $("#dropmask").hidden = !on; }
function wireDrop() {
  window.addEventListener("dragenter", e => { e.preventDefault(); dragDepth++; showDropMask(true); });
  window.addEventListener("dragover", e => { e.preventDefault(); showDropMask(true); });
  window.addEventListener("dragleave", e => { e.preventDefault(); dragDepth = Math.max(0, dragDepth - 1); if (!dragDepth) showDropMask(false); });
  window.addEventListener("drop", async e => {
    e.preventDefault(); dragDepth = 0; showDropMask(false);
    const f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (!f) return;
    try { await openDropped(f); }
    catch (err) { announce(t("ui.open-failed") + " " + err.message); }
  });
}
async function openDropped(f) {
  if (!/\.(pdf|epub)$/i.test(f.name)) throw new Error(t("ui.filter.documents"));
  const buf = new Uint8Array(await f.arrayBuffer());
  const sc = await post("/api/host/scratch", { data: bytesToB64(buf) });
  const dest = dirName(sc.json.path) + f.name;
  await post("/api/host/write", { path: dest, data: bytesToB64(buf) });
  await openPath(dest);
}

// ---- S5 reading-quality pixel evidence ------------------------------------
function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}
function hexToRgb(hex) {
  const h = hex.replace("#", "");
  const n = parseInt(h.length === 3 ? h.split("").map(c => c + c).join("") : h, 16);
  return "rgb(" + ((n >> 16) & 255) + ", " + ((n >> 8) & 255) + ", " + (n & 255) + ")";
}
function tokenColor(name) {
  const d = document.createElement("div");
  d.style.background = cssVar(name);
  document.body.appendChild(d);
  const c = getComputedStyle(d).backgroundColor;
  d.remove();
  return c;
}
function readerBg() { return getComputedStyle($("#reader")).backgroundColor; }
function readerInk() { return getComputedStyle($("#reader")).color; }
async function runS5Stage() {
  const steps = {};
  const step = async (id, fn) => {
    try { const detail = await fn(); steps[id] = { ok: true, detail: String(detail ?? "") }; }
    catch (err) { steps[id] = { ok: false, detail: err.message }; }
  };
  const fx = await post("/api/host/e2e_fixtures", {});
  settings = await rpc("save-reading-prefs", { theme: "paper", fontScalePermille: 1000 });
  readTheme = "paper"; fontPermille = 1000; applyTheme();
  await step("open-pdf", async () => { await openPath(fx.json.pdf); await new Promise(r => setTimeout(r, 400)); return active.pageCount; });
  await step("ink-token", async () => {
    const spans = active.shell.tl.querySelectorAll("span");
    const range = document.createRange();
    range.setStart(spans[0], 0); range.setEnd(spans[1], spans[1].childNodes.length || 1);
    const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
    onPdfSelect(); await addAnnotation("yellow", "highlight");
    window.getSelection().removeAllRanges();
    const hl = $("#prose .hl-layer .hl.highlight.yellow");
    if (!hl) throw new Error("no ink");
    const got = getComputedStyle(hl).backgroundColor;
    const want = tokenColor("--hl-yellow");
    if (got !== want) throw new Error("ink " + got + " != " + want);
    if (getComputedStyle(hl).opacity !== "0.45") throw new Error("ink opacity");
    return got;
  });
  await step("underline-accent", async () => {
    const spans = active.shell.tl.querySelectorAll("span");
    const range = document.createRange();
    range.setStart(spans[1], 0); range.setEnd(spans[Math.min(2, spans.length - 1)], 1);
    const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
    onPdfSelect(); await addAnnotation("", "underline");
    window.getSelection().removeAllRanges();
    const ul = $$("#prose .hl-layer .hl.underline").pop();
    if (!ul) throw new Error("no underline ink");
    const got = getComputedStyle(ul).borderBottomColor;
    if (got !== tokenColor("--accent")) throw new Error("underline " + got);
    return got;
  });
  await step("sel-precision", async () => {
    const spans = active.shell.tl.querySelectorAll("span");
    const range = document.createRange();
    range.setStart(spans[0], 0); range.setEnd(spans[1], spans[1].childNodes.length || 1);
    const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
    onPdfSelect();
    const want = [...range.getClientRects()].filter(r => r.width > 1 && r.height > 1);
    await addAnnotation("blue", "highlight");
    window.getSelection().removeAllRanges();
    const got = $$("#prose .hl-layer .hl.highlight.blue").slice(-want.length).map(el => el.getBoundingClientRect());
    if (got.length !== want.length) throw new Error("ink rects " + got.length + " != " + want.length);
    let worst = 1;
    for (let i = 0; i < want.length; i++) {
      const a = want[i], g = got[i];
      const ix = Math.max(0, Math.min(a.right, g.right) - Math.max(a.left, g.left));
      const iy = Math.max(0, Math.min(a.bottom, g.bottom) - Math.max(a.top, g.top));
      const inter = ix * iy;
      const ua = a.width * a.height + g.width * g.height - inter;
      worst = Math.min(worst, inter / ua);
    }
    if (worst < 0.8) throw new Error("ink IoU " + worst.toFixed(2));
    return worst.toFixed(2);
  });
  await step("open-epub", async () => { await openPath(fx.json.epub); return active.view.chapters; });
  await step("measure", () => {
    const cs = getComputedStyle($("#prose"));
    if (cs.maxWidth !== "608px") throw new Error("measure " + cs.maxWidth);
    if (cs.paddingTop !== "40px") throw new Error("padding " + cs.paddingTop);
    return cs.maxWidth;
  });
  await step("serif", () => {
    const p = $("#prose p");
    if (!p) throw new Error("no prose");
    const cs = getComputedStyle(p);
    if (!/Georgia/.test(cs.fontFamily) || !/serif\s*$/.test(cs.fontFamily)) throw new Error("family " + cs.fontFamily);
    const ratio = parseFloat(cs.lineHeight) / parseFloat(cs.fontSize);
    if (Math.abs(ratio - 1.9) > 0.01) throw new Error("line-height " + ratio);
    if (cs.textAlign !== "justify") throw new Error("align " + cs.textAlign);
    return cs.fontFamily;
  });
  await step("citeblock", () => {
    const bq = document.createElement("blockquote");
    bq.textContent = "A cited passage.";
    $("#prose").appendChild(bq);
    const cs = getComputedStyle(bq);
    const fontSize = cs.fontSize, lh = cs.lineHeight, blw = cs.borderLeftWidth, op = cs.opacity, ff = cs.fontFamily;
    bq.remove();
    const ratio = parseFloat(lh) / parseFloat(fontSize);
    const ok = fontSize === "15px" && Math.abs(ratio - 1.8) < 0.01 &&
      Math.abs(parseFloat(blw) - 3) < 0.5 && op === "0.88" && /Georgia/.test(ff);
    if (!ok) throw new Error("citeblock " + fontSize + "/" + ratio + "/" + blw + "/" + op);
    return fontSize;
  });
  await step("pagination", async () => {
    const widths = [];
    for (const ch of [1, 2]) {
      await gotoChapter(ch);
      const m = getComputedStyle($("#prose"));
      widths.push(m.maxWidth);
      if ($("#prose").scrollWidth > $("#prose").clientWidth + 1) throw new Error("h-overflow ch" + ch);
    }
    if (widths[0] !== widths[1] || widths[0] !== "608px") throw new Error("measure drift " + widths);
    await gotoChapter(1);
    return widths.join(",");
  });
  await step("scroll-feel", async () => {
    const r = $("#reader");
    if (getComputedStyle(r).scrollBehavior !== "smooth") throw new Error("scroll-behavior");
    const saved = $("#prose").innerHTML;
    for (let i = 0; i < 40; i++) {
      const p = document.createElement("p");
      p.textContent = "Scroll feel probe paragraph " + i + ". The quick brown fox jumps over the lazy dog.";
      $("#prose").appendChild(p);
    }
    r.scrollTop = 240;
    await new Promise(res => setTimeout(res, 500));
    if (r.scrollTop <= 0) throw new Error("not scrollable");
    const top = r.scrollTop;
    $("#prose").innerHTML = saved;
    r.scrollTop = 0;
    return top;
  });
  await step("theme-paper", async () => {
    await setReadTheme("paper");
    if (readerBg() !== tokenColor("--reading-light-bg")) throw new Error("paper bg " + readerBg());
    if (readerInk() !== tokenColor("--reading-light-ink")) throw new Error("paper ink");
    return readerBg();
  });
  await step("theme-night", async () => {
    await setReadTheme("night");
    if (readerBg() !== tokenColor("--reading-dark-bg")) throw new Error("night bg " + readerBg());
    if (readerInk() !== tokenColor("--reading-dark-ink")) throw new Error("night ink");
    return readerBg();
  });
  await step("font-scale", async () => {
    settings = await rpc("save-reading-prefs", { theme: readTheme, fontScalePermille: 1200 });
    fontPermille = 1200; applyTheme();
    const px = parseFloat(getComputedStyle($("#prose p")).fontSize);
    if (Math.abs(px - 17 * 1.2) > 0.01) throw new Error("font px " + px);
    settings = await rpc("save-reading-prefs", { theme: readTheme, fontScalePermille: 1000 });
    fontPermille = 1000; applyTheme();
    return px;
  });
  await step("theme-sepia", async () => {
    await setReadTheme("sepia");
    if (readerBg() !== tokenColor("--reading-sepia-bg")) throw new Error("sepia bg " + readerBg());
    return readerBg();
  });
  const ok = Object.values(steps).every(s => s.ok);
  await post("/api/host/e2e_report", { payload: { phase: "done", ok, stepCount: Object.keys(steps).length, steps,
    state: { theme: readTheme, bg: readerBg(), ink: readerInk(),
      fontSize: getComputedStyle($("#prose p")).fontSize,
      lineHeight: getComputedStyle($("#prose p")).lineHeight,
      measure: getComputedStyle($("#prose")).maxWidth } } });
}

// Pixel-evidence stage for the drop mask: leave the mask over the library.
async function runDropStage() {
  const steps = {};
  const step = async (id, fn) => {
    try { const detail = await fn(); steps[id] = { ok: true, detail: String(detail ?? "") }; }
    catch (err) { steps[id] = { ok: false, detail: err.message }; }
  };
  const fx = await post("/api/host/e2e_fixtures", {});
  const f = await post("/api/host/file", { path: fx.json.epub });
  const bytes = b64ToBytes(f.json.data);
  const mkEvent = (type, withFile) => {
    const dt = new DataTransfer();
    if (withFile) dt.items.add(new File([bytes], "sample.epub", { type: "application/epub+zip" }));
    return new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt });
  };
  await step("mask-show", () => {
    renderLibrary();
    window.dispatchEvent(mkEvent("dragover", true));
    if ($("#dropmask").hidden) throw new Error("mask hidden");
    return $(".dm-t").textContent;
  });
  await step("drop-open", async () => {
    window.dispatchEvent(mkEvent("drop", true));
    // The drop handler opens the file asynchronously (scratch, write, parse)
    // and a DOM event cannot be awaited: poll for the result instead of
    // sleeping a fixed interval, which failed on a loaded macOS runner.
    const deadline = performance.now() + 8000;
    while ((!active || active.kind !== "epub") && performance.now() < deadline) {
      await new Promise(r => setTimeout(r, 100));
    }
    if (!active || active.kind !== "epub") throw new Error("drop did not open");
    return active.title;
  });
  await step("mask-again", () => {
    renderLibrary();
    window.dispatchEvent(mkEvent("dragover", true));
    if ($("#dropmask").hidden) throw new Error("mask hidden again");
    return true;
  });
  const ok = Object.values(steps).every(s => s.ok);
  await post("/api/host/e2e_report", { payload: { phase: "done", ok, stepCount: Object.keys(steps).length, steps,
    state: { maskHidden: $("#dropmask").hidden, libHidden: $("#view-library").hidden } } });
}

boot();