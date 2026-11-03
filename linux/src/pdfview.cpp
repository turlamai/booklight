// PDF reading core implementation — see pdfview.hpp.
#include "pdfview.hpp"

#include <gdk/gdk.h>
#include <glib-object.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

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

constexpr double kMinZoom = 0.4;
constexpr double kMaxZoom = 4.0;
constexpr double kMargin = 16.0;  // layout margin, CSS px
constexpr double kGap = 12.0;     // gap between rows, CSS px
constexpr int kThumbWidthPx = 110;
constexpr int kThumbSuperSample = 2;

double clamp_zoom(double zoom) {
  return std::min(kMaxZoom, std::max(kMinZoom, zoom));
}

bool ascii_fold_equal(unsigned short a, unsigned short b) {
  if (a >= 'A' && a <= 'Z') a = static_cast<unsigned short>(a - 'A' + 'a');
  if (b >= 'A' && b <= 'Z') b = static_cast<unsigned short>(b - 'A' + 'a');
  return a == b;
}

}  // namespace

// ----------------------------------------------------------- document ----

std::shared_ptr<PdfDocument> PdfDocument::open_file(
    std::string const& path, std::string const& password,
    unsigned long* error) {
  if (!pdfium::load()) {
    if (error != nullptr) *error = 0;
    return nullptr;
  }
  auto doc = std::shared_ptr<PdfDocument>(new PdfDocument());
  doc->impl_ = pdfium::Document::open_file(path, password);
  if (doc->impl_ == nullptr) return nullptr;
  doc->page_count_ = doc->impl_->page_count();
  doc->last_error_ = doc->impl_->last_error();
  doc->sizes_.assign(static_cast<std::size_t>(doc->page_count_), {0, 0});
  return doc;
}

std::shared_ptr<PdfDocument> PdfDocument::open_memory(
    std::vector<std::uint8_t> bytes, unsigned long* error) {
  if (!pdfium::load()) {
    if (error != nullptr) *error = 0;
    return nullptr;
  }
  auto doc = std::shared_ptr<PdfDocument>(new PdfDocument());
  // Preserve the borrowed backing buffer for the lifetime of PDFium's
  // FPDF_DOCUMENT. A temporary vector would become a dangling pointer.
  doc->memory_bytes_ = std::move(bytes);
  doc->impl_ = pdfium::Document::open_memory(
      doc->memory_bytes_.data(), doc->memory_bytes_.size(), "");
  if (doc->impl_ == nullptr) return nullptr;
  doc->page_count_ = doc->impl_->page_count();
  doc->last_error_ = doc->impl_->last_error();
  doc->sizes_.assign(static_cast<std::size_t>(doc->page_count_), {0, 0});
  return doc;
}

bool PdfDocument::page_size(int index, double* width, double* height) {
  if (index < 0 || index >= page_count_) return false;
  auto& cached = sizes_[static_cast<std::size_t>(index)];
  if (cached.first <= 0 &&
      !impl_->page_size(index, &cached.first, &cached.second)) {
    return false;
  }
  if (width != nullptr) *width = cached.first;
  if (height != nullptr) *height = cached.second;
  return true;
}

std::shared_ptr<pdfium::TextPage> PdfDocument::text_page(int index) {
  auto it = text_pages_.find(index);
  if (it != text_pages_.end()) return it->second;
  auto page = pdfium::TextPage::load(impl_->handle(), index);
  text_pages_[index] = std::move(page);  // cache negatives too
  return page;
}

// --------------------------------------------------------------- impl ----

struct PdfTab::Impl {
  explicit Impl(PdfTab* owner) : tab(owner) {}

  struct Row {
    int first = -1;
    int second = -1;  // -1 = single-page row
    double w = 0;     // points, includes the inter-page gap
    double h = 0;     // points
  };

  PdfTab* tab;
  GtkWidget* draw{nullptr};
  GtkAdjustment* vadj{nullptr};
  GtkAdjustment* hadj{nullptr};

  std::vector<Row> rows;
  std::vector<double> row_tops;  // CSS px from content top (zoom applied)
  double content_width{0};       // CSS px
  double content_height{0};      // CSS px
  bool layout_valid{false};

  void invalidate_layout() {
    layout_valid = false;
    if (draw != nullptr) gtk_widget_queue_resize(draw);
  }

  double page_width(int page) {
    double w = 0, h = 0;
    tab->doc_->page_size(page, &w, &h);
    return w;
  }

  void rebuild_layout() {
    auto const& doc = tab->doc_;
    rows.clear();
    row_tops.clear();
    int const count = doc->page_count();
    if (tab->double_mode_) {
      int i = 0;
      if (count > 0) {  // cover page alone (book spread)
        rows.push_back(Row{0, -1, 0, 0});
        i = 1;
      }
      for (; i < count; i += 2) {
        rows.push_back(
            Row{i, i + 1 < count ? i + 1 : -1, 0, 0});
      }
    } else {
      for (int i = 0; i < count; ++i) rows.push_back(Row{i, -1, 0, 0});
    }
    double max_w = 0;
    double total_h = 0;
    for (auto& row : rows) {
      row.w = page_width(row.first);
      row.h = 0;
      double h1 = 0, h2 = 0;
      doc->page_size(row.first, nullptr, &h1);
      row.h = h1;
      if (row.second >= 0) {
        row.w += kGap + page_width(row.second);
        doc->page_size(row.second, nullptr, &h2);
        row.h = std::max(row.h, h2);
      }
      max_w = std::max(max_w, row.w);
      total_h += row.h;
    }
    row_tops.reserve(rows.size());
    double top = kMargin;
    for (auto const& row : rows) {
      row_tops.push_back(top * tab->zoom_);
      top += row.h + kGap;
    }
    content_width = (max_w + 2 * kMargin) * tab->zoom_;
    content_height =
        (total_h + 2 * kMargin + kGap * std::max<double>(rows.size() - 1, 0)) *
        tab->zoom_;
    layout_valid = true;
  }

  void update_size_request() {
    if (!layout_valid) rebuild_layout();
    gtk_widget_set_size_request(draw,
                                static_cast<int>(std::ceil(content_width)),
                                static_cast<int>(std::ceil(content_height)));
  }

  int row_for_page(int page) const {  // 0-based page → row index
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].first == page || rows[i].second == page) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // Page geometry in CSS px. Returns false when the page is not laid out.
  bool page_rect(int page, double* x, double* y, double* w, double* h) {
    if (!layout_valid) rebuild_layout();
    int const row = row_for_page(page);
    if (row < 0) return false;
    Row const& r = rows[static_cast<std::size_t>(row)];
    double const pw_points = page_width(page);
    double x_points = 0;
    if (r.second < 0) {
      x_points = kMargin + (r.w - pw_points) / 2.0;
    } else {
      double const left_w = page_width(r.first);
      double const right_w = page_width(r.second);
      double const block = left_w + kGap + right_w;
      x_points = kMargin + (r.w - block) / 2.0;
      if (page == r.second) x_points += left_w + kGap;
    }
    if (x != nullptr) *x = x_points * tab->zoom_;
    if (y != nullptr) *y = row_tops[static_cast<std::size_t>(row)];
    if (w != nullptr) *w = pw_points * tab->zoom_;
    if (h != nullptr) {
      double ph = 0;
      tab->doc_->page_size(page, nullptr, &ph);
      *h = ph * tab->zoom_;
    }
    return true;
  }

  // ---- surface cache ----
  struct CacheEntry {
    cairo_surface_t* surface{nullptr};
  };
  // (page, scale, dark) — dark surfaces are a separate cache population so a
  // theme flip re-renders instead of reusing light pixels.
  using CacheKey = std::tuple<int, double, bool>;
  std::map<CacheKey, CacheEntry> cache;
  std::vector<CacheKey> cache_order;
  std::map<int, GdkTexture*> thumbs;

  cairo_surface_t* surface_for(int page) {
    int const sf = std::max(1, gtk_widget_get_scale_factor(draw));
    double const scale = tab->zoom_ * sf;
    auto const cache_key = CacheKey{page, scale, dark_};
    auto it = cache.find(cache_key);
    if (it != cache.end()) return it->second.surface;
    cairo_surface_t* surface =
        tab->doc_->render(page, scale, dark_, nullptr, nullptr);
    if (surface == nullptr) return nullptr;
    cache[cache_key] = CacheEntry{surface};
    cache_order.push_back(cache_key);
    while (cache_order.size() > 24) {
      auto const evict = cache_order.front();
      cache_order.erase(cache_order.begin());
      auto victim = cache.find(evict);
      if (victim != cache.end()) {
        cairo_surface_destroy(victim->second.surface);
        cache.erase(victim);
      }
    }
    return surface;
  }

  void clear_cache() {
    for (auto& [key, entry] : cache) {
      (void)key;
      cairo_surface_destroy(entry.surface);
    }
    cache.clear();
    cache_order.clear();
  }

  // ---- draw ----
  static void draw_func(GtkDrawingArea*, cairo_t* cr, int, int,
                        gpointer user_data) {
    static_cast<Impl*>(user_data)->paint(cr);
  }

  void paint(cairo_t* cr) {
    if (!layout_valid) rebuild_layout();
    // page paper on a neutral tray; PDFs fill their own background. The dark
    // tray matches the dark page background (0x1A, 0x17, 0x14) so pages sit
    // flush (same palette as windows/PdfDocument.cpp); sepia matches its
    // cream canvas (0xEF, 0xE6, 0xD5).
    if (dark_) {
      cairo_set_source_rgb(cr, 0.102, 0.090, 0.078);
    } else if (sepia_) {
      cairo_set_source_rgb(cr, 0.937, 0.902, 0.835);
    } else {
      cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    }
    cairo_paint(cr);

    double const view_y = gtk_adjustment_get_value(vadj);
    double const view_h = gtk_adjustment_get_page_size(vadj);
    double const view_x = gtk_adjustment_get_value(hadj);
    double const view_w = gtk_adjustment_get_page_size(hadj);
    double const zoom = tab->zoom_;

    for (int page = 0; page < tab->doc_->page_count(); ++page) {
      double px = 0, py = 0, pw = 0, ph = 0;
      if (!page_rect(page, &px, &py, &pw, &ph)) continue;
      if (py + ph < view_y || py > view_y + view_h || px + pw < view_x ||
          px > view_x + view_w) {
        continue;
      }
      cairo_surface_t* surface = surface_for(page);
      if (surface == nullptr) continue;
      int const sf = std::max(1, gtk_widget_get_scale_factor(draw));
      cairo_save(cr);
      cairo_translate(cr, px, py);
      cairo_scale(cr, 1.0 / sf, 1.0 / sf);
      cairo_set_source_surface(cr, surface, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);

      // selection overlay — FPDFText_GetRect rects are in PDF user space
      // (y-up): the screen (top-left) y of the upper edge is ph - top.
      auto rects = selection_rects_for(page);
      if (!rects.empty()) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.20, 0.47, 0.85, 0.35);
        for (auto const& r : rects) {
          cairo_rectangle(cr, px + r.left * zoom, py + (ph - r.top) * zoom,
                          (r.right - r.left) * zoom,
                          (r.top - r.bottom) * zoom);
        }
        cairo_fill(cr);
        cairo_restore(cr);
      }
      paint_find(cr, page, px, py, ph, zoom);
      paint_annotations(cr, page, px, py, ph, zoom);
    }
  }

  // Stored annotations of one page, painted in the stored PDF page space
  // (bottom-left origin, y-up): screen y of a stored rect's top edge is
  // ph - (y + height). Underline hugs the rect's bottom edge, strike runs
  // through the vertical middle (mac PDFAnnotation semantics).
  void paint_annotations(cairo_t* cr, int page, double px, double py,
                         double ph, double zoom) {
    for (auto const& annotation : tab->doc_data_.annotations) {
      if (annotation.page != page + 1) continue;
      double r = 0, g = 0, b = 0, a = 1;
      annotation_rgba(annotation.color, &r, &g, &b, &a);
      bool const underline = annotation.kind == "underline";
      bool const strike = annotation.kind == "strike";
      cairo_save(cr);
      cairo_set_source_rgba(cr, r, g, b, a);
      for (auto const& rect : annotation.rects) {
        if (rect.width <= 0 || rect.height <= 0) continue;
        if (underline || strike) {
          double const thickness = std::max(1.2, 0.012 * ph);
          double const y =
              strike ? ph - (rect.y + rect.height / 2.0) - thickness / 2.0
                     : ph - rect.y - thickness;
          cairo_rectangle(cr, px + rect.x * zoom, py + y * zoom,
                          rect.width * zoom, thickness * zoom);
        } else {
          double const y = ph - (rect.y + rect.height);
          cairo_rectangle(cr, px + rect.x * zoom, py + y * zoom,
                          rect.width * zoom, rect.height * zoom);
        }
      }
      cairo_fill(cr);
      cairo_restore(cr);
    }
  }

  void paint_find(cairo_t* cr, int page, double px, double py, double ph,
                  double zoom) {
    for (std::size_t i = 0; i < find_hits.size(); ++i) {
      FindHit const& hit = find_hits[i];
      if (hit.page != page) continue;
      auto const tp = tab->doc_->text_page(page);
      if (tp == nullptr) break;
      auto rects = tp->rects_of_chars(hit.start_char, hit.end_char);
      bool const current =
          find_index_ >= 0 && i == static_cast<std::size_t>(find_index_);
      cairo_save(cr);
      cairo_set_source_rgba(cr, 1.0, 0.83, 0.0, current ? 0.75 : 0.35);
      for (auto const& r : rects) {
        cairo_rectangle(cr, px + r.left * zoom, py + (ph - r.top) * zoom,
                        (r.right - r.left) * zoom,
                        (r.top - r.bottom) * zoom);
      }
      cairo_fill(cr);
      cairo_restore(cr);
    }
  }

  // ---- selection ----
  PdfSelection sel;
  bool selecting{false};
  std::vector<FindHit> find_hits;
  int find_index_{-1};
  bool dark_{false};
  bool sepia_{false};

  std::vector<pdfium::TextRect> selection_rects_for(int page) {
    if (sel.empty()) return {};
    int const lo_page = std::min(sel.start_page, sel.end_page);
    int const hi_page = std::max(sel.start_page, sel.end_page);
    if (page < lo_page || page > hi_page) return {};
    auto tp = tab->doc_->text_page(page);
    if (tp == nullptr) return {};
    int const last = std::max(tp->char_count() - 1, 0);
    bool const forward = sel.start_page < sel.end_page ||
                         (sel.start_page == sel.end_page &&
                          sel.start_char <= sel.end_char);
    int s = forward ? sel.start_char : sel.end_char;
    int e = forward ? sel.end_char : sel.start_char;
    if (page > lo_page) s = 0;
    if (page < hi_page) e = last;
    s = std::clamp(s, 0, last);
    e = std::clamp(e, 0, last);
    if (e < s) std::swap(s, e);
    if (s == e) return {};
    return tp->rects_of_chars(s, e);
  }

  // Hit-tests a widget-space point to (page, char); char = -1 without text.
  int char_at(double x, double y, int* page_out) {
    *page_out = -1;
    if (!layout_valid) rebuild_layout();
    for (int page = 0; page < tab->doc_->page_count(); ++page) {
      double px = 0, py = 0, pw = 0, ph = 0;
      if (!page_rect(page, &px, &py, &pw, &ph)) continue;
      if (x < px || x > px + pw || y < py || y > py + ph) continue;
      *page_out = page;
      double h_pts = 0;
      tab->doc_->page_size(page, nullptr, &h_pts);
      auto const tp = tab->doc_->text_page(page);
      if (tp == nullptr) return -1;
      return tp->char_index_at((x - px) / tab->zoom_,
                               (y - py) / tab->zoom_, h_pts);
    }
    return -1;
  }

  void notify_selection() {
    if (tab->on_selection_changed) tab->on_selection_changed(!sel.empty());
    if (draw != nullptr) gtk_widget_queue_draw(draw);
  }

  void select_word_at(double x, double y) {
    int page = -1;
    int const hit = char_at(x, y, &page);
    auto const tp = page >= 0 ? tab->doc_->text_page(page) : nullptr;
    if (tp == nullptr || hit < 0) {
      sel = PdfSelection{};
      notify_selection();
      return;
    }
    int const last = std::max(tp->char_count() - 1, 0);
    int s = hit;
    while (s > 0 && pdfium::TextPage::is_word_char(tp->unicode_at(s - 1))) --s;
    int e = hit;
    while (e < last &&
           pdfium::TextPage::is_word_char(tp->unicode_at(e + 1))) {
      ++e;
    }
    sel = PdfSelection{page, s, page, e};
    notify_selection();
  }

  // ---- scrolling / tracking ----
  static void on_vadj_changed(GtkAdjustment*, gpointer user_data) {
    static_cast<Impl*>(user_data)->track_current_page();
  }

  double scroll_ratio() const {
    double const span = gtk_adjustment_get_upper(vadj) -
                        gtk_adjustment_get_page_size(vadj);
    if (span <= 0) return 0.0;
    return std::clamp(gtk_adjustment_get_value(vadj) / span, 0.0, 1.0);
  }

  void set_scroll_ratio(double ratio) {
    double const span = gtk_adjustment_get_upper(vadj) -
                        gtk_adjustment_get_page_size(vadj);
    gtk_adjustment_set_value(vadj, std::clamp(ratio, 0.0, 1.0) * span);
  }

  void track_current_page() {
    if (!layout_valid) rebuild_layout();
    if (rows.empty()) return;
    // Split view follows the main view's scroll by ratio (mac SplitViewport).
    if (tab->split_tab_ != nullptr) {
      tab->split_tab_->impl->set_scroll_ratio(scroll_ratio());
    }
    double const y = gtk_adjustment_get_value(vadj) +
                     gtk_adjustment_get_page_size(vadj) * 0.35;
    std::size_t row = 0;
    for (std::size_t i = 0; i < row_tops.size(); ++i) {
      if (row_tops[i] <= y) row = i;
    }
    int const page = rows[row].first + 1;  // 1-based
    if (page != tab->current_page_) {
      tab->current_page_ = page;
      if (tab->on_page_changed) tab->on_page_changed(page);
    }
  }

  // Notify the host that the stored document data changed: repaint here,
  // persist + rebuild the notes pane over there. The split mirrors the data
  // (mac: annotations live on the shared document, both views show them).
  void notify_annotations_changed() {
    if (tab->split_tab_ != nullptr) {
      tab->split_tab_->doc_data_ = tab->doc_data_;
      if (tab->split_tab_->impl->draw != nullptr) {
        gtk_widget_queue_draw(tab->split_tab_->impl->draw);
      }
    }
    if (draw != nullptr) gtk_widget_queue_draw(draw);
    if (tab->on_annotations_changed) tab->on_annotations_changed();
  }

};

// ------------------------------------------------------------ lifecycle --

PdfTab::PdfTab(std::shared_ptr<PdfDocument> doc, std::string path,
               std::string title)
    : impl(std::make_unique<Impl>(this)),
      doc_(std::move(doc)),
      path_(std::move(path)),
      title_(std::move(title)) {
  auto* scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  impl->draw = gtk_drawing_area_new();
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(impl->draw),
                                 Impl::draw_func, impl.get(), nullptr);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), impl->draw);
  impl->vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  impl->hadj =
      gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled));
  hook(impl->vadj, "value-changed",
                   Impl::on_vadj_changed, impl.get());
  paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_wide_handle(GTK_PANED(paned_), TRUE);
  gtk_paned_set_start_child(GTK_PANED(paned_), scrolled);

  // mouse: selection drag, double-click word, right-click context menu
  auto* click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  gtk_widget_add_controller(impl->draw, GTK_EVENT_CONTROLLER(click));
  hook(
      click, "pressed",
      +[](GtkGestureClick* gesture, int n_press, double x,
                     double y, gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        auto const button = gtk_gesture_single_get_current_button(
            GTK_GESTURE_SINGLE(gesture));
        if (button == 3) {
          if (self->tab->on_context_menu) self->tab->on_context_menu();
          return;
        }
        if (button != 1) return;
        if (n_press >= 2) {
          self->selecting = false;
          self->select_word_at(x, y);
          return;
        }
        int page = -1;
        int const ch = self->char_at(x, y, &page);
        if (page < 0) {
          self->sel = PdfSelection{};
          self->notify_selection();
          return;
        }
        self->selecting = true;
        self->sel = PdfSelection{page, std::max(ch, 0), page,
                                 std::max(ch, 0)};
        self->notify_selection();
      },
      impl.get(), nullptr, G_CONNECT_AFTER);
  hook(
      click, "released",
      +[](GtkGestureClick*, int, double, double,
                     gpointer user_data) {
        static_cast<Impl*>(user_data)->selecting = false;
      },
      impl.get(), nullptr, G_CONNECT_AFTER);
  auto* motion = gtk_event_controller_motion_new();
  gtk_widget_add_controller(impl->draw, motion);
  hook(
      motion, "motion",
      +[](GtkEventControllerMotion*, double x, double y,
                     gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self->selecting) return;
        int page = -1;
        int ch = self->char_at(x, y, &page);
        if (page < 0) return;
        self->sel.end_page = page;
        self->sel.end_char = std::max(ch, 0);
        self->notify_selection();
      },
      impl.get(), nullptr, G_CONNECT_AFTER);
}

PdfTab::~PdfTab() {
  // A pending annotation-save debounce would fire on a dangling tab.
  if (annotation_save_source != 0) {
    g_source_remove(annotation_save_source);
    annotation_save_source = 0;
  }
  impl->clear_cache();
  for (auto& [page, texture] : impl->thumbs) {
    (void)page;
    g_object_unref(texture);
  }
  // The split tab dies with its owner; its widgets were children of the
  // paned already destroyed by the GTK hierarchy.
  split_tab_.reset();
}

// --------------------------------------------------------------- zoom ----

void PdfTab::set_zoom(double zoom) {
  zoom_ = clamp_zoom(zoom);
  impl->clear_cache();
  int const anchor = current_page_;
  impl->invalidate_layout();
  impl->update_size_request();
  go_to_page(anchor);
}

void PdfTab::zoom_in() { set_zoom(zoom_ * 1.2); }
void PdfTab::zoom_out() { set_zoom(zoom_ / 1.2); }
void PdfTab::zoom_reset() { set_zoom(1.0); }

void PdfTab::fit_width() {
  int const row = impl->row_for_page(current_page_ - 1);
  if (row < 0) return;
  double const view_w = gtk_adjustment_get_page_size(impl->hadj);
  if (view_w <= 2 * kMargin) return;
  set_zoom((view_w - 2 * kMargin) /
           impl->rows[static_cast<std::size_t>(row)].w);
}

void PdfTab::set_double_mode(bool on) {
  double_mode_ = on;
  impl->clear_cache();
  impl->invalidate_layout();
  impl->update_size_request();
  go_to_page(current_page_);
  if (split_tab_ != nullptr) split_tab_->set_double_mode(on);
}

void PdfTab::set_surface(std::string const& surface) {
  bool const dark = surface == "dark";
  bool const sepia = surface == "sepia";
  if (impl->dark_ == dark && impl->sepia_ == sepia) return;
  bool const dark_changed = impl->dark_ != dark;
  impl->dark_ = dark;
  impl->sepia_ = sepia;
  // Only a dark flip changes page rendering; sepia just re-tints the tray.
  if (dark_changed) {
    // Cached page surfaces are keyed with dark, but stale entries would
    // still hold memory and the thumbnail cache is keyed by page only —
    // drop both.
    impl->clear_cache();
    for (auto& [page, texture] : impl->thumbs) {
      (void)page;
      g_object_unref(texture);
    }
    impl->thumbs.clear();
  }
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
  if (split_tab_ != nullptr) split_tab_->set_surface(surface);
}

void PdfTab::set_dark(bool on) {
  if (impl->dark_ == on) return;
  impl->dark_ = on;
  // Cached page surfaces are keyed with dark, but stale entries would still
  // hold memory and the thumbnail cache is keyed by page only — drop both.
  impl->clear_cache();
  for (auto& [page, texture] : impl->thumbs) {
    (void)page;
    g_object_unref(texture);
  }
  impl->thumbs.clear();
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
  if (split_tab_ != nullptr) split_tab_->set_dark(on);
}

void PdfTab::go_to_page(int page) {
  int const count = std::max(doc_->page_count(), 1);
  page = std::clamp(page, 1, count);
  current_page_ = page;
  if (!impl->layout_valid) {
    impl->rebuild_layout();
    impl->update_size_request();
  }
  int const row = impl->row_for_page(page - 1);
  if (row >= 0) {
    double const top = impl->row_tops[static_cast<std::size_t>(row)];
    gtk_adjustment_set_value(impl->vadj,
                             std::max(0.0, top - kMargin * 0.5));
  }
  if (on_page_changed) on_page_changed(current_page_);
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
}

void PdfTab::restore_page(int page) {
  int const count = std::max(doc_->page_count(), 1);
  current_page_ = std::clamp(page, 1, count);
}

// ------------------------------------------------------------- content ----

std::string PdfTab::selection_text() {
  if (impl->sel.empty()) return {};
  int const lo = std::min(impl->sel.start_page, impl->sel.end_page);
  int const hi = std::max(impl->sel.start_page, impl->sel.end_page);
  std::string out;
  for (int page = lo; page <= hi; ++page) {
    auto const tp = doc_->text_page(page);
    if (tp == nullptr) continue;
    auto const rects = impl->selection_rects_for(page);
    (void)rects;  // bounds came from selection_rects_for clamping above
    int const last = std::max(tp->char_count() - 1, 0);
    bool const forward = impl->sel.start_page < impl->sel.end_page ||
                         (impl->sel.start_page == impl->sel.end_page &&
                          impl->sel.start_char <= impl->sel.end_char);
    int s = forward ? impl->sel.start_char : impl->sel.end_char;
    int e = forward ? impl->sel.end_char : impl->sel.start_char;
    if (page > lo) s = 0;
    if (page < hi) e = last;
    s = std::clamp(s, 0, last);
    e = std::clamp(e, 0, last);
    if (e < s) continue;
    if (!out.empty()) out += "\n";
    out += tp->text_of_chars(s, e);
  }
  return out;
}

void PdfTab::clear_selection() {
  impl->sel = PdfSelection{};
  impl->notify_selection();
}

std::string PdfTab::page_text(int page) {
  auto const tp = doc_->text_page(page - 1);
  if (tp == nullptr) return {};
  return tp->text();
}

void PdfTab::reload_document(std::shared_ptr<PdfDocument> doc) {
  // Only edited in-memory documents create unsaved page changes.
  // Resetting this flag belongs to a *successful* explicit export.
  pdf_edits_pending = doc != nullptr && !doc->memory_bytes().empty();
  doc_ = std::move(doc);
  impl->sel = PdfSelection{};
  impl->find_hits.clear();
  impl->find_index_ = -1;
  impl->clear_cache();
  impl->invalidate_layout();
  impl->update_size_request();
  go_to_page(1);
  // The split shares the document — reload it too (mac reloadTabDocument
  // sets splitPdfView.document).
  if (split_tab_ != nullptr) {
    split_tab_->doc_ = doc_;
    split_tab_->impl->sel = PdfSelection{};
    split_tab_->impl->find_hits.clear();
    split_tab_->impl->find_index_ = -1;
    split_tab_->impl->clear_cache();
    split_tab_->impl->invalidate_layout();
    split_tab_->impl->update_size_request();
    split_tab_->go_to_page(current_page_);
  }
}

// ---------------------------------------------------------------- find ----

int PdfTab::run_find(std::string const& query) {
  impl->find_hits.clear();
  impl->find_index_ = -1;
  if (!query.empty()) {
    auto const needle = pdfium::utf8_to_utf16(query);
    if (!needle.empty()) {
      for (int page = 0; page < doc_->page_count(); ++page) {
        auto const tp = doc_->text_page(page);
        if (tp == nullptr) continue;
        auto const corpus = tp->text_utf16();
        if (corpus.size() < needle.size()) continue;
        for (std::size_t i = 0; i + needle.size() <= corpus.size(); ++i) {
          bool match = true;
          for (std::size_t k = 0; k < needle.size(); ++k) {
            if (!ascii_fold_equal(corpus[i + k], needle[k])) {
              match = false;
              break;
            }
          }
          if (!match) continue;
          int const cs = tp->char_from_text_index(static_cast<int>(i));
          int const ce = tp->char_from_text_index(
              static_cast<int>(i + needle.size() - 1));
          if (cs < 0 || ce < 0 || ce < cs) continue;
          impl->find_hits.push_back(FindHit{page, cs, ce});
          i += needle.size() - 1;
        }
      }
    }
  }
  if (!impl->find_hits.empty()) {
    // first hit at/after the current page, else the first
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < impl->find_hits.size(); ++i) {
      if (impl->find_hits[i].page >= current_page_ - 1) {
        chosen = i;
        break;
      }
    }
    impl->find_index_ = static_cast<int>(chosen);
    go_to_page(impl->find_hits[chosen].page + 1);
  }
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
  return static_cast<int>(impl->find_hits.size());
}

bool PdfTab::find_next() {
  if (impl->find_hits.empty()) return false;
  impl->find_index_ =
      (impl->find_index_ + 1) % static_cast<int>(impl->find_hits.size());
  go_to_page(
      impl->find_hits[static_cast<std::size_t>(impl->find_index_)].page + 1);
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
  return true;
}

bool PdfTab::find_previous() {
  if (impl->find_hits.empty()) return false;
  impl->find_index_ =
      (impl->find_index_ - 1 + static_cast<int>(impl->find_hits.size())) %
      static_cast<int>(impl->find_hits.size());
  go_to_page(
      impl->find_hits[static_cast<std::size_t>(impl->find_index_)].page + 1);
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
  return true;
}

void PdfTab::clear_find() {
  impl->find_hits.clear();
  impl->find_index_ = -1;
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
}

int PdfTab::find_total() const {
  return static_cast<int>(impl->find_hits.size());
}

int PdfTab::find_index() const { return impl->find_index_; }

// ------------------------------------------------------- annotations ----

void PdfTab::set_doc_data(DocData data) {
  doc_data_ = std::move(data);
  impl->notify_annotations_changed();
}

std::string PdfTab::add_annotation_from_selection(std::string const& color,
                                                  std::string const& kind) {
  if (impl->sel.empty()) return {};
  // Text must be non-whitespace (mac trims and rejects empties).
  std::string text = selection_text();
  while (!text.empty() && (text.front() == ' ' || text.front() == '\n' ||
                           text.front() == '\r' || text.front() == '\t')) {
    text.erase(text.begin());
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
                           text.back() == '\r' || text.back() == '\t')) {
    text.pop_back();
  }
  if (text.empty()) return {};

  // One annotation on the first page of the selection (mac semantics): line
  // rects come from the same per-char machinery as the live selection.
  bool const forward = impl->sel.start_page < impl->sel.end_page ||
                       (impl->sel.start_page == impl->sel.end_page &&
                        impl->sel.start_char <= impl->sel.end_char);
  int const page = forward ? impl->sel.start_page : impl->sel.end_page;
  auto rects = impl->selection_rects_for(page);
  if (rects.empty()) return {};

  StoredAnnotation annotation;
  annotation.id = make_annotation_id();
  annotation.page = page + 1;
  for (auto const& r : rects) {
    // Stored space is PDF user space (bottom-left origin, y-up) — the same
    // space FPDFText_GetRect reports; no flip happens here, only at paint.
    if (r.right - r.left <= 0 || r.top - r.bottom <= 0) continue;
    annotation.rects.push_back(
        anno_rect_from_text_rect(r.left, r.top, r.right, r.bottom));
  }
  if (annotation.rects.empty()) return {};
  if (text.size() > 800) {
    // UTF-8 safe cut at a code point boundary.
    std::size_t end = 800;
    while (end > 0 &&
           (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
      --end;
    }
    text = text.substr(0, end);
  }
  annotation.excerpt = text;
  annotation.color = color;
  annotation.kind = kind;
  annotation.created = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now()
                               .time_since_epoch())
                           .count();
  std::string const id = annotation.id;
  doc_data_.annotations.push_back(std::move(annotation));
  impl->notify_annotations_changed();
  return id;
}

void PdfTab::remove_annotation(std::string const& id) {
  auto& annotations = doc_data_.annotations;
  annotations.erase(
      std::remove_if(annotations.begin(), annotations.end(),
                     [&](StoredAnnotation const& a) { return a.id == id; }),
      annotations.end());
  impl->notify_annotations_changed();
}

std::optional<StoredAnnotation> PdfTab::take_annotation(
    std::string const& id) {
  auto& annotations = doc_data_.annotations;
  auto const it = std::find_if(annotations.begin(), annotations.end(),
                               [&id](StoredAnnotation const& a) {
                                 return a.id == id;
                               });
  if (it == annotations.end()) return std::nullopt;
  StoredAnnotation taken = *it;
  auto const index = static_cast<std::size_t>(it - annotations.begin());
  annotations.erase(it);
  impl->notify_annotations_changed();
  last_taken_index_ = index;
  return taken;
}

void PdfTab::restore_annotation(StoredAnnotation const& annotation,
                                std::size_t index) {
  auto const at = std::min(index, doc_data_.annotations.size());
  doc_data_.annotations.insert(
      doc_data_.annotations.begin() + static_cast<std::ptrdiff_t>(at),
      annotation);
  impl->notify_annotations_changed();
}

bool PdfTab::is_bookmarked(int page) const {
  for (auto const& bookmark : doc_data_.bookmarks) {
    if (bookmark.page == page) return true;
  }
  return false;
}

void PdfTab::toggle_bookmark(int page) {
  auto& bookmarks = doc_data_.bookmarks;
  auto it = std::find_if(bookmarks.begin(), bookmarks.end(),
                         [&](StoredBookmark const& b) { return b.page == page; });
  if (it != bookmarks.end()) {
    bookmarks.erase(it);
  } else {
    StoredBookmark bookmark;
    bookmark.page = page;
    bookmark.label = Trf("ui.notes.page", std::to_string(page));
    bookmark.created = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now()
                               .time_since_epoch())
                           .count();
    bookmarks.push_back(std::move(bookmark));
  }
  std::sort(bookmarks.begin(), bookmarks.end(),
            [](StoredBookmark const& a, StoredBookmark const& b) {
              return a.page != b.page ? a.page < b.page
                                      : a.created < b.created;
            });
  impl->notify_annotations_changed();
}

void PdfTab::remove_bookmark(int page) {
  auto& bookmarks = doc_data_.bookmarks;
  bookmarks.erase(
      std::remove_if(bookmarks.begin(), bookmarks.end(),
                     [&](StoredBookmark const& b) { return b.page == page; }),
      bookmarks.end());
  impl->notify_annotations_changed();
}

void PdfTab::refresh_rendering() {
  impl->clear_cache();
  if (impl->draw != nullptr) gtk_widget_queue_draw(impl->draw);
  if (split_tab_ != nullptr) split_tab_->refresh_rendering();
}

// ------------------------------------------------------------ split ----

void PdfTab::set_split(bool on) {
  if (on == (split_tab_ != nullptr)) return;
  if (on) {
    split_tab_ = std::make_unique<PdfTab>(doc_, path_, title_);
    split_tab_->impl->dark_ = impl->dark_;
    split_tab_->set_double_mode(double_mode_);
    split_tab_->restore_page(current_page_);
    split_tab_->go_to_page(current_page_);
    gtk_paned_set_end_child(GTK_PANED(paned_), split_tab_->widget());
    // Half/half once the paned has a real allocation (an early set_position
    // gets clamped against the not-yet-known width).
    struct Once {
      GtkPaned* paned;
      int tries;
    };
    auto* once = new Once{GTK_PANED(paned_), 0};
    (void)g_timeout_add(
        30,
        +[](gpointer user_data) -> int {
          auto* ctx = static_cast<Once*>(user_data);
          if (!gtk_widget_get_mapped(GTK_WIDGET(ctx->paned)) &&
              ++ctx->tries < 100) {
            return G_SOURCE_CONTINUE;
          }
          int const width = gtk_widget_get_width(GTK_WIDGET(ctx->paned));
          if (width > 0) gtk_paned_set_position(ctx->paned, width / 2);
          delete ctx;
          return G_SOURCE_REMOVE;
        },
        once);
    split_tab_->impl->set_scroll_ratio(impl->scroll_ratio());
  } else {
    auto dying = std::move(split_tab_);
    gtk_paned_set_end_child(GTK_PANED(paned_), nullptr);  // destroys widgets
    dying.reset();
  }
}

// ---------------------------------------------------------- thumbnails ----

GdkTexture* PdfTab::thumbnail(int page) {
  int const index = page - 1;
  if (index < 0 || index >= doc_->page_count()) return nullptr;
  auto it = impl->thumbs.find(index);
  if (it != impl->thumbs.end()) return it->second;

  double w_pts = 0;
  if (!doc_->page_size(index, &w_pts, nullptr) || w_pts <= 0) return nullptr;
  cairo_surface_t* surface =
      doc_->render(index, kThumbWidthPx * kThumbSuperSample / w_pts,
                   impl->dark_, nullptr, nullptr);
  if (surface == nullptr) return nullptr;
  int const px_w = cairo_image_surface_get_width(surface);
  int const px_h = cairo_image_surface_get_height(surface);
  int const stride = cairo_image_surface_get_stride(surface);
  gsize const size = static_cast<gsize>(stride) * static_cast<gsize>(px_h);
  GBytes* bytes = g_bytes_new_with_free_func(
      cairo_image_surface_get_data(surface), size,
      +[](gpointer data) {
        cairo_surface_destroy(static_cast<cairo_surface_t*>(data));
      },
      surface);
  GdkTexture* texture = gdk_memory_texture_new(
      px_w, px_h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes,
      static_cast<gsize>(stride));
  g_bytes_unref(bytes);
  impl->thumbs[index] = texture;
  return texture;
}

// ------------------------------------------------------------- outline ----

std::vector<pdfium::OutlineItem> pdf_outline(PdfDocument const& doc) {
  return pdfium::outline(doc.handle());
}

}  // namespace pdfgist
