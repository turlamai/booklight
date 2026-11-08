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

;; On macOS the platform store shells out to security(1). Use a fake executable
;; on PATH to prove the password reaches it on stdin and never in argv, without
;; touching a real login keychain.
(when (eq? (system-type) 'macosx)
  (test-case "macOS Keychain password uses stdin, never argv"
    (define root (make-temporary-file "booklight-keychain-test-~a" 'directory))
    (define script (build-path root "security"))
    (define store-file (build-path root "item"))
    (define old-path (getenv "PATH"))
    (dynamic-wind
     void
     (lambda ()
       (call-with-output-file script
         (lambda (out)
           (display "#!/bin/sh\n" out)
           (display "cmd=\"$1\"\n" out)
           (display "case \"$cmd\" in\n" out)
           (display "  add-generic-password)\n" out)
           (display "    for arg in \"$@\"; do\n" out)
           (display "      case \"$arg\" in sk-*) exit 9;; esac\n" out)
           (display "    done\n" out)
           (display "    IFS= read -r one\n" out)
           (display "    IFS= read -r two\n" out)
           (display "    [ \"$one\" = \"$two\" ] || exit 3\n" out)
           (display "    printf '%s' \"$one\" > \"$BOOKLIGHT_FAKE_STORE\"\n" out)
           (display "    ;;\n" out)
           (display "  find-generic-password)\n" out)
           (display "    [ -f \"$BOOKLIGHT_FAKE_STORE\" ] || exit 44\n" out)
           (display "    cat \"$BOOKLIGHT_FAKE_STORE\"\n" out)
           (display "    ;;\n" out)
           (display "  delete-generic-password)\n" out)
           (display "    rm -f \"$BOOKLIGHT_FAKE_STORE\"\n" out)
           (display "    ;;\n" out)
           (display "  *) exit 2;;\n" out)
           (display "esac\n" out))
         #:exists 'truncate/replace)
       (file-or-directory-permissions script #o755)
       (putenv "PATH" (string-append (path->string root) ":" old-path))
       (putenv "BOOKLIGHT_FAKE_STORE" (path->string store-file))
       (define store (make-platform-secure-store root))
       (secure-store-write store "site.jrtx.booklight.test" "api-key"
                           #"sk-stdin-secret")
       (check-equal? (secure-store-read store "site.jrtx.booklight.test" "api-key")
                     #"sk-stdin-secret")
       (secure-store-delete store "site.jrtx.booklight.test" "api-key")
       (check-false (secure-store-read store "site.jrtx.booklight.test" "api-key")))
     (lambda ()
       (putenv "PATH" old-path)
       (delete-directory/files root)))))
