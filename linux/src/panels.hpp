// Right-hand AI panel: Translate / Summarize / Chat streams plus the
// Settings tab (presets, API key, connection test, reading + appearance
// options). Mirrors macos-host StreamCardView.swift / SettingsPane.swift /
// PDFGistModel.swift AI actions; behavior spec is the macOS host.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "GeneratedBackend.hpp"

namespace pdfgist {

// A page-text source: page/chapter number (1-based) + text.
struct TextSource {
  int page = 0;
  std::string text;
};

// Host-provided accessors over the active reader tab (PDF or EPUB).
struct PanelContext {
  rivet_app::API* api{nullptr};
  rivet_app::SettingsView settings;
  std::vector<rivet_app::ProviderPreset> presets;

  std::function<void(std::string const&)> alert;      // error surface
  std::function<void()> apply_view_mode;              // after settings save
  std::function<std::string()> selection_text;        // "" when none
  std::function<std::optional<TextSource>()> page_text;   // current page
  std::function<std::optional<TextSource>()> doc_text;    // first-12-pages cap
  std::function<bool()> epub_active;                      // page means chapter
  std::string ui_language{"system"};                      // host pref mirror
  double tts_rate{1.0};                                   // host pref mirror
};

// One AI stream (a card in the Translate/Summarize lists or an assistant
// chat bubble). v1 StreamBuffer semantics: reasoning accumulates until the
// first answer delta; a reasoning-only stream ends as dimmed reasoning;
// nothing at all becomes the stopped/empty placeholder.
class AiPanel;

struct StreamCard {
  enum class Phase { streaming, done, empty_stopped, empty, error, notice };

  std::int64_t request_id{-1};
  std::string title;
  std::string meta;
  std::string source_text;
  std::string reasoning;
  std::string content;
  Phase phase{Phase::streaming};
  bool chat{false};
  AiPanel* owner{nullptr};

  GtkWidget* row{nullptr};
  GtkLabel* title_label{nullptr};
  GtkLabel* content_label{nullptr};
  GtkButton* stop_button{nullptr};
  GtkWidget* spinner{nullptr};
};

class AiPanel {
 public:
  explicit AiPanel(PanelContext* ctx);

  GtkWidget* widget() const { return root_; }

  // main.cpp relays Rivet events (already on the UI thread).
  void on_stream_chunk(rivet_app::StreamChunk const& chunk);
  void on_stream_done(rivet_app::StreamDone const& done);
  void on_stream_error(rivet_app::StreamError const& error);

  void open_settings_tab();
  void sync_settings();  // refresh fields from ctx->settings

  // Appearance selector is host-side (theme lives in host.conf, like the
  // macOS host's UserDefaults).
  std::function<void(std::string const&)> on_theme_changed;  // system|light|dark
  std::function<void(std::string const&)> on_reading_theme_changed;  // system|paper|night
  std::function<void(std::string const&)> on_language_changed;  // system|zh|en
  std::function<void(double)> on_tts_rate_changed;
  void set_theme(std::string const& theme);

  // AI actions (menu items and the context menu route here).
  void do_translate_selection();
  void do_translate_text(std::string const& text);  // notes-pane excerpt button
  void do_translate_page();
  void do_summarize_page();
  void do_summarize_selection();
  void do_summarize_doc();

  // Shared card plumbing — public because async completions (and main.cpp's
  // launchers) land here.
  GtkWidget* make_card_row(StreamCard* card);
  StreamCard* new_card(GtkListBox* list, std::string const& title,
                       std::string const& meta, bool chat);
  void refresh_card(StreamCard* card);
  void attach_card(StreamCard* card, rivet_app::StreamStart const& start);
  void finish_stream(std::int64_t request_id, std::string const& error);
  void append_notice(GtkListBox* list, std::string const& message);
  void card_stop_clicked(StreamCard* card);
  void card_copy_clicked(StreamCard* card);
  void finalize_chat(StreamCard* card);
  static std::string describe_error(std::exception_ptr error);
  std::string language_name(rivet_app::TargetLanguage code) const;
  bool ensure_provider(GtkListBox* notice_list);
  void confirm_ai_send(
      rivet_app::AISendAction action, rivet_app::AISendScope scope,
      std::string const& content,
      std::vector<std::vector<std::string>> const& history,
      std::string const& message, std::function<void()> on_continue);

 private:
  PanelContext* ctx_;
  GtkWidget* root_{nullptr};
  GtkWidget* stack_{nullptr};

  // translate / summarize
  GtkListBox* translate_list_{nullptr};
  GtkListBox* summarize_list_{nullptr};
  // chat
  GtkWidget* chat_scroll_{nullptr};
  GtkBox* chat_list_{nullptr};
  GtkEntry* chat_entry_{nullptr};
  GtkDropDown* chat_scope_{nullptr};
  GtkButton* chat_stop_{nullptr};
  bool chat_busy_{false};
  std::vector<std::vector<std::string>> chat_history_;
  std::string chat_pending_question_;
  StreamCard* chat_live_{nullptr};

  // settings pane
  GtkDropDown* preset_dropdown_{nullptr};
  GtkEntry* base_url_{nullptr};
  GtkEntry* model_entry_{nullptr};
  GtkDropDown* model_suggestions_{nullptr};
  GtkEntry* api_key_{nullptr};
  GtkLabel* settings_status_{nullptr};
  GtkDropDown* target_language_{nullptr};
  GtkCheckButton* view_single_{nullptr};
  GtkCheckButton* view_double_{nullptr};
  GtkCheckButton* sidecar_{nullptr};
  GtkDropDown* theme_dropdown_{nullptr};
  GtkDropDown* reading_theme_dropdown_{nullptr};
  GtkDropDown* language_dropdown_{nullptr};
  GtkScale* tts_rate_scale_{nullptr};
  std::map<std::string, std::pair<std::string, std::string>> preset_stash_;
  std::string current_preset_id_;
  bool busy_{false};
  bool syncing_settings_{false};
  struct SyncGuard {
    bool& flag;
    explicit SyncGuard(bool& f) : flag(f) { flag = true; }
    ~SyncGuard() { flag = false; }
  };

  std::set<std::int64_t> stopping_streams_;
  std::map<std::int64_t, StreamCard*> stream_registry_;
  std::vector<std::unique_ptr<StreamCard>> cards_;

  GtkWidget* make_translate_tab();
  GtkWidget* make_summarize_tab();
  GtkWidget* make_chat_tab();
  GtkWidget* make_settings_tab();

  // actions
  void do_send_chat();

  // settings actions
  void settings_switch_preset();
  void settings_save_key();
  void settings_test();
  void settings_save();
  void set_settings_status(std::string const& text, bool is_error);
};

}  // namespace pdfgist
