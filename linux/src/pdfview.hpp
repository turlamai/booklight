// PDF reading core: document model, continuous scrolling page view,
// selection, find, outline, thumbnails, annotation overlay, split view.
//
// All PDFium calls run on the GTK main thread (PDFium has no per-document
// thread-safety guarantees; the only background work in this host is RPC,
// which never touches a document). The macOS host (PDFKitViews.swift +
// PDFGistModel.swift) is the behavior spec.
#pragma once

#include <cairo/cairo.h>
#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "annotations.hpp"
#include "pdfium.hpp"

namespace pdfgist {

// ----------------------------------------------------------- document ----

class PdfDocument {
 public:
  static std::shared_ptr<PdfDocument> open_file(std::string const& path,
                                                std::string const& password,
                                                unsigned long* error);
  static std::shared_ptr<PdfDocument> open_memory(
      std::vector<std::uint8_t> bytes, unsigned long* error);

  int page_count() const { return page_count_; }
  unsigned long last_error() const { return last_error_; }

  // Page size in points, rotation applied. Cached.
  bool page_size(int index, double* width, double* height);

  // Page text layer (cached — selection, find and the AI actions all read).
  std::shared_ptr<pdfium::TextPage> text_page(int index);

  void* handle() const { return impl_ != nullptr ? impl_->handle() : nullptr; }

  // Document metadata tag ("Title", "Author") as UTF-8; "" when absent.
  // The shelf import reads this for catalog titles.
  std::string metadata(char const* tag) {
    return impl_ != nullptr ? impl_->metadata(tag) : std::string{};
  }

  // Renders one page at `scale` device pixels per point into a fresh
  // ARGB32 cairo surface (viewer + thumbnails + printing; main thread only).
  // With `dark`, painted pixels invert and margins take the dark page
  // background so pages sit flush in dark mode.
  cairo_surface_t* render(int index, double scale, bool dark,
                          int* out_width_px, int* out_height_px) {
    return impl_ != nullptr
               ? impl_->render_page(index, scale, dark, out_width_px,
                                    out_height_px)
               : nullptr;
  }

  // AcroForm surface (see pdfium::Document); empty vector when the document
  // has no fillable fields or the form surface is unavailable.
  std::vector<pdfium::FormFieldInfo> form_fields() {
    return impl_ != nullptr ? impl_->form_fields()
                            : std::vector<pdfium::FormFieldInfo>{};
  }
  bool form_set_text(int page, int annot_index, std::string const& value) {
    return impl_ != nullptr &&
           impl_->form_set_text(page, annot_index, value);
  }
  bool form_set_checked(int page, int annot_index, bool on) {
    return impl_ != nullptr &&
           impl_->form_set_checked(page, annot_index, on);
  }
  bool form_set_choice(int page, int annot_index, std::string const& value) {
    return impl_ != nullptr &&
           impl_->form_set_choice(page, annot_index, value);
  }
  bool form_save(std::string const& path, std::string* error) {
    return impl_ != nullptr && impl_->form_save(path, error);
  }

  // PDFium's FPDF_LoadMemDocument BORROWS the input bytes. An edited
  // document must retain them for its entire lifetime; callers may copy
  // these bytes only when the user explicitly exports the changed PDF.
  std::vector<std::uint8_t> const& memory_bytes() const {
    return memory_bytes_;
  }

 private:
  PdfDocument() = default;
  // Declared before impl_: C++ destroys members in reverse order, so PDFium
  // closes its document handle before the borrowed byte buffer is freed.
  std::vector<std::uint8_t> memory_bytes_;
  std::unique_ptr<pdfium::Document> impl_;
  int page_count_{0};
  unsigned long last_error_{0};
  std::vector<std::pair<double, double>> sizes_;
  std::map<int, std::shared_ptr<pdfium::TextPage>> text_pages_;
};

// -------------------------------------------------------------- tab -----

// Text selection across a char range (possibly spanning pages). Empty
// selection = start == end.
struct PdfSelection {
  int start_page = 0;  // 0-based
  int start_char = 0;
  int end_page = 0;
  int end_char = 0;

  bool empty() const {
    return start_page == end_page && start_char == end_char;
  }
};

// One find hit: page + inclusive char range (rectangles derive from the
// page's text layer at highlight time).
struct FindHit {
  int page = 0;
  int start_char = 0;
  int end_char = 0;
};

class PdfTab {
 public:
  PdfTab(std::shared_ptr<PdfDocument> doc, std::string path,
         std::string title);
  ~PdfTab();

  PdfTab(PdfTab const&) = delete;
  PdfTab& operator=(PdfTab const&) = delete;

  std::string const& path() const { return path_; }
  std::string const& title() const { return title_; }
  std::shared_ptr<PdfDocument> document() const { return doc_; }

  // The container to put into the notebook (the split view lives inside).
  GtkWidget* widget() const { return paned_; }

  int current_page() const { return current_page_; }  // 1-based
  int zoom_percent() const {
    return static_cast<int>(zoom_ * 100.0 + 0.5);
  }
  bool double_mode() const { return double_mode_; }

  // Callbacks owned by main.cpp.
  std::function<void(int page)> on_page_changed;            // 1-based
  std::function<void(bool has_selection)> on_selection_changed;
  std::function<void()> on_annotations_changed;

  void restore_page(int page);  // 1-based resume, before first show
  void go_to_page(int page);    // 1-based
  void set_zoom(double zoom);   // clamped 0.4 .. 4.0
  void zoom_in();
  void zoom_out();
  void zoom_reset();
  void fit_width();
  void set_double_mode(bool on);
  void set_dark(bool on);  // tray background behind pages
  // Reading page surface: "dark" (inverted render + dark tray), "sepia"
  // (normal render + cream tray) or anything else (normal + light tray).
  void set_surface(std::string const& surface);
  void reload_document(std::shared_ptr<PdfDocument> doc);  // page edit
  // Page edits are separate from annotation persistence; the original PDF
  // remains unchanged until an explicit, successful Save As.
  bool pdf_edits_pending{false};

  std::string selection_text();
  void clear_selection();
  std::string page_text(int page);  // 1-based, "" when empty

  // Find: case-insensitive (ASCII fold) whole-document scan; navigation
  // wraps.
  int run_find(std::string const& query);  // returns hit count
  bool find_next();                        // true when a hit was visited
  bool find_previous();
  void clear_find();
  int find_total() const;
  int find_index() const;  // 0-based, -1 = none

  // Thumbnail texture for the sidebar (rendered on demand, cached).
  GdkTexture* thumbnail(int page);  // 1-based; null when not renderable

  // Right-click on an existing selection (pdfview.cpp invokes it).
  std::function<void()> on_context_menu;

  // ------------------------------------------------------ annotations --
  // Stored sidecar data + overlay painting. The document data loads through
  // main.cpp (get-annotations RPC); every mutation notifies
  // on_annotations_changed so the host can persist (debounced) and rebuild
  // the notes pane.

  DocData const& doc_data() const { return doc_data_; }
  void set_doc_data(DocData data);  // replace wholesale (load path)

  // Creates an annotation covering the current selection's first page
  // (mac addAnnotationFromSelection semantics, ids included). Returns the
  // new annotation's id, or "" without a usable selection.
  std::string add_annotation_from_selection(std::string const& color,
                                            std::string const& kind);
  void remove_annotation(std::string const& id);
  // One-step undo (taskly banner parity): take returns the removed
  // annotation (nullopt when the id is absent), restore re-inserts it at
  // its old position; both repaint and mark the tab for persistence.
  std::optional<StoredAnnotation> take_annotation(std::string const& id);
  void restore_annotation(StoredAnnotation const& annotation,
                          std::size_t index);
  std::size_t last_taken_index_{0};

  bool is_bookmarked(int page) const;  // 1-based
  void toggle_bookmark(int page);      // 1-based
  void remove_bookmark(int page);      // 1-based

  // Host-owned debounce timer id for set-annotations (removed on destroy so
  // the callback never fires on a dead tab).
  guint annotation_save_source{0};
  bool annotations_loaded{false};
  bool annotation_dirty{false};
  std::uint64_t annotation_revision{0};
  AnnotationSavePhase annotation_save_phase{AnnotationSavePhase::Saved};
  std::string annotation_save_error;

  // Drops the rendered-page cache and repaints (appearance streams changed
  // underneath us — form fills, page edits).
  void refresh_rendering();

  // ------------------------------------------------------- split view --
  // Second view of the same document (mac SplitViewport): no toolbar state
  // of its own, only follows the main view's scroll ratio. The split tab
  // shares the PdfDocument (render/text caches) but owns its own layout.

  bool split_on() const { return split_tab_ != nullptr; }
  void set_split(bool on);
  PdfTab* split() const { return split_tab_.get(); }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  std::shared_ptr<PdfDocument> doc_;
  std::string path_;
  std::string title_;
  GtkWidget* paned_{nullptr};
  int current_page_{1};
  double zoom_{1.0};
  bool double_mode_{false};
  DocData doc_data_;
  std::unique_ptr<PdfTab> split_tab_;  // owned; never split recursively
};

// --------------------------------------------------------- outline -----

// Flattened outline of a document (empty when the PDF has none).
std::vector<pdfium::OutlineItem> pdf_outline(PdfDocument const& doc);

}  // namespace pdfgist
