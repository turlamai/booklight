/* Booklight pdf.js spike (#75 S0). Render-only pdf.js inside the glaze
 * webview: canvas + text layer + native selection mapped to highlight
 * overlays, then re-render of bytes edited by the kept Racket pdfdoc
 * (host route api/spike/edited-pdf). Page surgery never moves into JS.
 * Tokens come from design/prototype/shared.css; no new value table.
 * The status line is CI evidence: stepwise progress + page-level error
 * hooks, because headless webviews give us no console channel. */
(function () {
  'use strict';
  var pagesEl = document.getElementById('spike-pages');
  var statusEl = document.getElementById('spike-status');
  var e2e = window.location.hash === '#e2e' ||
    new URLSearchParams(window.location.search).has('e2e');
  var report = { phase: 'running', ok: false, failures: [] };
  var posted = false;

  function status(text) { statusEl.textContent = text; }
  function fail(name, err) {
    report.failures.push(name + ': ' + (err && err.message ? err.message : err));
  }
  function postNow() {
    if (!e2e || posted) return;
    posted = true;
    report.phase = 'done';
    report.ok = report.failures.length === 0;
    window.__spikeReport = report;
    fetch('/api/spike/report', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ payload: report })
    }).then(function (r) {
      status((report.ok ? 'spike ok' : 'spike FAILED') + ' post=' + r.status);
    }).catch(function (e) {
      status('spike post ERROR: ' + (e && e.message ? e.message : e));
    });
  }
  function fatal(name, err) {
    fail(name, err);
    status('ERR ' + name + ': ' + (err && err.message ? err.message : err));
    postNow();
  }
  window.addEventListener('error', function (ev) {
    fatal('page-error', ev.message || ev.error);
  });
  window.addEventListener('unhandledrejection', function (ev) {
    fatal('unhandled-rejection', ev.reason);
  });

  var lib = window.pdfjsLib;
  if (!lib) { fatal('pdfjs-lib', 'window.pdfjsLib missing (script load/parse failed)'); return; }
  lib.GlobalWorkerOptions.workerSrc =
    'vendor/pdfjs-dist/legacy/build/pdf.worker.min.js';
  status('lib ok');
  var FONTS = {
    cMapUrl: 'vendor/pdfjs-dist/cmaps/',
    cMapPacked: true,
    standardFontDataUrl: 'vendor/pdfjs-dist/standard_fonts/'
  };

  function pageShell(viewport) {
    var wrap = document.createElement('div');
    wrap.className = 'pdf-page';
    var canvas = document.createElement('canvas');
    var tl = document.createElement('div');
    tl.className = 'textLayer';
    tl.style.setProperty('--scale-factor', viewport.scale);
    var hl = document.createElement('div');
    hl.className = 'hl-layer';
    wrap.appendChild(canvas);
    wrap.appendChild(tl);
    wrap.appendChild(hl);
    pagesEl.appendChild(wrap);
    return { wrap: wrap, canvas: canvas, tl: tl, hl: hl };
  }

  function renderPage(page, shell) {
    var viewport = page.getViewport({ scale: 1.5 });
    shell.canvas.width = viewport.width;
    shell.canvas.height = viewport.height;
    shell.canvas.style.width = viewport.width + 'px';
    shell.canvas.style.height = viewport.height + 'px';
    shell.tl.style.width = viewport.width + 'px';
    shell.tl.style.height = viewport.height + 'px';
    var task = page.render({
      canvasContext: shell.canvas.getContext('2d'),
      viewport: viewport
    });
    var textTask = lib.renderTextLayer({
      textContentSource: page.streamTextContent(),
      container: shell.tl,
      viewport: viewport
    });
    return Promise.all([task.promise, textTask.promise]).then(function () {
      return viewport;
    });
  }

  function overlaysFromRange(range, shell) {
    var base = shell.wrap.getBoundingClientRect();
    var rects = range.getClientRects();
    var n = 0;
    for (var i = 0; i < rects.length; i++) {
      var r = rects[i];
      if (r.width < 1 || r.height < 1) continue;
      var div = document.createElement('div');
      div.className = 'hl';
      div.style.left = (r.left - base.left) + 'px';
      div.style.top = (r.top - base.top) + 'px';
      div.style.width = r.width + 'px';
      div.style.height = r.height + 'px';
      shell.hl.appendChild(div);
      n++;
    }
    return n;
  }

  function clearOverlays(shell) { shell.hl.textContent = ''; }

  function interactiveSelection(shells) {
    document.addEventListener('mouseup', function () {
      var sel = window.getSelection();
      if (!sel || sel.isCollapsed || sel.rangeCount === 0) return;
      var range = sel.getRangeAt(0);
      for (var i = 0; i < shells.length; i++) {
        if (shells[i].tl.contains(range.commonAncestorContainer)) {
          clearOverlays(shells[i]);
          overlaysFromRange(range, shells[i]);
          return;
        }
      }
    });
  }

  function withTimeout(promise, ms, label) {
    return Promise.race([
      promise,
      new Promise(function (_, reject) {
        setTimeout(function () { reject(new Error(label + ' stalled ' + ms + 'ms')); }, ms);
      })
    ]);
  }

  var t0 = performance.now();
  fetch('fixture.pdf').then(function (r) {
    if (!r.ok) throw new Error('fixture.pdf HTTP ' + r.status);
    return r.arrayBuffer();
  }).then(function (buf) {
    report.fixtureBytes = buf.byteLength;
    status('fixture ' + buf.byteLength + ' bytes');
    return withTimeout(
      lib.getDocument(Object.assign({ data: new Uint8Array(buf) }, FONTS)).promise,
      45000, 'getDocument(worker?)');
  })
    .then(function (pdf) {
      report.pageCount = pdf.numPages;
      status('doc open pages=' + pdf.numPages);
      var shells = [];
      var chain = Promise.resolve();
      for (var i = 1; i <= pdf.numPages; i++) {
        (function (n) {
          chain = chain.then(function () {
            return pdf.getPage(n);
          }).then(function (page) {
            var shell = pageShell(page.getViewport({ scale: 1.5 }));
            shells.push(shell);
            return renderPage(page, shell).then(function () {
              status('rendered page ' + n);
            });
          });
        })(i);
      }
      return chain.then(function () {
        report.renderMs = Math.round(performance.now() - t0);
        report.textSpans = shells[0].tl.querySelectorAll('span').length;
        if (!e2e) {
          interactiveSelection(shells);
          status('ready — select text');
          return;
        }
        // journey 1: native selection over the text layer -> overlays
        var spans = shells[0].tl.querySelectorAll('span');
        if (spans.length < 2) throw new Error('text layer has <2 spans');
        var range = document.createRange();
        range.setStart(spans[0], 0);
        range.setEnd(spans[1], spans[1].childNodes.length || 1);
        var sel = window.getSelection();
        sel.removeAllRanges();
        sel.addRange(range);
        report.selectionChars = sel.toString().length;
        report.highlightRects = overlaysFromRange(range, shells[0]);
        // Drop the native selection so the capture shows the highlight ink
        // (content color), which is what the production surface paints.
        sel.removeAllRanges();
        if (report.selectionChars < 10) throw new Error('selection text too short');
        if (report.highlightRects < 1) throw new Error('no highlight overlays');
        // journey 2: domain-core edited bytes re-render (rotate page 1 = 90)
        return fetch('api/spike/edited-pdf').then(function (r) {
          if (!r.ok) throw new Error('edited-pdf HTTP ' + r.status);
          return r.arrayBuffer();
        }).then(function (buf) {
          return withTimeout(
            lib.getDocument(Object.assign({ data: new Uint8Array(buf) }, FONTS)).promise,
            30000, 'getDocument(edited)');
        }).then(function (edited) {
          report.editedPageCount = edited.numPages;
          return edited.getPage(1);
        }).then(function (page1) {
          report.editedRotation = page1.getViewport({ scale: 1 }).rotation;
          if (report.editedRotation !== 90) {
            throw new Error('edited page 1 rotation ' + report.editedRotation + ' != 90');
          }
        });
      });
    })
    .catch(function (e) { fail('spike', e); })
    .then(function () {
      report.ok = report.failures.length === 0;
      if (!e2e) {
        window.__spikeReport = report;
        if (!report.ok) status('spike FAILED: ' + report.failures.join(' | '));
        return;
      }
      postNow();
    });
})();