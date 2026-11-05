#lang racket/base

;; OpenAI-compatible chat-completions client, ported from
;; the v1 (Tauri) llm.rs. Speaks HTTP/1.1 directly (racket/tcp + openssl) so
;; the domain core stays pure Racket with no Rivet dependency and no external
;; HTTP library. Parity notes:
;;  - POST {normalized}/chat/completions with stream:true; GET {normalized}/models
;;  - connect timeout 15 s (reqwest connect_timeout), list-models total 20 s
;;  - HTTP errors report status + first 400 chars of body + request URL;
;;    list-models HTTP errors report status only (same as list_models)
;;  - no retry, no cache
;;  - cancellation: all sockets are opened under (current-custodian); shutting
;;    that custodian down aborts in-flight reads and closes the connection

(require json
         net/url
         openssl
         racket/format
         racket/port
         racket/string
         racket/tcp
         "i18n.rkt"
         "sse.rkt"
         "urlnorm.rkt")

(provide chat-completions-url
         models-url
         stream-chat!
         list-models
         parse-sse-stream!
         max-sse-line-bytes
         default-connect-timeout-seconds
         list-models-timeout-seconds
         messages->jsexpr
         redact-secret)

;; ---- constants (parity with llm.rs) ----

(define default-connect-timeout-seconds 15)
(define list-models-timeout-seconds 20)
;; A single SSE line larger than 4 MiB is treated as a broken stream,
;; exactly like the leftover-buffer check in run_chat.
(define max-sse-line-bytes (* 4 1024 1024))
;; Divergence (documented): error/model bodies are capped at 1 MiB instead of
;; reading unbounded like reqwest; providers never send larger error bodies.
(define max-captured-body-bytes (* 1024 1024))

(define (raise-backend-error! message)
  (raise (exn:fail message (current-continuation-marks))))

;; ---- URL building ----

(define (chat-completions-url base-url)
  (string-append (normalize-base-url base-url) "/chat/completions"))

(define (models-url base-url)
  (string-append (normalize-base-url base-url) "/models"))

;; Messages are (cons role content) pairs in the core; the wire form is the
;; OpenAI JSON shape.
(define (messages->jsexpr messages)
  (for/list ([message (in-list messages)])
    (hasheq 'role (car message) 'content (cdr message))))

;; ---- connection setup ----

(struct http-connection (input output host-header custodian) #:transparent)

(define (parse-url! url-string)
  (with-handlers ([exn:fail?
                   (lambda (e)
                     (raise-backend-error!
                      (tf "backend.error.connect" (exn-message e))))])
    (define u (string->url url-string))
    (unless (member (url-scheme u) '("http" "https"))
      (raise-backend-error!
       (tf "backend.error.connect" (format "unsupported URL scheme: ~a" (url-scheme u)))))
    u))

(define (effective-port u)
  (or (url-port u)
      (if (string=? (url-scheme u) "https") 443 80)))

(define (host-header u)
  (define host (url-host u))
  (define port (effective-port u))
  (define standard? (if (string=? (url-scheme u) "https") (= port 443) (= port 80)))
  (if standard? host (format "~a:~a" host port)))

(define (request-path u)
  (define segments (map path/param-path (url-path u)))
  (string-append "/" (string-join segments "/")))

;; Opens the TCP/TLS connection under a dedicated custodian so a connect that
;; outlives the timeout can be torn down (killing the helper thread and any
;; half-open socket).
(define (connect-http! url-string timeout-seconds)
  (define u (parse-url! url-string))
  (define host (url-host u))
  (define port (effective-port u))
  (define result-box (box #f))
  (define done (make-semaphore))
  (define cust (make-custodian))
  (parameterize ([current-custodian cust])
    (thread
     (lambda ()
       (with-handlers ([exn:fail? (lambda (e) (set-box! result-box e))])
         (define-values (in out)
           (if (string=? (url-scheme u) "https")
               (ssl-connect host port)
               (tcp-connect host port)))
         (set-box! result-box (cons in out)))
       (semaphore-post done))))
  (cond
    [(not (sync/timeout timeout-seconds done))
     (custodian-shutdown-all cust)
     (raise-backend-error!
      (tf "backend.error.connect-timeout" (number->string timeout-seconds)))]
    [(exn:fail? (unbox result-box))
     (define cause (unbox result-box))
     (custodian-shutdown-all cust)
     (raise-backend-error! (tf "backend.error.connect" (exn-message cause)))]
    [else
     (define pair (unbox result-box))
     (http-connection (car pair) (cdr pair) (host-header u) cust)]))

(define (close-http-connection! conn)
  (custodian-shutdown-all (http-connection-custodian conn)))

;; ---- request/response plumbing ----

;; Diagnostics must never quote a credential back to the user or a log.
;; Provider error bodies occasionally echo the Authorization header.
(define (redact-secret text secret)
  (cond
    [(or (not (string? text)) (not (string? secret)) (string=? secret "")) text]
    [else
     (string-replace
      (string-replace text secret "[redacted]")
      (string-append "Bearer " secret)
      "Bearer [redacted]")]))

(define (send-request! conn method url-string api-key body)
  (define u (parse-url! url-string))
  (define out (http-connection-output conn))
  (fprintf out "~a ~a HTTP/1.1\r\n" method (request-path u))
  (fprintf out "Host: ~a\r\n" (http-connection-host-header conn))
  (fprintf out "Accept: text/event-stream\r\n")
  (unless (string=? api-key "")
    (fprintf out "Authorization: Bearer ~a\r\n" api-key))
  (fprintf out "Content-Type: application/json\r\n")
  (fprintf out "Content-Length: ~a\r\n" (bytes-length body))
  ;; Close-delimited responses keep non-chunked streaming simple.
  (fprintf out "Connection: close\r\n\r\n")
  (unless (zero? (bytes-length body))
    (display body out))
  (flush-output out))

(struct http-status (code reason) #:transparent)

(define (read-response-head! conn)
  (define in (http-connection-input conn))
  (define status-line (read-line in 'return-linefeed))
  (when (eof-object? status-line)
    (raise-backend-error! (tf "backend.error.read-response" "empty response")))
  (define status
    (cond
      [(regexp-match #px"^HTTP/\\S+\\s+(\\d+)\\s*(.*)$" status-line)
       => (lambda (m) (http-status (string->number (cadr m)) (caddr m)))]
      [else
       (raise-backend-error!
        (tf "backend.error.read-response"
            (truncate-chars (string-trim status-line) 100)))]))
  (define headers
    (let loop ([acc '()])
      (define line (read-line in 'return-linefeed))
      (cond
        [(or (eof-object? line) (string=? line "")) (reverse acc)]
        [else (loop (cons line acc))])))
  (values status headers))

(define (success-status? status)
  (define code (http-status-code status))
  (and (>= code 200) (<= code 299)))

;; Rust `response.status()` Display renders "404 Not Found"; keep that shape.
(define (status-display status)
  (format "~a ~a" (http-status-code status) (http-status-reason status)))

(define (header-value headers name)
  (for/or ([header (in-list headers)])
    (define match
      (regexp-match (regexp (format "(?i:^~a:[ \\t]*(.*)$)" name)) header))
    (and match (string-trim (cadr match)))))

(define (chunked-response? headers)
  (define encoding (header-value headers "transfer-encoding"))
  (and encoding (string-contains? (string-downcase encoding) "chunked")))

;; Reads at most `limit` bytes, then stops (close-delimited bodies end at EOF;
;; chunked bodies end at the terminating chunk).
(define (read-bytes-up-to in limit)
  (let loop ([acc '()] [total 0])
    (define want (min 65536 (- limit total)))
    (cond
      [(<= want 0) (apply bytes-append (reverse acc))]
      [else
       (define chunk (read-bytes want in))
       (cond
         [(or (eof-object? chunk) (zero? (bytes-length chunk)))
          (apply bytes-append (reverse acc))]
         [else (loop (cons chunk acc) (+ total (bytes-length chunk)))])])))

(define (read-response-body-string! conn headers)
  (define in (http-connection-input conn))
  (define source
    (if (chunked-response? headers)
        (open-chunked-input in)
        in))
  (bytes->string/utf-8 (read-bytes-up-to source max-captured-body-bytes) #\uFFFD))

;; ---- chunked transfer decoding ----

;; Wraps a raw socket port in a port that decodes HTTP/1.1 chunked framing.
(define (open-chunked-input raw-in)
  (define remaining 0)
  (define finished? #f)
  (define (fail! why)
    (set! finished? #t)
    (raise-backend-error! (tf "backend.error.read-response" why)))
  (define (read-chunk-header!)
    (define line (read-line raw-in 'return-linefeed))
    (cond
      [(eof-object? line) (set! finished? #t)]
      [(string=? line "") (read-chunk-header!)] ; tolerate stray blank lines
      [else
       (define size-text
         (string-trim (car (regexp-split #rx";" line))))
       (define size (string->number size-text 16))
       (cond
         [(not size) (fail! "malformed chunk size")]
         [(zero? size)
          ;; Consume trailer lines up to the blank terminator (or EOF).
          (let trailer-loop ()
            (define trailer (read-line raw-in 'return-linefeed))
            (unless (or (eof-object? trailer) (string=? trailer ""))
              (trailer-loop)))
          (set! finished? #t)]
         [else (set! remaining size)])]))
  (define (read-in dest)
    (cond
      [finished? eof]
      [(zero? remaining)
       (read-chunk-header!)
       (if finished?
           eof
           (read-in dest))]
      [else
       (define count (read-bytes! dest raw-in 0 (min remaining (bytes-length dest))))
       (cond
         [(eof-object? count)
          (set! finished? #t)
          eof]
         [else
          (set! remaining (- remaining count))
          count])]))  (make-input-port 'chunked-sse-body read-in #f #f))

;; ---- SSE pump ----

;; Reads SSE lines from an already-unframed port. Returns 'done when a
;; [DONE] sentinel was seen, 'end at EOF without one. `on-chunk` is called
;; as (on-chunk text is-reasoning) for every non-empty delta.
(define (parse-sse-stream! in on-chunk)
  (define buf (make-bytes 65536))
  (define acc (open-output-bytes))
  ;; Read cursor into `buf`, persisted across lines because one chunk
  ;; usually holds several of them: cursor = (cons have pos).
  (define cursor (box (cons 0 0)))
  (define (next-line total)
    (define have (car (unbox cursor)))
    (define pos (cdr (unbox cursor)))
    (cond
      [(= pos have)
       ;; Buffer drained: refill from the stream.
       (define count (read-bytes! buf in))
       (cond
         [(or (eof-object? count) (zero? count))
          (if (zero? total)
              eof
              (finish-capped-line! acc))]
         [else
          (set-box! cursor (cons count 0))
          (next-line total)])]
      [else
       (define newline-at
         (for/first ([i (in-range pos have)]
                     #:when (= (bytes-ref buf i) 10))
           i))
       (cond
         [newline-at
          (write-bytes buf acc pos newline-at)
          (set-box! cursor (cons have (add1 newline-at)))
          (finish-capped-line! acc)]
         [else
          (when (> (+ total (- have pos)) max-sse-line-bytes)
            (raise-backend-error! (tf "backend.error.sse-line-too-long")))
          (write-bytes buf acc pos have)
          (set-box! cursor (cons have have))
          (next-line (+ total (- have pos)))])]))
  (let dispatch ()
    (define line (next-line 0))
    (cond
      [(eof-object? line) 'end]
      [else
       (define action (sse-line-action line))
       (case (car action)
         [(done) 'done]
         [(skip) (dispatch)]
         [(data)
          (define delta (payload->delta (cadr action)))
          (case (car delta)
            [(delta)
             (define text (cadr delta))
             (unless (string=? text "")
               (on-chunk text (caddr delta)))
             (dispatch)]
            [else (dispatch)])])])))

(define (pump-sse! conn headers on-chunk)
  (define in (http-connection-input conn))
  (parse-sse-stream! (if (chunked-response? headers)
                         (open-chunked-input in)
                         in)
                     on-chunk))

;; Decodes the accumulated line bytes (CR tolerated, lossy UTF-8 like
;; Rust's from_utf8_lossy) and drains the accumulator for the next line.
(define (finish-capped-line! acc)
  (bytes->string/utf-8 (get-output-bytes acc #t) #\uFFFD))

;; ---- public entry points ----

;; Streams one chat completion. messages: list of (cons role content).
;; on-chunk: (lambda (text is-reasoning)). Returns 'done (a [DONE] sentinel
;; arrived) or 'end (stream ended without one) — both mean the request
;; finished. Raises exn:fail with a user-presentable message on failure.
;; No retry, no cache (parity with llm.rs).
(define (stream-chat! #:base-url base-url
                      #:model model
                      #:messages messages
                      #:api-key [api-key ""]
                      #:on-chunk on-chunk)
  (unless (and (procedure? on-chunk) (procedure-arity-includes? on-chunk 2))
    (raise-argument-error 'stream-chat! "(procedure/c 2)" on-chunk))
  (define url-string (chat-completions-url base-url))
  (define body
    (jsexpr->bytes
     (hasheq 'model model
             'messages (messages->jsexpr messages)
             'stream #t)))
  (define conn (connect-http! url-string default-connect-timeout-seconds))
  (dynamic-wind
    void
    (lambda ()
      (send-request! conn "POST" url-string api-key body)
      (define-values (status headers) (read-response-head! conn))
      (unless (success-status? status)
        (define error-body (read-response-body-string! conn headers))
        (raise-backend-error!
         (tf "backend.error.http-status-body"
             (status-display status)
             (redact-secret (truncate-chars (string-trim error-body) 400) api-key)
             url-string)))
      (pump-sse! conn headers on-chunk))
    (lambda ()
      (close-http-connection! conn))))

;; Lists model ids from GET {normalized}/models. Raises exn:fail with a
;; user-presentable message on failure (parity with list_models in llm.rs).
(define (list-models base-url api-key)
  (define url-string (models-url base-url))
  (call-with-total-timeout
   list-models-timeout-seconds
   (lambda ()
     (define conn (connect-http! url-string default-connect-timeout-seconds))
     (dynamic-wind
       void
       (lambda ()
         (send-request! conn "GET" url-string api-key #"")
         (define-values (status headers) (read-response-head! conn))
         (unless (success-status? status)
           (raise-backend-error! (tf "backend.error.http-status" (status-display status))))
         (define body-string (read-response-body-string! conn headers))
         (with-handlers
             ([exn:fail?
               (lambda (e)
                 (raise-backend-error!
                  (tf "backend.error.parse-response" (exn-message e))))])
           (define value (read-json (open-input-string body-string)))
           (define data (and (hash? value) (hash-ref value 'data #f)))
           (if (list? data)
               (for/list ([item (in-list data)]
                          #:when (and (hash? item) (string? (hash-ref item 'id #f))))
                 (hash-ref item 'id))
               '())))
       (lambda ()
         (close-http-connection! conn))))))

;; Runs `thunk` on a fresh thread under a dedicated custodian. On timeout the
;; custodian is shut down (killing the thread and every socket it owns) and
;; an exn:fail carrying the timeout message is raised.
(define (call-with-total-timeout timeout-seconds thunk)
  (define result-box (box #f))
  (define done (make-semaphore))
  (define cust (make-custodian))
  (parameterize ([current-custodian cust])
    (thread
     (lambda ()
       (with-handlers ([exn:fail? (lambda (e) (set-box! result-box e))])
         (set-box! result-box (thunk)))
       (semaphore-post done))))
  (cond
    [(not (sync/timeout timeout-seconds done))
     (custodian-shutdown-all cust)
     (raise-backend-error!
      (tf "backend.error.connect-timeout" (number->string timeout-seconds)))]
    [else
     (define result (unbox result-box))
     (custodian-shutdown-all cust)
     (if (exn:fail? result) (raise result) result)]))
