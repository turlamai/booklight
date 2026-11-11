#lang racket/base

;; Qualification driver for the published-upgrade workflow
;; (.github/workflows/appimage-upgrade-qualification.yml): drives the
;; shipped updater entry points against the real feed. Two modes:
;;
;; upgrade (default) — perform-check! against the live signed feed
;;   (releases/latest/download/update-manifest.json, the family-canonical
;;   feed name), then start-download! and wait for the terminal phase. Emits
;;   one JSON object with the offered version, the downloaded path and its
;;   SHA-256. Booklight's throttle contract deviates from the family
;;   baseline (24h, booklight#docs): the interval lives in the
;;   check-updates RPC (app/backend.rkt auto-check-interval-seconds) and is
;;   never consulted by these entry points, so driving them directly
;;   bypasses it without touching the shipped code.
;;
;; bad-signature (TAMPER_BYTES_PATH set) — the workflow hands us the real
;;   published feed bytes with one payload byte flipped (PRISTINE_BYTES_PATH
;;   carries the untampered bytes as the positive control). Verification is
;;   the first gate fetch-update-manifest applies, before selection or
;;   download; the pristine bytes must verify, the tampered bytes must be
;;   refused, and the config directory's updates/ must stay empty — a
;;   refused manifest can never reach disk as an artifact.
;;
;; The config directory comes from the environment (XDG_CONFIG_HOME), which
;; the workflow fixes for both this driver and every packaged boot, so the
;; driver mutates exactly the state the real installs see.
;;
;; Inputs (environment):
;;   EXPECTED_VERSION    N+1 version string (upgrade mode)
;;   PRISTINE_BYTES_PATH real feed bytes (bad-signature mode, positive control)
;;   TAMPER_BYTES_PATH   tampered feed bytes (bad-signature mode)
;;   OUTPUT_JSON         where to write the result object
;;
;; Prints or writes one JSON object and exits non-zero on any mismatch.

(require crypto
         json
         net/base64
         racket/file
         racket/string
         (only-in rivet/distribution
                  base64-string->bytes
                  sha256-file/hex
                  verify-signed-manifest)
         "../../app/updater.rkt"
         "../../app/version.rkt"
         "../../racket/booklight/settings.rkt")

(define (need name)
  (or (getenv name)
      (error 'upgrade-qualification "~a is not set" name)))

(define (fail fmt . args)
  (apply error 'upgrade-qualification fmt args))

(define output-json (getenv "OUTPUT_JSON"))
(define pristine-path (getenv "PRISTINE_BYTES_PATH"))
(define tamper-path (getenv "TAMPER_BYTES_PATH"))

(define embedded-public-key
  (datum->pk-key (base64-string->bytes update-public-key-b64)
                 'SubjectPublicKeyInfo))

(define updates-dir (build-path (config-dir-path) "updates"))
(define (landed-artifacts)
  (if (directory-exists? updates-dir)
      (directory-list updates-dir)
      '()))

(define result
  (cond
    ;; ---------- bad-signature mode ----------
    [tamper-path
     ;; positive control: the pristine published bytes must verify, so the
     ;; refusal below is attributable to the flipped byte alone
     (define pristine-ok
       (with-handlers ([exn:fail? (lambda (_) #f)])
         (verify-signed-manifest (open-input-bytes (file->bytes pristine-path))
                                 embedded-public-key
                                 #:key-id update-key-id)
         #t))
     (unless pristine-ok
       (fail "the pristine live feed bytes did not verify — the environment, not the tamper, is broken"))
     (define tampered-accepted?
       (with-handlers ([exn:fail? (lambda (_) #f)])
         (verify-signed-manifest (open-input-bytes (file->bytes tamper-path))
                                 embedded-public-key
                                 #:key-id update-key-id)
         #t))
     (when tampered-accepted?
       (fail "one-byte-tampered live feed bytes were ACCEPTED — signature enforcement is broken"))
     (define left (landed-artifacts))
     (unless (null? left)
       (fail "a refused manifest must not reach download; updates/ holds: ~a" left))
     (hasheq 'mode "bad-signature"
             'pristineVerified #t
             'tamperedRefused #t
             'updatesDirEntries '())]
    ;; ---------- upgrade mode ----------
    [else
     (define expected-version (need "EXPECTED_VERSION"))
     (define checked (perform-check!))
     (unless (equal? (hash-ref checked 'status #f) "available")
       (fail "feed did not offer an update: ~a" checked))
     (unless (equal? (hash-ref checked 'availableVersion #f) expected-version)
       (fail "expected version ~a, feed offered ~a"
             expected-version (hash-ref checked 'availableVersion #f)))
     (start-download!)
     (define snapshot
       (let loop ([n 0])
         (define s (update-state-snapshot))
         (define phase (hash-ref s 'phase #f))
         (cond
           [(member phase '("downloaded" "error")) s]
           [(> n 600) s]
           [else (sleep 0.2) (loop (add1 n))])))
     (define phase (hash-ref snapshot 'phase #f))
     (unless (equal? phase "downloaded")
       (fail "download ended in phase ~a (message: ~a)"
             phase (hash-ref snapshot 'message #f)))
     (define downloaded-path (hash-ref snapshot 'downloadedPath #f))
     (unless (string? downloaded-path)
       (fail "downloadedPath is not a string: ~a" downloaded-path))
     (hasheq 'mode "upgrade"
             'status "downloaded"
             'availableVersion (hash-ref checked 'availableVersion #f)
             'installer (hash-ref checked 'installer #f)
             'sizeBytes (hash-ref checked 'sizeBytes #f)
             'feedUrl (manifest-url)
             'downloadedPath downloaded-path
             'downloadedSha256 (sha256-file/hex downloaded-path)
             'percent (hash-ref snapshot 'percent #f))]))

(if output-json
    (call-with-output-file output-json
      (lambda (out) (write-json result out) (newline out))
      #:exists 'truncate)
    (begin (write-json result) (newline)))
