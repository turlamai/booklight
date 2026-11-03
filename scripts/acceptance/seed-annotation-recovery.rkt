#lang racket/base

;; Seed the packaged-host acceptance profile with a pending sidecar recovery
;; document. This is synthetic data: native UI suites use it to prove that a
;; reopen crosses the embedded bridge and exposes retry/export rather than
;; silently presenting the draft as saved.

(require racket/file
         racket/path
         "../../racket/booklight/annotations.rkt"
         "../../racket/booklight/settings.rkt")

(define arguments (current-command-line-arguments))
(unless (= (vector-length arguments) 2)
  (error 'seed-annotation-recovery
         "usage: seed-annotation-recovery.rkt CONFIG-DIR DOCUMENT"))

(define config-dir (path->complete-path (vector-ref arguments 0)))
(define document-path (path->string (path->complete-path (vector-ref arguments 1))))
(define recovery-file
  (parameterize ([current-config-dir config-dir])
    (annotation-recovery-path document-path #t)))

(make-directory* (path-only recovery-file))
(call-with-output-file recovery-file
  #:exists 'truncate/replace
  (lambda (output)
    (display
     (string-append
      "{\"annotations\":[{\"id\":\"acceptance-recovery\","
      "\"page\":1,\"rects\":[],\"excerpt\":\"Recovered fixture note\","
      "\"color\":\"yellow\",\"kind\":\"highlight\",\"note\":\"\","
      "\"created\":1,\"anchor\":{\"chapter\":1,\"start\":0,\"end\":9}}],"
      "\"bookmarks\":[]}")
     output)))

(displayln recovery-file)
