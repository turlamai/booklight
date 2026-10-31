#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxcp = winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

using Microsoft::UI::Xaml::Visibility;

// ---- sidebar chrome ------------------------------------------------------------------

void MainWindow::BuildAiSidebar() {
  ai_tab_buttons_.clear();
  chat_scope_buttons_.clear();
  AiTabRow().Children().Clear();

  char const* tabs[] = {"ui.tab.translate", "ui.tab.summarize",
                        "ui.tab.chat", "ui.tab.forms", "ui.tab.settings"};
  for (int i = 0; i < 5; ++i) {
    auto button = muxcp::ToggleButton();
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(tabs[i]));
    label.FontSize(12);
    button.Content(label);
    button.Padding({10, 4, 10, 4});
    button.MinWidth(0);
    button.MinHeight(0);
    button.IsChecked(i == 0);
    auto const weak = get_weak();
    int const index = i;
    button.Click([weak, index](winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->ShowAiPane(index);
    });
    ai_tab_buttons_.push_back(button);
    AiTabRow().Children().Append(button);
  }

  translate_list_ = muxc::StackPanel();
  translate_list_.Spacing(8);
  summarize_list_ = muxc::StackPanel();
  summarize_list_.Spacing(8);
  chat_list_ = muxc::StackPanel();
  chat_list_.Spacing(8);

  // ---- translate pane: page action + cards ----
  auto translate_pane = muxc::StackPanel();
  translate_pane.Spacing(8);
  auto translate_page = muxc::Button();
  muxc::TextBlock translate_page_label;
  translate_page_label.Text(pdfgist::Tr("ui.translate.page"));
  translate_page_label.FontSize(12);
  translate_page.Content(translate_page_label);
  translate_page.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  translate_page.Click([weak = get_weak()](
                           winrt::Windows::Foundation::IInspectable const&,
                           mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->LaunchTranslateCurrentPage();
  });
  auto translate_hint = muxc::TextBlock();
  translate_hint.Text(pdfgist::Tr("ui.welcome.feature1"));
  translate_hint.FontSize(11);
  translate_hint.Opacity(0.5);
  translate_pane.Children().Append(translate_page);
  translate_pane.Children().Append(translate_hint);
  translate_pane.Children().Append(translate_list_);

  // ---- summarize pane: three actions + cards ----
  auto summarize_pane = muxc::StackPanel();
  summarize_pane.Spacing(8);
  struct Action {
    char const* key;
    rivet_app::SummarizeMode mode;
  };
  Action const actions[] = {
      {"ui.summarize.page", rivet_app::SummarizeMode::page},
      {"ui.summarize.selection", rivet_app::SummarizeMode::selection},
      {"ui.summarize.doc", rivet_app::SummarizeMode::doc}};
  for (auto const& action : actions) {
    auto button = muxc::Button();
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(action.key));
    label.FontSize(12);
    button.Content(label);
    button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    auto const weak = get_weak();
    auto const mode = action.mode;
    button.Click([weak, mode](winrt::Windows::Foundation::IInspectable const&,
                              mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        switch (mode) {
          case rivet_app::SummarizeMode::page:
            window->LaunchSummarizePage();
            break;
          case rivet_app::SummarizeMode::selection:
            window->LaunchSummarizeSelection();
            break;
          case rivet_app::SummarizeMode::doc:
            window->LaunchSummarizeDoc();
            break;
        }
      }
    });
    summarize_pane.Children().Append(button);
  }
  auto summarize_hint = muxc::TextBlock();
  summarize_hint.Text(pdfgist::Tr("ui.welcome.feature1"));
  summarize_hint.FontSize(11);
  summarize_hint.Opacity(0.5);
  summarize_pane.Children().Append(summarize_hint);
  summarize_pane.Children().Append(summarize_list_);

  // ---- chat pane: scope row + bubbles (input bar lives at the panel bottom) ----
  auto chat_pane = muxc::StackPanel();
  chat_pane.Spacing(8);
  auto scope_row = muxc::StackPanel();
  scope_row.Orientation(muxc::Orientation::Horizontal);
  scope_row.Spacing(4);
  auto scope_label = muxc::TextBlock();
  scope_label.Text(pdfgist::Tr("ui.chat.scope"));
  scope_label.FontSize(11);
  scope_label.Opacity(0.7);
  scope_label.VerticalAlignment(mux::VerticalAlignment::Center);
  scope_row.Children().Append(scope_label);
  char const* scopes[] = {"ui.chat.scope.page", "ui.chat.scope.selection",
                          "ui.chat.scope.doc"};
  for (int i = 0; i < 3; ++i) {
    auto button = muxcp::ToggleButton();
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(scopes[i]));
    label.FontSize(11);
    button.Content(label);
    button.Padding({8, 2, 8, 2});
    button.MinWidth(0);
    button.MinHeight(0);
    button.IsChecked(i == chat_scope_);
    auto const weak = get_weak();
    int const scope = i;
    button.Click([weak, scope](winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        if (window->chat_scope_ != scope) {
          window->chat_scope_ = scope;
          // mac: switching scope keeps history as reference only
          window->AppendNoticeBubble(pdfgist::Tr("ui.chat.history-cleared"));
        }
        window->RefreshChatScopeButtons();
      }
    });
    chat_scope_buttons_.push_back(button);
    scope_row.Children().Append(button);
  }
  chat_pane.Children().Append(scope_row);
  chat_pane.Children().Append(chat_list_);

  AiContent().Children().Clear();
  AiContent().Children().Append(translate_pane);
  AiContent().Children().Append(summarize_pane);
  AiContent().Children().Append(chat_pane);
  BuildFormsPane();  // appends the forms pane as the fourth child
  AiContent().Children().Append(forms_pane_);
  ShowAiPane(ai_pane_);
}

void MainWindow::ShowAiPane(int index) {
  ai_pane_ = std::clamp(index, 0, 4);
  for (std::size_t i = 0; i < ai_tab_buttons_.size(); ++i) {
    ai_tab_buttons_[i].IsChecked(static_cast<int>(i) == ai_pane_);
  }
  if (ai_pane_ == 4 && !settings_built_) {
    BuildSettingsPane();  // appends the settings pane as the fifth child
  }
  // children: 0 translate / 1 summarize / 2 chat / 3 forms / 4 settings
  for (std::uint32_t i = 0; i < AiContent().Children().Size(); ++i) {
    auto child = AiContent().Children().GetAt(i);
    child.Visibility(static_cast<int>(i) == ai_pane_ ? Visibility::Visible
                                                     : Visibility::Collapsed);
  }
  UpdateChrome();
  if (ai_pane_ == 4) LoadSettingsIntoPane();
  if (ai_pane_ == 3) RefreshFormFields();
}

void MainWindow::RefreshChatScopeButtons() {
  for (std::size_t i = 0; i < chat_scope_buttons_.size(); ++i) {
    chat_scope_buttons_[i].IsChecked(static_cast<int>(i) == chat_scope_);
  }
}

void MainWindow::AppendNoticeBubble(std::wstring const& text) {
  ChatBubble bubble;
  bubble.user = false;
  bubble.text = text;
  AppendChatBubble(bubble);
}

// ---- cards -----------------------------------------------------------------------------

std::shared_ptr<StreamCardUi> MainWindow::MakeNoticeCard(
    std::wstring const& message) {
  auto card = std::make_shared<StreamCardUi>();
  card->phase = StreamCardUi::Phase::Notice;
  card->content = message;
  return card;
}

void MainWindow::InsertCard(std::vector<std::shared_ptr<StreamCardUi>>& cards,
                            std::shared_ptr<StreamCardUi> card) {
  cards.insert(cards.begin(), card);
  // newest first, into the list that owns this card vector
  muxc::StackPanel const& list =
      &cards == &translate_cards_ ? translate_list_ : summarize_list_;
  AppendCardUi(list, card);
}
void MainWindow::AppendCardUi(muxc::StackPanel const& list,
                              std::shared_ptr<StreamCardUi> card) {
  if (!card) return;
  auto border = muxc::Border();
  border.Padding({10, 10, 10, 10});
  border.CornerRadius({8, 8, 8, 8});
  border.BorderThickness({1, 1, 1, 1});
  border.BorderBrush(ThemeBorderBrush());
  border.Background(ThemePanelBrush());

  auto stack = muxc::StackPanel();
  stack.Spacing(6);

  // header: title · meta [stop]
  if (!card->title.empty()) {
    auto header = muxc::StackPanel();
    header.Orientation(muxc::Orientation::Horizontal);
    header.Spacing(6);
    muxc::TextBlock title;
    title.Text(card->title);
    title.FontSize(12);
    title.FontWeight(wut::FontWeights::SemiBold());
    header.Children().Append(title);
    if (!card->meta.empty()) {
      muxc::TextBlock meta;
      meta.Text(card->meta);
      meta.FontSize(11);
      meta.Opacity(0.5);
      meta.VerticalAlignment(mux::VerticalAlignment::Center);
      header.Children().Append(meta);
    }
    auto spacer = muxc::Border();
    spacer.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    header.Children().Append(spacer);
    if (card->phase == StreamCardUi::Phase::Streaming) {
      auto stop = muxc::Button();
      muxc::TextBlock stop_label;
      stop_label.Text(pdfgist::Tr("ui.card.stop"));
      stop_label.FontSize(11);
      stop.Content(stop_label);
      stop.Padding({6, 2, 6, 2});
      stop.MinWidth(0);
      stop.MinHeight(0);
      auto const weak = get_weak();
      StreamCardUi* const card_ptr = card.get();
      stop.Click([weak, card_ptr](winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) window->StopCardAt(card_ptr);
      });
      card->stop_button = stop;
      header.Children().Append(stop);
    }
    stack.Children().Append(header);
  }

  // source excerpt (3 lines, dimmed)
  if (!card->source.empty()) {
    muxc::TextBlock source;
    source.Text(card->source);
    source.FontSize(11);
    source.Opacity(0.55);
    source.MaxLines(3);
    source.TextTrimming(
        mux::TextTrimming::CharacterEllipsis);
    source.TextWrapping(mux::TextWrapping::Wrap);
    source.Padding({6, 4, 6, 4});
    stack.Children().Append(source);
  }

  // status line: thinking / notices / errors
  auto status = muxc::TextBlock();
  status.FontSize(11);
  status.TextWrapping(mux::TextWrapping::Wrap);
  status.IsTextSelectionEnabled(true);
  card->status_line = status;
  stack.Children().Append(status);

  // streamed body
  auto body = muxc::TextBlock();
  body.FontSize(12);
  body.IsTextSelectionEnabled(true);
  body.TextWrapping(mux::TextWrapping::Wrap);
  card->body = body;
  stack.Children().Append(body);

  border.Child(stack);
  card->root = border;

  UpdateCardBody(*card);

  // newest first, matching the mac insert-at-0 card lists
  list.Children().InsertAt(0, border);
}

void MainWindow::UpdateCardBody(StreamCardUi& card) {
  switch (card.phase) {
    case StreamCardUi::Phase::Notice:
      if (card.status_line) {
        card.status_line.Text(card.content);
        card.status_line.Opacity(0.7);
        card.status_line.FontStyle(
            winrt::Windows::UI::Text::FontStyle::Italic);
      }
      if (card.body) card.body.Text(L"");
      return;
    case StreamCardUi::Phase::Error:
      if (card.status_line) {
        card.status_line.Text(card.failure);
        card.status_line.Foreground(
            ThemeBrush(wu::Color{0xFF, 0xDC, 0x26, 0x26}));
        card.status_line.FontStyle(
            winrt::Windows::UI::Text::FontStyle::Normal);
      }
      if (card.body) card.body.Text(card.content);
      return;
    case StreamCardUi::Phase::Streaming:
      if (card.status_line) {
        if (card.content.empty() && !card.reasoning.empty()) {
          card.status_line.Text(pdfgist::Tr("backend.stream.thinking"));
          card.status_line.Opacity(0.6);
          card.status_line.FontStyle(
              winrt::Windows::UI::Text::FontStyle::Italic);
        } else {
          card.status_line.Text(L"");
        }
      }
      if (card.body) card.body.Text(card.content);
      return;
    case StreamCardUi::Phase::Done:
      if (card.status_line) card.status_line.Text(L"");
      if (card.body) {
        if (!card.content.empty()) {
          card.body.Text(card.content);
        } else {
          // reasoning-only stream renders dimmed (mac doneBody parity)
          card.body.Text(card.reasoning);
          card.body.Opacity(0.55);
        }
      }
      return;
    case StreamCardUi::Phase::Empty:
      if (card.status_line) {
        card.status_line.Text(card.stopped
                                  ? pdfgist::Tr("backend.stream.stopped")
                                  : pdfgist::Tr("backend.stream.no-content"));
        card.status_line.Opacity(0.6);
        card.status_line.FontStyle(
            winrt::Windows::UI::Text::FontStyle::Italic);
      }
      if (card.body) card.body.Text(L"");
      return;
  }
}

void MainWindow::RemoveCard(std::vector<std::shared_ptr<StreamCardUi>>& cards,
                            StreamCardUi const& card) {
  cards.erase(std::remove_if(cards.begin(), cards.end(),
                             [&card](auto const& candidate) {
                               return candidate.get() == &card;
                             }),
              cards.end());
  if (card.root) {
    auto remove_from = [&](muxc::StackPanel const& list) {
      std::uint32_t index = 0;
      if (list.Children().IndexOf(card.root, index)) {
        list.Children().RemoveAt(index);
      }
    };
    remove_from(translate_list_);
    remove_from(summarize_list_);
    remove_from(chat_list_);
  }
}

void MainWindow::StopCardAt(StreamCardUi* card) {
  for (auto const& candidate : translate_cards_) {
    if (candidate.get() == card) {
      StopCardStream(*candidate);
      return;
    }
  }
  for (auto const& candidate : summarize_cards_) {
    if (candidate.get() == card) {
      StopCardStream(*candidate);
      return;
    }
  }
  for (auto const& bubble : chat_bubbles_) {
    if (bubble.card && bubble.card.get() == card) {
      StopCardStream(*bubble.card);
      return;
    }
  }
}

// ---- stream lifecycle -----------------------------------------------------------------

void MainWindow::StartCardStream(std::shared_ptr<StreamCardUi> card,
                                 std::vector<std::shared_ptr<StreamCardUi>>& list,
                                 std::function<rivet_app::StreamStart()> launch) {
  InsertCard(list, card);
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  // The shared_ptr rides along; the card still holds its UI even if the user
  // closed it mid-launch (registration below then finds no owning list).
  std::thread([weak, dispatcher, backend, card,
               launch = std::move(launch)]() mutable {
    std::string failure;
    rivet_app::StreamStart start{-1};
    try {
      start = launch();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, card, start,
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      bool const alive = window->card_alive(card);
      if (!alive) return;  // the card was closed while the launch ran
      if (!failure.empty()) {
        card->phase = StreamCardUi::Phase::Error;
        card->failure = to_wide(failure);
        window->UpdateCardBody(*card);
        return;
      }
      card->request_id = start.request_id;
      window->streams_[start.request_id] = card;
    });
  }).detach();
}

bool MainWindow::card_alive(std::shared_ptr<StreamCardUi> const& card) const {
  for (auto const& candidate : translate_cards_) {
    if (candidate == card) return true;
  }
  for (auto const& candidate : summarize_cards_) {
    if (candidate == card) return true;
  }
  for (auto const& bubble : chat_bubbles_) {
    if (bubble.card == card) return true;
  }
  return false;
}

void MainWindow::FinishStream(std::int64_t request_id,
                              std::optional<std::wstring> const& error) {
  auto const found = streams_.find(request_id);
  if (found == streams_.end()) return;
  auto card = found->second;
  streams_.erase(found);
  if (error.has_value()) {
    card->phase = StreamCardUi::Phase::Error;
    card->failure = *error;
  } else if (card->content.empty() && card->reasoning.empty()) {
    card->phase = StreamCardUi::Phase::Empty;
    card->stopped = card->stopping;
  } else {
    card->phase = StreamCardUi::Phase::Done;
  }
  if (card->stop_button) {
    try {
      card->stop_button.Visibility(Visibility::Collapsed);
    } catch (...) {
    }
  }
  UpdateCardBody(*card);
  if (chat_busy_ && card->title == L"chat") FinalizeChat(*card);
}

void MainWindow::StopCardStream(StreamCardUi& card) {
  card.stopping = true;
  if (card.request_id > 0 && backend_) {
    auto const backend = backend_;
    auto const id = card.request_id;
    std::thread([backend, id]() mutable {
      try {
        rivet_app::API api(*backend);
        (void)api.stop_stream(id).get();
      } catch (...) {
      }
    }).detach();
  } else {
    FinishStream(card.request_id, std::nullopt);
  }
}

// ---- text sources -----------------------------------------------------------------------

std::wstring MainWindow::SelectionText() {
  std::wstring text;
  if (epub_active_ && epub_) {
    text = epub_->selection_text;
  } else if (active_pdf_) {
    text = active_pdf_->selection_text;
  }
  return text.size() > 1 ? text : std::wstring();
}

void MainWindow::CurrentPageText(
    std::function<void(std::wstring, int)> on_ready) {
  if (epub_active_ && epub_) {
    auto const book = epub_;
    int const chapter = book->current_chapter;
    std::wstring text;
    auto const found = book->text_cache.find(chapter);
    if (found != book->text_cache.end()) text = found->second;
    on_ready(std::move(text), chapter);
    return;
  }
  auto const tab = active_pdf_;
  if (!tab || !tab->doc) {
    on_ready(L"", 0);
    return;
  }
  int const page = tab->current_page;
  auto const doc = tab->doc;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc, page,
               on_ready = std::move(on_ready)]() mutable {
    auto text = to_wide(doc->PageText(page - 1));
    dispatcher.TryEnqueue([weak, text = std::move(text), page,
                           on_ready = std::move(on_ready)]() mutable {
      if (auto window = weak.get()) on_ready(std::move(text), page);
    });
  }).detach();
}

void MainWindow::DocText(std::function<void(std::wstring, int)> on_ready) {
  if (epub_active_ && epub_) {
    auto const book = epub_;
    std::wstring text;
    for (int i = 1; i <= std::max(1, book->chapters); ++i) {
      auto const found = book->text_cache.find(i);
      if (found == book->text_cache.end()) continue;
      std::wstring raw = found->second;
      if (raw.empty()) continue;
      if (raw.size() > 3000) raw = raw.substr(0, 3000) + L"…";
      text += L"--- 第 " + std::to_wstring(i) + L" 章 ---\n" + raw + L"\n\n";
      if (text.size() >= 24000) break;
    }
    on_ready(std::move(text), std::max(1, book->chapters));
    return;
  }
  auto const tab = active_pdf_;
  if (!tab || !tab->doc) {
    on_ready(L"", 0);
    return;
  }
  auto const doc = tab->doc;
  int const pages = std::min(tab->page_count, 12);
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc, pages,
               on_ready = std::move(on_ready)]() mutable {
    std::wstring text;
    for (int i = 0; i < pages; ++i) {
      auto raw = to_wide(doc->PageText(i));
      if (raw.empty()) continue;
      if (raw.size() > 3000) raw = raw.substr(0, 3000) + L"…";
      text += L"--- 第 " + std::to_wstring(i + 1) + L" 页 ---\n" + raw + L"\n\n";
      if (text.size() >= 24000) break;
    }
    dispatcher.TryEnqueue([weak, text = std::move(text), pages,
                           on_ready = std::move(on_ready)]() mutable {
      if (auto window = weak.get()) on_ready(std::move(text), pages);
    });
  }).detach();
}

std::wstring MainWindow::LanguageName(rivet_app::TargetLanguage lang) const {
  switch (lang) {
    case rivet_app::TargetLanguage::zh: return L"中文";
    case rivet_app::TargetLanguage::zh_hant: return L"繁體中文";
    case rivet_app::TargetLanguage::en: return L"English";
    case rivet_app::TargetLanguage::ja: return L"日本語";
    case rivet_app::TargetLanguage::ko: return L"한국어";
    case rivet_app::TargetLanguage::fr: return L"Français";
    case rivet_app::TargetLanguage::de: return L"Deutsch";
    case rivet_app::TargetLanguage::es: return L"Español";
  }
  return L"中文";
}

// v1 ensureProviderConfigured: nothing configured means the settings tab
// opens with a notice instead of a doomed request.
bool MainWindow::EnsureProviderConfigured(
    std::vector<std::shared_ptr<StreamCardUi>>& list) {
  bool const ok = !settings_.base_url.empty() && !settings_.model.empty() &&
                  (settings_.has_api_key || settings_.provider == "ollama");
  if (!ok) {
    ShowAiPane(3);
    auto notice = MakeNoticeCard(pdfgist::Tr("ui.settings.need-config"));
    if (ai_pane_ == 1) {
      InsertCard(summarize_cards_, notice);
    } else {
      InsertCard(translate_cards_, notice);
    }
  }
  return ok;
}

void MainWindow::ConfirmAISend(
    rivet_app::AISendAction action, rivet_app::AISendScope scope,
    std::string const& content,
    std::vector<std::vector<std::string>> const& history,
    std::string const& message, std::function<void()> on_continue) {
  if (!backend_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, action, scope, content, history,
               message, on_continue = std::move(on_continue)]() mutable {
    std::optional<rivet_app::AISendPreview> preview;
    std::string failure;
    try {
      rivet_app::API api(*backend);
      preview = api.preview_ai_send(action, scope, content, history, message).get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue(
        [weak, preview = std::move(preview), failure = std::move(failure),
         on_continue = std::move(on_continue)]() mutable {
          auto window = weak.get();
          if (!window) return;
          if (!preview.has_value()) {
            window->Alert(pdfgist::Trf("ui.ai-preview.failed",
                                       to_wide(failure)));
            return;
          }
          char const* action_key = "ui.ai-preview.action.chat";
          switch (preview->action) {
            case rivet_app::AISendAction::translate:
              action_key = "ui.ai-preview.action.translate";
              break;
            case rivet_app::AISendAction::summarize:
              action_key = "ui.ai-preview.action.summarize";
              break;
            case rivet_app::AISendAction::chat: break;
          }
          char const* scope_key = "ui.ai-preview.scope.page";
          switch (preview->scope) {
            case rivet_app::AISendScope::selection:
              scope_key = "ui.ai-preview.scope.selection";
              break;
            case rivet_app::AISendScope::page: break;
            case rivet_app::AISendScope::chapter:
              scope_key = "ui.ai-preview.scope.chapter";
              break;
            case rivet_app::AISendScope::document:
              scope_key = "ui.ai-preview.scope.document";
              break;
          }
          auto const body = pdfgist::Trf(
              "ui.ai-preview.body",
              pdfgist::Tr(action_key), pdfgist::Tr(scope_key),
              std::to_wstring(preview->content_chars),
              std::to_wstring(preview->history_chars),
              std::to_wstring(preview->message_chars),
              std::to_wstring(preview->total_chars));
          window->ShowDialog(
              pdfgist::Tr("ui.ai-preview.title"), body,
              pdfgist::Tr("ui.ai-preview.continue"),
              pdfgist::Tr("ui.ai-preview.cancel"), std::move(on_continue));
        });
  }).detach();
}

// ---- translate --------------------------------------------------------------------------

void MainWindow::LaunchTranslateCurrentPage() {
  if (!ready_ || !api_) return;
  auto const weak = get_weak();
  CurrentPageText([weak](std::wstring text, int page) {
    auto window = weak.get();
    if (!window || !window->ready_ || !window->api_) return;
    if (text.empty()) {
      auto notice = window->MakeNoticeCard(
          pdfgist::Trf("ui.translate.empty-page", std::to_wstring(page)));
      window->InsertCard(window->translate_cards_, notice);
      return;
    }
    if (!window->EnsureProviderConfigured(window->translate_cards_)) return;
    if (text.size() > 8000) {
      text = text.substr(0, 8000) + pdfgist::Tr("backend.text.truncated");
    }
    auto const lang = window->LanguageName(window->settings_.target_language);
    auto const payload_utf8 =
        to_utf8(L"以下是 PDF 第 " + std::to_wstring(page) + L" 页提取的文本：\n\n" +
                text);
    auto const target = window->settings_.target_language;
    auto const backend = window->backend_;
    auto const preview_scope = window->epub_active_ && window->epub_
                                   ? rivet_app::AISendScope::chapter
                                   : rivet_app::AISendScope::page;
    window->ConfirmAISend(
        rivet_app::AISendAction::translate, preview_scope, payload_utf8, {},
        "", [weak, backend, payload_utf8, target, text, page, lang] {
          auto confirmed = weak.get();
          if (!confirmed) return;
          auto card = std::make_shared<StreamCardUi>();
          card->title =
              confirmed->epub_active_ && confirmed->epub_
                  ? pdfgist::Trf("ui.epub.translate-title",
                                  std::to_wstring(page))
                  : pdfgist::Trf("ui.translate.page-title",
                                  std::to_wstring(page));
          card->meta = L"→ " + lang;
          card->source = text;
          confirmed->StartCardStream(
              card, confirmed->translate_cards_,
              [backend, payload_utf8, target]() {
                rivet_app::API api(*backend);
                return api.translate_text(payload_utf8, target).get();
              });
        });
  });
}

void MainWindow::LaunchTranslateSelection() {
  auto const text = SelectionText();
  if (text.empty()) {
    InsertCard(translate_cards_,
               MakeNoticeCard(pdfgist::Tr("ui.need.selection")));
    return;
  }
  LaunchTranslateSelectionText(text);
}

void MainWindow::LaunchTranslateSelectionText(std::wstring const& text) {
  if (!ready_ || !api_ || !backend_) return;
  if (!EnsureProviderConfigured(translate_cards_)) return;
  auto const lang = LanguageName(settings_.target_language);
  auto const payload = to_utf8(text);
  auto const target = settings_.target_language;
  auto const backend = backend_;
  auto const weak = get_weak();
  ConfirmAISend(
      rivet_app::AISendAction::translate, rivet_app::AISendScope::selection,
      payload, {}, "", [weak, backend, payload, target, text, lang] {
        auto window = weak.get();
        if (!window) return;
        auto card = std::make_shared<StreamCardUi>();
        card->title = pdfgist::Tr("ui.menu.translate");
        card->meta = L"→ " + lang;
        card->source = text;
        window->StartCardStream(
            card, window->translate_cards_, [backend, payload, target]() {
              rivet_app::API api(*backend);
              return api.translate_text(payload, target).get();
            });
      });
}

// ---- summarize --------------------------------------------------------------------------

void MainWindow::LaunchSummarizePage() {
  if (!ready_ || !api_) return;
  auto const weak = get_weak();
  CurrentPageText([weak](std::wstring text, int page) {
    auto window = weak.get();
    if (!window || !window->ready_ || !window->api_) return;
    if (text.empty()) {
      window->InsertCard(window->summarize_cards_,
                         window->MakeNoticeCard(pdfgist::Tr("ui.need.document")));
      return;
    }
    if (!window->EnsureProviderConfigured(window->summarize_cards_)) return;
    if (text.size() > 8000) {
      text = text.substr(0, 8000) + pdfgist::Tr("backend.text.truncated");
    }
    auto const lang = window->LanguageName(window->settings_.target_language);
    auto const payload = to_utf8(text);
    auto const backend = window->backend_;
    auto const preview_scope = window->epub_active_ && window->epub_
                                   ? rivet_app::AISendScope::chapter
                                   : rivet_app::AISendScope::page;
    window->ConfirmAISend(
        rivet_app::AISendAction::summarize, preview_scope, payload, {}, "",
        [weak, backend, payload, text, page, lang] {
          auto confirmed = weak.get();
          if (!confirmed) return;
          auto card = std::make_shared<StreamCardUi>();
          card->title =
              confirmed->epub_active_ && confirmed->epub_
                  ? pdfgist::Trf("ui.epub.summarize-title",
                                  std::to_wstring(page))
                  : pdfgist::Trf("ui.summarize.page-title",
                                  std::to_wstring(page));
          card->meta = lang;
          card->source = text;
          confirmed->StartCardStream(
              card, confirmed->summarize_cards_, [backend, payload]() {
                rivet_app::API api(*backend);
                return api.summarize_text(
                           payload, rivet_app::SummarizeMode::page)
                    .get();
              });
        });
  });
}

void MainWindow::LaunchSummarizeSelection() {
  auto const text = SelectionText();
  if (text.empty()) {
    InsertCard(summarize_cards_,
               MakeNoticeCard(pdfgist::Tr("ui.need.selection")));
    return;
  }
  if (!EnsureProviderConfigured(summarize_cards_)) return;
  if (!ready_ || !api_ || !backend_) return;
  auto const lang = LanguageName(settings_.target_language);
  auto const payload = to_utf8(text);
  auto const backend = backend_;
  auto const weak = get_weak();
  ConfirmAISend(
      rivet_app::AISendAction::summarize,
      rivet_app::AISendScope::selection, payload, {}, "",
      [weak, backend, payload, text, lang] {
        auto window = weak.get();
        if (!window) return;
        auto card = std::make_shared<StreamCardUi>();
        card->title = pdfgist::Tr("ui.summarize.selection");
        card->meta = lang;
        card->source = text;
        window->StartCardStream(
            card, window->summarize_cards_, [backend, payload]() {
              rivet_app::API api(*backend);
              return api.summarize_text(
                         payload, rivet_app::SummarizeMode::selection)
                  .get();
            });
      });
}

void MainWindow::LaunchSummarizeDoc() {
  if (!ready_ || !api_) return;
  auto const weak = get_weak();
  DocText([weak](std::wstring text, int pages) {
    auto window = weak.get();
    if (!window || !window->ready_ || !window->api_) return;
    if (text.empty()) {
      window->InsertCard(window->summarize_cards_,
                         window->MakeNoticeCard(pdfgist::Tr("ui.need.document")));
      return;
    }
    if (!window->EnsureProviderConfigured(window->summarize_cards_)) return;
    auto const lang = window->LanguageName(window->settings_.target_language);
    auto const payload = to_utf8(text);
    auto const backend = window->backend_;
    window->ConfirmAISend(
        rivet_app::AISendAction::summarize,
        rivet_app::AISendScope::document, payload, {}, "",
        [weak, backend, payload, text, pages, lang] {
          auto confirmed = weak.get();
          if (!confirmed) return;
          auto card = std::make_shared<StreamCardUi>();
          card->title = pdfgist::Tr("ui.summarize.doc");
          card->meta = lang + L" · " + pdfgist::Trf(
                                           "ui.summarize.doc-meta",
                                           std::to_wstring(pages));
          card->source = text;
          confirmed->StartCardStream(
              card, confirmed->summarize_cards_, [backend, payload]() {
                rivet_app::API api(*backend);
                return api.summarize_text(payload,
                                          rivet_app::SummarizeMode::doc)
                    .get();
              });
        });
  });
}

// ---- chat ---------------------------------------------------------------------------------

void MainWindow::SendChat(std::wstring const& raw_question) {
  std::wstring question = raw_question;
  auto const is_space = [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
  };
  while (!question.empty() && is_space(question.front())) question.erase(0, 1);
  while (!question.empty() && is_space(question.back())) question.pop_back();
  if (question.empty() || chat_busy_) return;

  bool const configured = !settings_.base_url.empty() &&
                          !settings_.model.empty() &&
                          (settings_.has_api_key || settings_.provider == "ollama");
  if (!configured) {
    ShowAiPane(3);
    AppendNoticeBubble(pdfgist::Tr("ui.settings.need-config"));
    return;
  }

  auto const weak = get_weak();
  int const scope = chat_scope_;
  bool const epub_scope = epub_active_ && epub_;
  auto dispatch = [weak, question, scope, epub_scope](std::wstring text,
                                                      int pages) {
    auto window = weak.get();
    if (!window) return;
    std::wstring context;
    std::wstring label;
    if (scope == 1) {
      context = text;
      label = L"选区";
      if (context.empty()) {
        window->AppendNoticeBubble(pdfgist::Tr("ui.chat.need-selection"));
        return;
      }
    } else if (scope == 2) {
      context = text;
      label = L"全文前 " + std::to_wstring(pages) + L" 页";
      if (context.empty()) {
        window->AppendNoticeBubble(pdfgist::Tr("ui.need.document"));
        return;
      }
    } else {
      context = text;
      label = L"第 " + std::to_wstring(pages) + L" 页";
      if (context.empty()) {
        window->AppendNoticeBubble(pdfgist::Tr("ui.need.document"));
        return;
      }
      context = L"第 " + std::to_wstring(pages) + L" 页：\n" + context;
    }
    auto const preview_scope =
        scope == 1
            ? rivet_app::AISendScope::selection
            : scope == 2 ? rivet_app::AISendScope::document
                         : epub_scope ? rivet_app::AISendScope::chapter
                                      : rivet_app::AISendScope::page;
    window->DispatchChat(question, context, label, preview_scope);
  };

  if (scope == 1) {
    dispatch(SelectionText(), 0);
  } else if (scope == 2) {
    DocText(dispatch);
  } else {
    CurrentPageText(dispatch);
  }
}

// The chat RPC itself: bubbles + stream card + registry (mac sendChat parity).
void MainWindow::DispatchChat(std::wstring const& question,
                              std::wstring const& context_text,
                              std::wstring const& context_label,
                              rivet_app::AISendScope preview_scope) {
  auto const history = chat_history_;
  auto const context = to_utf8(context_text);
  auto const label = to_utf8(context_label);
  auto const message = to_utf8(question);
  auto const backend = backend_;
  auto const weak = get_weak();
  ConfirmAISend(
      rivet_app::AISendAction::chat, preview_scope, context, history, message,
      [weak, backend, history, context, label, message, question]() mutable {
        auto window = weak.get();
        if (!window) return;
        ChatBubble user_bubble;
        user_bubble.user = true;
        user_bubble.text = question;
        window->AppendChatBubble(user_bubble);

        auto card = std::make_shared<StreamCardUi>();
        card->title = L"chat";
        card->meta = window->LanguageName(window->settings_.target_language);
        ChatBubble assistant;
        assistant.user = false;
        assistant.card = card;
        window->AppendChatBubble(assistant);

        window->chat_busy_ = true;
        window->ChatSend().Visibility(Visibility::Collapsed);
        window->ChatStop().Visibility(Visibility::Visible);
        window->chat_pending_question_ = question;
        window->ChatInput().Text(L"");

        auto const dispatcher = window->DispatcherQueue();
        std::thread([weak, dispatcher, backend, history, context, label,
                     message]() mutable {
          std::string failure;
          rivet_app::StreamStart start{-1};
          try {
            rivet_app::API api(*backend);
            start = api.chat(context, label, history, message).get();
          } catch (std::exception const& e) {
            failure = e.what();
          }
          dispatcher.TryEnqueue(
              [weak, start, failure = std::move(failure)]() mutable {
                auto confirmed = weak.get();
                if (!confirmed || confirmed->chat_bubbles_.empty()) return;
                auto& last = confirmed->chat_bubbles_.back();
                if (!last.card) return;
                auto card = last.card;
                if (!failure.empty()) {
                  card->phase = StreamCardUi::Phase::Error;
                  card->failure = to_wide(failure);
                  confirmed->UpdateCardBody(*card);
                  confirmed->FinalizeChat(*card);
                  return;
                }
                card->request_id = start.request_id;
                confirmed->streams_[start.request_id] = card;
              });
        }).detach();
      });
}

void MainWindow::FinalizeChat(StreamCardUi& card) {
  chat_busy_ = false;
  ChatSend().Visibility(Visibility::Visible);
  ChatStop().Visibility(Visibility::Collapsed);
  if (!card.content.empty()) {
    chat_history_.push_back({"user", to_utf8(chat_pending_question_)});
    chat_history_.push_back({"assistant", to_utf8(card.content)});
    if (chat_history_.size() > 24) {
      chat_history_.erase(chat_history_.begin(), chat_history_.end() - 24);
    }
    return;
  }
  if (!card.reasoning.empty()) return;  // dimmed reasoning already rendered
  card.phase = StreamCardUi::Phase::Empty;
  UpdateCardBody(card);
}

void MainWindow::AppendChatBubble(ChatBubble bubble) {
  if (bubble.user) {
    auto border = muxc::Border();
    border.Padding({10, 6, 10, 6});
    border.CornerRadius({8, 8, 8, 8});
    border.Background(ThemePanelAltBrush());
    border.BorderBrush(ThemeBorderBrush());
    border.BorderThickness({1, 1, 1, 1});
    muxc::TextBlock text;
    text.Text(bubble.text);
    text.FontSize(12);
    text.TextWrapping(mux::TextWrapping::Wrap);
    border.Child(text);
    bubble.root = border;
    chat_list_.Children().Append(border);
    chat_bubbles_.push_back(bubble);
    return;
  }
  if (bubble.card) {
    AppendCardUi(chat_list_, bubble.card);
    chat_bubbles_.push_back(bubble);
    return;
  }
  auto border = muxc::Border();
  muxc::TextBlock text;
  text.Text(bubble.text);
  text.FontSize(12);
  text.Opacity(0.6);
  text.TextWrapping(mux::TextWrapping::Wrap);
  border.Child(text);
  bubble.root = border;
  chat_list_.Children().Append(border);
  chat_bubbles_.push_back(bubble);
}

// ---- XAML handlers ---------------------------------------------------------------------

void MainWindow::ChatInput_KeyDown(
    winrt::Windows::Foundation::IInspectable const&,
    mux::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    SendChat(std::wstring(ChatInput().Text()));
    ChatInput().Text(L"");
    args.Handled(true);
  }
}

void MainWindow::SendChat_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  SendChat(std::wstring(ChatInput().Text()));
  ChatInput().Text(L"");
}

void MainWindow::StopChat_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  for (auto it = chat_bubbles_.rbegin(); it != chat_bubbles_.rend(); ++it) {
    if (it->card) {
      StopCardStream(*it->card);
      return;
    }
  }
}

}  // namespace winrt::RivetHost::implementation
