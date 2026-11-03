#include "pch.h"
#include "MainWindow.xaml.h"
#include "I18n.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace {

std::wstring tools_trim(std::wstring text) {
  std::size_t const first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  std::size_t const last = text.find_last_not_of(L" \t\r\n");
  return text.substr(first, last - first + 1);
}

// Filename without its extension — the export default-name stem (mac
// `(title as NSString).deletingPathExtension` parity).
std::wstring tools_stem(std::wstring const& title) {
  std::wstring const suffix = L".pdf";
  if (title.size() >= suffix.size() &&
      title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return title.substr(0, title.size() - suffix.size());
  }
  return title;
}

}  // namespace

// ---- tools menu handlers -------------------------------------------------------

// The PDF export retains tab identity and reports the actual write outcome.
// A failed/canceled save must leave the source tab's dirty state intact.
void MainWindow::SaveEditedPdf(
    std::shared_ptr<PdfTab> const& tab,
    std::function<void(bool)> on_complete) {
  if (!tab || !tab->doc) {
    if (on_complete) on_complete(false);
    return;
  }
  auto const doc = tab->doc;
  auto const title = tab->title;
  auto const path = tab->path;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, tab, doc, title, path,
               on_complete = std::move(on_complete)]() mutable {
    std::string failure;
    std::vector<std::uint8_t> data;
    try {
      data = doc->SaveToBytes();
    } catch (std::exception const& e) {
      failure = e.what();
    } catch (...) {
      failure = "serialize PDF";
    }
    dispatcher.TryEnqueue(
        [weak, tab, doc, title, path, data = std::move(data),
         failure = std::move(failure),
         on_complete = std::move(on_complete)]() mutable {
          auto window = weak.get();
          if (!window) return;
          bool const still_open = std::find(window->tabs_.begin(),
                                             window->tabs_.end(), tab) !=
                                  window->tabs_.end();
          if (!still_open || tab->doc != doc || !failure.empty() ||
              data.empty()) {
            if (!failure.empty()) {
              window->Alert(pdfgist::Trf("ui.pages.save-failed",
                                        to_wide(failure)));
            }
            if (on_complete) on_complete(false);
            return;
          }
          window->SaveData(
              std::move(data), tools_stem(title) + L"-edited.pdf", L"PDF",
              L".pdf",
              [weak, tab, doc, on_complete = std::move(on_complete)](bool saved) {
                auto window = weak.get();
                if (!window) return;
                bool const current =
                    std::find(window->tabs_.begin(), window->tabs_.end(), tab) !=
                        window->tabs_.end() &&
                    tab->doc == doc;
                if (saved && current) {
                  tab->pdf_edits_pending = false;
                  window->UpdateChrome();
                }
                if (on_complete) on_complete(saved && current);
              },
              path);
        });
  }).detach();
}

void MainWindow::ToolsSavePdf_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  SaveEditedPdf(epub_active_ ? nullptr : active_pdf_);
}

void MainWindow::ToolsWatermark_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  ShowWatermarkDialog();
}

void MainWindow::ToolsExportPage_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  ExportPageImage();
}

void MainWindow::ToolsExportText_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  ExportDocumentText();
}

// ---- Tools > Add Watermark -------------------------------------------------------

// Watermark dialog: text / size stepper (12-144) / opacity slider (5-100%)
// (mac WatermarkSheet parity; the default text 机密 is intentional there).
void MainWindow::ShowWatermarkDialog() {
  auto const tab = epub_active_ ? nullptr : active_pdf_;
  if (!tab) return;

  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("ui.tools.watermark")));

  auto stack = muxc::StackPanel();
  stack.Spacing(8);
  stack.MinWidth(320);

  auto input = muxc::TextBox();
  input.PlaceholderText(pdfgist::Tr("ui.watermark.text"));
  input.Text(L"\u673A\u5BC6");  // 机密 — the mac sheet's default draft
  stack.Children().Append(input);

  auto size_row = muxc::StackPanel();
  size_row.Orientation(muxc::Orientation::Horizontal);
  size_row.Spacing(10);
  auto size_label = muxc::TextBlock();
  size_label.Text(pdfgist::Tr("ui.watermark.size"));
  size_label.FontSize(12);
  size_label.Opacity(0.6);
  size_label.VerticalAlignment(mux::VerticalAlignment::Center);
  size_row.Children().Append(size_label);
  auto size_box = muxc::NumberBox();
  size_box.Minimum(12);
  size_box.Maximum(144);
  size_box.Value(48);
  size_box.SmallChange(1);
  size_box.SpinButtonPlacementMode(
      muxc::NumberBoxSpinButtonPlacementMode::Inline);  // the stepper
  size_box.Width(140);
  size_row.Children().Append(size_box);
  stack.Children().Append(size_row);

  auto opacity_row = muxc::StackPanel();
  opacity_row.Orientation(muxc::Orientation::Horizontal);
  opacity_row.Spacing(10);
  auto opacity_label = muxc::TextBlock();
  opacity_label.Text(pdfgist::Tr("ui.watermark.opacity"));
  opacity_label.FontSize(12);
  opacity_label.Opacity(0.6);
  opacity_label.VerticalAlignment(mux::VerticalAlignment::Center);
  opacity_row.Children().Append(opacity_label);
  auto opacity_slider = muxc::Slider();
  opacity_slider.Minimum(5);
  opacity_slider.Maximum(100);
  opacity_slider.Value(30);
  opacity_slider.StepFrequency(1);
  opacity_slider.Width(160);
  opacity_slider.VerticalAlignment(mux::VerticalAlignment::Center);
  opacity_row.Children().Append(opacity_slider);
  auto opacity_value = muxc::TextBlock();
  opacity_value.Text(L"30%");
  opacity_value.FontSize(12);
  opacity_value.Width(44);
  opacity_value.VerticalAlignment(mux::VerticalAlignment::Center);
  opacity_row.Children().Append(opacity_value);
  opacity_slider.ValueChanged(
      [opacity_value](winrt::Windows::Foundation::IInspectable const&,
                      mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
        opacity_value.Text(std::to_wstring(
            static_cast<int>(args.NewValue() + 0.5)) + L"%");
      });
  stack.Children().Append(opacity_row);

  dialog.Content(winrt::box_value(stack));
  dialog.PrimaryButtonText(pdfgist::Tr("ui.watermark.apply"));
  dialog.CloseButtonText(pdfgist::Tr("ui.password.cancel"));
  dialog.DefaultButton(muxc::ContentDialogButton::Primary);
  dialog.IsPrimaryButtonEnabled(true);  // the 机密 draft pre-fills the field
  dialog.XamlRoot(Content().XamlRoot());

  input.TextChanged(
      [dialog, input](winrt::Windows::Foundation::IInspectable const&,
                      muxc::TextChangedEventArgs const&) {
        try {
          dialog.IsPrimaryButtonEnabled(!tools_trim(std::wstring(input.Text())).empty());
        } catch (...) {
        }
      });

  auto op = dialog.ShowAsync();
  auto const weak = get_weak();
  op.Completed([weak, tab, input, size_box, opacity_slider](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    muxc::ContentDialogResult result{muxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      return;  // another dialog was up; the flow can be retried
    }
    if (result != muxc::ContentDialogResult::Primary) return;
    auto const text = tools_trim(std::wstring(input.Text()));
    if (text.empty()) return;
    auto const size = std::clamp(
        static_cast<int>(std::lround(size_box.Value())), 12, 144);
    auto const opacity = std::clamp(
        static_cast<int>(std::lround(opacity_slider.Value())), 5, 100);
    if (auto window = weak.get()) window->ApplyWatermark(tab, text, size, opacity);
  });
}

// edit-bake-text runs on a worker; the baked bytes reload the same tab in
// memory (mac applyWatermark → reloadTabDocument parity — the source file is
// never touched).
void MainWindow::ApplyWatermark(std::shared_ptr<PdfTab> tab,
                                std::wstring const& text, int size,
                                int opacity_percent) {
  if (!tab || !ready_ || !backend_) return;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const path_utf8 = to_utf8(tab->path);
  std::thread([weak, dispatcher, backend, tab, path_utf8,
               text_utf8 = to_utf8(text), size,
               milli = opacity_percent * 1000]() mutable {
    std::string failure;
    rivet::Bytes data;
    try {
      rivet_app::API api(*backend);
      data = api.edit_bake_text(path_utf8, text_utf8,
                                static_cast<std::int64_t>(size),
                                static_cast<std::int64_t>(milli), {})
                 .get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, tab, data = std::move(data),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (!failure.empty()) {
        window->Alert(pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
        return;
      }
      // Reload the same tab in memory only while it is still open.
      for (auto const& candidate : window->tabs_) {
        if (tab.get() == candidate.get()) {
          window->ReloadTabDocument(candidate, std::move(data));
          return;
        }
      }
    });
  }).detach();
}

// ---- Tools > Export Current Page as Image ------------------------------------------

// The visible page at 2x, saved as PNG (mac exportCurrentPageImage parity).
void MainWindow::ExportPageImage() {
  auto const tab = epub_active_ ? nullptr : active_pdf_;
  if (!tab || !tab->doc) return;
  int const page = tab->current_page;
  auto const title = tab->title;
  auto const doc = tab->doc;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc, page, title]() mutable {
    bool failed = true;
    std::vector<std::uint8_t> png;
    try {
      auto rendered = doc->RenderPage(page - 1, 2.0, false);
      if (!rendered.bgra.empty()) png = pdfgist::EncodePng(rendered);
      failed = png.empty();
    } catch (...) {
      failed = true;
    }
    dispatcher.TryEnqueue([weak, png = std::move(png), failed, page,
                           title]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (failed || png.empty()) {
        window->Alert(pdfgist::Trf("ui.export.failed", L"render"));
        return;
      }
      window->SaveData(std::move(png),
                       tools_stem(title) + L"-" + std::to_wstring(page) +
                           L".png",
                       L"PNG", L".png");
    });
  }).detach();
}

// ---- Tools > Export Text --------------------------------------------------------------

// The whole document's text layer as .txt; an empty layer alerts instead of
// saving an empty file (mac exportDocumentText parity).
void MainWindow::ExportDocumentText() {
  auto const tab = epub_active_ ? nullptr : active_pdf_;
  if (!tab || !tab->doc) return;
  auto const title = tab->title;
  auto const doc = tab->doc;
  int const pages = tab->page_count;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc, pages, title]() mutable {
    std::string utf8;
    for (int i = 0; i < pages; ++i) {
      std::string raw = doc->PageText(i);
      if (raw.empty()) continue;
      if (!utf8.empty()) utf8 += "\n";
      utf8 += raw;
    }
    // trimmed emptiness is the mac guard (whitespace-only layers never save)
    auto const first = utf8.find_first_not_of(" \t\r\n");
    bool const empty = first == std::string::npos;
    if (!empty) utf8 = utf8.substr(first, utf8.find_last_not_of(" \t\r\n") -
                                             first + 1);
    dispatcher.TryEnqueue([weak, utf8 = std::move(utf8), empty,
                           title]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (empty) {
        window->Alert(pdfgist::Tr("ui.need.text"));
        return;
      }
      window->SaveData(std::vector<std::uint8_t>(utf8.begin(), utf8.end()),
                       tools_stem(title) + L".txt", L"Text", L".txt");
    });
  }).detach();
}

// ---- shared typed save panel -----------------------------------------------------------

// mac saveData parity: one save dialog for every tool export. A write
// failure alerts (ui.export.failed) instead of failing silently.
void MainWindow::SaveData(std::vector<std::uint8_t> data,
                          std::wstring const& default_name,
                          std::wstring const& type_label,
                          std::wstring const& extension,
                          std::function<void(bool)> on_complete,
                          std::wstring const& protected_original) {
  auto picker = winrt::Windows::Storage::Pickers::FileSavePicker();
  auto const window_native = try_as<::IWindowNative>();
  if (window_native) {
    HWND hwnd = nullptr;
    if (SUCCEEDED(window_native->get_WindowHandle(&hwnd))) {
      if (auto initializer = picker.try_as<::IInitializeWithWindow>()) {
        initializer->Initialize(hwnd);
      }
    }
  }
  picker.SuggestedFileName(default_name);
  picker.FileTypeChoices().Insert(
      type_label,
      winrt::single_threaded_vector<winrt::hstring>(
          std::vector<winrt::hstring>{winrt::hstring(extension)}));
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  picker.PickSaveFileAsync().Completed(
      [weak, dispatcher, data = std::move(data), protected_original,
       on_complete = std::move(on_complete)](
          winrt::Windows::Foundation::IAsyncOperation<
              winrt::Windows::Storage::StorageFile> const& sender,
          winrt::Windows::Foundation::AsyncStatus status) mutable {
        auto report = [weak, dispatcher, on_complete](bool saved,
                                                      std::wstring error) {
          dispatcher.TryEnqueue([weak, on_complete, saved,
                                 error = std::move(error)] {
            if (auto window = weak.get()) {
              if (!error.empty()) {
                window->Alert(pdfgist::Trf("ui.export.failed", error));
              }
              if (on_complete) on_complete(saved);
            }
          });
        };
        if (status == winrt::Windows::Foundation::AsyncStatus::Canceled) {
          report(false, L"");
          return;
        }
        try {
          auto const file = sender.GetResults();
          if (file == nullptr) {
            report(false, L"");
            return;
          }
          if (!protected_original.empty()) {
            std::error_code ec;
            auto const source = std::filesystem::path(protected_original);
            auto const destination = std::filesystem::path(file.Path().c_str());
            if (std::filesystem::equivalent(source, destination, ec)) {
              report(false, pdfgist::Tr("ui.pdf.preserve-original"));
              return;
            }
          }
          winrt::Windows::Storage::FileIO::WriteBytesAsync(file, data).Completed(
              [report](auto const& op,
                       winrt::Windows::Foundation::AsyncStatus write_status) {
                if (write_status ==
                    winrt::Windows::Foundation::AsyncStatus::Completed) {
                  try {
                    op.GetResults();
                    report(true, L"");
                    return;
                  } catch (...) {
                  }
                }
                report(false, L"write");
              });
        } catch (std::exception const& e) {
          report(false, to_wide(e.what()));
        } catch (...) {
          report(false, L"file picker");
        }
      });
}


}  // namespace winrt::RivetHost::implementation
