#lang racket/base

;; Server-level RVT1 test: drives the real app/backend.rkt over a pipe pair
;; (the same bytes a native host speaks) and a local fake OpenAI-compatible
;; server on an ephemeral port. Covers the RPC surface, i18n defaults,
;; stream-chunk/stream-done/stream-error events, stop-stream cancellation,
;; annotations round-trips, and the recents trim policy.

(require racket/bool
         racket/file
         json
         racket/list
         racket/path
         racket/runtime-path
         racket/string
         racket/tcp
         rackunit
         rivet/backend
         rivet/protocol
         "../../app/backend.rkt"
         "../booklight/fnv.rkt"
         "../booklight/settings.rkt")

;; ---- watchdog: a hung stream must fail the file, not hang CI ----
(define test-done (make-semaphore))
(define watchdog
  (thread
   (lambda ()
     (define outcome
       (sync/timeout 180 (handle-evt test-done (lambda (_) 'done))))
     (unless (eq? outcome 'done)
       (fprintf (current-error-port) "backend test timed out\n")
       (exit 1)))))

(define temp-config (make-temporary-file "pdfgist-backend-~a" 'directory))
(define pdf-path (path->string (build-path temp-config "doc.pdf")))

;; ---- fake OpenAI-compatible server ----

(define requests-log (box '()))
(define server-mode (box 'fast))

(define (record-request! request)
  (set-box! requests-log (cons request (unbox requests-log))))

(define (read-http-request! cin)
  (define request-line (read-line cin 'return-linefeed))
  (define headers
    (let loop ([acc '()])
      (define line (read-line cin 'return-linefeed))
      (cond
        [(or (eof-object? line) (string=? line "")) (reverse acc)]
        [else (loop (cons line acc))])))
  (define content-length
    (for/or ([header (in-list headers)])
      (define m (regexp-match #px"(?i:^content-length: *([0-9]+))" header))
      (and m (string->number (cadr m)))))
  (define body
    (if content-length (read-bytes content-length cin) #""))
  (vector request-line headers body))

(define (respond-json! cout status body-string)
  (fprintf cout "HTTP/1.1 ~a\r\n" status)
  (fprintf cout "Content-Type: application/json\r\n")
  (fprintf cout "Content-Length: ~a\r\n\r\n" (string-length body-string))
  (display body-string cout)
  (flush-output cout))

(define (write-sse-chunk! cout payload)
  (write-chunked! cout (string->bytes/utf-8 payload)))

(define (write-chunked! cout payload-bytes)
  (fprintf cout "~x\r\n" (bytes-length payload-bytes))
  (write-bytes payload-bytes cout)
  (display "\r\n" cout)
  (flush-output cout))

(define (finish-chunked! cout)
  (display "0\r\n\r\n" cout)
  (flush-output cout))

(define (respond-sse-fast! cout)
  (fprintf cout "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n")
  (flush-output cout)
  (write-sse-chunk! cout "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"思考过程\"}}]}\n\n")
  (write-sse-chunk! cout "data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\n")
  (write-sse-chunk! cout "data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\n")
  (write-sse-chunk! cout "data: [DONE]\n\n")
  (finish-chunked! cout))

(define (respond-sse-slow! cout)
  (fprintf cout "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n")
  (flush-output cout)
  (write-sse-chunk! cout "data: {\"choices\":[{\"delta\":{\"content\":\"慢\"}}]}\n\n")
  ;; Keep streaming until the backend tears the socket down (broken pipe).
  (let loop ([i 0])
    (when (< i 200)
      (sleep 0.2)
      (write-sse-chunk! cout "data: {\"choices\":[{\"delta\":{\"content\":\"慢\"}}]}\n\n")
      (loop (add1 i))))
  (finish-chunked! cout))

(define (fake-handler cin cout)
  (define request (read-http-request! cin))
  (record-request! request)
  (define request-line (vector-ref request 0))
  (cond
    [(string-prefix? request-line "GET /v1/models")
     (respond-json! cout
                    "200 OK"
                    "{\"data\":[{\"id\":\"mock-small\"},{\"id\":\"mock-large\"}]}")]
    [(string-prefix? request-line "POST /v1/chat/completions")
     (if (eq? (unbox server-mode) 'slow)
         (respond-sse-slow! cout)
         (respond-sse-fast! cout))]
    [else
     (respond-json! cout "404 Not Found" "{\"error\":\"not found\"}")])
  (close-input-port cin)
  (close-output-port cout))

(define listener (tcp-listen 0 16 #f "127.0.0.1"))
(define-values (_lh fake-port _rh _rp) (tcp-addresses listener #t))
(define fake-base-url (format "http://127.0.0.1:~a" fake-port))
(define acceptor
  (thread
   (lambda ()
     (let loop ()
       (with-handlers ([exn:fail? void])
         (define-values (cin cout) (tcp-accept listener))
         (thread (lambda ()
                   (with-handlers ([exn:fail? void])
                     (fake-handler cin cout))))
         (loop))))))

;; ---- start the backend over pipes under the temp config dir ----

(define-values (server-in client-out) (make-pipe))
(define-values (client-in server-out) (make-pipe))
(define server-thread
  (parameterize ([current-config-dir temp-config])
    (thread (lambda () (serve server-in server-out)))))

(define hello (read-frame client-in))
(check-equal? (frame-type hello) message:hello)

(define next-id (box 0))

(define (send-request name . arguments)
  (define id (add1 (unbox next-id)))
  (set-box! next-id id)
  (write-frame
   (frame message:request id (encode-value (cons name arguments)))
   client-out)
  id)

;; Reads frames until the terminal frame for `id`, returning it and every
;; Event payload observed on the way (events and responses interleave).
(define (read-terminal id)
  (let loop ([events '()])
    (define f (read-frame client-in))
    (cond
      [(= (frame-type f) message:event)
       (loop (cons (decode-value (frame-payload f)) events))]
      [(= (frame-id f) id)
       (values f (reverse events))]
      [else (loop events)])))

(define (call name . arguments)
  (define id (apply send-request name arguments))
  (define-values (response events) (read-terminal id))
  (when (= (frame-type response) message:error)
    (error 'call "~a" (decode-value (frame-payload response))))
  (values (decode-value (frame-payload response)) events))

(define (event-name payload) (car payload))
(define (event-value payload) (cadr payload))

;; Collects stream terminal info for `stream-id` from a mixed event list,
;; reading more frames until the terminal event arrives.
(define (await-stream-done stream-id [seen '()])
  (define arrived (reverse seen))
  (define relevant
    (filter (lambda (payload)
              (and (member (event-name payload)
                           '("stream-chunk" "stream-done" "stream-error"))
                   (= (list-ref (event-value payload) 0) stream-id)))
            arrived))
  (define done?
    (for/or ([payload (in-list relevant)])
      (string=? (event-name payload) "stream-done")))
  (define failed?
    (for/or ([payload (in-list relevant)])
      (string=? (event-name payload) "stream-error")))
  (cond
    [(or done? failed?) relevant]
    [else
     (define f (read-frame client-in))
     (define payload (decode-value (frame-payload f)))
     (await-stream-done
      stream-id
      (if (= (frame-type f) message:event)
          (cons payload seen)
          seen))]))

;; ---- RPC surface ----

;; An existing catalog belongs to the user. A file-first startup must not
;; import it, overwrite it or silently create a new catalog from recents.
(define retired-library-path (build-path temp-config "library.json"))
(define retired-library-bytes #"{\"format\":1,\"books\":[{\"path\":\"/old.pdf\"}]}")
(call-with-output-file retired-library-path
  (lambda (out) (write-bytes retired-library-bytes out))
  #:exists 'truncate)

(define-values (init-result _) (call "initialize"))
(check-true (void? init-result))
(check-equal? (file->bytes retired-library-path) retired-library-bytes)
(delete-file retired-library-path)
(check-false (directory-exists? (build-path temp-config "covers")))

;; Presets are the v1 types.ts table, zh labels, needs-key policy.
(define-values (presets preset-events) (call "list-presets"))
(check-equal? (length presets) 12)
(check-equal? (map car presets)
              '("glm-coding" "zhipu" "deepseek" "qwen" "doubao" "doubao-coding"
                "moonshot" "kimi-coding" "siliconflow" "openai" "ollama" "custom"))
(define glm-coding (car presets))
(check-equal? (list-ref glm-coding 1) "GLM Coding 套餐（智谱）")
(check-equal? (list-ref glm-coding 2) "https://open.bigmodel.cn/api/coding/paas/v4")
(check-equal? (list-ref glm-coding 3) "glm-5")
(define deepseek (list-ref presets 2))
(check-equal? (list-ref deepseek 1) "DeepSeek")
(check-equal? (list-ref deepseek 3) "deepseek-chat")
(define ollama (list-ref presets 10))
(check-equal? (list-ref ollama 2) "http://localhost:11434/v1")
(check-false (list-ref ollama 4))
(check-true (list-ref deepseek 4))
(check-true (null? preset-events))

;; Default settings view (no file yet). Wire form = ordered list:
;; provider base-url model target-language view-mode annotation-sidecar
;; reading-theme reading-font-scale-permille has-api-key recent.
(define-values (initial-settings _e1) (call "get-settings"))
(check-equal? (list-ref initial-settings 0) "deepseek")
(check-equal? (list-ref initial-settings 1) "https://api.deepseek.com/v1")
(check-equal? (list-ref initial-settings 2) "deepseek-chat")
(check-equal? (list-ref initial-settings 3) "zh")
(check-equal? (list-ref initial-settings 4) "single")
(check-true (list-ref initial-settings 5))
(check-equal? (list-ref initial-settings 6) "system")
(check-equal? (list-ref initial-settings 7) 1000)
(check-false (list-ref initial-settings 8))
(check-true (null? (list-ref initial-settings 9)))

;; save-settings keeps the (still unset) key, updates the rest.
;; SettingsUpdate crosses as a record (wire form = ordered list).
(define-values (saved-settings _e2)
  (call "save-settings"
        (list "deepseek" fake-base-url "mock-small" "zh-hant" "double" #t)))
(check-equal? (list-ref saved-settings 1) fake-base-url)
(check-equal? (list-ref saved-settings 2) "mock-small")
(check-equal? (list-ref saved-settings 3) "zh-hant")
(check-equal? (list-ref saved-settings 4) "double")
(check-true (list-ref saved-settings 5))
(define-values (reread-settings _e3) (call "get-settings"))
(check-equal? (list-ref reread-settings 1) fake-base-url)

;; save-reading-prefs touches only the reading fields (permille wire form,
;; out-of-range scales clamp in the store).
(define-values (reading-saved _er1)
  (call "save-reading-prefs" "night" 1250))
(check-equal? (list-ref reading-saved 6) "night")
(check-equal? (list-ref reading-saved 7) 1250)
(define-values (reading-clamped _er2)
  (call "save-reading-prefs" "paper" 99999))
(check-equal? (list-ref reading-clamped 6) "paper")
(check-equal? (list-ref reading-clamped 7) 2000)
;; The full save-settings RPC preserves the reading fields it does not own.
(define-values (saved-settings-2 _e2b)
  (call "save-settings"
        (list "deepseek" fake-base-url "mock-small" "zh-hant" "double" #t)))
(check-equal? (list-ref saved-settings-2 6) "paper")
(check-equal? (list-ref saved-settings-2 7) 2000)

;; save-api-key: masked on the wire, persisted on disk.
(define-values (_key-result _e0) (call "save-api-key" "sk-test"))
(check-true (void? _key-result))
(define-values (keyed-settings _e4) (call "get-settings"))
(check-true (list-ref keyed-settings 8))
(check-false
 (string-contains? (format "~s" keyed-settings) "sk-test"))
(check-equal?
 (parameterize ([current-config-dir temp-config])
   (provider-config-api-key
    (settings-provider (load-settings))))
 "sk-test")

;; test-connection drives GET /v1/models through the fake server.
(define-values (connection-ok _e5) (call "test-connection" fake-base-url "sk-test" "mock-small"))
(check-equal? (list-ref connection-ok 0) #t)
(check-true (void? (list-ref connection-ok 1)))
(check-equal? (list-ref connection-ok 2) (list "mock-small" "mock-large"))

;; Connection failure surfaces as ok=#f with the i18n connect prefix.
(define-values (connection-fail _e6) (call "test-connection" "http://127.0.0.1:1" "" "x"))
(check-false (list-ref connection-fail 0))
(check-true (string-prefix? (list-ref connection-fail 1) "连接失败："))
(check-true (void? (list-ref connection-fail 2)))

;; ---- recents ----

(for ([i (in-range 13)])
  (call "update-recents" (format "/f/~a.pdf" i) (add1 i) (* 1000 i)))
(define-values (recents _e7) (call "get-recents"))
(check-equal? (length recents) 12)
(check-equal? (list-ref (car recents) 0) "/f/12.pdf")
(check-equal? (list-ref (car recents) 1) 13)
(check-equal? (list-ref (car recents) 2) 12000)
(check-true (> (list-ref (car recents) 3) 1600000000000))

;; Ratio clamping + page floor at the boundary.
(call "update-recents" "/f/clamp.pdf" 0 150000)
(define-values (recents-after-clamp _e8) (call "get-recents"))
(check-equal? (list-ref (car recents-after-clamp) 0) "/f/clamp.pdf")
(check-equal? (list-ref (car recents-after-clamp) 1) 1)
(check-equal? (list-ref (car recents-after-clamp) 2) 100000)

;; ---- epub (domain parse over the channel) ----

(define-runtime-path sample-epub "fixtures/sample.epub")
(define epub-path (path->string (build-path temp-config "book.epub")))
(copy-file sample-epub epub-path #t)

;; The retired library import RPC is a compatibility shim, not a catalog.
;; EPUB metadata is read when the real file is opened. Import must not create
;; cover files or a library.json database.
(define-values (epub-shelf-entry _e15-cover)
  (call "library-import" epub-path "The Sample Book" "" "epub" 2 #""))
(check-equal? (list-ref epub-shelf-entry 0) epub-path)
(check-equal? (list-ref epub-shelf-entry 6) "")
(check-false (file-exists? (build-path temp-config "library.json")))
(check-false (directory-exists? (build-path temp-config "covers")))
(define-values (empty-shelf _e15-read) (call "get-library"))
(check-equal? empty-shelf '())

(define-values (epub-view _e16) (call "epub-open" epub-path))
;; record shape: (title chapters (toc-items...))
(check-equal? (list-ref epub-view 0) "The Sample Book")
(check-equal? (list-ref epub-view 1) 2)
(define epub-toc (list-ref epub-view 2))
(check-equal? (length epub-toc) 3)

(define-values (epub-html _e17) (call "epub-chapter-html" epub-path 1))
(check-true (and (regexp-match? #"First Chapter" epub-html) #t))
(check-false (and (regexp-match? #"<script" epub-html) #t))

(define-values (epub-text _e18) (call "epub-chapter-text" epub-path 2))
(check-true (string-contains? epub-text "The quick brown fox"))

;; Deprecated collection commands are harmless no-ops: no catalog is
;; recreated even if an old native client invokes one after an upgrade.
(define-values (_col-created _ec1) (call "library-collection-create" "旧合集"))
(define-values (collections-with-empty _ec2) (call "library-collections"))
(check-equal? collections-with-empty '())
(check-false (file-exists? (build-path temp-config "library.json")))

(define-values (epub-doc _e19) (call "epub-doc-text" epub-path 12 24000))
(check-true (string-contains? epub-doc "--- 第 1 章 ---"))

;; the guide.epub the live host was opening when requests stalled
(define guide-src "/var/folders/y5/qjmwcjr92fb3_6wzf7vwwrhr0000gn/T/guide.epub")
(when (file-exists? guide-src)
  (define guide-path (path->string (build-path temp-config "guide.epub")))
  (copy-file guide-src guide-path #t)
  (define-values (guide-view _e20) (call "epub-open" guide-path))
  (check-equal? (list-ref guide-view 0) "Rivet Field Guide")
  (define-values (guide-html _e21) (call "epub-chapter-html" guide-path 2))
  (check-true (and (regexp-match? #"RVT1" guide-html) #t))
  (define-values (guide-text _e22) (call "epub-chapter-text" guide-path 2))
  (check-true (string-contains? guide-text "milli-units")))

;; ---- split-paragraphs ----

;; split-paragraphs
;; The whole page merges below the 400-char target into a single paragraph.
(define-values (paragraphs _e9) (call "split-paragraphs" "第一行\n第二行\nHello\nworld"))
(check-equal? paragraphs (list "第一行第二行Hello world"))

;; ---- annotations ----

(define-values (empty-annotations _e10) (call "get-annotations" pdf-path #f))
(check-equal?
 (bytes->string/utf-8 empty-annotations)
 "{\n  \"annotations\": [],\n  \"bookmarks\": []\n}")
(define-values (no-annotation-recovery _e10b)
  (call "annotation-recovery-pending" pdf-path #f))
(check-false no-annotation-recovery)
(define-values (empty-annotation-document _e10c)
  (call "load-annotations" pdf-path #f))
(check-equal? (first empty-annotation-document) empty-annotations)
(check-false (second empty-annotation-document))

;; Legacy bare arrays are load-compatible but rejected on save.
(define legacy-id (send-request "set-annotations" pdf-path #f #"[]"))
(define-values (legacy-response _e14) (read-terminal legacy-id))
(check-equal? (frame-type legacy-response) message:error)
(check-true
 (string-contains? (decode-value (frame-payload legacy-response)) "批注数据格式不正确"))

(call "set-annotations"
      pdf-path
      #f
      (string->bytes/utf-8
       "{\"annotations\":[{\"id\":\"a1\",\"page\":2,\"rects\":[{\"x\":1.5,\"y\":2.5,\"width\":30,\"height\":10}],\"excerpt\":\"摘录\"}]}"))
(define-values (stored-annotations _e11) (call "get-annotations" pdf-path #f))
(define stored-value (read-json (open-input-bytes stored-annotations)))
(check-equal? (length (hash-ref stored-value 'annotations)) 1)
(check-equal? (hash-ref stored-value 'bookmarks) '())
(check-equal?
 (hash-ref (car (hash-ref stored-value 'annotations)) 'color)
 "yellow")
;; Stored on disk under the FNV-1a key, byte-compatible with v1.
(check-true
 (file-exists?
  (build-path temp-config "annotations"
              (string-append (fnv1a64-hex pdf-path) ".json"))))

;; Sidecar mode stores next to the PDF.
(call "set-annotations" pdf-path #t (string->bytes/utf-8 "{\"annotations\":[],\"bookmarks\":[{\"page\":3}]}"))
(check-true (file-exists? (string-append pdf-path ".booklight.json")))
(define-values (sidecar-annotations _e12) (call "get-annotations" pdf-path #t))
(check-equal?
 (length (hash-ref (read-json (open-input-bytes sidecar-annotations)) 'bookmarks))
 1)

;; Invalid shape -> error frame.
(define bad-id (send-request "set-annotations" pdf-path #f (string->bytes/utf-8 "{\"annotations\":\"x\"}")))
(define-values (bad-response _e15) (read-terminal bad-id))
(check-equal? (frame-type bad-response) message:error)

;; ---- streaming: translate-text ----

(check-equal? (unbox server-mode) 'fast)
(define translate-id (send-request "translate-text" "你好世界" "zh"))
(define-values (translate-response translate-events) (read-terminal translate-id))
(check-equal? (frame-type translate-response) message:response)
(define stream-id (list-ref (decode-value (frame-payload translate-response)) 0))
(check-true (and (exact-integer? stream-id) (> stream-id 0)))
(define translate-stream (await-stream-done stream-id translate-events))
(define translate-chunks
  (filter (lambda (payload) (string=? (event-name payload) "stream-chunk"))
          translate-stream))
(check-equal?
 (for/list ([payload (in-list translate-chunks)])
   (list (list-ref (event-value payload) 1)
         (list-ref (event-value payload) 2)))
 (list (list "思考过程" #t) (list "你" #f) (list "好" #f)))
(check-true
 (for/or ([payload (in-list translate-stream)])
   (and (string=? (event-name payload) "stream-done")
        (= (list-ref (event-value payload) 0) stream-id))))

;; The POST hit the fake server with the v1-normalized path, bearer key,
;; and stream:true body.
(define chat-request
  (for/or ([request (in-list (unbox requests-log))])
    (and (string-prefix? (vector-ref request 0) "POST /v1/chat/completions")
         request)))
(check-true (vector? chat-request))
(check-true
 (for/or ([header (in-list (vector-ref chat-request 1))])
   (string-prefix? header "Authorization: Bearer sk-test")))
(check-true
 (string-contains? (bytes->string/utf-8 (vector-ref chat-request 2)) "\"stream\":true"))

;; ---- streaming: summarize-text uses the settings language ----

(define summarize-id (send-request "summarize-text" "页面内容" "page"))
(define-values (summarize-response summarize-events) (read-terminal summarize-id))
(define summarize-stream-id
  (list-ref (decode-value (frame-payload summarize-response)) 0))
(define summarize-stream
  (await-stream-done summarize-stream-id summarize-events))
(check-true
 (for/or ([payload (in-list summarize-stream)])
   (string=? (event-name payload) "stream-done")))
(define summarize-request
  (for/or ([request (in-list (unbox requests-log))])
    (and (string-prefix? (vector-ref request 0) "POST /v1/chat/completions")
         (not (eq? request chat-request))
         request)))
(check-true
 (string-contains? (bytes->string/utf-8 (vector-ref summarize-request 2))
                   "总结用户提交的 PDF 页面"))

;; ---- streaming: chat keeps history shape and context header ----

(define chat-rpc-id
  (send-request "chat" "第 1 页内容" "第 1 页"
                (list (list "user" "之前的问题") (list "assistant" "之前的回答"))
                "新问题"))
(define-values (chat-response chat-events) (read-terminal chat-rpc-id))
(define chat-stream-id (list-ref (decode-value (frame-payload chat-response)) 0))
(define chat-stream (await-stream-done chat-stream-id chat-events))
(check-true
 (for/or ([payload (in-list chat-stream)])
   (string=? (event-name payload) "stream-done")))
(define doc-chat-request
  (for/or ([request (in-list (unbox requests-log))])
    (and (string-prefix? (vector-ref request 0) "POST /v1/chat/completions")
         (string-contains? (bytes->string/utf-8 (vector-ref request 2))
                           "文档内容")
         request)))
(define doc-chat-body (bytes->string/utf-8 (vector-ref doc-chat-request 2)))
(check-true (string-contains? doc-chat-body "你是 PDF 阅读助手"))
(check-true (string-contains? doc-chat-body "=== 文档内容（第 1 页）==="))
(check-true (string-contains? doc-chat-body "之前的问题"))
(check-true (string-contains? doc-chat-body "新问题"))

;; ---- streaming: stop-stream cancels an in-flight stream ----

(set-box! server-mode 'slow)
(define slow-id (send-request "translate-text" "长文档" "zh"))
(define-values (slow-response slow-events) (read-terminal slow-id))
(define slow-stream-id (list-ref (decode-value (frame-payload slow-response)) 0))
;; Wait for the first chunk so the HTTP stream is definitely in flight.
(define saw-first-chunk?
  (let scan ([seen slow-events] [frames 0])
    (cond
      [(for/or ([payload (in-list seen)])
         (and (string=? (event-name payload) "stream-chunk")
              (= (list-ref (event-value payload) 0) slow-stream-id)))
       #t]
      [(> frames 50) #f]
      [else
       (define f (read-frame client-in))
       (scan (if (= (frame-type f) message:event)
                 (cons (decode-value (frame-payload f)) seen)
                 seen)
             (add1 frames))])))
(check-true saw-first-chunk?)
(define stop-id (send-request "stop-stream" slow-stream-id))
(define-values (stop-response stop-events) (read-terminal stop-id))
(check-equal? (frame-type stop-response) message:response)
(check-true (void? (decode-value (frame-payload stop-response))))
(define slow-final (await-stream-done slow-stream-id stop-events))
;; stop-stream owns the terminal: stream-done, never stream-error.
(check-false
 (for/or ([payload (in-list slow-final)])
   (string=? (event-name payload) "stream-error")))
(check-true
 (for/or ([payload (in-list slow-final)])
   (and (string=? (event-name payload) "stream-done")
        (= (list-ref (event-value payload) 0) slow-stream-id))))

;; stop-stream on a finished/unknown id is a silent no-op.
(define-values (_noop _e13) (call "stop-stream" 999999))

;; ---- teardown ----

(semaphore-post test-done)
(write-frame (frame message:shutdown 0 #"") client-out)
(thread-wait server-thread)
(kill-thread acceptor)
(sleep 0.3)
(tcp-close listener)
(delete-directory/files temp-config)
