#lang racket/base
;; GitHub-hosted Windows Racket 9.3 sometimes fails while enumerating its
;; win32-store ROOT certificates (X509*->C receives #f). Use the CA bundle
;; bundled with the runner's Node.js Mozilla roots, without disabling TLS/hostname validation.
;; This must run in the SAME Racket process as the package downloads.
(require openssl
         pkg
         racket/file)

(define bundle (getenv "BOOKLIGHT_CI_CA_BUNDLE"))
(unless (and bundle
             (file-exists? bundle)
             (regexp-match? #rx#"-----BEGIN CERTIFICATE-----"
                            (file->bytes bundle)))
  (error 'ci-install-windows-deps
         "trusted Mozilla PEM CA bundle is missing or invalid"))

(ssl-default-verify-sources (list bundle))

(define keel-pin "d56e38f6ad89a5e85c455ac1b26acd2dfd312165")
(define rivet-pin (or (getenv "RIVET_PIN") "v0.12.0"))
(define keel-url (format "https://github.com/turlamai/keel.git#~a" keel-pin))
(define rivet-url (format "https://github.com/turlamai/rivet.git#~a" rivet-pin))
(define include-crypto?
  (for/or ([arg (in-vector (current-command-line-arguments))])
    (equal? arg "--crypto")))

;; Exact equivalent of the previous two 'raco pkg install --auto --no-docs'
;; calls, except the Racket TLS trust source is explicitly a verified CA file.
;; The patched Keel must be installed first while the package catalog lags.
(pkg-install-command #:auto #t #:no-docs #t keel-url)
(if include-crypto?
    (pkg-install-command #:auto #t #:no-docs #t rivet-url "crypto")
    (pkg-install-command #:auto #t #:no-docs #t rivet-url))
