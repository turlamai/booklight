#lang racket/base

;; PDFGist Rivet backend: the hosts' single data channel. Everything that
;; lived in the old Rust backend (AI streaming, settings, annotations store,
;; recents) plus what is naturally server-side in the new architecture
;; (prompt assembly, provider presets, URL normalization, SSE decode).
;;
;; Streaming model: translate-text/summarize-text/chat return a StreamStart
;; immediately and run the HTTP stream on a worker thread that emits
;; stream-chunk events, then exactly one terminal event:
;;   - stream-done  after [DONE]/EOF, or after stop-stream cancelled it
;;   - stream-error with the provider error message on failure
;; If the host cancels the RPC itself (RVT1 cancel), Rivet tears down the
;; request custodian, the worker dies with it, and Rivet reports the
;; "request cancelled" error — no terminal event is emitted in that path.
;;
;; Scaling conventions at the RPC boundary (RVT1 has no floats):
;;   - RecentEntry.scroll-ratio-scaled = round(scroll_ratio * 100000)
;;   - RecentEntry.last-read-ms        = last_read (unix s) * 1000
;; Annotations cross as raw JSON bytes, so annotation rects keep their
;; page-space floats verbatim; no milli-unit conversion applies there.

(require json
         racket/file
         racket/list
         racket/string
         rivet/backend
         (prefix-in epub: "../racket/booklight/epub.rkt")
         (prefix-in llm: "../racket/booklight/llm.rkt")
         (prefix-in pdfedit: "../racket/booklight/pdfedit.rkt")
         (prefix-in prompts: "../racket/booklight/prompts.rkt")
         (prefix-in recents: "../racket/booklight/recents.rkt")
         (prefix-in annotations: "../racket/booklight/annotations.rkt")
         "../racket/booklight/i18n.rkt"
         "../racket/booklight/providers.rkt"
         "../racket/booklight/settings.rkt"
         "updater.rkt"
         "version.rkt")

(provide start
         start-stdio)

;; ---- schema ----

(define-enum TargetLanguage (zh zh-hant en ja ko fr de es))
(define-enum SummarizeMode (page selection doc))

(define-record ProviderPreset
  ([id : String]
   [label : String]
   [base-url : String]
   [default-model : String]
   [needs-key : Bool]))

(define-record RecentEntry
  ([path : String]
   [page : Int64]
   [scroll-ratio-scaled : Int64]
   [last-read-ms : Int64]))

;; Reading-theme values mirror the settings store: system follows the
;; effective app appearance, paper/night force the light/dark reader page
;; surface. Font scale crosses as permille because RVT1 has no floats.
(define-enum ReadingTheme (system paper night sepia))

(define-record SettingsView
  ([provider : String]
   [base-url : String]
   [model : String]
   [target-language : TargetLanguage]
   [view-mode : String]
   [annotation-sidecar : Bool]
   [reading-theme : ReadingTheme]
   [reading-font-scale-permille : Int64]
   [has-api-key : Bool]
   [recent : (List RecentEntry)]))

(define-record SettingsUpdate
  ([provider : String]
   [base-url : String]
   [model : String]
   [target-language : TargetLanguage]
   [view-mode : String]
   [annotation-sidecar : Bool]))

(define-record AnnotationDocument
  ([data : Bytes]
   [recovery-pending : Bool]))

(define-record StreamStart ([request-id : Int64]))

(define-record TestResult
  ([ok : Bool]
   [error : (Optional String)]
   [models : (Optional (List String))]))

(define-record StreamChunk
  ([request-id : Int64]
   [delta : String]
   [is-reasoning : Bool]))

(define-record StreamDone ([request-id : Int64]))

(define-record StreamError
  ([request-id : Int64]
   [message : String]))

(define-event stream-chunk : StreamChunk)
(define-event stream-done : StreamDone)
(define-event stream-error : StreamError)

;; Hosts can read the active backend locale (zh default); set-locale updates
;; it. The state is named backend-locale because the C++ name of the `locale`
;; setter (set_locale) collided with the set-locale RPC.
(define-state backend-locale : String "zh")

;; ---- stream bookkeeping ----
;;
;; request-id -> (vector custodian state-box). The state box makes the
;; running -> finished|error|stopped transition the single owner of the
;; terminal event, so a stop-stream racing a completing stream can never
;; emit two terminal events.

(define streams-lock (make-semaphore 1))
(define streams (make-hash))
(define next-stream-id (box 0))

(define (with-streams-lock thunk)
  (call-with-semaphore streams-lock thunk))

(define (allocate-stream-id!)
  (with-streams-lock
   (lambda ()
     (define id (add1 (unbox next-stream-id)))
     (set-box! next-stream-id id)
     id)))

;; body: (lambda (emit-chunk) ...) on a worker thread owned by a dedicated
;; custodian; raising inside body turns into stream-error.
(define (start-stream! body)
  (define id (allocate-stream-id!))
  (define cust (make-custodian))
  (with-streams-lock
   (lambda () (hash-set! streams id (vector cust (box 'running)))))
  (parameterize ([current-custodian cust])
    (thread
     (lambda ()
       (with-handlers
           ([exn:fail?
             (lambda (e) (finish-stream! id 'error (exn-message e)))])
         (body
          (lambda (text is-reasoning)
            (stream-chunk (StreamChunk id text is-reasoning))))
         (finish-stream! id 'done)))))
  (StreamStart id))

;; Claims the terminal transition under the lock; only the claimant emits.
(define (finish-stream! id kind [message ""])
  (define claimed?
    (with-streams-lock
     (lambda ()
       (define entry (hash-ref streams id #f))
       (cond
         [(not entry) #f]
         [else
          (define running? (eq? 'running (unbox (vector-ref entry 1))))
          (hash-remove! streams id)
          running?]))))
  (when claimed?
    (if (eq? kind 'done)
        (stream-done (StreamDone id))
        (stream-error (StreamError id message))))
  (void))

;; ---- LLM plumbing ----

(define (launch-llm-stream! messages)
  (define settings (load-settings))
  (define provider (settings-provider settings))
  (start-stream!
   (lambda (emit-chunk)
     (llm:stream-chat!
      #:base-url (provider-config-base-url provider)
      #:api-key (provider-config-api-key provider)
      #:model (provider-config-model provider)
      #:messages messages
      #:on-chunk emit-chunk))))

(define (settings-language-name)
  (settings-target-language (load-settings)))

;; ---- DTO conversion ----

(define (recent-entry->dto recent)
  (RecentEntry
   (recents:recent-file-path recent)
   (recents:recent-file-page recent)
   (inexact->exact (round (* 100000 (recents:recent-file-scroll-ratio recent))))
   (* 1000 (recents:recent-file-last-read recent))))

(define (settings->view settings)
  (define provider (settings-provider settings))
  (SettingsView
   (provider-config-name provider)
   (provider-config-base-url provider)
   (provider-config-model provider)
   (TargetLanguage
    (string->symbol (language-name->code (settings-target-language settings))))
   (settings-view-mode settings)
   (settings-annotation-sidecar settings)
   (ReadingTheme
    (string->symbol (settings-reading-theme settings)))
   (inexact->exact
    (round (* 1000 (settings-reading-font-scale settings))))
   (not (string=? (provider-config-api-key provider) ""))
   (map recent-entry->dto (settings-recent-files settings))))

;; ---- RPCs ----

;; Ensures the config directory exists; cheap warm-up for hosts.
(define-rpc (initialize : Void)
  ;; Startup must never clone a legacy settings tree or materialize a shelf.
  ;; Existing user files/configuration remain in place; no implicit migration.
  (make-directory* (config-dir-path))
  (void))

(define-rpc (list-presets : (List ProviderPreset))
  (for/list ([preset (in-list all-provider-presets)])
    (ProviderPreset
     (provider-preset-id preset)
     (tr (string-append "backend.provider." (provider-preset-id preset))
         (provider-preset-label-zh preset))
     (provider-preset-base-url preset)
     (provider-preset-default-model preset)
     (provider-preset-needs-key? preset))))

;; Parity with list_models in llm.rs: a GET /models round trip. `model` is
;; accepted for future validation but the old connection test never used it.
(define-rpc (test-connection [base-url : String]
                             [api-key : String]
                             [model : String]
                             : TestResult)
  (with-handlers
      ([exn:fail?
        (lambda (e) (TestResult #f (exn-message e) (void)))])
    (define models (llm:list-models base-url api-key))
    (TestResult #t (void) models)))

(define-rpc (translate-text [text : String]
                            [target : TargetLanguage]
                            : StreamStart)
  (define lang
    (language-code->name (symbol->string (enum-case target))))
  (launch-llm-stream! (prompts:translate-messages text lang)))

(define-rpc (summarize-text [text : String]
                            [mode : SummarizeMode]
                            : StreamStart)
  (define lang (settings-language-name))
  (define messages
    (case (enum-case mode)
      [(page) (prompts:summary-page-messages text lang)]
      [(selection) (prompts:summary-selection-messages text lang)]
      [(doc) (prompts:summary-doc-messages text lang)]))
  (launch-llm-stream! messages))

;; context-label feeds the `=== 文档内容（label）===` header so the prompt
;; matches the old UI byte for byte (第 N 页 / 选区 / 全文前 N 页).
;; History elements cross the wire as [role content] pairs: Rivet converts
;; named records only at the top level of an argument, so a
;; (List (List String)) shape is what the generated clients produce.
(define-rpc (chat [context-text : String]
                  [context-label : String]
                  [history : (List (List String))]
                  [user-message : String]
                  : StreamStart)
  (define lang (settings-language-name))
  (define history-pairs
    (for/list ([message (in-list history)]
               #:when (and (pair? message) (pair? (cdr message))))
      (cons (car message) (cadr message))))
  (launch-llm-stream!
   (prompts:build-chat-messages
    context-text context-label lang history-pairs user-message)))

;; Cancels one in-flight stream. Unknown/finished ids are a no-op (parity
;; with llm_stop in the old stack).
(define-rpc (stop-stream [request-id : Int64] : Void)
  (define entry
    (with-streams-lock
     (lambda ()
       (define entry (hash-ref streams request-id #f))
       (when entry
         (set-box! (vector-ref entry 1) 'stopped)
         (hash-remove! streams request-id))
       entry)))
  (when entry
    ;; Killing the custodian aborts in-flight port reads and closes the HTTP
    ;; connection; finish-stream! then finds no entry and stays silent.
    (custodian-shutdown-all (vector-ref entry 0))
    (stream-done (StreamDone request-id)))
  (void))

;; Pure helper for the host-side bilingual paragraph view.
(define-rpc (split-paragraphs [text : String] : (List String))
  (prompts:split-paragraphs text))

(define-rpc (get-settings : SettingsView)
  (settings->view (load-settings)))

;; Preserves the stored api key and recents; hosts never send secrets here.
;; Reading prefs have their own narrow RPC (save-reading-prefs) so the
;; A+/A- steps never round-trip the full settings record.
(define-rpc (save-settings [update : SettingsUpdate] : SettingsView)
  (define lang-name
    (language-code->name
     (symbol->string (enum-case (record-ref update 'target-language)))))
  (define updated
    (update-settings!
     (lambda (current)
       (pdfgist-settings
        (provider-config
         (record-ref update 'provider)
         (record-ref update 'base-url)
         (provider-config-api-key (settings-provider current))
         (record-ref update 'model))
        lang-name
        (settings-recent-files current)
        (record-ref update 'view-mode)
        (record-ref update 'annotation-sidecar)
        (settings-reading-theme current)
        (settings-reading-font-scale current)))))
  (settings->view updated))

;; Reading page surface + EPUB font scale, writable independently of the
;; full settings pane (the typography buttons fire per click).
(define-rpc (save-reading-prefs [theme : ReadingTheme]
                                [font-scale-permille : Int64]
                                : SettingsView)
  (define updated
    (update-settings!
     (lambda (current)
       (pdfgist-settings
        (settings-provider current)
        (settings-target-language current)
        (settings-recent-files current)
        (settings-view-mode current)
        (settings-annotation-sidecar current)
        (symbol->string (enum-case theme))
        (normalize-reading-font-scale (/ (exact->inexact font-scale-permille) 1000.0))))))
  (settings->view updated))

;; The key is written through the secure-store seam (settings.rkt) and never
;; echoed back: get-settings only reports has-api-key.
(define-rpc (save-api-key [key : String] : Void)
  (update-settings!
   (lambda (current) (set-api-key! current key)))
  (void))

;; Annotations cross the boundary as validated JSON bytes, so the on-disk
;; format (including page-space float rects) stays byte-compatible with v1.
(define-rpc (get-annotations [pdf-path : String] [sidecar : Bool] : Bytes)
  (annotations:load-document-bytes pdf-path sidecar))

;; New hosts load the bytes and their recovery state atomically. Keeping this
;; as one request matters for single-channel embedded transports and also
;; prevents an external writer from changing which copy wins between two
;; separate reads. The older narrow RPCs remain for compatible clients.
(define-rpc (load-annotations [pdf-path : String]
                              [sidecar : Bool]
                              : AnnotationDocument)
  (AnnotationDocument
   (annotations:load-document-bytes pdf-path sidecar)
   (annotations:annotation-recovery-pending? pdf-path sidecar)))

;; Hosts use this after loading to distinguish an ordinary stored document
;; from an app-owned recovery draft.  The bytes still come from
;; get-annotations so the established data format and old clients remain
;; compatible; this narrow status RPC only drives visible retry/export UI.
(define-rpc (annotation-recovery-pending [pdf-path : String]
                                         [sidecar : Bool]
                                         : Bool)
  (annotations:annotation-recovery-pending? pdf-path sidecar))

(define-rpc (set-annotations [pdf-path : String]
                             [sidecar : Bool]
                             [data : Bytes]
                             : Void)
  (annotations:store-document-bytes! pdf-path sidecar data)
  (void))

;; The host sends the annotation document it currently holds (unsaved
;; edits included — no flush race) and gets back a Markdown digest.
(define-rpc (export-annotations [data : Bytes] [title : String] : String)
  (annotations:document-bytes->markdown data title))

;; scroll-ratio-scaled: round(ratio * 100000); last-read-ms: unix seconds on
;; disk multiplied by 1000 on the wire.
(define-rpc (update-recents [path : String]
                            [page : Int64]
                            [scroll-ratio-scaled : Int64]
                            : Void)
  (define ratio
    (min 1.0
         (max 0.0 (/ (exact->inexact scroll-ratio-scaled) 100000.0))))
  (update-settings!
   (lambda (current)
     (pdfgist-settings
      (settings-provider current)
      (settings-target-language current)
      (recents:upsert-recent
       (settings-recent-files current)
       (recents:recent-file path
                            ""
                            (max 1 page)
                            ratio
                            (current-seconds)))
      (settings-view-mode current)
      (settings-annotation-sidecar current)
      (settings-reading-theme current)
      (settings-reading-font-scale current))))
  (void))

;; Recents come back newest-first (storage order).
(define-rpc (get-recents : (List RecentEntry))
  (map recent-entry->dto (settings-recent-files (load-settings))))

;; ---- retired bookshelf RPCs (wire compatibility only) ----
;;
;; File-first Booklight does not own a catalog, cover cache, collections, or
;; a database. These RPC shapes remain temporarily for already-generated
;; clients while the native hosts move to File > Open + a small recent list.
;; They NEVER write library.json, covers/, or a document copy.
;; Existing library.json is deliberately left untouched for manual recovery.

(define-record LibraryEntry
  ([path : String]
   [title : String]
   [author : String]
   [kind : String]
   [pages : Int64]
   [added-at-ms : Int64]
   [cover : String]
   [collections : (List String)]
   [status : String]))

(define-rpc (library-import [path : String]
                            [title : String]
                            [author : String]
                            [kind : String]
                            [pages : Int64]
                            [cover : Bytes]
                            : LibraryEntry)
  ;; Old hosts can complete their in-flight import call, but it has no
  ;; persistence, cover generation, copy or library side effect.
  (LibraryEntry path title author kind (max 1 pages) 0 "" '()
                (if (file-exists? path) "ok" "missing")))

(define-rpc (get-library : (List LibraryEntry)) '())
(define-rpc (library-remove [path : String] : Void) (void))
(define-rpc (library-relink [from : String] [to : String] : Void) (void))
(define-rpc (library-scan : (List LibraryEntry)) '())
(define-rpc (library-collection-create [name : String] : Void) (void))
(define-rpc (library-collection-delete [name : String] : Void) (void))
(define-rpc (library-book-collection [path : String]
                                     [name : String]
                                     [add : Bool]
                                     : Void)
  (void))
(define-rpc (library-collections : (List String)) '())

;; Backend-side error/status strings follow this locale ("zh" | "en").
(define-rpc (set-locale [code : String] : Void)
  (set-locale! code)
  (state-set! backend-locale code)
  (void))

;; ---- EPUB reading (domain parses, hosts render) ----

(define-record EpubTocItem
  ([title : String]
   [chapter : Int64]      ; 1-based spine index
   [level : Int64]))

(define-record EpubView
  ([title : String]
   [chapters : Int64]
   [toc : (List EpubTocItem)]))

(define-rpc (epub-open [path : String] : EpubView)
  (define book (epub:open-epub (file->bytes path) path))
  (EpubView
   (epub:epub-book-title book)
   (length (epub:epub-book-spine book))
   (map (λ (t) (EpubTocItem (epub:epub-toc-item-title t)
                            (epub:epub-toc-item-chapter t)
                            (epub:epub-toc-item-level t)))
        (epub:epub-book-toc book))))

;; sanitized chapter body HTML; the host wraps it with the reading CSS
(define-rpc (epub-chapter-html [path : String] [index : Int64] : Bytes)
  (epub:epub-chapter-html (epub:open-epub (file->bytes path) path) index))

;; plain chapter text — selection/chapter AI scope and whole-book search
(define-rpc (epub-chapter-text [path : String] [index : Int64] : String)
  (epub:epub-chapter-text (epub:open-epub (file->bytes path) path) index))

;; v1 getDocText: first N chapters, each capped, "--- 第 i 章 ---" separators
(define-rpc (epub-doc-text [path : String]
                           [max-chapters : Int64]
                           [cap-chars : Int64]
                           : String)
  (epub:epub-doc-text (epub:open-epub (file->bytes path) path)
                      max-chapters cap-chars))

;; ---- page-level editing (issue #1: the domain layer owns PDF surgery;
;; hosts render and save, never reimplement) ----

(define-record EditTextBox
  ([page : Int64]
   [x-ratio-milli : Int64]   ; 0..1000
   [y-ratio-milli : Int64]
   [text : String]
   [size : Int64]))          ; points

;; All editors take the source path (backend reads the bytes) and return
;; the edited document as bytes — the host decides where to save them.
(define-rpc (edit-delete-pages [path : String] [pages : (List Int64)] : Bytes)
  (pdfedit:edit-delete-pages (file->bytes path) pages))

(define-rpc (edit-rotate-pages [path : String]
                               [pages : (List Int64)]
                               [delta : Int64]
                               : Bytes)
  (pdfedit:edit-rotate-pages (file->bytes path) pages delta))

(define-rpc (edit-insert-blank-after [path : String] [page : Int64] : Bytes)
  (pdfedit:edit-insert-blank-after (file->bytes path) page))

(define-rpc (edit-extract-pages [path : String] [pages : (List Int64)] : Bytes)
  (pdfedit:edit-extract-pages (file->bytes path) pages))

(define-rpc (edit-append-doc [path : String] [other-path : String] : Bytes)
  (pdfedit:edit-append-doc (file->bytes path) (file->bytes other-path)))

(define-rpc (edit-bake-text [path : String]
                            [watermark-text : String]
                            [watermark-size : Int64]
                            [watermark-opacity-milli : Int64]
                            [boxes : (List EditTextBox)]
                            : Bytes)
  (pdfedit:edit-bake-text (file->bytes path)
                          #:watermark-text watermark-text
                          #:watermark-size watermark-size
                          #:watermark-opacity-milli watermark-opacity-milli
                          #:boxes
                          (map (λ (b) (pdfedit:edit-box
                                       (record-ref b 'page)
                                       (record-ref b 'x-ratio-milli)
                                       (record-ref b 'y-ratio-milli)
                                       (record-ref b 'text)
                                       (record-ref b 'size)))
                               boxes)))

;; ---- online updates (rivet/distribution; app/updater.rkt owns mechanics) --

;; The Racket side verifies the Ed25519-signed channel manifest and streams
;; the DMG on a background thread; the native host owns installation. The
;; wire payloads are raw jsexpr bytes so hosts JSON-decode them (same shape
;; as the payback family surface).

(define (jsexpr->bytes j)
  (string->bytes/utf-8 (jsexpr->string j)))

(define auto-check-interval-seconds (* 24 60 60))

(define-rpc (check-updates [force : Bool] : Bytes)
  (define last (last-update-check-at))
  (define throttled
    (and (not force)
         (exact-integer? last)
         (< (- (current-seconds) last) auto-check-interval-seconds)))
  (if throttled
      (jsexpr->bytes (hasheq 'status "throttled"))
      (jsexpr->bytes (perform-check!))))

(define-rpc (start-download : Void)
  (start-download!)
  (void))

(define-rpc (update-state : Bytes)
  (jsexpr->bytes (update-state-snapshot)))

;; ---- transports ----

;; Native embedded hosts pass anonymous pipe file descriptors here.
(define (start in-fd out-fd)
  (serve-fds in-fd out-fd))

;; The managed development host speaks the exact same RVT1 protocol over
;; stdin/stdout.
(define (start-stdio)
  (serve (current-input-port) (current-output-port)))

(module+ main
  (start-stdio))
