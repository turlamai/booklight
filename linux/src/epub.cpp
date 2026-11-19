// EPUB over WebKitGTK — see epub.hpp.
#include "epub.hpp"

#ifdef PDFGIST_HAVE_WEBKIT

#include <jsc/jsc.h>
#include <json-glib/json-glib.h>
// WebKitGTK 2.46+ installs the umbrella as webkit/webkit.h; older 6.0
// packages (2.40-2.44) shipped webkit/WebKit.h.
#if __has_include(<webkit/webkit.h>)
#include <webkit/webkit.h>
#else
#include <webkit/WebKit.h>
#endif

#include <algorithm>
#include <thread>

#include "host.hpp"
#include "i18n.h"

namespace pdfgist {

// g_signal_connect is a GLib macro; a lambda whose body contains top-level
// commas would break its argument splitting. Route through the real
// g_signal_connect_data function instead (handlers cast to GCallback).
inline gulong hook(gpointer instance, gchar const* detailed_signal,
                   auto&& handler, gpointer data,
                   GClosureNotify destroy_data, GConnectFlags flags) {
  return g_signal_connect_data(instance, detailed_signal,
                               reinterpret_cast<GCallback>(
                                   std::forward<decltype(handler)>(handler)),
                               data, destroy_data, flags);
}

inline gulong hook(gpointer instance, gchar const* detailed_signal,
                   auto&& handler, gpointer data) {
  return hook(instance, detailed_signal, std::forward<decltype(handler)>(handler),
              data, nullptr, static_cast<GConnectFlags>(0));
}
namespace {

std::string escape_html(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  for (char const c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string js_string(std::string const& s) {
  std::string out = "\"";
  for (char const c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  out += "\"";
  return out;
}

// v1 READING_CSS typography + theme; the marks helpers power search.
// Typography is the design contract (design/DESIGN.md invariant 2 +
// design/prototype/reading.html): ~608px measure, 1.9 line height,
// justified serif body. --rline/--rsel/--uacc follow the reading theme,
// which is orthogonal to the window theme. The selection bar is UI chrome,
// so it uses the UI font stack and the reading-theme surface pair.
char const* kReadingCss = R"CSS(
:root { --fs: 1; }
:root[data-theme="light"] { --fg: #292524; --bg: #F5F2ED;
  --rline: rgba(0,0,0,.08); --rsel: rgba(196,98,62,.16); --uacc: #C4623E;
  --bar-bg: #FFFDF9; --bar-hover: #F2EEE7; --bar-line: rgba(0,0,0,.08); --bar-ink: #292524; }
:root[data-theme="sepia"] { --fg: #57534E; --bg: #EFE6D5;
  --rline: rgba(0,0,0,.10); --rsel: rgba(196,98,62,.16); --uacc: #C4623E;
  --bar-bg: #FFFDF9; --bar-hover: #F2EEE7; --bar-line: rgba(0,0,0,.10); --bar-ink: #292524; }
:root[data-theme="dark"] { --fg: #D6D3D1; --bg: #171412;
  --rline: rgba(255,255,255,.10); --rsel: rgba(232,146,106,.20); --uacc: #E8926A;
  --bar-bg: #2A2521; --bar-hover: #322C27; --bar-line: rgba(255,255,255,.08); --bar-ink: #D6D3D1; }
html { background: var(--bg); }
body { margin: 0; padding: 0 0 72px; color: var(--fg);
  font-family: Georgia, "Source Han Serif SC", "Noto Serif SC", "Songti SC", serif;
  font-size: calc(17px * var(--fs)); line-height: 1.9; }
p { margin: 0 0 1em; text-align: justify; }
h1, h2, h3, h4 { line-height: 1.35; margin: 1.4em 0 0.6em; }
img, svg { max-width: 100%; height: auto; }
blockquote { margin: 1em 0; padding-left: 1em; font-size: 15px;
  line-height: 1.8; border-left: 3px solid var(--rline); opacity: .88; }
a { color: inherit; pointer-events: none; }
::selection { background: var(--rsel); }
.epub-book-title { max-width: 608px; box-sizing: border-box; margin: 0 auto;
  padding: 16vh 24px 8vh; text-align: center; }
.book-title-text { font-size: 2em; font-weight: 700; }
.epub-chapter { max-width: 608px; box-sizing: border-box; margin: 0 auto;
  padding: 0 24px; }
mark.epub-hit { background: rgba(255,213,0,.9); color: inherit; }
/* Selection bar (design/prototype/reading.html .selbar): direct
   manipulation; UI chrome never borrows the page's serif. */
.selbar { position: absolute; display: none; align-items: center; gap: 4px;
  padding: 5px 7px; border-radius: 8px; background: var(--bar-bg);
  border: 1px solid var(--bar-line); box-shadow: 0 4px 14px rgba(23,20,18,.16);
  z-index: 5; font-family: system-ui, "Segoe UI", "Noto Sans CJK SC",
    "Noto Sans SC", sans-serif; }
.selbar.on { display: flex; }
.selbar .sw { width: 17px; height: 17px; border-radius: 50%;
  border: 1px solid var(--bar-line); cursor: pointer; padding: 0; }
.selbar .sw-y { background: rgba(255,213,0,.9); }
.selbar .sw-b { background: rgba(86,156,255,.85); }
.selbar .sw-g { background: rgba(76,195,131,.85); }
.selbar .act { font-size: 11.5px; color: var(--bar-ink); padding: 3px 7px;
  border-radius: 5px; cursor: pointer; white-space: nowrap; }
.selbar .act:hover { background: var(--bar-hover); }
.selbar .sep { width: 1px; height: 16px; background: var(--bar-line); }
.selbar .ai { color: var(--uacc); font-weight: 600; }
)CSS";

char const* kBridgeScript = R"JS(
function clearMarks(root) {
  const marks = root.querySelectorAll('mark.epub-hit');
  marks.forEach(m => {
    const parent = m.parentNode;
    parent.replaceChild(document.createTextNode(m.textContent), m);
    parent.normalize();
  });
}
function markOccurrence(root, needle, ordinal) {
  let count = 0;
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    const lower = node.nodeValue.toLowerCase();
    let from = 0, at;
    while ((at = lower.indexOf(needle.toLowerCase(), from)) !== -1) {
      if (count === ordinal) {
        const range = document.createRange();
        range.setStart(node, at);
        range.setEnd(node, at + needle.length);
        const mark = document.createElement('mark');
        mark.className = 'epub-hit';
        try { range.surroundContents(mark); } catch (e) {}
        return;
      }
      count += 1;
      from = at + needle.length;
    }
  }
}
window.addEventListener('mouseup', () => {
  const sel = window.getSelection();
  if (!sel || sel.isCollapsed || sel.rangeCount === 0) return;
  const text = sel.toString().trim();
  if (text.length < 2) return;
  const range = sel.getRangeAt(0);
  const startNode = range.startContainer.nodeType === 1
    ? range.startContainer : range.startContainer.parentNode;
  const chapterEl = startNode && startNode.closest
    ? startNode.closest('.epub-chapter') : null;
  if (!chapterEl) return;
  const sections = Array.from(document.querySelectorAll('.epub-chapter'));
  const chapter = sections.indexOf(chapterEl) + 1;
  if (chapter < 1) return;
  const walker = document.createTreeWalker(chapterEl, NodeFilter.SHOW_TEXT);
  let start = -1, end = -1, offset = 0, node;
  while ((node = walker.nextNode())) {
    const len = node.nodeValue.length;
    if (start < 0 && node === range.startContainer) start = offset + range.startOffset;
    if (node === range.endContainer) end = offset + range.endOffset;
    offset += len;
  }
  if (start < 0 || end < 0 || end <= start) return;
  window.webkit.messageHandlers.bridge.postMessage(
    JSON.stringify({ kind: 'selection', text: text.slice(0, 800),
                     chapter: chapter, start: start, end: end }));
  placeSelBar(range);
});

// Selection bar (design/prototype/reading.html .selbar): floats above the
// selection; every action posts back to the host. This script runs in
// <head>, so the bar element is wired on DOMContentLoaded — a top-level
// null lookup here would abort the rest of the script.
var selbar = null;
function hideSelBar() { if (selbar) selbar.classList.remove('on'); }
function placeSelBar(range) {
  if (!selbar) return;
  var rect = range.getBoundingClientRect();
  selbar.classList.add('on');
  var bw = selbar.offsetWidth, bh = selbar.offsetHeight;
  var left = rect.left + window.scrollX + rect.width / 2 - bw / 2;
  left = Math.max(8, Math.min(left,
    document.documentElement.clientWidth - bw - 8));
  var top = rect.top + window.scrollY - bh - 10;
  if (top < window.scrollY) top = rect.bottom + window.scrollY + 10;
  selbar.style.left = left + 'px';
  selbar.style.top = top + 'px';
}
document.addEventListener('DOMContentLoaded', () => {
  selbar = document.getElementById('selbar');
  if (!selbar) return;
  selbar.addEventListener('mousedown', e => e.stopPropagation());
  selbar.addEventListener('click', e => {
    var t = e.target.closest('[data-a]');
    if (!t) return;
    window.webkit.messageHandlers.bridge.postMessage(
      JSON.stringify({ kind: 'sel-action', action: t.dataset.a }));
    var sel = window.getSelection();
    if (sel) sel.removeAllRanges();
    hideSelBar();
  });
});
window.addEventListener('mousedown', hideSelBar);
window.addEventListener('scroll', hideSelBar);

function epubTextNodeMap(root) {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const nodes = [];
  let total = 0, node;
  while ((node = walker.nextNode())) { nodes.push(node); total += node.nodeValue.length; }
  return { nodes, total };
}
function epubMarkColor(color) {
  return { yellow: '#FFD500', green: '#4CC383', blue: '#569CFF' }[color] || '#FFD500';
}
// Annotation ink follows the prototype (reading.html): highlighter swatches
// at the locked alphas, underline/strike drawn in the reading-theme accent.
function epubStyleFor(kind, color) {
  if (kind === 'underline') return 'text-decoration: underline; text-decoration-color: var(--uacc); text-underline-offset: 4px; text-decoration-thickness: 2px;';
  if (kind === 'strike') return 'text-decoration: line-through; text-decoration-color: var(--uacc); text-decoration-thickness: 2px;';
  const alpha = { yellow: '0.9', green: '0.85', blue: '0.85' }[color] || '0.9';
  const v = parseInt(epubMarkColor(color).slice(1), 16);
  return 'background: rgba(' + ((v >> 16) & 255) + ',' + ((v >> 8) & 255) + ',' + (v & 255) + ',' + alpha + ');';
}
// One annotation may wrap several text-node segments (same data-id — the
// PDF multi-rect model).
function applyEpubAnnotation(id, chapter, start, end, kind, color) {
  const root = document.querySelectorAll('.epub-chapter')[chapter - 1];
  if (!root) return;
  removeEpubAnnotation(id);
  const map = epubTextNodeMap(root);
  if (start < 0 || end > map.total) return;
  let offset = 0;
  for (const node of map.nodes) {
    const len = node.nodeValue.length;
    const from = Math.max(0, start - offset);
    const to = Math.min(len, end - offset);
    if (to > from) {
      const range = document.createRange();
      range.setStart(node, from);
      range.setEnd(node, to);
      const mark = document.createElement('mark');
      mark.className = 'epub-anno';
      mark.dataset.id = id;
      mark.setAttribute('style', epubStyleFor(kind, color));
      try { range.surroundContents(mark); } catch (e) {}
    }
    offset += len;
  }
}
function applyEpubAnnotations(list) {
  (list || []).forEach(a => applyEpubAnnotation(a.id, a.anchor.chapter,
    a.anchor.start, a.anchor.end, a.kind, a.color));
}
function removeEpubAnnotation(id) {
  document.querySelectorAll('mark.epub-anno[data-id="' + id + '"]').forEach(m => {
    const parent = m.parentNode;
    parent.replaceChild(document.createTextNode(m.textContent), m);
    parent.normalize();
  });
}
window.addEventListener('scroll', () => {
  const sections = document.querySelectorAll('.epub-chapter');
  const line = window.scrollY + window.innerHeight * 0.35;
  let current = 1;
  sections.forEach((el, i) => { if (el.offsetTop <= line) current = i + 1; });
  window.webkit.messageHandlers.bridge.postMessage(
    JSON.stringify({ kind: 'chapter', chapter: current }));
});
)JS";

// JSON string literal (quoted + escaped) for inline page scripts.
std::string json_string_literal(std::string const& text) {
  std::string out;
  out += '"';
  for (char const c : text) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  out += '"';
  return out;
}

}  // namespace

struct EpubTab::Impl {
  explicit Impl(EpubTab* owner) : tab(owner) {}
  EpubTab* tab;
  std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
  WebKitUserContentManager* ucm{nullptr};
  gulong message_handler_id{0};

  static void script_message(WebKitUserContentManager*, JSCValue* value,
                             gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if (value == nullptr || !jsc_value_is_string(value)) return;
    char* raw = jsc_value_to_string(value);
    if (raw == nullptr) return;
    std::string const json = raw;
    g_free(raw);
    JsonParser* parser = json_parser_new();
    GError* error = nullptr;
    json_parser_load_from_data(parser, json.c_str(),
                               static_cast<gssize>(json.size()), &error);
    if (error != nullptr) {
      g_error_free(error);
      g_object_unref(parser);
      return;
    }
    JsonObject* obj = json_node_get_object(json_parser_get_root(parser));
    std::string const kind = obj != nullptr && json_object_has_member(obj, "kind")
                                 ? json_object_get_string_member(obj, "kind")
                                 : "";
    if (kind == "selection") {
      EpubTab::Selection selection;
      selection.text = json_object_get_string_member(obj, "text");
      selection.chapter = static_cast<int>(
          json_object_get_int_member(obj, "chapter"));
      selection.start = json_object_get_int_member(obj, "start");
      selection.end = json_object_get_int_member(obj, "end");
      self->tab->selection_ = selection;
    } else if (kind == "chapter") {
      int const chapter = static_cast<int>(
          json_object_get_int_member(obj, "chapter"));
      if (chapter >= 1 && chapter <= self->tab->chapters_ &&
          chapter != self->tab->current_chapter_) {
        self->tab->current_chapter_ = chapter;
        if (self->tab->on_chapter_changed) {
          self->tab->on_chapter_changed(chapter);
        }
      }
    } else if (kind == "sel-action") {
      if (json_object_has_member(obj, "action")) {
        std::string const action =
            json_object_get_string_member(obj, "action");
        if (self->tab->on_selection_action) {
          self->tab->on_selection_action(action);
        }
      }
    }
    g_object_unref(parser);
  }

  void evaluate(std::string const& script) const {
    webkit_web_view_evaluate_javascript(
        WEBKIT_WEB_VIEW(tab->webview_), script.c_str(),
        static_cast<gssize>(script.size()), nullptr, nullptr, nullptr,
        nullptr, nullptr);
  }
};

EpubTab::EpubTab(PanelContext* ctx, std::string path, std::string title)
    : impl(std::make_unique<Impl>(this)),
      ctx_(ctx),
      path_(std::move(path)),
      title_(std::move(title)) {
  root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  impl->ucm = webkit_user_content_manager_new();
#if WEBKIT_CHECK_VERSION(2, 46, 0)
  // 2.46 gained the world_name parameter; NULL is the default world.
  webkit_user_content_manager_register_script_message_handler(
      impl->ucm, "bridge", nullptr);
#else
  webkit_user_content_manager_register_script_message_handler(impl->ucm,
                                                              "bridge");
#endif
  impl->message_handler_id = hook(
      impl->ucm, "script-message-received::bridge",
      Impl::script_message, impl.get());
  webview_ = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                     "user-content-manager", impl->ucm,
                                     nullptr));
  // The page paints its own background; avoid the white flash and honor
  // the reading theme behind it.
  GdkRGBA transparent = {1.0, 1.0, 1.0, 0.0};
  webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(webview_),
                                       &transparent);
  WebKitSettings* settings =
      webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_webaudio(settings, FALSE);
  webkit_settings_set_enable_media_stream(settings, FALSE);
  webkit_settings_set_enable_mediasource(settings, FALSE);
  gtk_box_append(GTK_BOX(root_), webview_);
  gtk_widget_set_vexpand(webview_, TRUE);
}

EpubTab::~EpubTab() {
  if (load_thread_.joinable()) load_thread_.join();
  if (impl->message_handler_id != 0) {
    g_signal_handler_disconnect(impl->ucm, impl->message_handler_id);
    impl->message_handler_id = 0;
  }
}

std::string EpubTab::chapter_text(int chapter) const {
  auto it = text_cache_.find(chapter);
  return it != text_cache_.end() ? it->second : std::string{};
}

void EpubTab::load() {
  std::string const path = path_;
  rivet_app::API* api = ctx_->api;
  bool const annotation_sidecar = ctx_->settings.annotation_sidecar;
  if (api == nullptr) {
    if (on_load_failed) on_load_failed("backend not ready");
    return;
  }
  auto alive = alive_;
  // Whole-book assembly runs on a worker: the serial chapter loop blocks
  // on N RPCs, and embedded-channel requests must stay serial (mac
  // prefetchEpubText comment). Every UI touch happens on the main loop.
  load_thread_ = std::thread([this, alive, api, path, annotation_sidecar]() {
    std::string error;
    rivet_app::EpubView view;
    std::vector<std::string> sections;
    std::map<int, std::string> texts;
    std::optional<rivet_app::AnnotationDocument> annotations;
    std::string annotation_error;
    bool failed = false;
    try {
      view = api->epub_open(path).get();
      sections.push_back(
          "<div class=\"epub-book-title\"><div class=\"book-title-text\">" +
          escape_html(view.title) + "</div></div>");
      int const count = static_cast<int>(std::max<std::int64_t>(
          view.chapters, 1));
      for (int i = 1; i <= count; ++i) {
        auto const body = api->epub_chapter_html(path, i).get();
        sections.push_back("<div class=\"epub-chapter\" data-chapter=\"" +
                           std::to_string(i) + "\">" +
                           std::string(body.begin(), body.end()) + "</div>");
        if (texts.find(i) == texts.end()) {
          texts[i] = api->epub_chapter_text(path, i).get();
        }
      }
      // Linux embeds one request channel. Keep the atomic annotation load in
      // the same serial worker as the EPUB metadata/chapter requests instead
      // of handing the channel to a second async or blocking worker.
      try {
        annotations = api->load_annotations(path, annotation_sidecar).get();
      } catch (std::exception const& exception) {
        annotation_error = exception.what();
      } catch (...) {
        annotation_error = "unknown annotation recovery failure";
      }
    } catch (std::exception const& e) {
      error = e.what();
      failed = true;
    } catch (...) {
      error = "unknown backend failure";
      failed = true;
    }
    host::dispatch_ui([this, alive, view, sections, texts, error, failed,
                       annotations, annotation_error]() {
      if (!*alive) return;  // tab destroyed while the worker ran
      if (failed) {
        if (on_load_failed) on_load_failed(error);
        return;
      }
      // The constructor starts with the filename so the tab can render while
      // RVT1 loads. Once the backend has parsed the EPUB, promote its metadata
      // title to the native header.
      title_ = view.title.empty() ? title_ : view.title;
      chapters_ = static_cast<int>(std::max<std::int64_t>(view.chapters, 1));
      toc_ = view.toc;
      text_cache_ = std::move(texts);
      std::string html = "<!doctype html><html data-theme=\"light\">"
                         "<head><meta charset=\"utf-8\"><style>";
      html += kReadingCss;
      html += "</style><script>";
      html += kBridgeScript;
      html += "</script></head><body>";
      for (auto const& section : sections) html += section;
      // Selection bar labels come from the i18n table (invariant 5); the
      // bar is inert chrome — every action routes to the host.
      html += std::string("<div class=\"selbar\" id=\"selbar\">") +
              "<button class=\"sw sw-y\" data-a=\"hl-yellow\" title=\"" +
              escape_html(Tr("ui.menu.highlight-yellow")) + "\"></button>" +
              "<button class=\"sw sw-b\" data-a=\"hl-blue\" title=\"" +
              escape_html(Tr("ui.menu.highlight-blue")) + "\"></button>" +
              "<button class=\"sw sw-g\" data-a=\"hl-green\" title=\"" +
              escape_html(Tr("ui.menu.highlight-green")) + "\"></button>" +
              "<span class=\"sep\"></span>" +
              "<span class=\"act\" data-a=\"underline\">" +
              escape_html(Tr("ui.menu.underline")) + "</span>" +
              "<span class=\"act\" data-a=\"strike\">" +
              escape_html(Tr("ui.menu.strike")) + "</span>" +
              "<span class=\"sep\"></span>" +
              "<span class=\"act\" data-a=\"copy\">" +
              escape_html(Tr("ui.card.copy")) + "</span>" +
              "<span class=\"act ai\" data-a=\"ai\">\u2726 " +
              escape_html(Tr("ui.toolbar.ai")) + "</span>" +
              "</div>";
      html += "</body></html>";
      webkit_web_view_load_html(WEBKIT_WEB_VIEW(webview_), html.c_str(),
                                nullptr);
      loaded_ = true;
      apply_font_scale(font_scale_);
      annotations_loaded = true;
      if (annotations.has_value()) {
        set_doc_data(pdfgist::parse_doc_data(annotations->data));
        annotation_dirty = annotations->recovery_pending;
        annotation_save_phase = annotations->recovery_pending
                                    ? AnnotationSavePhase::Recovered
                                    : AnnotationSavePhase::Saved;
        annotation_save_error.clear();
      } else {
        annotation_dirty = true;
        annotation_save_phase = AnnotationSavePhase::Failed;
        annotation_save_error = annotation_error;
      }
      if (on_load_finished) on_load_finished(title_);
    });
  });
}

void EpubTab::go_to_chapter(int chapter) {
  if (chapters_ <= 0) return;
  current_chapter_ = std::clamp(chapter, 1, chapters_);
  impl->evaluate(
      "(() => { const el = document.querySelectorAll('.epub-chapter')[" +
      std::to_string(current_chapter_ - 1) +
      "]; if (el) el.scrollIntoView({ block: 'start' }); })()");
}

void EpubTab::apply_font_scale(double scale) {
  font_scale_ = scale;
  impl->evaluate("document.documentElement.style.setProperty('--fs', " +
                 std::to_string(scale) + ")");
}

void EpubTab::apply_theme(std::string const& theme) {
  // The page theme axis is light | sepia | dark (design/DESIGN.md 阅读主题);
  // anything else falls back to light.
  std::string const resolved = theme == "dark"   ? "dark"
                               : theme == "sepia" ? "sepia"
                                                  : "light";
  impl->evaluate("document.documentElement.setAttribute('data-theme', " +
                 js_string(resolved) + ")");
}

void EpubTab::highlight_hit(int chapter, int ordinal,
                            std::string const& query) {
  if (chapter > 0) go_to_chapter(chapter);
  impl->evaluate(
      std::string("(() => { const root = ") +
      "document.querySelectorAll('.epub-chapter')[" +
      std::to_string(std::max(chapter - 1, 0)) +
      "]; if (!root) return; clearMarks(root); markOccurrence(root, " +
      js_string(query) + ", " + std::to_string(std::max(ordinal, 0)) +
      "); const mark = root.querySelector('mark.epub-hit'); "
      "if (mark) mark.scrollIntoView({ block: 'center' }); })()");
}


void EpubTab::add_annotation(StoredAnnotation const& annotation) {
  doc_data_.annotations.push_back(annotation);
  if (!loaded_ || !annotation.anchor) return;
  impl->evaluate(
      "applyEpubAnnotation(" + json_string_literal(annotation.id) + "," +
      std::to_string(annotation.anchor->chapter) + "," +
      std::to_string(annotation.anchor->start) + "," +
      std::to_string(annotation.anchor->end) + "," +
      json_string_literal(annotation.kind) + "," +
      json_string_literal(annotation.color) + ");");
}

void EpubTab::remove_annotation(std::string const& id) {
  auto& annotations = doc_data_.annotations;
  annotations.erase(std::remove_if(annotations.begin(), annotations.end(),
                                   [&](StoredAnnotation const& a) {
                                     return a.id == id;
                                   }),
                    annotations.end());
  if (loaded_) impl->evaluate("removeEpubAnnotation(" +
                              json_string_literal(id) + ");");
}

std::optional<StoredAnnotation> EpubTab::take_annotation(
    std::string const& id) {
  auto& annotations = doc_data_.annotations;
  auto const it = std::find_if(annotations.begin(), annotations.end(),
                               [&](StoredAnnotation const& a) {
                                 return a.id == id;
                               });
  if (it == annotations.end()) return std::nullopt;
  StoredAnnotation taken = *it;
  last_taken_index_ = static_cast<std::size_t>(it - annotations.begin());
  annotations.erase(it);
  if (loaded_) impl->evaluate("removeEpubAnnotation(" +
                              json_string_literal(id) + ");");
  return taken;
}

void EpubTab::restore_annotation(StoredAnnotation const& annotation,
                                 std::size_t index) {
  auto const at = std::min(index, doc_data_.annotations.size());
  doc_data_.annotations.insert(
      doc_data_.annotations.begin() + static_cast<std::ptrdiff_t>(at),
      annotation);
  if (loaded_ && annotation.anchor) {
    impl->evaluate(
        "applyEpubAnnotation(" + json_string_literal(annotation.id) + "," +
        std::to_string(annotation.anchor->chapter) + "," +
        std::to_string(annotation.anchor->start) + "," +
        std::to_string(annotation.anchor->end) + "," +
        json_string_literal(annotation.kind) + "," +
        json_string_literal(annotation.color) + ");");
  }
}

void EpubTab::set_doc_data(DocData data) {
  doc_data_ = std::move(data);
  if (!loaded_) return;
  // Citation-deck entries carry no ink — deck records, never page marks.
  DocData visible;
  visible.bookmarks = doc_data_.bookmarks;
  std::copy_if(doc_data_.annotations.begin(), doc_data_.annotations.end(),
               std::back_inserter(visible.annotations),
               [](StoredAnnotation const& a) { return a.kind != "cite"; });
  auto const bytes = pdfgist::encode_doc_data(visible);
  std::string const json(bytes.begin(), bytes.end());
  impl->evaluate("const d = JSON.parse(" + json_string_literal(json) +
                 "); applyEpubAnnotations(d.annotations);");
}

}  // namespace pdfgist

#else  // !PDFGIST_HAVE_WEBKIT

namespace pdfgist {

// WebKitGTK-less builds keep the host functional without EPUB: opening an
// EPUB surfaces a readable message instead of a missing feature crash.
EpubTab::EpubTab(PanelContext*, std::string path, std::string title)
    : impl(nullptr), ctx_(nullptr), path_(std::move(path)),
      title_(std::move(title)) {
  root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  auto* label = gtk_label_new(
      "EPUB support is unavailable in this build: install WebKitGTK "
      "(webkitgtk-6.0) or use a full package.");
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(label, TRUE);
  gtk_box_append(GTK_BOX(root_), label);
}

EpubTab::~EpubTab() = default;



std::string EpubTab::chapter_text(int) const { return {}; }

void EpubTab::highlight_hit(int, int, std::string const&) {}
void EpubTab::load() {
  if (on_load_failed) on_load_failed("WebKitGTK is not available");
}
void EpubTab::go_to_chapter(int) {}
void EpubTab::apply_font_scale(double) {}
void EpubTab::apply_theme(std::string const&) {}

}  // namespace pdfgist

#endif  // PDFGIST_HAVE_WEBKIT
