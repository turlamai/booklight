#pragma once

#include "pch.h"
#include "MainWindow.g.h"
#include "GeneratedBackend.hpp"
#include "PdfDocument.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Host helpers shared across the MainWindow.*.cpp parts (podlens pattern).
// Defined in MainWindow.xaml.cpp.
namespace winrt::RivetHost::implementation {
std::filesystem::path executable_path();
std::wstring to_wide(std::string const& utf8_text);
std::string to_utf8(std::wstring_view wide);

struct PdfTab;
struct EpubBook;
struct StreamCardUi;

// PdfDocument::FormField is a nested type; host code spells it FormField.
using FormField = pdfgist::PdfDocument::FormField;

// ---- annotation storage (JSON schema of racket/pdfgist/annotations.rkt) ------
// Rects are PDF page space, origin BOTTOM-LEFT, y up (PDFKit page space on
// mac). The Windows view flips PDFium's top-left rects on store and again on
// draw — this contract is what keeps cross-app sidecars in sync.

struct AnnoRectD {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

// EPUB annotations anchor to chapter + character offsets inside the
// chapter's plain text; PDF annotations use page-space rects and carry
// no anchor.
struct StoredAnchor {
  int chapter = 1;
  std::int64_t start = 0;
  std::int64_t end = 0;
};

struct StoredAnnotation {
  std::wstring id;
  int page = 1;
  std::vector<AnnoRectD> rects;
  std::wstring excerpt;
  std::wstring color;  // yellow / green / blue
  std::wstring kind;   // highlight / underline / strike
  std::wstring note;
  std::int64_t created = 0;
  std::optional<StoredAnchor> anchor;
};

struct StoredBookmark {
  int page = 1;
  std::wstring label;
  std::int64_t created = 0;
};

struct DocData {
  std::vector<StoredAnnotation> annotations;
  std::vector<StoredBookmark> bookmarks;
};

enum class AnnotationSavePhase { Saved, Pending, Recovered, Failed };

// ---- reading tab ---------------------------------------------------------------

struct PdfTab {
  std::wstring path;
  std::wstring title;
  std::shared_ptr<pdfgist::PdfDocument> doc;
  int page_count = 0;
  int current_page = 1;
  bool pdf_edits_pending = false;  // tab bytes differ from source on disk
  double zoom = 1.0;        // multiplier when not fit-width
  bool fit_width = true;    // v1 default: open fitted to width
  std::vector<pdfgist::OutlineItem> outline;

  winrt::Microsoft::UI::Xaml::Controls::TabViewItem tab_item{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroll{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel rows{nullptr};

  // split view (second read-only view of the same document; follows the main
  // view's scroll ratio — mac SplitViewport semantics)
  bool split_on = false;
  winrt::Microsoft::UI::Xaml::Controls::Grid split_container{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer split_scroll{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel split_rows{nullptr};

  struct Cell {
    int page = 0;  // 1-based
    winrt::Microsoft::UI::Xaml::Controls::Grid grid{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas canvas{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Image image{nullptr};
    // split-view twins (built only while split is on)
    winrt::Microsoft::UI::Xaml::Controls::Grid split_grid{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas split_canvas{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Image split_image{nullptr};
    double width = 0;   // effective px at the current scale
    double height = 0;
    std::int64_t rendered_key = 0;  // (scale, raster, dark) stamp of the bitmap
    bool requested = false;         // a render is in flight
  };
  std::vector<std::shared_ptr<Cell>> cells;  // index == page - 1
  double cells_scale_ = -1;                  // scale the current layout was built at

  // per-page find rectangles (top-left points), drawn under the selection
  std::map<int, std::vector<std::array<double, 4>>> find_rects;

  // stored annotations + bookmarks (loaded via get-annotations)
  DocData doc_data;
  bool doc_data_loaded = false;
  bool anno_dirty = false;
  std::uint64_t anno_revision = 0;
  AnnotationSavePhase anno_save_phase = AnnotationSavePhase::Saved;
  std::wstring anno_save_error;

  // rubber-band selection state (top-left points)
  bool selecting = false;
  int selection_page = 0;
  double selection_x0 = 0, selection_y0 = 0;
  double selection_left = 0, selection_top = 0, selection_right = 0,
         selection_bottom = 0;
  std::wstring selection_text;
  std::vector<std::array<double, 4>> selection_rects;

  std::set<int> rendering_;
  double layout_viewport_w = -1;  // fit-width is stable per viewport width
};

// ---- EPUB tab (one at a time) ----------------------------------------------------

struct EpubBook {
  std::wstring path;
  std::wstring title;
  int chapters = 0;
  int current_chapter = 1;
  double font_scale = 1.0;
  std::vector<rivet_app::EpubTocItem> toc;
  std::map<int, std::wstring> text_cache;  // chapter -> plain text
  bool text_ready = false;

  winrt::Microsoft::UI::Xaml::Controls::TabViewItem tab_item{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::WebView2 web{nullptr};
  // Selection with its chapter+offset anchor (page script computes the
  // offsets against the chapter's plain text).
  std::wstring selection_text;
  int selection_chapter = 0;
  std::int64_t selection_start = 0;
  std::int64_t selection_end = 0;
  DocData doc_data;
  bool doc_data_loaded = false;
  bool anno_dirty = false;
  std::uint64_t anno_revision = 0;
  AnnotationSavePhase anno_save_phase = AnnotationSavePhase::Saved;
  std::wstring anno_save_error;
  std::string doc_data_bytes;   // fetched on the worker thread, parsed on UI
  bool annotations_pending = false;
  bool annotations_recovered = false;
  std::optional<StoredAnnotation> undo_annotation;
  std::size_t undo_index = 0;
  int highlight_page = -1;  // page carrying the active find highlight
};

// ---- AI stream card ---------------------------------------------------------------

struct StreamCardUi {
  std::int64_t request_id = -1;
  std::wstring title;
  std::wstring meta;
  std::wstring source;
  std::wstring content;
  std::wstring reasoning;
  enum class Phase { Streaming, Done, Empty, Error, Notice };
  Phase phase = Phase::Streaming;
  bool stopping = false;  // the user pressed stop; an empty result reports it
  bool stopped = false;
  std::wstring failure;

  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock body{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock status_line{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button stop_button{nullptr};
};

struct ChatBubble {
  bool user = false;
  std::wstring text;
  std::shared_ptr<StreamCardUi> card;
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
};

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  // ---- XAML event handlers (MainWindow.xaml) ----
  void Open_Click(winrt::Windows::Foundation::IInspectable const&,
                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void LeftToggle_Click(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void FindToggle_Click(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void AiToggle_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Theme_Click(winrt::Windows::Foundation::IInspectable const&,
                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ThumbsMode_Click(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OutlineMode_Click(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void NotesMode_Click(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Bookmark_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Split_Click(winrt::Windows::Foundation::IInspectable const&,
                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void TtsButton_Click(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void PrintButton_Click(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ToolsWatermark_Click(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ToolsSavePdf_Click(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ToolsExportPage_Click(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ToolsExportText_Click(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ContinuePill_Click(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ContinuePillImport_Click(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void RecentsFlyout_Opening(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Windows::Foundation::IInspectable const&);
  void DocTabs_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void DocTabs_CloseRequested(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const&);
  void FindInput_TextChanged(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void FindInput_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void FindPrev_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void FindNext_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void FindClose_Click(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ChatInput_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void SendChat_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void StopChat_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void PageInput_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void ZoomIn_Click(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ZoomOut_Click(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ZoomReset_Click(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void FitWidth_Click(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ExportAnnotations();
  void UndoDeleteAnnotation();
  void UndoDeleteEpubAnnotation();
  void AddEpubAnnotation(std::wstring const& color, std::wstring const& kind);
  void RemoveEpubAnnotation(std::wstring const& id);
  void ApplyEpubAnnotations(EpubBook& book);
  void SaveEpubDocDataNow();
  void BuildEpubNotesPane(
      winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);

 private:
  // ---- lifecycle (MainWindow.xaml.cpp) ----
  winrt::fire_and_forget InitializeBackendAsync();
  void OnBackendEvent(std::string const& name, rivet::Value const& value);
  void Bootstrap();
  void ApplyTheme();
  void ToggleTheme();
  // Reading page surface (settings.json, independent of the chrome theme):
  // night forces dark, paper forces light, sepia forces the deep cream
  // parchment, system follows ActualTheme.
  std::wstring ReadingSurface();
  // Cell/page-canvas fill for the surface (the LayoutTab dark shortcut).
  winrt::Windows::UI::Color SurfaceCanvasColor();
  void ApplyReadingTheme();
  void SaveReadingPrefs(rivet_app::ReadingTheme theme);
  void SaveReadingFontScale(double scale);
  void UpdateChrome();  // visibility + labels that depend on state

  // host prefs (theme, panel toggles/widths): %APPDATA%\site.jrtx.pdfgist\host-ui.json
  void LoadHostPrefs();
  void SaveHostPrefs();

  void SetStatus(bool ok, std::wstring const& message);
  void Alert(std::wstring const& message);
  void ShowDialog(std::wstring const& title, std::wstring const& body,
                  std::wstring const& primary_button,
                  std::wstring const& close_button,
                  std::function<void()> on_primary,
                  std::wstring const& secondary_button = std::wstring(),
                  std::function<void()> on_secondary = nullptr,
                  std::function<void()> on_close = nullptr);

  // ---- tabs & document (MainWindow.xaml.cpp) ----
  void PickAndOpen();
  void OpenPath(std::wstring const& path, int resume_page = 1);
  void OpenDocReady(std::wstring const& path,
                    std::shared_ptr<pdfgist::PdfDocument> doc,
                    pdfgist::PdfOpenError error,
                    std::shared_ptr<std::vector<std::uint8_t>> bytes,
                    std::wstring const& password, bool wrong_password,
                    int resume_page = 1);
  void AskPassword(std::function<void(std::wstring const&)> on_answer,
                   bool wrong);
  void OpenDroppedRaw(winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
  void ActivateTab(std::shared_ptr<PdfTab> const& tab);
  std::shared_ptr<PdfTab> SharedTab(PdfTab* tab);
  void CloseTab(PdfTab const* tab);
  void ConfirmDirtyPdfClose(std::shared_ptr<PdfTab> const& tab,
                            std::function<void()> on_success = {},
                            std::function<void()> on_cancel = {});
  void CloseActiveReader();
  std::uint32_t IndexOfTabItem(
      winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& item);

  // left panel
  void RefreshLeftPanel();     // recents + thumbs/outline/toc + page ops
  void RefreshRecentsRows();

  // layout & rendering
  double EffectiveScale(PdfTab& tab);
  void RelayoutActive();
  void LayoutTab(PdfTab& tab);
  void RequestVisibleRenders(PdfTab& tab);
  void RenderCell(std::shared_ptr<PdfTab> tab, std::shared_ptr<PdfTab::Cell> cell,
                  double scale, bool dark, double raster);
  double RasterScale();
  void AttachCellHandlers(PdfTab& tab, PdfTab::Cell& cell,
                          winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas);
  void ScrollToPage(PdfTab& tab, int page);
  void TrackCurrentPage(PdfTab& tab);
  void UpdatePageBar();

  // selection overlays
  void ClearCellOverlays(PdfTab::Cell& cell);
  void DrawCellOverlays(PdfTab::Cell& cell, PdfTab const& tab);
  void OnCellPointerPressed(std::shared_ptr<PdfTab> tab,
                            std::shared_ptr<PdfTab::Cell> cell,
                            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void OnCellPointerMoved(std::shared_ptr<PdfTab> tab,
                          std::shared_ptr<PdfTab::Cell> cell,
                          winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void OnCellPointerReleased(std::shared_ptr<PdfTab> tab,
                             std::shared_ptr<PdfTab::Cell> cell,
                             winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void FinishSelection(std::shared_ptr<PdfTab> tab);
  void ClearSelection(PdfTab& tab);

  // annotations & bookmarks (MainWindow.Annotations.cpp)
  void LoadDocData(std::shared_ptr<PdfTab> tab);
  void QueueAnnotationSave();
  void SaveDocDataNow(std::shared_ptr<PdfTab> tab);
  void AddAnnotationFromSelection(std::wstring const& color,
                                  std::wstring const& kind);
  void RemoveAnnotation(std::wstring const& id);
  void ToggleBookmark();
  void UpdateBookmarkToggle();
  void BuildNotesPane(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content);
  void AppendAnnotationSaveBanner(
      winrt::Microsoft::UI::Xaml::Controls::StackPanel const& content,
      AnnotationSavePhase phase, std::wstring const& error);
  void DrawStoredOverlays(PdfTab::Cell& cell, PdfTab const& tab);
  std::string SerializeDocData(DocData const& data);
  bool ParseDocData(std::string const& json, DocData* out);

  // split view (MainWindow.xaml.cpp)
  void ApplySplitLayout(PdfTab& tab);
  void SyncSplitView(PdfTab& tab);
  void ToggleSplit();

  // TTS (MainWindow.Tts.cpp)
  void ToggleTts();
  void TtsSpeak(std::wstring const& text);
  void TtsStop();
  void TtsSetSpeaking(bool speaking);
  winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer tts_synth_{nullptr};
  winrt::Windows::Media::Playback::MediaPlayer tts_player_{nullptr};
  bool tts_speaking_ = false;

  // printing (MainWindow.Print.cpp)
  void PrintDocument();

  // find
  void RunFind();
  void ApplyFindResults(std::vector<pdfgist::FindHit> hits);
  void NavigateFind(int step);
  void CloseFind();
  void RunEpubFind(std::wstring const& query);
  void NavigateEpubFind(int step);
  void HighlightEpubHit(int chapter, int ordinal, std::wstring const& query);
  void ClearEpubHighlights();
  std::vector<std::pair<int, int>> epub_hits_;  // (chapter, ordinal)
  int epub_find_index_ = 0;

  // EPUB font scale (zoom in/out over the book tab)
  void ApplyEpubFontScale(EpubBook& book);

  // recents
  void NoteActivity();  // debounce-save the active reader position
  void SaveRecentNow();

  // ---- AI sidebar (MainWindow.Ai.cpp) ----
  void BuildAiSidebar();
  void ShowAiPane(int index);  // 0 translate / 1 summarize / 2 chat / 3 settings
  void AppendCardUi(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& list,
                    std::shared_ptr<StreamCardUi> card);
  void UpdateCardBody(StreamCardUi& card);
  void RemoveCard(std::vector<std::shared_ptr<StreamCardUi>>& cards,
                  StreamCardUi const& card);
  std::shared_ptr<StreamCardUi> MakeNoticeCard(std::wstring const& message);
  void InsertCard(std::vector<std::shared_ptr<StreamCardUi>>& cards,
                  std::shared_ptr<StreamCardUi> card);
  // Runs the launch RPC on a worker thread, registers the card under the
  // returned request id, and errors the card when the launch fails.
  void StartCardStream(std::shared_ptr<StreamCardUi> card,
                       std::vector<std::shared_ptr<StreamCardUi>>& list,
                       std::function<rivet_app::StreamStart()> launch);
  void StopCardAt(StreamCardUi* card);
  bool card_alive(std::shared_ptr<StreamCardUi> const& card) const;
  void LaunchTranslateCurrentPage();
  void LaunchTranslateSelection();
  void LaunchTranslateSelectionText(std::wstring const& text);
  void LaunchSummarizePage();
  void LaunchSummarizeSelection();
  void LaunchSummarizeDoc();
  void SendChat(std::wstring const& question);
  void DispatchChat(std::wstring const& question,
                    std::wstring const& context_text,
                    std::wstring const& context_label);
  void FinishStream(std::int64_t request_id,
                    std::optional<std::wstring> const& error);
  void FinalizeChat(StreamCardUi& card);
  void StopCardStream(StreamCardUi& card);
  void AppendChatBubble(ChatBubble bubble);
  void AppendNoticeBubble(std::wstring const& text);
  void RefreshChatScopeButtons();

  // theme-aware brushes for code-built cards (MainWindow.Settings.cpp)
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush ThemeBrush(winrt::Windows::UI::Color color);
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush ThemeBorderBrush();
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush ThemePanelBrush();
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush ThemePanelAltBrush();

  // text sources (parity with the mac model)
  std::wstring SelectionText();  // empty when none
  void CurrentPageText(std::function<void(std::wstring, int)> on_ready);
  void DocText(std::function<void(std::wstring, int)> on_ready);
  std::wstring LanguageName(rivet_app::TargetLanguage lang) const;
  bool EnsureProviderConfigured(std::vector<std::shared_ptr<StreamCardUi>>& list);

  // ---- settings pane (MainWindow.Settings.cpp) ----
  void BuildSettingsPane();
  void LoadSettingsIntoPane();
  void SwitchPreset(std::wstring const& id);
  std::wstring SelectedProviderId() const;
  void SaveSettingsFromPane();
  void SaveApiKeyFromPane();
  void TestConnectionFromPane();
  void SetSettingsStatus(std::wstring const& message, bool is_error);
  void ShowAboutDialog();
  std::wstring HostPrefsDir() const;

  // ---- EPUB (MainWindow.Epub.cpp) ----
  void OpenEpub(std::wstring const& path, int resume_chapter = 1);
  void CloseEpub();
  void ActivateEpub();
  void LoadEpubBook(std::shared_ptr<EpubBook> book);
  void GoToChapter(EpubBook& book, int chapter);
  void ApplyEpubTheme(EpubBook& book);
  void RenderEpubToc(EpubBook& book);
  void NavigateToEpubController(
      std::shared_ptr<EpubBook> book,
      std::vector<std::pair<int, std::wstring>> const& sections);
  void OnEpubWebMessage(
      EpubBook& book,
      winrt::Microsoft::Web::WebView2::Core::CoreWebView2WebMessageReceivedEventArgs const& args);
  std::wstring BuildEpubHtml(
      EpubBook const& book,
      std::vector<std::pair<int, std::wstring>> const& sections);

  // ---- page ops (MainWindow.Pages.cpp) ----
  void BuildPageOpsRow();
  void RunPageEdit(int op);  // 0 rotate / 1 blank / 2 delete / 3 extract / 4 merge / 5 bake
  void ReloadTabDocument(std::shared_ptr<PdfTab> tab,
                         std::vector<std::uint8_t> data);
  void SaveEditResult(std::vector<std::uint8_t> data,
                      std::wstring const& default_name);
  void RunMergeDialog();
  void RunBakeDialog();
  void PickPdfFile(std::function<void(std::wstring const&)> on_picked);

  // ---- tools (MainWindow.Tools.cpp) — watermark + exports ----
  void ShowWatermarkDialog();
  void ApplyWatermark(std::shared_ptr<PdfTab> tab, std::wstring const& text,
                      int size, int opacity_percent);
  void ExportPageImage();
  void ExportDocumentText();
  // One typed save panel for every export (mac saveData parity).
  void SaveData(std::vector<std::uint8_t> data, std::wstring const& default_name,
                std::wstring const& type_label, std::wstring const& extension,
                std::function<void(bool)> on_complete = {},
                std::wstring const& protected_original = {});
  void SaveEditedPdf(std::shared_ptr<PdfTab> const& tab,
                     std::function<void(bool)> on_complete = {});

  // ---- AcroForm pane (MainWindow.Forms.cpp) ----
  void BuildFormsPane();
  void ShowFormStatus(std::wstring const& text, bool is_error);
  void RefreshFormFields();
  void RebuildFormFieldsUi();
  void ApplyFormFieldText(std::wstring const& name);
  void ExportFilledPdf();
  std::vector<FormField> form_fields_;
  std::map<std::wstring, std::wstring> form_text_values_;
  std::map<std::wstring, bool> form_check_values_;
  winrt::Microsoft::UI::Xaml::Controls::StackPanel forms_pane_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel forms_list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock forms_status_{nullptr};
  bool forms_busy_ = false;

  // ---- keyboard (MainWindow.xaml.cpp) ----
  void BuildKeyboardAccelerators();

  // ---- updates (MainWindow.Update.cpp) ----
  void StartAutoUpdateCheck();
  void RunUpdateCheck(bool manual);
  void StartUpdateDownload(std::int64_t size_bytes, std::wstring const& version);
  void PollUpdateState(std::shared_ptr<rivet::windows::Backend> const& backend);
  void HandleUpdatePoll(std::string const& payload);
  void StopUpdateTimer();
  void FailDownload(std::wstring const& message);
  void ShowInstallConsent(std::wstring const& zip_path);
  void InstallDownloadedUpdate(std::wstring const& zip_path);
  void HandleInstallMarkers();
  bool IsDevCopy();
  bool IsMsiInstall();

  // ---- state ----
  std::shared_ptr<rivet::windows::Backend> backend_;
  std::unique_ptr<rivet_app::API> api_;
  rivet_app::SettingsView settings_{};
  std::string ui_language_ = "system";  // system | zh | en (host pref, relaunch applies)
  std::string tts_voice_ = "";          // display name; empty = system default
  double tts_rate_ = 1.0;
  bool settings_loaded_ = false;
  std::vector<rivet_app::ProviderPreset> presets_;
  std::vector<rivet_app::RecentEntry> recents_;
  bool ready_ = false;

  std::vector<std::shared_ptr<PdfTab>> tabs_;
  std::shared_ptr<PdfTab> active_pdf_;
  // One native tab and document state per EPUB, never replace an opened file.
  std::vector<std::shared_ptr<EpubBook>> epubs_;
  std::shared_ptr<EpubBook> epub_;  // selected EPUB; null while a PDF is active
  bool epub_active_ = false;
  bool window_close_prompt_active_ = false;

  bool left_visible_ = false;
  int left_mode_ = 0;  // 0 thumbnails / 1 outline / 2 notes
  bool ai_visible_ = false;  // reading canvas first; user pref may override
  int ai_pane_ = 0;
  std::string theme_ = "system";  // system | light | dark

  // find
  std::vector<pdfgist::FindHit> find_hits_;
  int find_index_ = 0;
  bool find_open_ = false;

  // AI cards
  std::vector<std::shared_ptr<StreamCardUi>> translate_cards_;
  std::vector<std::shared_ptr<StreamCardUi>> summarize_cards_;
  std::vector<ChatBubble> chat_bubbles_;
  std::map<std::int64_t, std::shared_ptr<StreamCardUi>> streams_;
  std::vector<std::vector<std::string>> chat_history_;
  std::wstring chat_pending_question_;
  bool chat_busy_ = false;
  int chat_scope_ = 0;  // 0 page / 1 selection / 2 doc

  // settings pane controls (built in code)
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_provider_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox settings_base_url_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox settings_model_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_model_suggest_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::PasswordBox settings_api_key_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock settings_status_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_target_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_view_mode_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_reading_theme_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_language_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ComboBox settings_tts_voice_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Slider settings_tts_rate_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::CheckBox settings_sidecar_{nullptr};
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> preset_stash_;
  bool settings_busy_ = false;
  bool settings_ui_updating_ = false;

  // dynamic content hosts
  winrt::Microsoft::UI::Xaml::Controls::StackPanel translate_list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel summarize_list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel chat_list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel page_ops_row_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock page_ops_hint_{nullptr};
  std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton> ai_tab_buttons_;
  std::vector<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton> chat_scope_buttons_;
  bool settings_built_ = false;

  // timers
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer find_timer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer recents_timer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer anno_timer_{nullptr};
  // One-step undo (taskly banner parity): the annotation the last delete
  // removed and its position; the banner lives for five seconds.
  // Outline follows the reading position: the active entry's page from
  // the last build (only re-scroll when the entry actually changed) and
  // the page-change-triggered rebuild flag.
  int outline_active_page_ = 0;
  bool outline_scroll_pending_ = false;
  std::optional<StoredAnnotation> undo_annotation_;
  std::size_t undo_index_ = 0;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer undo_timer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer check_timer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer update_timer_{nullptr};
  bool update_downloading_ = false;
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
