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
namespace wut = winrt::Windows::UI::Text;

using Microsoft::UI::Xaml::Visibility;

namespace {
std::wstring pdf_stem(std::wstring const& title) {
  std::wstring const suffix = L".pdf";
  if (title.size() >= suffix.size() &&
      title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return title.substr(0, title.size() - suffix.size());
  }
  return title;
}
}  // namespace

// ---- page ops row (the v1 editor.ts buttons; ops target the current page) ------

void MainWindow::BuildPageOpsRow() {
  page_ops_row_ = muxc::StackPanel();
  page_ops_row_.Spacing(4);
  page_ops_row_.Margin({0, 8, 0, 4});

  auto title = muxc::TextBlock();
  title.Text(pdfgist::Tr("ui.pages.title"));
  title.FontSize(11);
  title.FontWeight(wut::FontWeights::SemiBold());
  title.Opacity(0.7);
  page_ops_row_.Children().Append(title);

  auto const weak = get_weak();
  struct Op {
    char const* key;
    int op;
  };
  Op const ops_row1[] = {{"ui.pages.rotate", 0}, {"ui.pages.blank", 1},
                         {"ui.pages.delete", 2}};
  for (auto const& item : ops_row1) {
    auto button = muxc::Button();
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(item.key));
    label.FontSize(11);
    button.Content(label);
    button.Padding({7, 2, 7, 2});
    button.MinWidth(0);
    button.MinHeight(0);
    int const op = item.op;
    button.Click([weak, op](winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->RunPageEdit(op);
    });
    page_ops_row_.Children().Append(button);
  }

  auto row2 = muxc::StackPanel();
  row2.Orientation(muxc::Orientation::Horizontal);
  row2.Spacing(4);
  Op const ops_row2[] = {{"ui.pages.extract", 3}, {"ui.pages.merge", 4},
                         {"ui.pages.bake", 5}};
  for (auto const& item : ops_row2) {
    auto button = muxc::Button();
    muxc::TextBlock label;
    label.Text(pdfgist::Tr(item.key));
    label.FontSize(11);
    button.Content(label);
    button.Padding({7, 2, 7, 2});
    button.MinWidth(0);
    button.MinHeight(0);
    int const op = item.op;
    button.Click([weak, op](winrt::Windows::Foundation::IInspectable const&,
                            mux::RoutedEventArgs const&) {
      if (auto window = weak.get()) window->RunPageEdit(op);
    });
    row2.Children().Append(button);
  }
  page_ops_row_.Children().Append(row2);

  auto hint = muxc::TextBlock();
  hint.Name(L"PageOpsHint");
  hint.FontSize(10);
  hint.Opacity(0.5);
  page_ops_hint_ = hint;
  page_ops_row_.Children().Append(hint);
}

void MainWindow::RunPageEdit(int op) {
  auto const tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab || !ready_ || !api_ || !backend_) return;
  int64_t const page = tab->current_page;
  auto const path_utf8 = to_utf8(tab->path);
  auto const title = tab->title;
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  // extract / bake go through a save dialog instead of reloading the tab
  if (op == 3) {
    std::thread([weak, dispatcher, backend, path_utf8, page, title]() mutable {
      std::string failure;
      rivet::Bytes data;
      try {
        rivet_app::API api(*backend);
        data = api.edit_extract_pages(path_utf8, {page}).get();
      } catch (std::exception const& e) {
        failure = e.what();
      }
      dispatcher.TryEnqueue([weak, data = std::move(data),
                             failure = std::move(failure), title]() mutable {
        auto window = weak.get();
        if (!window) return;
        if (!failure.empty()) {
          window->Alert(
              pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
          return;
        }
        window->SaveEditResult(
            std::move(data),
            pdf_stem(title) + pdfgist::Tr("ui.pages.extract-suffix"));
      });
    }).detach();
    return;
  }
  if (op == 4) {
    RunMergeDialog();
    return;
  }
  if (op == 5) {
    RunBakeDialog();
    return;
  }

  std::thread([weak, dispatcher, backend, path_utf8, page, op]() mutable {
    std::string failure;
    rivet::Bytes data;
    try {
      rivet_app::API api(*backend);
      switch (op) {
        case 0:
          data = api.edit_rotate_pages(path_utf8, {page}, 90).get();
          break;
        case 1:
          data = api.edit_insert_blank_after(path_utf8, page).get();
          break;
        case 2:
          data = api.edit_delete_pages(path_utf8, {page}).get();
          break;
        default:
          return;
      }
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, data = std::move(data),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (!failure.empty()) {
        window->Alert(pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
        return;
      }
      // Reload the same tab in memory (mac reloadTabDocument parity).
      for (auto const& candidate : window->tabs_) {
        if (window->active_pdf_.get() == candidate.get()) {
          window->ReloadTabDocument(candidate, std::move(data));
          return;
        }
      }
    });
  }).detach();
}

void MainWindow::ReloadTabDocument(std::shared_ptr<PdfTab> tab,
                                  std::vector<std::uint8_t> data) {
  pdfgist::PdfOpenError error = pdfgist::PdfOpenError::None;
  auto doc = pdfgist::PdfDocument::OpenFromBytes(std::move(data), "", &error);
  if (!doc) {
    Alert(pdfgist::Trf("ui.pages.edit-failed",
                       pdfgist::Tr("backend.error.bad-annotations")));
    return;
  }
  tab->doc = std::move(doc);
  tab->pdf_edits_pending = true;  // only an explicit export persists edits
  if (auto label = tab->tab_item.Header().try_as<muxc::TextBlock>()) {
    label.Text(tab->title + L" •");
  }
  tab->page_count = tab->doc->PageCount();
  tab->current_page = 1;
  ClearSelection(*tab);
  tab->find_rects.clear();
  find_hits_.clear();
  tab->layout_viewport_w = -1;  // force relayout with the new page sizes
  LayoutTab(*tab);
  RequestVisibleRenders(*tab);
  UpdatePageBar();
  RefreshLeftPanel();
}

void MainWindow::SaveEditResult(std::vector<std::uint8_t> data,
                                std::wstring const& default_name) {
  SaveData(std::move(data), default_name, L"PDF", L".pdf");
}

void MainWindow::PickPdfFile(std::function<void(std::wstring const&)> on_picked) {
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
  auto const dispatcher = DispatcherQueue();
  picker.PickSingleFileAsync().Completed(
      [dispatcher, on_picked = std::move(on_picked)](
          winrt::Windows::Foundation::IAsyncOperation<
              winrt::Windows::Storage::StorageFile> const& sender,
          winrt::Windows::Foundation::AsyncStatus) {
        try {
          auto const file = sender.GetResults();
          if (file == nullptr) return;
          // Never touch current tab state from a picker completion worker.
          dispatcher.TryEnqueue(
              [on_picked, path = std::wstring(file.Path())]() {
                on_picked(path);
              });
        } catch (...) {
        }
      });
}

void MainWindow::RunMergeDialog() {
  auto const tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab) return;
  PickPdfFile([weak = get_weak()](std::wstring const& other_path) {
    if (auto window = weak.get()) {
      auto const target = window->epub_active_ ? nullptr : window->active_pdf_.get();
      if (!target || other_path.empty()) return;
      auto const backend = window->backend_;
      auto const dispatcher = window->DispatcherQueue();
      auto const path_utf8 = to_utf8(target->path);
      auto const other_utf8 = to_utf8(other_path);
      std::thread([weak, dispatcher, backend, path_utf8, other_utf8]() mutable {
        std::string failure;
        rivet::Bytes data;
        try {
          rivet_app::API api(*backend);
          data = api.edit_append_doc(path_utf8, other_utf8).get();
        } catch (std::exception const& e) {
          failure = e.what();
        }
        dispatcher.TryEnqueue([weak, data = std::move(data),
                               failure = std::move(failure)]() mutable {
          auto window2 = weak.get();
          if (!window2) return;
          if (!failure.empty()) {
            window2->Alert(
                pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
            return;
          }
          for (auto const& candidate : window2->tabs_) {
            if (window2->active_pdf_.get() == candidate.get()) {
              window2->ReloadTabDocument(candidate, std::move(data));
              return;
            }
          }
        });
      }).detach();
    }
  });
}

// Bake: watermark text over the whole document (backend bake RPC; the result
// goes through a save dialog — the source file is never touched).
void MainWindow::RunBakeDialog() {
  auto const tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab) return;

  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(pdfgist::Tr("ui.pages.bake")));

  auto stack = muxc::StackPanel();
  stack.Spacing(8);
  stack.MinWidth(300);

  muxc::TextBlock text_label;
  text_label.Text(pdfgist::Tr("ui.pages.bake-text-label"));
  text_label.FontSize(12);
  stack.Children().Append(text_label);

  auto input = muxc::TextBox();
  stack.Children().Append(input);

  muxc::TextBlock size_label;
  size_label.Text(pdfgist::Tr("ui.pages.bake-size-label"));
  size_label.FontSize(12);
  stack.Children().Append(size_label);

  auto size_input = muxc::TextBox();
  size_input.Text(L"48");
  stack.Children().Append(size_input);

  dialog.Content(stack);
  dialog.PrimaryButtonText(pdfgist::Tr("ui.pages.bake-run"));
  dialog.CloseButtonText(pdfgist::Tr("ui.password.cancel"));
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  auto const weak = get_weak();
  op.Completed([weak, input, size_input](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    muxc::ContentDialogResult result{muxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      return;
    }
    auto window = weak.get();
    if (!window || result != muxc::ContentDialogResult::Primary) return;
    auto const target = window->epub_active_ ? nullptr : window->active_pdf_.get();
    if (!target) return;
    std::wstring const text(input.Text());
    int const size = [&] {
      try {
        return std::max(8, std::min(200, std::stoi(std::wstring(size_input.Text()))));
      } catch (...) {
        return 48;
      }
    }();
    if (text.empty()) return;

    auto const backend = window->backend_;
    auto const dispatcher = window->DispatcherQueue();
    auto const path_utf8 = to_utf8(target->path);
    auto const title = target->title;
    std::thread([weak, dispatcher, backend, path_utf8, title,
                 text_utf8 = to_utf8(text), size]() mutable {
      std::string failure;
      rivet::Bytes data;
      try {
        rivet_app::API api(*backend);
        data = api.edit_bake_text(path_utf8, text_utf8, size, 300, {}).get();
      } catch (std::exception const& e) {
        failure = e.what();
      }
      dispatcher.TryEnqueue([weak, data = std::move(data),
                             failure = std::move(failure), title]() mutable {
        auto window2 = weak.get();
        if (!window2) return;
        if (!failure.empty()) {
          window2->Alert(
              pdfgist::Trf("ui.pages.edit-failed", to_wide(failure)));
          return;
        }
        window2->SaveEditResult(
            std::move(data),
            pdf_stem(title) + pdfgist::Tr("ui.pages.bake-suffix"));
      });
    }).detach();
  });
}

}  // namespace winrt::RivetHost::implementation
