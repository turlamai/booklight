#lang racket/base

;; Secure-store seam: in-memory and owner-only file backends, secret deletion,
;; and the redaction helper that keeps provider errors out of support logs.

(require rackunit
         racket/file
         racket/path
         "../booklight/secure-store.rkt"
         "../booklight/llm.rkt")

(define service "site.jrtx.booklight")
(define account "api-key")

(test-case "memory secure store round-trips and removes"
  (define store (make-memory-secure-store))
  (check-true (secure-store-available? store))
  (check-false (secure-store-read store service account))
  (secure-store-write store service account #"sk-memory")
  (check-equal? (secure-store-read store service account) #"sk-memory")
  (secure-store-delete store service account)
  (check-false (secure-store-read store service account)))

(test-case "file secure store is owner-only on Unix"
  (define dir (make-temporary-file "booklight-secret-~a" 'directory))
  (dynamic-wind
   void
   (lambda ()
     (define path (build-path dir "api-key"))
     (define store (make-file-secure-store path))
     (secure-store-write store service account #"sk-file")
     (check-true (file-exists? path))
     (check-equal? (secure-store-read store service account) #"sk-file")
     (when (eq? (system-type) 'unix)
       (check-equal? (file-or-directory-permissions path 'bits) #o600))
     (secure-store-delete store service account)
     (check-false (file-exists? path)))
   (lambda () (delete-directory/files dir))))

(test-case "provider error redaction removes the credential"
  (check-equal? (redact-secret "bad key sk-live-123" "sk-live-123")
                "bad key [redacted]")
  (check-equal? (redact-secret "Authorization: Bearer sk-live-123" "sk-live-123")
                "Authorization: Bearer [redacted]")
  (check-equal? (redact-secret "no secret here" "") "no secret here")
  (check-equal? (redact-secret "no secret here" "sk-missing") "no secret here"))
