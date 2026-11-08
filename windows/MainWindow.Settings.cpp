#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

using Microsoft::UI::Xaml::Visibility;

namespace {

// Warm neutrals matching the mac Theme.
wu::Color BorderColor(bool dark) {
  return dark ? wu::Color{0x2E, 0xFF, 0xFF, 0xFF} : wu::Color{0x1E, 0x00, 0x00, 0x00};
}
wu::Color PanelColor(bool dark) {
  return dark ? wu::Color{0xFF, 0x21, 0x1D, 0x1A} : wu::Color{0xFF, 0xF5, 0xF2, 0xED};
}
wu::Color PanelAltColor(bool dark) {
  return dark ? wu::Color{0xFF, 0x2B, 0x26, 0x22} : wu::Color{0xFF, 0xEC, 0xE7, 0xE1};
}

muxc::TextBlock SettingLabel(std::wstring const& text) {
  muxc::TextBlock label;
  label.Text(text);
  label.FontSize(12);
  label.FontWeight(wut::FontWeights::SemiBold());
  label.Opacity(0.8);
  label.Margin({0, 12, 0, 4});
  return label;
}

}  // namespace

// ---- theme-aware brushes (code-built cards use these) -----------------------------

mux::Media::SolidColorBrush MainWindow::ThemeBrush(wu::Color color) {
  mux::Media::SolidColorBrush brush;
  brush.Color(color);
  return brush;
}

mux::Media::SolidColorBrush MainWindow::ThemeBorderBrush() {
  return ThemeBrush(BorderColor(Root().ActualTheme() == mux::ElementTheme::Dark));
}

mux::Media::SolidColorBrush MainWindow::ThemePanelBrush() {
  return ThemeBrush(PanelColor(Root().ActualTheme() == mux::ElementTheme::Dark));
}

mux::Media::SolidColorBrush MainWindow::ThemePanelAltBrush() {
  return ThemeBrush(PanelAltColor(Root().ActualTheme() == mux::ElementTheme::Dark));
}

// ---- prefs location ----------------------------------------------------------------

std::wstring MainWindow::HostPrefsDir() const {
  // Same directory the backend uses (settings.rkt): %APPDATA%\site.jrtx.pdfgist.
  wchar_t buffer[MAX_PATH]{};
  if (::GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH) == 0) {
    return {};
  }
  return (std::filesystem::path(buffer) / L"site.jrtx.pdfgist").wstring();
}

// ---- settings pane -------------------------------------------------------------------

void MainWindow::BuildSettingsPane() {
  if (settings_built_) return;
  settings_built_ = true;

  auto pane = muxc::ScrollViewer();
  auto stack = muxc::StackPanel();
  stack.Spacing(8);
  stack.Padding({16, 16, 16, 16});
  stack.MaxWidth(560);

  // ---- provider section ----
  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.preset")));
  settings_provider_ = muxc::ComboBox();
  settings_provider_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  settings_provider_.SelectionChanged(
      [weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                          muxc::SelectionChangedEventArgs const&) {
        if (auto window = weak.get()) {
          if (window->settings_ui_updating_) return;
          try {
            auto const id = std::wstring(winrt::unbox_value<winrt::hstring>(
                window->settings_provider_.SelectedItem()
                    .as<muxc::ComboBoxItem>()
                    .Tag()));
            window->SwitchPreset(id);
          } catch (...) {
          }
        }
      });
  stack.Children().Append(settings_provider_);

  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.base-url")));
  settings_base_url_ = muxc::TextBox();
  settings_base_url_.PlaceholderText(L"https://…");
  stack.Children().Append(settings_base_url_);

  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.model")));
  settings_model_ = muxc::TextBox();
  stack.Children().Append(settings_model_);

  settings_model_suggest_ = muxc::ComboBox();
  settings_model_suggest_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  settings_model_suggest_.Visibility(Visibility::Collapsed);
  settings_model_suggest_.SelectionChanged(
      [weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                          muxc::SelectionChangedEventArgs const&) {
        if (auto window = weak.get()) {
          auto const selection = window->settings_model_suggest_.SelectedItem();
          if (selection) {
            window->settings_model_.Text(
                selection.as<muxc::ComboBoxItem>().Content().as<muxc::TextBlock>().Text());
          }
        }
      });
  stack.Children().Append(settings_model_suggest_);

  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.api-key")));
  settings_api_key_ = muxc::PasswordBox();
  auto key_row = muxc::StackPanel();
  key_row.Orientation(muxc::Orientation::Horizontal);
  key_row.Spacing(6);
  settings_api_key_.Width(220);
  key_row.Children().Append(settings_api_key_);
  auto save_key = muxc::Button();
  muxc::TextBlock save_key_label;
  save_key_label.Text(pdfgist::Tr("ui.settings.save"));
  save_key_label.FontSize(11);
  save_key.Content(save_key_label);
  save_key.Padding({8, 4, 8, 4});
  save_key.MinWidth(0);
  save_key.MinHeight(0);
  save_key.Click([weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                                     mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->SaveApiKeyFromPane();
  });
  key_row.Children().Append(save_key);
  stack.Children().Append(key_row);

  auto actions_row = muxc::StackPanel();
  actions_row.Orientation(muxc::Orientation::Horizontal);
  actions_row.Spacing(6);
  actions_row.Margin({0, 8, 0, 0});
  auto fetch_models = muxc::Button();
  muxc::TextBlock fetch_label;
  fetch_label.Text(pdfgist::Tr("ui.settings.fetch-models"));
  fetch_label.FontSize(11);
  fetch_models.Content(fetch_label);
  fetch_models.Padding({8, 4, 8, 4});
  fetch_models.MinWidth(0);
  fetch_models.MinHeight(0);
  fetch_models.Click([weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                                         mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->TestConnectionFromPane();
  });
  actions_row.Children().Append(fetch_models);
  auto test = muxc::Button();
  muxc::TextBlock test_label;
  test_label.Text(pdfgist::Tr("ui.settings.test"));
  test_label.FontSize(11);
  test.Content(test_label);
  test.Padding({8, 4, 8, 4});
  test.MinWidth(0);
  test.MinHeight(0);
  test.Click([weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->TestConnectionFromPane();
  });
  actions_row.Children().Append(test);
  stack.Children().Append(actions_row);

  settings_status_ = muxc::TextBlock();
  settings_status_.FontSize(11);
  settings_status_.TextWrapping(mux::TextWrapping::Wrap);
  settings_status_.IsTextSelectionEnabled(true);
  stack.Children().Append(settings_status_);

  // ---- reading section ----
  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.target-language")));
  settings_target_ = muxc::ComboBox();
  settings_target_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  for (auto const lang : {rivet_app::TargetLanguage::zh,
                          rivet_app::TargetLanguage::zh_hant,
                          rivet_app::TargetLanguage::en,
                          rivet_app::TargetLanguage::ja,
                          rivet_app::TargetLanguage::ko,
                          rivet_app::TargetLanguage::fr,
                          rivet_app::TargetLanguage::de,
                          rivet_app::TargetLanguage::es}) {
    auto item = muxc::ComboBoxItem();
    muxc::TextBlock label;
    label.Text(LanguageName(lang));
    label.FontSize(12);
    item.Content(label);
    item.Tag(winrt::box_value(static_cast<int>(lang)));
    settings_target_.Items().Append(item);
  }
  stack.Children().Append(settings_target_);

  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.view-mode")));
  settings_view_mode_ = muxc::ComboBox();
  settings_view_mode_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  for (char const* key : {"ui.settings.view-single", "ui.settings.view-double"}) {
    auto item = muxc::ComboBoxItem();
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(key));
    label.FontSize(12);
    item.Content(label);
    settings_view_mode_.Items().Append(item);
  }
  stack.Children().Append(settings_view_mode_);

  settings_sidecar_ = muxc::CheckBox();
  muxc::TextBlock sidecar_label;
  sidecar_label.Text(pdfgist::Tr("ui.settings.sidecar"));
  sidecar_label.FontSize(12);
  settings_sidecar_.Content(sidecar_label);
  stack.Children().Append(settings_sidecar_);

  auto save = muxc::Button();
  muxc::TextBlock save_label;
  save_label.Text(pdfgist::Tr("ui.settings.save"));
  save_label.FontSize(12);
  save.Content(save_label);
  save.Margin({0, 6, 0, 0});
  save.Click([weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->SaveSettingsFromPane();
  });
  stack.Children().Append(save);

  // ---- appearance section ----
  stack.Children().Append(SettingLabel(
      pdfgist::Tr("ui.toolbar.theme-light") + L" / " +
      pdfgist::Tr("ui.toolbar.theme-dark")));
  auto theme_combo = muxc::ComboBox();
  theme_combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  for (std::wstring const label :
       {std::wstring(L"System"), pdfgist::Tr("ui.toolbar.theme-light"),
        pdfgist::Tr("ui.toolbar.theme-dark")}) {
    auto item = muxc::ComboBoxItem();
    muxc::TextBlock label_block;
    label_block.Text(label);
    label_block.FontSize(12);
    item.Content(label_block);
    theme_combo.Items().Append(item);
  }
  theme_combo.SelectedIndex(theme_ == "light" ? 1 : theme_ == "dark" ? 2 : 0);
  theme_combo.SelectionChanged(
      [weak = get_weak()](winrt::Windows::Foundation::IInspectable const& sender,
                          muxc::SelectionChangedEventArgs const&) {
        if (auto window = weak.get()) {
          auto const combo = sender.as<muxc::ComboBox>();
          switch (combo.SelectedIndex()) {
            case 1: window->theme_ = "light"; break;
            case 2: window->theme_ = "dark"; break;
            default: window->theme_ = "system"; break;
          }
          window->ApplyTheme();
          window->SaveHostPrefs();
        }
      });
  stack.Children().Append(theme_combo);

  // ---- reading theme (page surface; persists in settings.json) ----
  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.reading.theme")));
  settings_reading_theme_ = muxc::ComboBox();
  settings_reading_theme_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  for (std::wstring const label :
       {pdfgist::Tr("ui.reading.theme.system"), pdfgist::Tr("ui.reading.theme.paper"),
        pdfgist::Tr("ui.reading.theme.sepia"),
        pdfgist::Tr("ui.reading.theme.night")}) {
    auto item = muxc::ComboBoxItem();
    muxc::TextBlock label_block;
    label_block.Text(label);
    label_block.FontSize(12);
    item.Content(label_block);
    settings_reading_theme_.Items().Append(item);
  }
  settings_reading_theme_.SelectedIndex(
      settings_.reading_theme == rivet_app::ReadingTheme::paper
          ? 1
          : settings_.reading_theme == rivet_app::ReadingTheme::sepia
                ? 2
                : settings_.reading_theme == rivet_app::ReadingTheme::night
                      ? 3
                      : 0);
  settings_reading_theme_.SelectionChanged(
      [weak = get_weak()](winrt::Windows::Foundation::IInspectable const& sender,
                          muxc::SelectionChangedEventArgs const&) {
        if (auto window = weak.get()) {
          auto const combo = sender.as<muxc::ComboBox>();
          using rivet_app::ReadingTheme;
          window->SaveReadingPrefs(
              combo.SelectedIndex() == 1   ? ReadingTheme::paper
              : combo.SelectedIndex() == 2 ? ReadingTheme::sepia
              : combo.SelectedIndex() == 3 ? ReadingTheme::night
                                           : ReadingTheme::system);
        }
      });
  stack.Children().Append(settings_reading_theme_);

  // ---- interface language (baked per render; applies on relaunch) ----
  stack.Children().Append(SettingLabel(pdfgist::Tr("ui.settings.language")));
  settings_language_ = muxc::ComboBox();
  settings_language_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  for (std::wstring const label :
       {pdfgist::Tr("ui.settings.language-system"), std::wstring(L"中文"),
        std::wstring(L"English")}) {
    auto item = muxc::ComboBoxItem();
    muxc::TextBlock label_block;
    label_block.Text(label);
    label_block.FontSize(12);
    item.Content(label_block);
    settings_language_.Items().Append(item);
  }
  settings_language_.SelectedIndex(ui_language_ == "zh"
                                       ? 1
                                       : ui_language_ == "en" ? 2 : 0);
  settings_language_.SelectionChanged(
      [weak = get_weak()](winrt::Windows::Foundation::IInspectable const& sender,
                          muxc::SelectionChangedEventArgs const&) {
        auto window = weak.get();
        if (!window) return;
        auto const combo = sender.as<muxc::ComboBox>();
        switch (combo.SelectedIndex()) {
          case 1: window->ui_language_ = "zh"; break;
          case 2: window->ui_language_ = "en"; break;
          default: window->ui_language_ = "system"; break;
        }
        window->SaveHostPrefs();
        // Same contract as the mac host: the note says a relaunch applies it.
        window->ShowDialog(pdfgist::Tr("app.name"),
                           pdfgist::Tr("ui.settings.language-note"),
                           pdfgist::Tr("ui.password.ok"),
                           pdfgist::Tr("ui.password.cancel"), nullptr);
      });
  stack.Children().Append(settings_language_);

  // ---- privacy footer + about ----
  auto privacy = muxc::TextBlock();
  privacy.Text(pdfgist::Tr("ui.settings.privacy"));
  privacy.FontSize(11);
  privacy.Opacity(0.55);
  privacy.TextWrapping(mux::TextWrapping::Wrap);
  privacy.Margin({0, 12, 0, 0});
  stack.Children().Append(privacy);

  auto version = muxc::TextBlock();
  version.Text(pdfgist::Tr("app.name") + L" " +
               to_wide(rivet_app::kVersion) + L" · Rivet protocol v" +
               std::to_wstring(rivet_app::kBuild));
  version.FontSize(10);
  version.Opacity(0.55);
  stack.Children().Append(version);

  auto links = muxc::StackPanel();
  links.Orientation(muxc::Orientation::Horizontal);
  links.Spacing(14);
  links.Margin({0, 6, 0, 0});
  auto website = muxc::HyperlinkButton();
  website.Content(winrt::box_value(pdfgist::Tr("ui.about.website")));
  website.FontSize(11);
  website.Padding({0});
  website.Click([](winrt::Windows::Foundation::IInspectable const&,
                   mux::RoutedEventArgs const&) {
    ::ShellExecuteW(nullptr, L"open", L"https://booklight.jrtx.site", nullptr,
                    nullptr, SW_SHOWNORMAL);
  });
  links.Children().Append(website);
  auto issues = muxc::HyperlinkButton();
  issues.Content(winrt::box_value(pdfgist::Tr("ui.about.issue")));
  issues.FontSize(11);
  issues.Padding({0});
  issues.Click([](winrt::Windows::Foundation::IInspectable const&,
                  mux::RoutedEventArgs const&) {
    ::ShellExecuteW(nullptr, L"open",
                    L"https://github.com/turlamai/booklight/issues", nullptr,
                    nullptr, SW_SHOWNORMAL);
  });
  links.Children().Append(issues);
  auto about = muxc::HyperlinkButton();
  about.Content(winrt::box_value(pdfgist::Tr("ui.menu.about")));
  about.FontSize(11);
  about.Padding({0});
  about.Click([weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                                  mux::RoutedEventArgs const&) {
    if (auto window = weak.get()) window->ShowAboutDialog();
  });
  links.Children().Append(about);
  stack.Children().Append(links);

  auto license = muxc::TextBlock();
  license.Text(pdfgist::Tr("ui.about.license"));
  license.FontSize(10);
  license.Opacity(0.55);
  stack.Children().Append(license);

  pane.Width(520);
  pane.Height(620);
  pane.Content(stack);
  settings_pane_ = pane;
}

void MainWindow::ShowSettingsDialog() {
  if (!settings_built_) BuildSettingsPane();
  LoadSettingsIntoPane();
  if (settings_dialog_open_) return;
  if (settings_pane_.Parent()) {
    try {
      settings_pane_.Parent().as<muxc::ContentControl>().Content(nullptr);
    } catch (...) {
    }
  }
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("ui.tab.settings")));
  dialog.Content(settings_pane_);
  dialog.CloseButtonText(pdfgist::Tr("ui.update.close"));
  dialog.XamlRoot(Content().XamlRoot());
  settings_dialog_open_ = true;
  dialog.Closed([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) {
      if (window->settings_pane_.Parent()) {
        try {
          window->settings_pane_.Parent()
              .as<muxc::ContentControl>()
              .Content(nullptr);
        } catch (...) {
        }
      }
      window->settings_dialog_open_ = false;
    }
  });
  try {
    (void)dialog.ShowAsync();
  } catch (...) {
    settings_dialog_open_ = false;
  }
}

void MainWindow::LoadSettingsIntoPane() {
  if (!settings_built_ || !settings_loaded_) return;
  // repopulate the preset combo
  try {
    settings_provider_.Items().Clear();
  } catch (...) {
  }
  settings_ui_updating_ = true;
  for (auto const& preset : presets_) {
    auto item = muxc::ComboBoxItem();
    muxc::TextBlock label;
    label.Text(to_wide(preset.label));
    label.FontSize(12);
    item.Content(label);
    item.Tag(winrt::box_value(to_wide(preset.id)));
    settings_provider_.Items().Append(item);
    if (to_wide(preset.id) == to_wide(settings_.provider)) {
      settings_provider_.SelectedItem(item);
    }
  }
  settings_ui_updating_ = false;
  settings_base_url_.Text(to_wide(settings_.base_url));
  settings_model_.Text(to_wide(settings_.model));
  int const target_index = static_cast<int>(settings_.target_language);
  settings_target_.SelectedIndex(std::clamp(target_index, 0, 7));
  settings_view_mode_.SelectedIndex(settings_.view_mode == "double" ? 1 : 0);
  settings_sidecar_.IsChecked(settings_.annotation_sidecar);
  if (settings_reading_theme_) {
    settings_reading_theme_.SelectedIndex(
        settings_.reading_theme == rivet_app::ReadingTheme::paper
            ? 1
            : settings_.reading_theme == rivet_app::ReadingTheme::sepia
                  ? 2
                  : settings_.reading_theme == rivet_app::ReadingTheme::night
                        ? 3
                        : 0);
  }
}

void MainWindow::SwitchPreset(std::wstring const& id) {
  // v1 preset behavior: stash the edited URL/model under the old preset,
  // restore the stash (or the preset defaults) for the new one.
  std::wstring const old_id = SelectedProviderId();
  if (old_id.empty() || old_id == id) return;
  preset_stash_[old_id] = {std::wstring(settings_base_url_.Text()),
                           std::wstring(settings_model_.Text())};
  std::wstring const provider_id = id;
  auto const stash = preset_stash_.find(provider_id);
  if (stash != preset_stash_.end()) {
    settings_base_url_.Text(stash->second.first);
    settings_model_.Text(stash->second.second);
  } else {
    for (auto const& preset : presets_) {
      if (to_wide(preset.id) == provider_id) {
        settings_base_url_.Text(to_wide(preset.base_url));
        settings_model_.Text(to_wide(preset.default_model));
        break;
      }
    }
  }
  settings_api_key_.Password(L"");
  try {
    settings_model_suggest_.Items().Clear();
    settings_model_suggest_.Visibility(Visibility::Collapsed);
  } catch (...) {
  }
}

std::wstring MainWindow::SelectedProviderId() const {
  try {
    auto const selection = settings_provider_.SelectedItem();
    if (!selection) return {};
    return std::wstring(winrt::unbox_value<winrt::hstring>(
        selection.as<muxc::ComboBoxItem>().Tag()));
  } catch (...) {
    return {};
  }
}

void MainWindow::SaveSettingsFromPane() {
  if (!ready_ || !api_ || settings_busy_) return;
  settings_busy_ = true;
  SetSettingsStatus(pdfgist::Tr("ui.settings.fetching"), false);
  rivet_app::SettingsUpdate update{};
  update.provider = to_utf8(SelectedProviderId());
  update.base_url = to_utf8(std::wstring(settings_base_url_.Text()));
  update.model = to_utf8(std::wstring(settings_model_.Text()));
  try {
    auto const tag = winrt::unbox_value<int>(
        settings_target_.SelectedItem().as<muxc::ComboBoxItem>().Tag());
    update.target_language = static_cast<rivet_app::TargetLanguage>(tag);
  } catch (...) {
    update.target_language = rivet_app::TargetLanguage::zh;
  }
  update.view_mode = settings_view_mode_.SelectedIndex() == 1 ? "double" : "single";
  auto const sidecar = settings_sidecar_.IsChecked();
  update.annotation_sidecar = sidecar && sidecar.GetBoolean();

  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, update]() mutable {
    std::string failure;
    rivet_app::SettingsView saved{};
    try {
      rivet_app::API api(*backend);
      saved = api.save_settings(update).get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, saved = std::move(saved),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      window->settings_busy_ = false;
      if (!failure.empty()) {
        window->SetSettingsStatus(
            pdfgist::Trf("ui.settings.save-failed", to_wide(failure)), true);
        return;
      }
      window->settings_ = std::move(saved);
      window->SetSettingsStatus(pdfgist::Tr("ui.settings.saved"), false);
      // mac saveSettings: view mode applies to open tabs immediately
      window->RelayoutActive();
    });
  }).detach();
}

void MainWindow::SaveApiKeyFromPane() {
  if (!ready_ || !api_ || settings_busy_) return;
  std::wstring key(settings_api_key_.Password());
  // trim
  while (!key.empty() && key.front() == L' ') key.erase(0, 1);
  while (!key.empty() && key.back() == L' ') key.pop_back();
  if (key.empty()) return;
  settings_busy_ = true;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, key = to_utf8(key)]() mutable {
    std::string failure;
    rivet_app::SettingsView saved{};
    try {
      rivet_app::API api(*backend);
      (void)api.save_api_key(key).get();
      saved = api.get_settings().get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, saved = std::move(saved),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      window->settings_busy_ = false;
      window->settings_api_key_.Password(L"");
      if (!failure.empty()) {
        window->SetSettingsStatus(
            pdfgist::Trf("ui.settings.save-failed", to_wide(failure)), true);
        return;
      }
      window->settings_ = std::move(saved);
      window->SetSettingsStatus(pdfgist::Tr("ui.settings.saved"), false);
    });
  }).detach();
}

void MainWindow::TestConnectionFromPane() {
  if (!ready_ || !api_ || settings_busy_) return;
  std::wstring const base_url(settings_base_url_.Text());
  std::wstring const key(settings_api_key_.Password());
  std::wstring const model(settings_model_.Text());
  // mac canTest: needs base URL and (model or a stored key)
  if (base_url.empty() || (model.empty() && !settings_.has_api_key)) return;
  settings_busy_ = true;
  SetSettingsStatus(pdfgist::Tr("ui.settings.fetching"), false);
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, url = to_utf8(base_url),
               key_utf8 = to_utf8(key), model_utf8 = to_utf8(model)]() mutable {
    std::string failure;
    rivet_app::TestResult result{};
    try {
      rivet_app::API api(*backend);
      result = api.test_connection(url, key_utf8, model_utf8).get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue(
        [weak, result, failure = std::move(failure)]() mutable {
          auto window = weak.get();
          if (!window) return;
          window->settings_busy_ = false;
          std::wstring status;
          auto models = result.models.value_or(std::vector<std::string>{});
          if (!failure.empty()) {
            status = to_wide(failure);
          } else if (result.ok) {
            status = models.empty()
                         ? pdfgist::Tr("ui.settings.test-ok-no-list")
                         : pdfgist::Trf("ui.settings.test-ok",
                                        std::to_wstring(models.size()));
          } else {
            status = to_wide(result.error.value_or("unknown error"));
          }
          window->SetSettingsStatus(status, status.rfind(L"\u2713", 0) != 0);
          // model suggestions
          try {
            window->settings_model_suggest_.Items().Clear();
            for (auto const& model_name : models) {
              auto item = muxc::ComboBoxItem();
              muxc::TextBlock label;
              label.Text(to_wide(model_name));
              label.FontSize(11);
              item.Content(label);
              window->settings_model_suggest_.Items().Append(item);
            }
            window->settings_model_suggest_.Visibility(
                models.empty() ? Visibility::Collapsed : Visibility::Visible);
          } catch (...) {
          }
        });
  }).detach();
}

void MainWindow::SetSettingsStatus(std::wstring const& message, bool is_error) {
  if (!settings_status_) return;
  settings_status_.Text(message);
  settings_status_.Opacity(is_error ? 1.0 : 0.7);
  settings_status_.Foreground(
      ThemeBrush(is_error ? wu::Color{0xFF, 0xDC, 0x26, 0x26}
                          : wu::Color{0xFF, 0x80, 0x80, 0x80}));
}

// ---- about ----------------------------------------------------------------------------

void MainWindow::ShowAboutDialog() {
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("app.name")));

  auto stack = muxc::StackPanel();
  stack.Spacing(8);

  auto version = muxc::TextBlock();
  version.Text(pdfgist::Tr("app.name") + L" v" +
               to_wide(rivet_app::kVersion));
  version.FontSize(14);
  version.FontWeight(wut::FontWeights::SemiBold());
  stack.Children().Append(version);

  auto license = muxc::TextBlock();
  license.Text(pdfgist::Tr("ui.about.license"));
  license.FontSize(12);
  license.Opacity(0.7);
  license.TextWrapping(mux::TextWrapping::Wrap);
  stack.Children().Append(license);

  auto links = muxc::StackPanel();
  links.Orientation(muxc::Orientation::Horizontal);
  links.Spacing(14);
  auto website = muxc::HyperlinkButton();
  website.Content(winrt::box_value(pdfgist::Tr("ui.about.website")));
  website.FontSize(12);
  website.Padding({0});
  website.Click([](winrt::Windows::Foundation::IInspectable const&,
                   mux::RoutedEventArgs const&) {
    ::ShellExecuteW(nullptr, L"open", L"https://booklight.jrtx.site", nullptr,
                    nullptr, SW_SHOWNORMAL);
  });
  links.Children().Append(website);
  auto issues = muxc::HyperlinkButton();
  issues.Content(winrt::box_value(pdfgist::Tr("ui.about.issue")));
  issues.FontSize(12);
  issues.Padding({0});
  issues.Click([](winrt::Windows::Foundation::IInspectable const&,
                  mux::RoutedEventArgs const&) {
    ::ShellExecuteW(nullptr, L"open",
                    L"https://github.com/turlamai/booklight/issues", nullptr,
                    nullptr, SW_SHOWNORMAL);
  });
  links.Children().Append(issues);
  stack.Children().Append(links);

  dialog.Content(stack);
  dialog.CloseButtonText(pdfgist::Tr("ui.update.close"));
  dialog.XamlRoot(Content().XamlRoot());
  (void)dialog.ShowAsync();
}

}  // namespace winrt::RivetHost::implementation
