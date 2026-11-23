// Booklight S1 carrier probe (#75). Exercises the 34-RPC surface over the
// loopback HTTP adapter and reports a typed verdict to the host harness.
// Journeys run only when BOTH ?e2e=1 and the host gate (BOOKLIGHT_E2E=1)
// are on; otherwise the page stays an idle boot probe and never overwrites
// an interactive status.
"use strict";

const statusEl = document.getElementById("status");
const stepsEl = document.getElementById("steps");
const params = new URLSearchParams(location.search);
const e2e = params.get("e2e") === "1";

const steps = [];
function record(id, ok, detail) {
  steps.push({ id, ok, detail });
  const li = document.createElement("li");
  li.className = ok ? "ok" : "bad";
  li.innerHTML =
    `<span class="mark">${ok ? "✓" : "✗"}</span>` +
    `<span class="name"></span><span class="detail"></span>`;
  li.querySelector(".name").textContent = id;
  li.querySelector(".detail").textContent = detail || "";
  stepsEl.appendChild(li);
}

async function rpc(name, args) {
  const res = await fetch("/api/rpc/" + name, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(args || {}),
  });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch (_) { /* non-JSON */ }
  return { status: res.status, json, text };
}

async function post(path, body) {
  const res = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body || {}),
  });
  return { status: res.status, json: await res.json().catch(() => null) };
}

function setStatus(text, cls) {
  statusEl.textContent = text;
  statusEl.className = "status" + (cls ? " " + cls : "");
}

async function boot() {
  const health = await post("/api/health", {});
  const gate = await post("/api/host/e2e", {});
  const enabled = !!(gate.json && gate.json.enabled);
  if (!e2e || !enabled) {
    setStatus(
      `probe ok · version ${health.json && health.json.version} · ` +
      (e2e ? "host gate off" : "idle (add ?e2e=1)"),
      "ok"
    );
    return;
  }
  setStatus("journey running…");
  const fixtures = (await post("/api/host/e2e_fixtures", {})).json || {};
  const report = { phase: "running", ok: false, steps: {} };
  const push = async () => {
    report.steps = {};
    for (const s of steps) report.steps[s.id] = { ok: s.ok, detail: s.detail };
    await post("/api/host/e2e_report", { payload: report });
  };

  async function step(id, fn) {
    try {
      const detail = await fn();
      record(id, true, detail);
    } catch (e) {
      record(id, false, String((e && e.message) || e));
    }
    await push();
  }

  const assert = (cond, msg) => { if (!cond) throw new Error(msg); };

  await step("initialize", async () => {
    const r = await rpc("initialize", {});
    assert(r.status === 200, "status " + r.status);
    return "void";
  });
  await step("get-settings", async () => {
    const r = await rpc("get-settings", {});
    assert(r.status === 200 && r.json, "status " + r.status);
    return "SettingsView";
  });
  await step("list-presets", async () => {
    const r = await rpc("list-presets", {});
    assert(r.status === 200 && Array.isArray(r.json), "status " + r.status);
    return r.json.length + " presets";
  });
  await step("epub-open", async () => {
    const r = await rpc("epub-open", { path: fixtures.epub });
    assert(r.status === 200 && r.json, "status " + r.status + " " + r.text);
    return "EpubView";
  });
  await step("epub-chapter-text", async () => {
    // chapter indices are 1-based in the domain core
    const r = await rpc("epub-chapter-text", { path: fixtures.epub, index: 1 });
    assert(r.status === 200 && r.json && r.json.length > 0,
           "status " + r.status + " " + (r.json && r.json.error));
    return r.json.length + " chars";
  });
  await step("epub-chapter-html", async () => {
    const r = await rpc("epub-chapter-html", { path: fixtures.epub, index: 1 });
    assert(r.status === 200 && typeof r.json === "string",
           "status " + r.status + " " + (r.json && r.json.error));
    return "base64 " + r.json.length;
  });
  await step("split-paragraphs", async () => {
    // paragraphs merge lines up to a 400-char budget; three long lines
    // must stay three paragraphs
    const text = Array(3).fill("paragraph line ".repeat(40)).join("\n\n");
    const r = await rpc("split-paragraphs", { text });
    assert(r.status === 200 && r.json.length === 3, "paras " + (r.json || []).length);
    return "3 paras";
  });
  await step("update-recents", async () => {
    const r = await rpc("update-recents", { path: fixtures.epub, page: 0, scrollRatioScaled: 0 });
    assert(r.status === 200, "status " + r.status);
    const g = await rpc("get-recents", {});
    assert(g.status === 200 && g.json.length >= 1, "recents empty");
    return g.json.length + " recents";
  });
  await step("pdf-rotate-bytes", async () => {
    const r = await rpc("edit-rotate-pages", { path: fixtures.pdf, pages: [1], delta: 90 });
    assert(r.status === 200 && typeof r.json === "string", "status " + r.status);
    assert(r.json.startsWith("JVBER"), "not a PDF base64");
    return "base64 " + r.json.length;
  });
  await step("annotations-roundtrip", async () => {
    const g = await rpc("get-annotations", { pdfPath: fixtures.pdf, sidecar: true });
    assert(g.status === 200 && typeof g.json === "string", "get status " + g.status);
    const s = await rpc("set-annotations", { pdfPath: fixtures.pdf, sidecar: true, data: g.json });
    assert(s.status === 200, "set status " + s.status);
    const l = await rpc("load-annotations", { pdfPath: fixtures.pdf, sidecar: true });
    assert(l.status === 200 && l.json, "load status " + l.status);
    return "AnnotationDocument";
  });
  await step("ai-preview-counts", async () => {
    const r = await rpc("preview-ai-send", {
      action: "translate", scope: "selection", contentText: "hello world",
      history: [], userMessage: "hi",
    });
    assert(r.status === 200 && r.json, "status " + r.status);
    return "AISendPreview";
  });
  await step("error-channel", async () => {
    const r = await rpc("epub-open", { path: "/nonexistent/booklight-probe.epub" });
    assert(r.status === 400, "expected 400, got " + r.status);
    assert(r.json && r.json.error, "no backend message");
    return "400: " + String(r.json.error).slice(0, 40);
  });

  report.phase = "done";
  report.ok = steps.every((s) => s.ok);
  report.stepCount = steps.length;
  await post("/api/host/e2e_report", { payload: report });
  setStatus(
    report.ok ? `journey ok · ${steps.length} steps` : `journey FAILED · see steps`,
    report.ok ? "ok" : "bad"
  );
}

window.addEventListener("error", (ev) => setStatus("probe error: " + ev.message, "bad"));
window.addEventListener("unhandledrejection", (ev) =>
  setStatus("probe rejection: " + ev.reason, "bad"));
boot().catch((e) => setStatus("probe boot failed: " + e, "bad"));