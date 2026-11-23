#lang racket/base

;; Booklight glaze carrier (#75 S1) — the single web UI's backend adapter.
;;
;; The Racket domain core (racket/) and the rivet backend (app/backend.rkt)
;; are unchanged: this module drives the production backend over real
;; in-process RVT1 frames through rivet/bridge (extracted upstream from
;; Podlens, turlamai/glaze#159 -> turlamai/rivet#493) and exposes every
;; registered RPC 1:1 as a typed glaze route. rivet-schema.json remains the
;; single source of the RPC contract: the bridge codec converts camelCase
;; JSON DTOs to field-ordered wire values and back (Bytes ride base64
;; strings), and scripts/check-rpc-surface.py gates this route surface
;; against the schema.
;;
;; Carrier decision (#75, declared on the issue): production runs glaze's
;; first-class HTTP adapter on 127.0.0.1 with a per-launch token; the native
;; GLZ1 carrier remains available for development (BOOKLIGHT_TRANSPORT=
;; native). S1 ships the carrier + RPC surface + a harness probe page; the
;; product web UI lands in S2.
;;
;; Run:  racket host/main.rkt
;; Dev:  raco glaze dev  /  BOOKLIGHT_TRANSPORT=native for the GLZ1 carrier.

(require glaze/app        ; run-app, open-app-window, current-app
         glaze/api        ; POST, GET, exn:fail:glaze:bad-param
         glaze/api-macros ; define-api-routes
         glaze/capability ; make-capability, allow-permission
         glaze/dialogs    ; make-dialog-routes (native open/save panels)
         glaze/events     ; make-event-bus, bus-broadcast!
         glaze/types      ; GString, GInt64, GBool, GValue
         racket/file
         racket/match
         racket/path
         racket/runtime-path
         net/base64
         json
         (prefix-in engine: "../app/backend.rkt")
         "../app/version.rkt"
         rivet/bridge)

;; The route list schema tooling reads (glaze.rktd points here).
;; start-engine! wires the codec to the in-process RVT1 transport — the
;; app entry and the real-window verifier both call it; schema checks
;; load this module without starting anything.
(provide api start-engine! event-bus probe-dir assemble-public-dir host-capability)

(define-runtime-path schema-path "../rivet-schema.json")
(define schema (call-with-input-file schema-path read-json))
(define codec (make-bridge-codec schema))

(define-runtime-path probe-dir "web")

(define bus (make-event-bus))
(define event-bus bus)

;; ---- engine event relay -------------------------------------------------
;; The backend really emits its three declared stream events. Relay each
;; by name onto the glaze bus so both carriers push it to every window.
;; Payloads must be JSON-serializable: normalize recursively (void → null)
;; before broadcasting — a #<void> would kill an SSE connection mid-stream.
(define (json-safe v)
  (cond
    [(void? v) 'null]
    [(hash? v)
     (for/hash ([(k val) (in-hash v)])
       (values (if (symbol? k) k (json-safe k)) (json-safe val)))]
    [(list? v) (map json-safe v)]
    [(pair? v) (cons (json-safe (car v)) (json-safe (cdr v)))]
    [else v]))

(define engine-events '("stream-chunk" "stream-done" "stream-error"))

(set-bridge-event-handler!
 (lambda (payload)
   (match payload
     [(list name data)
      #:when (and (string? name) (member name engine-events))
      (bus-broadcast! bus (string->symbol name) (json-safe data))]
     [_ (bus-broadcast! bus 'engine-event (json-safe payload))])))

;; ---- one RPC call, JSON in / JSON out ------------------------------------
(define (rpc/json name args)
  (define sym (string->symbol name))
  (define wire-args (bridge-codec-encode codec sym args))
  (define-values (result err) (apply bridge-call name wire-args))
  (when err
    ;; The backend's message is the user-facing message (save-lifecycle
    ;; contract). glaze answers plain exns with a generic "internal error"
    ;; on both carriers; bad-param is the framework's typed channel whose
    ;; message reaches the page (HTTP 400 / native invalid-args).
    (raise (exn:fail:glaze:bad-param (format "~a" err) (current-continuation-marks))))
  (bridge-codec-decode codec sym result))

;; ---- typed RPC surface (1:1 with rivet-schema.json) ----------------------
;; Structured DTOs (records/lists) ride GValue at the transport boundary;
;; their shapes are enforced by the schema-driven codec, and the surface
;; gate keeps names/arity/scalar types aligned with rivet-schema.json.
;; Bytes arguments/results ride base64 strings (GString).

(define-api-routes api-typed
  ;; lifecycle / settings
  [(POST "api/rpc/initialize" #:returns GValue)
   (initialize)
   (rpc/json "initialize" (hasheq))]
  [(POST "api/rpc/get-settings" #:returns GValue)
   (getSettings)
   (rpc/json "get-settings" (hasheq))]
  [(POST "api/rpc/save-settings" #:returns GValue)
   (saveSettings [update GValue])
   (rpc/json "save-settings" (hasheq 'update update))]
  [(POST "api/rpc/save-reading-prefs" #:returns GValue)
   (saveReadingPrefs [theme GString] [fontScalePermille GInt64])
   (rpc/json "save-reading-prefs"
             (hasheq 'theme theme 'fontScalePermille fontScalePermille))]
  [(POST "api/rpc/set-locale" #:returns GValue)
   (setLocale [code GString])
   (rpc/json "set-locale" (hasheq 'code code))]
  [(POST "api/rpc/list-presets" #:returns GValue)
   (listPresets)
   (rpc/json "list-presets" (hasheq))]
  [(POST "api/rpc/save-api-key" #:returns GValue)
   (saveApiKey [key GString])
   (rpc/json "save-api-key" (hasheq 'key key))]
  [(POST "api/rpc/test-connection" #:returns GValue)
   (testConnection [baseUrl GString] [apiKey GString] [model GString])
   (rpc/json "test-connection"
             (hasheq 'baseUrl baseUrl 'apiKey apiKey 'model model))]

  ;; recents / epub
  [(POST "api/rpc/get-recents" #:returns GValue)
   (getRecents)
   (rpc/json "get-recents" (hasheq))]
  [(POST "api/rpc/update-recents" #:returns GValue)
   (updateRecents [path GString] [page GInt64] [scrollRatioScaled GInt64])
   (rpc/json "update-recents"
             (hasheq 'path path 'page page 'scrollRatioScaled scrollRatioScaled))]
  [(POST "api/rpc/epub-open" #:returns GValue)
   (epubOpen [path GString])
   (rpc/json "epub-open" (hasheq 'path path))]
  [(POST "api/rpc/epub-chapter-html" #:returns GString)
   (epubChapterHtml [path GString] [index GInt64])
   (rpc/json "epub-chapter-html" (hasheq 'path path 'index index))]
  [(POST "api/rpc/epub-chapter-text" #:returns GString)
   (epubChapterText [path GString] [index GInt64])
   (rpc/json "epub-chapter-text" (hasheq 'path path 'index index))]
  [(POST "api/rpc/epub-doc-text" #:returns GString)
   (epubDocText [path GString] [maxChapters GInt64] [capChars GInt64])
   (rpc/json "epub-doc-text"
             (hasheq 'path path 'maxChapters maxChapters 'capChars capChars))]
  [(POST "api/rpc/split-paragraphs" #:returns GValue)
   (splitParagraphs [text GString])
   (rpc/json "split-paragraphs" (hasheq 'text text))]

  ;; annotations
  [(POST "api/rpc/get-annotations" #:returns GString)
   (getAnnotations [pdfPath GString] [sidecar GBool])
   (rpc/json "get-annotations" (hasheq 'pdfPath pdfPath 'sidecar sidecar))]
  [(POST "api/rpc/set-annotations" #:returns GValue)
   (setAnnotations [pdfPath GString] [sidecar GBool] [data GString])
   (rpc/json "set-annotations"
             (hasheq 'pdfPath pdfPath 'sidecar sidecar 'data data))]
  [(POST "api/rpc/load-annotations" #:returns GValue)
   (loadAnnotations [pdfPath GString] [sidecar GBool])
   (rpc/json "load-annotations" (hasheq 'pdfPath pdfPath 'sidecar sidecar))]
  [(POST "api/rpc/annotation-recovery-pending" #:returns GBool)
   (annotationRecoveryPending [pdfPath GString] [sidecar GBool])
   (rpc/json "annotation-recovery-pending"
             (hasheq 'pdfPath pdfPath 'sidecar sidecar))]
  [(POST "api/rpc/export-annotations" #:returns GString)
   (exportAnnotations [data GString] [title GString])
   (rpc/json "export-annotations" (hasheq 'data data 'title title))]

  ;; pdf page surgery (domain core; Bytes out = base64)
  [(POST "api/rpc/edit-rotate-pages" #:returns GString)
   (editRotatePages [path GString] [pages GValue] [delta GInt64])
   (rpc/json "edit-rotate-pages" (hasheq 'path path 'pages pages 'delta delta))]
  [(POST "api/rpc/edit-delete-pages" #:returns GString)
   (editDeletePages [path GString] [pages GValue])
   (rpc/json "edit-delete-pages" (hasheq 'path path 'pages pages))]
  [(POST "api/rpc/edit-extract-pages" #:returns GString)
   (editExtractPages [path GString] [pages GValue])
   (rpc/json "edit-extract-pages" (hasheq 'path path 'pages pages))]
  [(POST "api/rpc/edit-insert-blank-after" #:returns GString)
   (editInsertBlankAfter [path GString] [page GInt64])
   (rpc/json "edit-insert-blank-after" (hasheq 'path path 'page page))]
  [(POST "api/rpc/edit-append-doc" #:returns GString)
   (editAppendDoc [path GString] [otherPath GString])
   (rpc/json "edit-append-doc" (hasheq 'path path 'otherPath otherPath))]
  [(POST "api/rpc/edit-bake-text" #:returns GString)
   (editBakeText [path GString] [watermarkText GString] [watermarkSize GInt64]
                 [watermarkOpacityMilli GInt64] [boxes GValue])
   (rpc/json "edit-bake-text"
             (hasheq 'path path 'watermarkText watermarkText
                     'watermarkSize watermarkSize
                     'watermarkOpacityMilli watermarkOpacityMilli 'boxes boxes))]

  ;; AI (BYOK; preview counts before any network send)
  [(POST "api/rpc/preview-ai-send" #:returns GValue)
   (previewAiSend [action GString] [scope GString] [contentText GString]
                  [history GValue] [userMessage GString])
   (rpc/json "preview-ai-send"
             (hasheq 'action action 'scope scope 'contentText contentText
                     'history history 'userMessage userMessage))]
  [(POST "api/rpc/translate-text" #:returns GValue)
   (translateText [text GString] [target GString])
   (rpc/json "translate-text" (hasheq 'text text 'target target))]
  [(POST "api/rpc/summarize-text" #:returns GValue)
   (summarizeText [text GString] [mode GString])
   (rpc/json "summarize-text" (hasheq 'text text 'mode mode))]
  [(POST "api/rpc/chat" #:returns GValue)
   (chat [contextText GString] [contextLabel GString] [history GValue]
         [userMessage GString])
   (rpc/json "chat"
             (hasheq 'contextText contextText 'contextLabel contextLabel
                     'history history 'userMessage userMessage))]
  [(POST "api/rpc/stop-stream" #:returns GValue)
   (stopStream [requestId GInt64])
   (rpc/json "stop-stream" (hasheq 'requestId requestId))]

  ;; updates
  [(POST "api/rpc/check-updates" #:returns GString)
   (checkUpdates [force GBool])
   (rpc/json "check-updates" (hasheq 'force force))]
  [(POST "api/rpc/start-download" #:returns GValue)
   (startDownload)
   (rpc/json "start-download" (hasheq))]
  [(POST "api/rpc/update-state" #:returns GString)
   (updateState)
   (rpc/json "update-state" (hasheq))]

  ;; ---- host capabilities (not engine RPCs; outside the surface gate) ----
  [(POST "api/health" #:returns GValue)
   (healthHost)
   (hasheq 'ok "true" 'version app-version)]

  ;; ---- acceptance harness gate (#75 S1) ---------------------------------
  ;; The probe page carries an in-page journey harness (?e2e=1) because
  ;; glaze exposes no JS injection into the webview. These routes are inert
  ;; unless BOOKLIGHT_E2E=1 (verify.rkt / CI only).
  [(POST "api/host/e2e" #:returns GValue)
   (e2e)
   (hasheq 'enabled (e2e-enabled?))]
  [(POST "api/host/e2e_fixtures" #:returns GValue)
   (e2eFixtures)
   (host-e2e-fixtures)]
  [(POST "api/host/e2e_report" #:returns GBool)
   (e2eReport [payload GValue])
   (host-e2e-report! payload)
   #t]
  [(POST "api/host/e2e_report_get" #:returns GValue)
   (e2eReportGet)
   (or (unbox e2e-report-box) (hasheq 'phase "waiting"))]
  [(POST "api/host/e2e_reset" #:returns GBool)
   (e2eReset)
   (when (e2e-enabled?) (set-box! e2e-report-box #f))
   #t]

  ;; ---- S2 product web UI host capabilities -------------------------------
  ;; i18n single source (shared/i18n) served to the web UI; no copied string
  ;; table. zh is the default locale.
  [(GET "api/host/i18n" #:returns GValue)
   (hostI18n)
   (hasheq 'zh (call-with-input-file i18n-zh-path read-json)
           'en (call-with-input-file i18n-en-path read-json))]
  ;; Read-only file bytes for the pdf.js render layer (S0 spike). The page
  ;; only ever passes paths the user picked via the native open dialog or
  ;; from recents; the capability grant scopes this to the local user.
  [(POST "api/host/file" #:returns GValue #:permission 'host:file)
   (hostFile [path GString])
   (host-file-bytes path)]
  ;; Explicit save-as for page surgery / form export: the page only writes
  ;; to a destination the user picked in the native save panel. The source
  ;; is never overwritten implicitly (DATA-FORMAT save semantics).
  [(POST "api/host/write" #:returns GBool #:permission 'host:write)
   (hostWrite [path GString] [data GString])
   (host-write-bytes path data)
   #t]
  ;; Scratch file for chaining page-surgery previews: the edit RPCs read a
  ;; path, so the carrier stages the current preview bytes in a managed
  ;; temp file and hands back its path (never the user's source).
  [(POST "api/host/scratch" #:returns GValue #:permission 'host:write)
   (hostScratch [data GString])
   (hasheq 'path (host-scratch-path data))])

(define api (append api-typed (make-dialog-routes)))

;; Runtime authority for the production window: native open panel + the
;; read-only file bridge. The verifier runs without a capability (open-route
;; mode) so headless CI needs no interactive grant.
(define host-capability
  (make-capability "booklight" (list 'dialog:open 'dialog:save 'host:file 'host:write)))

(define-runtime-path i18n-zh-path "../shared/i18n/zh.json")
(define-runtime-path i18n-en-path "../shared/i18n/en.json")

(define (host-file-bytes path)
  (define p (string->path path))
  (define ext (path-get-extension p))
  (unless (and (file-exists? p) ext
               (member (string-downcase (bytes->string/utf-8 ext))
                       '(".pdf" ".epub")))
    (bad-param (format "not a readable pdf/epub: ~a" path)))
  (define bytes (file->bytes p))
  (when (> (bytes-length bytes) (* 64 1024 1024))
    (bad-param "file too large for the render bridge"))
  (hasheq 'data (bytes->base64 bytes)))

(define (bytes->base64 b)
  (bytes->string/utf-8 (base64-encode b #"")))

(define (host-write-bytes path data)
  (define p (string->path path))
  (define dir (path-only p))
  (unless (and dir (directory-exists? dir))
    (bad-param (format "destination directory missing: ~a" path)))
  (define bytes (base64-decode (string->bytes/utf-8 data)))
  ;; Atomic replace: write a sibling temp then rename, so a failed write
  ;; never truncates an existing destination.
  (define tmp (build-path dir (format ".booklight-save-~a" (current-seconds))))
  (call-with-output-file tmp (lambda (out) (write-bytes bytes out)) #:exists 'truncate/replace)
  (rename-file-or-directory tmp p #t))

(define (host-scratch-path data)
  (define dir (build-path (find-system-path 'temp-dir) "booklight-scratch"))
  (make-directory* dir)
  (define p (build-path dir (format "preview-~a-~a.pdf"
                                    (current-seconds) (current-inexact-milliseconds))))
  (call-with-output-file p
    (lambda (out) (write-bytes (base64-decode (string->bytes/utf-8 data)) out))
    #:exists 'truncate/replace)
  (path->string p))

;; ---- runtime public dir ---------------------------------------------------
;; The web UI's token sheet (design/prototype/shared.css) and the vendored
;; pdf.js (spike/pdfjs/vendor) stay single-source in the repo; the carrier
;; assembles a runtime public dir at startup instead of committing copies.
(define-runtime-path web-dir "web")
(define-runtime-path shared-css-path "../design/prototype/shared.css")
(define-runtime-path pdfjs-dist-path "../spike/pdfjs/vendor/pdfjs-dist")

(define (assemble-public-dir)
  (define dir (build-path (find-system-path 'temp-dir)
                          (format "booklight-public-~a-~a"
                                  (current-seconds) (current-inexact-milliseconds))))
  ;; copy-directory/files creates `dir` as the copy of web-dir; pre-creating
  ;; it would make the copy collide with the existing directory.
  (copy-directory/files web-dir dir)
  (copy-file shared-css-path (build-path dir "shared.css"))
  (copy-directory/files pdfjs-dist-path (build-path dir "pdfjs"))
  dir)

;; ---- harness gate state ---------------------------------------------------
(define (e2e-enabled?) (equal? (getenv "BOOKLIGHT_E2E") "1"))
(define e2e-report-box (box #f))

(define-runtime-path fixture-epub "../racket/tests/fixtures/sample.epub")
(define-runtime-path fixture-pdf "../spike/pdfjs/fixture.pdf")

(define (host-e2e-fixtures)
  (unless (e2e-enabled?) (bad-param "e2e fixtures: harness not enabled"))
  ;; The journey writes annotation sidecars next to the PDF (file-first
  ;; contract); hand it a disposable copy so the committed fixture tree
  ;; stays untouched in CI and in local checkouts.
  (define dir (make-temporary-file "booklight-e2e-~a"))
  (delete-file dir)
  (make-directory dir)
  (define pdf-copy (build-path dir "fixture.pdf"))
  (copy-file fixture-pdf pdf-copy)
  (hasheq 'epub (path->string fixture-epub) 'pdf (path->string pdf-copy)))

(define (host-e2e-report! payload)
  (when (e2e-enabled?) (set-box! e2e-report-box payload))
  (void))

(define (bad-param message)
  (raise (exn:fail:glaze:bad-param message (current-continuation-marks))))

;; ---- in-process engine transport -----------------------------------------
;; The backend serves RVT1 over a pipe pair inside this process: static
;; require above keeps raco exe embedding working (a runtime dynamic-require
;; cannot resolve source paths inside a distribution).
(define engine-started? #f)

(define (engine:start/ports)
  (define-values (c2s-in c2s-out) (make-pipe))
  (define-values (s2c-in s2c-out) (make-pipe))
  (define t
    (thread
     (lambda ()
       (parameterize ([current-input-port c2s-in]
                      [current-output-port s2c-out])
         (engine:start-stdio)))))
  (values t s2c-in c2s-out))

(define (start-engine!)
  (unless engine-started?
    (start-bridge! engine:start/ports)
    (set! engine-started? #t)))

(module+ main
  (start-engine!)
  (define transport
    (or (let ([v (getenv "BOOKLIGHT_TRANSPORT")])
          (and (member v '("native" "http")) (string->symbol v)))
        'http))
  (define-values (_kind stop)
    (run-app
     #:public-dir (assemble-public-dir)
     #:api api
     #:transport transport
     #:api-token (or (getenv "GLAZE_API_TOKEN") #t)
     #:capability host-capability
     #:events bus
     #:title "Booklight"
     #:width 1200
     #:height 800
     #:app-id "booklight"))
  (stop-bridge!)
  (stop))