#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

namespace {

// Same values as the mac Theme.annotationColor mapping.
wu::Color AnnotationColor(std::wstring const& name) {
  if (name == L"green") return wu::Color{0xD9, 0x4C, 0xC3, 0x83};
  if (name == L"blue") return wu::Color{0xD9, 0x56, 0x9C, 0xFF};
  return wu::Color{0xE6, 0xFF, 0xD5, 0x00};  // yellow
}

media::SolidColorBrush Solid(wu::Color color) {
  media::SolidColorBrush brush;
  brush.Color(color);
  return brush;
}

// Rects are stored in PDF page space (bottom-left origin); the view works in
// top-left points. y_stored = page_height - y_pdfium - height; flip back here.
std::array<double, 4> to_top_left_rect(AnnoRectD const& rect,
                                       double page_height) {
  double const top = page_height - rect.y - rect.height;
  return std::array<double, 4>{rect.x, top, rect.x + rect.width,
                               top + rect.height};
}

AnnoRectD to_stored_rect(std::array<double, 4> const& rect,
                         double page_height) {
  // rect = {left, top, right, bottom} in top-left page points
  AnnoRectD stored;
  stored.x = rect[0];
  stored.width = std::max(0.0, rect[2] - rect[0]);
  stored.height = std::max(0.0, rect[3] - rect[1]);
  stored.y = page_height - rect[3];
  return stored;
}

std::wstring json_escape(std::wstring const& text) {
  std::wstring out;
  out.reserve(text.size() + 8);
  for (wchar_t const c : text) {
    switch (c) {
      case L'"':
        out += L"\\\"";
        break;
      case L'\\':
        out += L"\\\\";
        break;
      case L'\n':
        out += L"\\n";
        break;
      case L'\r':
        out += L"\\r";
        break;
      case L'\t':
        out += L"\\t";
        break;
      default:
        if (static_cast<std::uint32_t>(c) < 0x20) {
          wchar_t buffer[8];
          std::swprintf(buffer, 8, L"\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string json_number(double value) {
  if (!std::isfinite(value)) return "0";
  char buffer[40];
  // Page-space points: four decimals are far below visual precision.
  std::snprintf(buffer, sizeof buffer, "%.4f", value);
  std::string text(buffer);
  // Trim trailing zeros (keep one decimal digit for float-typed fields).
  auto const dot = text.find('.');
  if (dot != std::string::npos) {
    std::size_t last = text.size();
    while (last > dot + 2 && text[last - 1] == '0') --last;
    text.resize(last);
  }
  return text;
}

void json_append_string(std::string* out, std::wstring const& value) {
  std::wstring const escaped = json_escape(value);
  int const size = ::WideCharToMultiByte(CP_UTF8, 0, escaped.data(),
                                         static_cast<int>(escaped.size()),
                                         nullptr, 0, nullptr, nullptr);
  *out += '"';
  if (size > 0) {
    std::string narrow(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, escaped.data(),
                          static_cast<int>(escaped.size()), narrow.data(),
                          size, nullptr, nullptr);
    *out += narrow;
  }
  *out += '"';
}

// ---- minimal JSON reader (schema-tolerant: unknown keys are ignored) ----

struct JsonCursor {
  std::string const& text;
  std::size_t at = 0;

  void skip_ws() {
    while (at < text.size()) {
      char const c = text[at];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++at;
      } else {
        break;
      }
    }
  }

  bool eat(char c) {
    skip_ws();
    if (at < text.size() && text[at] == c) {
      ++at;
      return true;
    }
    return false;
  }

  char peek() {
    skip_ws();
    return at < text.size() ? text[at] : '\0';
  }

  bool parse_string(std::wstring* out) {
    if (!eat('"')) return false;
    std::wstring value;
    std::string pending;  // raw utf8 bytes between escapes
    auto flush = [&]() {
      if (!pending.empty()) {
        int const size = ::MultiByteToWideChar(
            CP_UTF8, 0, pending.data(), static_cast<int>(pending.size()),
            nullptr, 0);
        if (size > 0) {
          std::wstring wide(static_cast<std::size_t>(size), L'\0');
          ::MultiByteToWideChar(CP_UTF8, 0, pending.data(),
                                static_cast<int>(pending.size()), wide.data(),
                                size);
          value += wide;
        }
        pending.clear();
      }
    };
    while (at < text.size()) {
      char const c = text[at++];
      if (c == '"') {
        flush();
        *out = value;
        return true;
      }
      if (c == '\\') {
        if (at >= text.size()) break;
        char const esc = text[at++];
        switch (esc) {
          case '"':
            pending += '"';
            break;
          case '\\':
            pending += '\\';
            break;
          case '/':
            pending += '/';
            break;
          case 'n':
            flush();
            value += L'\n';
            break;
          case 'r':
            flush();
            value += L'\r';
            break;
          case 't':
            flush();
            value += L'\t';
            break;
          case 'b':
            flush();
            value += L'\b';
            break;
          case 'f':
            flush();
            value += L'\f';
            break;
          case 'u': {
            if (at + 4 > text.size()) break;
            unsigned int code = 0;
            for (int i = 0; i < 4; ++i) {
              char const hex = text[at++];
              code <<= 4;
              if (hex >= '0' && hex <= '9') {
                code |= static_cast<unsigned int>(hex - '0');
              } else if (hex >= 'a' && hex <= 'f') {
                code |= static_cast<unsigned int>(hex - 'a' + 10);
              } else if (hex >= 'A' && hex <= 'F') {
                code |= static_cast<unsigned int>(hex - 'A' + 10);
              }
            }
            flush();
            value += static_cast<wchar_t>(code);
            break;
          }
          default:
            break;
        }
        continue;
      }
      pending += c;
    }
    return false;
  }

  bool parse_number(double* out) {
    skip_ws();
    char* end = nullptr;
    double const value = std::strtod(text.c_str() + at, &end);
    if (end == text.c_str() + at) return false;
    at = static_cast<std::size_t>(end - text.c_str());
    *out = value;
    return true;
  }

  bool skip_value() {
    char const c = peek();
    if (c == '"') {
      std::wstring discard;
      return parse_string(&discard);
    }
    if (c == '{' || c == '[') {
      std::vector<int> stack;
      while (at < text.size()) {
        char const scan = text[at++];
        if (scan == '"') {
          // skip string body
          while (at < text.size() && text[at] != '"') {
            if (text[at] == '\\' && at + 1 < text.size()) ++at;
            ++at;
          }
          ++at;
          continue;
        }
        if (scan == '{' || scan == '[') {
          stack.push_back(scan);
        } else if (scan == '}' || scan == ']') {
          if (stack.empty()) break;
          stack.pop_back();
          if (stack.empty()) return true;
        }
      }
      return false;
    }
    double discard;
    return parse_number(&discard);
  }
};

}  // namespace

// ---- serialization ------------------------------------------------------------

std::string MainWindow::SerializeDocData(DocData const& data) {
  std::string out;
  out += "{\"annotations\":[";
  bool first_annotation = true;
  for (auto const& annotation : data.annotations) {
    if (!first_annotation) out += ',';
    first_annotation = false;
    out += "{\"id\":";
    json_append_string(&out, annotation.id);
    out += ",\"page\":" + std::to_string(annotation.page);
    out += ",\"rects\":[";
    bool first_rect = true;
    for (auto const& rect : annotation.rects) {
      if (!first_rect) out += ',';
      first_rect = false;
      out += "{\"x\":" + json_number(rect.x);
      out += ",\"y\":" + json_number(rect.y);
      out += ",\"width\":" + json_number(rect.width);
      out += ",\"height\":" + json_number(rect.height);
      out += '}';
    }
    out += "],\"excerpt\":";
    json_append_string(&out, annotation.excerpt);
    out += ",\"color\":";
    json_append_string(&out, annotation.color);
    out += ",\"kind\":";
    json_append_string(&out, annotation.kind);
    out += ",\"note\":";
    json_append_string(&out, annotation.note);
    out += ",\"created\":" + std::to_string(annotation.created);
    if (annotation.anchor) {
      out += ",\"anchor\":{\"chapter\":" + std::to_string(annotation.anchor->chapter);
      out += ",\"start\":" + std::to_string(annotation.anchor->start);
      out += ",\"end\":" + std::to_string(annotation.anchor->end);
      out += '}';
    }
    out += '}';
  }
  out += "],\"bookmarks\":[";
  bool first_bookmark = true;
  for (auto const& bookmark : data.bookmarks) {
    if (!first_bookmark) out += ',';
    first_bookmark = false;
    out += "{\"page\":" + std::to_string(bookmark.page);
    out += ",\"label\":";
    json_append_string(&out, bookmark.label);
    out += ",\"created\":" + std::to_string(bookmark.created);
    out += '}';
  }
  out += "]}";
  return out;
}

bool MainWindow::ParseDocData(std::string const& json, DocData* out) {
  DocData parsed;
  JsonCursor cursor{json};
  if (!cursor.eat('{')) return false;
  bool saw_annotations = false;
  bool saw_bookmarks = false;
  char key_hint = cursor.peek();
  while (key_hint != '\0' && key_hint != '}') {
    std::wstring key;
    if (!cursor.parse_string(&key)) return false;
    if (!cursor.eat(':')) return false;
    if (key == L"annotations") {
      saw_annotations = true;
      if (!cursor.eat('[')) return false;
      while (cursor.peek() == '{') {
        StoredAnnotation annotation;
        if (!cursor.eat('{')) return false;
        char field = cursor.peek();
        while (field != '\0' && field != '}') {
          std::wstring field_name;
          if (!cursor.parse_string(&field_name)) return false;
          if (!cursor.eat(':')) return false;
          if (field_name == L"id") {
            cursor.parse_string(&annotation.id);
          } else if (field_name == L"page") {
            double number = 1;
            cursor.parse_number(&number);
            annotation.page = std::max(1, static_cast<int>(number));
          } else if (field_name == L"excerpt") {
            cursor.parse_string(&annotation.excerpt);
          } else if (field_name == L"color") {
            cursor.parse_string(&annotation.color);
          } else if (field_name == L"kind") {
            cursor.parse_string(&annotation.kind);
          } else if (field_name == L"note") {
            cursor.parse_string(&annotation.note);
          } else if (field_name == L"created") {
            double number = 0;
            cursor.parse_number(&number);
            annotation.created = static_cast<std::int64_t>(number);
          } else if (field_name == L"anchor" && cursor.peek() == '{') {
            cursor.eat('{');
            StoredAnchor anchor;
            char anchor_field = cursor.peek();
            while (anchor_field != '\0' && anchor_field != '}') {
              std::wstring name;
              if (!cursor.parse_string(&name)) return false;
              if (!cursor.eat(':')) return false;
              double number = 0;
              cursor.parse_number(&number);
              if (name == L"chapter") anchor.chapter = std::max(1, static_cast<int>(number));
              if (name == L"start") anchor.start = static_cast<std::int64_t>(number);
              if (name == L"end") anchor.end = static_cast<std::int64_t>(number);
              cursor.eat(',');
              anchor_field = cursor.peek();
            }
            cursor.eat('}');
            annotation.anchor = anchor;
          } else if (field_name == L"rects" && cursor.peek() == '[') {
            cursor.eat('[');
            while (cursor.peek() == '{') {
              cursor.eat('{');
              AnnoRectD rect;
              char rect_field = cursor.peek();
              while (rect_field != '\0' && rect_field != '}') {
                std::wstring name;
                if (!cursor.parse_string(&name)) return false;
                if (!cursor.eat(':')) return false;
                double number = 0;
                cursor.parse_number(&number);
                if (name == L"x") rect.x = number;
                if (name == L"y") rect.y = number;
                if (name == L"width") rect.width = number;
                if (name == L"height") rect.height = number;
                // consume optional comma
                cursor.eat(',');
                rect_field = cursor.peek();
              }
              cursor.eat('}');
              annotation.rects.push_back(rect);
              cursor.eat(',');
            }
            cursor.eat(']');
          } else {
            cursor.skip_value();
          }
          cursor.eat(',');
          field = cursor.peek();
        }
        cursor.eat('}');
        parsed.annotations.push_back(std::move(annotation));
        cursor.eat(',');
      }
      cursor.eat(']');
    } else if (key == L"bookmarks") {
      saw_bookmarks = true;
      if (!cursor.eat('[')) return false;
      while (cursor.peek() == '{') {
        StoredBookmark bookmark;
        cursor.eat('{');
        char field = cursor.peek();
        while (field != '\0' && field != '}') {
          std::wstring field_name;
          if (!cursor.parse_string(&field_name)) return false;
          if (!cursor.eat(':')) return false;
          if (field_name == L"page") {
            double number = 1;
            cursor.parse_number(&number);
            bookmark.page = std::max(1, static_cast<int>(number));
          } else if (field_name == L"label") {
            cursor.parse_string(&bookmark.label);
          } else if (field_name == L"created") {
            double number = 0;
            cursor.parse_number(&number);
            bookmark.created = static_cast<std::int64_t>(number);
          } else {
            cursor.skip_value();
          }
          cursor.eat(',');
          field = cursor.peek();
        }
        cursor.eat('}');
        parsed.bookmarks.push_back(std::move(bookmark));
        cursor.eat(',');
      }
      cursor.eat(']');
    } else {
      cursor.skip_value();
    }
    cursor.eat(',');
    key_hint = cursor.peek();
  }
  cursor.eat('}');
  (void)saw_annotations;
  (void)saw_bookmarks;
  *out = std::move(parsed);
  return true;
}

// ---- load / save -----------------------------------------------------------------

void MainWindow::LoadDocData(std::shared_ptr<PdfTab> tab) {
  if (!ready_ || !api_ || !backend_ || !tab) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path = to_utf8(tab->path);
  bool const sidecar = settings_.annotation_sidecar;
  std::thread([weak, dispatcher, backend, tab, path, sidecar]() mutable {
    std::string json;
    bool recovered = false;
    try {
      rivet_app::API api(*backend);
      rivet::Bytes const bytes = api.get_annotations(path, sidecar).get();
      json.assign(bytes.begin(), bytes.end());
      recovered = api.annotation_recovery_pending(path, sidecar).get();
    } catch (...) {
      return;  // annotations stay empty; the file may simply have none yet
    }
    dispatcher.TryEnqueue([weak, tab, json = std::move(json), recovered]() mutable {
      auto window = weak.get();
      if (!window) return;
      // The tab may have closed or been reloaded meanwhile.
      bool alive = false;
      for (auto const& candidate : window->tabs_) {
        if (candidate.get() == tab.get()) {
          alive = true;
          break;
        }
      }
      if (!alive) return;
      // A pending local edit wins over the stored bytes; the debounce timer
      // will write it out and a later load refreshes from disk.
      if (tab->anno_dirty) {
        window->QueueAnnotationSave();
        return;
      }
      DocData data;
      if (!window->ParseDocData(json, &data)) return;
      tab->doc_data = std::move(data);
      tab->doc_data_loaded = true;
      tab->anno_dirty = recovered;
      tab->anno_save_phase = recovered ? AnnotationSavePhase::Recovered
                                       : AnnotationSavePhase::Saved;
      tab->anno_save_error.clear();
      if (window->active_pdf_.get() == tab.get()) {
        window->UpdateBookmarkToggle();
        window->RefreshLeftPanel();
      }
      for (auto const& cell : tab->cells) {
        window->DrawCellOverlays(*cell, *tab);
      }
    });
  }).detach();
}

void MainWindow::QueueAnnotationSave() {
  for (auto const& tab : tabs_) {
    if (!tab->anno_dirty) continue;
    ++tab->anno_revision;
    tab->anno_save_phase = AnnotationSavePhase::Pending;
    tab->anno_save_error.clear();
  }
  if (epub_ && epub_->anno_dirty) {
    ++epub_->anno_revision;
    epub_->anno_save_phase = AnnotationSavePhase::Pending;
    epub_->anno_save_error.clear();
  }
  if (left_mode_ == 2) RefreshLeftPanel();
  anno_timer_.Stop();
  anno_timer_.Start();  // 800 ms debounce (mac queueAnnotationSave)
}

void MainWindow::SaveDocDataNow(std::shared_ptr<PdfTab> tab) {
  if (!ready_ || !api_ || !backend_ || !tab) return;
  if (!tab->anno_dirty && !tab->doc_data_loaded) return;
  std::string const json = SerializeDocData(tab->doc_data);
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path = to_utf8(tab->path);
  bool const sidecar = settings_.annotation_sidecar;
  auto const revision = tab->anno_revision;
  tab->anno_save_phase = AnnotationSavePhase::Pending;
  std::thread([weak, dispatcher, backend, tab, path, sidecar, json, revision]() mutable {
    std::wstring error;
    try {
      rivet_app::API api(*backend);
      rivet::Bytes bytes(json.begin(), json.end());
      (void)api.set_annotations(path, sidecar, bytes).get();
    } catch (std::exception const& e) {
      error = to_wide(e.what());
    } catch (...) {
      error = L"Unknown annotation save failure";
    }
    dispatcher.TryEnqueue([weak, tab, revision, error = std::move(error)]() mutable {
      auto window = weak.get();
      if (!window || tab->anno_revision != revision) return;
      tab->anno_dirty = !error.empty();
      tab->anno_save_phase = error.empty() ? AnnotationSavePhase::Saved
                                           : AnnotationSavePhase::Failed;
      tab->anno_save_error = std::move(error);
      if (window->active_pdf_.get() == tab.get() && window->left_mode_ == 2) {
        window->RefreshLeftPanel();
      }
    });
  }).detach();
}

// ---- annotation edits --------------------------------------------------------------

void MainWindow::AddAnnotationFromSelection(std::wstring const& color,
                                            std::wstring const& kind) {
  auto tab = active_pdf_;
  if (!tab || !tab->doc) return;
  if (tab->selection_page <= 0 || tab->selection_text.empty() ||
      tab->selection_rects.empty()) {
    return;
  }
  int const page = tab->selection_page;
  double page_width = 612, page_height = 792;
  tab->doc->PageSize(page - 1, &page_width, &page_height);

  StoredAnnotation annotation;
  std::int64_t const now = static_cast<std::int64_t>(std::time(nullptr));
  wchar_t id_buffer[48];
  std::swprintf(id_buffer, 48, L"a-%lld-%04d", static_cast<long long>(now),
                static_cast<int>(std::rand() % 10000));
  annotation.id = id_buffer;
  annotation.page = page;
  for (auto const& rect : tab->selection_rects) {
    annotation.rects.push_back(to_stored_rect(rect, page_height));
  }
  annotation.excerpt = tab->selection_text.substr(
      0, std::min<std::size_t>(tab->selection_text.size(), 800));
  annotation.color = color;
  annotation.kind = kind;
  annotation.created = now;
  tab->doc_data.annotations.push_back(annotation);
  tab->anno_dirty = true;
  QueueAnnotationSave();
  for (auto const& cell : tab->cells) {
    if (cell->page == page) DrawCellOverlays(*cell, *tab);
  }
  if (left_mode_ == 2) RefreshLeftPanel();
}

void MainWindow::RemoveAnnotation(std::wstring const& id) {
  auto tab = active_pdf_;
  if (!tab) return;
  auto& annotations = tab->doc_data.annotations;
  auto const it = std::find_if(annotations.begin(), annotations.end(),
                               [&id](StoredAnnotation const& candidate) {
                                 return candidate.id == id;
                               });
  if (it == annotations.end()) return;
  // One-step undo: keep the removed annotation and its position for the
  // banner's five-second window (deletion persists as usual).
  undo_index_ = static_cast<std::size_t>(it - annotations.begin());
  undo_annotation_ = *it;
  annotations.erase(it);
  tab->anno_dirty = true;
  QueueAnnotationSave();
  for (auto const& cell : tab->cells) {
    DrawCellOverlays(*cell, *tab);
  }
  if (left_mode_ == 2) RefreshLeftPanel();
  undo_timer_.Stop();
  undo_timer_.Start();
}

void MainWindow::UndoDeleteAnnotation() {
  auto tab = active_pdf_;
  undo_timer_.Stop();
  if (!tab || !undo_annotation_) {
    undo_annotation_.reset();
    return;
  }
  auto const index =
      std::min(undo_index_, tab->doc_data.annotations.size());
  tab->doc_data.annotations.insert(
      tab->doc_data.annotations.begin() + static_cast<std::ptrdiff_t>(index),
      *undo_annotation_);
  undo_annotation_.reset();
  tab->anno_dirty = true;
  QueueAnnotationSave();
  for (auto const& cell : tab->cells) {
    DrawCellOverlays(*cell, *tab);
  }
  if (left_mode_ == 2) RefreshLeftPanel();
}

void MainWindow::ToggleBookmark() {
  auto tab = active_pdf_;
  if (!tab) return;
  int const page = tab->current_page;
  auto& bookmarks = tab->doc_data.bookmarks;
  auto const found = std::find_if(bookmarks.begin(), bookmarks.end(),
                                  [page](StoredBookmark const& candidate) {
                                    return candidate.page == page;
                                  });
  if (found != bookmarks.end()) {
    bookmarks.erase(found);
  } else {
    StoredBookmark bookmark;
    bookmark.page = page;
    bookmark.label = pdfgist::Trf("ui.notes.page", std::to_wstring(page));
    bookmark.created = static_cast<std::int64_t>(std::time(nullptr));
    bookmarks.push_back(bookmark);
  }
  std::sort(bookmarks.begin(), bookmarks.end(),
            [](StoredBookmark const& a, StoredBookmark const& b) {
              return a.page == b.page ? a.created < b.created
                                      : a.page < b.page;
            });
  tab->anno_dirty = true;
  QueueAnnotationSave();
  UpdateBookmarkToggle();
  if (left_mode_ == 2) RefreshLeftPanel();
}

void MainWindow::UpdateBookmarkToggle() {
  auto tab = active_pdf_;
  bool const bookmarked =
      tab != nullptr &&
      std::any_of(tab->doc_data.bookmarks.begin(),
                  tab->doc_data.bookmarks.end(),
                  [tab](StoredBookmark const& candidate) {
                    return candidate.page == tab->current_page;
                  });
  BookmarkToggle().IsChecked(bookmarked);
}

// ---- overlays ----------------------------------------------------------------------

void MainWindow::DrawStoredOverlays(PdfTab::Cell& cell, PdfTab const& tab) {
  if (!tab.doc_data_loaded) return;
  double page_width = 612, page_height = 792;
  tab.doc->PageSize(cell.page - 1, &page_width, &page_height);
  double const scale = tab.cells_scale_;

  auto paint = [&](winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas) {
    if (!canvas) return;
    for (auto const& annotation : tab.doc_data.annotations) {
      if (annotation.page != cell.page) continue;
      wu::Color const color = AnnotationColor(annotation.color);
      bool const underline = annotation.kind == L"underline";
      bool const strike = annotation.kind == L"strike";
      for (auto const& stored : annotation.rects) {
        auto const rect = to_top_left_rect(stored, page_height);
        double const left = rect[0] * scale;
        double const width = (rect[2] - rect[0]) * scale;
        double const height = (rect[3] - rect[1]) * scale;
        if (width <= 0 || height <= 0) continue;
        double const top = rect[1] * scale;
        double const bar_height =
            std::max(1.2, 1.1 * scale);  // ~1.1pt bar, view px
        double const bar_top =
            underline ? (rect[3] * scale - bar_height)
                      : (top + height / 2.0 - bar_height / 2.0);
        auto shape = shapes::Rectangle();
        shape.Fill(Solid(color));
        shape.Width(width);
        shape.Height(underline || strike ? bar_height : height);
        muxc::Canvas::SetLeft(shape, left);
        muxc::Canvas::SetTop(shape, underline || strike ? bar_top : top);
        shape.IsHitTestVisible(false);
        canvas.Children().Append(shape);
      }
    }
  };
  paint(cell.canvas);
}

// ---- notes pane ----------------------------------------------------------------------

void MainWindow::AppendAnnotationSaveBanner(
    muxc::StackPanel const& content, AnnotationSavePhase phase,
    std::wstring const& error) {
  if (phase == AnnotationSavePhase::Saved) return;

  auto banner = muxc::Border();
  banner.Padding({8, 8, 8, 8});
  banner.CornerRadius({8, 8, 8, 8});
  banner.BorderThickness({1, 1, 1, 1});
  banner.BorderBrush(ThemeBorderBrush());
  banner.Background(ThemePanelBrush());
  auto stack = muxc::StackPanel();
  stack.Spacing(6);
  auto label = muxc::TextBlock();
  label.TextWrapping(mux::TextWrapping::Wrap);
  label.FontSize(11);
  if (phase == AnnotationSavePhase::Pending) {
    label.Text(pdfgist::Tr("ui.notes.save-pending"));
  } else if (phase == AnnotationSavePhase::Recovered) {
    label.Text(pdfgist::Tr("ui.notes.recovery-pending"));
  } else {
    label.Text(pdfgist::Trf("ui.notes.save-failed", error));
  }
  stack.Children().Append(label);

  if (phase == AnnotationSavePhase::Recovered ||
      phase == AnnotationSavePhase::Failed) {
    auto actions = muxc::StackPanel();
    actions.Orientation(muxc::Orientation::Horizontal);
    actions.Spacing(8);
    auto retry = muxc::Button();
    retry.Content(winrt::box_value(pdfgist::Tr("ui.notes.retry-save")));
    auto weak = get_weak();
    retry.Click([weak](auto&&, auto&&) {
      if (auto window = weak.get()) {
        if (window->epub_active_ && window->epub_) {
          window->epub_->anno_dirty = true;
          window->SaveEpubDocDataNow();
        } else if (window->active_pdf_) {
          window->active_pdf_->anno_dirty = true;
          window->SaveDocDataNow(window->active_pdf_);
        }
        window->RefreshLeftPanel();
      }
    });
    actions.Children().Append(retry);
    auto export_button = muxc::Button();
    export_button.Content(winrt::box_value(pdfgist::Tr("ui.notes.export")));
    export_button.Click([weak](auto&&, auto&&) {
      if (auto window = weak.get()) window->ExportAnnotations();
    });
    actions.Children().Append(export_button);
    stack.Children().Append(actions);
  }
  banner.Child(stack);
  content.Children().Append(banner);
}

void MainWindow::BuildNotesPane(
    muxc::StackPanel const& content) {
  auto tab = active_pdf_;
  if (!tab) return;

  AppendAnnotationSaveBanner(content, tab->anno_save_phase,
                             tab->anno_save_error);

  // One-step undo banner (mac NotesPane banner parity).
  if (undo_annotation_) {
    auto banner = muxc::Border();
    banner.Padding({8, 8, 8, 8});
    banner.CornerRadius({8, 8, 8, 8});
    banner.BorderThickness({1, 1, 1, 1});
    banner.BorderBrush(ThemeBorderBrush());
    banner.Background(ThemePanelBrush());
    auto banner_stack = muxc::StackPanel();
    banner_stack.Orientation(muxc::Orientation::Horizontal);
    banner_stack.Spacing(8);
    auto banner_label = muxc::TextBlock();
    banner_label.Text(pdfgist::Tr("ui.notes.deleted"));
    banner_label.FontSize(11);
    banner_label.VerticalAlignment(mux::VerticalAlignment::Center);
    banner_stack.Children().Append(banner_label);
    auto undo_btn = muxc::Button();
    auto undo_label = muxc::TextBlock();
    undo_label.Text(pdfgist::Tr("ui.undo"));
    undo_label.FontSize(11);
    undo_btn.Content(undo_label);
    auto const undo_weak = get_weak();
    undo_btn.Click([undo_weak](winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
      if (auto window = undo_weak.get()) window->UndoDeleteAnnotation();
    });
    banner_stack.Children().Append(undo_btn);
    banner.Child(banner_stack);
    content.Children().Append(banner);
  }

  // Notes digest export (mac NotesPane export button parity): one click,
  // backend renders the Markdown, SaveData picks the destination.
  if (!tab->doc_data.annotations.empty() ||
      !tab->doc_data.bookmarks.empty()) {
    auto export_btn = muxc::Button();
    auto label = muxc::TextBlock();
    label.Text(pdfgist::Tr("ui.notes.export"));
    label.FontSize(12);
    export_btn.Content(label);
    export_btn.Margin({0, 0, 0, 6});
    auto const weak = get_weak();
    export_btn.Click([weak](winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->ExportAnnotations();
    });
    content.Children().Append(export_btn);
  }

  if (tab->doc_data.annotations.empty()) {
    auto empty = muxc::TextBlock();
    empty.Text(pdfgist::Tr("ui.notes.empty"));
    empty.FontSize(11);
    empty.Opacity(0.6);
    empty.TextWrapping(mux::TextWrapping::Wrap);
    empty.Padding({6, 6, 6, 6});
    content.Children().Append(empty);
  }

  auto const weak = get_weak();
  auto sorted = tab->doc_data.annotations;
  std::sort(sorted.begin(), sorted.end(),
            [](StoredAnnotation const& a, StoredAnnotation const& b) {
              return a.page == b.page ? a.created < b.created
                                      : a.page < b.page;
            });

  for (auto const& annotation : sorted) {
    auto card = muxc::Border();
    card.Padding({8, 8, 8, 8});
    card.CornerRadius({8, 8, 8, 8});
    card.BorderThickness({1, 1, 1, 1});
    card.BorderBrush(ThemeBorderBrush());
    card.Background(ThemePanelBrush());

    auto stack = muxc::StackPanel();
    stack.Spacing(5);

    auto header = muxc::StackPanel();
    header.Orientation(muxc::Orientation::Horizontal);
    header.Spacing(6);

    auto dot = shapes::Ellipse();
    dot.Width(9);
    dot.Height(9);
    dot.Fill(Solid(AnnotationColor(annotation.color)));
    dot.VerticalAlignment(mux::VerticalAlignment::Center);
    header.Children().Append(dot);

    auto page_label = muxc::TextBlock();
    page_label.Text(
        pdfgist::Trf("ui.notes.page", std::to_wstring(annotation.page)));
    page_label.FontSize(11);
    page_label.Opacity(0.7);
    page_label.VerticalAlignment(mux::VerticalAlignment::Center);
    header.Children().Append(page_label);

    if (annotation.kind != L"highlight" && !annotation.kind.empty()) {
      auto kind_label = muxc::TextBlock();
      kind_label.Text(annotation.kind);
      kind_label.FontSize(9);
      kind_label.Opacity(0.5);
      kind_label.VerticalAlignment(mux::VerticalAlignment::Center);
      header.Children().Append(kind_label);
    }

    auto translate_button = muxc::Button();
    translate_button.Padding({5, 0, 5, 0});
    translate_button.MinWidth(0);
    translate_button.MinHeight(0);
    muxc::TextBlock translate_label;
    translate_label.Text(pdfgist::Tr("ui.notes.translate-btn"));
    translate_label.FontSize(10);
    translate_label.Foreground(Solid({0xFF, 0xB4, 0x53, 0x09}));
    translate_button.Content(translate_label);
    std::wstring const excerpt = annotation.excerpt;
    translate_button.Click([weak, excerpt](
                               winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->LaunchTranslateSelectionText(excerpt);
    });
    header.Children().Append(translate_button);

    auto delete_button = muxc::Button();
    delete_button.Padding({5, 0, 5, 0});
    delete_button.MinWidth(0);
    delete_button.MinHeight(0);
    muxc::TextBlock delete_label;
    delete_label.Text(pdfgist::Tr("ui.notes.delete-btn"));
    delete_label.FontSize(10);
    delete_label.Opacity(0.6);
    delete_button.Content(delete_label);
    std::wstring const id = annotation.id;
    delete_button.Click([weak, id](
                            winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->RemoveAnnotation(id);
    });
    header.Children().Append(delete_button);
    stack.Children().Append(header);

    auto excerpt_label = muxc::TextBlock();
    excerpt_label.Text(annotation.excerpt);
    excerpt_label.FontSize(12);
    excerpt_label.TextWrapping(mux::TextWrapping::Wrap);
    excerpt_label.MaxLines(4);
    excerpt_label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    stack.Children().Append(excerpt_label);

    if (!annotation.note.empty()) {
      auto note_label = muxc::TextBlock();
      note_label.Text(annotation.note);
      note_label.FontSize(11);
      note_label.Opacity(0.7);
      note_label.TextWrapping(mux::TextWrapping::Wrap);
      stack.Children().Append(note_label);
    }

    card.Child(stack);
    int const page = annotation.page;
    card.PointerPressed([weak, page](
                            winrt::Windows::Foundation::IInspectable const&,
                            mux::Input::PointerRoutedEventArgs const& args) {
      if (auto window = weak.get()) {
        if (auto shared = window->active_pdf_) window->ScrollToPage(*shared, page);
        args.Handled(true);
      }
    });
    content.Children().Append(card);
  }

  // bookmarks section
  auto bookmarks_header = muxc::TextBlock();
  bookmarks_header.Text(pdfgist::Tr("ui.notes.bookmarks"));
  bookmarks_header.FontSize(11);
  bookmarks_header.FontWeight(wut::FontWeights::SemiBold());
  bookmarks_header.Opacity(0.7);
  bookmarks_header.Margin({0, 8, 0, 0});
  content.Children().Append(bookmarks_header);

  auto bookmarks = tab->doc_data.bookmarks;
  if (bookmarks.empty()) {
    auto empty = muxc::TextBlock();
    empty.Text(pdfgist::Tr("ui.notes.bookmark-empty"));
    empty.FontSize(11);
    empty.Opacity(0.5);
    empty.TextWrapping(mux::TextWrapping::Wrap);
    empty.Margin({0, 2, 0, 0});
    content.Children().Append(empty);
  }
  for (auto const& bookmark : bookmarks) {
    auto row = muxc::Border();
    row.Padding({8, 4, 8, 4});
    row.CornerRadius({6, 6, 6, 6});
    row.BorderThickness({1, 1, 1, 1});
    row.BorderBrush(ThemeBorderBrush());
    row.Background(ThemePanelBrush());

    auto stack = muxc::StackPanel();
    stack.Orientation(muxc::Orientation::Horizontal);
    stack.Spacing(6);

    auto page_label = muxc::TextBlock();
    page_label.Text(std::to_wstring(bookmark.page));
    page_label.FontSize(10);
    page_label.FontWeight(wut::FontWeights::SemiBold());
    page_label.Foreground(Solid({0xFF, 0xB4, 0x53, 0x09}));
    page_label.VerticalAlignment(mux::VerticalAlignment::Center);
    stack.Children().Append(page_label);

    auto label = muxc::TextBlock();
    label.Text(bookmark.label.empty()
                   ? pdfgist::Trf("ui.notes.page",
                                  std::to_wstring(bookmark.page))
                   : bookmark.label);
    label.FontSize(12);
    label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    label.MaxWidth(130);
    label.VerticalAlignment(mux::VerticalAlignment::Center);
    stack.Children().Append(label);

    auto spacer = muxc::Border();
    spacer.Width(8);
    stack.Children().Append(spacer);

    auto delete_button = muxc::Button();
    delete_button.Padding({5, 0, 5, 0});
    delete_button.MinWidth(0);
    delete_button.MinHeight(0);
    muxc::TextBlock delete_label;
    delete_label.Text(pdfgist::Tr("ui.notes.delete-btn"));
    delete_label.FontSize(10);
    delete_label.Opacity(0.6);
    delete_button.Content(delete_label);
    muxc::ToolTipService::SetToolTip(delete_button,
                                     winrt::box_value(pdfgist::Tr(
                                         "ui.notes.bookmark-remove")));
    int const page = bookmark.page;
    delete_button.Click([weak, page](
                            winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        if (auto shared = window->active_pdf_) {
          auto& list = shared->doc_data.bookmarks;
          list.erase(std::remove_if(list.begin(), list.end(),
                                    [page](StoredBookmark const& candidate) {
                                      return candidate.page == page;
                                    }),
                     list.end());
          shared->anno_dirty = true;
          window->QueueAnnotationSave();
          window->UpdateBookmarkToggle();
          window->RefreshLeftPanel();
        }
      }
    });
    stack.Children().Append(delete_button);

    row.Child(stack);
    row.PointerPressed([weak, page](
                           winrt::Windows::Foundation::IInspectable const&,
                           mux::Input::PointerRoutedEventArgs const& args) {
      if (auto window = weak.get()) {
        if (auto shared = window->active_pdf_) window->ScrollToPage(*shared, page);
        args.Handled(true);
      }
    });
    content.Children().Append(row);
  }
}

// ---- notes digest export ------------------------------------------------------------

void MainWindow::ExportAnnotations() {
  auto tab = active_pdf_.get();
  auto book = epub_;
  bool const for_epub = epub_active_ && book != nullptr;
  if (!for_epub && (!tab || !ready_ || !backend_)) return;
  std::string const json =
      for_epub ? SerializeDocData(book->doc_data)
               : SerializeDocData(tab->doc_data);
  std::wstring const title = for_epub ? book->title : tab->title;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, json, title]() mutable {
    std::string markdown;
    try {
      rivet_app::API api(*backend);
      rivet::Bytes bytes(json.begin(), json.end());
      markdown = api.export_annotations(bytes, to_utf8(title)).get();
    } catch (...) {
      return;  // best effort; the pane stays usable
    }
    dispatcher.TryEnqueue([weak, markdown = std::move(markdown),
                           title]() {
      auto window = weak.get();
      if (!window) return;
      std::wstring stem = title;
      auto const dot = stem.find_last_of(L'.');
      if (dot != std::wstring::npos) stem.resize(dot);
      std::vector<std::uint8_t> bytes(markdown.begin(), markdown.end());
      window->SaveData(std::move(bytes),
                       stem + pdfgist::Tr("ui.notes.export-suffix") + L".md",
                       L"Markdown", L".md");
    });
  }).detach();
}


// ---- EPUB notes pane (annotate buttons + list + undo + export) ----------

void MainWindow::BuildEpubNotesPane(
    muxc::StackPanel const& content) {
  auto book = epub_;
  if (!book) return;

  AppendAnnotationSaveBanner(content, book->anno_save_phase,
                             book->anno_save_error);

  if (book->undo_annotation) {
    auto banner = muxc::Border();
    banner.Padding({8, 8, 8, 8});
    banner.CornerRadius({8, 8, 8, 8});
    banner.BorderThickness({1, 1, 1, 1});
    banner.BorderBrush(ThemeBorderBrush());
    banner.Background(ThemePanelBrush());
    auto stack = muxc::StackPanel();
    stack.Orientation(muxc::Orientation::Horizontal);
    stack.Spacing(8);
    auto label = muxc::TextBlock();
    label.Text(pdfgist::Tr("ui.notes.deleted"));
    label.FontSize(11);
    label.VerticalAlignment(mux::VerticalAlignment::Center);
    stack.Children().Append(label);
    auto undo_btn = muxc::Button();
    auto undo_label = muxc::TextBlock();
    undo_label.Text(pdfgist::Tr("ui.undo"));
    undo_label.FontSize(11);
    undo_btn.Content(undo_label);
    auto const undo_weak = get_weak();
    undo_btn.Click([undo_weak](winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
      if (auto window = undo_weak.get()) window->UndoDeleteEpubAnnotation();
    });
    stack.Children().Append(undo_btn);
    banner.Child(stack);
    content.Children().Append(banner);
  }

  // Annotate the pending selection (mac EpubNotesPane annotate row parity).
  if (!book->selection_text.empty() && book->selection_chapter >= 1) {
    auto panel = muxc::Border();
    panel.Padding({8, 8, 8, 8});
    panel.CornerRadius({8, 8, 8, 8});
    panel.BorderThickness({1, 1, 1, 1});
    panel.BorderBrush(ThemeBorderBrush());
    panel.Background(ThemePanelBrush());
    auto stack = muxc::StackPanel();
    auto heading = muxc::TextBlock();
    heading.Text(pdfgist::Tr("ui.notes.epub-annotate"));
    heading.FontSize(11);
    stack.Children().Append(heading);
    auto row = muxc::StackPanel();
    row.Orientation(muxc::Orientation::Horizontal);
    row.Spacing(6);
    row.Margin({0, 6, 0, 0});
    auto const weak = get_weak();
    auto add_button = [&](std::wstring const& title, std::wstring const& color,
                          std::wstring const& kind) {
      auto btn = muxc::Button();
      btn.Padding({8, 2, 8, 2});
      auto text = muxc::TextBlock();
      text.Text(title);
      text.FontSize(11);
      btn.Content(text);
      btn.Click([weak, color, kind](
                    winrt::Windows::Foundation::IInspectable const&,
                    mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) {
          window->AddEpubAnnotation(color, kind);
        }
      });
      row.Children().Append(btn);
    };
    add_button(pdfgist::Tr("ui.menu.highlight-yellow"), L"yellow", L"highlight");
    add_button(pdfgist::Tr("ui.menu.highlight-green"), L"green", L"highlight");
    add_button(pdfgist::Tr("ui.menu.highlight-blue"), L"blue", L"highlight");
    add_button(pdfgist::Tr("ui.menu.underline"), L"", L"underline");
    add_button(pdfgist::Tr("ui.menu.strike"), L"", L"strike");
    stack.Children().Append(row);
    panel.Child(stack);
    content.Children().Append(panel);
  }

  if (book->doc_data.annotations.empty()) {
    auto empty = muxc::TextBlock();
    empty.Text(pdfgist::Tr("ui.notes.empty"));
    empty.FontSize(11);
    empty.Opacity(0.6);
    empty.TextWrapping(mux::TextWrapping::Wrap);
    empty.Padding({6, 6, 6, 6});
    content.Children().Append(empty);
    return;
  }

  auto sorted = book->doc_data.annotations;
  std::sort(sorted.begin(), sorted.end(),
            [](StoredAnnotation const& a, StoredAnnotation const& b) {
              return a.page == b.page ? a.created < b.created
                                      : a.page < b.page;
            });
  for (auto const& annotation : sorted) {
    auto card = muxc::Border();
    card.Padding({8, 8, 8, 8});
    card.CornerRadius({8, 8, 8, 8});
    card.BorderThickness({1, 1, 1, 1});
    card.BorderBrush(ThemeBorderBrush());
    card.Background(ThemePanelBrush());
    auto stack = muxc::StackPanel();
    stack.Spacing(4);
    auto head = muxc::StackPanel();
    head.Orientation(muxc::Orientation::Horizontal);
    head.Spacing(6);
    auto page_label = muxc::TextBlock();
    page_label.Text(pdfgist::Trf("ui.epub.chapter-n",
                                 std::to_wstring(annotation.page)));
    page_label.FontSize(10);
    page_label.Opacity(0.6);
    head.Children().Append(page_label);
    auto spacer = muxc::Border();
    spacer.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    head.Children().Append(spacer);
    auto del = muxc::Button();
    del.Padding({6, 0, 6, 0});
    auto del_label = muxc::TextBlock();
    del_label.Text(pdfgist::Tr("ui.notes.delete-btn"));
    del_label.FontSize(10);
    del.Content(del_label);
    auto const weak = get_weak();
    auto const id = annotation.id;
    del.Click([weak, id](winrt::Windows::Foundation::IInspectable const&,
                         mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->RemoveEpubAnnotation(id);
    });
    head.Children().Append(del);
    stack.Children().Append(head);
    auto excerpt = muxc::TextBlock();
    excerpt.Text(annotation.excerpt);
    excerpt.FontSize(12);
    excerpt.TextWrapping(mux::TextWrapping::Wrap);
    stack.Children().Append(excerpt);
    if (!annotation.note.empty()) {
      auto note = muxc::TextBlock();
      note.Text(annotation.note);
      note.FontSize(11);
      note.Opacity(0.7);
      note.TextWrapping(mux::TextWrapping::Wrap);
      stack.Children().Append(note);
    }
    card.Child(stack);
    content.Children().Append(card);
  }

  auto export_btn = muxc::Button();
  auto export_label = muxc::TextBlock();
  export_label.Text(pdfgist::Tr("ui.notes.export"));
  export_label.FontSize(12);
  export_btn.Content(export_label);
  auto const export_weak = get_weak();
  export_btn.Click([export_weak](winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
    if (auto window = export_weak.get()) window->ExportAnnotations();
  });
  content.Children().Append(export_btn);
}

}  // namespace winrt::RivetHost::implementation
