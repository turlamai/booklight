// PDFGist Linux host — GTK4 window over one embedded Racket CS backend.
//
// The macOS host (macos-host/Sources/RivetHost) is the behavior spec; the
// taskly Linux host donated the family patterns: boot the runtime off the
// UI thread, dispatch every completion/event back to the main loop before
// touching widgets, no-libadwaita dark theming, and the three-shape update
// flow (deb/rpm → package manager, AppImage → in-place swap + restart,
// tarball → extraction hint).
//
// Parity surface: PDF reading (tabs/continuous/double/zoom/dark/keys),
// AI (translate/summarize/chat streaming), settings, EPUB (WebKitGTK),
// page-level editing, annotation overlay, TTS, printing, split view,
// AcroForm, online updates, About — the README roadmap tracks what is
// still open, not this list.
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "GeneratedBackend.hpp"
#include "annotations.hpp"
#include "epub.hpp"
#include "forms.hpp"
#include "host.hpp"
#include "i18n.h"
#include "panels.hpp"
#include "pdfview.hpp"
#include "printing.hpp"
#include "tts.hpp"

namespace {

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

using pdfgist::AiPanel;
using pdfgist::DocData;
using pdfgist::EpubTab;
using pdfgist::PanelContext;
using pdfgist::PdfDocument;
using pdfgist::PdfTab;
using pdfgist::StoredAnnotation;
using pdfgist::StoredBookmark;
using pdfgist::TextSource;
using pdfgist::Tr;
using pdfgist::Trf;
using pdfgist::forms::FormsPane;

// ---------------------------------------------------------------------------
// host state
// ---------------------------------------------------------------------------

struct Host {
  GtkWindow* window{nullptr};
  GtkNotebook* notebook{nullptr};
  GtkStack* left_stack{nullptr};
  GtkWidget* left_sidebar{nullptr};
  GtkWidget* right_sidebar{nullptr};
  GtkFlowBox* thumbs{nullptr};
  GtkListBox* outline_list{nullptr};
  GtkScrolledWindow* outline_scroll{nullptr};
  // Outline follows the reading position (mac OutlineList parity).
  int outline_active_key{0};  // active page (PDF) or chapter (EPUB)
  bool outline_scroll_pending{false};
  GtkListBox* notes_list{nullptr};
  GtkWidget* notes_export_button{nullptr};
  // One-step undo (taskly banner parity): the annotation the last delete
  // removed (tab-anchored), the banner widget, and its 5 s timeout.
  StoredAnnotation undo_annotation{};
  bool undo_valid{false};
  std::size_t undo_index{0};
  PdfTab* undo_tab{nullptr};
  guint undo_timeout_source{0};
  GtkWidget* notes_undo_bar{nullptr};
  GtkWidget* notes_undo_label{nullptr};
  // EPUB twin (same banner; the annotation lives on the EpubTab).
  StoredAnnotation epub_undo_annotation{};
  bool epub_undo_valid{false};
  std::size_t epub_undo_index{0};
  EpubTab* epub_undo_tab{nullptr};
  guint epub_undo_timeout_source{0};
  GtkRevealer* find_revealer{nullptr};
  GtkSearchEntry* find_entry{nullptr};
  GtkLabel* find_count{nullptr};
  GtkWidget* main_stack{nullptr};
  GtkBox* welcome_recents{nullptr};
  GtkLabel* status_label{nullptr};
  GtkLabel* page_label{nullptr};
  GtkLabel* zoom_label{nullptr};
  GtkLabel* title_label{nullptr};
  GtkLabel* subtitle_label{nullptr};
  GtkButton* header_open{nullptr};
  GtkWidget* header_edit_menu{nullptr};
  GtkWidget* header_toc_menu{nullptr};
  GtkWidget* header_bookmark{nullptr};
  GtkWidget* header_split{nullptr};
  GtkWidget* header_tts{nullptr};
  GtkWidget* header_print{nullptr};
  GtkMenuButton* primary_menu{nullptr};
  std::unique_ptr<FormsPane> forms_pane;

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::unique_ptr<rivet_app::API> api;
  PanelContext panel_ctx;
  std::unique_ptr<AiPanel> ai_panel;
  bool shelf_visible{true};  // launch state: the shelf IS the empty state

  std::vector<std::unique_ptr<PdfTab>> tabs;
  EpubTab* epub{nullptr};
  int epub_tab_index{-1};

  rivet_app::SettingsView settings;
  std::vector<rivet_app::ProviderPreset> presets;
  std::vector<rivet_app::RecentEntry> recents;
  // GApplication emits `open` instead of `activate` for file-association
  // launches. Keep those paths until the window and RVT1 backend are ready.
  std::vector<std::string> pending_open_paths;
  bool ready{false};
  std::string theme{"system"};
  double tts_rate{1.0};
  double epub_font_scale{1.0};

  // backend boot handshake (mirrors the scaffold)
  std::thread startup_thread;
  std::thread bootstrap_thread;  // joined on shutdown (holds a raw api ptr)
  // Blocking annotation loads use the embedded channel from workers. Keep
  // their lifetimes explicit so shutdown never destroys api/backend first.
  std::vector<std::thread> annotation_load_threads;
  std::mutex startup_mutex;
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  std::string startup_error;
  std::atomic<bool> shutting_down{false};

  // EPUB whole-book search (mac runEpubFind): chapter+ordinal pairs
  std::vector<std::pair<int, int>> epub_find_hits;
  int epub_find_index{-1};
  std::string epub_find_query;

  // update flow
  struct UpdateCheckResult {
    std::string status;
    std::string message;
    std::string current_version;
    std::string available_version;
    std::string installer;
    std::int64_t size_bytes{0};
  };
  struct UpdatePollState {
    GtkDialog* progress{nullptr};
    GtkProgressBar* bar{nullptr};
    GtkLabel* label{nullptr};
    guint poll_source{0};
    bool in_flight{false};
  } update;

  // Invariant: PDF tabs occupy notebook indexes [0, tabs.size()), the EPUB
  // tab (when open) is appended last at index tabs.size(). All mapping math
  // hangs off that.
  PdfTab* active_pdf() {
    // Null during window assembly (the forms pane constructs before the
    // notebook exists); GTK would only warn, but stay explicit.
    if (notebook == nullptr) return nullptr;
    gint const index = gtk_notebook_get_current_page(notebook);
    if (index < 0 || static_cast<std::size_t>(index) >= tabs.size()) {
      return nullptr;
    }
    return tabs[static_cast<std::size_t>(index)].get();
  }

  // Active PDF tab even when the notebook sits on the EPUB page.
  PdfTab* active_pdf_only() { return active_pdf(); }

  bool epub_active() const {
    if (epub == nullptr || epub_tab_index < 0) return false;
    return const_cast<Host*>(this)->current_page_index() == epub_tab_index;
  }

  int current_page_index() {
    return gtk_notebook_get_current_page(notebook);
  }
};

Host g_host;

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

void host_alert(std::string const& message);
void show_selection_menu();
void build_epub_toc_menu();
void populate_thumbs();
void populate_notes();
void refresh_bookmark_button();
void refresh_forms_pane();
void load_annotations_for_tab(PdfTab* tab);
void schedule_annotation_save(PdfTab* tab);
void store_annotations_now(PdfTab* tab);
int epub_find_run(std::string const& query);
int epub_find_step(int direction);
int epub_find_total();
void epub_find_clear();
void update_main_stack();
void refresh_recents_menu();

template <typename Fn>
void with_api(std::string const& notice, Fn&& run) {
  if (g_host.api == nullptr || !g_host.ready) {
    if (!notice.empty()) host::dispatch_ui([notice] { host_alert(notice); });
    return;
  }
  run();
}

void host_alert(std::string const& message) {
  auto* dialog = gtk_message_dialog_new(
      g_host.window, static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                                 GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
  hook(dialog, "response",
                   +[](GtkDialog* dialog, gint, gpointer) {
                     gtk_window_destroy(GTK_WINDOW(dialog));
                   },
                   nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ---------------------------------------------------------------------------
// theme (no libadwaita: taskly's strategy)
// ---------------------------------------------------------------------------

bool system_prefers_dark() {
  GSettingsSchemaSource* source = g_settings_schema_source_get_default();
  GSettingsSchema* schema = nullptr;
  if (source != nullptr) {
    schema = g_settings_schema_source_lookup(
        source, "org.gnome.desktop.interface", TRUE);
  }
  if (schema == nullptr) return false;
  GSettings* settings = g_settings_new_full(schema, nullptr, nullptr);
  gchar* scheme = g_settings_get_string(settings, "color-scheme");
  bool const dark = scheme != nullptr && std::string(scheme) == "prefer-dark";
  g_free(scheme);
  g_object_unref(settings);
  g_settings_schema_unref(schema);
  return dark;
}

bool theme_is_dark() {
  auto const& theme = g_host.theme;
  if (theme == "dark") return true;
  if (theme == "light") return false;
  return system_prefers_dark();
}

// Defined after set_theme(); apply_theme() re-skins open books through it.
void apply_reading_theme();

void apply_theme() {
  bool const dark = theme_is_dark();
  g_object_set(gtk_settings_get_default(),
               "gtk-application-prefer-dark-theme",
               static_cast<gboolean>(dark), nullptr);
  // Without libadwaita, GTK4 picks its light/dark variant from the theme
  // NAME, not the portal: pin the widget theme to the chosen palette by
  // (un)suffixing "-dark" (taskly apply_theme).
  gchar* theme_name = nullptr;
  g_object_get(gtk_settings_get_default(), "gtk-theme-name", &theme_name,
               nullptr);
  std::string base = theme_name != nullptr ? theme_name : "Adwaita";
  g_free(theme_name);
  if (base.size() > 5 && base.compare(base.size() - 5, 5, "-dark") == 0) {
    base.erase(base.size() - 5);
  }
  base += dark ? "-dark" : "";
  g_object_set(gtk_settings_get_default(), "gtk-theme-name", base.c_str(),
               nullptr);
  // apply_theme() also runs before the window's content tree is assembled.
  apply_reading_theme();
}

void set_theme(std::string const& theme) {
  g_host.theme = theme;
  host::save_pref("theme", theme);
  apply_theme();
  if (g_host.ai_panel != nullptr) g_host.ai_panel->set_theme(theme);
}

// The reading page surface (settings.json) can override the chrome palette:
// night forces dark, paper forces light, sepia forces the deep cream
// parchment, system follows theme_is_dark().
std::string reading_surface() {
  using rivet_app::ReadingTheme;
  if (g_host.settings.reading_theme == ReadingTheme::night) return "dark";
  if (g_host.settings.reading_theme == ReadingTheme::sepia) return "sepia";
  if (g_host.settings.reading_theme == ReadingTheme::paper) return "light";
  return theme_is_dark() ? "dark" : "light";
}

// Re-skin open books only; the chrome palette is apply_theme()'s job.
void apply_reading_theme() {
  std::string const surface = reading_surface();
  for (auto& tab : g_host.tabs) tab->set_surface(surface);
  if (g_host.epub != nullptr) {
    g_host.epub->apply_theme(surface);
  }
  // set_dark dropped each tab's cached thumbnails; rebuild the sidebar so
  // visible thumbs re-render in the new palette.
  if (g_host.thumbs != nullptr) populate_thumbs();
}

void save_reading_prefs(rivet_app::ReadingTheme theme) {
  auto* api = g_host.api.get();
  if (api == nullptr) return;
  api->save_reading_prefs_async(
      theme, g_host.settings.reading_font_scale_permille,
      [](rivet_app::Result<rivet_app::SettingsView> result) {
        if (!result.succeeded()) return;
        host::dispatch_ui([saved = result.get()]() mutable {
          g_host.settings = std::move(saved);
          apply_reading_theme();
        });
      });
}

void save_reading_font_scale(double scale) {
  auto* api = g_host.api.get();
  if (api == nullptr) return;
  auto const permille =
      static_cast<std::int64_t>(std::llround(scale * 1000.0));
  api->save_reading_prefs_async(
      g_host.settings.reading_theme, permille,
      [](rivet_app::Result<rivet_app::SettingsView> result) {
        if (!result.succeeded()) return;
        host::dispatch_ui([saved = result.get()]() mutable {
          g_host.settings = std::move(saved);
        });
      });
}

// ---------------------------------------------------------------------------
// status / title
// ---------------------------------------------------------------------------

void refresh_header_title() {
  // The empty-window welcome is not a bookshelf or collection scope.
  if (g_host.shelf_visible) {
    gtk_label_set_text(g_host.title_label, "Booklight");
    gtk_label_set_text(g_host.subtitle_label, "");
    return;
  }
  PdfTab* tab = g_host.active_pdf();
  if (g_host.epub_active()) {
    gtk_label_set_text(g_host.title_label,
                       g_host.epub->title().c_str());
    gtk_label_set_text(g_host.subtitle_label,
                       Trf("ui.viewer.epub-subtitle",
                           std::to_string(g_host.epub->current_page()),
                           std::to_string(g_host.epub->chapters()))
                           .c_str());
    gtk_widget_set_visible(g_host.header_toc_menu,
                           !g_host.epub->toc().empty());
    return;
  }
  gtk_widget_set_visible(g_host.header_toc_menu, FALSE);
  if (tab != nullptr) {
    std::string title = tab->title();
    if (tab->pdf_edits_pending) {
      title += " •";  // differs from the original PDF until explicit export
    }
    gtk_label_set_text(g_host.title_label, title.c_str());
    gtk_label_set_text(g_host.subtitle_label,
                       (std::to_string(tab->current_page()) + " / " +
                        std::to_string(tab->document()->page_count()))
                           .c_str());
  } else {
    gtk_label_set_text(g_host.title_label, "Booklight");
    gtk_label_set_text(g_host.subtitle_label, "");
  }
}

void refresh_status() {
  PdfTab* tab = g_host.active_pdf();
  if (g_host.epub_active()) {
    gtk_label_set_text(
        g_host.page_label,
        Trf("ui.viewer.chapter-status",
            std::to_string(g_host.epub->current_page()),
            std::to_string(g_host.epub->chapters()))
            .c_str());
    gtk_label_set_text(g_host.zoom_label, "");
    return;
  }
  if (tab != nullptr) {
    gtk_label_set_text(
        g_host.page_label,
        Trf("ui.viewer.page-status", std::to_string(tab->current_page()),
            std::to_string(tab->document()->page_count()))
            .c_str());
    gtk_label_set_text(g_host.zoom_label,
                       (std::to_string(tab->zoom_percent()) + "%").c_str());
  } else {
    gtk_label_set_text(g_host.page_label, "");
    gtk_label_set_text(g_host.zoom_label, "");
  }
}

// ---------------------------------------------------------------------------
// text sources for the AI panel (mac PDFGistModel parity)
// ---------------------------------------------------------------------------

constexpr int kDocPageCap = 12;
constexpr int kPageCharCap = 8000;

std::string truncate_chars(std::string const& text, std::size_t cap) {
  if (text.size() <= cap) return text;
  // UTF-8 safe cut: back off to a code point boundary.
  std::size_t end = cap;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return text.substr(0, end) + "…";
}

std::optional<TextSource> current_page_text() {
  if (g_host.epub_active()) {
    auto* epub = g_host.epub;
    std::string text = epub->chapter_text(epub->current_page());
    if (text.empty()) return std::nullopt;
    return TextSource{epub->current_page(), truncate_chars(text, kPageCharCap)};
  }
  PdfTab* tab = g_host.active_pdf();
  if (tab == nullptr) return std::nullopt;
  std::string text = tab->page_text(tab->current_page());
  if (text.empty()) return std::nullopt;
  return TextSource{tab->current_page(), truncate_chars(text, kPageCharCap)};
}

std::optional<TextSource> doc_text() {
  // The "--- 第 N 章/页 ---" separators are model-facing payload (the
  // domain prompt expects the Chinese markers; windows hosts them the same
  // way) — deliberately not localized.
  if (g_host.epub_active()) {
    auto* epub = g_host.epub;
    std::string joined;
    for (int i = 1; i <= std::max(epub->chapters(), 1); ++i) {
      std::string raw = epub->chapter_text(i);
      if (raw.empty()) continue;
      std::string const slice = truncate_chars(raw, 3000);
      joined += "--- 第 " + std::to_string(i) + " 章 ---\n" + slice + "\n\n";
      if (joined.size() >= 24000) break;
    }
    if (joined.empty()) return std::nullopt;
    return TextSource{std::max(epub->chapters(), 1), joined};
  }
  PdfTab* tab = g_host.active_pdf();
  if (tab == nullptr || tab->document()->page_count() <= 0) {
    return std::nullopt;
  }
  int const n = std::min(tab->document()->page_count(), kDocPageCap);
  std::string joined;
  for (int i = 1; i <= n; ++i) {
    std::string raw = tab->page_text(i);
    if (raw.empty()) continue;
    std::string const slice = truncate_chars(raw, 3000);
    joined += "--- 第 " + std::to_string(i) + " 页 ---\n" + slice + "\n\n";
    if (joined.size() >= 24000) break;
  }
  if (joined.empty()) return std::nullopt;
  return TextSource{n, joined};
}

std::string selection_text() {
  if (g_host.epub_active()) {
    return g_host.epub != nullptr ? g_host.epub->pending_selection().text
                                  : "";
  }
  PdfTab* tab = g_host.active_pdf();
  return tab != nullptr ? tab->selection_text() : "";
}

void update_recents_debounced(std::string const& path, int page) {
  // Debounced noteActivity parity: 2s, page-based scroll estimate.
  struct Ctx {
    std::string path;
    int page;
    int pages;
  };
  auto* ctx = new Ctx{path, page, 0};
  if (g_host.epub_active()) {
    ctx->pages = std::max(g_host.epub->chapters(), 1);
  } else if (PdfTab* tab = g_host.active_pdf(); tab != nullptr) {
    ctx->pages = std::max(tab->document()->page_count(), 1);
  }
  (void)g_timeout_add(
      2000,
      +[](gpointer user_data) -> int {
        std::unique_ptr<Ctx> ctx(static_cast<Ctx*>(user_data));
        if (g_host.api == nullptr || !g_host.ready) return G_SOURCE_REMOVE;
        auto const ratio = static_cast<std::int64_t>(
            static_cast<double>(ctx->page) /
                static_cast<double>(std::max(ctx->pages, 1)) * 100000.0 +
            0.5);
        (void)g_host.api->update_recents_async(
            ctx->path, ctx->page, ratio,
            [](rivet_app::Result<void> result) {
              if (!result.succeeded()) return;
              // The async completion is not on the GTK thread. Marshal
              // access to host state/API through the native main loop.
              host::dispatch_ui([] {
                if (!g_host.ready || g_host.api == nullptr) return;
                (void)g_host.api->get_recents_async(
                    [](rivet_app::Result<std::vector<rivet_app::RecentEntry>> result) {
                      if (!result.succeeded()) return;
                      auto recents = std::move(result.value.value());
                      host::dispatch_ui([recents = std::move(recents)]() mutable {
                        g_host.recents = std::move(recents);
                        refresh_recents_menu();
                      });
                    });
              });
            });
        return G_SOURCE_REMOVE;
      },
      ctx);
}

// The accent outline entry (current position). #B45309, the family accent.
void init_outline_css() {
  static bool done = false;
  if (done) return;
  done = true;
  auto* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(
      provider,
      ".outline-active { color: #b45309; font-weight: 700; }");
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

// Centers the row tagged outline-active inside the outline scroll (light
// scroll-to-visible; a no-op when the list or the pane is not on screen).
void scroll_outline_to_active() {
  if (g_host.outline_scroll == nullptr || g_host.outline_list == nullptr) {
    return;
  }
  GtkListBoxRow* row = nullptr;
  int index = 0;
  while ((row = gtk_list_box_get_row_at_index(g_host.outline_list,
                                              index++)) != nullptr) {
    if (gtk_widget_has_css_class(GTK_WIDGET(row), "outline-active")) break;
  }
  if (row == nullptr) return;
  if (!gtk_widget_get_visible(GTK_WIDGET(g_host.outline_scroll))) return;
  GtkAllocation alloc{};
  gtk_widget_get_allocation(GTK_WIDGET(row), &alloc);
  auto* va = gtk_scrolled_window_get_vadjustment(g_host.outline_scroll);
  double const target =
      alloc.y - (gtk_adjustment_get_page_size(va) - alloc.height) / 2.0;
  gtk_adjustment_set_value(
      va, std::max(0.0, std::min(target, gtk_adjustment_get_upper(va) -
                                            gtk_adjustment_get_page_size(va))));
}

void build_outline_for(PdfTab* tab) {
  init_outline_css();
  gtk_list_box_remove_all(g_host.outline_list);
  auto items = pdfgist::pdf_outline(*tab->document());
  // Active entry: the deepest item at or before the current page.
  int active_page = 0;
  for (auto const& item : items) {
    int const page = item.page + 1;  // outline pages are 0-based here
    if (page >= 1 && page <= tab->current_page() && page > active_page) {
      active_page = page;
    }
  }
  g_host.outline_active_key = active_page;
  for (auto const& item : items) {
    auto* row = gtk_list_box_row_new();
    auto* label = gtk_label_new(item.title.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_start(
        label, static_cast<guint>(8 + 14 * std::min(item.level, 6)));
    gtk_widget_set_margin_top(label, 3);
    gtk_widget_set_margin_bottom(label, 3);
    if (item.page >= 0 && item.page + 1 == active_page) {
      gtk_widget_add_css_class(label, "outline-active");
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    if (item.page >= 0) {
      g_object_set_data(G_OBJECT(row), "pdfgist-page",
                        GINT_TO_POINTER(item.page + 1));
    }
    gtk_list_box_append(g_host.outline_list, row);
  }
  if (g_host.outline_scroll_pending) {
    g_host.outline_scroll_pending = false;
    scroll_outline_to_active();
  }
}

void build_epub_outline_for(EpubTab* epub) {
  init_outline_css();
  gtk_list_box_remove_all(g_host.outline_list);
  int const current = epub->current_page();  // chapter, 1-based
  g_host.outline_active_key = current;
  for (auto const& item : epub->toc()) {
    auto* label = gtk_label_new(item.title.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_start(
        label,
        static_cast<guint>(
            8 + 14 * std::min<std::int64_t>(item.level, 6)));
    gtk_widget_set_margin_top(label, 3);
    gtk_widget_set_margin_bottom(label, 3);
    if (item.chapter == current) {
      gtk_widget_add_css_class(label, "outline-active");
    }
    // Let GtkListBox create and parent its GtkListBoxRow.  Explicitly
    // parenting a pre-built row triggered GTK's "widget already has a
    // parent" assertion in the packaged Wayland run and left the native TOC
    // empty even though the backend had returned all outline items.
    gtk_list_box_append(g_host.outline_list, label);
    auto* row = gtk_widget_get_parent(label);
    if (item.chapter > 0) {
      g_object_set_data(G_OBJECT(row), "booklight-epub-chapter",
                        GINT_TO_POINTER(static_cast<int>(item.chapter)));
    }
  }
  if (g_host.outline_scroll_pending) {
    g_host.outline_scroll_pending = false;
    scroll_outline_to_active();
  }
}

// ---------------------------------------------------------------------------
// tabs & notebook — invariant: PDF tabs occupy notebook indexes
// [0, tabs.size()), the EPUB tab (when open) is appended last.
// ---------------------------------------------------------------------------

std::string path_title(std::string const& path) {
  return std::filesystem::path(path).filename().string();
}

void close_tab_index(gint index);
void schedule_epub_annotation_save();
void export_annotations_ui();

void build_outline_for_active() {
  if (g_host.epub_active()) {
    build_epub_outline_for(g_host.epub);
  } else if (auto* tab = g_host.active_pdf_only(); tab != nullptr) {
    build_outline_for(tab);
  }
}

GtkWidget* build_tab_label(std::string const& title) {
  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  auto* label = gtk_label_new(title.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  auto* close = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close, "flat");
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), close);
  g_object_set_data(G_OBJECT(box), "pdfgist-tab-close", close);
  return box;
}

void connect_tab_label(GtkWidget* label, gint index) {
  auto* close =
      GTK_BUTTON(g_object_get_data(G_OBJECT(label), "pdfgist-tab-close"));
  g_object_set_data(G_OBJECT(close), "pdfgist-tab-index",
                    GINT_TO_POINTER(index));
  hook(
      close, "clicked",
      +[](GtkButton* button, gpointer) {
        auto const index = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "pdfgist-tab-index"));
        close_tab_index(index);
      },
      nullptr);
}

// PDFium FPDF_ERR_PASSWORD: the document needs a (or a correct) password.
constexpr unsigned long kPdfErrorPassword = 4;

// Common tail of every successful open: tab creation, notebook wiring,
// outline + annotation load (see load_annotations_for_tab).
void finish_pdf_open(std::string const& path, int resume_page,
                     std::shared_ptr<PdfDocument> doc) {
  std::string const title = path_title(path);
  auto tab = std::make_unique<PdfTab>(doc, path, title);
  PdfTab* raw = tab.get();
  raw->set_double_mode(g_host.settings.view_mode == "double");
  raw->set_surface(reading_surface());
  raw->on_page_changed = [raw](int page) {
    if (raw != g_host.active_pdf()) return;
    refresh_status();
    refresh_header_title();
    update_recents_debounced(raw->path(), page);
    refresh_bookmark_button();
    // Outline follows the reading position when its pane is on screen.
    if (g_host.left_stack != nullptr &&
        gtk_stack_get_visible_child_name(g_host.left_stack) != nullptr &&
        std::string(gtk_stack_get_visible_child_name(g_host.left_stack)) ==
            "outline") {
      g_host.outline_scroll_pending = true;
      build_outline_for(raw);
    }
  };
  raw->on_selection_changed = [](bool) {};
  raw->on_context_menu = [] { show_selection_menu(); };
  raw->on_annotations_changed = [raw] {
    schedule_annotation_save(raw);
    refresh_bookmark_button();
    populate_notes();
  };
  g_host.tabs.push_back(std::move(tab));
  gint index = gtk_notebook_append_page(g_host.notebook, raw->widget(),
                                        build_tab_label(title));
  if (g_host.epub_tab_index >= 0 && index != g_host.epub_tab_index) {
    // keep the EPUB tab last
    gtk_notebook_reorder_child(g_host.notebook, raw->widget(),
                               g_host.epub_tab_index);
    g_host.epub_tab_index += 1;
    index = g_host.epub_tab_index - 1;
  }
  connect_tab_label(
      gtk_notebook_get_tab_label(g_host.notebook,
                                 gtk_notebook_get_nth_page(
                                     g_host.notebook, index)),
      index);
  gtk_notebook_set_tab_reorderable(g_host.notebook, raw->widget(), TRUE);
  gtk_notebook_set_current_page(g_host.notebook, index);
  raw->restore_page(std::max(resume_page, 1));
  raw->go_to_page(std::max(resume_page, 1));
  build_outline_for(raw);
  load_annotations_for_tab(raw);
  refresh_bookmark_button();
  populate_notes();
  refresh_forms_pane();
  refresh_status();
  refresh_header_title();
  // Reading-first: every successful open grows the shelf (mac
  // importOpenedBook — the upsert dedupes on the path).
}

// Modal password prompt with an in-place wrong-password retry loop (the
// while-doc.isLocked semantics of the macOS ReaderModel.openPath; windows
// OpenDocReady does the same). Empty input cancels the open entirely.
struct PasswordCtx {
  std::string path;
  int resume_page;
  GtkDialog* dialog{nullptr};
  GtkLabel* message{nullptr};
  GtkEntry* entry{nullptr};
};

void on_password_dialog_response(GtkDialog* dialog, int response,
                                 gpointer user_data) {
  auto* ctx = static_cast<PasswordCtx*>(user_data);
  if (response != GTK_RESPONSE_OK) {
    gtk_window_destroy(GTK_WINDOW(dialog));  // ctx freed on destroy
    return;
  }
  std::string const password =
      gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
  // mac parity: empty input aborts the open instead of retrying.
  if (password.empty()) {
    gtk_window_destroy(GTK_WINDOW(dialog));
    return;
  }
  unsigned long error = 0;
  auto doc = PdfDocument::open_file(ctx->path, password, &error);
  if (doc != nullptr) {
    std::string const path = ctx->path;
    int const resume = ctx->resume_page;
    gtk_window_destroy(GTK_WINDOW(dialog));
    finish_pdf_open(path, resume, std::move(doc));
    return;
  }
  if (error == kPdfErrorPassword) {
    // wrong password: keep the dialog up and loop.
    gtk_label_set_text(ctx->message, Tr("ui.password.wrong").c_str());
    gtk_editable_set_text(GTK_EDITABLE(ctx->entry), "");
    gtk_window_present(GTK_WINDOW(dialog));
    return;
  }
  // Non-password failure: destroy the dialog (frees ctx — copy what the
  // alert still needs) and surface the generic open failure.
  std::string const failed_path = ctx->path;
  gtk_window_destroy(GTK_WINDOW(dialog));
  host_alert(Trf("ui.open-failed", failed_path));
}

void prompt_pdf_password(std::string const& path, int resume_page,
                         bool wrong) {
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.password.title").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL,
      Tr("ui.password.ok").c_str(), GTK_RESPONSE_OK, nullptr);
  auto* ctx = new PasswordCtx{path, resume_page, GTK_DIALOG(dialog), nullptr,
                              nullptr};
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_box_append(GTK_BOX(area), box);
  auto* message = gtk_label_new(wrong ? Tr("ui.password.wrong").c_str()
                                      : nullptr);
  gtk_widget_set_visible(message, wrong);
  gtk_widget_add_css_class(message, "warning");
  ctx->message = GTK_LABEL(message);
  gtk_box_append(GTK_BOX(box), message);
  auto* entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  ctx->entry = GTK_ENTRY(entry);
  gtk_box_append(GTK_BOX(box), entry);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  hook(
      dialog, "response", on_password_dialog_response, ctx,
      +[](gpointer data, GClosure*) { delete static_cast<PasswordCtx*>(data); },
      static_cast<GConnectFlags>(0));
  gtk_window_present(GTK_WINDOW(dialog));
}

void open_pdf_path(std::string const& path, int resume_page = 1) {
  for (std::size_t i = 0; i < g_host.tabs.size(); ++i) {
    if (g_host.tabs[i]->path() == path) {  // already open → activate
      gtk_notebook_set_current_page(g_host.notebook, static_cast<gint>(i));
      if (resume_page > 1) g_host.tabs[i]->go_to_page(resume_page);
      return;
    }
  }
  if (!pdfium::load()) {
    host_alert(Trf("ui.pdfium.unavailable", std::string(pdfium::load_error())));
    return;
  }
  unsigned long error = 0;
  auto doc = PdfDocument::open_file(path, "", &error);
  if (doc != nullptr) {
    finish_pdf_open(path, resume_page, std::move(doc));
    return;
  }
  if (error == kPdfErrorPassword) {
    prompt_pdf_password(path, resume_page, false);
    return;
  }
  host_alert(Trf("ui.open-failed", path));
}

void open_epub_path(std::string const& path, int resume_chapter = 1) {
  if (g_host.epub != nullptr && g_host.epub->path() == path) {
    gtk_notebook_set_current_page(g_host.notebook, g_host.epub_tab_index);
    if (resume_chapter > 1) g_host.epub->go_to_chapter(resume_chapter);
    return;
  }
  // one EPUB tab at a time (mac parity)
  if (g_host.epub != nullptr) {
    auto* dying = g_host.epub;
    gtk_notebook_remove_page(g_host.notebook, g_host.epub_tab_index);
    dying->destroy();
    delete dying;
    g_host.epub = nullptr;
    g_host.epub_tab_index = -1;
  }
  auto epub =
      std::make_unique<EpubTab>(&g_host.panel_ctx, path, path_title(path));
  EpubTab* raw = epub.release();
  g_host.epub = raw;
  // Persisted typography rides in; EpubTab::load() re-applies it to the
  // page once the web view is up.
  g_host.epub_font_scale =
      g_host.settings.reading_font_scale_permille / 1000.0;
  raw->apply_font_scale(g_host.epub_font_scale);
  auto* label = build_tab_label(path_title(path));
  g_host.epub_tab_index =
      gtk_notebook_append_page(g_host.notebook, raw->widget(), label);
  connect_tab_label(label, g_host.epub_tab_index);
  gtk_notebook_set_current_page(g_host.notebook, g_host.epub_tab_index);
  // EPUB has a table of contents rather than thumbnails.  Put the shared
  // native sidebar on its Outline page immediately, matching the Windows and
  // macOS reading surfaces instead of hiding the TOC in a header-only menu.
  gtk_stack_set_visible_child_name(g_host.left_stack, "outline");
  build_outline_for_active();

  raw->on_chapter_changed = [](int chapter) {
    refresh_status();
    refresh_header_title();
    update_recents_debounced(g_host.epub->path(), chapter);
    // TOC follows the reading position when its pane is on screen.
    if (g_host.left_stack != nullptr &&
        gtk_stack_get_visible_child_name(g_host.left_stack) != nullptr &&
        std::string(gtk_stack_get_visible_child_name(g_host.left_stack)) ==
            "outline") {
      g_host.outline_scroll_pending = true;
      build_epub_outline_for(g_host.epub);
    }
  };
  raw->on_annotations_changed = [] {
    schedule_epub_annotation_save();
    if (g_host.left_stack != nullptr &&
        gtk_stack_get_visible_child_name(g_host.left_stack) != nullptr &&
        std::string(gtk_stack_get_visible_child_name(g_host.left_stack)) ==
            "notes") {
      populate_notes();
    }
  };
  raw->on_load_finished = [raw](std::string const&) {
    build_epub_toc_menu();
    // Populate the native TOC for the still-open book even if GTK has not yet
    // delivered the notebook switch-page signal.  That signal will rebuild
    // the sidebar again if another document is actually selected.
    if (raw == g_host.epub) {
      build_epub_outline_for(raw);
    }
    refresh_status();
    refresh_header_title();
    if (raw == g_host.epub) {
      raw->apply_theme(theme_is_dark() ? "dark" : "light");
      // Annotation recovery is part of EpubTab's serial load worker, so all
      // blocking requests use one embedded-channel sequence.
      populate_notes();
    }
  };
  raw->on_load_failed = [](std::string const& message) {
    host_alert(Trf("ui.epub.open-failed", message));
  };
  raw->load();
  if (resume_chapter > 1) raw->go_to_chapter(resume_chapter);
  refresh_status();
  refresh_header_title();
  // Reading-first: the opened EPUB lands on the shelf too (mac
  // importOpenedEpub).
}

void open_path(std::string const& path, int resume_page = 1) {
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".epub") {
    open_epub_path(path, resume_page);
  } else {
    open_pdf_path(path, resume_page);
  }
}

void save_edited_pdf_for_tab(PdfTab* tab, std::function<void(bool)> on_complete);
void confirm_close_dirty_pdf(PdfTab* tab, std::function<void()> proceed);

void close_tab_index_unchecked(gint index) {
  if (g_host.epub_tab_index >= 0 && index == g_host.epub_tab_index) {
    auto* dying = g_host.epub;
    gtk_notebook_remove_page(g_host.notebook, index);
    dying->destroy();
    delete dying;
    g_host.epub = nullptr;
    g_host.epub_tab_index = -1;
    refresh_status();
    refresh_header_title();
    return;
  }
  if (index < 0 || static_cast<std::size_t>(index) >= g_host.tabs.size()) {
    return;
  }
  // mac closeDocument: flush pending annotation persistence before the tab
  // dies (the debounce timer would otherwise fire on a dangling pointer).
  store_annotations_now(g_host.tabs[static_cast<std::size_t>(index)].get());
  gtk_notebook_remove_page(g_host.notebook, index);
  g_host.tabs.erase(g_host.tabs.begin() + index);
  if (g_host.epub_tab_index > index) {
    g_host.epub_tab_index -= 1;
    if (g_host.epub != nullptr) {
      connect_tab_label(
          gtk_notebook_get_tab_label(
              g_host.notebook,
              gtk_notebook_get_nth_page(g_host.notebook,
                                        g_host.epub_tab_index)),
          g_host.epub_tab_index);
    }
  }
  for (gint i = index; i < static_cast<gint>(g_host.tabs.size()); ++i) {
    connect_tab_label(
        gtk_notebook_get_tab_label(
            g_host.notebook,
            gtk_notebook_get_nth_page(g_host.notebook, i)),
        i);
  }
  refresh_status();
  refresh_header_title();
}

bool pdf_tab_still_open(PdfTab const* tab) {
  return std::any_of(g_host.tabs.begin(), g_host.tabs.end(),
                     [tab](auto const& entry) { return entry.get() == tab; });
}

void close_tab_index(gint index) {
  if (index >= 0 && static_cast<std::size_t>(index) < g_host.tabs.size()) {
    PdfTab* const tab = g_host.tabs[static_cast<std::size_t>(index)].get();
    if (tab->pdf_edits_pending) {
      confirm_close_dirty_pdf(tab, [tab] {
        for (gint i = 0; i < static_cast<gint>(g_host.tabs.size()); ++i) {
          if (g_host.tabs[static_cast<std::size_t>(i)].get() == tab) {
            close_tab_index_unchecked(i);
            break;
          }
        }
      });
      return;
    }
  }
  close_tab_index_unchecked(index);
}

void close_active_reader() {
  close_tab_index(g_host.current_page_index());
}

// ---------------------------------------------------------------------------
// thumbnails (left sidebar)
// ---------------------------------------------------------------------------

void populate_thumbs() {
  auto* tab = g_host.active_pdf_only();
  gtk_flow_box_remove_all(g_host.thumbs);
  if (tab == nullptr) return;
  int const count = tab->document()->page_count();
  for (int i = 1; i <= count; ++i) {
    auto* child = gtk_flow_box_child_new();
    auto* picture = gtk_picture_new();
    gtk_widget_set_size_request(picture, 110, -1);
    gtk_widget_set_margin_top(picture, 4);
    gtk_widget_set_margin_bottom(picture, 4);
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), picture);
    g_object_set_data(G_OBJECT(child), "pdfgist-thumb-page",
                      GINT_TO_POINTER(i));
    // Render lazily: mapping a thumb into the visible sidebar is what
    // makes it worth rendering.
    hook(
        child, "map",
        +[](GtkWidget* widget, gpointer) {
          auto const page = GPOINTER_TO_INT(
              g_object_get_data(G_OBJECT(widget), "pdfgist-thumb-page"));
          auto* tab2 = g_host.active_pdf_only();
          if (tab2 == nullptr) return;
          auto* texture = tab2->thumbnail(page);
          if (texture == nullptr) return;
          auto* picture2 = GTK_PICTURE(
              gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(widget)));
          if (gtk_picture_get_paintable(picture2) == nullptr) {
            gtk_picture_set_paintable(picture2, GDK_PAINTABLE(texture));
            gtk_picture_set_can_shrink(picture2, TRUE);
          }
        },
        nullptr);
    gtk_flow_box_append(g_host.thumbs, child);
  }
}

// ---------------------------------------------------------------------------
// find bar
// ---------------------------------------------------------------------------

void close_find() {
  gtk_revealer_set_reveal_child(g_host.find_revealer, FALSE);
  if (auto* tab = g_host.active_pdf_only(); tab != nullptr) tab->clear_find();
  epub_find_clear();
  gtk_label_set_text(g_host.find_count, "");
}

void run_find_now() {
  std::string const query = gtk_editable_get_text(
      GTK_EDITABLE(g_host.find_entry));
  if (g_host.epub_active()) {
    int const total = epub_find_run(query);
    gtk_label_set_text(g_host.find_count,
                       total > 0 ? ("1 / " + std::to_string(total)).c_str()
                                 : Tr("ui.find.no-results").c_str());
    return;
  }
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr) {
    gtk_label_set_text(g_host.find_count, Tr("ui.find.no-document").c_str());
    return;
  }
  int const total = tab->run_find(query);
  gtk_label_set_text(g_host.find_count,
                     total > 0
                         ? (std::to_string(
                                (tab->find_index() < 0 ? 0 : tab->find_index() + 1)) +
                            " / " + std::to_string(total))
                               .c_str()
                         : Tr("ui.find.no-results").c_str());
}

void find_step(int direction) {
  if (g_host.epub_active()) {
    if (epub_find_step(direction) && epub_find_total() > 0) {
      gtk_label_set_text(g_host.find_count,
                         (std::to_string(g_host.epub_find_index + 1) + " / " +
                          std::to_string(epub_find_total()))
                             .c_str());
    }
    return;
  }
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || tab->find_total() == 0) return;
  bool const moved = direction > 0 ? tab->find_next() : tab->find_previous();
  if (moved) {
    gtk_label_set_text(
        g_host.find_count,
        (std::to_string(tab->find_index() + 1) + " / " +
         std::to_string(tab->find_total()))
            .c_str());
  }
}

// ---------------------------------------------------------------------------
// selection context menu
// ---------------------------------------------------------------------------

GtkWidget* g_selection_popover{nullptr};

void show_selection_menu() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr) return;
  if (tab->selection_text().empty()) return;

  if (g_selection_popover != nullptr) {
    gtk_widget_unparent(g_selection_popover);
    g_selection_popover = nullptr;
  }
  // Same shape as the mac selectionMenu: translate | highlights (3 colors)
  // + underline + strike | copy.
  auto* menu = g_menu_new();
  {
    auto* section = g_menu_new();
    auto* translate =
        g_menu_item_new(Tr("ui.menu.translate").c_str(), "app.translate-selection");
    g_menu_append_item(section, translate);
    g_object_unref(translate);
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  {
    auto* section = g_menu_new();
    g_menu_append(section, Tr("ui.menu.highlight-yellow").c_str(),
                  "app.annotate-highlight::yellow");
    g_menu_append(section, Tr("ui.menu.highlight-green").c_str(),
                  "app.annotate-highlight::green");
    g_menu_append(section, Tr("ui.menu.highlight-blue").c_str(),
                  "app.annotate-highlight::blue");
    g_menu_append(section, Tr("ui.menu.underline").c_str(),
                  "app.annotate-underline");
    g_menu_append(section, Tr("ui.menu.strike").c_str(),
                  "app.annotate-strike");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  {
    auto* section = g_menu_new();
    g_menu_append(section, Tr("ui.menu.copy").c_str(), "app.copy-selection");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  g_selection_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  // Anchor to the scrolled view; the popover picks a pointing position at
  // the top-left of the document area (pointer-exact anchoring is a
  // P2 nicety — the actions matter more than the pixel).
  GdkRectangle rect{12, 12, 1, 1};
  gtk_widget_set_parent(g_selection_popover, GTK_WIDGET(tab->widget()));
  gtk_popover_set_pointing_to(GTK_POPOVER(g_selection_popover), &rect);
  gtk_popover_popup(GTK_POPOVER(g_selection_popover));
}

// ---------------------------------------------------------------------------
// annotations: persistence + notes pane (left sidebar's third mode)
// ---------------------------------------------------------------------------

void store_annotations_now(PdfTab* tab) {
  if (tab == nullptr || g_host.api == nullptr || !g_host.ready) return;
  if (tab->annotation_save_source != 0) {
    g_source_remove(tab->annotation_save_source);
    tab->annotation_save_source = 0;
  }
  auto bytes = encode_doc_data(tab->doc_data());
  auto* identity = tab;
  auto const revision = tab->annotation_revision;
  tab->annotation_save_phase = pdfgist::AnnotationSavePhase::Pending;
  (void)g_host.api->set_annotations_async(
      tab->path(), g_host.settings.annotation_sidecar, std::move(bytes),
      [identity, revision](rivet_app::Result<void> result) {
        std::string const error = result.succeeded()
                                      ? std::string{}
                                      : AiPanel::describe_error(result.error);
        host::dispatch_ui([identity, revision, error] {
          auto const it = std::find_if(
              g_host.tabs.begin(), g_host.tabs.end(),
              [identity](auto const& candidate) { return candidate.get() == identity; });
          if (it == g_host.tabs.end()) return;
          auto* live = it->get();
          if (live->annotation_revision != revision) return;
          live->annotation_dirty = !error.empty();
          live->annotation_save_phase =
              error.empty() ? pdfgist::AnnotationSavePhase::Saved
                            : pdfgist::AnnotationSavePhase::Failed;
          live->annotation_save_error = error;
          if (g_host.active_pdf_only() == live) populate_notes();
        });
      });
}

void schedule_annotation_save(PdfTab* tab) {
  tab->annotation_dirty = true;
  ++tab->annotation_revision;
  tab->annotation_save_phase = pdfgist::AnnotationSavePhase::Pending;
  tab->annotation_save_error.clear();
  populate_notes();
  if (tab->annotation_save_source != 0) {
    g_source_remove(tab->annotation_save_source);
  }
  // mac queueAnnotationSave: 800 ms debounce, flush on tab close.
  tab->annotation_save_source = g_timeout_add(
      800,
      +[](gpointer user_data) -> int {
        auto* tab = static_cast<PdfTab*>(user_data);
        tab->annotation_save_source = 0;
        store_annotations_now(tab);
        return G_SOURCE_REMOVE;
      },
      tab);
}

void load_annotations_for_tab(PdfTab* tab) {
  if (g_host.api == nullptr || !g_host.ready || tab == nullptr) return;
  auto const path = tab->path();
  bool const sidecar = g_host.settings.annotation_sidecar;
  auto* api = g_host.api.get();
  g_host.annotation_load_threads.emplace_back([tab, api, path, sidecar] {
    std::optional<rivet_app::AnnotationDocument> document;
    std::string error;
    try {
      document = api->load_annotations(path, sidecar).get();
    } catch (std::exception const& exception) {
      error = exception.what();
    } catch (...) {
      error = "unknown annotation recovery failure";
    }
    host::dispatch_ui(
        [tab, document = std::move(document), error = std::move(error)]() mutable {
          auto const live = std::find_if(
              g_host.tabs.begin(), g_host.tabs.end(),
              [tab](auto const& candidate) { return candidate.get() == tab; });
          if (live == g_host.tabs.end()) return;
          if (!document.has_value()) {
            tab->annotations_loaded = true;
            tab->annotation_dirty = true;
            tab->annotation_save_phase = pdfgist::AnnotationSavePhase::Failed;
            tab->annotation_save_error = error;
          } else {
            tab->set_doc_data(pdfgist::parse_doc_data(document->data));
            tab->annotations_loaded = true;
            tab->annotation_dirty = document->recovery_pending;
            tab->annotation_save_phase =
                document->recovery_pending
                    ? pdfgist::AnnotationSavePhase::Recovered
                    : pdfgist::AnnotationSavePhase::Saved;
            tab->annotation_save_error.clear();
          }
          if (g_host.active_pdf_only() == tab) populate_notes();
        });
  });
}

// ---- one-step undo (taskly banner parity) ---------------------------------

void notes_undo_hide() {
  if (g_host.undo_timeout_source != 0) {
    g_source_remove(g_host.undo_timeout_source);
    g_host.undo_timeout_source = 0;
  }
  g_host.undo_valid = false;
  g_host.undo_tab = nullptr;
  if (g_host.notes_undo_bar != nullptr) {
    gtk_widget_set_visible(g_host.notes_undo_bar, FALSE);
  }
}

gboolean notes_undo_timeout(gpointer) {
  g_host.undo_timeout_source = 0;
  notes_undo_hide();
  return G_SOURCE_REMOVE;
}

void notes_undo_restore() {
  if (!g_host.undo_valid) return;
  auto* tab = g_host.active_pdf_only();
  if (tab == g_host.undo_tab && tab != nullptr) {
    tab->restore_annotation(g_host.undo_annotation, g_host.undo_index);
  }
  notes_undo_hide();
}

// ---- EPUB annotations (same store; page = chapter ordinal) ---------------

void schedule_epub_annotation_save() {
  if (g_host.api == nullptr || !g_host.ready) return;
  if (g_host.epub == nullptr) return;
  auto* identity = g_host.epub;
  identity->annotation_dirty = true;
  ++identity->annotation_revision;
  auto const revision = identity->annotation_revision;
  identity->annotation_save_phase = pdfgist::AnnotationSavePhase::Pending;
  identity->annotation_save_error.clear();
  auto bytes = pdfgist::encode_doc_data(g_host.epub->doc_data());
  (void)g_host.api->set_annotations_async(
      g_host.epub->path(), g_host.settings.annotation_sidecar,
      std::move(bytes), [identity, revision](rivet_app::Result<void> result) {
        std::string const error = result.succeeded()
                                      ? std::string{}
                                      : AiPanel::describe_error(result.error);
        host::dispatch_ui([identity, revision, error] {
          if (g_host.epub != identity ||
              identity->annotation_revision != revision) {
            return;
          }
          identity->annotation_dirty = !error.empty();
          identity->annotation_save_phase =
              error.empty() ? pdfgist::AnnotationSavePhase::Saved
                            : pdfgist::AnnotationSavePhase::Failed;
          identity->annotation_save_error = error;
          populate_notes();
        });
      });
  populate_notes();
}

void notes_undo_hide();
void epub_notes_undo_restore() {
  if (!g_host.epub_undo_valid) return;
  auto* epub = g_host.epub;
  if (epub == g_host.epub_undo_tab && epub != nullptr) {
    epub->restore_annotation(g_host.epub_undo_annotation,
                             g_host.epub_undo_index);
  }
  notes_undo_hide();
}

gboolean epub_notes_undo_timeout(gpointer) {
  g_host.epub_undo_timeout_source = 0;
  g_host.epub_undo_valid = false;
  g_host.epub_undo_tab = nullptr;
  if (g_host.notes_undo_bar != nullptr) {
    gtk_widget_set_visible(g_host.notes_undo_bar, FALSE);
  }
  return G_SOURCE_REMOVE;
}

void epub_annotate_ui(char const* color, char const* kind) {
  auto* epub = g_host.epub;
  if (epub == nullptr) return;
  auto const& selection = epub->pending_selection();
  if (selection.text.size() < 2 || selection.chapter < 1) return;
  auto id = "e-" + std::to_string(static_cast<long long>(std::time(nullptr))) +
            "-" + std::to_string(std::rand() % 10000);
  StoredAnnotation annotation;
  annotation.id = id;
  annotation.page = selection.chapter;
  annotation.excerpt = selection.text.substr(0, 800);
  annotation.color = color;
  annotation.kind = kind;
  annotation.created = static_cast<std::int64_t>(std::time(nullptr));
  annotation.anchor = pdfgist::StoredAnchor{selection.chapter,
                                            selection.start, selection.end};
  epub->add_annotation(annotation);
  epub->clear_pending_selection();
  schedule_epub_annotation_save();
  if (g_host.left_stack != nullptr &&
      std::string(gtk_stack_get_visible_child_name(g_host.left_stack)) ==
          "notes") {
    populate_notes();
  }
}

void epub_remove_annotation(std::string const& id) {
  auto* epub = g_host.epub;
  if (epub == nullptr) return;
  auto taken = epub->take_annotation(id);
  if (!taken.has_value()) return;
  g_host.epub_undo_annotation = *taken;
  g_host.epub_undo_index = epub->last_taken_index();
  g_host.epub_undo_tab = epub;
  g_host.epub_undo_valid = true;
  if (g_host.epub_undo_timeout_source != 0) {
    g_source_remove(g_host.epub_undo_timeout_source);
  }
  g_host.epub_undo_timeout_source =
      g_timeout_add(5000, epub_notes_undo_timeout, nullptr);
  if (g_host.notes_undo_bar != nullptr) {
    gtk_widget_set_visible(g_host.notes_undo_bar, TRUE);
  }
  schedule_epub_annotation_save();
}

// One annotation card / bookmark row click target: stored id or page.
void on_note_row_activated(GtkListBox*, GtkListBoxRow* row, gpointer) {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || row == nullptr) return;
  auto const page = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(row), "pdfgist-note-page"));
  if (page > 0) tab->go_to_page(page);
}

void populate_notes() {
  auto* list = g_host.notes_list;
  if (list == nullptr) return;
  gtk_list_box_remove_all(list);
  auto* tab = g_host.active_pdf_only();
  bool const epub_now = g_host.epub_active() && g_host.epub != nullptr;
  if (g_host.notes_export_button != nullptr) {
    gtk_widget_set_sensitive(g_host.notes_export_button,
                             tab != nullptr || epub_now);
  }
  if (!epub_now && tab != g_host.undo_tab) notes_undo_hide();
  if (epub_now && g_host.epub != g_host.epub_undo_tab) {
    g_host.epub_undo_valid = false;
    g_host.epub_undo_tab = nullptr;
  }
  if (g_host.notes_undo_bar != nullptr) {
    gtk_widget_set_visible(
        g_host.notes_undo_bar,
        (tab != nullptr && g_host.undo_valid && tab == g_host.undo_tab) ||
            (epub_now && g_host.epub_undo_valid));
  }
  auto append_save_state = [list](pdfgist::AnnotationSavePhase phase,
                                  std::string const& error) {
    if (phase == pdfgist::AnnotationSavePhase::Saved) return;
    auto* row = gtk_list_box_row_new();
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_add_css_class(box, "card");
    std::string message;
    if (phase == pdfgist::AnnotationSavePhase::Pending) {
      message = Tr("ui.notes.save-pending");
    } else if (phase == pdfgist::AnnotationSavePhase::Recovered) {
      message = Tr("ui.notes.recovery-pending");
    } else {
      message = Trf("ui.notes.save-failed", error);
    }
    auto* label = gtk_label_new(message.c_str());
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(box), label);
    if (phase == pdfgist::AnnotationSavePhase::Recovered ||
        phase == pdfgist::AnnotationSavePhase::Failed) {
      auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      auto* retry = gtk_button_new_with_label(Tr("ui.notes.retry-save").c_str());
      auto* export_button =
          gtk_button_new_with_label(Tr("ui.notes.export").c_str());
      hook(retry, "clicked", +[](GtkButton*, gpointer) {
        if (g_host.epub_active() && g_host.epub != nullptr) {
          schedule_epub_annotation_save();
        } else if (auto* active = g_host.active_pdf_only(); active != nullptr) {
          active->annotation_dirty = true;
          store_annotations_now(active);
        }
      }, nullptr);
      hook(export_button, "clicked",
           +[](GtkButton*, gpointer) { export_annotations_ui(); }, nullptr);
      gtk_box_append(GTK_BOX(actions), retry);
      gtk_box_append(GTK_BOX(actions), export_button);
      gtk_box_append(GTK_BOX(box), actions);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_append(list, row);
  };
  if (epub_now) {
    append_save_state(g_host.epub->annotation_save_phase,
                      g_host.epub->annotation_save_error);
  } else if (tab != nullptr) {
    append_save_state(tab->annotation_save_phase,
                      tab->annotation_save_error);
  }
  if (epub_now) {
    // EPUB annotations: annotate-the-selection row + chapter-labeled cards.
    auto* epub = g_host.epub;
    auto const& selection = epub->pending_selection();
    if (selection.text.size() > 1 && selection.chapter >= 1) {
      auto* panel_row = gtk_list_box_row_new();
      auto* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      gtk_widget_set_margin_start(panel, 8);
      gtk_widget_set_margin_end(panel, 8);
      gtk_widget_set_margin_top(panel, 8);
      auto* heading = gtk_label_new(Tr("ui.notes.epub-annotate").c_str());
      gtk_widget_set_halign(heading, GTK_ALIGN_START);
      gtk_widget_add_css_class(heading, "caption");
      gtk_widget_add_css_class(heading, "dim-label");
      gtk_box_append(GTK_BOX(panel), heading);
      auto* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      struct {
        char const* label;
        char const* color;
        char const* kind;
      } const actions[] = {
          {Tr("ui.menu.highlight-yellow").c_str(), "yellow", "highlight"},
          {Tr("ui.menu.highlight-green").c_str(), "green", "highlight"},
          {Tr("ui.menu.highlight-blue").c_str(), "blue", "highlight"},
          {Tr("ui.menu.underline").c_str(), "", "underline"},
          {Tr("ui.menu.strike").c_str(), "", "strike"},
      };
      for (auto const& action : actions) {
        auto* btn = gtk_button_new_with_label(action.label);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "caption");
        hook(btn, "clicked",
             +[](GtkButton*, gpointer user_data) {
               auto* pair = static_cast<std::pair<char const*, char const*>*>(
                   user_data);
               epub_annotate_ui(pair->first, pair->second);
               delete pair;
             },
             new std::pair<char const*, char const*>(action.color, action.kind),
             +[](gpointer user_data, GClosure*) {
               delete static_cast<std::pair<char const*, char const*>*>(user_data);
             },
             static_cast<GConnectFlags>(0));
        gtk_box_append(GTK_BOX(row_box), btn);
      }
      gtk_box_append(GTK_BOX(panel), row_box);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(panel_row), panel);
      gtk_list_box_append(list, panel_row);
    }
    auto const& data = epub->doc_data();
    if (data.annotations.empty()) {
      auto* row = gtk_list_box_row_new();
      auto* hint = gtk_label_new(Tr("ui.notes.empty").c_str());
      gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
      gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
      gtk_widget_add_css_class(hint, "dim-label");
      gtk_widget_add_css_class(hint, "caption");
      gtk_widget_set_margin_top(hint, 10);
      gtk_widget_set_margin_start(hint, 10);
      gtk_widget_set_margin_end(hint, 10);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hint);
      gtk_list_box_append(list, row);
      gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
    }
    std::vector<StoredAnnotation const*> sorted;
    sorted.reserve(data.annotations.size());
    for (auto const& annotation : data.annotations) {
      sorted.push_back(&annotation);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](auto const* a, auto const* b) {
                return a->page == b->page ? a->created < b->created
                                          : a->page < b->page;
              });
    for (auto const* annotation : sorted) {
      auto* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_add_css_class(card, "card");
      gtk_widget_set_margin_start(card, 8);
      gtk_widget_set_margin_end(card, 8);
      gtk_widget_set_margin_top(card, 4);
      auto* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      auto* chapter_label = gtk_label_new(
          Trf("ui.epub.chapter-n", std::to_string(annotation->page)).c_str());
      gtk_widget_set_halign(chapter_label, GTK_ALIGN_START);
      gtk_widget_add_css_class(chapter_label, "caption");
      gtk_widget_add_css_class(chapter_label, "dim-label");
      gtk_box_append(GTK_BOX(head), chapter_label);
      auto* del = gtk_button_new_with_label(Tr("ui.notes.delete-btn").c_str());
      gtk_widget_add_css_class(del, "flat");
      gtk_widget_add_css_class(del, "caption");
      gtk_widget_set_halign(del, GTK_ALIGN_END);
      auto* annotation_id = new std::string(annotation->id);
      hook(del, "clicked",
           +[](GtkButton*, gpointer user_data) {
             std::unique_ptr<std::string> id(
                 static_cast<std::string*>(user_data));
             epub_remove_annotation(*id);
           },
           annotation_id,
           +[](gpointer user_data, GClosure*) {
             delete static_cast<std::string*>(user_data);
           },
           static_cast<GConnectFlags>(0));
      gtk_box_append(GTK_BOX(head), del);
      gtk_box_append(GTK_BOX(card), head);
      auto* excerpt = gtk_label_new(annotation->excerpt.c_str());
      gtk_label_set_wrap(GTK_LABEL(excerpt), TRUE);
      gtk_label_set_xalign(GTK_LABEL(excerpt), 0.0f);
      gtk_box_append(GTK_BOX(card), excerpt);
      if (!annotation->note.empty()) {
        auto* note = gtk_label_new(annotation->note.c_str());
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
        gtk_widget_add_css_class(note, "caption");
        gtk_widget_add_css_class(note, "dim-label");
        gtk_box_append(GTK_BOX(card), note);
      }
      auto* card_row = gtk_list_box_row_new();
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(card_row), card);
      gtk_list_box_append(list, card_row);
    }
    auto* export_row = gtk_list_box_row_new();
    auto* export_btn =
        gtk_button_new_with_label(Tr("ui.notes.export").c_str());
    gtk_widget_set_halign(export_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_start(export_btn, 8);
    gtk_widget_set_margin_end(export_btn, 8);
    gtk_widget_set_margin_bottom(export_btn, 8);
    hook(export_btn, "clicked",
         +[](GtkButton*, gpointer) { export_annotations_ui(); }, nullptr);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(export_row), export_btn);
    gtk_list_box_append(list, export_row);
    return;
  }
  if (tab == nullptr) {
    // No document: the pane shows the welcome hint.
    auto* row = gtk_list_box_row_new();
    auto* hint = gtk_label_new(
        g_host.epub_active() ? Tr("ui.need.document").c_str()
                             : Tr("ui.welcome.open-any").c_str());
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_add_css_class(hint, "caption");
    gtk_widget_set_margin_top(hint, 10);
    gtk_widget_set_margin_start(hint, 10);
    gtk_widget_set_margin_end(hint, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hint);
    gtk_list_box_append(list, row);
    gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
    return;
  }

  auto const& data = tab->doc_data();
  if (data.annotations.empty()) {
    auto* row = gtk_list_box_row_new();
    auto* hint = gtk_label_new(Tr("ui.notes.empty").c_str());
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_add_css_class(hint, "caption");
    gtk_widget_set_margin_top(hint, 10);
    gtk_widget_set_margin_start(hint, 10);
    gtk_widget_set_margin_end(hint, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hint);
    gtk_list_box_append(list, row);
    gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
  }

  // Annotation cards sorted by page then creation (mac NotesPane).
  std::vector<StoredAnnotation const*> sorted;
  sorted.reserve(data.annotations.size());
  for (auto const& annotation : data.annotations) sorted.push_back(&annotation);
  std::sort(sorted.begin(), sorted.end(),
            [](auto const* a, auto const* b) {
              return a->page != b->page ? a->page < b->page
                                        : a->created < b->created;
            });
  for (auto const* annotation : sorted) {
    auto* row = gtk_list_box_row_new();
    gtk_widget_add_css_class(GTK_WIDGET(row), "card");
    auto* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(card, 6);
    gtk_widget_set_margin_bottom(card, 6);
    gtk_widget_set_margin_start(card, 8);
    gtk_widget_set_margin_end(card, 8);

    auto* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    // color dot
    auto* dot = gtk_drawing_area_new();
    gtk_widget_set_size_request(dot, 9, 9);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    auto* color = new std::string(annotation->color);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(dot),
        +[](GtkDrawingArea* area, cairo_t* cr, int, int, gpointer user_data) {
          auto const* color = static_cast<std::string const*>(user_data);
          double r = 0, g = 0, b = 0, a = 1;
          pdfgist::annotation_rgba(*color, &r, &g, &b, &a);
          cairo_set_source_rgb(cr, r, g, b);
          cairo_arc(cr, 4.5, 4.5, 4.0, 0, 2 * 3.14159265358979);
          cairo_set_source_rgba(cr, r, g, b, a);
          cairo_fill(cr);
          (void)area;
        },
        color,
        +[](gpointer user_data) { delete static_cast<std::string*>(user_data); });
    gtk_box_append(GTK_BOX(head), dot);
    auto* page_label = gtk_label_new(
        Trf("ui.notes.page", std::to_string(annotation->page)).c_str());
    gtk_widget_add_css_class(page_label, "caption");
    gtk_widget_add_css_class(page_label, "dim-label");
    gtk_box_append(GTK_BOX(head), page_label);
    if (!annotation->kind.empty() && annotation->kind != "highlight") {
      auto* kind = gtk_label_new(annotation->kind.c_str());
      gtk_widget_add_css_class(kind, "caption");
      gtk_widget_add_css_class(kind, "dim-label");
      gtk_box_append(GTK_BOX(head), kind);
    }
    auto* spacer = gtk_label_new(nullptr);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(head), spacer);
    auto* translate_btn =
        gtk_button_new_with_label(Tr("ui.notes.translate-btn").c_str());
    gtk_widget_add_css_class(translate_btn, "flat");
    gtk_widget_add_css_class(translate_btn, "caption");
    auto* excerpt = new std::string(annotation->excerpt);
    hook(
        translate_btn, "clicked",
        +[](GtkButton*, gpointer user_data) {
          std::unique_ptr<std::string> excerpt(
              static_cast<std::string*>(user_data));
          g_host.ai_panel->do_translate_text(*excerpt);
        },
        excerpt,
        +[](gpointer user_data, GClosure*) {
          delete static_cast<std::string*>(user_data);
        },
        static_cast<GConnectFlags>(0));
    gtk_box_append(GTK_BOX(head), translate_btn);
    auto* delete_btn =
        gtk_button_new_with_label(Tr("ui.notes.delete-btn").c_str());
    gtk_widget_add_css_class(delete_btn, "flat");
    gtk_widget_add_css_class(delete_btn, "caption");
    auto* annotation_id = new std::string(annotation->id);
    hook(
        delete_btn, "clicked",
        +[](GtkButton*, gpointer user_data) {
          std::unique_ptr<std::string> id(static_cast<std::string*>(user_data));
          auto* tab = g_host.active_pdf_only();
          if (tab == nullptr) return;
          auto taken = tab->take_annotation(*id);
          if (!taken.has_value()) return;
          // Stage the banner for a five-second undo window.
          g_host.undo_annotation = *taken;
          g_host.undo_index = tab->last_taken_index_;
          g_host.undo_tab = tab;
          g_host.undo_valid = true;
          if (g_host.undo_timeout_source != 0) {
            g_source_remove(g_host.undo_timeout_source);
          }
          g_host.undo_timeout_source = g_timeout_add(
              5000, notes_undo_timeout, nullptr);
          if (g_host.notes_undo_bar != nullptr) {
            gtk_widget_set_visible(g_host.notes_undo_bar, TRUE);
          }
        },
        annotation_id,
        +[](gpointer user_data, GClosure*) {
          delete static_cast<std::string*>(user_data);
        },
        static_cast<GConnectFlags>(0));
    gtk_box_append(GTK_BOX(head), delete_btn);
    gtk_box_append(GTK_BOX(card), head);

    auto* excerpt_label = gtk_label_new(annotation->excerpt.c_str());
    gtk_label_set_wrap(GTK_LABEL(excerpt_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(excerpt_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(excerpt_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(excerpt_label), 4);
    gtk_box_append(GTK_BOX(card), excerpt_label);
    if (!annotation->note.empty()) {
      auto* note = gtk_label_new(annotation->note.c_str());
      gtk_label_set_wrap(GTK_LABEL(note), TRUE);
      gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
      gtk_widget_add_css_class(note, "dim-label");
      gtk_widget_add_css_class(note, "caption");
      gtk_box_append(GTK_BOX(card), note);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    g_object_set_data(G_OBJECT(row), "pdfgist-note-page",
                      GINT_TO_POINTER(annotation->page));
    gtk_list_box_append(list, row);
  }

  // bookmarks
  auto* bookmarks_header_row = gtk_list_box_row_new();
  auto* bookmarks_header =
      gtk_label_new(Tr("ui.notes.bookmarks").c_str());
  gtk_label_set_xalign(GTK_LABEL(bookmarks_header), 0.0f);
  gtk_widget_add_css_class(bookmarks_header, "caption");
  gtk_widget_set_margin_top(bookmarks_header, 10);
  gtk_widget_set_margin_start(bookmarks_header, 10);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(bookmarks_header_row),
                             bookmarks_header);
  gtk_widget_set_sensitive(GTK_WIDGET(bookmarks_header_row), FALSE);
  gtk_list_box_append(list, bookmarks_header_row);

  if (data.bookmarks.empty()) {
    auto* row = gtk_list_box_row_new();
    auto* hint = gtk_label_new(Tr("ui.notes.bookmark-empty").c_str());
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_add_css_class(hint, "caption");
    gtk_widget_set_margin_start(hint, 10);
    gtk_widget_set_margin_end(hint, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hint);
    gtk_list_box_append(list, row);
    gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
  }
  auto bookmarks = data.bookmarks;  // already page-sorted by the model
  for (auto const& bookmark : bookmarks) {
    auto* row = gtk_list_box_row_new();
    auto* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(line, 3);
    gtk_widget_set_margin_bottom(line, 3);
    gtk_widget_set_margin_start(line, 8);
    gtk_widget_set_margin_end(line, 8);
    auto* page = gtk_label_new(std::to_string(bookmark.page).c_str());
    gtk_widget_add_css_class(page, "caption");
    gtk_box_append(GTK_BOX(line), page);
    auto* label = gtk_label_new(
        (bookmark.label.empty()
             ? Trf("ui.notes.page", std::to_string(bookmark.page))
             : bookmark.label)
            .c_str());
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(line), label);
    auto* remove = gtk_button_new_with_label(Tr("ui.notes.delete-btn").c_str());
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_add_css_class(remove, "caption");
    auto const page_no = bookmark.page;
    hook(
        remove, "clicked",
        +[](GtkButton*, gpointer user_data) {
          auto const page_no = GPOINTER_TO_INT(user_data);
          if (auto* tab = g_host.active_pdf_only(); tab != nullptr) {
            tab->remove_bookmark(page_no);
          }
        },
        GINT_TO_POINTER(page_no));
    gtk_box_append(GTK_BOX(line), remove);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), line);
    g_object_set_data(G_OBJECT(row), "pdfgist-note-page",
                      GINT_TO_POINTER(bookmark.page));
    gtk_list_box_append(list, row);
  }
}

void refresh_bookmark_button() {
  if (g_host.header_bookmark == nullptr) return;
  auto* tab = g_host.active_pdf();
  bool const marked = tab != nullptr && tab->is_bookmarked(tab->current_page());
  gtk_button_set_icon_name(GTK_BUTTON(g_host.header_bookmark),
                           marked ? "starred-symbolic" : "non-starred-symbolic");
}

void refresh_forms_pane() {
  if (g_host.forms_pane != nullptr) g_host.forms_pane->refresh();
}

void annotate_from_selection(std::string const& color, std::string const& kind) {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr) return;
  tab->add_annotation_from_selection(color, kind);
}

void toggle_bookmark_ui() {
  auto* tab = g_host.active_pdf();
  if (tab == nullptr) return;
  tab->toggle_bookmark(tab->current_page());
}

void toggle_split_ui() {
  auto* tab = g_host.active_pdf();
  if (tab == nullptr) return;
  tab->set_split(!tab->split_on());
}

void toggle_tts_ui() {
  std::string text;
  if (g_host.epub_active() && g_host.epub != nullptr) {
    text = g_host.epub->chapter_text(g_host.epub->current_page());
  } else if (auto* tab = g_host.active_pdf(); tab != nullptr) {
    text = tab->page_text(tab->current_page());
  }
  // Trim; mac guards on whitespace-only pages.
  auto trimmed = [](std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' ||
                          s.front() == '\r' || s.front() == '\t')) {
      s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' ||
                          s.back() == '\r' || s.back() == '\t')) {
      s.pop_back();
    }
    return s;
  }(std::move(text));
  if (trimmed.empty()) {
    host_alert(Tr("ui.tts.empty"));
    return;
  }
  if (trimmed.size() > 20000) {
    // UTF-8 safe cut (mac String.prefix(20000) parity).
    std::size_t end = 20000;
    while (end > 0 &&
           (static_cast<unsigned char>(trimmed[end]) & 0xC0) == 0x80) {
      --end;
    }
    trimmed = trimmed.substr(0, end);
  }
  if (!pdfgist::tts::toggle(trimmed)) {
    host_alert(Tr("ui.tts.unavailable"));
  }
}

void print_ui() {
  auto* tab = g_host.active_pdf();
  if (tab == nullptr) {
    host_alert(Tr("ui.need.document"));
    return;
  }
  std::string const error =
      pdfgist::printing::print_document(g_host.window, tab->document(),
                                        tab->current_page());
  if (!error.empty()) {
    // GError text from the print stack is already human-readable.
    host_alert(error);
  }
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// EPUB whole-book search over the prefetched chapter texts (mac runEpubFind)
// ---------------------------------------------------------------------------

void highlight_epub_hit(int chapter, int ordinal);
void check_updates_silent();
std::string ascii_lower(std::string const& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return out;
}

int epub_find_total() {
  return static_cast<int>(g_host.epub_find_hits.size());
}

void epub_find_clear() {
  g_host.epub_find_hits.clear();
  g_host.epub_find_index = -1;
  g_host.epub_find_query.clear();
  if (g_host.epub != nullptr) {
    highlight_epub_hit(-1, -1);
  }
}

int epub_find_run(std::string const& query) {
  auto* epub = g_host.epub;
  g_host.epub_find_hits.clear();
  g_host.epub_find_index = -1;
  g_host.epub_find_query = query;
  if (epub == nullptr || query.empty()) return 0;
  std::string const needle = ascii_lower(query);
  for (int chapter = 1; chapter <= std::max(epub->chapters(), 1); ++chapter) {
    std::string const text = ascii_lower(epub->chapter_text(chapter));
    std::size_t at = 0;
    int ordinal = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
      g_host.epub_find_hits.emplace_back(chapter, ordinal);
      ++ordinal;
      at += needle.size();
    }
  }
  if (!g_host.epub_find_hits.empty()) {
    g_host.epub_find_index = 0;
    auto const [chapter, ordinal] = g_host.epub_find_hits.front();
    highlight_epub_hit(chapter, ordinal);
  }
  return epub_find_total();
}

int epub_find_step(int direction) {
  auto& hits = g_host.epub_find_hits;
  if (hits.empty()) return 0;
  g_host.epub_find_index = (g_host.epub_find_index + direction +
                            static_cast<int>(hits.size())) %
                           static_cast<int>(hits.size());
  auto const [chapter, ordinal] = hits[static_cast<std::size_t>(
      g_host.epub_find_index)];
  highlight_epub_hit(chapter, ordinal);
  return 1;
}

void highlight_epub_hit(int chapter, int ordinal) {
  auto* epub = g_host.epub;
  if (epub == nullptr) return;
  if (chapter > 0) epub->go_to_chapter(chapter);
  epub->highlight_hit(chapter, ordinal, g_host.epub_find_query);
}

// ---------------------------------------------------------------------------
// EPUB table of contents (header popover)
// ---------------------------------------------------------------------------

void build_epub_toc_menu() {
  auto* epub = g_host.epub;
  if (epub == nullptr) return;
  auto* menu = g_menu_new();
  for (auto const& item : epub->toc()) {
    std::string const action =
        "win.epub-chapter::" + std::to_string(item.chapter);
    auto* menu_item = g_menu_item_new(item.title.c_str(), action.c_str());
    g_menu_append_item(menu, menu_item);
    g_object_unref(menu_item);
  }
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(g_host.header_toc_menu),
                                 G_MENU_MODEL(menu));
  g_object_unref(menu);
}

// ---------------------------------------------------------------------------
// backend boot (scaffold handshake + mac bootstrap semantics)
// ---------------------------------------------------------------------------

std::optional<rivet::linux_runtime::RacketRuntimeConfig> backend_config(
    std::string& error) {
  std::error_code ec;
  auto const exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    error = "cannot resolve the executable path";
    return std::nullopt;
  }
  auto const root = exe.parent_path();
  rivet::linux_runtime::RacketRuntimeConfig config;
  config.petite_boot = (root / "runtime" / "petite.boot").string();
  config.scheme_boot = (root / "runtime" / "scheme.boot").string();
  config.racket_boot = (root / "runtime" / "racket.boot").string();
  config.backend_bundle = (root / "res" / "core.zo").string();
  if (!std::filesystem::exists(config.petite_boot) ||
      !std::filesystem::exists(config.scheme_boot) ||
      !std::filesystem::exists(config.racket_boot) ||
      !std::filesystem::exists(config.backend_bundle)) {
    error = "Missing Rivet runtime layout (runtime/*.boot, res/core.zo) "
            "next to the executable. Build with raco rivet build/dev.";
    return std::nullopt;
  }
  config.executable_path = exe.string();
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;
  return config;
}

void relay_event(std::string const& name, rivet::Value const& value) {
  rivet_app::Event event;
  try {
    event = rivet_app::decode_event(name, value);
  } catch (...) {
    return;
  }
  host::dispatch_ui([&host = g_host, event]() {
    if (!host.ai_panel) return;
    std::visit(
        [&](auto const& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, rivet_app::Stream_chunkEvent>) {
            host.ai_panel->on_stream_chunk(e.value);
          } else if constexpr (std::is_same_v<T,
                                              rivet_app::Stream_doneEvent>) {
            host.ai_panel->on_stream_done(e.value);
          } else if constexpr (std::is_same_v<T,
                                              rivet_app::Stream_errorEvent>) {
            host.ai_panel->on_stream_error(e.value);
          }
        },
        event);
  });
}

void bootstrap() {
  auto* api = g_host.api.get();
  try {
    api->initialize().get();
    // Mirror the UI language (detected before the window built) into the
    // backend so backend-formatted text agrees with the host.
    api->set_locale(pdfgist::UiLanguageCode()).get();
    auto settings = api->get_settings().get();
    auto presets = api->list_presets().get();
    auto recents = api->get_recents().get();
    host::dispatch_ui([settings, presets, recents]() mutable {
      g_host.settings = std::move(settings);
      g_host.presets = std::move(presets);
      g_host.recents = std::move(recents);
      g_host.panel_ctx.settings = g_host.settings;
      g_host.panel_ctx.presets = g_host.presets;
      g_host.ready = true;
      gtk_label_set_text(g_host.status_label, "");
      g_host.ai_panel->sync_settings();
      refresh_recents_menu();
      // No bookshelf to refresh; recents remain file shortcuts.

      // Tabs opened before the backend was ready get their stored
      // annotations now (mac loadDocData needs the API too).
      for (auto& tab : g_host.tabs) load_annotations_for_tab(tab.get());

      auto pending = std::move(g_host.pending_open_paths);
      g_host.pending_open_paths.clear();
      for (auto const& path : pending) open_path(path);

      // Environment launch hook for deterministic automation and legacy
      // PdfGist callers. File-association launches use the queue above.
      char const* launch = getenv("BOOKLIGHT_OPEN");
      if (launch == nullptr || *launch == '\0') launch = getenv("PDFGIST_OPEN");
      if (launch != nullptr && *launch != '\0') {
        std::string const path = launch;
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                         return static_cast<char>(std::tolower(c));
                       });
        if (lower.size() > 4 &&
            (lower.substr(lower.size() - 4) == ".pdf" ||
             lower.substr(lower.size() - 5) == ".epub")) {
          open_path(path);
        }
      }
      // silent update check (throttled server-side), like the mac host
      g_timeout_add(
          3000,
          +[](gpointer) -> int {
            check_updates_silent();
            return G_SOURCE_REMOVE;
          },
          nullptr);
    });
  } catch (std::exception const& e) {
    std::string const message = e.what();
    host::dispatch_ui([message] {
      gtk_label_set_text(g_host.status_label,
                         Trf("ui.backend.error", message).c_str());
    });
  }
}

int on_backend_finished(gpointer) {
  if (g_host.startup_thread.joinable()) g_host.startup_thread.join();
  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::string error;
  {
    std::lock_guard lock(g_host.startup_mutex);
    backend = std::move(g_host.startup_backend);
    error = std::move(g_host.startup_error);
  }
  if (g_host.shutting_down.load(std::memory_order_acquire)) {
    if (backend != nullptr) backend->stop();
    return G_SOURCE_REMOVE;
  }
  if (!error.empty()) {
    gtk_label_set_text(g_host.status_label,
                       Trf("ui.backend.error", error).c_str());
    return G_SOURCE_REMOVE;
  }
  if (backend == nullptr) {
    gtk_label_set_text(g_host.status_label, Tr("ui.backend.no-instance").c_str());
    return G_SOURCE_REMOVE;
  }
  backend->set_event_handler(relay_event);
  g_host.backend = std::move(backend);
  g_host.api = std::make_unique<rivet_app::API>(*g_host.backend);
  g_host.panel_ctx.api = g_host.api.get();
  gtk_label_set_text(g_host.status_label, Tr("ui.backend-not-ready").c_str());
  g_host.bootstrap_thread = std::thread(bootstrap);
  return G_SOURCE_REMOVE;
}

void start_backend() {
  std::string error;
  auto config = backend_config(error);
  if (!config.has_value()) {
    gtk_label_set_text(g_host.status_label, error.c_str());
    return;
  }
  g_host.startup_thread = std::thread(
      [config = std::move(*config)]() mutable {
        auto backend = std::make_unique<rivet::linux_runtime::Backend>(
            std::move(config));
        try {
          backend->start();
          std::lock_guard lock(g_host.startup_mutex);
          g_host.startup_backend = std::move(backend);
        } catch (std::exception const& e) {
          std::lock_guard lock(g_host.startup_mutex);
          g_host.startup_error = e.what();
        }
        g_idle_add(on_backend_finished, nullptr);
      });
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// page-level editing (backend owns the PDF surgery; hosts render and save)
// ---------------------------------------------------------------------------

void page_extract_save(PdfTab* tab, rivet::Bytes bytes);

std::vector<std::int64_t> parse_page_ranges(std::string const& text,
                                            int page_count) {
  // "1-3,5" style, 1-based inclusive.
  std::vector<std::int64_t> pages;
  std::size_t i = 0;
  while (i < text.size()) {
    auto const sep = text.find(',', i);
    std::string const part =
        text.substr(i, sep == std::string::npos ? std::string::npos
                                                : sep - i);
    i = sep == std::string::npos ? text.size() : sep + 1;
    auto const dash = part.find('-');
    if (dash != std::string::npos) {
      int from = std::atoi(part.substr(0, dash).c_str());
      int to = std::atoi(part.substr(dash + 1).c_str());
      from = std::clamp(from, 1, page_count);
      to = std::clamp(to, 1, page_count);
      if (from > to) std::swap(from, to);
      for (int p = from; p <= to; ++p) {
        pages.push_back(p);
      }
    } else if (!part.empty()) {
      int const p = std::clamp(std::atoi(part.c_str()), 1, page_count);
      pages.push_back(p);
    }
  }
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  return pages;
}

void reload_tab_from_bytes(PdfTab* tab, rivet::Bytes const& bytes) {
  auto doc = PdfDocument::open_memory(bytes, nullptr);
  if (doc == nullptr) {
    host_alert(Trf("ui.pages.edit-failed", std::string("unreadable result")));
    return;
  }
  tab->reload_document(std::move(doc));
  build_outline_for(tab);
  populate_thumbs();
  refresh_forms_pane();  // form fields may differ in the edited document
  refresh_status();
  refresh_header_title();
}

template <typename Launch>
void run_page_edit_rpc(PdfTab* tab, Launch&& launch) {
  launch([tab](rivet_app::Result<rivet::Bytes> result) {
    host::dispatch_ui([tab, result]() mutable {
      if (result.succeeded()) {
        reload_tab_from_bytes(tab, result.get());
      } else {
        host_alert(Trf("ui.pages.edit-failed",
                       AiPanel::describe_error(result.error)));
      }
    });
  });
}

void page_delete_current() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || g_host.api == nullptr) return;
  run_page_edit_rpc(tab, [&](auto done) {
    (void)g_host.api->edit_delete_pages_async(tab->path(),
                                            {tab->current_page()}, done);
  });
}

void page_rotate_current() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || g_host.api == nullptr) return;
  run_page_edit_rpc(tab, [&](auto done) {
    (void)g_host.api->edit_rotate_pages_async(tab->path(),
                                              {tab->current_page()}, 90,
                                              done);
  });
}

void page_insert_blank_after() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || g_host.api == nullptr) return;
  run_page_edit_rpc(tab, [&](auto done) {
    (void)g_host.api->edit_insert_blank_after_async(tab->path(),
                                                    tab->current_page(),
                                                    done);
  });
}

void page_merge_pick();

void page_merge_with(std::string const& other) {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || g_host.api == nullptr) return;
  run_page_edit_rpc(tab, [&](auto done) {
    (void)g_host.api->edit_append_doc_async(tab->path(), other, done);
  });
}

void page_merge_pick() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr) return;
  auto* chooser = gtk_file_chooser_native_new(
      Tr("ui.pages.merge").c_str(), g_host.window, GTK_FILE_CHOOSER_ACTION_OPEN,
      Tr("ui.dialog.merge").c_str(), Tr("ui.password.cancel").c_str());
  auto* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, Tr("ui.filter.pdf").c_str());
  gtk_file_filter_add_mime_type(filter, "application/pdf");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
  hook(
      chooser, "response",
      +[](GtkNativeDialog* native, int response, gpointer) {
        if (response == GTK_RESPONSE_ACCEPT) {
          if (GFile* file = gtk_file_chooser_get_file(
                  GTK_FILE_CHOOSER(native));
              file != nullptr) {
            gchar* path = g_file_get_path(file);
            if (path != nullptr) page_merge_with(path);
            g_free(path);
            g_object_unref(file);
          }
        }
        g_object_unref(native);
      },
      nullptr, nullptr, static_cast<GConnectFlags>(0));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

void page_extract_pick() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || g_host.api == nullptr) return;
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.pages.extract").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL,
      Tr("ui.dialog.extract").c_str(), GTK_RESPONSE_OK, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_box_append(GTK_BOX(area), box);
  auto* label = gtk_label_new(nullptr);
  gtk_label_set_markup(
      GTK_LABEL(label),
      Trf("ui.pages.extract-prompt",
          std::to_string(tab->document()->page_count()))
          .c_str());
  gtk_box_append(GTK_BOX(box), label);
  auto* entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "1-3,5");
  gtk_editable_set_text(GTK_EDITABLE(entry),
                        std::to_string(tab->current_page()).c_str());
  gtk_box_append(GTK_BOX(box), entry);
  g_object_ref(dialog);
  hook(
      dialog, "response",
      +[](GtkDialog* dialog, int response, gpointer user_data) {
        auto* entry = GTK_ENTRY(user_data);
        if (response == GTK_RESPONSE_OK) {
          auto* tab2 = g_host.active_pdf_only();
          std::string const spec = gtk_editable_get_text(
              GTK_EDITABLE(entry));
          if (tab2 != nullptr && !spec.empty()) {
            auto const pages =
                parse_page_ranges(spec, tab2->document()->page_count());
            if (!pages.empty()) {
              (void)g_host.api->edit_extract_pages_async(
                  tab2->path(), pages,
                    [tab2](rivet_app::Result<rivet::Bytes> result) {
                    host::dispatch_ui([tab2, result]() mutable {
                      if (!result.succeeded()) {
                        host_alert(Trf("ui.pages.edit-failed",
                                       AiPanel::describe_error(result.error)));
                        return;
                      }
                      // extract goes to a new file (mac parity)
                      page_extract_save(tab2, result.get());
                    });
                  });
            }
          }
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
        g_object_unref(dialog);
      },
      entry);
  gtk_window_present(GTK_WINDOW(dialog));
}

void page_extract_save(PdfTab* tab, rivet::Bytes bytes) {
  std::string title = tab->title();
  if (title.size() >= 4 && title.compare(title.size() - 4, 4, ".pdf") == 0) {
    title.erase(title.size() - 4);
  }
  auto* chooser = gtk_file_chooser_native_new(
      Tr("ui.pages.extract-save-title").c_str(), g_host.window,
      GTK_FILE_CHOOSER_ACTION_SAVE, Tr("ui.dialog.save").c_str(),
      Tr("ui.password.cancel").c_str());
  gtk_file_chooser_set_current_name(
      GTK_FILE_CHOOSER(chooser),
      (title + Tr("ui.pages.extract-suffix") + ".pdf").c_str());
  auto* payload = new rivet::Bytes(std::move(bytes));
  hook(
      chooser, "response",
      +[](GtkNativeDialog* native, int response,
                     gpointer user_data) {
        std::unique_ptr<rivet::Bytes> bytes(
            static_cast<rivet::Bytes*>(user_data));
        if (response == GTK_RESPONSE_ACCEPT) {
          if (GFile* file =
                  gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
              file != nullptr) {
            gchar* path = g_file_get_path(file);
            g_object_unref(file);
            if (path != nullptr) {
              std::string const target = path;
              g_free(path);
              std::thread([target, bytes = *bytes]() {
                std::string error;
                GError* gerror = nullptr;
                if (!g_file_set_contents(
                        target.c_str(),
                        reinterpret_cast<char const*>(bytes.data()),
                        static_cast<gssize>(bytes.size()), &gerror)) {
                  error = gerror != nullptr ? gerror->message
                                            : "write failed";
                  if (gerror != nullptr) g_error_free(gerror);
                }
                host::dispatch_ui([target, error]() {
                  if (!error.empty()) {
                    host_alert(Trf("ui.pages.save-failed", error));
                  }
                });
              }).detach();
            }
          }
        }
        g_object_unref(native);
      },
      payload,
      +[](gpointer p, GClosure*) { delete static_cast<rivet::Bytes*>(p); },
      static_cast<GConnectFlags>(0));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

// ---------------------------------------------------------------------------
// tools (mac Tools menu / ReaderModel parity: watermark + the two exports)
// ---------------------------------------------------------------------------

// "book.pdf" → "book" — the export default names are <stem>-<page>.png and
// <stem>.txt (mac saveData nameFieldStringValue parity).
std::string title_stem(std::string const& title) {
  std::size_t const dot = title.rfind('.');
  return dot == std::string::npos ? title : title.substr(0, dot);
}

std::string ascii_trim(std::string const& s) {
  auto const begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  auto const end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// Shared Save As dialog. This does not mutate the source PDF. A completion
// callback allows close/quit to proceed only after a successful disk write.
struct SaveBytesRequest {
  rivet::Bytes data;
  std::function<void(bool)> on_complete;
  std::string protected_source;
};

void save_bytes_dialog(std::string const& title,
                       std::string const& default_name, rivet::Bytes data,
                       std::function<void(bool)> on_complete = {},
                       std::string protected_source = {}) {
  auto* chooser = gtk_file_chooser_native_new(
      title.c_str(), g_host.window, GTK_FILE_CHOOSER_ACTION_SAVE,
      Tr("ui.dialog.save").c_str(), Tr("ui.password.cancel").c_str());
  // Portal file chooser confirms overwrite of existing non-source targets.
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                     default_name.c_str());
  auto* request = new SaveBytesRequest{
      std::move(data), std::move(on_complete), std::move(protected_source)};
  hook(chooser, "response",
       +[](GtkNativeDialog* native, int response, gpointer user_data) {
         auto* request = static_cast<SaveBytesRequest*>(user_data);
         bool saved = false;
         if (response == GTK_RESPONSE_ACCEPT) {
           if (GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
               file != nullptr) {
             gchar* path = g_file_get_path(file);
             g_object_unref(file);
             if (path != nullptr) {
               // Save As may overwrite an explicitly chosen different file;
               // it may NEVER replace the PDF that the reader opened.
               bool original = false;
               if (!request->protected_source.empty()) {
                 gchar* from = g_canonicalize_filename(
                     request->protected_source.c_str(), nullptr);
                 gchar* to = g_canonicalize_filename(path, nullptr);
                 original = g_strcmp0(from, to) == 0;
                 g_free(from);
                 g_free(to);
                 // Also reject symlink/hard-link aliases of the source.
                 std::error_code ec;
                 original = original || std::filesystem::equivalent(
                     std::filesystem::path(path),
                     std::filesystem::path(request->protected_source), ec);
               }
               if (original) {
                 host_alert(Tr("ui.pdf.preserve-original"));
               } else {
                 GError* error = nullptr;
                 saved = g_file_set_contents(
                     path,
                     reinterpret_cast<char const*>(request->data.data()),
                     static_cast<gssize>(request->data.size()), &error);
                 if (!saved) {
                   std::string const message =
                       error != nullptr ? error->message : "write failed";
                   host_alert(Trf("ui.export.failed", message));
                 }
                 if (error != nullptr) g_error_free(error);
               }
               g_free(path);
             }
           }
         }
         auto completed = std::move(request->on_complete);
         g_object_unref(native);  // frees request via closure destroy notify
         if (completed) completed(saved);
       },
       request,
       +[](gpointer p, GClosure*) { delete static_cast<SaveBytesRequest*>(p); },
       static_cast<GConnectFlags>(0));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

// Export the actual current in-memory document, not the original bytes.
// Cancellation/failure leaves the edited tab open and visibly dirty.
void save_edited_pdf_for_tab(PdfTab* tab, std::function<void(bool)> on_complete) {
  if (tab == nullptr || !pdf_tab_still_open(tab)) {
    if (on_complete) on_complete(false);
    return;
  }
  auto const document = tab->document();
  if (document == nullptr) {
    if (on_complete) on_complete(false);
    return;
  }
  rivet::Bytes data = document->memory_bytes();
  if (data.empty()) {
    // An unedited PDF can still be exported on an explicit Tools request.
    gchar* raw = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    if (!g_file_get_contents(tab->path().c_str(), &raw, &length, &error)) {
      std::string const message =
          error != nullptr ? error->message : "read failed";
      if (error != nullptr) g_error_free(error);
      host_alert(Trf("ui.export.failed", message));
      if (on_complete) on_complete(false);
      return;
    }
    auto const* first = reinterpret_cast<std::uint8_t const*>(raw);
    data.assign(first, first + length);
    g_free(raw);
  }
  save_bytes_dialog(
      Tr("ui.dialog.save") + " PDF", title_stem(tab->title()) + "-edited.pdf",
      std::move(data),
      [tab, document, on_complete = std::move(on_complete)](bool saved) {
        bool const unchanged = pdf_tab_still_open(tab) &&
                               tab->document() == document;
        if (saved && unchanged) {
          tab->pdf_edits_pending = false;
          refresh_header_title();
        }
        if (on_complete) on_complete(saved && unchanged);
      },
      tab->path());
}

void tools_save_pdf() {
  save_edited_pdf_for_tab(g_host.active_pdf_only(), {});
}

// Save As / Discard / Cancel also covers tab-close, window-manager close and
// Quit. A canceled/failed export MUST NOT invoke the pending close action.
void confirm_close_dirty_pdf(PdfTab* tab, std::function<void()> proceed) {
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.pdf.pending-title").c_str(), g_host.window, GTK_DIALOG_MODAL,
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL,
      Tr("ui.pdf.discard").c_str(), GTK_RESPONSE_REJECT,
      Tr("ui.pdf.save-as").c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* message = gtk_label_new(Tr("ui.pdf.pending-body").c_str());
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_widget_set_margin_top(message, 16);
  gtk_widget_set_margin_bottom(message, 16);
  gtk_widget_set_margin_start(message, 16);
  gtk_widget_set_margin_end(message, 16);
  gtk_box_append(GTK_BOX(area), message);
  struct CloseRequest {
    PdfTab* tab;
    std::function<void()> proceed;
  };
  auto* request = new CloseRequest{tab, std::move(proceed)};
  hook(dialog, "response",
       +[](GtkDialog* dialog, gint response, gpointer user_data) {
         auto* request = static_cast<CloseRequest*>(user_data);
         PdfTab* const tab = request->tab;
         auto proceed = request->proceed;
         gtk_window_destroy(GTK_WINDOW(dialog));
         if (!pdf_tab_still_open(tab)) return;
         if (response == GTK_RESPONSE_REJECT) {
           tab->pdf_edits_pending = false;  // explicitly discard; no writes
           proceed();
         } else if (response == GTK_RESPONSE_ACCEPT) {
           save_edited_pdf_for_tab(
               tab, [proceed = std::move(proceed)](bool saved) {
                 if (saved) proceed();
               });
         }
       }, request,
       +[](gpointer p, GClosure*) { delete static_cast<CloseRequest*>(p); },
       static_cast<GConnectFlags>(0));
  gtk_window_present(GTK_WINDOW(dialog));
}

// Tools > Add Watermark (mac WatermarkSheet parity): watermark text, a
// 12..144 pt size stepper, a 5..100% opacity slider with a live percent
// label — baked in-memory through edit-bake-text (opacity in milli-percent
// = percent * 1000) and reloaded like the other page edits.
void tools_watermark_dialog() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr || g_host.api == nullptr) return;
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.tools.watermark").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL,
      Tr("ui.watermark.apply").c_str(), GTK_RESPONSE_OK, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_widget_set_margin_top(grid, 12);
  gtk_widget_set_margin_bottom(grid, 6);
  gtk_widget_set_margin_start(grid, 12);
  gtk_widget_set_margin_end(grid, 12);
  gtk_box_append(GTK_BOX(area), grid);
  auto attach = [&](int row, char const* label, GtkWidget* field) {
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(label), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field, 1, row, 1, 1);
  };
  auto* text = gtk_entry_new();
  // the sheet ships with a starter watermark (mac @State text = "机密";
  // deliberately not localized, the mac default is the same constant)
  gtk_editable_set_text(GTK_EDITABLE(text), "机密");
  attach(0, Tr("ui.watermark.text").c_str(), text);
  auto* size = gtk_spin_button_new_with_range(12, 144, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(size), 48);
  attach(1, Tr("ui.watermark.size").c_str(), size);
  auto* opacity = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 5, 100,
                                           1);
  gtk_range_set_value(GTK_RANGE(opacity), 30);
  gtk_scale_set_draw_value(GTK_SCALE(opacity), FALSE);
  gtk_widget_set_hexpand(opacity, TRUE);
  auto* percent = gtk_label_new("30%");
  gtk_widget_set_size_request(percent, 40, -1);
  gtk_widget_set_halign(percent, GTK_ALIGN_END);
  hook(
      opacity, "value-changed",
      +[](GtkRange* range, gpointer user_data) {
        std::string const label =
            std::to_string(static_cast<int>(gtk_range_get_value(range))) + "%";
        gtk_label_set_text(GTK_LABEL(user_data), label.c_str());
      },
      percent);
  auto* opacity_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(opacity_row), opacity);
  gtk_box_append(GTK_BOX(opacity_row), percent);
  attach(2, Tr("ui.watermark.opacity").c_str(), opacity_row);

  // Apply stays insensitive while the watermark text is blank (mac
  // .disabled(text.trimmed.isEmpty) parity).
  auto sync_apply = +[](GtkEditable* editable, gpointer user_data) {
    std::string const value = gtk_editable_get_text(editable);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(user_data), GTK_RESPONSE_OK,
                                      !ascii_trim(value).empty());
  };
  sync_apply(GTK_EDITABLE(text), dialog);
  hook(text, "changed", sync_apply, dialog);

  hook(
      dialog, "response",
      +[](GtkDialog* dialog, int response, gpointer user_data) {
        auto* widgets = static_cast<std::vector<GtkWidget*>*>(user_data);
        if (response == GTK_RESPONSE_OK) {
          auto* tab2 = g_host.active_pdf_only();
          std::string const watermark = gtk_editable_get_text(
              GTK_EDITABLE((*widgets)[0]));
          if (tab2 != nullptr && g_host.api != nullptr &&
              !watermark.empty()) {
            int const size_points = static_cast<int>(
                gtk_spin_button_get_value(
                    GTK_SPIN_BUTTON((*widgets)[1])));
            int const opacity_percent = static_cast<int>(
                gtk_range_get_value(GTK_RANGE((*widgets)[2])));
            (void)g_host.api->edit_bake_text_async(
                tab2->path(), watermark, size_points,
                opacity_percent * 1000, {},
                [tab2](rivet_app::Result<rivet::Bytes> result) {
                  host::dispatch_ui([tab2, result]() mutable {
                    if (result.succeeded()) {
                      reload_tab_from_bytes(tab2, result.get());
                    } else {
                      host_alert(Trf("ui.pages.edit-failed",
                                     AiPanel::describe_error(result.error)));
                    }
                  });
                });
          }
        }
        delete widgets;
        gtk_window_destroy(GTK_WINDOW(dialog));
      },
      new std::vector<GtkWidget*>{text, size, opacity});
  gtk_window_present(GTK_WINDOW(dialog));
}

// Tools > Export Current Page as Image: the visible page rendered at 2x
// (2 device pixels per point, natural colors — no dark inversion), PNG
// bytes through cairo, then the shared save dialog.
void tools_export_page_image() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr) return;
  int const page = tab->current_page();
  cairo_surface_t* surface =
      tab->document()->render(page - 1, 2.0, false, nullptr, nullptr);
  if (surface == nullptr) {
    host_alert(Trf("ui.export.failed", std::string("render")));
    return;
  }
  rivet::Bytes png;
  cairo_status_t const rc = cairo_surface_write_to_png_stream(
      surface,
      +[](void* closure, unsigned char const* data,
          unsigned int length) -> cairo_status_t {
        auto* out = static_cast<rivet::Bytes*>(closure);
        out->insert(out->end(), data, data + length);
        return CAIRO_STATUS_SUCCESS;
      },
      &png);
  cairo_surface_destroy(surface);
  if (rc != CAIRO_STATUS_SUCCESS) {
    host_alert(Trf("ui.export.failed", std::string("png")));
    return;
  }
  save_bytes_dialog(Tr("ui.tools.export-page"),
                    title_stem(tab->title()) + "-" + std::to_string(page) +
                        ".png",
                    std::move(png));
}

// Notes sidebar export (mac NotesPane export button parity): the backend
// renders the Markdown over the bytes this tab holds (unsaved edits
// included — no flush race).
void export_annotations_ui() {
  auto* tab = g_host.active_pdf_only();
  auto* epub = g_host.epub_active() ? g_host.epub : nullptr;
  if (epub != nullptr) {
    if (g_host.api == nullptr || !g_host.ready) return;
    auto bytes = encode_doc_data(epub->doc_data());
    std::string const title = epub->title();
    g_host.api->export_annotations_async(
        std::move(bytes), title,
        [title](rivet_app::Result<std::string> result) {
          host::dispatch_ui([title, result]() mutable {
            if (!result.succeeded()) return;
            auto const markdown = result.get();
            save_bytes_dialog(
                Tr("ui.notes.export"),
                title_stem(title) + Tr("ui.notes.export-suffix") + ".md",
                rivet::Bytes(markdown.begin(), markdown.end()));
          });
        });
    return;
  }
  if (tab == nullptr || g_host.api == nullptr || !g_host.ready) return;
  auto bytes = encode_doc_data(tab->doc_data());
  std::string const title = tab->title();
  g_host.api->export_annotations_async(
      std::move(bytes), title,
      [title](rivet_app::Result<std::string> result) {
        host::dispatch_ui([title, result]() mutable {
          if (!result.succeeded()) return;
          auto const markdown = result.get();
          save_bytes_dialog(
              Tr("ui.notes.export"),
              title_stem(title) + Tr("ui.notes.export-suffix") + ".md",
              rivet::Bytes(markdown.begin(), markdown.end()));
        });
      });
}

// Tools > Export Text: the whole document's text layer as .txt (mac
// exportDocumentText — a whitespace-only document reports ui.need.text).
void tools_export_text() {
  auto* tab = g_host.active_pdf_only();
  if (tab == nullptr) return;
  int const pages = tab->document()->page_count();
  std::string joined;
  for (int i = 1; i <= pages; ++i) {
    std::string const text = tab->page_text(i);
    if (text.empty()) continue;
    joined += text;
    joined += '\n';
  }
  std::string const text = ascii_trim(joined);
  if (text.empty()) {
    host_alert(Tr("ui.need.text"));
    return;
  }
  save_bytes_dialog(Tr("ui.tools.export-text"),
                    title_stem(tab->title()) + ".txt",
                    rivet::Bytes(text.begin(), text.end()));
}



}  // namespace

namespace {

// ---------------------------------------------------------------------------
// online updates — check → consent → download → three-shape install
// (taskly's flow; pdfgist's backend verifies and downloads, this host owns
// installation. deb/rpm upgrade through the package manager; AppImage
// swaps the sibling image in place (backend-side) and re-execs on consent;
// tarball installs get the extraction hint.)
// ---------------------------------------------------------------------------

// Parses into a shared handle that keeps the JsonParser alive as long as
// the caller holds the object (json-glib nodes are owned by the parser).
std::shared_ptr<JsonObject> parse_json(std::string const& text) {
  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  json_parser_load_from_data(parser, text.c_str(),
                             static_cast<gssize>(text.size()), &error);
  if (error != nullptr) {
    g_error_free(error);
    g_object_unref(parser);
    return nullptr;
  }
  JsonObject* obj = json_node_get_object(json_parser_get_root(parser));
  if (obj == nullptr) {
    g_object_unref(parser);
    return nullptr;
  }
  return std::shared_ptr<JsonObject>(
      obj, [parser](JsonObject*) { g_object_unref(parser); });
}

std::string json_str(JsonObject* obj, char const* key) {
  if (!json_object_has_member(obj, key)) return {};
  auto* node = json_object_get_member(obj, key);
  if (node == nullptr || json_node_is_null(node)) return {};
  return json_node_get_string(node) != nullptr
             ? json_node_get_string(node)
             : std::string{};
}

std::int64_t json_int(JsonObject* obj, char const* key) {
  if (!json_object_has_member(obj, key)) return 0;
  auto* node = json_object_get_member(obj, key);
  if (node == nullptr || json_node_is_null(node)) return 0;
  return static_cast<std::int64_t>(json_node_get_int(node));
}

Host::UpdateCheckResult parse_check(rivet::Bytes const& bytes) {
  Host::UpdateCheckResult result;
  std::string const text(bytes.begin(), bytes.end());
  auto obj = parse_json(text);
  if (obj == nullptr) {
    result.status = "error";
    result.message = "malformed update payload";
    return result;
  }
  result.status = json_str(obj.get(), "status");
  result.message = json_str(obj.get(), "message");
  result.current_version = json_str(obj.get(), "currentVersion");
  result.available_version = json_str(obj.get(), "availableVersion");
  result.installer = json_str(obj.get(), "installer");
  result.size_bytes = json_int(obj.get(), "sizeBytes");
  return result;
}

struct UpdateStateJson {
  std::string phase;
  std::string message;
  std::string downloaded_path;
  std::string available_version;
  int percent{0};
};

UpdateStateJson parse_state(rivet::Bytes const& bytes) {
  UpdateStateJson state;
  std::string const text(bytes.begin(), bytes.end());
  auto obj = parse_json(text);
  if (obj == nullptr) return state;
  state.phase = json_str(obj.get(), "phase");
  state.message = json_str(obj.get(), "message");
  state.downloaded_path = json_str(obj.get(), "downloadedPath");
  state.available_version = json_str(obj.get(), "availableVersion");
  state.percent = static_cast<int>(json_int(obj.get(), "percent"));
  return state;
}

void update_message_dialog(std::string const& title,
                           std::string const& body) {
  auto* dialog = gtk_message_dialog_new(
      g_host.window, static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                                 GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", title.c_str());
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                           body.c_str());
  hook(dialog, "response",
                   +[](GtkDialog* dialog, gint, gpointer) {
                     gtk_window_destroy(GTK_WINDOW(dialog));
                   },
                   nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

void show_update_error(std::string const& message) {
  update_message_dialog(Tr("ui.update.error"), message);
}

void stop_update_poll() {
  if (g_host.update.poll_source != 0) {
    g_source_remove(g_host.update.poll_source);
    g_host.update.poll_source = 0;
  }
}

void close_update_progress() {
  if (g_host.update.progress != nullptr) {
    gtk_window_destroy(GTK_WINDOW(g_host.update.progress));
    g_host.update.progress = nullptr;
    g_host.update.bar = nullptr;
    g_host.update.label = nullptr;
  }
}

// AppImage: the backend already swapped the sibling .AppImage over the
// running image (sidecar checksum verified), so the only step left is the
// user's consent to re-exec. Declining keeps the old process running; the
// next launch is the new version regardless.
void show_appimage_restart_dialog() {
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.update.downloaded").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.update.install-now").c_str(), GTK_RESPONSE_ACCEPT,
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CLOSE, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* hint = gtk_label_new(Tr("ui.update.appimage-hint").c_str());
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(hint), 60);
  gtk_widget_set_margin_top(hint, 12);
  gtk_widget_set_margin_bottom(hint, 6);
  gtk_widget_set_margin_start(hint, 12);
  gtk_widget_set_margin_end(hint, 12);
  gtk_box_append(GTK_BOX(area), hint);
  hook(
      dialog, "response",
      +[](GtkDialog* dialog, gint response, gpointer) {
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (response != GTK_RESPONSE_ACCEPT) return;
        char const* self = getenv("APPIMAGE");
        if (self == nullptr || self[0] == '\0') return;
        execl(self, self, static_cast<char*>(nullptr));
        // execl only returns on failure — surface it; the old image keeps
        // running either way.
        show_update_error(std::string("re-exec failed: ") +
                          std::strerror(errno));
      },
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

// deb/rpm installs cannot self-swap a system-managed prefix: explain the
// upgrade path and show where the downloaded archive sits.
void show_package_manager_dialog(std::string const& path) {
  update_message_dialog(
      Tr("ui.update.downloaded"),
      Tr("ui.update.package-downloaded") +
          (path.empty() ? "" : "\n\n" + path));
}

// tarball: installation stays manual, so the dialog explains the step and
// opens the folder.
void show_downloaded_dialog(std::string const& path) {
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.update.downloaded").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.update.close").c_str(), GTK_RESPONSE_CLOSE, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 6);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_box_append(GTK_BOX(area), content);
  auto* hint = gtk_label_new(Tr("ui.update.tarball-hint").c_str());
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(hint), 60);
  gtk_box_append(GTK_BOX(content), hint);
  if (!path.empty()) {
    auto* path_label = gtk_label_new(path.c_str());
    gtk_label_set_selectable(GTK_LABEL(path_label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(path_label), 60);
    gtk_box_append(GTK_BOX(content), path_label);
    auto* open = gtk_button_new_with_label(Tr("ui.update.open-folder").c_str());
    hook(
        open, "clicked",
        +[](GtkButton*, gpointer user_data) {
          auto const* p = static_cast<std::string const*>(user_data);
          auto const slash = p->find_last_of('/');
          if (slash == std::string::npos) return;
          std::string const dir = p->substr(0, slash);
          if (dir.empty()) return;
          GFile* folder = g_file_new_for_path(dir.c_str());
          gchar* uri = g_file_get_uri(folder);
          gtk_show_uri(g_host.window, uri, GDK_CURRENT_TIME);
          g_free(uri);
          g_object_unref(folder);
        },
        new std::string(path),
        +[](gpointer data, GClosure*) { delete static_cast<std::string*>(data); },
        static_cast<GConnectFlags>(0));
    gtk_box_append(GTK_BOX(content), open);
  }
  hook(dialog, "response",
                   +[](GtkDialog* dialog, gint, gpointer) {
                     gtk_window_destroy(GTK_WINDOW(dialog));
                   },
                   nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

int on_update_poll_tick(gpointer) {
  if (g_host.api == nullptr || g_host.update.progress == nullptr) {
    g_host.update.poll_source = 0;
    return G_SOURCE_REMOVE;
  }
  (void)g_host.api->update_state_async(
      [](rivet_app::Result<rivet::Bytes> result) {
        host::dispatch_ui([result]() mutable {
          if (g_host.update.progress == nullptr || !result.succeeded()) {
            return;  // detached UI or a hiccup — keep polling
          }
          UpdateStateJson const state = parse_state(result.get());
          int const percent = std::clamp(state.percent, 0, 100);
          if (g_host.update.bar != nullptr) {
            gtk_progress_bar_set_fraction(g_host.update.bar,
                                          percent / 100.0);
          }
          if (g_host.update.label != nullptr) {
            gtk_label_set_text(
                g_host.update.label,
                (Tr("ui.update.downloading") + " " + std::to_string(percent) +
                 "%")
                    .c_str());
          }
          if (state.phase != "downloaded" && state.phase != "error") return;
          stop_update_poll();
          close_update_progress();
          g_host.update.in_flight = false;
          if (state.phase == "error") {
            show_update_error(state.message.empty() ? "unknown failure"
                                                    : state.message);
            return;
          }
          auto const shape = host::install_shape();
          if (shape == "appimage") {
            show_appimage_restart_dialog();
            return;
          }
          if (shape == "package") {
            show_package_manager_dialog(state.downloaded_path);
            return;
          }
          show_downloaded_dialog(state.downloaded_path);
        });
      });
  return G_SOURCE_CONTINUE;
}

void start_update_download() {
  if (g_host.api == nullptr || g_host.update.in_flight) return;
  g_host.update.in_flight = true;

  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.update.downloading").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 6);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_box_append(GTK_BOX(area), content);
  auto* bar = gtk_progress_bar_new();
  gtk_widget_set_size_request(bar, 300, -1);
  gtk_box_append(GTK_BOX(content), bar);
  auto* label = gtk_label_new((Tr("ui.update.downloading") + " 0%").c_str());
  gtk_box_append(GTK_BOX(content), label);
  // Cancel only detaches the progress UI — the backend keeps downloading
  // (family parity: there is no cancel RPC).
  hook(dialog, "response",
                   +[](GtkDialog* dialog, gint, gpointer) {
                     stop_update_poll();
                     g_host.update.progress = nullptr;
                     g_host.update.bar = nullptr;
                     g_host.update.label = nullptr;
                     g_host.update.in_flight = false;
                     gtk_window_destroy(GTK_WINDOW(dialog));
                   },
                   nullptr);
  hook(
      dialog, "destroy",
      +[](GtkWidget*, gpointer) {
        stop_update_poll();
        g_host.update.progress = nullptr;
        g_host.update.bar = nullptr;
        g_host.update.label = nullptr;
      },
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
  g_host.update.progress = GTK_DIALOG(dialog);
  g_host.update.bar = GTK_PROGRESS_BAR(bar);
  g_host.update.label = GTK_LABEL(label);
  g_host.update.poll_source = g_timeout_add(400, on_update_poll_tick, nullptr);

  (void)g_host.api->start_download_async(
      [](rivet_app::Result<void> result) {
        host::dispatch_ui([result]() mutable {
          if (g_host.update.progress == nullptr) return;  // detached UI
          if (result.succeeded()) return;  // the poll owns the rest
          stop_update_poll();
          close_update_progress();
          g_host.update.in_flight = false;
          show_update_error(AiPanel::describe_error(result.error));
        });
      });
}

void show_update_consent(Host::UpdateCheckResult const& check) {
  if (host::install_shape() == "package") {
    // deb/rpm upgrade through the package manager; never download or exec.
    update_message_dialog(
        Tr("ui.update.available"),
        Trf("ui.update.package-available",
            check.available_version.empty() ? std::string("?")
                                            : check.available_version));
    return;
  }
  std::string size_part;
  if (check.size_bytes > 0) {
    size_part = " (" + host::human_size(check.size_bytes) + ")";
  }
  std::string const body =
      Trf("ui.update.consent-body",
          check.available_version.empty() ? std::string("?")
                                          : check.available_version,
          size_part);
  auto* dialog = gtk_dialog_new_with_buttons(
      Tr("ui.update.available").c_str(), g_host.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      Tr("ui.password.cancel").c_str(), GTK_RESPONSE_CANCEL,
      Tr("ui.update.download").c_str(), GTK_RESPONSE_ACCEPT, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* text = gtk_label_new(body.c_str());
  gtk_label_set_wrap(GTK_LABEL(text), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(text), 60);
  gtk_widget_set_margin_top(text, 12);
  gtk_widget_set_margin_bottom(text, 6);
  gtk_widget_set_margin_start(text, 12);
  gtk_widget_set_margin_end(text, 12);
  gtk_box_append(GTK_BOX(area), text);
  hook(
      dialog, "response",
      +[](GtkDialog* dialog, gint response, gpointer) {
        bool const go = response == GTK_RESPONSE_ACCEPT;
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (go) start_update_download();
      },
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

void handle_check_result(Host::UpdateCheckResult const& check,
                         bool present) {
  if (check.status == "available") {
    show_update_consent(check);
    return;
  }
  if (!present) return;
  if (check.status == "up-to-date") {
    update_message_dialog(Tr("ui.update.check-title"),
                          Tr("ui.update.up-to-date"));
    return;
  }
  show_update_error(check.message.empty() ? "unknown failure"
                                          : check.message);
}

void check_updates(bool manual) {
  if (g_host.api == nullptr || !g_host.ready) {
    if (manual) {
      update_message_dialog(Tr("ui.update.check-title"),
                            Tr("ui.backend-not-ready"));
    }
    return;
  }
  (void)g_host.api->check_updates_async(
      manual, [manual](rivet_app::Result<rivet::Bytes> result) {
        host::dispatch_ui([manual, result]() mutable {
          if (!result.succeeded()) {
            if (manual) {
              show_update_error(AiPanel::describe_error(result.error));
            }
            return;
          }
          handle_check_result(parse_check(result.get()), manual);
        });
      });
}

void check_updates_silent() { check_updates(false); }

void check_updates_manual() { check_updates(true); }

// ---------------------------------------------------------------------------
// About
// ---------------------------------------------------------------------------

void show_about() {
  GtkWidget* dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "Booklight");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog),
                               rivet_app::kVersion);
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                               "https://pdfgist.jrtx.site");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                     "pdfgist.jrtx.site");
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                    GTK_LICENSE_AGPL_3_0);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                Tr("ui.about.blurb").c_str());
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_host.window);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  hook(dialog, "response",
                   +[](GtkDialog* dialog, gint, gpointer) {
                     gtk_window_destroy(GTK_WINDOW(dialog));
                   },
                   nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}



}  // namespace

namespace {

// ---------------------------------------------------------------------------
// window assembly
// ---------------------------------------------------------------------------

constexpr int kLeftSidebarWidth = 232;
constexpr int kAiSidebarWidth = 400;

void update_main_stack() {
  // welcome when no documents are open; reader for active documents
  if (g_host.main_stack == nullptr) return;
  gint const pages = gtk_notebook_get_n_pages(g_host.notebook);
  bool const reading = pages > 0;
  gtk_stack_set_visible_child_name(GTK_STACK(g_host.main_stack),
                                   reading ? "reader" : "shelf");
  bool const shelf_active = !reading;
  g_host.shelf_visible = shelf_active;
  gtk_widget_set_visible(GTK_WIDGET(g_host.header_edit_menu), pages > 0);
  if (g_host.header_bookmark != nullptr) {
    gtk_widget_set_visible(GTK_WIDGET(g_host.header_bookmark), pages > 0);
    gtk_widget_set_visible(GTK_WIDGET(g_host.header_split), pages > 0);
    gtk_widget_set_visible(GTK_WIDGET(g_host.header_tts), pages > 0);
    gtk_widget_set_visible(GTK_WIDGET(g_host.header_print), pages > 0);
  }
}

void build_primary_menu();

void rebuild_welcome_recents() {
  if (g_host.welcome_recents == nullptr) return;
  auto* box = GTK_WIDGET(g_host.welcome_recents);
  while (GtkWidget* child = gtk_widget_get_first_child(box)) {
    gtk_box_remove(g_host.welcome_recents, child);
  }
  gtk_widget_set_visible(box, !g_host.recents.empty());
  if (g_host.recents.empty()) return;
  auto* title = gtk_label_new(Tr("ui.viewer.recents").c_str());
  gtk_widget_add_css_class(title, "heading");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(g_host.welcome_recents, title);
  int count = 0;
  for (auto const& recent : g_host.recents) {
    if (++count > 10) break;
    gchar* name = g_path_get_basename(recent.path.c_str());
    auto* button = gtk_button_new_with_label(name);
    g_free(name);
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_widget_set_tooltip_text(button, recent.path.c_str());
    g_object_set_data_full(G_OBJECT(button), "booklight-recent-path",
                           g_strdup(recent.path.c_str()), g_free);
    hook(button, "clicked", +[](GtkButton* b, gpointer) {
      auto const* path = static_cast<char const*>(
          g_object_get_data(G_OBJECT(b), "booklight-recent-path"));
      if (path != nullptr) {
        int page = 1;
        for (auto const& recent : g_host.recents) {
          if (recent.path == path) {
            page = std::max(1, static_cast<int>(recent.page));
            break;
          }
        }
        open_path(path, page);
      }
    }, nullptr);
    gtk_box_append(g_host.welcome_recents, button);
  }
}

void refresh_recents_menu() {
  build_primary_menu();
  rebuild_welcome_recents();
}

void run_find_ui() {
  gtk_revealer_set_reveal_child(g_host.find_revealer, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(g_host.find_entry));
}

void toggle_left_sidebar() {
  bool const visible =
      !gtk_widget_get_visible(g_host.left_sidebar);
  gtk_widget_set_visible(g_host.left_sidebar, visible);
  host::save_pref_int("left-sidebar", visible ? 1 : 0);
  if (visible) populate_thumbs();
}

void toggle_ai_sidebar() {
  bool const visible = !gtk_widget_get_visible(g_host.right_sidebar);
  gtk_widget_set_visible(g_host.right_sidebar, visible);
  host::save_pref_int("ai-sidebar", visible ? 1 : 0);
}

void page_step(int direction) {
  if (g_host.epub_active()) {
    g_host.epub->go_to_chapter(g_host.epub->current_page() + direction);
    return;
  }
  auto* tab = g_host.active_pdf();
  if (tab == nullptr) return;
  tab->go_to_page(tab->current_page() + direction);
}

void zoom_in_ui() {
  if (g_host.epub_active()) {  // mac parity: zoom keys scale EPUB type
    g_host.epub_font_scale =
        std::min(2.0, g_host.epub_font_scale * 1.1);
    g_host.epub->apply_font_scale(g_host.epub_font_scale);
    save_reading_font_scale(g_host.epub_font_scale);
    return;
  }
  if (auto* tab = g_host.active_pdf(); tab != nullptr) tab->zoom_in();
  refresh_status();
}

void zoom_out_ui() {
  if (g_host.epub_active()) {
    g_host.epub_font_scale = std::max(0.7, g_host.epub_font_scale / 1.1);
    g_host.epub->apply_font_scale(g_host.epub_font_scale);
    save_reading_font_scale(g_host.epub_font_scale);
    return;
  }
  if (auto* tab = g_host.active_pdf(); tab != nullptr) tab->zoom_out();
  refresh_status();
}

void zoom_reset_ui() {
  if (g_host.epub_active()) {
    g_host.epub_font_scale = 1.0;
    g_host.epub->apply_font_scale(1.0);
    save_reading_font_scale(1.0);
    return;
  }
  if (auto* tab = g_host.active_pdf(); tab != nullptr) tab->zoom_reset();
  refresh_status();
}

void fit_width_ui() {
  if (auto* tab = g_host.active_pdf(); tab != nullptr) {
    tab->fit_width();
    refresh_status();
  }
}

void apply_view_mode_to_tabs() {
  bool const double_mode = g_host.settings.view_mode == "double";
  for (auto& tab : g_host.tabs) tab->set_double_mode(double_mode);
}

void copy_selection_ui() {
  std::string const text = selection_text();
  if (text.empty()) return;
  GdkClipboard* const clipboard =
      gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, text.c_str());
}

// ---------------------------------------------------------------------------
// open dialog + drag & drop
// ---------------------------------------------------------------------------

void pick_and_open() {
  auto* chooser = gtk_file_chooser_native_new(
      Tr("ui.open.dialog").c_str(), g_host.window, GTK_FILE_CHOOSER_ACTION_OPEN,
      Tr("ui.dialog.open").c_str(), Tr("ui.password.cancel").c_str());
  auto* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, Tr("ui.filter.documents").c_str());
  gtk_file_filter_add_mime_type(filter, "application/pdf");
  gtk_file_filter_add_mime_type(filter, "application/epub+zip");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
  hook(
      chooser, "response",
      +[](GtkNativeDialog* native, int response, gpointer) {
        if (response == GTK_RESPONSE_ACCEPT) {
          // The GTK portal returns a list of originals; each opens in its
          // own native tab. There is no import, staging folder or catalog.
          GListModel* files = gtk_file_chooser_get_files(
              GTK_FILE_CHOOSER(native));
          if (files != nullptr) {
            for (guint i = 0; i < g_list_model_get_n_items(files); ++i) {
              auto* file = G_FILE(g_list_model_get_item(files, i));
              if (file == nullptr) continue;
              gchar* path = g_file_get_path(file);
              g_object_unref(file);
              if (path != nullptr) {
                open_path(path);
                g_free(path);
              }
            }
            g_object_unref(files);
          }
        }
        g_object_unref(native);
      },
      nullptr, nullptr, static_cast<GConnectFlags>(0));
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

void on_view_mode_change(GSimpleAction* action, GVariant* value,
                         gpointer) {
  std::string const mode = g_variant_get_string(value, nullptr);
  g_simple_action_set_state(action, value);
  g_host.settings.view_mode = mode;
  apply_view_mode_to_tabs();
}

void on_theme_change(GSimpleAction* action, GVariant* value, gpointer) {
  std::string const theme = g_variant_get_string(value, nullptr);
  g_simple_action_set_state(action, value);
  set_theme(theme);
}

void install_actions(GtkApplication* app) {
  auto add = [&](char const* name, auto&& handler, gpointer user_data,
                 char const* param = nullptr) {
    auto* action =
        param != nullptr ? g_simple_action_new(name, G_VARIANT_TYPE(param))
                         : g_simple_action_new(name, nullptr);
    hook(action, "activate", handler, user_data);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
    g_object_unref(action);
  };
  add("open", +[](GSimpleAction*, GVariant*, gpointer) {
        pick_and_open();
      }, nullptr);
  add("open-path", +[](GSimpleAction*, GVariant* value,
                                   gpointer) {
        if (value != nullptr) {
          open_path(g_variant_get_string(value, nullptr));
        }
      }, nullptr, "s");
  add("open-recent", +[](GSimpleAction*, GVariant* value,
                                     gpointer) {
        if (value == nullptr) return;
        std::string const path = g_variant_get_string(value, nullptr);
        int page = 1;
        for (auto const& recent : g_host.recents) {
          if (recent.path == path) {
            page = static_cast<int>(recent.page);
            break;
          }
        }
        open_path(path, std::max(page, 1));
      }, nullptr, "s");
  add("close-tab", +[](GSimpleAction*, GVariant*, gpointer) {
        close_active_reader();
      }, nullptr);
  add("quit", +[](GSimpleAction*, GVariant*, gpointer) {
        // graceful: destroy the window; shutdown stops the backend
        if (g_host.window != nullptr) gtk_window_close(g_host.window);
      }, nullptr);
  add("zoom-in", +[](GSimpleAction*, GVariant*, gpointer) {
        zoom_in_ui();
      }, nullptr);
  add("zoom-out", +[](GSimpleAction*, GVariant*, gpointer) {
        zoom_out_ui();
      }, nullptr);
  add("zoom-reset", +[](GSimpleAction*, GVariant*, gpointer) {
        zoom_reset_ui();
      }, nullptr);
  add("fit-width", +[](GSimpleAction*, GVariant*, gpointer) {
        fit_width_ui();
      }, nullptr);
  add("find", +[](GSimpleAction*, GVariant*, gpointer) {
        run_find_ui();
      }, nullptr);
  add("find-next", +[](GSimpleAction*, GVariant*, gpointer) {
        find_step(1);
      }, nullptr);
  add("find-previous", +[](GSimpleAction*, GVariant*, gpointer) {
        find_step(-1);
      }, nullptr);
  add("toggle-sidebar", +[](GSimpleAction*, GVariant*, gpointer) {
        toggle_left_sidebar();
      }, nullptr);
  add("toggle-ai", +[](GSimpleAction*, GVariant*, gpointer) {
        toggle_ai_sidebar();
      }, nullptr);
  add("translate-selection",
      +[](GSimpleAction*, GVariant*, gpointer) {
        g_host.ai_panel->do_translate_selection();
      }, nullptr);
  add("translate-page",
      +[](GSimpleAction*, GVariant*, gpointer) {
        g_host.ai_panel->do_translate_page();
      }, nullptr);
  add("copy-selection",
      +[](GSimpleAction*, GVariant*, gpointer) {
        copy_selection_ui();
      }, nullptr);
  add("summarize-page",
      +[](GSimpleAction*, GVariant*, gpointer) {
        g_host.ai_panel->do_summarize_page();
      }, nullptr);
  add("summarize-selection",
      +[](GSimpleAction*, GVariant*, gpointer) {
        g_host.ai_panel->do_summarize_selection();
      }, nullptr);
  add("summarize-doc",
      +[](GSimpleAction*, GVariant*, gpointer) {
        g_host.ai_panel->do_summarize_doc();
      }, nullptr);
  add("page-delete", +[](GSimpleAction*, GVariant*, gpointer) {
        page_delete_current();
      }, nullptr);
  add("page-rotate", +[](GSimpleAction*, GVariant*, gpointer) {
        page_rotate_current();
      }, nullptr);
  add("page-blank", +[](GSimpleAction*, GVariant*, gpointer) {
        page_insert_blank_after();
      }, nullptr);
  add("page-extract", +[](GSimpleAction*, GVariant*, gpointer) {
        page_extract_pick();
      }, nullptr);
  add("page-merge", +[](GSimpleAction*, GVariant*, gpointer) {
        page_merge_pick();
      }, nullptr);
  add("tools-watermark",
      +[](GSimpleAction*, GVariant*, gpointer) {
        tools_watermark_dialog();
      }, nullptr);
  add("tools-export-page",
      +[](GSimpleAction*, GVariant*, gpointer) {
        tools_export_page_image();
      }, nullptr);
  add("tools-save-pdf",
      +[](GSimpleAction*, GVariant*, gpointer) {
        tools_save_pdf();
      }, nullptr);
  add("tools-export-text",
      +[](GSimpleAction*, GVariant*, gpointer) {
        tools_export_text();
      }, nullptr);
  add("epub-chapter", +[](GSimpleAction*, GVariant* value,
                                      gpointer) {
        if (value != nullptr && g_host.epub != nullptr) {
          g_host.epub->go_to_chapter(
              static_cast<int>(g_variant_get_int32(value)));
        }
      }, nullptr, "i");
  add("annotate-highlight",
      +[](GSimpleAction*, GVariant* value, gpointer) {
        if (value != nullptr) {
          annotate_from_selection(g_variant_get_string(value, nullptr),
                                  "highlight");
        }
      }, nullptr, "s");
  add("annotate-underline", +[](GSimpleAction*, GVariant*,
                                            gpointer) {
        annotate_from_selection("blue", "underline");
      }, nullptr);
  add("annotate-strike", +[](GSimpleAction*, GVariant*,
                                         gpointer) {
        annotate_from_selection("yellow", "strike");
      }, nullptr);
  add("bookmark-toggle", +[](GSimpleAction*, GVariant*,
                                         gpointer) {
        toggle_bookmark_ui();
      }, nullptr);
  add("split-toggle", +[](GSimpleAction*, GVariant*,
                                      gpointer) {
        toggle_split_ui();
      }, nullptr);
  add("tts-toggle", +[](GSimpleAction*, GVariant*,
                                    gpointer) {
        toggle_tts_ui();
      }, nullptr);
  add("print", +[](GSimpleAction*, GVariant*, gpointer) {
        print_ui();
      }, nullptr);
  add("check-updates", +[](GSimpleAction*, GVariant*, gpointer) {
        check_updates_manual();
      }, nullptr);
  add("about", +[](GSimpleAction*, GVariant*, gpointer) {
        show_about();
      }, nullptr);

  // stateful: view mode + theme radios
  auto* view_mode = g_simple_action_new_stateful(
      "view-mode", G_VARIANT_TYPE_STRING,
      g_variant_new_string(
          g_host.settings.view_mode == "double" ? "double" : "single"));
  hook(view_mode, "change-state", on_view_mode_change,
                   nullptr);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(view_mode));
  g_object_unref(view_mode);

  auto* theme = g_simple_action_new_stateful(
      "theme", G_VARIANT_TYPE_STRING,
      g_variant_new_string(g_host.theme.c_str()));
  hook(theme, "change-state", on_theme_change,
                   nullptr);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(theme));
  g_object_unref(theme);
}

void install_accels(GtkApplication* app) {
  auto set = [&](char const* action,
                 std::vector<char const*> const& accels) {
    std::vector<char const*> list = accels;
    list.push_back(nullptr);
    gtk_application_set_accels_for_action(app, action, list.data());
  };
  set("app.open", {"<Primary>o"});
  set("app.close-tab", {"<Primary>w"});
  set("app.quit", {"<Primary>q"});
  set("app.zoom-in", {"<Primary>plus", "<Primary>equal"});
  set("app.zoom-out", {"<Primary>minus"});
  set("app.zoom-reset", {"<Primary>0"});
  set("app.find", {"<Primary>f"});
  set("app.find-next", {"F3", "<Primary>g"});
  set("app.find-previous", {"<Shift>F3", "<Shift><Primary>g"});
  set("app.translate-selection", {"<Primary>t"});
  set("app.toggle-sidebar", {"<Primary><Shift>l"});
  set("app.toggle-ai", {"<Primary><Shift>a"});
  set("app.check-updates", {"<Primary><Shift>u"});
  set("app.bookmark-toggle", {"<Primary>b"});
  set("app.print", {"<Primary>p"});
}

// ---------------------------------------------------------------------------
// menus
// ---------------------------------------------------------------------------

void build_primary_menu() {
  auto* menu = g_menu_new();

  // File
  auto* file = g_menu_new();
  g_menu_append(file, Tr("ui.menu.open").c_str(), "app.open");
  auto* recents = g_menu_new();
  int shown = 0;
  for (auto const& recent : g_host.recents) {
    if (shown >= 10) break;
    auto* item = g_menu_item_new(
        path_title(recent.path).c_str(),
        ("app.open-recent::" + recent.path).c_str());
    g_menu_append_item(recents, item);
    g_object_unref(item);
    ++shown;
  }
  if (shown > 0) {
    g_menu_append_submenu(file, Tr("ui.viewer.recents").c_str(),
                          G_MENU_MODEL(recents));
  }
  g_menu_append(file, Tr("ui.menu.close-tab").c_str(), "app.close-tab");
  g_object_unref(recents);
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(file));
  g_object_unref(file);

  // View
  auto* view = g_menu_new();
  g_menu_append(view, Tr("ui.menu.zoom-in").c_str(), "app.zoom-in");
  g_menu_append(view, Tr("ui.menu.zoom-out").c_str(), "app.zoom-out");
  g_menu_append(view, Tr("ui.menu.zoom-reset").c_str(), "app.zoom-reset");
  g_menu_append(view, Tr("ui.viewer.fit").c_str(), "app.fit-width");
  g_menu_append(view, Tr("ui.toolbar.split").c_str(), "app.split-toggle");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(view));
  g_object_unref(view);
  {
    auto* modes = g_menu_new();
    g_menu_append(modes, Tr("ui.settings.view-single").c_str(),
                  "app.view-mode::single");
    g_menu_append(modes, Tr("ui.settings.view-double").c_str(),
                  "app.view-mode::double");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(modes));
    g_object_unref(modes);
  }
  {
    auto* appearance = g_menu_new();
    g_menu_append(appearance, Tr("ui.theme.system").c_str(),
                  "app.theme::system");
    g_menu_append(appearance, Tr("ui.toolbar.theme-light").c_str(),
                  "app.theme::light");
    g_menu_append(appearance, Tr("ui.toolbar.theme-dark").c_str(),
                  "app.theme::dark");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(appearance));
    g_object_unref(appearance);
  }

  // Document (bookmark / speech / print — mac reader menu parity)
  auto* doc = g_menu_new();
  g_menu_append(doc, Tr("ui.menu.bookmark").c_str(), "app.bookmark-toggle");
  g_menu_append(doc, Tr("ui.menu.tts").c_str(), "app.tts-toggle");
  g_menu_append(doc, Tr("ui.menu.print").c_str(), "app.print");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(doc));
  g_object_unref(doc);

  // AI
  auto* ai = g_menu_new();
  g_menu_append(ai, Tr("ui.menu.translate").c_str(),
                "app.translate-selection");
  g_menu_append(ai, Tr("ui.translate.page").c_str(), "app.translate-page");
  g_menu_append(ai, Tr("ui.summarize.page").c_str(), "app.summarize-page");
  g_menu_append(ai, Tr("ui.summarize.selection").c_str(),
                "app.summarize-selection");
  g_menu_append(ai, Tr("ui.summarize.doc").c_str(), "app.summarize-doc");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(ai));
  g_object_unref(ai);

  // Page edit
  auto* pages = g_menu_new();
  g_menu_append(pages, Tr("ui.pages.delete").c_str(), "app.page-delete");
  g_menu_append(pages, Tr("ui.pages.rotate-90").c_str(), "app.page-rotate");
  g_menu_append(pages, Tr("ui.pages.blank").c_str(), "app.page-blank");
  g_menu_append(pages, Tr("ui.pages.extract").c_str(), "app.page-extract");
  g_menu_append(pages, Tr("ui.pages.merge").c_str(), "app.page-merge");
  g_menu_append(pages, Tr("ui.tools.watermark").c_str(), "app.tools-watermark");
  g_menu_append(pages, Tr("ui.tools.export-page").c_str(),
                "app.tools-export-page");
  g_menu_append(pages, Tr("ui.tools.export-text").c_str(),
                "app.tools-export-text");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(pages));
  g_object_unref(pages);

  // Help
  auto* help = g_menu_new();
  g_menu_append(help, Tr("ui.update.check-menu").c_str(),
                "app.check-updates");
  g_menu_append(help, Tr("ui.menu.about").c_str(), "app.about");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(help));
  g_object_unref(help);

  if (g_host.primary_menu != nullptr) {
    gtk_menu_button_set_menu_model(g_host.primary_menu,
                                   G_MENU_MODEL(menu));
  }
  g_object_unref(menu);
}

void build_page_edit_menu(GtkMenuButton* button) {
  auto* menu = g_menu_new();
  auto* section = g_menu_new();
  g_menu_append(section, Tr("ui.pages.delete").c_str(), "app.page-delete");
  g_menu_append(section, Tr("ui.pages.rotate-90").c_str(), "app.page-rotate");
  g_menu_append(section, Tr("ui.pages.blank").c_str(), "app.page-blank");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
  g_object_unref(section);
  auto* section2 = g_menu_new();
  g_menu_append(section2, Tr("ui.pages.extract").c_str(), "app.page-extract");
  g_menu_append(section2, Tr("ui.pages.merge").c_str(), "app.page-merge");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(section2));
  g_object_unref(section2);
  // tools (mac Tools menu parity: watermark, then the two exports)
  auto* tools = g_menu_new();
  g_menu_append(tools, Tr("ui.tools.watermark").c_str(), "app.tools-watermark");
  g_menu_append(tools, (Tr("ui.dialog.save") + " PDF…").c_str(),
                "app.tools-save-pdf");
  g_menu_append(tools, Tr("ui.tools.export-page").c_str(),
                "app.tools-export-page");
  g_menu_append(tools, Tr("ui.tools.export-text").c_str(),
                "app.tools-export-text");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(tools));
  g_object_unref(tools);
  gtk_menu_button_set_menu_model(button, G_MENU_MODEL(menu));
  g_object_unref(menu);
}



}  // namespace

namespace {

// ---------------------------------------------------------------------------
// keyboard navigation
// ---------------------------------------------------------------------------

gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint,
                        GdkModifierType, gpointer) {
  switch (keyval) {
    case GDK_KEY_Page_Down:
    case GDK_KEY_space:
      page_step(1);
      return TRUE;
    case GDK_KEY_Page_Up:
    case GDK_KEY_BackSpace:
      page_step(-1);
      return TRUE;
    case GDK_KEY_Home:
      if (auto* tab = g_host.active_pdf(); tab != nullptr) {
        tab->go_to_page(1);
      }
      return TRUE;
    case GDK_KEY_End:
      if (auto* tab = g_host.active_pdf(); tab != nullptr) {
        tab->go_to_page(tab->document()->page_count());
      }
      return TRUE;
    default:
      return FALSE;
  }
}

// ---------------------------------------------------------------------------
// activate
// ---------------------------------------------------------------------------

void on_activate(GtkApplication* app, gpointer) {
  if (g_host.window != nullptr) {
    gtk_window_present(g_host.window);
    return;
  }

  auto* window = gtk_application_window_new(app);
  g_host.window = GTK_WINDOW(window);
  gtk_window_set_title(g_host.window, "Booklight");
  gtk_window_set_default_size(g_host.window, 1280, 860);
  // Window-manager close and Quit must never bypass unsaved PDF edits.
  // Re-request closing after each successful Save As/explicit Discard,
  // so multiple dirty tabs each receive their own decision.
  hook(g_host.window, "close-request",
       +[](GtkWindow*, gpointer) -> gboolean {
         for (auto const& tab : g_host.tabs) {
           if (tab->pdf_edits_pending) {
             PdfTab* const target = tab.get();
             confirm_close_dirty_pdf(target, [] {
               if (g_host.window != nullptr) gtk_window_close(g_host.window);
             });
             return TRUE;  // defer close until the user makes a safe choice
           }
         }
         return FALSE;
       }, nullptr);
  apply_theme();  // display is connected; gtk_settings are live

  // --- header bar ---
  auto* header = gtk_header_bar_new();
  auto* header_widget = GTK_WIDGET(header);
  g_object_ref(header_widget);

  auto* open_button = gtk_button_new_from_icon_name(
      "document-open-symbolic");
  gtk_widget_set_tooltip_text(open_button, Tr("ui.tooltip.open").c_str());
  hook(open_button, "clicked",
                   +[](GtkButton*, gpointer) { pick_and_open(); },
                   nullptr);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open_button);

  auto* edit_menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(edit_menu_button,
                                "document-edit-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(edit_menu_button),
                              Tr("ui.tooltip.page-edit").c_str());
  build_page_edit_menu(edit_menu_button);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header),
                            GTK_WIDGET(edit_menu_button));
  g_host.header_edit_menu = GTK_WIDGET(edit_menu_button);

  // bookmark toggle (icon mirrors the current page's bookmark state)
  auto* bookmark_button =
      gtk_button_new_from_icon_name("non-starred-symbolic");
  gtk_widget_set_tooltip_text(bookmark_button,
                              Tr("ui.toolbar.bookmark").c_str());
  gtk_actionable_set_action_name(GTK_ACTIONABLE(bookmark_button),
                                 "app.bookmark-toggle");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), bookmark_button);
  g_host.header_bookmark = bookmark_button;

  // read aloud
  auto* tts_button =
      gtk_button_new_from_icon_name("audio-x-generic-symbolic");
  gtk_widget_set_tooltip_text(tts_button, Tr("ui.toolbar.tts").c_str());
  gtk_actionable_set_action_name(GTK_ACTIONABLE(tts_button),
                                 "app.tts-toggle");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), tts_button);
  g_host.header_tts = tts_button;

  // split view toggle
  auto* split_button = gtk_button_new_from_icon_name("view-dual-symbolic");
  gtk_widget_set_tooltip_text(split_button, Tr("ui.toolbar.split").c_str());
  gtk_actionable_set_action_name(GTK_ACTIONABLE(split_button),
                                 "app.split-toggle");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), split_button);
  g_host.header_split = split_button;

  // print
  auto* print_button =
      gtk_button_new_from_icon_name("document-print-symbolic");
  gtk_widget_set_tooltip_text(print_button, Tr("ui.toolbar.print").c_str());
  gtk_actionable_set_action_name(GTK_ACTIONABLE(print_button), "app.print");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), print_button);
  g_host.header_print = print_button;

  auto* toc_button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(toc_button, "view-list-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(toc_button),
                              Tr("ui.viewer.outline").c_str());
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), GTK_WIDGET(toc_button));
  g_host.header_toc_menu = GTK_WIDGET(toc_button);
  gtk_widget_set_visible(g_host.header_toc_menu, FALSE);

  auto* primary_button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(primary_button, "open-menu-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(primary_button),
                              Tr("ui.tooltip.menu").c_str());
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header),
                          GTK_WIDGET(primary_button));
  g_host.primary_menu = primary_button;
  build_primary_menu();

  // File-first: no catalog, cover cache, reading-time timer or bookshelf.
  // The existing Open button and application menu handle document selection.

  auto* title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  // launch state is the shelf; refresh_header_title keeps it in sync
  g_host.title_label =
      GTK_LABEL(gtk_label_new(Tr("ui.library.title").c_str()));
  gtk_widget_add_css_class(GTK_WIDGET(g_host.title_label), "title");
  gtk_label_set_ellipsize(g_host.title_label, PANGO_ELLIPSIZE_END);
  g_host.subtitle_label = GTK_LABEL(gtk_label_new(nullptr));
  gtk_widget_add_css_class(GTK_WIDGET(g_host.subtitle_label), "subtitle");
  gtk_widget_add_css_class(GTK_WIDGET(g_host.subtitle_label), "caption");
  gtk_label_set_ellipsize(g_host.subtitle_label, PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(title_box), GTK_WIDGET(g_host.title_label));
  gtk_box_append(GTK_BOX(title_box), GTK_WIDGET(g_host.subtitle_label));
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_box);

  // --- content ---
  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(g_host.window, root);

  // find bar
  auto* find_revealer = gtk_revealer_new();
  g_host.find_revealer = GTK_REVEALER(find_revealer);
  auto* find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(find_row, 6);
  gtk_widget_set_margin_bottom(find_row, 6);
  gtk_widget_set_margin_start(find_row, 8);
  gtk_widget_set_margin_end(find_row, 8);
  auto* find_entry = gtk_search_entry_new();
  g_host.find_entry = GTK_SEARCH_ENTRY(find_entry);
  gtk_widget_set_hexpand(find_entry, TRUE);
  gtk_box_append(GTK_BOX(find_row), find_entry);
  auto* find_prev = gtk_button_new_from_icon_name(
      "go-up-symbolic");
  gtk_widget_set_tooltip_text(find_prev, Tr("ui.find.previous").c_str());
  gtk_box_append(GTK_BOX(find_row), find_prev);
  auto* find_next = gtk_button_new_from_icon_name(
      "go-down-symbolic");
  gtk_widget_set_tooltip_text(find_next, Tr("ui.find.next").c_str());
  gtk_box_append(GTK_BOX(find_row), find_next);
  g_host.find_count = GTK_LABEL(gtk_label_new(nullptr));
  gtk_box_append(GTK_BOX(find_row), GTK_WIDGET(g_host.find_count));
  auto* find_close = gtk_button_new_from_icon_name(
      "window-close-symbolic");
  gtk_widget_add_css_class(find_close, "flat");
  gtk_box_append(GTK_BOX(find_row), find_close);
  gtk_revealer_set_child(g_host.find_revealer, find_row);
  gtk_box_append(GTK_BOX(root), find_revealer);

  hook(find_entry, "search-changed",
                   +[](GtkSearchEntry*, gpointer) {
                     run_find_now();
                   },
                   nullptr);
  hook(find_entry, "activate",
                   +[](GtkSearchEntry*, gpointer) {
                     find_step(1);
                   },
                   nullptr);
  hook(find_entry, "stop-search",
                   +[](GtkSearchEntry*, gpointer) {
                     close_find();
                   },
                   nullptr);
  hook(find_next, "clicked",
                   +[](GtkButton*, gpointer) { find_step(1); },
                   nullptr);
  hook(find_prev, "clicked",
                   +[](GtkButton*, gpointer) { find_step(-1); },
                   nullptr);
  hook(find_close, "clicked",
                   +[](GtkButton*, gpointer) { close_find(); },
                   nullptr);

  // main stack: shelf | reader (the launch surface IS the shelf)
  auto* main_stack = gtk_stack_new();
  g_host.main_stack = main_stack;
  gtk_stack_set_transition_type(GTK_STACK(main_stack),
                                GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_box_append(GTK_BOX(root), main_stack);

  // Empty-document welcome. Documents stay at their original paths;
  // this screen never imports or copies them into an application library.
  auto* welcome = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_halign(welcome, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(welcome, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(welcome, 32);
  gtk_widget_set_margin_end(welcome, 32);
  auto* welcome_title = gtk_label_new("Booklight");
  gtk_widget_add_css_class(welcome_title, "title-1");
  gtk_box_append(GTK_BOX(welcome), welcome_title);
  auto* welcome_hint = gtk_label_new(Tr("ui.welcome.open-any").c_str());
  gtk_box_append(GTK_BOX(welcome), welcome_hint);
  auto* welcome_open = gtk_button_new_with_label(Tr("ui.menu.open").c_str());
  gtk_widget_add_css_class(welcome_open, "suggested-action");
  gtk_widget_set_halign(welcome_open, GTK_ALIGN_CENTER);
  hook(welcome_open, "clicked",
       +[](GtkButton*, gpointer) { pick_and_open(); }, nullptr);
  gtk_box_append(GTK_BOX(welcome), welcome_open);
  auto* welcome_drop = gtk_label_new(Tr("ui.welcome.linux-hint").c_str());
  gtk_widget_add_css_class(welcome_drop, "dim-label");
  gtk_box_append(GTK_BOX(welcome), welcome_drop);
  auto* recent_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  g_host.welcome_recents = GTK_BOX(recent_box);
  gtk_widget_set_margin_top(recent_box, 18);
  gtk_widget_set_visible(recent_box, FALSE);
  gtk_box_append(GTK_BOX(welcome), recent_box);
  gtk_stack_add_named(GTK_STACK(main_stack), welcome, "shelf");

  // reader page: left sidebar | notebook | AI panel
  auto* reader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  auto* left_sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  g_host.left_sidebar = left_sidebar;
  gtk_widget_set_size_request(left_sidebar, kLeftSidebarWidth, -1);
  auto* left_switcher = gtk_stack_switcher_new();
  auto* left_stack = gtk_stack_new();
  g_host.left_stack = GTK_STACK(left_stack);
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(left_switcher),
                               GTK_STACK(left_stack));
  auto* switcher_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(switcher_wrap, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(switcher_wrap), left_switcher);
  gtk_box_append(GTK_BOX(left_sidebar), switcher_wrap);
  gtk_widget_set_vexpand(left_stack, TRUE);
  gtk_box_append(GTK_BOX(left_sidebar), left_stack);

  auto* thumbs_scroll = gtk_scrolled_window_new();
  auto* thumbs = gtk_flow_box_new();
  g_host.thumbs = GTK_FLOW_BOX(thumbs);
  gtk_flow_box_set_max_children_per_line(g_host.thumbs, 1);
  gtk_flow_box_set_selection_mode(g_host.thumbs, GTK_SELECTION_SINGLE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(thumbs_scroll), thumbs);
  gtk_stack_add_titled(GTK_STACK(left_stack), thumbs_scroll, "thumbs",
                       Tr("ui.viewer.thumbs").c_str());

  auto* outline_scroll = gtk_scrolled_window_new();
  g_host.outline_scroll = GTK_SCROLLED_WINDOW(outline_scroll);
  auto* outline = gtk_list_box_new();
  g_host.outline_list = GTK_LIST_BOX(outline);
  gtk_list_box_set_selection_mode(g_host.outline_list,
                                  GTK_SELECTION_SINGLE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outline_scroll),
                                outline);
  gtk_stack_add_titled(GTK_STACK(left_stack), outline_scroll, "outline",
                       Tr("ui.viewer.outline").c_str());
  hook(
      outline, "row-activated",
      +[](GtkListBox*, GtkListBoxRow* row, gpointer) {
        auto const chapter = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(row), "booklight-epub-chapter"));
        if (g_host.epub_active() && g_host.epub != nullptr && chapter > 0) {
          g_host.epub->go_to_chapter(chapter);
          return;
        }
        auto const page = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(row), "pdfgist-page"));
        if (auto* tab = g_host.active_pdf(); tab != nullptr && page > 0) {
          tab->go_to_page(page);
        }
      },
      nullptr);
  hook(
      thumbs, "child-activated",
      +[](GtkFlowBox*, GtkFlowBoxChild* child, gpointer) {
        auto const page = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(child), "pdfgist-thumb-page"));
        if (auto* tab = g_host.active_pdf(); tab != nullptr && page > 0) {
          tab->go_to_page(page);
        }
      },
      nullptr);

  // notes (annotations + bookmarks) — the sidebar's third mode (mac
  // NotesPane); the header carries the digest export button (insensitive
  // while the document has nothing to export).
  auto* notes_scroll = gtk_scrolled_window_new();
  auto* notes = gtk_list_box_new();
  g_host.notes_list = GTK_LIST_BOX(notes);
  gtk_list_box_set_selection_mode(g_host.notes_list, GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(notes_scroll), notes);
  auto* notes_export = gtk_button_new_with_label(Tr("ui.notes.export").c_str());
  g_host.notes_export_button = notes_export;
  gtk_widget_set_halign(notes_export, GTK_ALIGN_START);
  gtk_widget_set_margin_start(notes_export, 8);
  gtk_widget_set_margin_top(notes_export, 8);
  gtk_widget_set_margin_end(notes_export, 8);
  gtk_widget_set_sensitive(notes_export, FALSE);
  hook(notes_export, "clicked",
       +[](GtkButton*, gpointer) { export_annotations_ui(); },
       nullptr);
  auto* notes_undo_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_host.notes_undo_bar = notes_undo_bar;
  gtk_widget_set_visible(notes_undo_bar, FALSE);
  gtk_widget_set_margin_start(notes_undo_bar, 8);
  gtk_widget_set_margin_end(notes_undo_bar, 8);
  auto* undo_label = gtk_label_new(Tr("ui.notes.deleted").c_str());
  g_host.notes_undo_label = undo_label;
  gtk_widget_set_halign(undo_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(undo_label, "caption");
  gtk_widget_add_css_class(undo_label, "dim-label");
  gtk_box_append(GTK_BOX(notes_undo_bar), undo_label);
  auto* undo_btn = gtk_button_new_with_label(Tr("ui.undo").c_str());
  gtk_widget_add_css_class(undo_btn, "flat");
  gtk_widget_add_css_class(undo_btn, "caption");
  hook(undo_btn, "clicked",
       +[](GtkButton*, gpointer) {
         if (g_host.epub_undo_valid) {
           epub_notes_undo_restore();
         } else {
           notes_undo_restore();
         }
       },
       nullptr);
  gtk_box_append(GTK_BOX(notes_undo_bar), undo_btn);
  auto* notes_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(notes_box), notes_export);
  gtk_box_append(GTK_BOX(notes_box), notes_undo_bar);
  gtk_box_append(GTK_BOX(notes_box), notes_scroll);
  gtk_stack_add_titled(GTK_STACK(left_stack), notes_box, "notes",
                       Tr("ui.tab.notes").c_str());
  hook(notes, "row-activated", on_note_row_activated, nullptr);

  // forms (AcroForm fill + export) — mac FormsPane, sidebar mode
  g_host.forms_pane = std::make_unique<FormsPane>(
      pdfgist::forms::PaneContext{
          [] { return g_host.active_pdf_only(); },
          [](std::string const& message) { host_alert(message); },
          [] {
            if (auto* tab = g_host.active_pdf_only(); tab != nullptr) {
              tab->refresh_rendering();
            }
          }});
  gtk_stack_add_titled(GTK_STACK(left_stack), g_host.forms_pane->widget(),
                       "forms", Tr("ui.tab.forms").c_str());

  // notebook
  auto* notebook = gtk_notebook_new();
  g_host.notebook = GTK_NOTEBOOK(notebook);
  gtk_notebook_set_scrollable(g_host.notebook, TRUE);
  gtk_widget_set_hexpand(notebook, TRUE);

  // AI panel
  g_host.panel_ctx.alert = host_alert;
  g_host.panel_ctx.apply_view_mode = [] {
    apply_view_mode_to_tabs();
    refresh_status();
  };
  g_host.panel_ctx.selection_text = selection_text;
  g_host.panel_ctx.page_text = current_page_text;
  g_host.panel_ctx.doc_text = doc_text;
  g_host.ai_panel = std::make_unique<AiPanel>(&g_host.panel_ctx);
  g_host.right_sidebar = g_host.ai_panel->widget();
  gtk_widget_set_size_request(g_host.right_sidebar, kAiSidebarWidth, -1);
  g_host.ai_panel->on_theme_changed = [](std::string const& theme) {
    set_theme(theme);
    if (auto* action = G_SIMPLE_ACTION(g_action_map_lookup_action(
            G_ACTION_MAP(g_application_get_default()), "theme"));
        action != nullptr) {
      g_simple_action_set_state(action, g_variant_new_string(theme.c_str()));
    }
  };
  g_host.ai_panel->on_language_changed = [](std::string const& language) {
    host::save_pref("ui-language", language);
    update_message_dialog(Tr("ui.settings.language"),
                          Tr("ui.settings.language-note"));
  };
  g_host.ai_panel->on_tts_rate_changed = [](double rate) {
    g_host.tts_rate = std::clamp(rate, 0.5, 3.0);
    pdfgist::tts::set_rate(g_host.tts_rate);
    host::save_pref("tts-rate", std::to_string(g_host.tts_rate));
  };
  g_host.ai_panel->on_reading_theme_changed = [](std::string const& theme) {
    using rivet_app::ReadingTheme;
    save_reading_prefs(theme == "paper"   ? ReadingTheme::paper
                       : theme == "night" ? ReadingTheme::night
                                          : ReadingTheme::system);
  };
  g_host.ai_panel->set_theme(g_host.theme);

  // panes: sidebar | notebook | AI
  auto* inner = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(inner), notebook);
  gtk_paned_set_shrink_start_child(GTK_PANED(inner), FALSE);
  gtk_paned_set_end_child(GTK_PANED(inner), g_host.right_sidebar);
  gtk_widget_set_visible(g_host.right_sidebar, FALSE);
  gtk_paned_set_position(GTK_PANED(inner), 880);
  auto* outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(outer), left_sidebar);
  gtk_paned_set_shrink_start_child(GTK_PANED(outer), FALSE);
  gtk_paned_set_end_child(GTK_PANED(outer), inner);
  gtk_paned_set_position(GTK_PANED(outer), kLeftSidebarWidth);
  gtk_box_append(GTK_BOX(reader), outer);
  gtk_stack_add_named(GTK_STACK(main_stack), reader, "reader");

  // status bar
  auto* status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(status, 4);
  gtk_widget_set_margin_bottom(status, 4);
  gtk_widget_set_margin_start(status, 8);
  gtk_widget_set_margin_end(status, 8);
  g_host.status_label =
      GTK_LABEL(gtk_label_new(Tr("ui.backend-not-ready").c_str()));
  gtk_widget_add_css_class(GTK_WIDGET(g_host.status_label), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(g_host.status_label), "dim-label");
  gtk_widget_set_halign(GTK_WIDGET(g_host.status_label), GTK_ALIGN_START);
  gtk_widget_set_hexpand(GTK_WIDGET(g_host.status_label), TRUE);
  gtk_box_append(GTK_BOX(status), GTK_WIDGET(g_host.status_label));
  g_host.page_label = GTK_LABEL(gtk_label_new(nullptr));
  gtk_widget_add_css_class(GTK_WIDGET(g_host.page_label), "caption");
  gtk_box_append(GTK_BOX(status), GTK_WIDGET(g_host.page_label));
  g_host.zoom_label = GTK_LABEL(gtk_label_new(nullptr));
  gtk_widget_add_css_class(GTK_WIDGET(g_host.zoom_label), "caption");
  gtk_box_append(GTK_BOX(status), GTK_WIDGET(g_host.zoom_label));
  gtk_box_append(GTK_BOX(root), status);

  // sidebar visibility from prefs
  if (host::pref_int("left-sidebar", 1) == 0) {
    gtk_widget_set_visible(left_sidebar, FALSE);
  }
  if (host::pref_int("ai-sidebar", 0) == 0) {
    gtk_widget_set_visible(g_host.right_sidebar, FALSE);
  }

  // notebook events
  hook(
      notebook, "switch-page",
      +[](GtkNotebook*, GtkWidget*, guint, gpointer) {
        close_find();
        refresh_status();
        refresh_header_title();
        populate_thumbs();
        build_outline_for_active();
        populate_notes();
        refresh_forms_pane();
        refresh_bookmark_button();
        update_main_stack();
      },
      nullptr);
  hook(
      notebook, "page-removed",
      +[](GtkNotebook*, GtkWidget*, guint, gpointer) {
        update_main_stack();
      },
      nullptr);

  // drag & drop (mac openDropped parity)
  auto* drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
  hook(
      drop, "drop",
      +[](GtkDropTarget*, GObject* value, double, double,
                     gpointer) -> gboolean {
        if (G_FILE_TYPE_REGULAR == g_file_query_file_type(
                                       G_FILE(value),
                                       G_FILE_QUERY_INFO_NONE, nullptr)) {
          gchar* path = g_file_get_path(G_FILE(value));
          if (path != nullptr) {
            std::string const p = path;
            g_free(path);
            // File-first: the dropped path is the document identity.
            open_path(p);
            return TRUE;
          }
        }
        return FALSE;
      },
      nullptr);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(drop));

  // keyboard navigation
  auto* keys = gtk_event_controller_key_new();
  hook(keys, "key-pressed", on_key_pressed, nullptr);
  gtk_widget_add_controller(window, keys);

  gtk_widget_set_visible(GTK_WIDGET(g_host.header_edit_menu), FALSE);
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
  // The header was built before the window; attach it now.
  gtk_window_set_titlebar(g_host.window, header_widget);
  g_object_unref(header_widget);

  gtk_window_present(g_host.window);
  update_main_stack();
  install_actions(app);
  install_accels(app);
  start_backend();
}

void on_open_files(GtkApplication* app, GFile** files, gint n_files, gpointer) {
  // With G_APPLICATION_HANDLES_OPEN GTK does not emit `activate` first.
  // Build the native tree now, but defer document work until bootstrap has
  // completed the embedded RVT1 handshake.
  if (g_host.window == nullptr) on_activate(app, nullptr);
  for (gint i = 0; i < n_files; ++i) {
    gchar* path = g_file_get_path(files[i]);
    if (path == nullptr) continue;
    std::string const p = path;
    g_free(path);
    if (g_host.ready) {
      open_path(p);
    } else {
      g_host.pending_open_paths.push_back(p);
    }
  }
}

void on_shutdown(GApplication*, gpointer) {
  g_host.shutting_down.store(true, std::memory_order_release);
  stop_update_poll();
  pdfgist::tts::stop();
  if (g_host.startup_thread.joinable()) {
    g_host.startup_thread.join();
  }
  // bootstrap runs against the raw api pointer; join it before the api and
  // backend below are torn down.
  if (g_host.bootstrap_thread.joinable()) {
    g_host.bootstrap_thread.join();
  }
  for (auto& thread : g_host.annotation_load_threads) {
    if (thread.joinable()) thread.join();
  }
  g_host.annotation_load_threads.clear();
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  {
    std::lock_guard lock(g_host.startup_mutex);
    startup_backend = std::move(g_host.startup_backend);
  }
  if (startup_backend != nullptr) startup_backend->stop();
  // Flush pending annotation saves synchronously while backend + api are
  // still alive. An async request followed immediately by backend->stop()
  // can report success to the UI while never reaching the recovery journal.
  if (g_host.api != nullptr && g_host.ready) {
    for (auto& tab : g_host.tabs) {
      if (!tab->annotations_loaded && !tab->annotation_dirty) continue;
      try {
        auto bytes = pdfgist::encode_doc_data(tab->doc_data());
        (void)g_host.api
            ->set_annotations(tab->path(), g_host.settings.annotation_sidecar,
                              std::move(bytes))
            .get();
      } catch (...) {
        // The normal save callback already exposes failure. Shutdown cannot
        // keep UI alive, but the backend stages its recovery draft first.
      }
    }
    if (g_host.epub != nullptr &&
        (g_host.epub->annotations_loaded || g_host.epub->annotation_dirty)) {
      try {
        auto bytes = pdfgist::encode_doc_data(g_host.epub->doc_data());
        (void)g_host.api
            ->set_annotations(g_host.epub->path(),
                              g_host.settings.annotation_sidecar,
                              std::move(bytes))
            .get();
      } catch (...) {
      }
    }
  }
  if (g_host.backend != nullptr) g_host.backend->stop();
  if (g_host.epub != nullptr) {
    g_host.epub->destroy();
    delete g_host.epub;
    g_host.epub = nullptr;
  }
  g_host.tabs.clear();
  g_host.forms_pane.reset();
  g_host.ai_panel.reset();
  // No shelf instance to dispose.
  g_host.api.reset();
  g_host.backend.reset();
}

}  // namespace

// System UI language: the first of LC_ALL / LC_MESSAGES / LANG that names a
// zh or en locale wins; anything else — including set locales for languages
// the catalog doesn't ship — falls back to en. Runs before the window builds
// so first paint is already in the right language; bootstrap mirrors the
// choice into the backend via set-locale.
std::string detect_ui_language() {
  for (char const* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    char const* value = getenv(name);
    if (value == nullptr || *value == '\0') continue;
    std::string locale = value;
    std::transform(locale.begin(), locale.end(), locale.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    if (locale.rfind("zh", 0) == 0) return "zh";
    if (locale.rfind("en", 0) == 0) return "en";
    break;
  }
  return "en";
}

int main(int argc, char** argv) {
  // The in-app override wins over the locale (applies on relaunch, mac
  // parity); "system" keeps the LC_* detection.
  std::string const language_pref = host::pref_string("ui-language", "system");
  pdfgist::SetUiLanguage(language_pref == "zh" || language_pref == "en"
                             ? language_pref
                             : detect_ui_language());
  g_host.theme = host::pref_string("theme", "system");
  g_host.panel_ctx.ui_language = language_pref;
  try {
    g_host.tts_rate = std::stod(host::pref_string("tts-rate", "1.0"));
  } catch (...) {
  }
  g_host.tts_rate = std::clamp(g_host.tts_rate, 0.5, 3.0);
  g_host.panel_ctx.tts_rate = g_host.tts_rate;
  pdfgist::tts::set_rate(g_host.tts_rate);

  auto* app = gtk_application_new("site.jrtx.booklight",
                                  G_APPLICATION_HANDLES_OPEN);
  hook(app, "activate", on_activate, nullptr);
  hook(app, "open", on_open_files, nullptr);
  hook(app, "shutdown", on_shutdown, nullptr);
  int const status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
