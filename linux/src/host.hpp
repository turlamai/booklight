// Shared host plumbing: UI dispatch, install shape, host-side prefs.
//
// The macOS host is the behavior spec (PDFGistModel.swift); this file holds
// only what several Linux-host translation units share. UI copy comes from
// the shared i18n source via i18n.h (Tr/Trf); model/backend-facing strings
// stay hardcoded by design (windows parity).
#pragma once

#include <glib.h>

#include <filesystem>
#include <functional>
#include <string>

namespace host {

// Runs `run` on the main loop (thread-safe; g_idle_add from any thread).
void dispatch_ui(std::function<void()> run);

std::filesystem::path executable_path();

std::filesystem::path app_config_dir();  // ~/.config/site.jrtx.pdfgist

// Install shape for the update flow: the AppImage runtime exports APPIMAGE
// pointing at the running image; deb lands under /opt/pdfgist and rpm under
// /usr — those upgrade through the package manager; everything else is the
// portable tarball layout. Matches app/updater.rkt's flavor detection.
std::string install_shape();

// Host-side preferences (theme, panel widths) — the backend owns settings
// that the RPC surface exposes; these stay host-local in
// <config-dir>/host.conf (GKeyFile), mirroring the macOS host's
// UserDefaults usage.
std::string pref_string(std::string const& key, std::string const& fallback);
void save_pref(std::string const& key, std::string const& value);
int pref_int(std::string const& key, int fallback);
void save_pref_int(std::string const& key, int value);

// String formatting with {0} positional substitution (family i18n rule).
std::string format_positional(std::string const& pattern,
                              std::string const& arg0);
std::string format_positional(std::string const& pattern,
                              std::string const& arg0,
                              std::string const& arg1);

// Human file size (ByteCountFormatter parity, file style).
std::string human_size(std::int64_t bytes);

}  // namespace host
