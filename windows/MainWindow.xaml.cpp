#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cwchar>
#include <cwctype>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace imaging = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

using Microsoft::UI::Xaml::Visibility;

namespace {

using pdfgist::PdfOpenError;

constexpr wchar_t kAppTitle[] = L"Booklight";
constexpr double kZoomStep = 1.2;
constexpr double kMinZoom = 0.4;
constexpr double kMaxZoom = 4.0;
constexpr double kMinSelectionPoints = 2.0;  // smaller drags are clicks

}  // namespace

// Host helpers declared in MainWindow.xaml.h — namespace scope so the other
// MainWindow.*.cpp translation units link against them.
std::filesystem::path executable_path() {
  std::wstring buffer(32768, L'\0');
  auto const length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length == buffer.size()) {
    throw std::runtime_error("GetModuleFileNameW failed");
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::wstring to_wide(std::string const& utf8_text) {
  if (utf8_text.empty()) return {};
  int const size = ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                                         static_cast<int>(utf8_text.size()),
                                         nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
  if (size > 0) {
    ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                          static_cast<int>(utf8_text.size()), result.data(),
                          size);
  }
  return result;
}

std::string to_utf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  int const size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          result.data(), size, nullptr, nullptr);
  }
  return result;
}

std::string utf8(std::filesystem::path const& path) {
  return to_utf8(path.wstring());
}

namespace {

rivet::windows::RacketRuntimeConfig runtime_config() {
  auto const exe = executable_path();
  auto const root = exe.parent_path();
  auto const runtime = root / L"runtime";

  rivet::windows::RacketRuntimeConfig config;
  config.executable_path = utf8(exe);
  config.petite_boot = utf8(runtime / L"petite.boot");
  config.scheme_boot = utf8(runtime / L"scheme.boot");
  config.racket_boot = utf8(runtime / L"racket.boot");
  config.backend_bundle = utf8(root / L"res" / L"core.zo");
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;
  config.dll_dir = runtime.wstring();
  return config;
}

std::wstring file_name(std::wstring const& path) {
  std::size_t const slash = path.find_last_of(L"/\\");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring lower(std::wstring text) {
  for (auto& c : text) c = static_cast<wchar_t>(::towlower(c));
  return text;
}

bool has_extension(std::wstring const& path, std::wstring const& ext) {
  auto const tail = lower(path.size() >= ext.size()
                              ? path.substr(path.size() - ext.size())
                              : path);
  return tail == lower(ext);
}

media::SolidColorBrush Brush(wu::Color color) {
  media::SolidColorBrush brush;
  brush.Color(color);
  return brush;
}

wu::Color AccentColor() { return wu::Color{0xFF, 0xB4, 0x53, 0x09}; }
wu::Color DangerColor() { return wu::Color{0xFF, 0xDC, 0x26, 0x26}; }
wu::Color FindHighlight() { return wu::Color{0x88, 0xFF, 0xD5, 0x00}; }
wu::Color SelectionFill() { return wu::Color{0x50, 0xB4, 0x53, 0x09}; }

}  // namespace

// ---- construction ---------------------------------------------------------------

MainWindow::MainWindow() {
  InitializeComponent();
  Title(kAppTitle);

  // v1 UI is Chinese-first; en.json exists, so follow the system locale the
  // same way the mac host does (zh prefix → zh, otherwise en) — unless the
  // in-app override picked a language (applied on next launch, mac parity).
  LoadHostPrefs();
  if (ui_language_ == "zh" || ui_language_ == "en") {
    pdfgist::SetUiLanguage(ui_language_);
  } else {
    LANGID const lang = ::GetUserDefaultUILanguage();
    pdfgist::SetUiLanguage(PRIMARYLANGID(lang) == LANG_CHINESE ? "zh" : "en");
  }

  OpenLabel().Text(pdfgist::Tr("ui.toolbar.open"));
  auto set_tip = [](mux::FrameworkElement const& element,
                    std::wstring const& text) {
    muxc::ToolTipService::SetToolTip(element, winrt::box_value(text));
  };
  set_tip(OpenButton(), pdfgist::Tr("ui.toolbar.open"));
  set_tip(LeftToggle(), pdfgist::Tr("ui.toolbar.panel"));
  set_tip(RecentsButton(), pdfgist::Tr("ui.viewer.recents"));
  set_tip(FindToggle(), pdfgist::Tr("ui.toolbar.find"));
  set_tip(BookmarkToggle(), pdfgist::Winize(pdfgist::Tr("ui.toolbar.bookmark")));
  set_tip(SplitToggle(), pdfgist::Tr("ui.toolbar.split"));
  set_tip(TtsButton(), pdfgist::Tr("ui.toolbar.tts"));
  set_tip(PrintButton(), pdfgist::Winize(pdfgist::Tr("ui.toolbar.print")));
  set_tip(ToolsButton(), pdfgist::Tr("ui.menu.tools"));
  ToolsWatermarkItem().Text(pdfgist::Tr("ui.tools.watermark"));
  ToolsSavePdfItem().Text(pdfgist::Tr("ui.dialog.save") + L" PDF…");
  ToolsExportPageItem().Text(pdfgist::Tr("ui.tools.export-page"));
  ToolsExportTextItem().Text(pdfgist::Tr("ui.tools.export-text"));
  set_tip(AiToggle(), pdfgist::Tr("ui.toolbar.ai"));
  set_tip(ThemeButton(), pdfgist::Tr("ui.toolbar.theme-light") + L" / " +
                             pdfgist::Tr("ui.toolbar.theme-dark"));
  ThumbsModeLabel().Text(pdfgist::Tr("ui.viewer.thumbs"));
  OutlineModeLabel().Text(pdfgist::Tr("ui.viewer.outline"));
  NotesModeLabel().Text(pdfgist::Tr("ui.sidebar.annotations"));
  FindInput().PlaceholderText(pdfgist::Tr("ui.viewer.find-placeholder"));
  FitWidthLabel().Text(pdfgist::Tr("ui.viewer.fit"));
  ChatSendLabel().Text(pdfgist::Tr("ui.chat.send"));
  ChatStopLabel().Text(pdfgist::Tr("ui.chat.stop"));
  ChatInput().PlaceholderText(pdfgist::Tr("ui.chat.placeholder"));

  LoadHostPrefs();
  ApplyTheme();
  BuildAiSidebar();
  BuildSettingsPane();
  BuildPageOpsRow();
  BuildKeyboardAccelerators();
  RefreshLeftPanel();
  UpdateChrome();

  LeftToggle().IsChecked(left_visible_);
  AiToggle().IsChecked(ai_visible_);

  Root().AllowDrop(true);
  Root().DragOver([](winrt::Windows::Foundation::IInspectable const&,
                     mux::DragEventArgs const& args) {
    args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::
                               DataPackageOperation::Copy);
  });
  Root().Drop([weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                                  mux::DragEventArgs const& args) {
    if (auto window = weak.get()) window->OpenDroppedRaw(args);
  });

  // Titlebar close, Alt+F4 and explicit Close share the same PDF safeguard.
  AppWindow().Closing([weak = get_weak()](auto&&, auto const& args) {
    auto window = weak.get();
    if (!window) return;
    for (auto const& tab : window->tabs_) {
      if (!tab->pdf_edits_pending) continue;
      args.Cancel(true);
      if (window->window_close_prompt_active_) return;
      window->window_close_prompt_active_ = true;
      window->ConfirmDirtyPdfClose(
          tab,
          [weak] {
            if (auto w = weak.get()) {
              w->window_close_prompt_active_ = false;
              w->Close();  // other pending edits get their own confirmation
            }
          },
          [weak] {
            if (auto w = weak.get()) w->window_close_prompt_active_ = false;
          });
      return;
    }
  });

  Closed([weak = get_weak()](auto&&, auto&&) {
    auto window = weak.get();
    if (!window || !window->ready_ || !window->api_) return;
    // A final synchronous handoff is intentional: detached debounce workers
    // are not a durability guarantee once the window owns the last backend
    // reference. The backend journals recovery bytes before the primary write.
    for (auto const& tab : window->tabs_) {
      if (!tab->doc_data_loaded && !tab->anno_dirty) continue;
      try {
        auto json = window->SerializeDocData(tab->doc_data);
        rivet::Bytes bytes(json.begin(), json.end());
        (void)window->api_
            ->set_annotations(to_utf8(tab->path),
                              window->settings_.annotation_sidecar,
                              std::move(bytes))
            .get();
      } catch (...) {
      }
    }
    for (auto const& book : window->epubs_) {
      if (!book->doc_data_loaded && !book->anno_dirty) continue;
      try {
        auto json = window->SerializeDocData(book->doc_data);
        rivet::Bytes bytes(json.begin(), json.end());
        (void)window->api_
            ->set_annotations(to_utf8(book->path),
                              window->settings_.annotation_sidecar,
                              std::move(bytes))
            .get();
      } catch (...) {
      }
    }
  });

  find_timer_ = DispatcherQueue().CreateTimer();
  find_timer_.Interval(std::chrono::milliseconds{250});
  find_timer_.IsRepeating(false);
  find_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->RunFind();
  });

  recents_timer_ = DispatcherQueue().CreateTimer();
  recents_timer_.Interval(std::chrono::seconds{2});
  recents_timer_.IsRepeating(false);
  recents_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->SaveRecentNow();
  });

  anno_timer_ = DispatcherQueue().CreateTimer();
  anno_timer_.Interval(std::chrono::milliseconds{800});
  anno_timer_.IsRepeating(false);
  anno_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) {
      for (auto const& tab : window->tabs_) {
        if (tab->anno_dirty) window->SaveDocDataNow(tab);
      }
      if (window->epub_ && window->epub_->anno_dirty) {
        window->SaveEpubDocDataNow();
      }
    }
  });
  undo_timer_ = DispatcherQueue().CreateTimer();
  undo_timer_.Interval(std::chrono::milliseconds{5000});
  undo_timer_.IsRepeating(false);
  undo_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->undo_annotation_.reset();
      if (window->left_mode_ == 2) window->RefreshLeftPanel();
    }
  });


  InitializeBackendAsync();
}

// ---- backend lifecycle ----------------------------------------------------------

winrt::fire_and_forget MainWindow::InitializeBackendAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = std::make_shared<rivet::windows::Backend>(runtime_config());

  try {
    // Booting the embedded runtime can block on file I/O, so only startup is
    // moved off the UI thread. RPC/State traffic below is completion-driven.
    co_await winrt::resume_background();
    backend->start();

    dispatcher.TryEnqueue([weak, backend = std::move(backend)]() mutable {
      if (auto window = weak.get()) {
        window->backend_ = std::move(backend);
        try {
          auto& backend_ref = *window->backend_;
          auto const event_dispatcher = window->DispatcherQueue();
          auto const event_weak = window->get_weak();
          // Backend events arrive on the reader thread; hop to the UI thread
          // before touching any UI object (podlens pattern).
          backend_ref.set_event_handler(
              [event_dispatcher,
               event_weak](std::string const& name, rivet::Value const& value) {
                event_dispatcher.TryEnqueue(
                    [event_weak, name, value]() mutable {
                      if (auto window = event_weak.get()) {
                        window->OnBackendEvent(name, value);
                      }
                    });
              });
          window->Bootstrap();
        } catch (std::exception const& e) {
          window->SetStatus(false, to_wide(e.what()));
        }
      } else {
        // Never destroy the last Backend reference on its own reader thread.
        std::thread([backend = std::move(backend)]() mutable {
          backend->stop();
        }).detach();
      }
    });
  } catch (std::exception const& e) {
    auto message = std::string(e.what());
    dispatcher.TryEnqueue([weak, message = std::move(message)] {
      if (auto window = weak.get()) {
        window->SetStatus(false, to_wide(message));
      }
    });
  }
}

void MainWindow::OnBackendEvent(std::string const& name, rivet::Value const& value) {
  try {
    auto const event = rivet_app::decode_event(name, value);
    if (auto const chunk = std::get_if<rivet_app::Stream_chunkEvent>(&event)) {
      auto const found = streams_.find(chunk->value.request_id);
      if (found != streams_.end()) {
        auto card = found->second;
        if (chunk->value.is_reasoning) {
          if (card->content.empty()) card->reasoning += to_wide(chunk->value.delta);
        } else {
          card->content += to_wide(chunk->value.delta);
        }
        UpdateCardBody(*card);
      }
      return;
    }
    if (auto const done = std::get_if<rivet_app::Stream_doneEvent>(&event)) {
      FinishStream(done->value.request_id, std::nullopt);
      return;
    }
    if (auto const error = std::get_if<rivet_app::Stream_errorEvent>(&event)) {
      FinishStream(error->value.request_id, to_wide(error->value.message));
      return;
    }
  } catch (std::exception const&) {
    // unknown event names are ignored — forward compatibility
  }
}

// Post-start initialization (mac bootstrap parity): locale, settings,
// presets, recents, install markers, the throttled update check, and the
// PDFGIST_OPEN launch convention.
void MainWindow::Bootstrap() {
  api_ = std::make_unique<rivet_app::API>(*backend_);
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;

  std::thread([weak, dispatcher, backend]() mutable {
    std::string failure;
    rivet_app::SettingsView settings{};
    std::vector<rivet_app::ProviderPreset> presets;
    std::vector<rivet_app::RecentEntry> recents;
    try {
      rivet_app::API api(*backend);
      api.initialize().get();
      (void)api.set_locale(pdfgist::UiLanguageCode()).get();
      settings = api.get_settings().get();
      presets = api.list_presets().get();
      recents = api.get_recents().get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, dispatcher, backend,
                           settings = std::move(settings),
                           presets = std::move(presets),
                           recents = std::move(recents),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (!failure.empty()) {
        window->SetStatus(false, to_wide(failure));
        return;
      }
      window->settings_ = std::move(settings);
      window->presets_ = std::move(presets);
      window->recents_ = std::move(recents);
      window->settings_loaded_ = true;
      window->ready_ = true;
      window->SetStatus(true, L"");
      window->LoadSettingsIntoPane();
      window->RefreshLeftPanel();
      window->WelcomeFileHint().Text(pdfgist::Tr("ui.welcome.open-any"));
      window->WelcomeFileOpen().Text(pdfgist::Tr("ui.menu.open"));
      window->WelcomeFileDrop().Text(pdfgist::Tr("ui.welcome.or-drag"));
      window->RefreshRecentsRows();
      window->HandleInstallMarkers();
      window->StartAutoUpdateCheck();
      // Stand-in for v1's file association (mac bootstrap parity).
      wchar_t buffer[32768]{};
      DWORD length =
          ::GetEnvironmentVariableW(L"BOOKLIGHT_OPEN", buffer, 32768);
      if (length == 0) {
        length = ::GetEnvironmentVariableW(L"PDFGIST_OPEN", buffer, 32768);
      }
      if (length > 0 && length < 32768) {
        std::wstring const launch = buffer;
        if (has_extension(launch, L".pdf") || has_extension(launch, L".epub")) {
          window->OpenPath(launch);
        }
      }
    });
  }).detach();
}

// ---- chrome / theme ---------------------------------------------------------------

void MainWindow::SetStatus(bool ok, std::wstring const& message) {
  if (message.empty()) {
    StatusBar().IsOpen(false);
    return;
  }
  StatusBar().IsOpen(true);
  StatusBar().Severity(ok ? muxc::InfoBarSeverity::Informational
                          : muxc::InfoBarSeverity::Error);
  StatusBar().Message(message);
}

void MainWindow::Alert(std::wstring const& message) {
  ShowDialog(pdfgist::Tr("app.name"), message, L"",
             pdfgist::Tr("ui.update.close"), nullptr);
}

void MainWindow::ShowDialog(std::wstring const& title, std::wstring const& body,
                            std::wstring const& primary_button,
                            std::wstring const& close_button,
                            std::function<void()> on_primary,
                            std::wstring const& secondary_button,
                            std::function<void()> on_secondary,
                            std::function<void()> on_close) {
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(title));
  // Selection-enabled text so failure messages stay copyable (mac parity).
  muxc::TextBlock content;
  content.Text(body);
  content.TextWrapping(mux::TextWrapping::Wrap);
  content.IsTextSelectionEnabled(true);
  content.MaxWidth(360);
  dialog.Content(winrt::box_value(content));
  if (!primary_button.empty()) {
    dialog.PrimaryButtonText(primary_button);
    dialog.DefaultButton(muxc::ContentDialogButton::Primary);
  }
  if (!secondary_button.empty()) {
    dialog.SecondaryButtonText(secondary_button);
  }
  dialog.CloseButtonText(close_button);
  dialog.XamlRoot(Content().XamlRoot());

  auto const dispatcher = DispatcherQueue();
  auto op = dialog.ShowAsync();
  op.Completed([dispatcher, on_primary = std::move(on_primary),
                on_secondary = std::move(on_secondary),
                on_close = std::move(on_close)](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) mutable {
    muxc::ContentDialogResult result{muxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      // A dialog collision or cancellation must not leave close pending.
    }
    dispatcher.TryEnqueue(
        [result, on_primary = std::move(on_primary),
         on_secondary = std::move(on_secondary),
         on_close = std::move(on_close)]() {
          if (result == muxc::ContentDialogResult::Primary && on_primary) {
            on_primary();
          } else if (result == muxc::ContentDialogResult::Secondary &&
                     on_secondary) {
            on_secondary();
          } else if (result == muxc::ContentDialogResult::None && on_close) {
            on_close();
          }
        });
  });
}

void MainWindow::ApplyTheme() {
  if (theme_ == "light") {
    Root().RequestedTheme(mux::ElementTheme::Light);
  } else if (theme_ == "dark") {
    Root().RequestedTheme(mux::ElementTheme::Dark);
  } else {
    Root().RequestedTheme(mux::ElementTheme::Default);
  }
  if (epub_) ApplyEpubTheme(*epub_);
  RelayoutActive();
}

void MainWindow::ToggleTheme() {
  // mac toggleTheme: flip relative to the resolved scheme.
  bool const dark = theme_ == "dark" ||
                    (theme_ == "system" &&
                     Root().ActualTheme() == mux::ElementTheme::Dark);
  theme_ = dark ? "light" : "dark";
  ApplyTheme();
  SaveHostPrefs();
}

std::wstring MainWindow::ReadingSurface() {
  using rivet_app::ReadingTheme;
  if (settings_.reading_theme == ReadingTheme::night) return L"dark";
  if (settings_.reading_theme == ReadingTheme::sepia) return L"sepia";
  if (settings_.reading_theme == ReadingTheme::paper) return L"light";
  return Root().ActualTheme() == mux::ElementTheme::Dark ? L"dark"
                                                         : L"light";
}

wu::Color MainWindow::SurfaceCanvasColor() {
  auto const surface = ReadingSurface();
  if (surface == L"dark") return wu::Color{0xFF, 0x1A, 0x17, 0x14};
  if (surface == L"sepia") return wu::Color{0xFF, 0xEF, 0xE6, 0xD5};
  return wu::Color{0xFF, 0xFF, 0xFF, 0xFF};
}

void MainWindow::ApplyReadingTheme() {
  if (epub_) ApplyEpubTheme(*epub_);
  // PDF page cells bake their surface color in at layout time.
  RelayoutActive();
}

void MainWindow::SaveReadingPrefs(rivet_app::ReadingTheme theme) {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  api_->save_reading_prefs_async(
      theme, settings_.reading_font_scale_permille,
      [weak = get_weak(), dispatcher](
          rivet_app::Result<rivet_app::SettingsView> result) {
        if (!result.value) return;
        dispatcher.TryEnqueue([weak, saved = *result.value]() {
          auto window = weak.get();
          if (!window) return;
          // Reconcile with the store (the backend clamps) and re-skin now.
          window->settings_ = saved;
          window->ApplyReadingTheme();
        });
      });
}

void MainWindow::SaveReadingFontScale(double scale) {
  if (!api_) return;
  auto const permille =
      static_cast<std::int64_t>(std::llround(scale * 1000.0));
  auto const dispatcher = DispatcherQueue();
  api_->save_reading_prefs_async(
      settings_.reading_theme, permille,
      [weak = get_weak(), dispatcher](
          rivet_app::Result<rivet_app::SettingsView> result) {
        if (!result.value) return;
        dispatcher.TryEnqueue([weak, saved = *result.value]() {
          auto window = weak.get();
          if (!window) return;
          window->settings_ = saved;
        });
      });
}

void MainWindow::Theme_Click(winrt::Windows::Foundation::IInspectable const&,
                             mux::RoutedEventArgs const&) {
  ToggleTheme();
}

void MainWindow::UpdateChrome() {
  // No file open: show an Open/Recent welcome instead of a catalog.
  bool const empty = tabs_.empty() && epubs_.empty();
  WelcomeView().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
  auto const reader_vis = empty ? Visibility::Collapsed : Visibility::Visible;
  LeftToggle().Visibility(reader_vis);
  RecentsButton().Visibility(reader_vis);
  FindToggle().Visibility(reader_vis);
  BookmarkToggle().Visibility(reader_vis);
  SplitToggle().Visibility(reader_vis);
  TtsButton().Visibility(reader_vis);
  PrintButton().Visibility(reader_vis);
  ToolsButton().Visibility(reader_vis);
  BottomBar().Visibility(!epub_active_ && active_pdf_ != nullptr
                             ? Visibility::Visible
                             : Visibility::Collapsed);
  ChatInputBar().Visibility(ai_visible_ && ai_pane_ == 2 ? Visibility::Visible
                                                         : Visibility::Collapsed);
  UpdatePageBar();
}

// ---- host prefs --------------------------------------------------------------------

void MainWindow::LoadHostPrefs() {
  try {
    std::ifstream file(std::filesystem::path(HostPrefsDir()) / L"host-ui.json");
    std::string line;
    while (std::getline(file, line)) {
      auto value_of = [&line](char const* key) -> std::string {
        std::string const prefix = std::string(key) + "=";
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        return {};
      };
      if (auto v = value_of("theme"); !v.empty()) theme_ = v;
      if (auto v = value_of("ai-visible"); !v.empty()) ai_visible_ = v != "false";
      if (auto v = value_of("left-visible"); !v.empty()) {
        left_visible_ = v == "true";
      }
      if (auto v = value_of("ui-language"); !v.empty()) ui_language_ = v;
      if (auto v = value_of("tts-voice"); !v.empty()) tts_voice_ = v;
      if (auto v = value_of("tts-rate"); !v.empty()) {
        try { tts_rate_ = std::stod(v); } catch (...) {}
      }
      tts_rate_ = std::clamp(tts_rate_, 0.5, 3.0);
    }
  } catch (...) {
    // defaults are fine
  }
  LeftPanel().Visibility(left_visible_ ? Visibility::Visible
                                       : Visibility::Collapsed);
  AiPanel().Visibility(ai_visible_ ? Visibility::Visible
                                   : Visibility::Collapsed);
}

void MainWindow::SaveHostPrefs() {
  try {
    auto dir = std::filesystem::path(HostPrefsDir());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(dir / L"host-ui.json", std::ios::trunc);
    file << "theme=" << theme_ << "\n"
         << "ai-visible=" << (ai_visible_ ? "true" : "false") << "\n"
         << "left-visible=" << (left_visible_ ? "true" : "false") << "\n"
         << "ui-language=" << ui_language_ << "\n"
         << "tts-voice=" << tts_voice_ << "\n"
         << "tts-rate=" << tts_rate_ << "\n";
  } catch (...) {
    // best effort
  }
}

// ---- toolbar -----------------------------------------------------------------------

void MainWindow::Open_Click(winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
  PickAndOpen();
}

void MainWindow::LeftToggle_Click(winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
  left_visible_ = LeftToggle().IsChecked().GetBoolean();
  LeftPanel().Visibility(left_visible_ ? Visibility::Visible
                                       : Visibility::Collapsed);
  SaveHostPrefs();
}

void MainWindow::AiToggle_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  ai_visible_ = AiToggle().IsChecked().GetBoolean();
  AiPanel().Visibility(ai_visible_ ? Visibility::Visible
                                   : Visibility::Collapsed);
  UpdateChrome();
  SaveHostPrefs();
}

void MainWindow::FindToggle_Click(winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
  if (find_open_) {
    CloseFind();
    return;
  }
  if (active_pdf_ == nullptr && epub_ == nullptr) return;
  find_open_ = true;
  FindBar().Visibility(Visibility::Visible);
  FindInput().Focus(mux::FocusState::Programmatic);
}

void MainWindow::FindInput_TextChanged(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::TextChangedEventArgs const&) {
  find_timer_.Stop();
  find_timer_.Start();  // 250 ms debounce (mac runFind parity)
}

void MainWindow::FindInput_KeyDown(
    winrt::Windows::Foundation::IInspectable const&,
    mux::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    RunFind();
    args.Handled(true);
  }
}

void MainWindow::FindPrev_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  NavigateFind(-1);
}

void MainWindow::FindNext_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  NavigateFind(1);
}

void MainWindow::FindClose_Click(winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
  CloseFind();
}

void MainWindow::ThumbsMode_Click(winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
  left_mode_ = 0;
  RefreshLeftPanel();
}

void MainWindow::OutlineMode_Click(winrt::Windows::Foundation::IInspectable const&,
                                   mux::RoutedEventArgs const&) {
  left_mode_ = 1;
  RefreshLeftPanel();
}

void MainWindow::NotesMode_Click(winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
  left_mode_ = 2;
  RefreshLeftPanel();
}

void MainWindow::Bookmark_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  ToggleBookmark();
}

void MainWindow::Split_Click(winrt::Windows::Foundation::IInspectable const&,
                             mux::RoutedEventArgs const&) {
  ToggleSplit();
}

// ---- tabs ---------------------------------------------------------------------------

void MainWindow::DocTabs_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::SelectionChangedEventArgs const&) {
  if (!DocTabs()) return;
  active_pdf_ = nullptr;
  epub_ = nullptr;
  epub_active_ = false;
  auto const index = DocTabs().SelectedIndex();
  if (index >= 0) {
    try {
      // TabItems contains explicit TabViewItem instances. ContainerFromIndex
      // is a ListView container lookup and can return null for those items,
      // leaving both active_pdf_ and epub_active_ false even though the tab is
      // visibly selected. Read the item collection directly.
      auto const item = DocTabs()
                            .TabItems()
                            .GetAt(static_cast<std::uint32_t>(index))
                            .as<muxc::TabViewItem>();
      for (auto const& tab : tabs_) {
        if (tab->tab_item == item) active_pdf_ = tab;
      }
      for (auto const& book : epubs_) {
        if (book->tab_item == item) {
          epub_ = book;
          epub_active_ = true;
          break;
        }
      }
    } catch (...) {
    }
  }
  CloseFind();
  if (auto tab = active_pdf_) {
    ApplySplitLayout(*tab);
    LayoutTab(*tab);
    RequestVisibleRenders(*tab);
    LoadDocData(tab);
  }
  SplitToggle().IsChecked(active_pdf_ != nullptr && active_pdf_->split_on);
  UpdateBookmarkToggle();
  RefreshLeftPanel();
  UpdateChrome();
  NoteActivity();
}

void MainWindow::DocTabs_CloseRequested(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::TabViewTabCloseRequestedEventArgs const& args) {
  try {
    auto const item = args.Tab().as<muxc::TabViewItem>();
    for (auto const& book : epubs_) {
      if (book->tab_item == item) {
        epub_ = book;
        epub_active_ = true;
        CloseEpub();
        return;
      }
    }
    for (auto const& tab : tabs_) {
      if (tab->tab_item == item) {
        CloseTab(tab.get());
        return;
      }
    }
  } catch (...) {
  }
}

void MainWindow::PickAndOpen() {
  auto picker = winrt::Windows::Storage::Pickers::FileOpenPicker();
  auto const window_native = try_as<::IWindowNative>();
  if (window_native) {
    HWND hwnd = nullptr;
    if (SUCCEEDED(window_native->get_WindowHandle(&hwnd))) {
      if (auto initializer = picker.try_as<::IInitializeWithWindow>()) {
        initializer->Initialize(hwnd);
      }
    }
  }
  picker.FileTypeFilter().Append(L".pdf");
  picker.FileTypeFilter().Append(L".epub");
  auto const pickerDispatcher = DispatcherQueue();
  picker.PickMultipleFilesAsync().Completed(
      [weak = get_weak(), pickerDispatcher](winrt::Windows::Foundation::IAsyncOperation<
                          winrt::Windows::Foundation::Collections::IVectorView<
                              winrt::Windows::Storage::StorageFile>> const&
                              sender,
                          winrt::Windows::Foundation::AsyncStatus) {
        try {
          auto const files = sender.GetResults();
          if (files == nullptr || files.Size() == 0) return;
          std::vector<std::wstring> paths;
          for (std::uint32_t i = 0; i < files.Size(); ++i) {
            paths.push_back(std::wstring(files.GetAt(i).Path()));
          }
          // The WinRT picker completion may run on a worker thread.
          // WinUI document/tab operations are strictly UI-thread bound.
          pickerDispatcher.TryEnqueue(
              [weak, paths = std::move(paths)]() {
                if (auto window = weak.get()) {
                  for (auto const& path : paths) window->OpenPath(path);
                }
              });
        } catch (...) {
        }
      });
}

void MainWindow::OpenDroppedRaw(mux::DragEventArgs const& args) {
  try {
    auto const deferral = args.GetDeferral();
    auto const weak = get_weak();
    auto const dropDispatcher = DispatcherQueue();
    args.DataView().GetStorageItemsAsync().Completed(
        [weak, deferral, dropDispatcher](winrt::Windows::Foundation::IAsyncOperation<
                         winrt::Windows::Foundation::Collections::IVectorView<
                             winrt::Windows::Storage::IStorageItem>> const& sender,
                         winrt::Windows::Foundation::AsyncStatus) mutable {
          try {
            auto const items = sender.GetResults();
            std::vector<std::wstring> paths;
            for (std::uint32_t i = 0; i < items.Size(); ++i) {
              std::wstring const path(items.GetAt(i).Path());
              if (has_extension(path, L".pdf") ||
                  has_extension(path, L".epub")) {
                paths.push_back(path);
              }
            }
            // Resolve all storage items before ending the deferral. The
            // dispatcher then performs every XAML change on the UI thread.
            dropDispatcher.TryEnqueue(
                [weak, paths = std::move(paths)]() {
                  if (auto window = weak.get()) {
                    for (auto const& path : paths) window->OpenPath(path);
                  }
                });
          } catch (...) {
          }
          if (deferral) deferral.Complete();
        });
  } catch (...) {
  }
}

std::uint32_t MainWindow::IndexOfTabItem(muxc::TabViewItem const& item) {
  std::uint32_t index = 0;
  (void)DocTabs().TabItems().IndexOf(item, index);
  return index;
}

void MainWindow::OpenPath(std::wstring const& path, int resume_page) {
  if (has_extension(path, L".epub")) {
    OpenEpub(path, resume_page > 0 ? resume_page : 1);
    return;
  }
  for (auto const& tab : tabs_) {
    if (tab->path == path) {
      ActivateTab(tab);
      if (resume_page > 1) ScrollToPage(*tab, resume_page);
      return;
    }
  }

  auto bytes = std::make_shared<std::vector<std::uint8_t>>();
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path_copy = std::make_shared<std::wstring>(path);
  auto const resume = std::make_shared<int>(resume_page);

  // File read + first open attempt off the UI thread (PDFium serializes on
  // its own mutex anyway).
  std::thread([weak, dispatcher, bytes, path_copy, resume]() mutable {
    PdfOpenError error = PdfOpenError::None;
    std::shared_ptr<pdfgist::PdfDocument> doc;
    try {
      std::ifstream file(*path_copy, std::ios::binary);
      if (file) {
        bytes->assign(std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>());
      }
      doc = pdfgist::PdfDocument::OpenFromBytes(*bytes, "", &error);
    } catch (...) {
      error = PdfOpenError::Invalid;
    }
    dispatcher.TryEnqueue([weak, doc, error, bytes, path_copy, resume]() mutable {
      if (auto window = weak.get()) {
        window->OpenDocReady(*path_copy, doc, error, bytes, L"", false, *resume);
      }
    });
  }).detach();
}

// UI-thread continuation of OpenPath; loops the password prompt.
void MainWindow::OpenDocReady(
    std::wstring const& path, std::shared_ptr<pdfgist::PdfDocument> doc,
    pdfgist::PdfOpenError error, std::shared_ptr<std::vector<std::uint8_t>> bytes,
    std::wstring const& password, bool wrong_password, int resume_page) {
  if (error == PdfOpenError::NeedsPassword) {
    AskPassword(
        [weak = get_weak(), path, bytes, resume_page](std::wstring const& answer) {
          if (auto window = weak.get()) {
            if (answer.empty()) return;
            window->OpenDocReady(path, nullptr, PdfOpenError::NeedsPassword,
                                 bytes, answer, true, resume_page);
          }
        },
        wrong_password);
    return;
  }
  if (!doc) {
    Alert(pdfgist::Trf("ui.open-failed", path));
    return;
  }
  if (!password.empty()) {
    // reopen with the accepted password (PDFium reads bytes_ in place)
    PdfOpenError retry = PdfOpenError::None;
    doc = pdfgist::PdfDocument::OpenFromBytes(*bytes, to_utf8(password), &retry);
    if (!doc) {
      if (retry == PdfOpenError::NeedsPassword) {
        OpenDocReady(path, nullptr, PdfOpenError::NeedsPassword, bytes, L"",
                     true, resume_page);
        return;
      }
      Alert(pdfgist::Trf("ui.open-failed", path));
      return;
    }
  }

  auto tab = std::make_shared<PdfTab>();
  tab->path = path;
  tab->title = file_name(path);
  tab->doc = std::move(doc);
  tab->page_count = tab->doc->PageCount();
  tab->fit_width = settings_.view_mode != "double";

  muxc::TabViewItem item;
  muxc::TextBlock header;
  header.Text(tab->title);
  header.MaxWidth(150);
  header.TextTrimming(mux::TextTrimming::CharacterEllipsis);
  item.Header(header);
  item.IsClosable(true);

  muxc::ScrollViewer scroll;
  muxc::StackPanel rows;
  rows.Spacing(8);
  rows.Padding({16, 16, 16, 32});
  scroll.Content(rows);
  scroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
  scroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
  scroll.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

  auto const weak_tab = std::weak_ptr<PdfTab>(tab);
  auto const weak_window = get_weak();
  scroll.ViewChanged([weak_window, weak_tab](
                         winrt::Windows::Foundation::IInspectable const&,
                         muxc::ScrollViewerViewChangedEventArgs const&) {
    auto window = weak_window.get();
    auto shared = weak_tab.lock();
    if (!window || !shared) return;
    if (window->active_pdf_.get() == shared.get()) {
      window->RequestVisibleRenders(*shared);
      window->TrackCurrentPage(*shared);
      window->SyncSplitView(*shared);
    }
  });
  scroll.SizeChanged([weak_window, weak_tab](
                         winrt::Windows::Foundation::IInspectable const&,
                         mux::SizeChangedEventArgs const&) {
    auto window = weak_window.get();
    auto shared = weak_tab.lock();
    if (!window || !shared) return;
    if (window->active_pdf_.get() == shared.get()) {
      shared->layout_viewport_w = -1;  // force fit-width relayout
      window->LayoutTab(*shared);
      window->RequestVisibleRenders(*shared);
    }
  });

  // two-column container: main view | divider | split view
  muxc::Grid container;
  muxc::ColumnDefinition main_column;
  main_column.Width({1, mux::GridUnitType::Star});
  container.ColumnDefinitions().Append(main_column);
  muxc::ColumnDefinition divider_column;
  divider_column.Width({0, mux::GridUnitType::Pixel});
  container.ColumnDefinitions().Append(divider_column);
  muxc::ColumnDefinition split_column;
  split_column.Width({0, mux::GridUnitType::Pixel});
  container.ColumnDefinitions().Append(split_column);
  container.Children().Append(scroll);
  muxc::Grid::SetColumn(scroll, 0);

  muxc::Border divider;
  divider.Width(1);
  divider.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  divider.Background(ThemeBorderBrush());
  container.Children().Append(divider);
  muxc::Grid::SetColumn(divider, 1);

  muxc::ScrollViewer split_scroll;
  muxc::StackPanel split_rows;
  split_rows.Spacing(8);
  split_rows.Padding({8, 16, 16, 32});
  split_scroll.Content(split_rows);
  split_scroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
  split_scroll.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  split_scroll.Visibility(Visibility::Collapsed);
  container.Children().Append(split_scroll);
  muxc::Grid::SetColumn(split_scroll, 2);

  item.Content(container);
  tab->tab_item = item;
  tab->scroll = scroll;
  tab->rows = rows;
  tab->split_container = container;
  tab->split_scroll = split_scroll;
  tab->split_rows = split_rows;

  DocTabs().TabItems().Append(item);
  tabs_.push_back(tab);
  if (tabs_.size() == 1 && !left_visible_) {
    left_visible_ = true;
    LeftToggle().IsChecked(true);
    LeftPanel().Visibility(Visibility::Visible);
    SaveHostPrefs();
  }
  ActivateTab(tab);
  ApplySplitLayout(*tab);
  UpdateBookmarkToggle();

  // outline loads in the background; the left panel picks it up
  auto const dispatcher = DispatcherQueue();
  auto const weak_outline = get_weak();
  std::thread([weak_outline, dispatcher, tab]() mutable {
    auto outline = tab->doc->Outline();
    dispatcher.TryEnqueue([weak_outline, tab,
                           outline = std::move(outline)]() mutable {
      if (auto window = weak_outline.get()) {
        tab->outline = std::move(outline);
        if (window->active_pdf_.get() == tab.get()) window->RefreshLeftPanel();
      }
    });
  }).detach();

  // stored annotations + bookmarks ride the backend sidecar
  LoadDocData(tab);

  if (resume_page > 1 && resume_page <= tab->page_count) {
    ScrollToPage(*tab, resume_page);
  }
}

void MainWindow::AskPassword(std::function<void(std::wstring const&)> on_answer,
                             bool wrong) {
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("ui.password.title")));
  muxc::StackPanel content;
  content.Spacing(10);
  if (wrong) {
    muxc::TextBlock error_line;
    error_line.Text(pdfgist::Tr("ui.password.wrong"));
    error_line.Foreground(Brush(DangerColor()));
    content.Children().Append(error_line);
  }
  muxc::PasswordBox box;
  box.Width(240);
  content.Children().Append(box);
  dialog.Content(content);
  dialog.PrimaryButtonText(pdfgist::Tr("ui.password.ok"));
  dialog.CloseButtonText(pdfgist::Tr("ui.password.cancel"));
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  auto const weak = get_weak();
  op.Completed([weak, box, on_answer = std::move(on_answer)](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    muxc::ContentDialogResult result{muxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      return;
    }
    auto const answer = result == muxc::ContentDialogResult::Primary
                            ? box.Password()
                            : winrt::hstring(L"");
    if (auto window = weak.get()) on_answer(std::wstring(answer));
  });
}

void MainWindow::ActivateTab(std::shared_ptr<PdfTab> const& tab) {
  DocTabs().SelectedIndex(IndexOfTabItem(tab->tab_item));
}

void MainWindow::ConfirmDirtyPdfClose(
    std::shared_ptr<PdfTab> const& tab,
    std::function<void()> on_success, std::function<void()> on_cancel) {
  if (!tab || !tab->pdf_edits_pending) {
    if (on_success) on_success();
    return;
  }
  auto const weak = get_weak();
  ShowDialog(
      pdfgist::Tr("ui.pdf.pending-title"),
      pdfgist::Tr("ui.pdf.pending-body"),
      pdfgist::Tr("ui.pdf.save-as"),
      pdfgist::Tr("ui.password.cancel"),
      [weak, tab, done = on_success, cancelled = on_cancel] {
        if (auto w = weak.get()) {
          w->SaveEditedPdf(tab, [weak, tab, done, cancelled](bool saved) {
            if (auto w = weak.get()) {
              if (saved) {
                w->CloseTab(tab.get());
                if (done) done();
              } else if (cancelled) {
                cancelled();
              }
            }
          });
        }
      },
      pdfgist::Tr("ui.pdf.discard"),
      [weak, tab, done = on_success] {
        if (auto w = weak.get()) {
          tab->pdf_edits_pending = false;
          w->CloseTab(tab.get());
          if (done) done();
        }
      },
      [cancelled = std::move(on_cancel)] {
        if (cancelled) cancelled();
      });
}

void MainWindow::CloseTab(PdfTab const* tab) {
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].get() == tab) {
      auto const shared = tabs_[i];
      if (shared->pdf_edits_pending) {
        ConfirmDirtyPdfClose(shared);
        return;
      }
      auto const index = IndexOfTabItem(shared->tab_item);
      if (shared->anno_dirty) SaveDocDataNow(shared);  // flush pending save
      if (index < DocTabs().TabItems().Size()) {
        DocTabs().TabItems().RemoveAt(index);
      }
      tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
      if (active_pdf_.get() == tab) active_pdf_ = nullptr;
      // SelectionChanged already rebuilt the chrome; belt and braces.
      if (tabs_.empty()) {
        UpdateChrome();
      }
      NoteActivity();
      RefreshLeftPanel();
      return;
    }
  }
}

void MainWindow::CloseActiveReader() {
  if (epub_active_ && epub_) {
    CloseEpub();
    return;
  }
  if (active_pdf_) CloseTab(active_pdf_.get());
}

// ---- layout & rendering ------------------------------------------------------------

double MainWindow::RasterScale() {
  double raster = 1.0;
  try {
    raster = Root().XamlRoot().RasterizationScale();
  } catch (...) {
  }
  return std::clamp(raster, 1.0, 3.0);
}

double MainWindow::EffectiveScale(PdfTab& tab) {
  if (!tab.fit_width) return tab.zoom;
  double viewport = tab.scroll ? tab.scroll.ViewportWidth() : 0;
  if (viewport <= 0) viewport = 800;
  double widest = 0;
  for (int i = 0; i < tab.page_count; ++i) {
    double w = 612, h = 792;
    tab.doc->PageSize(i, &w, &h);
    (void)h;
    widest = std::max(widest, w);
  }
  if (widest <= 0 || viewport <= 32) return tab.zoom;
  // Two-up shows a pair; the pair shares the same width budget.
  double const per_page =
      settings_.view_mode == "double" ? (viewport - 56.0) / 2.0 : viewport - 56.0;
  return std::clamp(per_page / widest, kMinZoom, kMaxZoom);
}

void MainWindow::RelayoutActive() {
  for (auto const& tab : tabs_) {
    tab->layout_viewport_w = -1;
    if (tab == active_pdf_) {
      LayoutTab(*tab);
      RequestVisibleRenders(*tab);
    }
  }
  UpdatePageBar();
}

void MainWindow::LayoutTab(PdfTab& tab) {
  if (!tab.doc || !tab.rows) return;
  double const scale = EffectiveScale(tab);
  double const viewport = tab.scroll ? tab.scroll.ViewportWidth() : 0;
  bool const relayout = tab.cells.empty() ||
                        tab.cells_scale_ != scale ||
                        tab.layout_viewport_w != viewport;
  if (!relayout) return;
  tab.cells_scale_ = scale;
  tab.layout_viewport_w = viewport;

  tab.rows.Children().Clear();
  tab.cells.clear();
  tab.rendering_.clear();
  if (tab.split_rows) tab.split_rows.Children().Clear();

  bool const two_up = settings_.view_mode == "double";
  bool const build_split = tab.split_on && tab.split_rows != nullptr;
  int page = 1;
  while (page <= tab.page_count) {
    auto row = muxc::StackPanel();
    row.Orientation(muxc::Orientation::Horizontal);
    row.Spacing(8);
    row.HorizontalAlignment(mux::HorizontalAlignment::Center);
    auto split_row = muxc::StackPanel();
    split_row.Orientation(muxc::Orientation::Horizontal);
    split_row.Spacing(8);
    split_row.HorizontalAlignment(mux::HorizontalAlignment::Center);
    int const second = two_up && page + 1 <= tab.page_count ? page + 1 : 0;
    for (int const p : {page, second}) {
      if (p == 0) break;
      double w = 612, h = 792;
      tab.doc->PageSize(p - 1, &w, &h);
      auto cell = std::make_shared<PdfTab::Cell>();
      cell->page = p;
      cell->width = std::max(1.0, std::round(w * scale));
      cell->height = std::max(1.0, std::round(h * scale));

      auto grid = muxc::Grid();
      grid.Width(cell->width);
      grid.Height(cell->height);
      grid.Background(Brush(SurfaceCanvasColor()));

      auto image = muxc::Image();
      image.Stretch(media::Stretch::Fill);
      image.Width(cell->width);
      image.Height(cell->height);
      grid.Children().Append(image);

      auto canvas = muxc::Canvas();
      canvas.Width(cell->width);
      canvas.Height(cell->height);
      canvas.Background(Brush(wu::Color{0x01, 0, 0, 0}));  // hit-testable
      AttachCellHandlers(tab, *cell, canvas);
      grid.Children().Append(canvas);

      cell->grid = grid;
      cell->canvas = canvas;
      cell->image = image;

      if (build_split) {
        // read-only mirror: same bitmap, no input handling
        auto split_grid = muxc::Grid();
        split_grid.Width(cell->width);
        split_grid.Height(cell->height);
        split_grid.Background(Brush(SurfaceCanvasColor()));
        auto split_image = muxc::Image();
        split_image.Stretch(media::Stretch::Fill);
        split_image.Width(cell->width);
        split_image.Height(cell->height);
        split_grid.Children().Append(split_image);
        auto split_canvas = muxc::Canvas();
        split_canvas.Width(cell->width);
        split_canvas.Height(cell->height);
        split_grid.Children().Append(split_canvas);
        cell->split_grid = split_grid;
        cell->split_canvas = split_canvas;
        cell->split_image = split_image;
        split_row.Children().Append(split_grid);
      }
      tab.cells.push_back(cell);
      row.Children().Append(grid);
    }
    tab.rows.Children().Append(row);
    if (build_split && split_row.Children().Size() > 0) {
      tab.split_rows.Children().Append(split_row);
    }
    page = second == 0 ? page + 1 : second + 1;
  }
  RequestVisibleRenders(tab);
}

void MainWindow::RequestVisibleRenders(PdfTab& tab) {
  if (!tab.doc || tab.cells.empty()) return;
  double const offset = tab.scroll ? tab.scroll.VerticalOffset() : 0;
  double const viewport = tab.scroll ? tab.scroll.ViewportHeight() : 600;
  double const scale = tab.cells_scale_;
  bool const dark = Root().ActualTheme() == mux::ElementTheme::Dark;
  double const raster = RasterScale();
  std::int64_t const key = static_cast<std::int64_t>(scale * raster * 1000.0) *
                           (dark ? -1 : 1);

  double top = 8;
  int first_visible = 1;
  for (auto const& cell : tab.cells) {
    if (top + cell->height >= offset) {
      first_visible = cell->page;
      break;
    }
    top += cell->height + 8;
  }
  double const bottom = offset + viewport;

  // The `requested` flag guards duplicate dispatches; rendered_key is only
  // stamped on success, so a failed render retries on the next pass.
  double row_top = 8;
  for (auto const& cell : tab.cells) {
    bool const intersects =
        row_top < bottom && row_top + cell->height + 8 > offset;
    row_top += cell->height + 8;
    if (!intersects) {
      continue;
    }
    if (cell->rendered_key == key || cell->requested) continue;
    if (std::abs(cell->page - first_visible) > 2) continue;
    cell->requested = true;
    RenderCell(SharedTab(&tab), cell, scale, dark, raster);
  }

  // trim far-away bitmaps so many tabs cannot hoard memory
  int visible_lo = std::max(1, first_visible - 4);
  int visible_hi = std::min(tab.page_count, first_visible + 6);
  for (auto const& cell : tab.cells) {
    if (cell->page < visible_lo || cell->page > visible_hi) {
      if (!cell->requested) {
        cell->image.Source(nullptr);
        cell->rendered_key = 0;
      }
    }
  }
}

std::shared_ptr<PdfTab> MainWindow::SharedTab(PdfTab* tab_ptr) {
  for (auto const& candidate : tabs_) {
    if (candidate.get() == tab_ptr) return candidate;
  }
  return nullptr;
}

void MainWindow::RenderCell(std::shared_ptr<PdfTab> tab,
                            std::shared_ptr<PdfTab::Cell> cell, double scale,
                            bool dark, double raster) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const doc = tab->doc;
  auto const tab_key = tab.get();
  std::thread([weak, dispatcher, doc, tab_key, cell, scale, dark, raster]() mutable {
    try {
      auto rendered = doc->RenderPage(cell->page - 1, scale * raster, dark);
      dispatcher.TryEnqueue([weak, tab_key, cell,
                             rendered = std::move(rendered), scale, dark,
                             raster]() mutable {
        cell->requested = false;
        auto window = weak.get();
        if (!window) return;
        // The tab may have re-laid out or closed while the render ran.
        auto const& cells = window->tabs_;
        bool const still_current =
            std::any_of(cells.begin(), cells.end(), [tab_key](auto const& t) {
              return t.get() == tab_key;
            });
        if (!still_current) return;
        try {
          auto bitmap = imaging::WriteableBitmap(rendered.width, rendered.height);
          auto const data = bitmap.PixelBuffer().data();
          std::memcpy(data, rendered.bgra.data(), rendered.bgra.size());
          bitmap.Invalidate();
          cell->image.Source(bitmap);
          // the split mirror shares the same ImageSource
          if (cell->split_image) cell->split_image.Source(bitmap);
          cell->rendered_key = static_cast<std::int64_t>(scale * raster * 1000.0) *
                               (dark ? -1 : 1);
        } catch (...) {
        }
        if (auto const tab_found = window->SharedTab(tab_key)) {
          window->DrawCellOverlays(*cell, *tab_found);
        }
      });
    } catch (...) {
      cell->requested = false;
    }
  }).detach();
}

void MainWindow::ScrollToPage(PdfTab& tab, int page) {
  page = std::clamp(page, 1, tab.page_count);
  tab.current_page = page;
  if (!tab.scroll) return;
  double offset = 8;
  for (auto const& cell : tab.cells) {
    if (cell->page == page) break;
    offset += cell->height + 8;
  }
  tab.scroll.ChangeView(nullptr, offset, nullptr, true);
  RequestVisibleRenders(tab);
  UpdatePageBar();
  NoteActivity();
}

void MainWindow::TrackCurrentPage(PdfTab& tab) {
  if (!tab.scroll) return;
  double const offset = tab.scroll.VerticalOffset();
  double const viewport = tab.scroll.ViewportHeight();
  double top = 8;
  int current = 1;
  for (auto const& cell : tab.cells) {
    if (top + cell->height * 0.35 >= offset + viewport * 0.2) {
      current = cell->page;
      break;
    }
    top += cell->height + 8;
  }
  if (current != tab.current_page) {
    tab.current_page = current;
    UpdatePageBar();
    UpdateBookmarkToggle();
    NoteActivity();
    // Outline follows the reading position (mac OutlineList parity):
    // rebuild only when the outline pane is the visible left mode.
    if (!epub_active_ && left_mode_ == 1) {
      outline_scroll_pending_ = true;
      RefreshLeftPanel();
    }
  }
}

void MainWindow::UpdatePageBar() {
  if (page_ops_hint_) {
    page_ops_hint_.Text(
        active_pdf_ && !epub_active_
            ? pdfgist::Trf("ui.pages.current-hint",
                           std::to_wstring(active_pdf_->current_page))
            : L"");
  }
  auto const tab = epub_active_ ? nullptr : active_pdf_.get();
  if (tab == nullptr) {
    PageTotal().Text(L"");
    PageInfo().Text(L"");
    ScaleLabel().Text(L"");
    return;
  }
  PageTotal().Text(L" / " + std::to_wstring(tab->page_count));
  PageInfo().Text(pdfgist::Trf("ui.viewer.pdf-info",
                               std::to_wstring(tab->page_count)));
  int const percent =
      static_cast<int>(std::lround(EffectiveScale(*tab) * 100.0));
  ScaleLabel().Text(std::to_wstring(percent) + L"%");
  std::wstring const expected = std::to_wstring(tab->current_page);
  if (PageInput().Text() != expected) {
    PageInput().Text(expected);
  }
}

// ---- zoom / page bar -----------------------------------------------------------------

void MainWindow::ZoomIn_Click(winrt::Windows::Foundation::IInspectable const&,
                              mux::RoutedEventArgs const&) {
  if (epub_active_ && epub_) {
    epub_->font_scale = std::min(2.0, epub_->font_scale * 1.1);
    ApplyEpubFontScale(*epub_);
    SaveReadingFontScale(epub_->font_scale);
    return;
  }
  auto tab = active_pdf_.get();
  if (!tab) return;
  if (tab->fit_width) {
    tab->zoom = EffectiveScale(*tab) * kZoomStep;
    tab->fit_width = false;
  } else {
    tab->zoom = std::min(tab->zoom * kZoomStep, kMaxZoom);
  }
  RelayoutActive();
}

void MainWindow::ZoomOut_Click(winrt::Windows::Foundation::IInspectable const&,
                               mux::RoutedEventArgs const&) {
  if (epub_active_ && epub_) {
    epub_->font_scale = std::max(0.7, epub_->font_scale / 1.1);
    ApplyEpubFontScale(*epub_);
    SaveReadingFontScale(epub_->font_scale);
    return;
  }
  auto tab = active_pdf_.get();
  if (!tab) return;
  if (tab->fit_width) {
    tab->zoom = EffectiveScale(*tab) / kZoomStep;
    tab->fit_width = false;
  } else {
    tab->zoom = std::max(tab->zoom / kZoomStep, kMinZoom);
  }
  RelayoutActive();
}

void MainWindow::ZoomReset_Click(winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
  if (epub_active_ && epub_) {
    epub_->font_scale = 1.0;
    ApplyEpubFontScale(*epub_);
    SaveReadingFontScale(1.0);
    return;
  }
  auto tab = active_pdf_.get();
  if (!tab) return;
  tab->fit_width = false;
  tab->zoom = 1.0;
  RelayoutActive();
}

void MainWindow::FitWidth_Click(winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab) return;
  tab->fit_width = true;
  RelayoutActive();
}

// ---- split view ------------------------------------------------------------------

// Column widths follow the toggle; the split rows themselves are rebuilt by
// LayoutTab on the relayout the viewport change triggers.
void MainWindow::ApplySplitLayout(PdfTab& tab) {
  if (!tab.split_container) return;
  try {
    auto const& columns = tab.split_container.ColumnDefinitions();
    if (columns.Size() < 3) return;
    // divider column stays 1px only while split is on
    columns.GetAt(1).Width(tab.split_on ? mux::GridLength{1, mux::GridUnitType::Pixel}
                                        : mux::GridLength{0, mux::GridUnitType::Pixel});
    columns.GetAt(2).Width(tab.split_on
                               ? mux::GridLength{1, mux::GridUnitType::Star}
                               : mux::GridLength{0, mux::GridUnitType::Pixel});
    if (tab.split_scroll) {
      tab.split_scroll.Visibility(tab.split_on ? Visibility::Visible
                                               : Visibility::Collapsed);
    }
  } catch (...) {
  }
}

void MainWindow::ToggleSplit() {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab) return;
  tab->split_on = !tab->split_on;
  SplitToggle().IsChecked(tab->split_on);
  ApplySplitLayout(*tab);
  // force a relayout so the split rows exist at the new viewport
  tab->layout_viewport_w = -1;
  LayoutTab(*tab);
  RequestVisibleRenders(*tab);
  SyncSplitView(*tab);
}

void MainWindow::SyncSplitView(PdfTab& tab) {
  if (!tab.split_on || !tab.split_scroll || !tab.scroll) return;
  try {
    double const main_extent = tab.scroll.ExtentHeight();
    double const main_viewport = tab.scroll.ViewportHeight();
    if (main_extent <= main_viewport + 1) return;
    double const ratio =
        std::clamp(tab.scroll.VerticalOffset() / (main_extent - main_viewport),
                   0.0, 1.0);
    double const split_extent = tab.split_scroll.ExtentHeight();
    double const split_viewport = tab.split_scroll.ViewportHeight();
    if (split_extent <= split_viewport + 1) return;
    tab.split_scroll.ChangeView(nullptr, ratio * (split_extent - split_viewport),
                                nullptr, true);
  } catch (...) {
  }
}

void MainWindow::PageInput_KeyDown(
    winrt::Windows::Foundation::IInspectable const&,
    mux::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() != winrt::Windows::System::VirtualKey::Enter) return;
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab) return;
  try {
    int const page = std::stoi(std::wstring(PageInput().Text()));
    if (page >= 1 && page <= tab->page_count) {
      ScrollToPage(*tab, page);
      return;
    }
  } catch (...) {
  }
  UpdatePageBar();
}

// ---- selection -----------------------------------------------------------------------

void MainWindow::AttachCellHandlers(PdfTab& tab, PdfTab::Cell& cell,
                                    muxc::Canvas const& canvas) {
  auto shared_tab = SharedTab(&tab);
  auto shared_cell = [&]() -> std::shared_ptr<PdfTab::Cell> {
    for (auto const& candidate : tab.cells) {
      if (candidate.get() == &cell) return candidate;
    }
    return nullptr;
  }();
  if (!shared_tab || !shared_cell) return;

  auto const weak_window = get_weak();
  canvas.PointerPressed([weak_window, shared_tab,
                         shared_cell](winrt::Windows::Foundation::IInspectable const&,
                                      mux::Input::PointerRoutedEventArgs const& args) {
    if (auto window = weak_window.get()) {
      window->OnCellPointerPressed(shared_tab, shared_cell, args);
    }
  });
  canvas.PointerMoved([weak_window, shared_tab,
                       shared_cell](winrt::Windows::Foundation::IInspectable const&,
                                    mux::Input::PointerRoutedEventArgs const& args) {
    if (auto window = weak_window.get()) {
      window->OnCellPointerMoved(shared_tab, shared_cell, args);
    }
  });
  canvas.PointerReleased([weak_window, shared_tab,
                          shared_cell](winrt::Windows::Foundation::IInspectable const&,
                                       mux::Input::PointerRoutedEventArgs const& args) {
    if (auto window = weak_window.get()) {
      window->OnCellPointerReleased(shared_tab, shared_cell, args);
    }
  });
  // right-click context menu with the selection actions (mac selectionMenu)
  muxc::MenuFlyout flyout;
  muxc::MenuFlyoutItem translate_item;
  translate_item.Text(pdfgist::Tr("ui.menu.translate"));
  translate_item.Click([weak_window](winrt::Windows::Foundation::IInspectable const&,
                                     mux::RoutedEventArgs const&) {
    if (auto window = weak_window.get()) window->LaunchTranslateSelection();
  });
  flyout.Items().Append(translate_item);

  // annotation items; enabled only while a selection exists (flyout.Opening)
  struct AnnotationItem {
    char const* label_key;
    wchar_t const* color;
    wchar_t const* kind;
  };
  AnnotationItem const annotation_items[] = {
      {"", L"yellow", L"highlight"},
      {"ui.menu.highlight-yellow", L"yellow", L"highlight"},
      {"ui.menu.highlight-green", L"green", L"highlight"},
      {"ui.menu.highlight-blue", L"blue", L"highlight"},
      {"ui.menu.underline", L"blue", L"underline"},
      {"ui.menu.strike", L"yellow", L"strike"},
  };
  std::vector<muxc::MenuFlyoutItem> annotation_menu_items;
  for (std::size_t i = 0; i < std::size(annotation_items); ++i) {
    auto const& spec = annotation_items[i];
    if (i == 0) {
      flyout.Items().Append(muxc::MenuFlyoutSeparator());
      continue;
    }
    auto item = muxc::MenuFlyoutItem();
    item.Text(pdfgist::Tr(spec.label_key));
    std::wstring const color = spec.color;
    std::wstring const kind = spec.kind;
    item.Click([weak_window, color, kind](
                   winrt::Windows::Foundation::IInspectable const&,
                   mux::RoutedEventArgs const&) {
      if (auto window = weak_window.get()) {
        window->AddAnnotationFromSelection(color, kind);
      }
    });
    annotation_menu_items.push_back(item);
  }
  for (auto const& item : annotation_menu_items) {
    flyout.Items().Append(item);
  }
  flyout.Opening([weak_window, annotation_menu_items](
                     winrt::Windows::Foundation::IInspectable const&,
                     winrt::Windows::Foundation::IInspectable const&) {
    auto window = weak_window.get();
    if (!window) return;
    auto const tab = window->epub_active_ ? nullptr : window->active_pdf_.get();
    bool const has_selection =
        tab != nullptr && !tab->selection_text.empty() &&
        !tab->selection_rects.empty();
    for (auto const& item : annotation_menu_items) {
      item.IsEnabled(has_selection);
    }
  });
  muxc::MenuFlyoutItem copy_item;
  copy_item.Text(pdfgist::Tr("ui.menu.copy"));
  copy_item.Click([weak_window](winrt::Windows::Foundation::IInspectable const&,
                                mux::RoutedEventArgs const&) {
    if (auto window = weak_window.get()) {
      auto const text = window->SelectionText();
      if (text.empty()) return;
      try {
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(text);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(
            package);
      } catch (...) {
      }
    }
  });
  flyout.Items().Append(copy_item);
  canvas.ContextFlyout(flyout);
}

void MainWindow::ClearCellOverlays(PdfTab::Cell& cell) {
  try {
    cell.canvas.Children().Clear();
  } catch (...) {
  }
}

void MainWindow::DrawCellOverlays(PdfTab::Cell& cell, PdfTab const& tab) {
  if (!cell.canvas) return;
  double const scale = tab.cells_scale_;
  if (scale <= 0) return;
  try {
    cell.canvas.Children().Clear();
    if (cell.split_canvas) cell.split_canvas.Children().Clear();

    auto add_rect = [&](std::array<double, 4> const& rect, wu::Color color) {
      double const left = rect[0] * scale;
      double const top = rect[1] * scale;
      double const width = (rect[2] - rect[0]) * scale;
      double const height = (rect[3] - rect[1]) * scale;
      if (width <= 0 || height <= 0) return;
      auto shape = shapes::Rectangle();
      shape.Fill(Brush(color));
      shape.Width(width);
      shape.Height(height);
      muxc::Canvas::SetLeft(shape, left);
      muxc::Canvas::SetTop(shape, top);
      shape.IsHitTestVisible(false);
      cell.canvas.Children().Append(shape);
    };

    // stored annotations & bookmarks (under find + selection)
    DrawStoredOverlays(cell, tab);
    // find highlights (page space → px)
    auto const finds = tab.find_rects.find(cell.page);
    if (finds != tab.find_rects.end()) {
      for (auto const& rect : finds->second) add_rect(rect, FindHighlight());
    }
    // persistent selection (same page only)
    if (tab.selection_page == cell.page) {
      for (auto const& rect : tab.selection_rects) {
        add_rect(rect, SelectionFill());
      }
    }
    // rubber band while dragging
    if (tab.selecting && tab.selection_page == cell.page) {
      std::array<double, 4> const rect{tab.selection_left, tab.selection_top,
                                       tab.selection_right, tab.selection_bottom};
      add_rect(rect, SelectionFill());
    }
  } catch (...) {
  }
}

void MainWindow::OnCellPointerPressed(
    std::shared_ptr<PdfTab> tab, std::shared_ptr<PdfTab::Cell> cell,
    mux::Input::PointerRoutedEventArgs const& args) {
  try {
    auto const point = args.GetCurrentPoint(cell->canvas).Position();
    tab->selection_page = cell->page;
    tab->selection_x0 = point.X;
    tab->selection_y0 = point.Y;
    tab->selecting = true;
    tab->selection_left = point.X;
    tab->selection_top = point.Y;
    tab->selection_right = point.X;
    tab->selection_bottom = point.Y;
    tab->selection_text.clear();
    tab->selection_rects.clear();
    cell->canvas.CapturePointer(args.Pointer());
    DrawCellOverlays(*cell, *tab);
  } catch (...) {
  }
}

void MainWindow::OnCellPointerMoved(
    std::shared_ptr<PdfTab> tab, std::shared_ptr<PdfTab::Cell> cell,
    mux::Input::PointerRoutedEventArgs const& args) {
  if (!tab->selecting || tab->selection_page != cell->page) return;
  try {
    auto const point = args.GetCurrentPoint(cell->canvas).Position();
    double const x = point.X;
    double const y = point.Y;
    tab->selection_left = std::min(tab->selection_x0, x);
    tab->selection_right = std::max(tab->selection_x0, x);
    tab->selection_top = std::min(tab->selection_y0, y);
    tab->selection_bottom = std::max(tab->selection_y0, y);
    DrawCellOverlays(*cell, *tab);
  } catch (...) {
  }
}

void MainWindow::OnCellPointerReleased(
    std::shared_ptr<PdfTab> tab, std::shared_ptr<PdfTab::Cell> cell,
    mux::Input::PointerRoutedEventArgs const& args) {
  if (!tab->selecting) return;
  tab->selecting = false;
  try {
    cell->canvas.ReleasePointerCapture(args.Pointer());
  } catch (...) {
  }
  double const width = tab->selection_right - tab->selection_left;
  double const height = tab->selection_bottom - tab->selection_top;
  if (width < kMinSelectionPoints || height < kMinSelectionPoints) {
    // a click, not a drag — clear the selection (mac mouseDown parity)
    ClearSelection(*tab);
    DrawCellOverlays(*cell, *tab);
    return;
  }
  FinishSelection(tab);
}

// The selected band crosses to text + line rects on a worker thread
// (PDFium serializes under its mutex).
void MainWindow::FinishSelection(std::shared_ptr<PdfTab> tab) {
  int const page = tab->selection_page;
  double const left = tab->selection_left / tab->cells_scale_;
  double const top = tab->selection_top / tab->cells_scale_;
  double const right = tab->selection_right / tab->cells_scale_;
  double const bottom = tab->selection_bottom / tab->cells_scale_;
  auto const doc = tab->doc;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  std::thread([weak, dispatcher, doc, tab, page, left, top, right, bottom]() mutable {
    auto selection = doc->SelectionInRect(page - 1, left, top, right, bottom);
    dispatcher.TryEnqueue([weak, tab, page, text = std::move(selection.utf8),
                           rects = std::move(selection.rects)]() mutable {
      auto window = weak.get();
      if (!window) return;
      // The tab may have closed or re-laid out meanwhile.
      bool alive = false;
      for (auto const& candidate : window->tabs_) {
        if (candidate.get() == tab.get()) {
          alive = true;
          break;
        }
      }
      if (!alive) return;
      tab->selection_text = to_wide(text);
      tab->selection_rects = std::move(rects);
      for (auto const& cell : tab->cells) {
        if (cell->page == page) window->DrawCellOverlays(*cell, *tab);
      }
    });
  }).detach();
}

void MainWindow::ClearSelection(PdfTab& tab) {
  tab.selection_text.clear();
  tab.selection_rects.clear();
  tab.selection_page = 0;
}

// ---- find -----------------------------------------------------------------------------

void MainWindow::RunFind() {
  std::wstring const query(FindInput().Text());
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (epub_active_ && epub_) {
    RunEpubFind(query);
    return;
  }
  if (!tab || query.empty()) {
    find_hits_.clear();
    find_index_ = 0;
    FindCount().Text(query.empty() ? L"" : L"0");
    if (tab) {
      tab->find_rects.clear();
      RelayoutActive();
    }
    return;
  }
  auto const doc = tab->doc;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const needle = to_utf8(query);
  std::thread([weak, dispatcher, doc, needle]() mutable {
    auto hits = doc->Find(needle);
    dispatcher.TryEnqueue([weak, hits = std::move(hits)]() mutable {
      if (auto window = weak.get()) window->ApplyFindResults(std::move(hits));
    });
  }).detach();
}

void MainWindow::ApplyFindResults(std::vector<pdfgist::FindHit> hits) {
  auto tab = active_pdf_;
  if (!tab) return;
  find_hits_ = hits;
  find_index_ = 0;
  tab->find_rects.clear();
  for (auto const& hit : hits) {
    tab->find_rects[hit.page].push_back(hit.rect);
  }
  FindCount().Text(find_hits_.empty()
                       ? (FindInput().Text().empty() ? L"" : L"0")
                       : L"1/" + std::to_wstring(find_hits_.size()));
  // repaint canvases + jump to the first hit at/after the current page
  int current = tab->current_page;
  std::size_t start = 0;
  for (std::size_t i = 0; i < find_hits_.size(); ++i) {
    if (find_hits_[i].page >= current) {
      start = i;
      break;
    }
  }
  find_index_ = static_cast<int>(start);
  if (!find_hits_.empty()) {
    ScrollToPage(*tab, find_hits_[static_cast<std::size_t>(find_index_)].page);
  }
  for (auto const& cell : tab->cells) {
    DrawCellOverlays(*cell, *tab);
  }
}

void MainWindow::NavigateFind(int step) {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (epub_active_ && epub_) {
    NavigateEpubFind(step);
    return;
  }
  if (!tab || find_hits_.empty()) return;
  find_index_ = (find_index_ + step + static_cast<int>(find_hits_.size())) %
                static_cast<int>(find_hits_.size());
  auto const& hit = find_hits_[static_cast<std::size_t>(find_index_)];
  FindCount().Text(std::to_wstring(find_index_ + 1) + L"/" +
                   std::to_wstring(find_hits_.size()));
  ScrollToPage(*tab, hit.page);
}

void MainWindow::CloseFind() {
  find_open_ = false;
  FindBar().Visibility(Visibility::Collapsed);
  FindInput().Text(L"");
  find_hits_.clear();
  find_index_ = 0;
  if (active_pdf_) {
    active_pdf_->find_rects.clear();
    for (auto const& cell : active_pdf_->cells) {
      DrawCellOverlays(*cell, *active_pdf_);
    }
  }
  if (epub_) epub_->highlight_page = -1;
}

// ---- recents ------------------------------------------------------------------------

void MainWindow::NoteActivity() {
  recents_timer_.Stop();
  recents_timer_.Start();
}

void MainWindow::SaveRecentNow() {
  recents_timer_.Stop();
  if (!ready_ || !api_) return;
  std::wstring path;
  int page = 1;
  int pages = 1;
  if (epub_active_ && epub_) {
    path = epub_->path;
    page = epub_->current_chapter;
    pages = std::max(epub_->chapters, 1);
  } else if (active_pdf_) {
    path = active_pdf_->path;
    page = active_pdf_->current_page;
    pages = std::max(active_pdf_->page_count, 1);
  } else {
    return;
  }
  auto const ratio = static_cast<std::int64_t>(
      std::llround(static_cast<double>(page) / static_cast<double>(pages) *
                   100000.0));
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  std::thread([weak, dispatcher, backend, path, page, ratio]() mutable {
    try {
      rivet_app::API api(*backend);
      (void)api.update_recents(to_utf8(path), page, ratio).get();
      auto recents = api.get_recents().get();
      dispatcher.TryEnqueue([weak, recents = std::move(recents)]() mutable {
        if (auto window = weak.get()) {
          window->recents_ = std::move(recents);
          window->RefreshRecentsRows();
        }
      });
    } catch (...) {
    }
  }).detach();
}

// ---- keyboard ------------------------------------------------------------------------

void MainWindow::BuildKeyboardAccelerators() {
  // Page flips: plain (modifier-free) accelerators, Linux parity — the
  // ScrollViewer only pages when it owns focus, so the reader never saw
  // PageUp/PageDown with the focus elsewhere.
  auto add_page_accel = [&](winrt::Windows::System::VirtualKey key,
                            int direction) {
    auto accel = mux::Input::KeyboardAccelerator();
    accel.Key(key);
    accel.Invoked([weak = get_weak(), direction](
                      mux::Input::KeyboardAccelerator const&,
                      mux::Input::KeyboardAcceleratorInvokedEventArgs const&
                          args) {
      args.Handled(true);
      auto window = weak.get();
      if (!window) return;
      auto tab = window->active_pdf_.get();
      if (!tab || window->epub_active_) return;
      window->ScrollToPage(*tab, tab->current_page + direction);
    });
    Root().KeyboardAccelerators().Append(accel);
  };
  add_page_accel(winrt::Windows::System::VirtualKey::PageUp, -1);
  add_page_accel(winrt::Windows::System::VirtualKey::PageDown, 1);

  auto add_accel = [&](winrt::Windows::System::VirtualKey key, bool shift,
                       std::function<void()> action) {
    auto accel = mux::Input::KeyboardAccelerator();
    accel.Key(key);
    accel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control);
    if (shift) {
      accel.Modifiers(winrt::Windows::System::VirtualKeyModifiers::Control |
                      winrt::Windows::System::VirtualKeyModifiers::Shift);
    }
    accel.Invoked([action](mux::Input::KeyboardAccelerator const&,
                           mux::Input::KeyboardAcceleratorInvokedEventArgs const&
                               args) {
      if (action) action();
      args.Handled(true);
    });
    Root().KeyboardAccelerators().Append(accel);
  };

  add_accel(winrt::Windows::System::VirtualKey::O, false, [weak = get_weak()] {
    if (auto window = weak.get()) window->PickAndOpen();
  });
  add_accel(winrt::Windows::System::VirtualKey::F, false, [weak = get_weak()] {
    if (auto window = weak.get()) {
      if (window->find_open_) {
        window->CloseFind();
      } else {
        window->FindToggle_Click({}, {});
      }
    }
  });
  add_accel(winrt::Windows::System::VirtualKey::W, false, [weak = get_weak()] {
    if (auto window = weak.get()) window->CloseActiveReader();
  });
  add_accel(winrt::Windows::System::VirtualKey::Add, false, [weak = get_weak()] {
    if (auto window = weak.get()) window->ZoomIn_Click({}, {});
  });
  add_accel(winrt::Windows::System::VirtualKey::Subtract, false,
            [weak = get_weak()] {
              if (auto window = weak.get()) window->ZoomOut_Click({}, {});
            });
  add_accel(winrt::Windows::System::VirtualKey::Number0, false,
            [weak = get_weak()] {
              if (auto window = weak.get()) window->ZoomReset_Click({}, {});
            });
  add_accel(winrt::Windows::System::VirtualKey::F3, false, [weak = get_weak()] {
    if (auto window = weak.get()) window->NavigateFind(1);
  });
  add_accel(winrt::Windows::System::VirtualKey::F3, true, [weak = get_weak()] {
    if (auto window = weak.get()) window->NavigateFind(-1);
  });
  add_accel(winrt::Windows::System::VirtualKey::B, false, [weak = get_weak()] {
    if (auto window = weak.get()) window->ToggleBookmark();
  });
  add_accel(winrt::Windows::System::VirtualKey::P, false, [weak = get_weak()] {
    if (auto window = weak.get()) window->PrintDocument();
  });
}


// ---- left panel ------------------------------------------------------------------

void MainWindow::RefreshLeftPanel() {
  // (re)build: mode row content (thumbs / outline / notes) + page ops
  // The page-ops row is reused across rebuilds; an element can only have one
  // parent in XAML, so detach it from the panel of the previous build first.
  try {
    if (page_ops_row_ && page_ops_row_.Parent()) {
      page_ops_row_.Parent().as<muxc::Panel>().Children().Clear();
    }
  } catch (...) {
  }
  auto const host = LeftContent();

  // mode row: thumbnails are PDF-only; outline and notes serve both.
  bool const epub_now = epub_active_ && epub_ != nullptr;
  bool const has_reader = epub_now || active_pdf_ != nullptr;
  LeftModeRow().Visibility(has_reader ? Visibility::Visible : Visibility::Collapsed);
  ThumbsMode().Visibility(epub_now ? Visibility::Collapsed : Visibility::Visible);
  NotesMode().Visibility(has_reader ? Visibility::Visible : Visibility::Collapsed);
  ThumbsMode().IsChecked(!epub_now && left_mode_ == 0);
  OutlineMode().IsChecked(!epub_now && left_mode_ == 1 ? true
                          : epub_now && left_mode_ != 2);
  NotesMode().IsChecked(left_mode_ == 2);
  // EPUB: TOC or notes (mac sidebarMode parity)
  int const effective_mode =
      epub_now ? (left_mode_ == 2 ? 2 : 1) : left_mode_;

  // page ops row (PDF only — page surgery is a PDF concern)
  page_ops_row_.Visibility(!epub_now && active_pdf_ != nullptr
                               ? Visibility::Visible
                               : Visibility::Collapsed);

  auto content = muxc::StackPanel();
  content.Spacing(4);
  if (epub_now) {
    if (effective_mode == 2) {
      BuildEpubNotesPane(content);
    } else {
      RenderEpubToc(*epub_);
      return;  // RenderEpubToc assembles the whole panel itself
    }
  } else
  if (active_pdf_) {
    if (effective_mode == 0) {
      // thumbnails (rendered lazily at thumbnail scale)
      auto strip = muxc::StackPanel();
      strip.Spacing(4);
      auto tab = active_pdf_;
      double const thumb_width = 96;
      for (int p = 1; p <= tab->page_count; ++p) {
        double w = 612, h = 792;
        tab->doc->PageSize(p - 1, &w, &h);
        double const scale = thumb_width / w;
        auto button = muxc::Button();
        button.Padding({0});
        button.MinWidth(0);
        button.MinHeight(0);
        button.HorizontalAlignment(mux::HorizontalAlignment::Left);
        auto image = muxc::Image();
        image.Width(thumb_width);
        image.Height(std::max(24.0, h * scale));
        button.Content(image);
        auto const weak_window = get_weak();
        auto const weak_tab = std::weak_ptr<PdfTab>(tab);
        int const page = p;
        button.Click([weak_window, weak_tab, page](
                         winrt::Windows::Foundation::IInspectable const&,
                         mux::RoutedEventArgs const&) {
          if (auto window = weak_window.get()) {
            if (auto shared = weak_tab.lock()) {
              window->ScrollToPage(*shared, page);
            }
          }
        });
        strip.Children().Append(button);

        // render off the UI thread; PDFium serializes internally
        auto const dispatcher = DispatcherQueue();
        auto const doc = tab->doc;
        bool const dark = Root().ActualTheme() == mux::ElementTheme::Dark;
        std::thread([dispatcher, doc, image, page, scale, dark]() mutable {
          try {
            auto rendered = doc->RenderPage(page - 1, scale, dark);
            dispatcher.TryEnqueue([image, rendered = std::move(rendered)]() mutable {
              try {
                auto bitmap = imaging::WriteableBitmap(rendered.width,
                                                       rendered.height);
                std::memcpy(bitmap.PixelBuffer().data(), rendered.bgra.data(),
                            rendered.bgra.size());
                bitmap.Invalidate();
                image.Source(bitmap);
              } catch (...) {
              }
            });
          } catch (...) {
          }
        }).detach();
      }
      content.Children().Append(strip);
      content.Children().Append(page_ops_row_);
    } else if (effective_mode == 2) {
      // notes (annotations + bookmarks)
      BuildNotesPane(content);
    } else {
      // outline
      if (active_pdf_->outline.empty()) {
        auto empty = muxc::TextBlock();
        empty.Text(pdfgist::Tr("ui.viewer.outline-empty"));
        empty.FontSize(11);
        empty.Opacity(0.5);
        empty.Margin({4, 8, 0, 0});
        content.Children().Append(empty);
      }
      auto const weak_window = get_weak();
      auto const weak_tab = std::weak_ptr<PdfTab>(active_pdf_);
      // Active entry: the deepest outline item at or before the current
      // page (mac activeID parity).
      int const current_page = active_pdf_->current_page;
      int active_page = 0;
      for (auto const& node : active_pdf_->outline) {
        if (node.page >= 1 && node.page <= current_page &&
            node.page > active_page) {
          active_page = node.page;
        }
      }
      muxc::Button active_button{nullptr};
      for (auto const& node : active_pdf_->outline) {
        if (node.page <= 0 && node.title.empty()) continue;
        auto button = muxc::Button();
        button.Padding({2, 3, 4, 3});
        button.MinWidth(0);
        button.MinHeight(0);
        button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        button.Margin({static_cast<double>(10 + node.level * 12), 0, 0, 0});
        muxc::TextBlock label;
        label.Text(node.title);
        label.FontSize(12);
        label.TextTrimming(
            mux::TextTrimming::CharacterEllipsis);
        label.Foreground(ThemeBrush(
            node.page == active_page
                ? wu::Color{0xFF, 0xB4, 0x53, 0x09}
                : (Root().ActualTheme() == mux::ElementTheme::Dark
                       ? wu::Color{0xFF, 0xE7, 0xE5, 0xE4}
                       : wu::Color{0xFF, 0x29, 0x25, 0x24})));
        button.Content(label);
        int const page = node.page;
        button.Click([weak_window, weak_tab, page](
                         winrt::Windows::Foundation::IInspectable const&,
                         mux::RoutedEventArgs const&) {
          if (auto window = weak_window.get()) {
            if (auto shared = weak_tab.lock()) {
              if (page >= 1) window->ScrollToPage(*shared, page);
            }
          }
        });
        if (node.page == active_page) active_button = button;
        content.Children().Append(button);
      }
      // Lightweight scroll-to-visible: only when the reading position
      // triggered this rebuild, and only when the entry actually changed.
      if (outline_scroll_pending_) {
        outline_scroll_pending_ = false;
        if (active_page != outline_active_page_ && active_button) {
          active_button.StartBringIntoView();
        }
      }
      outline_active_page_ = active_page;
    }
  } else if (!epub_now) {
    auto empty = muxc::TextBlock();
    empty.Text(pdfgist::Tr("ui.welcome.open"));
    empty.FontSize(11);
    empty.Opacity(0.5);
    empty.Margin({4, 8, 0, 0});
    content.Children().Append(empty);
  }

  host.Children().Clear();
  host.Children().Append(content);
}

void MainWindow::RefreshRecentsRows() {
  // A recent document is an ordinary file path, not a catalog entry.
  auto const children = WelcomeRecentStack().Children();
  children.Clear();
  WelcomeRecentSection().Visibility(
      recents_.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
  WelcomeRecentTitle().Text(pdfgist::Tr("ui.viewer.recents"));
  for (auto const& entry : recents_) {
    auto const path = to_wide(entry.path);
    auto const last = path.find_last_of(L"/\\");
    auto label = path.substr(last == std::wstring::npos ? 0 : last + 1);
    auto button = muxc::Button();
    button.Content(winrt::box_value(label));
    button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(mux::HorizontalAlignment::Left);
    button.Click([weak = get_weak(), path = to_wide(entry.path),
                  page = static_cast<int>(entry.page)](
                     auto const&, auto const&) {
      if (auto window = weak.get()) window->OpenPath(path, std::max(page, 1));
    });
    children.Append(button);
  }
}

// ---- recents flyout ----------------------------------------------------------------

void MainWindow::RecentsFlyout_Opening(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Windows::Foundation::IInspectable const&) {
  try {
    RecentsFlyout().Items().Clear();
  } catch (...) {
    return;
  }
  auto const weak = get_weak();
  int const shown = std::min<std::size_t>(10, recents_.size());
  if (shown == 0) {
    muxc::MenuFlyoutItem empty;
    empty.IsEnabled(false);
    empty.Text(pdfgist::Tr("ui.viewer.recents"));
    RecentsFlyout().Items().Append(empty);
    return;
  }
  for (int i = 0; i < shown; ++i) {
    auto const& entry = recents_[static_cast<std::size_t>(i)];
    auto item = muxc::MenuFlyoutItem();
    item.Text(file_name(to_wide(entry.path)));
    rivet_app::RecentEntry const value = entry;
    item.Click([weak, value](winrt::Windows::Foundation::IInspectable const&,
                             mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        window->OpenPath(to_wide(value.path), static_cast<int>(value.page));
      }
    });
    RecentsFlyout().Items().Append(item);
  }
}

}  // namespace winrt::RivetHost::implementation
