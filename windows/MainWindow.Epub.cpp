#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <cwchar>
#include <fstream>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

using Microsoft::UI::Xaml::Visibility;

namespace {

std::wstring file_name(std::wstring const& path) {
  std::size_t const slash = path.find_last_of(L"/\\");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring escape_html(std::wstring const& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t const c : s) {
    switch (c) {
      case L'&': out += L"&amp;"; break;
      case L'<': out += L"&lt;"; break;
      case L'>': out += L"&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

std::wstring js_string(std::wstring const& s) {
  std::wstring out = L"\"";
  for (wchar_t const c : s) {
    switch (c) {
      case L'\\': out += L"\\\\"; break;
      case L'"': out += L"\\\""; break;
      case L'\n': out += L"\\n"; break;
      case L'\r': out += L"\\r"; break;
      default: out += c;
    }
  }
  out += L"\"";
  return out;
}

}  // namespace

// ---- open / close ---------------------------------------------------------------------

void MainWindow::OpenEpub(std::wstring const& path, int resume_chapter) {
  for (auto const& existing : epubs_) {
    if (existing->path == path) {
      epub_ = existing;
      ActivateEpub();
      if (resume_chapter > 1) GoToChapter(*existing, resume_chapter);
      return;
    }
  }
  if (!ready_ || !api_) {
    Alert(pdfgist::Tr("ui.backend-not-ready"));
    return;
  }
  // File-first multi-document reading: keep every original EPUB open.
  auto book = std::make_shared<EpubBook>();
  book->path = path;
  book->title = file_name(path);
  // Persisted typography rides in; the page picks it up post-load.
  book->font_scale =
      settings_.reading_font_scale_permille / 1000.0;
  epubs_.push_back(book);
  epub_ = book;

  muxc::TabViewItem item;
  muxc::TextBlock header;
  header.Text(book->title);
  header.MaxWidth(150);
  header.TextTrimming(mux::TextTrimming::CharacterEllipsis);
  item.Header(header);
  item.IsClosable(true);

  // WebView2 needs a placeholder in the tree before the async controller
  // creation completes.
  auto web = muxc::WebView2();
  book->web = web;
  book->tab_item = item;
  item.Content(web);

  DocTabs().TabItems().Append(item);
  // Match PDF startup: the first reader reveals its native navigation pane.
  // Without this, a fresh Windows profile parsed the EPUB TOC correctly but
  // kept every chapter inaccessible behind a collapsed left panel.
  if (tabs_.empty() && !left_visible_) {
    left_visible_ = true;
    LeftToggle().IsChecked(true);
    LeftPanel().Visibility(Visibility::Visible);
    SaveHostPrefs();
  }
  ActivateEpub();
  RefreshLeftPanel();
  UpdateChrome();
  NoteActivity();
  LoadEpubBook(book);
  if (resume_chapter > 1) book->current_chapter = resume_chapter;
}

void MainWindow::ActivateEpub() {
  if (!epub_) return;
  DocTabs().SelectedIndex(IndexOfTabItem(epub_->tab_item));
}

void MainWindow::CloseEpub() {
  if (!epub_) return;
  auto const book = epub_;
  // Persist this book's annotations/chapter before releasing its own tab.
  // SelectionChanged may fire synchronously while TabItems is modified.
  SaveEpubDocDataNow();
  SaveRecentNow();
  auto const index = IndexOfTabItem(book->tab_item);
  epubs_.erase(std::remove(epubs_.begin(), epubs_.end(), book), epubs_.end());
  if (index < DocTabs().TabItems().Size()) {
    DocTabs().TabItems().RemoveAt(index);
  }
  epub_ = nullptr;
  epub_active_ = false;
  auto const selected = DocTabs().SelectedIndex();
  if (selected >= 0 && static_cast<std::uint32_t>(selected) <
                            DocTabs().TabItems().Size()) {
    auto const item = DocTabs().TabItems()
                          .GetAt(static_cast<std::uint32_t>(selected))
                          .try_as<muxc::TabViewItem>();
    for (auto const& remaining : epubs_) {
      if (remaining->tab_item == item) {
        epub_ = remaining;
        epub_active_ = true;
        break;
      }
    }
  }
  UpdateChrome();
  RefreshLeftPanel();
}

void MainWindow::LoadEpubBook(std::shared_ptr<EpubBook> book) {
  if (!api_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path_utf8 = to_utf8(book->path);
  bool const sidecar = settings_.annotation_sidecar;

  // Serial chapter fetches (mac loadBook parity: concurrent epub RPCs can
  // stall on the embedded channel).
  std::thread([weak, dispatcher, backend, book, path_utf8, sidecar]() mutable {
    std::string failure;
    rivet_app::EpubView view{};
    std::vector<std::pair<int, std::wstring>> sections;
    // Worker threads never mutate EpubBook: the selected tab and its search
    // cache are read on the UI thread while other EPUBs are still loading.
    std::map<int, std::wstring> loaded_text;
    std::string loaded_annotations;
    bool annotations_loaded = false;
    bool recovery_pending = false;
    try {
      rivet_app::API api(*backend);
      view = api.epub_open(path_utf8).get();
      int const chapters = static_cast<int>(view.chapters);
      for (int i = 1; i <= std::max(1, chapters); ++i) {
        auto body_bytes = api.epub_chapter_html(path_utf8, i).get();
        // sanitized UTF-8 chapter body; widen for NavigateToString
        sections.emplace_back(
            i, to_wide(std::string(body_bytes.begin(), body_bytes.end())));
        loaded_text.emplace(
            i, to_wide(api.epub_chapter_text(path_utf8, i).get()));
      }
      // Annotations share the PDF store (same JSON shape; page = chapter).
      try {
        auto anno_bytes = api.get_annotations(path_utf8, sidecar).get();
        loaded_annotations.assign(anno_bytes.begin(), anno_bytes.end());
        annotations_loaded = true;
        recovery_pending =
            api.annotation_recovery_pending(path_utf8, sidecar).get();
      } catch (...) {
      }
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, book, view, sections = std::move(sections),
                           loaded_text = std::move(loaded_text),
                           loaded_annotations = std::move(loaded_annotations),
                           annotations_loaded, recovery_pending,
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (std::find(window->epubs_.begin(), window->epubs_.end(), book) ==
          window->epubs_.end()) return;  // tab closed meanwhile
      if (!failure.empty()) {
        window->Alert(pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
        return;
      }
      if (!view.title.empty()) {
        book->title = to_wide(view.title);
        if (auto header = book->tab_item.Header().try_as<muxc::TextBlock>()) {
          header.Text(book->title);
        }
      }
      book->chapters = static_cast<int>(view.chapters);
      book->toc = std::move(view.toc);
      // Commit the complete snapshot on the UI thread only, after confirming
      // this book still belongs to the window (it may have closed mid-load).
      book->text_cache = std::move(loaded_text);
      book->doc_data_bytes = std::move(loaded_annotations);
      book->annotations_pending = annotations_loaded;
      book->annotations_recovered = recovery_pending;
      book->text_ready = true;
      window->NavigateToEpubController(book, sections);
      window->ApplyEpubTheme(*book);
      if (book->annotations_pending) {
        book->annotations_pending = false;
        if (window->ParseDocData(book->doc_data_bytes, &book->doc_data)) {
          book->doc_data_loaded = true;
          book->anno_dirty = book->annotations_recovered;
          book->anno_save_phase = book->annotations_recovered
                                      ? AnnotationSavePhase::Recovered
                                      : AnnotationSavePhase::Saved;
          window->ApplyEpubAnnotations(*book);
        }
      }
      window->RefreshLeftPanel();
      // EPUB remains at the original path; no library registration.

    });
  }).detach();
}

// Controller creation is async; the HTML load starts once it completes.
// The default environment keeps the user-data folder under the per-user
// local app data for unpackaged apps, so read-only install directories do
// not break the controller; the event handlers use generic lambdas because
// the wv2winrt-generated projection is not always include-ready.
void MainWindow::NavigateToEpubController(
    std::shared_ptr<EpubBook> book,
    std::vector<std::pair<int, std::wstring>> const& sections) {
  auto const weak = get_weak();
  auto html = BuildEpubHtml(*book, sections);

  auto const weak_book = std::weak_ptr<EpubBook>(book);
  book->web.EnsureCoreWebView2Async().Completed(
      [weak, weak_book, html = std::move(html)](
          winrt::Windows::Foundation::IAsyncAction const& sender,
          winrt::Windows::Foundation::AsyncStatus) {
        auto window = weak.get();
        auto locked_book = weak_book.lock();
        if (!window || !locked_book || !locked_book->web) return;
        try {
          sender.GetResults();
        } catch (winrt::hresult_error const& e) {
          window->Alert(
              pdfgist::Trf("ui.pages.edit-failed", std::wstring(e.message())));
          return;
        } catch (...) {
          return;
        }
        auto const webview = locked_book->web.CoreWebView2();
        if (!webview) return;
        // selection + chapter-tracking bridge (window.chrome.webview)
        webview.WebMessageReceived(
            [weak, weak_book](winrt::Windows::Foundation::IInspectable const&,
                              auto const& args) {
              if (auto current = weak.get()) {
                if (auto current_book = weak_book.lock()) {
                  // A WebView2 callback may outlive its closed native tab.
                  // Never let that stale book schedule state updates.
                  if (std::find(current->epubs_.begin(),
                                current->epubs_.end(), current_book) !=
                      current->epubs_.end()) {
                    current->OnEpubWebMessage(*current_book, args);
                  }
                }
              }
            });
        // NavigateToString's limit is 2 MB; books beyond that would need a
        // virtual host mapping — a documented later change.
        webview.NavigateToString(html);
      });
}

void MainWindow::OnEpubWebMessage(
    EpubBook& book,
    winrt::Microsoft::Web::WebView2::Core::CoreWebView2WebMessageReceivedEventArgs const&
        args) {
  // The page posts JSON strings: {kind:'selection'|'chapter', ...}
  try {
    auto const message = args.WebMessageAsJson();
    auto const json = std::wstring(message);
    auto const find_string_field = [&json](wchar_t const* key) -> std::wstring {
      // tiny extractor for machine-generated {"key":"value"} pairs
      std::wstring const needle = std::wstring(L"\"") + key + L"\":\"";
      std::size_t const at = json.find(needle);
      if (at == std::wstring::npos) return {};
      std::size_t const start = at + needle.size();
      std::size_t cursor = start;
      while (cursor < json.size()) {
        if (json[cursor] == L'\\') {
          cursor += 2;
          continue;
        }
        if (json[cursor] == L'"') break;
        ++cursor;
      }
      std::wstring raw = json.substr(start, cursor - start);
      // minimal unescape
      std::wstring out;
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\\' && i + 1 < raw.size()) {
          ++i;
          switch (raw[i]) {
            case L'n': out += L'\n'; break;
            case L't': out += L'\t'; break;
            default: out += raw[i];
          }
        } else {
          out += raw[i];
        }
      }
      return out;
    };
    auto const kind = find_string_field(L"kind");
    if (kind == L"selection") {
      book.selection_text = find_string_field(L"text");
      auto const find_number_field = [&json](wchar_t const* key) -> int {
        std::wstring const needle = std::wstring(L"\"") + key + L"\":";
        std::size_t const at = json.find(needle);
        if (at == std::wstring::npos) return 0;
        return static_cast<int>(
            std::wcstol(json.c_str() + at + needle.size(), nullptr, 10));
      };
      book.selection_chapter = find_number_field(L"chapter");
      book.selection_start = find_number_field(L"start");
      book.selection_end = find_number_field(L"end");
      return;
    }
    if (kind == L"chapter") {
      // {"kind":"chapter","chapter":N}
      std::size_t const at = json.find(L"\"chapter\":");
      if (at != std::wstring::npos) {
        int const chapter = std::wcstol(json.c_str() + at + 10, nullptr, 10);
        if (chapter >= 1 && chapter <= std::max(1, book.chapters) &&
            chapter != book.current_chapter) {
          book.current_chapter = chapter;
          NoteActivity();
          if (left_mode_ == 1 && epub_active_) RenderEpubToc(book);
        }
      }
      return;
    }
    if (kind == L"sel-action") {
      HandleEpubSelectionAction(book, find_string_field(L"action"));
      return;
    }
  } catch (...) {
  }
}

// Selection bar actions (design/prototype/reading.html .selbar): the bar is
// inert chrome, every action lands here so business stays host-side.
void MainWindow::HandleEpubSelectionAction(
    EpubBook& book, std::wstring const& action) {
  auto clear_pending_selection = [this, &book]() {
    book.selection_text.clear();
    book.selection_chapter = 0;
    book.selection_start = 0;
    book.selection_end = 0;
    if (left_mode_ == 2 && epub_active_) RefreshLeftPanel();
  };
  if (action == L"hl-yellow" || action == L"hl-blue" || action == L"hl-green") {
    AddEpubAnnotation(action.substr(3), L"highlight");
    clear_pending_selection();
    return;
  }
  if (action == L"underline" || action == L"strike") {
    AddEpubAnnotation(L"", action);
    clear_pending_selection();
    return;
  }
  if (action == L"copy") {
    if (!book.selection_text.empty()) {
      try {
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(book.selection_text);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(
            package);
      } catch (...) {
      }
    }
    return;
  }
  if (action == L"ai") {
    // Open the inspector on the selection scope, as the prototype's AI act.
    ai_visible_ = true;
    try {
      AiToggle().IsChecked(true);
    } catch (...) {
    }
    chat_scope_ = 1;
    RefreshChatScopeButtons();
    UpdateChrome();
  }
}

// ---- presentation ----------------------------------------------------------------------

void MainWindow::ApplyEpubTheme(EpubBook& book) {
  if (!book.web || !book.web.CoreWebView2()) return;
  std::wstring const theme = ReadingSurface();
  auto const script = L"document.documentElement.setAttribute('data-theme', " +
                      js_string(theme) + L");";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
}

void MainWindow::ApplyEpubFontScale(EpubBook& book) {
  if (!book.web || !book.web.CoreWebView2()) return;
  auto const script =
      L"document.documentElement.style.setProperty('--fs', " +
      std::to_wstring(book.font_scale) + L");";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
}

void MainWindow::GoToChapter(EpubBook& book, int chapter) {
  book.current_chapter = std::clamp(chapter, 1, std::max(1, book.chapters));
  if (!book.web || !book.web.CoreWebView2()) return;
  auto const script =
      L"(() => { const els = document.querySelectorAll('.epub-chapter');"
      L" const el = els[" +
      std::to_wstring(book.current_chapter - 1) +
      L"]; if (el) el.scrollIntoView({ block: 'start' }); })()";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
  NoteActivity();
}

// Whole-book search over the prefetched chapter texts (mac runEpubFind).
void MainWindow::RunEpubFind(std::wstring const& query) {
  if (!epub_) return;
  auto& book = *epub_;
  epub_hits_.clear();
  if (query.empty()) {
    FindCount().Text(L"");
    ClearEpubHighlights();
    return;
  }
  std::wstring const needle = query;
  auto const lower_of = [](std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(::towlower(c));
    return text;
  };
  std::wstring const haystack_needle = lower_of(needle);
  for (int i = 1; i <= std::max(1, book.chapters); ++i) {
    auto const found = book.text_cache.find(i);
    if (found == book.text_cache.end()) continue;
    std::wstring const text = lower_of(found->second);
    std::size_t at = 0;
    int ordinal = 0;
    while ((at = text.find(haystack_needle, at)) != std::wstring::npos) {
      epub_hits_.push_back({i, ordinal});
      ++ordinal;
      at += haystack_needle.size();
    }
  }
  FindCount().Text(epub_hits_.empty()
                       ? L"0"
                       : L"1/" + std::to_wstring(epub_hits_.size()));
  epub_find_index_ = 0;
  if (!epub_hits_.empty()) {
    HighlightEpubHit(epub_hits_.front().first, epub_hits_.front().second, needle);
  }
}

void MainWindow::NavigateEpubFind(int step) {
  if (!epub_ || epub_hits_.empty()) return;
  epub_find_index_ = (epub_find_index_ + step +
                      static_cast<int>(epub_hits_.size())) %
                     static_cast<int>(epub_hits_.size());
  auto const& hit = epub_hits_[static_cast<std::size_t>(epub_find_index_)];
  FindCount().Text(std::to_wstring(epub_find_index_ + 1) + L"/" +
                   std::to_wstring(epub_hits_.size()));
  HighlightEpubHit(hit.first, hit.second, std::wstring(FindInput().Text()));
}

void MainWindow::HighlightEpubHit(int chapter, int ordinal,
                                  std::wstring const& query) {
  if (!epub_) return;
  auto& book = *epub_;
  GoToChapter(book, chapter);
  if (!book.web || !book.web.CoreWebView2()) return;
  auto const script =
      L"(() => { const els = document.querySelectorAll('.epub-chapter');"
      L" const root = els[" +
      std::to_wstring(chapter - 1) +
      L"]; if (!root) return; clearMarks(root);"
      L" markOccurrence(root, " +
      js_string(query) + L", " + std::to_wstring(ordinal) +
      L");"
      L" const mark = root.querySelector('mark.epub-hit');"
      L" if (mark) mark.scrollIntoView({ block: 'center' }); })()";
  try {
    book.web.CoreWebView2().ExecuteScriptAsync(script);
  } catch (...) {
  }
}

void MainWindow::ClearEpubHighlights() {
  if (!epub_ || !epub_->web || !epub_->web.CoreWebView2()) return;
  try {
    epub_->web.CoreWebView2().ExecuteScriptAsync(
        L"document.querySelectorAll('.epub-chapter').forEach(el => clearMarks(el));");
  } catch (...) {
  }
}

// ---- TOC (left panel) ---------------------------------------------------------------------

void MainWindow::RenderEpubToc(EpubBook& book) {
  auto const host = LeftContent();

  // recents stay in the welcome overlay + 打开最近 flyout — TOC only here
  auto content = muxc::StackPanel();
  content.Spacing(1);
  if (book.toc.empty()) {
    auto empty = muxc::TextBlock();
    empty.Text(pdfgist::Tr("ui.epub.toc-empty"));
    empty.FontSize(11);
    empty.Opacity(0.5);
    empty.Margin({4, 8, 0, 0});
    content.Children().Append(empty);
    for (int i = 1; i <= std::max(1, book.chapters); ++i) {
      rivet_app::EpubTocItem chapter_item{};
      chapter_item.title = to_utf8(
          pdfgist::Trf("ui.epub.chapter-n", std::to_wstring(i)));
      chapter_item.chapter = i;
      chapter_item.level = 0;
      book.toc.push_back(chapter_item);
    }
  }
  auto const weak = get_weak();
  for (auto const& toc_item : book.toc) {
    auto button = muxc::Button();
    button.Padding({2, 4, 4, 4});
    button.MinWidth(0);
    button.MinHeight(0);
    button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    button.Margin({static_cast<double>(10 + toc_item.level * 12), 0, 0, 0});
    muxc::TextBlock label;
    label.Text(to_wide(toc_item.title));
    label.FontSize(12);
    label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    button.Content(label);
    int const chapter = static_cast<int>(toc_item.chapter);
    button.Foreground(ThemeBrush(
        book.current_chapter == chapter
            ? wu::Color{0xFF, 0xB4, 0x53, 0x09}
            : (Root().ActualTheme() == mux::ElementTheme::Dark
                   ? wu::Color{0xFF, 0xE7, 0xE5, 0xE4}
                   : wu::Color{0xFF, 0x29, 0x25, 0x24})));
    button.Click([weak, chapter](winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        if (window->epub_) {
          window->GoToChapter(*window->epub_, chapter);
        }
      }
    });
    content.Children().Append(button);
  }

  host.Children().Clear();
  host.Children().Append(content);
}

// ---- html ----------------------------------------------------------------------------------

std::wstring MainWindow::BuildEpubHtml(
    EpubBook const& book,
    std::vector<std::pair<int, std::wstring>> const& sections) {
  // v1 READING_CSS typography + theme; the marks helpers power whole-book
  // search; the bridge posts to window.chrome.webview (WebView2).
  std::wstring html = L"<!doctype html><html><head><meta charset=\"utf-8\">";
  html += LR"(
<style>
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
  font-size: calc(16px * var(--fs)); line-height: 1.9; }
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
   manipulation; UI chrome uses the UI font stack and the reading-theme
   surface pair, never the page's serif. */
.selbar { position: absolute; display: none; align-items: center; gap: 4px;
  padding: 5px 7px; border-radius: 8px; background: var(--bar-bg);
  border: 1px solid var(--bar-line); box-shadow: 0 4px 14px rgba(23,20,18,.16);
  z-index: 5; font-family: system-ui, "Segoe UI", "Microsoft YaHei", sans-serif; }
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
</style>
<script>
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
function post(message) {
  if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(message);
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
  post(JSON.stringify({ kind: 'selection', text: text.slice(0, 800),
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
    post(JSON.stringify({ kind: 'sel-action', action: t.dataset.a }));
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
  post(JSON.stringify({ kind: 'chapter', chapter: current }));
});
</script>
</head><body>)";
  html += L"<div class=\"epub-book-title\"><div class=\"book-title-text\">" +
          escape_html(book.title) + L"</div></div>";
  for (auto const& [index, body] : sections) {
    (void)index;
    html += L"<div class=\"epub-chapter\">" + body + L"</div>";
  }
  // Selection bar labels come from the i18n table; the bar is inert chrome —
  // every action routes to the host over the bridge.
  html += std::wstring(L"<div class=\"selbar\" id=\"selbar\">") +
          L"<button class=\"sw sw-y\" data-a=\"hl-yellow\" title=\"" +
          escape_html(pdfgist::Tr("ui.menu.highlight-yellow")) + L"\"></button>" +
          L"<button class=\"sw sw-b\" data-a=\"hl-blue\" title=\"" +
          escape_html(pdfgist::Tr("ui.menu.highlight-blue")) + L"\"></button>" +
          L"<button class=\"sw sw-g\" data-a=\"hl-green\" title=\"" +
          escape_html(pdfgist::Tr("ui.menu.highlight-green")) + L"\"></button>" +
          L"<span class=\"sep\"></span>" +
          L"<span class=\"act\" data-a=\"underline\">" +
          escape_html(pdfgist::Tr("ui.menu.underline")) + L"</span>" +
          L"<span class=\"act\" data-a=\"strike\">" +
          escape_html(pdfgist::Tr("ui.menu.strike")) + L"</span>" +
          L"<span class=\"sep\"></span>" +
          L"<span class=\"act\" data-a=\"copy\">" +
          escape_html(pdfgist::Tr("ui.card.copy")) + L"</span>" +
          L"<span class=\"act ai\" data-a=\"ai\">&#10022; " +
          escape_html(pdfgist::Tr("ui.toolbar.ai")) + L"</span>" +
          L"</div>";
  html += L"</body></html>";
  return html;
}


// ---- EPUB annotations (chapter+offset anchored; marks live in the page) ----

void MainWindow::SaveEpubDocDataNow() {
  auto book = epub_;
  if (!book || !ready_ || !api_ || !backend_) return;
  if (!book->anno_dirty && !book->doc_data_loaded) return;
  std::string const json = SerializeDocData(book->doc_data);
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path = to_utf8(book->path);
  bool const sidecar = settings_.annotation_sidecar;
  auto const revision = book->anno_revision;
  book->anno_save_phase = AnnotationSavePhase::Pending;
  std::thread([weak, dispatcher, backend, book, path, sidecar, json,
               revision]() mutable {
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
    dispatcher.TryEnqueue([weak, book, revision,
                           error = std::move(error)]() mutable {
      auto window = weak.get();
      if (!window || book->anno_revision != revision) return;
      book->anno_dirty = !error.empty();
      book->anno_save_phase = error.empty() ? AnnotationSavePhase::Saved
                                            : AnnotationSavePhase::Failed;
      book->anno_save_error = std::move(error);
      if (window->epub_.get() == book.get() && window->left_mode_ == 2) {
        window->RefreshLeftPanel();
      }
    });
  }).detach();
}

void MainWindow::ApplyEpubAnnotations(EpubBook& book) {
  if (!book.web || !book.web.CoreWebView2()) return;
  for (auto const& annotation : book.doc_data.annotations) {
    if (!annotation.anchor) continue;
    auto const js = L"applyEpubAnnotation(" + js_string(annotation.id) +
                    L"," + std::to_wstring(annotation.anchor->chapter) +
                    L"," + std::to_wstring(annotation.anchor->start) +
                    L"," + std::to_wstring(annotation.anchor->end) +
                    L"," + js_string(annotation.kind) +
                    L"," + js_string(annotation.color) + L");";
    try {
      book.web.CoreWebView2().ExecuteScriptAsync(js);
    } catch (...) {
    }
  }
}

void MainWindow::AddEpubAnnotation(std::wstring const& color,
                                   std::wstring const& kind) {
  auto book = epub_;
  if (!book || book->selection_text.empty() || !book->selection_chapter) return;
  auto id = L"e-" + std::to_wstring(static_cast<long long>(std::time(nullptr))) +
            L"-" + std::to_wstring(std::rand() % 10000);
  StoredAnnotation annotation;
  annotation.id = id;
  annotation.page = book->selection_chapter;
  annotation.excerpt = book->selection_text.substr(0, 800);
  annotation.color = color;
  annotation.kind = kind;
  annotation.created = static_cast<std::int64_t>(std::time(nullptr));
  annotation.anchor = StoredAnchor{book->selection_chapter,
                                   book->selection_start,
                                   book->selection_end};
  book->doc_data.annotations.push_back(annotation);
  book->anno_dirty = true;
  auto const js = L"applyEpubAnnotation(" + js_string(id) +
                  L"," + std::to_wstring(book->selection_chapter) +
                  L"," + std::to_wstring(book->selection_start) +
                  L"," + std::to_wstring(book->selection_end) +
                  L"," + js_string(kind) + L"," + js_string(color) + L");";
  try {
    book->web.CoreWebView2().ExecuteScriptAsync(js);
  } catch (...) {
  }
  QueueAnnotationSave();
}

void MainWindow::RemoveEpubAnnotation(std::wstring const& id) {
  auto book = epub_;
  if (!book) return;
  auto& annotations = book->doc_data.annotations;
  auto const it = std::find_if(annotations.begin(), annotations.end(),
                               [&id](StoredAnnotation const& a) {
                                 return a.id == id;
                               });
  if (it == annotations.end()) return;
  book->undo_index = static_cast<std::size_t>(it - annotations.begin());
  book->undo_annotation = *it;
  annotations.erase(it);
  book->anno_dirty = true;
  try {
    book->web.CoreWebView2().ExecuteScriptAsync(
        L"removeEpubAnnotation(" + js_string(id) + L");");
  } catch (...) {
  }
  QueueAnnotationSave();
  RefreshLeftPanel();
  undo_timer_.Stop();
  undo_timer_.Start();
}

void MainWindow::UndoDeleteEpubAnnotation() {
  auto book = epub_;
  undo_timer_.Stop();
  if (!book || !book->undo_annotation) {
    if (book) book->undo_annotation.reset();
    return;
  }
  auto const index =
      std::min(book->undo_index, book->doc_data.annotations.size());
  auto annotation = *book->undo_annotation;
  book->doc_data.annotations.insert(
      book->doc_data.annotations.begin() + static_cast<std::ptrdiff_t>(index),
      annotation);
  book->undo_annotation.reset();
  book->anno_dirty = true;
  if (annotation.anchor) {
    try {
      book->web.CoreWebView2().ExecuteScriptAsync(
          L"applyEpubAnnotation(" + js_string(annotation.id) +
          L"," + std::to_wstring(annotation.anchor->chapter) +
          L"," + std::to_wstring(annotation.anchor->start) +
          L"," + std::to_wstring(annotation.anchor->end) +
          L"," + js_string(annotation.kind) +
          L"," + js_string(annotation.color) + L");");
    } catch (...) {
    }
  }
  QueueAnnotationSave();
  RefreshLeftPanel();
}

}  // namespace winrt::RivetHost::implementation
