#lang racket/base
;; Real-window evidence for the glaze carrier (#75 S1): open the host, wait
;; for the probe page, flip the in-page harness gate (?e2e=1), poll the typed
;; report route until the journey terminates, capture PNGs, assert verdicts.
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

(define-values (_kind stop)
  (run-app
   #:public-dir probe-dir
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
     ;; A fragment-only navigate would not reload the document (the harness
     ;; gate is read at load); the query param forces a full load.
     (webview-navigate wv (string-append url
                                       (if (string-contains? url "?") "&" "?")
                                       "e2e=1"))
     (define report-deadline (+ (current-inexact-milliseconds) 180000))
     (define report
       (let loop ()
         (define r (api-post url "/api/host/e2e_report_get" (hasheq)))
         (cond
           [(and (hash? r) (equal? (hash-ref r 'phase #f) "done")) r]
           [(>= (current-inexact-milliseconds) report-deadline)
            (or r (hasheq 'phase "timeout"))]
           [else (sleep 0.5) (loop)])))
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
     (webview-close wv))))

(stop)
(stop-bridge!)
(make-directory* evidence-dir)
(call-with-output-file (build-path evidence-dir "booklight-host-report.json")
  (lambda (out) (write-json (or (unbox report-box) (hasheq)) out) (newline out))
  #:exists 'truncate/replace)
(for ([v (in-list verdicts)])
  (printf "[verify] ~a = ~a\n" (car v) (cdr v)))
(printf "[verify] report: ~a\n" (jsexpr->string (or (unbox report-box) (hasheq))))
(unless (and (pair? verdicts) (andmap cdr verdicts))
  (eprintf "[verify] glaze carrier verification failed\n")
  (exit 1))
(displayln "[verify] ALL PASS")