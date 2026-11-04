// AI panel + settings pane implementation — see panels.hpp.
#include "panels.hpp"

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "host.hpp"
#include "i18n.h"

#include <algorithm>
#include <exception>

namespace pdfgist {

// g_signal_connect is a GLib macro; a lambda whose body contains top-level
// commas would break its argument splitting. Route through the real
// g_signal_connect_data function instead (handlers cast to GCallback).
inline gulong hook(gpointer instance, gchar const* detailed_signal,
                   auto&& handler, gpointer data,
                   GClosureNotify destroy_data, GConnectFlags flags) {
  return g_signal_connect_data(instance, detailed_signal,
                               reinterpret_cast<GCallback>(
                                   std::forward<decltype(handler)>(handler)),
                               data, destroy_data, flags);
}

inline gulong hook(gpointer instance, gchar const* detailed_signal,
                   auto&& handler, gpointer data) {
  return hook(instance, detailed_signal, std::forward<decltype(handler)>(handler),
              data, nullptr, static_cast<GConnectFlags>(0));
}
namespace {

std::string trim(std::string const& s) {
  auto const begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  auto const end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

GtkWidget* small_button(char const* label) {
  auto* button = gtk_button_new_with_label(label);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  return button;
}

GtkWidget* section_label(char const* text) {
  auto* label = gtk_label_new(text);
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  return label;
}

// Escapes markup-significant characters for a Pango-marked-up label.
std::string escape_markup(std::string const& text) {
  std::string out;
  out.reserve(text.size());
  for (char const c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- setup --

AiPanel::AiPanel(PanelContext* ctx) : ctx_(ctx) {
  root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  auto* switcher = gtk_stack_switcher_new();
  stack_ = gtk_stack_new();
  gtk_stack_set_hhomogeneous(GTK_STACK(stack_), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(stack_), FALSE);
  gtk_stack_add_titled(GTK_STACK(stack_), make_translate_tab(), "translate",
                       Tr("ui.tab.translate").c_str());
  gtk_stack_add_titled(GTK_STACK(stack_), make_summarize_tab(), "summarize",
                       Tr("ui.tab.summarize").c_str());
  gtk_stack_add_titled(GTK_STACK(stack_), make_chat_tab(), "chat",
                       Tr("ui.tab.chat").c_str());
  gtk_stack_add_titled(GTK_STACK(stack_), make_settings_tab(), "settings",
                       Tr("ui.tab.settings").c_str());
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher),
                               GTK_STACK(stack_));
  auto* switcher_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(switcher_wrap, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(switcher_wrap), switcher);
  gtk_box_append(GTK_BOX(root_), switcher_wrap);
  gtk_box_append(GTK_BOX(root_),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(root_), stack_);
}

GtkWidget* AiPanel::make_translate_tab() {
  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(root, 10);
  gtk_widget_set_margin_bottom(root, 10);
  gtk_widget_set_margin_start(root, 10);
  gtk_widget_set_margin_end(root, 10);

  auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto* page = small_button(Tr("ui.translate.page").c_str());
  auto* selection = small_button(Tr("ui.menu.translate").c_str());
  gtk_box_append(GTK_BOX(actions), page);
  gtk_box_append(GTK_BOX(actions), selection);
  gtk_box_append(GTK_BOX(root), actions);
  hook(page, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->do_translate_page();
                   },
                   this);
  hook(selection, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->do_translate_selection();
                   },
                   this);

  translate_list_ = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(translate_list_, GTK_SELECTION_NONE);
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(translate_list_));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(root), scroll);
  return root;
}

GtkWidget* AiPanel::make_summarize_tab() {
  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(root, 10);
  gtk_widget_set_margin_bottom(root, 10);
  gtk_widget_set_margin_start(root, 10);
  gtk_widget_set_margin_end(root, 10);

  // op codes for the shared clicked callback
  auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  // std::string copies: the array must own its labels, not borrow
  // .c_str() from destroyed temporaries.
  struct Entry {
    std::string label;
    int op;
  };
  Entry const entries[] = {{Tr("ui.summarize.page"), 0},
                           {Tr("ui.summarize.selection"), 1},
                           {Tr("ui.summarize.doc"), 2}};
  for (auto const& entry : entries) {
    auto* button = small_button(entry.label.c_str());
    g_object_set_data(G_OBJECT(button), "pdfgist-op",
                      GINT_TO_POINTER(entry.op));
    hook(
        button, "clicked",
        +[](GtkButton* button, gpointer self) {
          auto* panel = static_cast<AiPanel*>(self);
          switch (GPOINTER_TO_INT(
              g_object_get_data(G_OBJECT(button), "pdfgist-op"))) {
            case 0: panel->do_summarize_page(); break;
            case 1: panel->do_summarize_selection(); break;
            default: panel->do_summarize_doc(); break;
          }
        },
        this);
    gtk_box_append(GTK_BOX(actions), button);
  }
  gtk_box_append(GTK_BOX(root), actions);

  summarize_list_ = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(summarize_list_, GTK_SELECTION_NONE);
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(summarize_list_));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(root), scroll);
  return root;
}

GtkWidget* AiPanel::make_chat_tab() {
  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(root, 10);
  gtk_widget_set_margin_bottom(root, 10);
  gtk_widget_set_margin_start(root, 10);
  gtk_widget_set_margin_end(root, 10);

  auto* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  // std::string copies: own the labels instead of borrowing .c_str()
  // from destroyed temporaries.
  std::string const scopes[] = {Tr("ui.chat.scope.page"),
                                Tr("ui.chat.scope.selection"),
                                Tr("ui.chat.scope.doc")};
  auto* strings = gtk_string_list_new(nullptr);
  for (auto const& scope : scopes) {
    gtk_string_list_append(GTK_STRING_LIST(strings), scope.c_str());
  }
  chat_scope_ =
      GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(strings), nullptr));
  gtk_widget_set_hexpand(GTK_WIDGET(chat_scope_), TRUE);
  gtk_box_append(GTK_BOX(top), GTK_WIDGET(chat_scope_));
  chat_stop_ = GTK_BUTTON(small_button(Tr("ui.card.stop").c_str()));
  gtk_widget_set_sensitive(GTK_WIDGET(chat_stop_), FALSE);
  gtk_box_append(GTK_BOX(top), GTK_WIDGET(chat_stop_));
  gtk_box_append(GTK_BOX(root), top);

  chat_list_ = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
  chat_scroll_ = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(chat_scroll_),
                                GTK_WIDGET(chat_list_));
  gtk_widget_set_vexpand(chat_scroll_, TRUE);
  gtk_box_append(GTK_BOX(root), chat_scroll_);

  auto* input = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  chat_entry_ = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(chat_entry_,
                                 Tr("ui.chat.placeholder").c_str());
  gtk_widget_set_hexpand(GTK_WIDGET(chat_entry_), TRUE);
  gtk_box_append(GTK_BOX(input), GTK_WIDGET(chat_entry_));
  auto* send = small_button(Tr("ui.chat.send").c_str());
  gtk_box_append(GTK_BOX(input), send);
  gtk_box_append(GTK_BOX(root), input);

  hook(send, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->do_send_chat();
                   },
                   this);
  hook(chat_entry_, "activate",
                   +[](GtkEntry*, gpointer self) {
                     static_cast<AiPanel*>(self)->do_send_chat();
                   },
                   this);
  hook(chat_stop_, "clicked",
                   +[](GtkButton*, gpointer self) {
                     auto* panel = static_cast<AiPanel*>(self);
                     if (panel->chat_live_ != nullptr) {
                       panel->card_stop_clicked(panel->chat_live_);
                     }
                   },
                   this);
  return root;
}

// ----------------------------------------------------------- settings ----

GtkWidget* AiPanel::make_settings_tab() {
  auto* scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(root, 10);
  gtk_widget_set_margin_bottom(root, 10);
  gtk_widget_set_margin_start(root, 10);
  gtk_widget_set_margin_end(root, 10);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), root);

  gtk_box_append(GTK_BOX(root), section_label(Tr("ui.settings.preset").c_str()));
  preset_dropdown_ = GTK_DROP_DOWN(gtk_drop_down_new(nullptr, nullptr));
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(preset_dropdown_));
  hook(
      preset_dropdown_, "notify::selected",
      +[](GObject*, GParamSpec*, gpointer self) {
        static_cast<AiPanel*>(self)->settings_switch_preset();
      },
      this);

  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.settings.base-url").c_str()));
  base_url_ = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(base_url_, "https://…");
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(base_url_));

  gtk_box_append(GTK_BOX(root), section_label(Tr("ui.settings.model").c_str()));
  model_entry_ = GTK_ENTRY(gtk_entry_new());
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(model_entry_));
  model_suggestions_ = GTK_DROP_DOWN(gtk_drop_down_new(nullptr, nullptr));
  gtk_widget_set_visible(GTK_WIDGET(model_suggestions_), FALSE);
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(model_suggestions_));
  hook(
      model_suggestions_, "notify::selected",
      +[](GObject*, GParamSpec*, gpointer self) {
        auto* panel = static_cast<AiPanel*>(self);
        auto const position =
            gtk_drop_down_get_selected(panel->model_suggestions_);
        if (position == GTK_INVALID_LIST_POSITION) return;
        auto* item =
            gtk_drop_down_get_selected_item(panel->model_suggestions_);
        if (item == nullptr) return;
        gtk_entry_buffer_set_text(
            gtk_entry_get_buffer(panel->model_entry_),
            gtk_string_object_get_string(GTK_STRING_OBJECT(item)), -1);
      },
      this);

  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.settings.api-key").c_str()));
  auto* key_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  api_key_ = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_visibility(api_key_, FALSE);
  gtk_widget_set_hexpand(GTK_WIDGET(api_key_), TRUE);
  gtk_box_append(GTK_BOX(key_row), GTK_WIDGET(api_key_));
  auto* save_key = small_button(Tr("ui.dialog.save").c_str());
  gtk_box_append(GTK_BOX(key_row), save_key);
  gtk_box_append(GTK_BOX(root), key_row);
  hook(save_key, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->settings_save_key();
                   },
                   this);

  auto* test_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto* fetch = small_button(Tr("ui.settings.fetch-models").c_str());
  auto* test = small_button(Tr("ui.settings.test").c_str());
  gtk_box_append(GTK_BOX(test_row), fetch);
  gtk_box_append(GTK_BOX(test_row), test);
  gtk_box_append(GTK_BOX(root), test_row);
  hook(fetch, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->settings_test();
                   },
                   this);
  hook(test, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->settings_test();
                   },
                   this);

  settings_status_ = GTK_LABEL(gtk_label_new(nullptr));
  gtk_label_set_wrap(settings_status_, TRUE);
  gtk_label_set_selectable(settings_status_, TRUE);
  gtk_label_set_xalign(settings_status_, 0.0f);
  gtk_widget_add_css_class(GTK_WIDGET(settings_status_), "caption");
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(settings_status_));

  gtk_box_append(GTK_BOX(root),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.settings.target-language").c_str()));
  char const* languages[] = {"中文", "繁體中文", "English", "日本語",
                             "한국어", "Français", "Deutsch", "Español"};
  auto* lang_model = gtk_string_list_new(nullptr);
  for (auto* lang : languages) {
    gtk_string_list_append(GTK_STRING_LIST(lang_model), lang);
  }
  target_language_ =
      GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(lang_model), nullptr));
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(target_language_));

  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.settings.view-mode").c_str()));
  auto* mode_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  view_single_ = GTK_CHECK_BUTTON(
      gtk_check_button_new_with_label(Tr("ui.settings.view-single").c_str()));
  view_double_ = GTK_CHECK_BUTTON(
      gtk_check_button_new_with_label(Tr("ui.settings.view-double").c_str()));
  gtk_check_button_set_group(view_single_, view_double_);
  gtk_box_append(GTK_BOX(mode_row), GTK_WIDGET(view_single_));
  gtk_box_append(GTK_BOX(mode_row), GTK_WIDGET(view_double_));
  gtk_box_append(GTK_BOX(root), mode_row);

  sidecar_ = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
      Tr("ui.settings.sidecar").c_str()));
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(sidecar_));

  auto* save = small_button(Tr("ui.settings.save").c_str());
  gtk_box_append(GTK_BOX(root), save);
  hook(save, "clicked",
                   +[](GtkButton*, gpointer self) {
                     static_cast<AiPanel*>(self)->settings_save();
                   },
                   this);

  gtk_box_append(GTK_BOX(root),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.settings.appearance").c_str()));
  // std::string copies: own the labels instead of borrowing .c_str()
  // from destroyed temporaries.
  std::string const themes[] = {Tr("ui.theme.system"),
                                Tr("ui.toolbar.theme-light"),
                                Tr("ui.toolbar.theme-dark")};
  auto* theme_model = gtk_string_list_new(nullptr);
  for (auto const& theme : themes) {
    gtk_string_list_append(GTK_STRING_LIST(theme_model), theme.c_str());
  }
  theme_dropdown_ =
      GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(theme_model), nullptr));
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(theme_dropdown_));
  hook(
      theme_dropdown_, "notify::selected",
      +[](GObject*, GParamSpec*, gpointer self) {
        auto* panel = static_cast<AiPanel*>(self);
        if (panel->syncing_settings_ || !panel->on_theme_changed) return;
        switch (gtk_drop_down_get_selected(panel->theme_dropdown_)) {
          case 1: panel->on_theme_changed("light"); break;
          case 2: panel->on_theme_changed("dark"); break;
          default: panel->on_theme_changed("system"); break;
        }
      },
      this);

  // Interface language: strings are baked per render, so a change asks for
  // a relaunch (mac parity) — the dropdown only stages the preference.
  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.settings.language").c_str()));
  auto* language_model = gtk_string_list_new(nullptr);
  std::string const language_labels[] = {Tr("ui.settings.language-system"),
                                         "中文", "English"};
  for (auto const& label : language_labels) {
    gtk_string_list_append(GTK_STRING_LIST(language_model), label.c_str());
  }
  language_dropdown_ =
      GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(language_model), nullptr));
  gtk_drop_down_set_selected(
      language_dropdown_,
      ctx_->ui_language == "zh" ? 1 : ctx_->ui_language == "en" ? 2 : 0);
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(language_dropdown_));
  hook(
      language_dropdown_, "notify::selected",
      +[](GObject*, GParamSpec*, gpointer self) {
        auto* panel = static_cast<AiPanel*>(self);
        if (panel->syncing_settings_ || !panel->on_language_changed) return;
        switch (gtk_drop_down_get_selected(panel->language_dropdown_)) {
          case 1: panel->on_language_changed("zh"); break;
          case 2: panel->on_language_changed("en"); break;
          default: panel->on_language_changed("system"); break;
        }
      },
      this);

  // Read aloud: rate only on Linux (spd-say has no enumerable voices);
  // 0.5x-3.0x maps onto speech-dispatcher's -100..100 range at speak time.
  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.tts.section").c_str()));
  auto* rate_label = gtk_label_new(Tr("ui.tts.rate").c_str());
  gtk_widget_set_halign(rate_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(rate_label, "caption");
  gtk_widget_add_css_class(rate_label, "dim-label");
  gtk_box_append(GTK_BOX(root), rate_label);
  tts_rate_scale_ = GTK_SCALE(gtk_scale_new_with_range(
      GTK_ORIENTATION_HORIZONTAL, 0.5, 3.0, 0.1));
  gtk_range_set_value(GTK_RANGE(tts_rate_scale_), ctx_->tts_rate);
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(tts_rate_scale_));
  hook(
      tts_rate_scale_, "value-changed",
      +[](GObject*, gpointer self) {
        auto* panel = static_cast<AiPanel*>(self);
        if (!panel->on_tts_rate_changed || panel->tts_rate_scale_ == nullptr) {
          return;
        }
        panel->on_tts_rate_changed(
            gtk_range_get_value(GTK_RANGE(panel->tts_rate_scale_)));
      },
      this);

  // Reading theme: the page surface inside books, persisted in
  // settings.json (the chrome dropdown above stays a host pref).
  gtk_box_append(GTK_BOX(root),
                 section_label(Tr("ui.reading.theme").c_str()));
  auto* reading_model = gtk_string_list_new(nullptr);
  // std::string copies: own the labels instead of borrowing .c_str()
  // from destroyed temporaries.
  std::string const reading_labels[] = {Tr("ui.reading.theme.system"),
                                        Tr("ui.reading.theme.paper"),
                                        Tr("ui.reading.theme.night")};
  for (auto const& label : reading_labels) {
    gtk_string_list_append(GTK_STRING_LIST(reading_model), label.c_str());
  }
  reading_theme_dropdown_ =
      GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(reading_model), nullptr));
  gtk_box_append(GTK_BOX(root), GTK_WIDGET(reading_theme_dropdown_));
  hook(
      reading_theme_dropdown_, "notify::selected",
      +[](GObject*, GParamSpec*, gpointer self) {
        auto* panel = static_cast<AiPanel*>(self);
        if (panel->syncing_settings_ || !panel->on_reading_theme_changed) return;
        switch (gtk_drop_down_get_selected(panel->reading_theme_dropdown_)) {
          case 1: panel->on_reading_theme_changed("paper"); break;
          case 2: panel->on_reading_theme_changed("night"); break;
          default: panel->on_reading_theme_changed("system"); break;
        }
      },
      this);

  gtk_box_append(GTK_BOX(root),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  auto* privacy = gtk_label_new(Tr("ui.settings.privacy").c_str());
  gtk_label_set_wrap(GTK_LABEL(privacy), TRUE);
  gtk_label_set_xalign(GTK_LABEL(privacy), 0.0f);
  gtk_widget_add_css_class(privacy, "caption");
  gtk_widget_add_css_class(privacy, "dim-label");
  gtk_box_append(GTK_BOX(root), privacy);

  sync_settings();
  return root;
}

void AiPanel::open_settings_tab() {
  gtk_stack_set_visible_child_name(GTK_STACK(stack_), "settings");
}

void AiPanel::sync_settings() {
  if (ctx_ == nullptr) return;
  SyncGuard guard(syncing_settings_);
  auto const& s = ctx_->settings;

  if (!ctx_->presets.empty()) {
    GListStore* store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    for (auto const& preset : ctx_->presets) {
      g_list_store_append(store,
                          gtk_string_object_new(preset.label.c_str()));
    }
    gtk_drop_down_set_model(preset_dropdown_, G_LIST_MODEL(store));
    g_object_unref(store);
    guint selected = 0;
    for (guint i = 0; i < ctx_->presets.size(); ++i) {
      if (ctx_->presets[i].id == s.provider) {
        selected = i;
        break;
      }
    }
    gtk_drop_down_set_selected(preset_dropdown_, selected);
  }

  gtk_entry_buffer_set_text(gtk_entry_get_buffer(base_url_),
                            s.base_url.c_str(), -1);
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(model_entry_),
                            s.model.c_str(), -1);
  gtk_widget_set_visible(GTK_WIDGET(model_suggestions_), FALSE);
  guint lang = 0;
  switch (s.target_language) {
    case rivet_app::TargetLanguage::zh: lang = 0; break;
    case rivet_app::TargetLanguage::zh_hant: lang = 1; break;
    case rivet_app::TargetLanguage::en: lang = 2; break;
    case rivet_app::TargetLanguage::ja: lang = 3; break;
    case rivet_app::TargetLanguage::ko: lang = 4; break;
    case rivet_app::TargetLanguage::fr: lang = 5; break;
    case rivet_app::TargetLanguage::de: lang = 6; break;
    case rivet_app::TargetLanguage::es: lang = 7; break;
  }
  gtk_drop_down_set_selected(target_language_, lang);
  gtk_check_button_set_active(s.view_mode == "double" ? view_double_
                                                      : view_single_,
                              TRUE);
  gtk_check_button_set_active(sidecar_, s.annotation_sidecar);
  if (reading_theme_dropdown_ != nullptr) {
    gtk_drop_down_set_selected(
        reading_theme_dropdown_,
        s.reading_theme == rivet_app::ReadingTheme::paper
            ? 1
            : s.reading_theme == rivet_app::ReadingTheme::night ? 2 : 0);
  }
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(api_key_), "", -1);
  gtk_entry_set_placeholder_text(
      api_key_, s.has_api_key ? Tr("ui.settings.api-key-saved").c_str() : "");
}

// --------------------------------------------------------------- cards ----

GtkWidget* AiPanel::make_card_row(StreamCard* card) {
  auto* row = gtk_list_box_row_new();
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_add_css_class(box, "card");

  auto* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  card->title_label = GTK_LABEL(gtk_label_new(nullptr));
  gtk_label_set_xalign(card->title_label, 0.0f);
  gtk_label_set_use_markup(card->title_label, TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(card->title_label), TRUE);
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(card->title_label));
  card->spinner = gtk_spinner_new();
  gtk_box_append(GTK_BOX(header), card->spinner);
  gtk_spinner_start(GTK_SPINNER(card->spinner));
  card->stop_button = GTK_BUTTON(small_button(Tr("ui.card.stop").c_str()));
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(card->stop_button));
  auto* copy = small_button(Tr("ui.card.copy").c_str());
  gtk_box_append(GTK_BOX(header), copy);
  gtk_box_append(GTK_BOX(box), header);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  card->row = row;

  hook(card->stop_button, "clicked",
                   +[](GtkButton*, gpointer user_data) {
                     auto* target = static_cast<StreamCard*>(user_data);
                     target->owner->card_stop_clicked(target);
                   },
                   card);
  hook(copy, "clicked",
                   +[](GtkButton*, gpointer user_data) {
                     auto* target = static_cast<StreamCard*>(user_data);
                     target->owner->card_copy_clicked(target);
                   },
                   card);
  return row;
}

StreamCard* AiPanel::new_card(GtkListBox* list, std::string const& title,
                              std::string const& meta, bool chat) {
  auto card = std::make_unique<StreamCard>();
  card->title = title;
  card->meta = meta;
  card->chat = chat;
  card->owner = this;
  card->row = make_card_row(card.get());
  StreamCard* raw = card.get();
  cards_.push_back(std::move(card));
  refresh_card(raw);
  if (chat) {
    gtk_box_append(chat_list_, raw->row);
  } else {
    gtk_list_box_prepend(list, raw->row);
  }
  return raw;
}

void AiPanel::refresh_card(StreamCard* card) {
  std::string title_markup;
  if (!card->title.empty()) {
    title_markup = "<b>" + escape_markup(card->title) + "</b>";
    if (!card->meta.empty()) {
      title_markup += "  <small>" + escape_markup(card->meta) + "</small>";
    }
  } else {
    title_markup = escape_markup(card->meta);
  }
  gtk_label_set_markup(card->title_label, title_markup.c_str());

  std::string body;
  switch (card->phase) {
    case StreamCard::Phase::streaming:
      if (!card->content.empty()) {
        body = escape_markup(card->content);
      } else if (!card->reasoning.empty()) {
        body = "<small><i>" + escape_markup(card->reasoning) + "</i></small>";
      }
      break;
    case StreamCard::Phase::done:
      if (!card->content.empty()) {
        body = escape_markup(card->content);
      } else {
        body = "<small><i>" + escape_markup(card->reasoning) + "</i></small>";
      }
      break;
    case StreamCard::Phase::empty_stopped:
      body = "<small><i>" + Tr("backend.stream.stopped") + "</i></small>";
      break;
    case StreamCard::Phase::empty:
      body = "<small><i>" + Tr("backend.stream.no-content") + "</i></small>";
      break;
    case StreamCard::Phase::error:
      body = "<span foreground=\"#c0392b\">" +
             escape_markup(card->content) + "</span>";
      break;
    case StreamCard::Phase::notice:
      body = "<small><i>" + escape_markup(card->content) + "</i></small>";
      break;
  }
  gtk_label_set_markup(card->content_label, body.c_str());
  bool const streaming = card->phase == StreamCard::Phase::streaming;
  gtk_widget_set_visible(card->spinner, streaming);
  gtk_widget_set_visible(GTK_WIDGET(card->stop_button),
                         streaming && card->request_id > 0);
}

void AiPanel::append_notice(GtkListBox* list, std::string const& message) {
  auto* card = new_card(list, "", message, list == nullptr);
  card->phase = StreamCard::Phase::notice;
  card->content = message;
  refresh_card(card);
}

void AiPanel::card_stop_clicked(StreamCard* card) {
  if (card->request_id > 0) {
    stopping_streams_.insert(card->request_id);
    if (ctx_->api != nullptr) {
      (void)ctx_->api->stop_stream_async(card->request_id,
                                         [](rivet_app::Result<void>&&) {});
    }
    return;
  }
  card->phase = card->content.empty() && card->reasoning.empty()
                    ? StreamCard::Phase::empty_stopped
                    : StreamCard::Phase::done;
  refresh_card(card);
  if (card == chat_live_) finalize_chat(card);
}

void AiPanel::card_copy_clicked(StreamCard* card) {
  std::string const text =
      !card->content.empty() ? card->content : card->reasoning;
  if (text.empty()) return;
  GdkClipboard* const clipboard =
      gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, text.c_str());
}

void AiPanel::attach_card(StreamCard* card,
                          rivet_app::StreamStart const& start) {
  card->request_id = start.request_id;
  stream_registry_[start.request_id] = card;
  refresh_card(card);
}

void AiPanel::finish_stream(std::int64_t request_id,
                            std::string const& error) {
  auto it = stream_registry_.find(request_id);
  if (it == stream_registry_.end()) return;
  StreamCard* card = it->second;
  stream_registry_.erase(it);
  bool const stopped = stopping_streams_.erase(request_id) > 0;
  if (!error.empty()) {
    card->phase = StreamCard::Phase::error;
    card->content = error;
  } else if (card->content.empty() && card->reasoning.empty()) {
    card->phase = stopped ? StreamCard::Phase::empty_stopped
                          : StreamCard::Phase::empty;
  } else {
    card->phase = StreamCard::Phase::done;
  }
  refresh_card(card);
  if (card == chat_live_) finalize_chat(card);
}

void AiPanel::on_stream_chunk(rivet_app::StreamChunk const& chunk) {
  auto it = stream_registry_.find(chunk.request_id);
  if (it == stream_registry_.end()) return;
  StreamCard* card = it->second;
  if (chunk.is_reasoning) {
    if (card->content.empty()) card->reasoning += chunk.delta;
  } else {
    card->content += chunk.delta;
  }
  refresh_card(card);
}

void AiPanel::on_stream_done(rivet_app::StreamDone const& done) {
  finish_stream(done.request_id, {});
}

void AiPanel::on_stream_error(rivet_app::StreamError const& error) {
  finish_stream(error.request_id, error.message);
}

// -------------------------------------------------------------- actions ----

std::string AiPanel::describe_error(std::exception_ptr error) {
  try {
    if (error) std::rethrow_exception(error);
  } catch (std::exception const& e) {
    std::string const text = e.what();
    // RVT1 backend failures arrive as "...error: <message>"; keep the tail.
    auto const at = text.rfind("error: ");
    if (at != std::string::npos) return text.substr(at + 7);
    return text;
  } catch (...) {
  }
  return "unknown backend failure";
}

std::string AiPanel::language_name(rivet_app::TargetLanguage code) const {
  switch (code) {
    case rivet_app::TargetLanguage::zh: return "中文";
    case rivet_app::TargetLanguage::zh_hant: return "繁體中文";
    case rivet_app::TargetLanguage::en: return "English";
    case rivet_app::TargetLanguage::ja: return "日本語";
    case rivet_app::TargetLanguage::ko: return "한국어";
    case rivet_app::TargetLanguage::fr: return "Français";
    case rivet_app::TargetLanguage::de: return "Deutsch";
    case rivet_app::TargetLanguage::es: return "Español";
  }
  return "中文";
}

bool AiPanel::ensure_provider(GtkListBox* notice_list) {
  auto const& s = ctx_->settings;
  bool const ok = !s.base_url.empty() && !s.model.empty() &&
                  (s.has_api_key || s.provider == "ollama");
  if (!ok) {
    open_settings_tab();
    append_notice(notice_list, Tr("ui.settings.need-config"));
  }
  return ok;
}

void AiPanel::confirm_ai_send(
    rivet_app::AISendAction action, rivet_app::AISendScope scope,
    std::string const& content,
    std::vector<std::vector<std::string>> const& history,
    std::string const& message, std::function<void()> on_continue) {
  (void)ctx_->api->preview_ai_send_async(
      action, scope, content, history, message,
      [this, on_continue = std::move(on_continue)](
          rivet_app::Result<rivet_app::AISendPreview> result) mutable {
        host::dispatch_ui(
            [this, result = std::move(result),
             on_continue = std::move(on_continue)]() mutable {
              if (!result.succeeded()) {
                ctx_->alert(Trf("ui.ai-preview.failed",
                                describe_error(result.error)));
                return;
              }
              auto const& preview = result.get();
              char const* action_key = "ui.ai-preview.action.chat";
              switch (preview.action) {
                case rivet_app::AISendAction::translate:
                  action_key = "ui.ai-preview.action.translate";
                  break;
                case rivet_app::AISendAction::summarize:
                  action_key = "ui.ai-preview.action.summarize";
                  break;
                case rivet_app::AISendAction::chat: break;
              }
              char const* scope_key = "ui.ai-preview.scope.page";
              switch (preview.scope) {
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
              std::string const body = Trf(
                  "ui.ai-preview.body", Tr(action_key), Tr(scope_key),
                  std::to_string(preview.content_chars),
                  std::to_string(preview.history_chars),
                  std::to_string(preview.message_chars),
                  std::to_string(preview.total_chars));
              auto* root = gtk_widget_get_root(root_);
              auto* parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr;
              auto* dialog = gtk_dialog_new_with_buttons(
                  Tr("ui.ai-preview.title").c_str(), parent, GTK_DIALOG_MODAL,
                  Tr("ui.ai-preview.cancel").c_str(), GTK_RESPONSE_CANCEL,
                  Tr("ui.ai-preview.continue").c_str(), GTK_RESPONSE_ACCEPT,
                  nullptr);
              gtk_dialog_set_default_response(GTK_DIALOG(dialog),
                                              GTK_RESPONSE_CANCEL);
              auto* label = gtk_label_new(body.c_str());
              gtk_label_set_wrap(GTK_LABEL(label), TRUE);
              gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
              gtk_widget_set_margin_top(label, 16);
              gtk_widget_set_margin_bottom(label, 16);
              gtk_widget_set_margin_start(label, 16);
              gtk_widget_set_margin_end(label, 16);
              gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(
                                 GTK_DIALOG(dialog))),
                             label);
              auto* continuation =
                  new std::function<void()>(std::move(on_continue));
              hook(dialog, "response",
                   +[](GtkDialog* dialog, int response, gpointer data) {
                     std::unique_ptr<std::function<void()>> continuation(
                         static_cast<std::function<void()>*>(data));
                     gtk_window_destroy(GTK_WINDOW(dialog));
                     if (response == GTK_RESPONSE_ACCEPT && *continuation) {
                       (*continuation)();
                     }
                   },
                   continuation);
              gtk_window_present(GTK_WINDOW(dialog));
            });
      });
}

// Shared completion body for the four stream launchers: success attaches
// the card into the event registry; failure turns the card into the error
// surface. Runs on the reader thread, dispatches to the UI thread.
template <typename ResultT>
void complete_stream_start(AiPanel* panel, StreamCard* card,
                           rivet_app::Result<ResultT> result) {
  host::dispatch_ui([panel, card, result]() mutable {
    if (result.succeeded()) {
      panel->attach_card(card, result.get());
    } else {
      card->phase = StreamCard::Phase::error;
      card->content = AiPanel::describe_error(result.error);
      panel->refresh_card(card);
    }
  });
}

void AiPanel::do_translate_selection() {
  std::string const text = trim(ctx_->selection_text());
  if (text.size() < 2) {
    append_notice(translate_list_, Tr("ui.need.selection"));
    return;
  }
  do_translate_text(text);
}

// Notes-pane excerpt translation (mac translateSelectionText).
void AiPanel::do_translate_text(std::string const& text) {
  if (!ensure_provider(translate_list_)) return;
  auto const lang = language_name(ctx_->settings.target_language);
  auto const target = ctx_->settings.target_language;
  confirm_ai_send(
      rivet_app::AISendAction::translate, rivet_app::AISendScope::selection,
      text, {}, "", [this, text, lang, target] {
        auto* card = new_card(translate_list_, Tr("ui.menu.translate"),
                              "→ " + lang, false);
        card->source_text = text;
        (void)ctx_->api->translate_text_async(
            text, target,
            [this, card](rivet_app::Result<rivet_app::StreamStart> result) {
              complete_stream_start(this, card, std::move(result));
            });
      });
}

void AiPanel::do_translate_page() {
  if (!ensure_provider(translate_list_)) return;
  auto const source = ctx_->page_text();
  if (!source.has_value() || trim(source->text).empty()) {
    append_notice(translate_list_, Tr("ui.need.document"));
    return;
  }
  auto const lang = language_name(ctx_->settings.target_language);
  // Model-facing prompt wrapper (windows hardcodes the same Chinese
  // markers; the domain prompt expects them) — deliberately not localized.
  std::string const payload = "以下是 PDF 第 " +
                              std::to_string(source->page) +
                              " 页提取的文本：\n\n" + source->text;
  auto const target = ctx_->settings.target_language;
  auto const preview_scope =
      ctx_->epub_active && ctx_->epub_active()
          ? rivet_app::AISendScope::chapter
          : rivet_app::AISendScope::page;
  confirm_ai_send(
      rivet_app::AISendAction::translate, preview_scope, payload, {}, "",
      [this, source = *source, payload, lang, target] {
        auto* card = new_card(
            translate_list_,
            Trf("ui.translate.page-title", std::to_string(source.page)),
            "→ " + lang, false);
        card->source_text = source.text;
        (void)ctx_->api->translate_text_async(
            payload, target,
            [this, card](rivet_app::Result<rivet_app::StreamStart> result) {
              complete_stream_start(this, card, std::move(result));
            });
      });
}

void AiPanel::do_summarize_page() {
  if (!ensure_provider(summarize_list_)) return;
  auto const source = ctx_->page_text();
  if (!source.has_value() || trim(source->text).empty()) {
    append_notice(summarize_list_, Tr("ui.need.document"));
    return;
  }
  auto const lang = language_name(ctx_->settings.target_language);
  confirm_ai_send(
      rivet_app::AISendAction::summarize,
      ctx_->epub_active && ctx_->epub_active()
          ? rivet_app::AISendScope::chapter
          : rivet_app::AISendScope::page,
      source->text, {}, "", [this, source = *source, lang] {
        auto* card = new_card(
            summarize_list_,
            Trf("ui.summarize.page-title", std::to_string(source.page)), lang,
            false);
        card->source_text = source.text;
        (void)ctx_->api->summarize_text_async(
            source.text, rivet_app::SummarizeMode::page,
            [this, card](rivet_app::Result<rivet_app::StreamStart> result) {
              complete_stream_start(this, card, std::move(result));
            });
      });
}

void AiPanel::do_summarize_selection() {
  std::string const text = trim(ctx_->selection_text());
  if (text.size() < 2) {
    append_notice(summarize_list_, Tr("ui.need.selection"));
    return;
  }
  if (!ensure_provider(summarize_list_)) return;
  auto const lang = language_name(ctx_->settings.target_language);
  confirm_ai_send(
      rivet_app::AISendAction::summarize,
      rivet_app::AISendScope::selection, text, {}, "",
      [this, text, lang] {
        auto* card = new_card(summarize_list_, Tr("ui.summarize.selection"),
                              lang, false);
        card->source_text = text;
        (void)ctx_->api->summarize_text_async(
            text, rivet_app::SummarizeMode::selection,
            [this, card](rivet_app::Result<rivet_app::StreamStart> result) {
              complete_stream_start(this, card, std::move(result));
            });
      });
}

void AiPanel::do_summarize_doc() {
  if (!ensure_provider(summarize_list_)) return;
  auto const source = ctx_->doc_text();
  if (!source.has_value() || trim(source->text).empty()) {
    append_notice(summarize_list_, Tr("ui.need.document"));
    return;
  }
  auto const lang = language_name(ctx_->settings.target_language);
  confirm_ai_send(
      rivet_app::AISendAction::summarize,
      rivet_app::AISendScope::document, source->text, {}, "",
      [this, source = *source, lang] {
        auto* card = new_card(
            summarize_list_, Tr("ui.summarize.doc"),
            lang + " · " +
                Trf("ui.summarize.doc-meta", std::to_string(source.page)),
            false);
        card->source_text = source.text;
        (void)ctx_->api->summarize_text_async(
            source.text, rivet_app::SummarizeMode::doc,
            [this, card](rivet_app::Result<rivet_app::StreamStart> result) {
              complete_stream_start(this, card, std::move(result));
            });
      });
}

void AiPanel::finalize_chat(StreamCard* card) {
  chat_busy_ = false;
  chat_live_ = nullptr;
  gtk_widget_set_sensitive(GTK_WIDGET(chat_stop_), FALSE);
  std::string const content = trim(card->content);
  if (!content.empty()) {
    chat_history_.push_back({"user", chat_pending_question_});
    chat_history_.push_back({"assistant", card->content});
    return;
  }
  if (!card->reasoning.empty()) return;  // reasoning-only: keep rendering
  // Empty stream: swap the bubble for the placeholder text (mac parity).
  bool const stopped = card->phase == StreamCard::Phase::empty_stopped;
  card->phase = StreamCard::Phase::notice;
  card->content = stopped ? Tr("backend.stream.stopped")
                          : Tr("backend.stream.no-content");
  refresh_card(card);
}

void AiPanel::do_send_chat() {
  std::string const question = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(chat_entry_)));
  if (question.empty() || chat_busy_) return;
  auto const scope = gtk_drop_down_get_selected(chat_scope_);

  std::string context_text;
  std::string context_label;
  bool have_context = false;
  // The context labels + "第 N 页：" wrapper below are backend-facing (the
  // windows host sends the same hardcoded Chinese strings; the domain prompt
  // expects them) — deliberately not localized.
  if (scope == 1) {
    context_text = trim(ctx_->selection_text());
    context_label = "选区";
    have_context = context_text.size() > 1;
  } else if (scope == 2) {
    auto const source = ctx_->doc_text();
    if (source.has_value() && !source->text.empty()) {
      context_text = source->text;
      context_label = "全文前 " + std::to_string(source->page) + " 页";
      have_context = true;
    }
  } else {
    auto const page = ctx_->page_text();
    if (page.has_value() && !trim(page->text).empty()) {
      context_text =
          "第 " + std::to_string(page->page) + " 页：\n" + page->text;
      context_label = "第 " + std::to_string(page->page) + " 页";
      have_context = true;
    }
  }
  if (!have_context) {
    append_notice(nullptr, scope == 1 ? Tr("ui.need.selection")
                                      : Tr("ui.need.document"));
    return;
  }
  {
    auto const& s = ctx_->settings;
    bool const configured = !s.base_url.empty() && !s.model.empty() &&
                            (s.has_api_key || s.provider == "ollama");
    if (!configured) {
      open_settings_tab();
      append_notice(nullptr, Tr("ui.settings.need-config"));
      return;
    }
  }

  // keep the last 12 turns (mac parity)
  std::vector<std::vector<std::string>> history(
      chat_history_.end() - std::min<std::size_t>(chat_history_.size(), 12),
      chat_history_.end());
  auto const preview_scope =
      scope == 1
          ? rivet_app::AISendScope::selection
          : scope == 2
                ? rivet_app::AISendScope::document
                : (ctx_->epub_active && ctx_->epub_active()
                       ? rivet_app::AISendScope::chapter
                       : rivet_app::AISendScope::page);
  confirm_ai_send(
      rivet_app::AISendAction::chat, preview_scope, context_text, history,
      question,
      [this, question, context_text, context_label,
       history = std::move(history)]() mutable {
        // The user bubble appears only after confirmation; cancelling leaves
        // both the conversation and the typed question untouched.
        auto* user_row = gtk_list_box_row_new();
        auto* user_label = gtk_label_new(
            ("<b>" + Tr("ui.chat.you") + ":</b> " + escape_markup(question))
                .c_str());
        gtk_label_set_use_markup(GTK_LABEL(user_label), TRUE);
        gtk_label_set_wrap(GTK_LABEL(user_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(user_label), 0.0f);
        gtk_widget_set_margin_top(user_row, 4);
        gtk_widget_set_margin_start(user_row, 8);
        gtk_widget_set_margin_end(user_row, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(user_row), user_label);
        gtk_box_append(chat_list_, user_row);

        auto* card = new_card(
            nullptr, "chat", language_name(ctx_->settings.target_language),
            true);
        chat_live_ = card;
        chat_pending_question_ = question;
        chat_busy_ = true;
        gtk_widget_set_sensitive(GTK_WIDGET(chat_stop_), TRUE);
        gtk_entry_buffer_set_text(gtk_entry_get_buffer(chat_entry_), "", -1);

        (void)ctx_->api->chat_async(
            context_text, context_label, history, question,
            [this, card](rivet_app::Result<rivet_app::StreamStart> result) {
              host::dispatch_ui([this, card, result]() mutable {
                if (result.succeeded()) {
                  attach_card(card, result.get());
                } else {
                  card->phase = StreamCard::Phase::error;
                  card->content = describe_error(result.error);
                  refresh_card(card);
                  finalize_chat(card);
                }
              });
            });
      });
}

// ------------------------------------------------------------- settings ----

void AiPanel::set_settings_status(std::string const& text, bool is_error) {
  gtk_label_set_text(settings_status_, text.c_str());
  if (is_error) {
    gtk_widget_add_css_class(GTK_WIDGET(settings_status_), "error");
    gtk_widget_remove_css_class(GTK_WIDGET(settings_status_), "suggested-action");
  } else {
    gtk_widget_add_css_class(GTK_WIDGET(settings_status_),
                             "suggested-action");
    gtk_widget_remove_css_class(GTK_WIDGET(settings_status_), "error");
  }
}

// v1 preset behavior: stash the edited URL/model under the outgoing
// preset, restore the stash (or the preset defaults) for the new one.
void AiPanel::settings_switch_preset() {
  if (syncing_settings_ || busy_) return;
  auto const position = gtk_drop_down_get_selected(preset_dropdown_);
  if (position == GTK_INVALID_LIST_POSITION ||
      position >= ctx_->presets.size()) {
    return;
  }
  std::string const new_id = ctx_->presets[position].id;
  if (new_id == current_preset_id_) return;
  if (!current_preset_id_.empty()) {
    preset_stash_[current_preset_id_] = {
        gtk_entry_buffer_get_text(gtk_entry_get_buffer(base_url_)),
        gtk_entry_buffer_get_text(gtk_entry_get_buffer(model_entry_))};
  }
  current_preset_id_ = new_id;
  auto const stash = preset_stash_.find(new_id);
  if (stash != preset_stash_.end()) {
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(base_url_),
                              stash->second.first.c_str(), -1);
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(model_entry_),
                              stash->second.second.c_str(), -1);
  } else {
    for (auto const& preset : ctx_->presets) {
      if (preset.id == new_id) {
        gtk_entry_buffer_set_text(gtk_entry_get_buffer(base_url_),
                                  preset.base_url.c_str(), -1);
        gtk_entry_buffer_set_text(gtk_entry_get_buffer(model_entry_),
                                  preset.default_model.c_str(), -1);
        break;
      }
    }
  }
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(api_key_), "", -1);
}

void AiPanel::settings_save_key() {
  if (busy_ || ctx_->api == nullptr) return;
  std::string const key = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(api_key_)));
  if (key.empty()) return;
  busy_ = true;
  set_settings_status(Tr("ui.settings.saving-key"), false);
  (void)ctx_->api->save_api_key_async(
      key, [this](rivet_app::Result<void> result) {
        host::dispatch_ui([this, result]() mutable {
          busy_ = false;
          if (result.succeeded()) {
            set_settings_status(Tr("ui.settings.saved"), false);
          } else {
            set_settings_status(
                Trf("ui.settings.save-failed", describe_error(result.error)),
                true);
          }
          (void)ctx_->api->get_settings_async(
              [this](rivet_app::Result<rivet_app::SettingsView> view) {
                host::dispatch_ui([this, view]() mutable {
                  if (view.succeeded()) {
                    ctx_->settings = view.get();
                    sync_settings();
                  }
                });
              });
        });
      });
}

void AiPanel::settings_test() {
  if (busy_ || ctx_->api == nullptr) return;
  std::string const base_url = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(base_url_)));
  std::string const model = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(model_entry_)));
  std::string const key = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(api_key_)));
  if (base_url.empty() ||
      (model.empty() && !ctx_->settings.has_api_key)) {
    set_settings_status(Tr("ui.settings.need-input"), true);
    return;
  }
  busy_ = true;
  set_settings_status(Tr("ui.settings.fetching"), false);
  (void)ctx_->api->test_connection_async(
      base_url, key, model,
      [this](rivet_app::Result<rivet_app::TestResult> result) {
        host::dispatch_ui([this, result]() mutable {
          busy_ = false;
          if (!result.succeeded()) {
            set_settings_status(describe_error(result.error), true);
            return;
          }
          rivet_app::TestResult const& test = result.get();
          if (!test.ok) {
            set_settings_status(test.error.value_or("unknown error"), true);
            return;
          }
          auto const& models = test.models;
          if (!models.has_value() || models->empty()) {
            set_settings_status(Tr("ui.settings.test-ok-no-list"), false);
            gtk_widget_set_visible(GTK_WIDGET(model_suggestions_), FALSE);
            return;
          }
          set_settings_status(
              Trf("ui.settings.test-ok", std::to_string(models->size())),
              false);
          GListStore* store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
          for (auto const& model_name : *models) {
            g_list_store_append(store,
                                gtk_string_object_new(model_name.c_str()));
          }
          gtk_drop_down_set_model(model_suggestions_, G_LIST_MODEL(store));
          g_object_unref(store);
          gtk_widget_set_visible(GTK_WIDGET(model_suggestions_), TRUE);
        });
      });
}

void AiPanel::settings_save() {
  if (busy_ || ctx_->api == nullptr) return;
  std::string const base_url = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(base_url_)));
  std::string const model = trim(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(model_entry_)));
  auto const preset_index = gtk_drop_down_get_selected(preset_dropdown_);
  std::string const provider =
      preset_index != GTK_INVALID_LIST_POSITION &&
              preset_index < ctx_->presets.size()
          ? ctx_->presets[preset_index].id
          : "deepseek";
  static rivet_app::TargetLanguage const languages[] = {
      rivet_app::TargetLanguage::zh,     rivet_app::TargetLanguage::zh_hant,
      rivet_app::TargetLanguage::en,     rivet_app::TargetLanguage::ja,
      rivet_app::TargetLanguage::ko,     rivet_app::TargetLanguage::fr,
      rivet_app::TargetLanguage::de,     rivet_app::TargetLanguage::es};
  auto const lang_index = gtk_drop_down_get_selected(target_language_);
  auto const target =
      languages[lang_index == GTK_INVALID_LIST_POSITION
                    ? 0
                    : std::min<guint>(lang_index, 7)];
  std::string const view_mode =
      gtk_check_button_get_active(view_double_) ? "double" : "single";
  bool const sidecar = gtk_check_button_get_active(sidecar_);

  rivet_app::SettingsUpdate update;
  update.provider = provider;
  update.base_url = base_url;
  update.model = model;
  update.target_language = target;
  update.view_mode = view_mode;
  update.annotation_sidecar = sidecar;
  busy_ = true;
  (void)ctx_->api->save_settings_async(
      update, [this](rivet_app::Result<rivet_app::SettingsView> result) {
        host::dispatch_ui([this, result]() mutable {
          busy_ = false;
          if (result.succeeded()) {
            ctx_->settings = result.get();
            set_settings_status(Tr("ui.settings.saved"), false);
            sync_settings();
            if (ctx_->apply_view_mode) ctx_->apply_view_mode();
          } else {
            set_settings_status(
                Trf("ui.settings.save-failed", describe_error(result.error)),
                true);
          }
        });
      });
}

void AiPanel::set_theme(std::string const& theme) {
  if (theme_dropdown_ == nullptr) return;
  SyncGuard guard(syncing_settings_);
  gtk_drop_down_set_selected(theme_dropdown_,
                             theme == "light"   ? 1
                             : theme == "dark"  ? 2
                                                : 0);
}

}  // namespace pdfgist
