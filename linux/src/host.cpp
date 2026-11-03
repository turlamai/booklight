// Shared host plumbing — see host.hpp.
#include "host.hpp"

#include <glib/gi18n.h>

#include <cstdio>
#include <mutex>
#include <utility>

namespace host {
namespace {

struct UiTask {
  std::function<void()> run;
};

int run_ui_task(gpointer user_data) {
  auto* task = static_cast<UiTask*>(user_data);
  task->run();
  delete task;
  return G_SOURCE_REMOVE;
}

GKeyFile* pref_file() {
  static GKeyFile* file = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    file = g_key_file_new();
    auto const path = app_config_dir() / "host.conf";
    g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr);
  });
  return file;
}

void persist_prefs() {
  auto const dir = app_config_dir();
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto const path = dir / "host.conf";
  gsize length = 0;
  if (gchar* data =
          g_key_file_to_data(pref_file(), &length, nullptr)) {
    GError* error = nullptr;
    g_file_set_contents(path.c_str(), data, static_cast<gssize>(length),
                        &error);
    if (error != nullptr) g_error_free(error);
    g_free(data);
  }
}

}  // namespace

void dispatch_ui(std::function<void()> run) {
  g_idle_add(run_ui_task, new UiTask{std::move(run)});
}

std::filesystem::path executable_path() {
  std::error_code ec;
  auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return {};
  return self;
}

std::filesystem::path app_config_dir() {
  char const* config_home = g_get_user_config_dir();
  return std::filesystem::path(config_home != nullptr ? config_home : ".") /
         "site.jrtx.pdfgist";
}

std::string install_shape() {
  char const* appimage = getenv("APPIMAGE");
  if (appimage != nullptr && appimage[0] != '\0') return "appimage";
  auto const exe = executable_path().string();
  if (exe.rfind("/opt/pdfgist/", 0) == 0 || exe.rfind("/usr/", 0) == 0) {
    return "package";
  }
  return "tarball";
}

std::string pref_string(std::string const& key, std::string const& fallback) {
  GError* error = nullptr;
  gchar* value = g_key_file_get_string(pref_file(), "host", key.c_str(),
                                       &error);
  if (error != nullptr) {
    g_error_free(error);
    return fallback;
  }
  std::string out = value != nullptr ? value : fallback;
  g_free(value);
  return out;
}

void save_pref(std::string const& key, std::string const& value) {
  g_key_file_set_string(pref_file(), "host", key.c_str(), value.c_str());
  persist_prefs();
}

int pref_int(std::string const& key, int fallback) {
  GError* error = nullptr;
  int value = g_key_file_get_integer(pref_file(), "host", key.c_str(), &error);
  if (error != nullptr) {
    g_error_free(error);
    return fallback;
  }
  return value;
}

void save_pref_int(std::string const& key, int value) {
  g_key_file_set_integer(pref_file(), "host", key.c_str(), value);
  persist_prefs();
}

std::string format_positional(std::string const& pattern,
                              std::string const& arg0) {
  std::string out;
  out.reserve(pattern.size() + arg0.size());
  for (std::size_t i = 0; i < pattern.size();) {
    if (pattern.compare(i, 3, "{0}") == 0) {
      out += arg0;
      i += 3;
    } else {
      out += pattern[i++];
    }
  }
  return out;
}

std::string format_positional(std::string const& pattern,
                              std::string const& arg0,
                              std::string const& arg1) {
  std::string out;
  out.reserve(pattern.size() + arg0.size() + arg1.size());
  for (std::size_t i = 0; i < pattern.size();) {
    if (pattern.compare(i, 3, "{0}") == 0) {
      out += arg0;
      i += 3;
    } else if (pattern.compare(i, 3, "{1}") == 0) {
      out += arg1;
      i += 3;
    } else {
      out += pattern[i++];
    }
  }
  return out;
}

std::string human_size(std::int64_t bytes) {
  char buffer[32];
  if (bytes < 1024) {
    std::snprintf(buffer, sizeof(buffer), "%lld B",
                  static_cast<long long>(bytes));
  } else if (bytes < 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.0f KB",
                  static_cast<double>(bytes) / 1024.0);
  } else if (bytes < 1024LL * 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f GB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  }
  return buffer;
}

}  // namespace host
