#lang racket/base
;; Real-window evidence for the glaze carrier (#75 S1+S2): open the host, run
;; the S1 probe journey (probe.html?e2e=1) then the S2 product journey
;; (index.html?e2e=1), poll the typed report route until each terminates,
;; capture PNGs, assert verdicts.
;; Run: racket host/verify.rkt  (graphical session; Xvfb on Linux CI)
(require glaze/app
         glaze/window
         json
         net/http-client
         net/url
         racket/file
         racket/port
         racket/runtime-path
         racket/string
         (only-in rivet/bridge stop-bridge!)
         "main.rkt")

(define-runtime-path evidence-dir "../.glaze/verify")

;; Surface glaze hatch/readback log lines in headless CI evidence.
(void
 (thread
  (lambda ()
    (define rcv (make-log-receiver (current-logger) 'info))
    (let loop ()
      (define v (sync rcv))
      (eprintf "[glaze-log] ~a: ~a\n" (vector-ref v 0) (vector-ref v 1))
      (loop)))))

(define verdicts '())
(define report-box (box #f))
(define s2-report-box (box #f))
(define s3-report-box (box #f))
(define s4-report-box (box #f))
(define s5-report-box (box #f))
(define drop-report-box (box #f))

(define (api-post url path body)
  (define u (string->url url))
  (define-values (status headers port)
    (http-sendrecv (url-host u) path
                   #:port (url-port u)
                   #:method #"POST"
                   #:headers (list #"Content-Type: application/json")
                   #:data (string->bytes/utf-8 (jsexpr->string body))))
  (define text (port->string port))
  (close-input-port port)
  (with-handlers ([exn:fail? (lambda (_) #f)])
    (string->jsexpr text)))

(define (save-shot! shot name)
  (cond
    [(and shot (file-exists? shot))
     (make-directory* evidence-dir)
     (define dest (build-path evidence-dir name))
     (copy-file shot dest #:exists-ok? #t)
     dest]
    [else #f]))

(putenv "BOOKLIGHT_E2E" "1")
(start-engine!)

(define (await-report url deadline-ms)
  (define deadline (+ (current-inexact-milliseconds) deadline-ms))
  (let loop ()
    (define r (api-post url "/api/host/e2e_report_get" (hasheq)))
    (cond
      [(and (hash? r) (equal? (hash-ref r 'phase #f) "done")) r]
      [(>= (current-inexact-milliseconds) deadline) (or r (hasheq 'phase "timeout"))]
      [else (sleep 0.5) (loop)])))

(define-values (_kind stop)
  (run-app
   #:public-dir (assemble-public-dir)
   #:api api
   #:transport 'http
   #:api-token #f
   #:title "Booklight carrier probe"
   #:width 1100
   #:height 800
   #:on-ready
   (lambda (wv url)
     (define deadline (+ (current-inexact-milliseconds) 45000))
     (define loaded?
       (let loop ()
         (cond
           [(and (string? (webview-title wv))
                 (string-prefix? (webview-title wv) "Booklight")) #t]
           [(>= (current-inexact-milliseconds) deadline) #f]
           [else (sleep 0.1) (loop)])))
     (sleep 1.0)
     (define url-ok? (equal? (webview-url wv) url))
     (define boot-shot (save-shot! (webview-capture! wv) "booklight-host-boot.png"))
     ;; ---- S1 probe journey ----
     ;; A fragment-only navigate would not reload the document (the harness
     ;; gate is read at load); the query param forces a full load.
     (webview-navigate wv (string-append url "probe.html?e2e=1"))
     (define report (await-report url 180000))
     (set-box! report-box report)
     (define e2e-shot (save-shot! (webview-capture! wv) "booklight-host-e2e.png"))
     (define steps (hash-ref report 'steps (hasheq)))
     (define (step-ok? id)
       (define s (hash-ref steps (string->symbol id) #f))
       (and (hash? s) (eq? #t (hash-ref s 'ok #f))))
     (set! verdicts
           (list (cons 'page-loaded (or loaded? (and url-ok? boot-shot)))
                 (cons 'url url-ok?)
                 (cons 'boot-capture (and boot-shot (> (file-size boot-shot) 0)))
                 (cons 'report-done (equal? (hash-ref report 'phase #f) "done"))
                 (cons 'journey-ok (eq? #t (hash-ref report 'ok #f)))
                 (cons 'step-count (= 12 (hash-ref report 'stepCount 0)))
                 (cons 'initialize (step-ok? "initialize"))
                 (cons 'settings (step-ok? "get-settings"))
                 (cons 'presets (step-ok? "list-presets"))
                 (cons 'epub-open (step-ok? "epub-open"))
                 (cons 'epub-text (step-ok? "epub-chapter-text"))
                 (cons 'epub-html-bytes (step-ok? "epub-chapter-html"))
                 (cons 'paragraphs (step-ok? "split-paragraphs"))
                 (cons 'recents (step-ok? "update-recents"))
                 (cons 'pdf-rotate-bytes (step-ok? "pdf-rotate-bytes"))
                 (cons 'annotations (step-ok? "annotations-roundtrip"))
                 (cons 'ai-preview (step-ok? "ai-preview-counts"))
                 (cons 'error-channel (step-ok? "error-channel"))
                 (cons 'e2e-capture (and e2e-shot (> (file-size e2e-shot) 0)))))
     ;; ---- S2 product journey ----
     (api-post url "/api/host/e2e_reset" (hasheq))
     (webview-navigate wv (string-append url "index.html?e2e=1"))
     (define s2-report (await-report url 180000))
     (set-box! s2-report-box s2-report)
     (define s2-shot (save-shot! (webview-capture! wv) "booklight-host-s2.png"))
     (define s2-steps (hash-ref s2-report 'steps (hasheq)))
     (define (s2-ok? id)
       (define s (hash-ref s2-steps (string->symbol id) #f))
       (and (hash? s) (eq? #t (hash-ref s 'ok #f))))
     (set! verdicts
           (append verdicts
                   (list (cons 's2-report-done (equal? (hash-ref s2-report 'phase #f) "done"))
                         (cons 's2-journey-ok (eq? #t (hash-ref s2-report 'ok #f)))
                         (cons 's2-step-count (= 29 (hash-ref s2-report 'stepCount 0)))
                         (cons 's2-i18n (s2-ok? "i18n"))
                         (cons 's2-library (s2-ok? "library"))
                         (cons 's2-open-epub (s2-ok? "open-epub"))
                         (cons 's2-toc (s2-ok? "toc"))
                         (cons 's2-chapter-html (s2-ok? "chapter-html"))
                         (cons 's2-chapter-next (s2-ok? "chapter-next"))
                         (cons 's2-notes-tab (s2-ok? "notes-tab"))
                         (cons 's2-read-theme (s2-ok? "read-theme"))
                         (cons 's2-font-scale (s2-ok? "font-scale"))
                         (cons 's2-open-pdf (s2-ok? "open-pdf"))
                         (cons 's2-pdf-canvas (s2-ok? "pdf-canvas"))
                         (cons 's2-tabs (s2-ok? "tabs"))
                         (cons 's2-tab-close (s2-ok? "tab-close"))
                         (cons 's3-sel-highlight (s2-ok? "sel-highlight"))
                         (cons 's3-sel-underline (s2-ok? "sel-underline"))
                         (cons 's3-ann-persist (s2-ok? "ann-persist"))
                         (cons 's3-edit-rotate (s2-ok? "edit-rotate"))
                         (cons 's3-edit-undo (s2-ok? "edit-undo"))
                         (cons 's3-edit-delete (s2-ok? "edit-delete"))
                         (cons 's3-edit-save (s2-ok? "edit-save"))
                         (cons 's3-edit-discard (s2-ok? "edit-discard"))
                         (cons 's4-settings-open (s2-ok? "s4-settings-open"))
                         (cons 's4-settings-save (s2-ok? "s4-settings-save"))
                         (cons 's4-settings-theme (s2-ok? "s4-settings-theme"))
                         (cons 's4-research-open (s2-ok? "s4-research-open"))
                         (cons 's4-research-search (s2-ok? "s4-research-search"))
                         (cons 's4-research-pin (s2-ok? "s4-research-pin"))
                         (cons 's4-ai-open (s2-ok? "s4-ai-open"))
                         (cons 's4-ai-preview (s2-ok? "s4-ai-preview"))
                         (cons 's2-capture (and s2-shot (> (file-size s2-shot) 0))))))
     ;; ---- S3 pixel-evidence stage (PDF ink + pending bar on screen) ----
     (api-post url "/api/host/e2e_reset" (hasheq))
     (webview-navigate wv (string-append url "index.html?e2e=1&stage=pdf"))
     (define s3-report (await-report url 120000))
     (set-box! s3-report-box s3-report)
     (sleep 1.5)
     (define s3-shot (save-shot! (webview-capture! wv) "booklight-host-s3.png"))
     (define s3-steps (hash-ref s3-report 'steps (hasheq)))
     (define (s3-ok? id)
       (define s (hash-ref s3-steps (string->symbol id) #f))
       (and (hash? s) (eq? #t (hash-ref s 'ok #f))))
     (set! verdicts
           (append verdicts
                   (list (cons 's3-stage-ok (eq? #t (hash-ref s3-report 'ok #f)))
                         (cons 's3-stage-open-pdf (s3-ok? "open-pdf"))
                         (cons 's3-stage-ink (s3-ok? "ink"))
                         (cons 's3-stage-pending (s3-ok? "pending"))
                         (cons 's3-capture (and s3-shot (> (file-size s3-shot) 0))))))
     ;; ---- S4 pixel-evidence stage (research results + citation deck) ----
     (api-post url "/api/host/e2e_reset" (hasheq))
     (webview-navigate wv (string-append url "index.html?e2e=1&stage=s4"))
     (define s4-report (await-report url 120000))
     (set-box! s4-report-box s4-report)
     (sleep 1.5)
     (define s4-shot (save-shot! (webview-capture! wv) "booklight-host-s4.png"))
     (define s4-steps (hash-ref s4-report 'steps (hasheq)))
     (define (s4-ok? id)
       (define s (hash-ref s4-steps (string->symbol id) #f))
       (and (hash? s) (eq? #t (hash-ref s 'ok #f))))
     (define s4-state (hash-ref s4-report 'state (hasheq)))
     (set! verdicts
           (append verdicts
                   (list (cons 's4-stage-ok (eq? #t (hash-ref s4-report 'ok #f)))
                         (cons 's4-stage-open-epub (s4-ok? "open-epub"))
                         (cons 's4-stage-open-pdf (s4-ok? "open-pdf"))
                         (cons 's4-stage-research (s4-ok? "research"))
                         (cons 's4-stage-cards (positive? (hash-ref s4-state 'cards 0)))
                         (cons 's4-stage-deck (positive? (hash-ref s4-state 'deck 0)))
                         (cons 's4-capture (and s4-shot (> (file-size s4-shot) 0))))))
     ;; ---- S5 reading-quality pixel evidence (typography + theme tokens) ----
     (api-post url "/api/host/e2e_reset" (hasheq))
     (webview-navigate wv (string-append url "index.html?e2e=1&stage=s5"))
     (define s5-report (await-report url 120000))
     (set-box! s5-report-box s5-report)
     (sleep 1.5)
     (define s5-shot (save-shot! (webview-capture! wv) "booklight-host-s5.png"))
     (define s5-steps (hash-ref s5-report 'steps (hasheq)))
     (define (s5-ok? id)
       (define s (hash-ref s5-steps (string->symbol id) #f))
       (and (hash? s) (eq? #t (hash-ref s 'ok #f))))
     (set! verdicts
           (append verdicts
                   (list (cons 's5-stage-ok (eq? #t (hash-ref s5-report 'ok #f)))
                         (cons 's5-ink-token (s5-ok? "ink-token"))
                         (cons 's5-underline-accent (s5-ok? "underline-accent"))
                         (cons 's5-sel-precision (s5-ok? "sel-precision"))
                         (cons 's5-measure (s5-ok? "measure"))
                         (cons 's5-serif (s5-ok? "serif"))
                         (cons 's5-citeblock (s5-ok? "citeblock"))
                         (cons 's5-pagination (s5-ok? "pagination"))
                         (cons 's5-scroll-feel (s5-ok? "scroll-feel"))
                         (cons 's5-theme-paper (s5-ok? "theme-paper"))
                         (cons 's5-theme-night (s5-ok? "theme-night"))
                         (cons 's5-font-scale (s5-ok? "font-scale"))
                         (cons 's5-theme-sepia (s5-ok? "theme-sepia"))
                         (cons 's5-capture (and s5-shot (> (file-size s5-shot) 0))))))
     ;; ---- drop-mask pixel evidence (mask over library on screen) ----
     (api-post url "/api/host/e2e_reset" (hasheq))
     (webview-navigate wv (string-append url "index.html?e2e=1&stage=drop"))
     (define drop-report (await-report url 120000))
     (set-box! drop-report-box drop-report)
     (sleep 1.5)
     (define drop-shot (save-shot! (webview-capture! wv) "booklight-host-drop.png"))
     (define drop-steps (hash-ref drop-report 'steps (hasheq)))
     (define (drop-ok? id)
       (define s (hash-ref drop-steps (string->symbol id) #f))
       (and (hash? s) (eq? #t (hash-ref s 'ok #f))))
     (define drop-state (hash-ref drop-report 'state (hasheq)))
     (set! verdicts
           (append verdicts
                   (list (cons 'drop-stage-ok (eq? #t (hash-ref drop-report 'ok #f)))
                         (cons 'drop-mask-show (drop-ok? "mask-show"))
                         (cons 'drop-open (drop-ok? "drop-open"))
                         (cons 'drop-mask-on-screen (eq? #f (hash-ref drop-state 'maskHidden #t)))
                         (cons 'drop-capture (and drop-shot (> (file-size drop-shot) 0))))))
     (webview-close wv))))

(stop)
(stop-bridge!)
(make-directory* evidence-dir)
(call-with-output-file (build-path evidence-dir "booklight-host-report.json")
  (lambda (out)
    (write-json (hasheq 's1 (or (unbox report-box) (hasheq))
                        's2 (or (unbox s2-report-box) (hasheq))
                        's3 (or (unbox s3-report-box) (hasheq))
                        's4 (or (unbox s4-report-box) (hasheq))
                        's5 (or (unbox s5-report-box) (hasheq))
                        'drop (or (unbox drop-report-box) (hasheq)))
                out)
    (newline out))
  #:exists 'truncate/replace)
(for ([v (in-list verdicts)])
  (printf "[verify] ~a = ~a\n" (car v) (cdr v)))
(printf "[verify] s1 report: ~a\n" (jsexpr->string (or (unbox report-box) (hasheq))))
(printf "[verify] s2 report: ~a\n" (jsexpr->string (or (unbox s2-report-box) (hasheq))))
(printf "[verify] s3 report: ~a\n" (jsexpr->string (or (unbox s3-report-box) (hasheq))))
(printf "[verify] s4 report: ~a\n" (jsexpr->string (or (unbox s4-report-box) (hasheq))))
(printf "[verify] s5 report: ~a\n" (jsexpr->string (or (unbox s5-report-box) (hasheq))))
(printf "[verify] drop report: ~a\n" (jsexpr->string (or (unbox drop-report-box) (hasheq))))
(unless (and (pair? verdicts) (andmap cdr verdicts))
  (eprintf "[verify] glaze carrier verification failed\n")
  (exit 1))
(displayln "[verify] ALL PASS")