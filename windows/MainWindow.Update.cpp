// Online update flow for the Booklight window, ported from taskly/podlens's
// MainWindow.Update.cpp. Differences from the family shape are dictated by
// the backend: check-updates/update-state return jsexpr bytes (the mac host
// JSON-decodes the same payloads), and the backend itself throttles the
// silent check to once a day — so the host needs no last-check stamp.
// The install handoff swaps the downloaded portable zip over the install
// directory; MSI installs (Program Files, no write access for the swap) keep
// the manual path: a dialog pointing at the releases page.

#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <shellapi.h>

#include <chrono>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

namespace winrt::RivetHost::implementation {

namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace {

constexpr wchar_t kReleasesUrl[] =
    L"https://github.com/turlamai/booklight/releases";

std::int64_t epoch_seconds() {
  FILETIME now{};
  ::GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER value{};
  value.LowPart = now.dwLowDateTime;
  value.HighPart = now.dwHighDateTime;
  // FILETIME is 100 ns ticks since 1601-01-01; the constant converts to
  // unix epoch seconds.
  return static_cast<std::int64_t>(
      (value.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

std::wstring lowercase(std::wstring text) {
  for (auto& c : text) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  return text;
}

// %TEMP%\pdfgist-update — handoff script, extract dir, and markers live
// here. Update state must not pollute the data directory.
std::filesystem::path update_work_dir() {
  wchar_t buffer[MAX_PATH]{};
  std::filesystem::path base;
  DWORD length = ::GetEnvironmentVariableW(L"TEMP", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    base = std::filesystem::path(buffer);
  } else {
    length = ::GetEnvironmentVariableW(L"TMP", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      base = std::filesystem::path(buffer);
    }
  }
  if (base.empty()) {
    return {};
  }
  auto const dir = base / L"pdfgist-update";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path update_failure_marker() {
  return update_work_dir() / L"update-failed.txt";
}

std::filesystem::path update_success_marker() {
  return update_work_dir() / L"update-success.txt";
}

// The detached handoff batch. ASCII on purpose: cmd parses batches in the
// ANSI code page, so paths travel as arguments instead of being embedded in
// the script. Args: %1 zip, %2 install dir, %3 exe, %4 failure marker,
// %5 success marker. The rivet release zip carries one top-level directory
// (pdfgist-<ver>-windows-<arch>/…), so extract-then-flatten picks the
// payload dir before the swap. Order matters: extract and verify BEFORE
// touching the old install, so a corrupt zip can never break the running
// copy.
std::string update_handoff_batch(unsigned long pid) {
  std::string const pid_text = std::to_string(pid);
  std::string batch;
  batch += "@echo off\r\n";
  batch += "rem Booklight update handoff - auto-generated, safe to delete.\r\n";
  batch += "set /a n=0\r\n";
  batch += ":wait\r\n";
  batch += "tasklist /FI \"PID eq " + pid_text + "\" 2>nul | find \"" +
           pid_text + "\" >nul\r\n";
  batch += "if errorlevel 1 goto extract\r\n";
  batch += "ping -n 2 127.0.0.1 >nul\r\n";
  batch += "set /a n+=1\r\n";
  batch += "if %n% LSS 30 goto wait\r\n";
  // The app never freed itself within 30 s: leave it running and report.
  batch += "> \"%~4\" echo process-exit-timeout\r\n";
  batch += "exit /b 1\r\n";
  batch += ":extract\r\n";
  batch += "set \"extract=%~dp0extract\"\r\n";
  batch += "if exist \"%extract%\" rmdir /s /q \"%extract%\"\r\n";
  batch += "mkdir \"%extract%\" 2>nul\r\n";
  batch += "tar -xf \"%~1\" -C \"%extract%\"\r\n";
  batch += "if errorlevel 1 goto fail_extract\r\n";
  batch += "set \"payload=%extract%\"\r\n";
  batch += "for /d %%D in (\"%extract%\\*\") do set \"payload=%%D\"\r\n";
  batch += "if exist \"%~2.old\" rmdir /s /q \"%~2.old\"\r\n";
  batch += "move \"%~2\" \"%~2.old\" >nul 2>&1\r\n";
  batch += "if errorlevel 1 goto fail_rename\r\n";
  batch += "robocopy \"%payload%\" \"%~2\" /E /MOVE /NFL /NDL /NJH /NJS /NP >nul\r\n";
  batch += "if errorlevel 8 goto fail_swap\r\n";
  batch += "start \"\" \"%~3\"\r\n";
  batch += "> \"%~5\" echo ok\r\n";
  batch += "exit /b 0\r\n";
  batch += ":fail_swap\r\n";
  batch += "> \"%~4\" echo swap-failed (%errorlevel%)\r\n";
  batch += "move \"%~2.old\" \"%~2\" >nul 2>&1\r\n";
  batch += "goto restart_old\r\n";
  batch += ":fail_rename\r\n";
  batch += "> \"%~4\" echo rename-failed (%errorlevel%)\r\n";
  batch += "goto restart_old\r\n";
  batch += ":fail_extract\r\n";
  batch += "> \"%~4\" echo extract-failed (%errorlevel%)\r\n";
  batch += ":restart_old\r\n";
  batch += "start \"\" \"%~3\"\r\n";
  batch += "exit /b 1\r\n";
  return batch;
}

void open_releases_page() {
  ::ShellExecuteW(nullptr, L"open", kReleasesUrl, nullptr, nullptr,
                  SW_SHOWNORMAL);
}

// The updater payloads are machine-generated JSON
// ({"status":"available","currentVersion":...,"sizeBytes":1800000,...}); a
// keyed regex beats carrying a JSON parser in the host.
std::wstring JsonStringField(std::string const& json, char const* key) {
  std::regex const re(std::string("\"") + key + "\":\"((?:[^\"\\\\]|\\\\.)*)\"");
  std::smatch match;
  if (!std::regex_search(json, match, re)) return {};
  std::string raw = match[1].str();
  // minimal unescape for \n \" \\ sequences
  std::string out;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      ++i;
      switch (raw[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        default: out += raw[i];
      }
    } else {
      out += raw[i];
    }
  }
  return to_wide(out);
}

std::int64_t JsonIntField(std::string const& json, char const* key) {
  std::regex const re(std::string("\"") + key + "\":(-?[0-9]+)");
  std::smatch match;
  if (!std::regex_search(json, match, re)) return -1;
  try {
    return std::stoll(match[1].str());
  } catch (...) {
    return -1;
  }
}

std::wstring JsonStatusField(std::string const& json) {
  std::regex const re(std::string("\"status\":\"([a-z-]+)\""));
  std::smatch match;
  if (!std::regex_search(json, match, re)) return {};
  return to_wide(match[1].str());
}

std::wstring FormatSize(std::int64_t bytes) {
  wchar_t buffer[64]{};
  if (bytes >= 1024 * 1024) {
    swprintf(buffer, 64, L"%.1f MB",
             static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes > 0) {
    swprintf(buffer, 64, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    return L"";
  }
  return buffer;
}

}  // namespace

// ---------- entry points ------------------------------------------------------

// Startup path: a few seconds after ready, fire the throttled silent check.
// The backend enforces the daily window (check-updates force=false answers
// "throttled" without a network round trip beyond the manifest).
void MainWindow::StartAutoUpdateCheck() {
  if (backend_ == nullptr || !backend_->running()) {
    return;
  }
  check_timer_ = DispatcherQueue().CreateTimer();
  check_timer_.Interval(std::chrono::seconds{3});
  check_timer_.IsRepeating(false);
  check_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->RunUpdateCheck(/*manual=*/false);
    }
  });
  check_timer_.Start();
}

// A manual check surfaces every outcome; a silent (launch-time) check only
// reports an available update through the consent dialog — never nags.
void MainWindow::RunUpdateCheck(bool manual) {
  if (backend_ == nullptr || !backend_->running() || update_downloading_) {
    return;
  }
  if (IsDevCopy()) {
    if (manual) {
      ShowDialog(pdfgist::Tr("app.name"),
                 pdfgist::Tr("update.dev-copy"),
                 pdfgist::Tr("ui.update.check-menu"),
                 pdfgist::Tr("ui.password.cancel"), [] { open_releases_page(); });
    }
    return;
  }
  if (IsMsiInstall()) {
    if (manual) {
      ShowDialog(pdfgist::Tr("app.name"),
                 pdfgist::Tr("update.msi-installed"),
                 pdfgist::Tr("ui.update.check-menu"),
                 pdfgist::Tr("ui.password.cancel"), [] { open_releases_page(); });
    }
    return;
  }
  if (manual) {
    SetStatus(true, pdfgist::Tr("ui.update.checking"));
  }
  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend, manual]() mutable {
    std::string failure;
    std::string payload;
    try {
      rivet_app::API api(*backend);
      auto bytes = api.check_updates(!manual).get();
      payload.assign(bytes.begin(), bytes.end());
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue([weak, manual, payload = std::move(payload),
                           failure = std::move(failure)]() mutable {
      auto window = weak.get();
      if (!window) return;
      if (manual) window->SetStatus(true, L"");
      if (!failure.empty()) {
        if (manual) {
          window->Alert(pdfgist::Trf("update.check_failed", to_wide(failure)));
        }
        return;
      }
      auto const status = JsonStatusField(payload);
      if (status == L"available") {
        auto const version = JsonStringField(payload, "availableVersion");
        auto const size = JsonIntField(payload, "sizeBytes");
        std::wstring body =
            pdfgist::Trf("update.available_body",
                         version.empty() ? L"" : version);
        if (size > 0) {
          body += L"\n" + FormatSize(size);
        }
        window->ShowDialog(
            pdfgist::Tr("ui.update.available"), body,
            pdfgist::Tr("ui.update.download"),
            pdfgist::Tr("ui.password.cancel"),
            [weak, size, version]() {
              if (auto current = weak.get()) {
                current->StartUpdateDownload(size, version);
              }
            });
        return;
      }
      if (status == L"throttled" || status.empty()) {
        return;  // silent path or unknown — stay quiet
      }
      if (status == L"up-to-date") {
        if (manual) {
          window->ShowDialog(pdfgist::Tr("ui.update.check-menu"),
                             pdfgist::Tr("ui.update.up-to-date"), L"",
                             pdfgist::Tr("ui.update.close"), nullptr);
        }
        return;
      }
      if (status == L"error" && manual) {
        window->Alert(JsonStringField(payload, "message"));
      }
    });
  }).detach();
}

// ---------- download -----------------------------------------------------------

// Nothing downloads before the user picks "Download" — an update is an
// offer, never an ambient side effect.
void MainWindow::StartUpdateDownload(std::int64_t size_bytes,
                                     std::wstring const& version) {
  if (backend_ == nullptr || !backend_->running() || update_downloading_) {
    return;
  }
  update_downloading_ = true;
  SetStatus(true, pdfgist::Tr("ui.update.downloading"));

  auto const backend = backend_;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend]() mutable {
    std::string failure;
    try {
      rivet_app::API api(*backend);
      (void)api.start_download().get();
    } catch (std::exception const& e) {
      failure = e.what();
    }
    if (!failure.empty()) {
      dispatcher.TryEnqueue([weak, failure = std::move(failure)] {
        if (auto window = weak.get()) {
          window->FailDownload(to_wide(failure));
        }
      });
    }
  }).detach();

  // Poll update_state while the backend downloads (taskly's 400 ms timer).
  update_timer_ = DispatcherQueue().CreateTimer();
  update_timer_.Interval(std::chrono::milliseconds{400});
  update_timer_.Tick([weak, backend](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->PollUpdateState(backend);
    }
  });
  update_timer_.Start();
  (void)size_bytes;
  (void)version;
}

void MainWindow::PollUpdateState(
    std::shared_ptr<rivet::windows::Backend> const& backend) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend]() mutable {
    std::string payload;
    try {
      rivet_app::API api(*backend);
      auto bytes = api.update_state().get();
      payload.assign(bytes.begin(), bytes.end());
    } catch (...) {
      return;  // a transient RPC failure is not a download failure; the
               // next tick retries
    }
    dispatcher.TryEnqueue([weak, payload = std::move(payload)]() mutable {
      if (auto window = weak.get()) {
        window->HandleUpdatePoll(payload);
      }
    });
  }).detach();
}

void MainWindow::HandleUpdatePoll(std::string const& payload) {
  if (!update_downloading_) return;
  std::regex const phase_re("\"phase\":\"([a-z]+)\"");
  std::smatch match;
  std::string phase;
  if (std::regex_search(payload, match, phase_re)) {
    phase = match[1].str();
  }
  if (phase == "downloaded") {
    StopUpdateTimer();
    update_downloading_ = false;
    auto const path = JsonStringField(payload, "downloadedPath");
    if (path.empty()) {
      FailDownload(L"downloaded file path missing");
      return;
    }
    SetStatus(true, pdfgist::Tr("ui.update.downloaded"));
    ShowInstallConsent(path);
    return;
  }
  if (phase == "error") {
    StopUpdateTimer();
    update_downloading_ = false;
    FailDownload(JsonStringField(payload, "message"));
    return;
  }
  if (phase == "downloading") {
    auto const percent = JsonIntField(payload, "percent");
    wchar_t buffer[128];
    swprintf(buffer, 128, pdfgist::Tr("update.downloading_percent").c_str(),
             static_cast<int>(percent < 0 ? 0 : percent));
    SetStatus(true, buffer);
  }
}

void MainWindow::StopUpdateTimer() {
  if (update_timer_) {
    update_timer_.Stop();
    update_timer_ = nullptr;
  }
}

void MainWindow::FailDownload(std::wstring const& message) {
  update_downloading_ = false;
  StopUpdateTimer();
  SetStatus(true, L"");
  ShowDialog(pdfgist::Tr("app.name"),
             pdfgist::Trf("update.install_failed", message), L"",
             pdfgist::Tr("ui.update.close"), nullptr,
             pdfgist::Tr("update.copy_error"),
             [message] {
               try {
                 winrt::Windows::ApplicationModel::DataTransfer::DataPackage
                     package;
                 package.SetText(message);
                 winrt::Windows::ApplicationModel::DataTransfer::Clipboard::
                     SetContent(package);
               } catch (...) {
               }
             });
}

// ---------- install handoff ------------------------------------------------------

// Installing is a separate, explicit step: the download is complete and
// verified (backend-side, size + SHA-256 against the signed manifest), and
// only the user's "Quit and install" closes the app.
void MainWindow::ShowInstallConsent(std::wstring const& zip_path) {
  ShowDialog(pdfgist::Tr("ui.update.downloaded"),
             pdfgist::Tr("update.ready_body"),
             pdfgist::Tr("ui.update.install-now"),
             pdfgist::Tr("ui.password.cancel"),
             [weak = get_weak(), zip_path] {
               if (auto window = weak.get()) {
                 window->InstallDownloadedUpdate(zip_path);
               }
             });
}

void MainWindow::InstallDownloadedUpdate(std::wstring const& zip_path) {
  auto const work = update_work_dir();
  if (work.empty()) {
    ShowDialog(pdfgist::Tr("app.name"),
               pdfgist::Trf("update.install_failed", L"temp"), L"",
               pdfgist::Tr("ui.update.close"), nullptr);
    return;
  }
  std::error_code ec;
  auto const script = work / L"update-install.cmd";
  // Stale markers from earlier attempts must not survive this one.
  std::filesystem::remove(update_failure_marker(), ec);
  std::filesystem::remove(update_success_marker(), ec);

  {
    std::ofstream file(script, std::ios::binary | std::ios::trunc);
    if (!file) {
      ShowDialog(pdfgist::Tr("app.name"),
                 pdfgist::Trf("update.install_failed", L"script"), L"",
                 pdfgist::Tr("ui.update.close"), nullptr);
      return;
    }
    auto const batch = update_handoff_batch(::GetCurrentProcessId());
    file.write(batch.data(), static_cast<std::streamsize>(batch.size()));
    file.close();
    if (!file) {
      ShowDialog(pdfgist::Tr("app.name"),
                 pdfgist::Trf("update.install_failed", L"script"), L"",
                 pdfgist::Tr("ui.update.close"), nullptr);
      return;
    }
  }

  std::wstring const install_dir = executable_path().parent_path().wstring();
  std::wstring const exe = executable_path().wstring();
  std::wstring const parameters =
      L"/c call \"" + script.wstring() + L"\" \"" + zip_path + L"\" \"" +
      install_dir + L"\" \"" + exe + L"\" \"" +
      update_failure_marker().wstring() + L"\" \"" +
      update_success_marker().wstring() + L"\"";
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = L"cmd.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    ShowDialog(pdfgist::Tr("app.name"),
               pdfgist::Trf("update.install_failed", L"handoff"), L"",
               pdfgist::Tr("ui.update.close"), nullptr);
    return;
  }
  // The handoff script takes it from here: it waits for this process to
  // exit, swaps the zip in, relaunches, and leaves markers behind either
  // way (a failure marker is HandleInstallMarkers' input on next launch).
  winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

// Surface a previous failed install once: read the error the handoff script
// left behind, clear the marker, and report. A success marker means the
// swapped-in version launched — retire the .old fallback copy.
void MainWindow::HandleInstallMarkers() {
  auto const work = update_work_dir();
  if (work.empty()) {
    return;
  }
  std::error_code ec;
  auto const success = update_success_marker();
  if (std::filesystem::exists(success, ec)) {
    std::filesystem::remove(success, ec);
    try {
      auto const install_dir = executable_path().parent_path();
      std::filesystem::remove_all(
          std::filesystem::path(install_dir.wstring() + L".old"), ec);
    } catch (...) {
      // The fallback copy is expendable; never block startup on it.
    }
    return;
  }
  auto const marker = update_failure_marker();
  if (!std::filesystem::exists(marker, ec)) {
    return;
  }
  std::ifstream file(marker);
  std::string code;
  std::getline(file, code);
  file.close();
  std::filesystem::remove(marker, ec);
  ShowDialog(pdfgist::Tr("app.name"),
             pdfgist::Trf("update.install_failed",
                          to_wide(code.empty() ? std::string("unknown")
                                               : code)),
             L"", pdfgist::Tr("ui.update.close"), nullptr,
             pdfgist::Tr("update.copy_error"), nullptr);
}

// 便携 zip 语义: a copy without the packaged layout (embedded backend at
// res/core.zo) or one running from a temp path is a dev copy — auto-update
// is unavailable there.
bool MainWindow::IsDevCopy() {
  try {
    auto const install_dir = executable_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::exists(install_dir / L"res" / L"core.zo", ec)) {
      return true;
    }
    return lowercase(install_dir.wstring()).find(L"temp") != std::wstring::npos;
  } catch (...) {
    return true;
  }
}

// MSI 安装版语义: the MSI lands under Program Files, where the in-place
// zip swap has no write access — those installs get the manual path
// (per-user zip installs stay fully automatic).
bool MainWindow::IsMsiInstall() {
  wchar_t buffer[32768]{};
  auto const under_program_files = [&](wchar_t const* variable) {
    DWORD const length = ::GetEnvironmentVariableW(variable, buffer, 32768);
    if (length == 0 || length >= 32768) return false;
    std::wstring program_files = lowercase(std::wstring{buffer});
    while (!program_files.empty() && program_files.back() == L'\\') {
      program_files.pop_back();
    }
    if (program_files.empty()) return false;
    std::wstring const directory =
        lowercase(executable_path().parent_path().wstring());
    return directory == program_files ||
           directory.rfind(program_files + L"\\", 0) == 0;
  };
  return under_program_files(L"ProgramFiles") ||
         under_program_files(L"ProgramFiles(x86)") ||
         under_program_files(L"ProgramW6432");
}

}  // namespace winrt::RivetHost::implementation
