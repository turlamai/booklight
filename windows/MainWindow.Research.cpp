// 研究工作台（design/prototype/research.html）— 跨书检索 + 可溯源引用集。
// Free feature, no gating (PRICING.md 2026-11-18 owner decision). Search
// runs host-side over the text layers each surface already owns (PDFium
// PageText for PDFs, the EPUB chapter cache for open books, on-demand
// extraction for not-open recents). Pinned citations are `kind: "cite"`
// entries in the same per-document annotation store, so save, recovery
// and export need no new storage.

#include "pch.h"
#include "MainWindow.xaml.h"
#include "I18n.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <algorithm>
#include <fstream>
#include <iterator>

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxd = winrt::Microsoft::UI::Xaml::Documents;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;
using Microsoft::UI::Xaml::Visibility;
// implementation only — opening winrt::RivetHost too makes MainWindow
// ambiguous (the projected type lives there).
using namespace winrt::RivetHost::implementation;

namespace {
struct ResearchTargetUi {
    std::wstring path;
    std::wstring title;
    std::wstring kind;  // L"pdf" | L"epub"
    bool is_open = false;
    bool selected = false;
};
struct ResearchHitUi {
    std::wstring path;
    std::wstring title;
    std::wstring kind;
    int page = 0;  // 1-based page, chapter ordinal for EPUBs
    std::wstring excerpt;
    std::size_t match_start = 0;
    std::size_t match_len = 0;
};
struct ResearchCiteUi {
    std::wstring path;
    std::wstring title;
    std::wstring kind;
    int page = 0;
    std::wstring excerpt;
    std::wstring note;
    std::wstring anno_id;
};

constexpr int kPerDocumentCap = 20;
constexpr int kTotalCap = 50;
constexpr int kExcerptBefore = 40;
constexpr int kExcerptAfter = 80;
constexpr int kRecentsDeckLimit = 8;

std::wstring fold_text(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(::towlower(c));
    return text;
}
std::wstring file_title(std::wstring const& path) {
    std::size_t const slash = path.find_last_of(L"/\\");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
std::wstring kind_of(std::wstring const& path) {
    return path.size() >= 5 &&
                   _wcsnicmp(path.c_str() + path.size() - 5, L".epub", 5) == 0
               ? L"epub"
               : L"pdf";
}
// Window the excerpt ±40/80 characters around the match (CJK-safe: index
// space, not byte space), then report the match position inside it.
bool excerpt_window(std::wstring const& text, std::size_t at,
                    std::size_t needle_len, std::wstring& excerpt,
                    std::size_t& match_start, std::size_t& match_len) {
    std::size_t const start = at > static_cast<std::size_t>(kExcerptBefore)
                                  ? at - kExcerptBefore
                                  : 0;
    std::size_t const end = std::min(text.size(), at + needle_len + kExcerptAfter);
    if (end <= start) return false;
    excerpt = text.substr(start, end - start);
    // Trim and re-locate the match inside the trimmed excerpt (folded
    // search again — trimming only removed whitespace at the edges).
    std::size_t const first = excerpt.find_first_not_of(L" \t\r\n");
    std::size_t const last = excerpt.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    excerpt = excerpt.substr(first, last - first + 1);
    std::size_t const rel = at - start;
    match_start = rel > first ? rel - first : 0;
    match_len = std::min(needle_len, excerpt.size() - match_start);
    return true;
}
}  // namespace

// ---- entry / visibility ----------------------------------------------------------------

void MainWindow::ToolsResearch_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
    ShowResearch();
}

void MainWindow::ShowResearch() {
    BuildResearchScope();
    RefreshResearchDeck();
    // Layout keeps the pending-edits banner in sync.
    ResearchView().Visibility(Visibility::Visible);
    CenterHostTabs().Visibility(Visibility::Collapsed);
    WelcomeView().Visibility(Visibility::Collapsed);
    research_visible_ = true;
    UpdateChrome();
    SetStatus(true, pdfgist::Tr("ui.research.subtitle"));
}

void MainWindow::HideResearch() {
    ResearchView().Visibility(Visibility::Collapsed);
    CenterHostTabs().Visibility(Visibility::Visible);
    research_visible_ = false;
    UpdateChrome();
}

// ---- scope -----------------------------------------------------------------------------

void MainWindow::BuildResearchScope() {
    research_targets_.clear();
    for (auto const& tab : tabs_) {
        research_targets_.push_back({tab->path, tab->title, L"pdf",
                                     true, true});
    }
    if (epub_ && epub_active_) {
        research_targets_.push_back(
            {epub_->path, epub_->title, L"epub", true, true});
    }
    for (auto const& entry : recents_) {
        std::wstring const path = to_wide(entry.path);
        bool const open = std::any_of(
            research_targets_.begin(), research_targets_.end(),
            [&](ResearchTargetUi const& t) { return t.path == path; });
        if (!open) {
            research_targets_.push_back(
                {path, file_title(path), kind_of(path), false, false});
        }
    }

    ResearchScopeTitle().Text(pdfgist::Tr("ui.research.scope"));
    ResearchOpenedTitle().Text(pdfgist::Trf(
        "ui.research.opened",
        std::to_wstring(static_cast<int>(std::count_if(
            research_targets_.begin(), research_targets_.end(),
            [](ResearchTargetUi const& t) { return t.is_open; })))));
    ResearchRecentsTitle().Text(pdfgist::Tr("ui.research.recent-docs"));
    ResearchScopeFooter().Text(pdfgist::Tr("ui.research.footer"));
    ResearchQuery().PlaceholderText(pdfgist::Tr("ui.research.query-placeholder"));
    ResearchSearchButton().Content(
        winrt::box_value(pdfgist::Tr("ui.research.search")));
    ResearchBusyBar().Message(pdfgist::Trf("ui.research.searching", L""));
    ResearchFooter().Text(pdfgist::Tr("ui.research.footer"));

    BuildResearchScopeList(ResearchOpenedList(),
                           /*recents=*/false);
    BuildResearchScopeList(ResearchRecentsList(),
                           /*recents=*/true);
}

void MainWindow::BuildResearchScopeList(
    muxc::StackPanel const& host, bool recents) {
    host.Children().Clear();
    auto const weak = get_weak();
    for (auto const& target : research_targets_) {
        if (target.is_open == recents) continue;
        auto row = muxc::Button();
        row.Padding({4, 4, 4, 4});
        row.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        row.Background(media::SolidColorBrush());
        auto stack = muxc::StackPanel();
        stack.Orientation(muxc::Orientation::Horizontal);
        stack.Spacing(8);
        muxc::TextBlock mark;
        mark.Text(target.is_open ? L"\u2713" : L"\uFF0B");  // ✓ / ＋
        mark.FontSize(11);
        mark.FontWeight(wut::FontWeights::SemiBold());
        mark.Foreground(ThemeBrush(target.is_open ? AccentColor()
                                                  : wu::Color{0xFF, 0x8A, 0x84, 0x7C}));
        stack.Children().Append(mark);
        muxc::TextBlock label;
        label.Text(target.title);
        label.FontSize(12.5);
        label.MaxWidth(190);
        label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        label.Foreground(ThemeBrush(
            target.is_open ? wu::Color{0xFF, 0x29, 0x25, 0x24}
                           : wu::Color{0xFF, 0x6F, 0x68, 0x60}));
        stack.Children().Append(label);
        row.Content(stack);
        if (!target.is_open) {
            ResearchTargetUi const snapshot = target;
            row.Click([weak, snapshot](
                          winrt::Windows::Foundation::IInspectable const&,
                          mux::RoutedEventArgs const&) {
                if (auto window = weak.get()) {
                    window->ToggleResearchScope(snapshot.path);
                }
            });
        }
        host.Children().Append(row);
    }
}

void MainWindow::ToggleResearchScope(std::wstring const& path) {
    for (auto& target : research_targets_) {
        if (target.path == path) target.selected = !target.selected;
    }
    // Re-render both lists (open rows never toggle, but the list pair is
    // rebuilt together for simplicity).
    BuildResearchScopeList(ResearchOpenedList(), false);
    BuildResearchScopeList(ResearchRecentsList(), true);
}

// ---- search ----------------------------------------------------------------------------

void MainWindow::ResearchQuery_KeyDown(
    winrt::Windows::Foundation::IInspectable const&,
    mux::Input::KeyRoutedEventArgs const& args) {
    if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
        RunResearchSearch();
    }
}

void MainWindow::ResearchSearch_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
    RunResearchSearch();
}

void MainWindow::RunResearchSearch() {
    std::wstring const needle =
        ResearchQuery().Text().size() > 200
            ? ResearchQuery().Text().substr(0, 200)
            : ResearchQuery().Text();
    auto const folded_needle = fold_text(needle);
    research_hits_.clear();
    ResearchResults().Children().Clear();
    ResearchStats().Text(L"");
    ResearchDeckPane().Visibility(Visibility::Collapsed);

    bool const any_needle = !folded_needle.empty();
    std::vector<ResearchTargetUi> scope;
    for (auto const& target : research_targets_) {
        if (target.selected ||
            (target.is_open && !any_needle)) {
            if (!folded_needle.empty() || target.is_open) scope.push_back(target);
        }
    }
    if (needle.empty() || scope.empty()) {
        RenderResearchEmpty(!needle.empty());
        return;
    }

    ResearchBusyBar().Message(
        pdfgist::Trf("ui.research.searching",
                     std::to_wstring(static_cast<int>(scope.size()))));
    ResearchBusyBar().IsOpen(true);
    auto const dispatcher = DispatcherQueue();
    auto const weak = get_weak();
    auto const started = std::chrono::steady_clock::now();
    std::thread([weak, dispatcher, scope, folded_needle, needle,
                 started]() mutable {
        auto const lower_of = [](std::wstring text) {
            for (auto& c : text) c = static_cast<wchar_t>(::towlower(c));
            return text;
        };
        std::vector<ResearchHitUi> hits;
        for (auto const& target : scope) {
            auto const units = ResearchTextUnits(target);
            int per_doc = 0;
            for (auto const& [page, text] : units) {
                if (per_doc >= kPerDocumentCap || hits.size() >= kTotalCap) break;
                std::wstring const folded = lower_of(text);
                std::size_t at = folded.find(folded_needle);
                while (at != std::wstring::npos &&
                       per_doc < kPerDocumentCap && hits.size() < kTotalCap) {
                    std::wstring excerpt;
                    std::size_t match_start = 0;
                    std::size_t match_len = 0;
                    if (excerpt_window(text, at, needle.size(), excerpt,
                                       match_start, match_len)) {
                        hits.push_back({target.path, target.title, target.kind,
                                        page, excerpt, match_start, match_len});
                        ++per_doc;
                    }
                    if (at + needle.size() >= folded.size()) break;
                    at = folded.find(folded_needle, at + needle.size());
                }
            }
        }
        dispatcher.TryEnqueue([weak, hits = std::move(hits), count = scope.size(),
                               started]() mutable {
            if (auto window = weak.get()) {
                window->RenderResearchResults(std::move(hits), count, started);
            }
        });
    }).detach();
}

// (page-or-chapter, text) units for one target — open documents read
// their existing layers; not-open recents extract on demand.
std::vector<std::pair<int, std::wstring>> MainWindow::ResearchTextUnits(
    ResearchTargetUi const& target) {
    std::vector<std::pair<int, std::wstring>> units;
    if (target.is_open) {
        if (target.kind == L"epub") {
            if (epub_ && epub_->path == target.path) {
                for (auto const& [chapter, text] : epub_->text_cache) {
                    units.emplace_back(chapter, text);
                }
            }
        } else if (auto const tab = pdf_tab_by_path(target.path);
                   tab && tab->doc) {
            for (int i = 0; i < tab->doc->PageCount(); ++i) {
                auto const text = to_wide(tab->doc->PageText(i));
                if (!text.empty()) units.emplace_back(i + 1, text);
            }
        }
        return units;
    }
    if (target.kind == L"epub") {
        if (!api_) return units;
        try {
            rivet_app::API api(*backend_);
            auto const view = api.epub_open(to_utf8(target.path)).get();
            int const count = static_cast<int>(std::max<std::int64_t>(view.chapters, 1));
            for (int chapter = 1; chapter <= count; ++chapter) {
                auto const text = to_wide(
                    api.epub_chapter_text(to_utf8(target.path), chapter).get());
                if (!text.empty()) units.emplace_back(chapter, text);
            }
        } catch (...) {
        }
        return units;
    }
    std::ifstream file(target.path, std::ios::binary);
    if (!file) return units;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                    std::istreambuf_iterator<char>()};
    pdfgist::PdfOpenError error = pdfgist::PdfOpenError::None;
    if (auto const doc = pdfgist::PdfDocument::OpenFromBytes(bytes, "", &error);
        doc && error == pdfgist::PdfOpenError::None) {
        for (int i = 0; i < doc->PageCount(); ++i) {
            auto const text = to_wide(doc->PageText(i));
            if (!text.empty()) units.emplace_back(i + 1, text);
        }
    }
    return units;
}

void MainWindow::RenderResearchResults(
    std::vector<ResearchHitUi> hits, int document_count,
    std::chrono::steady_clock::time_point started) {
    ResearchBusyBar().IsOpen(false);
    research_hits_ = hits;
    ResearchResults().Children().Clear();
    double const seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    auto const elapsed = std::to_wstring(
        static_cast<int>(seconds * 10 + 0.5));
    ResearchStats().Text(hits.empty() && !any_research_scope()
                             ? L""
                             : pdfgist::Trf("ui.research.stats",
                                            std::to_wstring(document_count),
                                            std::to_wstring(static_cast<int>(hits.size())),
                                            elapsed.substr(0, elapsed.size() - 1) + L"." +
                                                elapsed.substr(elapsed.size() - 1)));
    if (hits.empty()) {
        RenderResearchEmpty(true);
        return;
    }
    // Group by document (prototype .rdoc headers).
    std::vector<std::wstring> order;
    std::map<std::wstring, std::vector<ResearchHitUi>> buckets;
    for (auto const& hit : hits) {
        if (!buckets.count(hit.path)) order.push_back(hit.path);
        buckets[hit.path].push_back(hit);
    }
    auto const weak = get_weak();
    for (auto const& path : order) {
        auto const& group = buckets[path];
        auto header = muxc::TextBlock();
        header.Text(pdfgist::Trf("ui.research.hits", group.front().title,
                                 std::to_wstring(static_cast<int>(group.size()))));
        header.FontSize(12);
        header.FontWeight(wut::FontWeights::Bold());
        header.Opacity(0.7);
        header.Margin({0, 10, 0, 2});
        ResearchResults().Children().Append(header);
        for (auto const& hit : group) {
            ResearchResults().Children().Append(BuildResearchCard(hit));
        }
    }
}

muxc::Border MainWindow::BuildResearchCard(ResearchHitUi const& hit) {
    auto border = muxc::Border();
    border.Padding({10, 10, 10, 10});
    border.CornerRadius({8, 8, 8, 8});
    border.BorderThickness({1, 1, 1, 1});
    border.BorderBrush(ThemeBorderBrush());
    border.Background(ThemePanelBrush());
    auto stack = muxc::StackPanel();
    stack.Spacing(8);

    // Excerpt with the highlighted match (prototype <mark>).
    muxc::TextBlock excerpt;
    excerpt.FontSize(13.5);
    excerpt.TextWrapping(mux::TextWrapping::Wrap);
    excerpt.IsTextSelectionEnabled(true);
    if (hit.match_start <= hit.excerpt.size() &&
        hit.match_start + hit.match_len <= hit.excerpt.size()) {
        muxc::RichTextBlock rich;
        muxd::Paragraph paragraph;
        auto add_run = [&](std::wstring const& text, bool highlight) {
            muxd::Run run;
            run.Text(text);
            if (highlight) {
                run.Background(ThemeBrush(
                    wu::Color{0x33, 0xE8, 0x92, 0x6A}));  // accent-soft
            }
            paragraph.Inlines().Append(run);
        };
        add_run(hit.excerpt.substr(0, hit.match_start), false);
        add_run(hit.excerpt.substr(hit.match_start, hit.match_len), true);
        add_run(hit.excerpt.substr(hit.match_start + hit.match_len), false);
        rich.Blocks().Append(paragraph);
        stack.Children().Append(rich);
    } else {
        excerpt.Text(hit.excerpt);
        stack.Children().Append(excerpt);
    }

    auto row = muxc::StackPanel();
    row.Orientation(muxc::Orientation::Horizontal);
    row.Spacing(6);
    auto const weak = get_weak();
    auto const location =
        hit.kind == L"epub"
            ? pdfgist::Trf("ui.epub.chapter-n", std::to_wstring(hit.page))
            : pdfgist::Trf("ui.notes.page", std::to_wstring(hit.page));
    auto chip = muxc::Button();
    chip.Padding({9, 2, 9, 2});
    chip.CornerRadius({99, 99, 99, 99});
    chip.Background(ThemeBrush(wu::Color{0x1F, 0xC4, 0x62, 0x3E}));  // accent-soft
    muxc::TextBlock chip_text;
    chip_text.Text(hit.title + L" · " + location);
    chip_text.FontSize(11);
    chip_text.FontWeight(wut::FontWeights::SemiBold());
    chip_text.Foreground(ThemeBrush(AccentColor()));
    chip.Content(chip_text);
    ResearchHitUi const snapshot = hit;
    chip.Click([weak, snapshot](
                   winrt::Windows::Foundation::IInspectable const&,
                   mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) window->JumpToResearchHit(snapshot);
    });
    row.Children().Append(chip);
    row.Children().Append(research_card_button(
        L"ui.research.jump", false, [weak, snapshot] {
            if (auto window = weak.get()) window->JumpToResearchHit(snapshot);
        }));
    row.Children().Append(research_card_button(
        L"ui.research.cite", false, [weak, snapshot] {
            if (auto window = weak.get()) window->CopyResearchCitation(snapshot);
        }));
    bool const pinned = research_cite_pinned(snapshot);
    row.Children().Append(research_card_button(
        pinned ? L"ui.research.pinned" : L"ui.research.pin", pinned,
        [weak, snapshot] {
            if (auto window = weak.get()) window->ToggleResearchPin(snapshot);
        }));
    stack.Children().Append(row);

    border.Child(stack);
    return border;
}

muxc::Button MainWindow::research_card_button(
    std::wstring const& key, bool emphasized,
    std::function<void()> action) {
    auto button = muxc::Button();
    button.Padding({9, 3, 9, 3});
    button.Background(emphasized ? ThemeBrush(wu::Color{0x1F, 0xC4, 0x62, 0x3E})
                                 : media::SolidColorBrush());
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(to_utf8(key).c_str()));
    label.FontSize(11);
    if (emphasized) label.FontWeight(wut::FontWeights::SemiBold());
    label.Foreground(ThemeBrush(emphasized ? AccentColor()
                                           : wu::Color{0xFF, 0x6F, 0x68, 0x60}));
    button.Content(label);
    button.Click([action](winrt::Windows::Foundation::IInspectable const&,
                          mux::RoutedEventArgs const&) { action(); });
    return button;
}

bool MainWindow::any_research_scope() const {
    return std::any_of(research_targets_.begin(), research_targets_.end(),
                       [](ResearchTargetUi const& t) { return t.selected || t.is_open; });
}

void MainWindow::RenderResearchEmpty(bool searched) {
    auto panel = muxc::StackPanel();
    panel.HorizontalAlignment(mux::HorizontalAlignment::Center);
    panel.Margin({0, 60, 0, 0});
    muxc::TextBlock title;
    title.Text(pdfgist::Tr(research_targets_.empty()
                               ? "ui.research.no-docs"
                               : "ui.research.no-result-title"));
    title.FontSize(14);
    title.FontWeight(wut::FontWeights::SemiBold());
    title.HorizontalAlignment(mux::HorizontalAlignment::Center);
    panel.Children().Append(title);
    muxc::TextBlock hint;
    hint.Text(pdfgist::Tr(research_targets_.empty()
                              ? "ui.research.no-docs-hint"
                              : "ui.research.no-result-hint"));
    hint.FontSize(12.5);
    hint.Opacity(0.7);
    hint.TextAlignment(mux::TextAlignment::Center);
    hint.TextWrapping(mux::TextWrapping::Wrap);
    hint.MaxWidth(420);
    hint.HorizontalAlignment(mux::HorizontalAlignment::Center);
    hint.Margin({0, 8, 0, 0});
    panel.Children().Append(hint);
    ResearchResults().Children().Append(panel);
}

// ---- citation deck ---------------------------------------------------------------------

void MainWindow::RefreshResearchDeck() {
    research_cites_.clear();
    auto collect = [&](std::wstring const& path, std::wstring const& title,
                       std::wstring const& kind, bool is_open,
                       DocData const& data) {
        for (auto const& annotation : data.annotations) {
            if (annotation.kind != L"cite") continue;
            research_cites_.push_back({path, title, kind, annotation.page,
                                       annotation.excerpt, annotation.note,
                                       annotation.id});
        }
    };
    for (auto const& tab : tabs_) {
        collect(tab->path, tab->title, L"pdf", true, tab->doc_data);
    }
    if (epub_) {
        collect(epub_->path, epub_->title, L"epub", true, epub_->doc_data);
    }
    auto const weak = get_weak();
    auto const dispatcher = DispatcherQueue();
    auto const backend = backend_;
    auto const sidecar = settings_.annotation_sidecar;
    auto const recents = recents_;
    std::thread([weak, dispatcher, backend, sidecar, recents,
                 collected = research_cites_]() mutable {
        // Not-open recents read their store through the RPC.
        for (std::size_t i = 0;
             i < recents.size() &&
             static_cast<int>(collected.size()) < kTotalCap &&
             i < static_cast<std::size_t>(kRecentsDeckLimit);
             ++i) {
            std::wstring const path = to_wide(recents[i].path);
            bool const open = std::any_of(
                collected.begin(), collected.end(),
                [&](ResearchCiteUi const& c) { return c.path == path; });
            if (open) continue;
            try {
                rivet_app::API api(*backend);
                auto const bytes = api.get_annotations(recents[i].path,
                                                       sidecar).get();
                auto const window2 = weak.get();
                if (!window2) break;
                DocData doc;
                if (!window2->ParseDocData(std::string(bytes.begin(), bytes.end()), &doc)) break;
                for (auto const& annotation : doc.annotations) {
                    if (annotation.kind != L"cite") continue;
                    collected.push_back({path, file_title(path),
                                         kind_of(path), annotation.page,
                                         annotation.excerpt, annotation.note,
                                         annotation.id});
                }
            } catch (...) {
            }
        }
        dispatcher.TryEnqueue([weak, collected = std::move(collected)]() mutable {
            if (auto window = weak.get()) {
                window->RenderResearchDeck(std::move(collected));
            }
        });
    }).detach();
}

void MainWindow::RenderResearchDeck(std::vector<ResearchCiteUi> cites) {
    research_cites_ = cites;
    ResearchDeckList().Children().Clear();
    ResearchDeckPane().Visibility(cites.empty() ? Visibility::Collapsed
                                                : Visibility::Visible);
    ResearchDeckTitle().Text(pdfgist::Tr("ui.research.deck"));
    ResearchDeckCount().Text(
        pdfgist::Trf("ui.research.deck-count",
                     std::to_wstring(static_cast<int>(cites.size()))));
    ResearchDeckFooter().Text(pdfgist::Tr("ui.research.deck-footer"));
    auto const weak = get_weak();
    for (auto const& cite : cites) {
        auto border = muxc::Border();
        border.Padding({11, 11, 11, 11});
        border.CornerRadius({8, 8, 8, 8});
        border.BorderThickness({1, 1, 1, 1});
        border.BorderBrush(ThemeBorderBrush());
        border.Background(ThemePanelBrush());
        auto stack = muxc::StackPanel();
        stack.Spacing(7);
        muxc::TextBlock quote;
        quote.Text(L"「" + cite.excerpt + L"」");
        quote.FontSize(12.5);
        quote.TextWrapping(mux::TextWrapping::Wrap);
        quote.IsTextSelectionEnabled(true);
        stack.Children().Append(quote);
        auto location =
            cite.kind == L"epub"
                ? pdfgist::Trf("ui.epub.chapter-n", std::to_wstring(cite.page))
                : pdfgist::Trf("ui.notes.page", std::to_wstring(cite.page));
        muxc::TextBlock source;
        source.Text(L"《" + cite.title + L"》· " + location);
        source.FontSize(11);
        source.Opacity(0.7);
        stack.Children().Append(source);
        if (!cite.note.empty()) {
            muxc::TextBlock note;
            note.Text(cite.note);
            note.FontSize(11.5);
            note.Opacity(0.7);
            note.TextWrapping(mux::TextWrapping::Wrap);
            stack.Children().Append(note);
        }
        auto row = muxc::StackPanel();
        row.Orientation(muxc::Orientation::Horizontal);
        row.Spacing(6);
        ResearchCiteUi const snapshot = cite;
        row.Children().Append(research_card_button(
            L"ui.research.cite", false, [weak, snapshot] {
                if (auto window = weak.get()) window->CopyResearchCitation(snapshot);
            }));
        row.Children().Append(research_card_button(
            L"ui.notes.delete-annotation", false, [weak, snapshot] {
                if (auto window = weak.get()) window->UnpinResearchCite(snapshot);
            }));
        stack.Children().Append(row);
        border.Child(stack);
        ResearchDeckList().Children().Append(border);
    }
}

// ---- pin / unpin / copy / jump ---------------------------------------------------------

bool MainWindow::research_cite_pinned(ResearchHitUi const& hit) const {
    return std::any_of(research_cites_.begin(), research_cites_.end(),
                       [&](ResearchCiteUi const& c) {
                           return c.path == hit.path && c.page == hit.page &&
                                  c.excerpt == hit.excerpt;
                       });
}

void MainWindow::ToggleResearchPin(ResearchHitUi const& hit) {
    if (research_cite_pinned(hit)) {
        // Remove the matching cite entry.
        if (auto const tab = pdf_tab_by_path(hit.path); tab) {
            auto& annotations = tab->doc_data.annotations;
            annotations.erase(
                std::remove_if(annotations.begin(), annotations.end(),
                               [&](StoredAnnotation const& a) {
                                   return a.kind == L"cite" &&
                                          a.page == hit.page &&
                                          a.excerpt == hit.excerpt;
                               }),
                annotations.end());
            tab->anno_dirty = true;
            QueueAnnotationSave(tab.get());
        } else if (epub_ && epub_->path == hit.path) {
            auto& annotations = epub_->doc_data.annotations;
            annotations.erase(
                std::remove_if(annotations.begin(), annotations.end(),
                               [&](StoredAnnotation const& a) {
                                   return a.kind == L"cite" &&
                                          a.page == hit.page &&
                                          a.excerpt == hit.excerpt;
                               }),
                annotations.end());
            epub_->anno_dirty = true;
            QueueAnnotationSave();
        }
        RefreshResearchDeck();
        return;
    }
    PinResearchHit(hit);
}

void MainWindow::PinResearchHit(ResearchHitUi const& hit) {
    StoredAnnotation annotation;
    annotation.id = L"c-" +
                    std::to_wstring(static_cast<long long>(std::time(nullptr))) +
                    L"-" + std::to_wstring(std::rand() % 10000);
    annotation.page = hit.page;
    annotation.excerpt = hit.excerpt;
    annotation.kind = L"cite";
    annotation.created = static_cast<std::int64_t>(std::time(nullptr));
    if (auto const tab = pdf_tab_by_path(hit.path); tab) {
        tab->doc_data.annotations.push_back(annotation);
        tab->anno_dirty = true;
        QueueAnnotationSave(tab.get());
        RefreshResearchDeck();
        return;
    }
    if (epub_ && epub_->path == hit.path) {
        epub_->doc_data.annotations.push_back(annotation);
        epub_->anno_dirty = true;
        QueueAnnotationSave();
        RefreshResearchDeck();
        return;
    }
    // Not open: rewrite that document's store through the RPC. DocData
    // (de)serialization lives on the window (pure JSON, thread-safe).
    auto const backend = backend_;
    auto const dispatcher = DispatcherQueue();
    auto const weak = get_weak();
    auto const sidecar = settings_.annotation_sidecar;
    auto const path_utf8 = to_utf8(hit.path);
    std::thread([weak, dispatcher, backend, sidecar, path_utf8, hit,
                 annotation]() mutable {
        auto const window = weak.get();
        if (!window) return;
        try {
            rivet_app::API api(*backend);
            auto const bytes = api.get_annotations(path_utf8, sidecar).get();
            DocData doc;
            if (!window->ParseDocData(std::string(bytes.begin(), bytes.end()), &doc)) return;
            doc.annotations.push_back(annotation);
            auto const encoded = window->SerializeDocData(doc);
            (void)api.set_annotations(path_utf8, sidecar,
                                      rivet::Bytes(encoded.begin(), encoded.end()))
                .get();
        } catch (...) {
        }
        dispatcher.TryEnqueue([weak]() {
            if (auto window = weak.get()) window->RefreshResearchDeck();
        });
    }).detach();
}

void MainWindow::UnpinResearchCite(ResearchCiteUi const& cite) {
    if (auto const tab = pdf_tab_by_path(cite.path); tab) {
        auto& annotations = tab->doc_data.annotations;
        annotations.erase(std::remove_if(annotations.begin(), annotations.end(),
                                         [&](StoredAnnotation const& a) {
                                             return a.id == cite.anno_id;
                                         }),
                          annotations.end());
        tab->anno_dirty = true;
        QueueAnnotationSave(tab.get());
        RefreshResearchDeck();
        return;
    }
    if (epub_ && epub_->path == cite.path) {
        epub_->doc_data.annotations.erase(
            std::remove_if(epub_->doc_data.annotations.begin(),
                           epub_->doc_data.annotations.end(),
                           [&](StoredAnnotation const& a) {
                               return a.id == cite.anno_id;
                           }),
            epub_->doc_data.annotations.end());
        epub_->anno_dirty = true;
        QueueAnnotationSave();
        RefreshResearchDeck();
        return;
    }
    auto const backend = backend_;
    auto const dispatcher = DispatcherQueue();
    auto const weak = get_weak();
    auto const sidecar = settings_.annotation_sidecar;
    auto const path_utf8 = to_utf8(cite.path);
    auto const anno_id = to_utf8(cite.anno_id);
    std::thread([weak, dispatcher, backend, sidecar, path_utf8, anno_id]() mutable {
        auto const window = weak.get();
        if (!window) return;
        try {
            rivet_app::API api(*backend);
            auto const bytes = api.get_annotations(path_utf8, sidecar).get();
            DocData doc;
            if (!window->ParseDocData(std::string(bytes.begin(), bytes.end()), &doc)) return;
            doc.annotations.erase(
                std::remove_if(doc.annotations.begin(), doc.annotations.end(),
                               [&](StoredAnnotation const& a) { return a.id == anno_id; }),
                doc.annotations.end());
            auto const encoded = window->SerializeDocData(doc);
            (void)api.set_annotations(path_utf8, sidecar,
                                      rivet::Bytes(encoded.begin(), encoded.end()))
                .get();
        } catch (...) {
        }
        dispatcher.TryEnqueue([weak]() {
            if (auto window = weak.get()) window->RefreshResearchDeck();
        });
    }).detach();
}

void MainWindow::CopyResearchCitation(ResearchHitUi const& hit) {
    CopyResearchCitation(hit.excerpt, hit.title, hit.kind, hit.page);
}

void MainWindow::CopyResearchCitation(ResearchCiteUi const& cite) {
    CopyResearchCitation(cite.excerpt, cite.title, cite.kind, cite.page);
}

void MainWindow::CopyResearchCitation(std::wstring const& excerpt,
                                      std::wstring const& title,
                                      std::wstring const& kind, int page) {
    auto const location =
        kind == L"epub"
            ? pdfgist::Trf("ui.epub.chapter-n", std::to_wstring(page))
            : pdfgist::Trf("ui.notes.page", std::to_wstring(page));
    auto const citation =
        pdfgist::Trf("ui.research.cite-format", title, location, excerpt);
    try {
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(citation);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(
            package);
    } catch (...) {
    }
}

void MainWindow::JumpToResearchHit(ResearchHitUi const& hit) {
    JumpToResearchSource(hit.path, hit.kind, hit.page);
}

void MainWindow::JumpToResearchSource(std::wstring const& path,
                                      std::wstring const& kind, int page) {
    // Open documents navigate to the page/chapter in place; not-open ones
    // open through the existing OpenPath (resume at the source page).
    for (auto const& tab : tabs_) {
        if (tab->path == path) {
            ActivateTab(tab);
            if (kind != L"epub" && page >= 1) ScrollToPage(*tab, page);
            return;
        }
    }
    if (epub_ && epub_->path == path && epub_active_) {
        if (page >= 1) GoToChapter(*epub_, page);
        return;
    }
    OpenPath(path, kind == L"epub" ? std::max(page, 1) : std::max(page, 1));
    HideResearch();
}

std::shared_ptr<PdfTab> MainWindow::pdf_tab_by_path(std::wstring const& path) {
    for (auto const& tab : tabs_) {
        if (tab->path == path) return tab;
    }
    return nullptr;
}
