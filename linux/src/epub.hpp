// EPUB reading over WebKitGTK: the whole book renders as one continuous
// scroll (the v1 epub.ts model the macOS EpubTab.swift keeps). Business —
// parse/sanitize/text — lives in the Racket backend; this module only
// drives presentation.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "annotations.hpp"

#include "GeneratedBackend.hpp"
#include "panels.hpp"

namespace pdfgist {

class EpubTab {
 public:
  EpubTab(PanelContext* ctx, std::string path, std::string title);
  ~EpubTab();

  EpubTab(EpubTab const&) = delete;
  EpubTab& operator=(EpubTab const&) = delete;

  // Destroys the tab safely: flips the shared alive flag so the detached
  // load worker's dispatched completion becomes a no-op.
  void destroy() { *alive_ = false; }

  GtkWidget* widget() const { return root_; }
  std::string const& path() const { return path_; }
  std::string const& title() const { return title_; }

  int chapters() const { return chapters_; }
  int current_page() const { return current_chapter_; }  // chapter, 1-based
  bool loaded() const { return loaded_; }
  std::vector<rivet_app::EpubTocItem> const& toc() const { return toc_; }

  // A text selection with its anchor: character offsets inside the
  // chapter's plain text, computed by the page script on mouseup.
  struct Selection {
    std::string text;
    int chapter = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
  };

  // Last selection relayed from the web view (mouseup bridge).
  Selection const& pending_selection() const { return selection_; }
  void clear_pending_selection() { selection_ = Selection{}; }

  // Annotations share the PDF store (same JSON shape; page = chapter).
  DocData const& doc_data() const { return doc_data_; }
  void set_doc_data(DocData data);
  // Marks live in the page; these drive them and persist (host saves via
  // the same on_annotations_changed-style hook).
  void add_annotation(StoredAnnotation const& annotation);
  void remove_annotation(std::string const& id);
  std::optional<StoredAnnotation> take_annotation(std::string const& id);
  void restore_annotation(StoredAnnotation const& annotation,
                          std::size_t index);
  std::size_t last_taken_index() const { return last_taken_index_; }
  std::function<void()> on_annotations_changed;
  bool annotations_loaded{false};
  bool annotation_dirty{false};
  std::uint64_t annotation_revision{0};
  AnnotationSavePhase annotation_save_phase{AnnotationSavePhase::Saved};
  std::string annotation_save_error;

  // Chapter text from the prefetch cache ("" until loaded).
  std::string chapter_text(int chapter) const;

  void load();  // async serial fetch of the whole book, then one load

  void go_to_chapter(int chapter);
  void apply_font_scale(double scale);
  void apply_theme(std::string const& theme);  // "light" | "dark"

  // Jump to the nth occurrence of `query` (whole-book search): the chapter
  // scroll plus the in-page <mark.epub-hit> highlight.
  void highlight_hit(int chapter, int ordinal, std::string const& query);

  // Callbacks owned by main.cpp.
  std::function<void(int chapter)> on_chapter_changed;      // 1-based
  std::function<void(std::string const&)> on_load_finished;
  std::function<void(std::string const&)> on_load_failed;
  // Selection-bar action relayed from the page (design/prototype/
  // reading.html .selbar): "hl-yellow" | "hl-blue" | "hl-green" |
  // "underline" | "strike" | "copy" | "ai". The bar is inert chrome —
  // business lives host-side.
  std::function<void(std::string const&)> on_selection_action;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  // Shared lifetime flag: the load worker and its dispatched completion hold
  // a copy and bail out once the tab is destroyed.
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::thread load_thread_;

  PanelContext* ctx_;
  std::string path_;
  std::string title_;
  GtkWidget* root_{nullptr};
  GtkWidget* webview_{nullptr};
  int chapters_{0};
  int current_chapter_{1};
  double font_scale_{1.0};
  bool loaded_{false};
  std::vector<rivet_app::EpubTocItem> toc_;
  Selection selection_;
  DocData doc_data_;
  std::size_t last_taken_index_{0};
  std::map<int, std::string> text_cache_;
};

}  // namespace pdfgist
