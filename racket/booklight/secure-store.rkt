#lang racket/base

;; Secret storage for Booklight's BYOK provider key.
;;
;; The domain core owns settings, but a provider key must not sit in
;; settings.json in clear text. This module is the narrow seam:
;;
;;   * macOS  -> generic-password item in the login Keychain (`security`)
;;   * Linux  -> Secret Service item through `secret-tool` (libsecret)
;;   * Windows-> DPAPI CurrentUser blob in the profile config directory
;;   * fallback -> owner-only file (0600) when the desktop store is missing
;;
;; The fallback exists so a headless CI runner or a locked keychain never
;; makes the reader unusable; it is reported by `secure-store-backend-name`
;; so the UI/docs can say where the key actually lives. Tests parameterize
;; `current-secure-store` with an in-memory store.

(require racket/file
         racket/format
         racket/list
         racket/match
         racket/path
         racket/port
         racket/string
         racket/system)

(provide (struct-out secure-store)
         current-secure-store
         make-memory-secure-store
         make-file-secure-store
         make-platform-secure-store
         secure-store-current
         secure-store-backend-name
         secure-store-available?
         secure-store-read
         secure-store-write
         secure-store-delete)

(struct secure-store (id read-fn write-fn delete-fn available-fn) #:transparent)

(define (secure-store-read store service account)
  ((secure-store-read-fn store) service account))

(define (secure-store-write store service account secret)
  (unless (bytes? secret)
    (raise-argument-error 'secure-store-write "bytes?" secret))
  ((secure-store-write-fn store) service account secret))

(define (secure-store-delete store service account)
  ((secure-store-delete-fn store) service account))

(define (secure-store-available? store)
  ((secure-store-available-fn store)))

(define (secure-store-backend-name store)
  (secure-store-id store))

;; ---- in-memory backend (tests) ----

(define (make-memory-secure-store)
  (define table (make-hash))
  (secure-store
   'memory
   (lambda (service account)
     (hash-ref table (cons service account) #f))
   (lambda (service account secret)
     (hash-set! table (cons service account) secret))
   (lambda (service account)
     (hash-remove! table (cons service account)))
   (lambda () #t)))

;; ---- owner-only file backend ----

(define (chmod-private! path)
  (unless (eq? (system-type) 'windows)
    (with-handlers ([exn:fail? void])
      (file-or-directory-permissions path #o600))))

(define (make-file-secure-store path)
  (define (read _service _account)
    (and (file-exists? path) (file->bytes path)))
  (define (write _service _account secret)
    (make-parent-directory* (path-only path))
    (call-with-atomic-output-file
     path
     (lambda (out _temporary)
       (write-bytes secret out)))
    (chmod-private! path))
  (define (delete _service _account)
    (when (file-exists? path)
      (with-handlers ([exn:fail? void])
        (delete-file path))))
  (secure-store 'file read write delete (lambda () #t)))

;; ---- process helpers ----

;; Returns (values exit-code stdout stderr). Commands are short-lived and
;; their output is tiny, so reading both pipes before waiting is safe.
(define (run/capture executable arguments [stdin-bytes #f])
  (define-values (process stdout stdin stderr)
    (apply subprocess #f #f #f executable arguments))
  (when stdin-bytes (write-bytes stdin-bytes stdin))
  (close-output-port stdin)
  (define out (port->string stdout))
  (define err (port->string stderr))
  (close-input-port stdout)
  (close-input-port stderr)
  (subprocess-wait process)
  (values (subprocess-status process) out err))

(define (hex-encode bytes)
  (string-join
   (for/list ([byte (in-bytes bytes)])
     (~r byte #:base 16 #:min-width 2 #:pad-string "0"))
   ""))

;; ---- macOS Keychain ----

(define (macos-keychain-store)
  (define security (find-executable-path "security"))
  (define (read service account)
    (and security
         (let-values ([(status out _err)
                       (run/capture security
                                    (list "find-generic-password"
                                          "-s" service "-a" account "-w"))])
           (and (zero? status)
                (let ([text (string-trim out)])
                  (and (not (string=? text ""))
                       (string->bytes/utf-8 text)))))))
  (define (write service account secret)
    (unless security
      (raise (exn:fail "macOS Keychain tool is unavailable" (current-continuation-marks))))
    ;; `security` has no stdin form for a new password; -X keeps the secret out
    ;; of shell history and the file, though a same-user process can briefly
    ;; observe argv. The long-term fix belongs in the native host's
    ;; RivetSecureStorage call, not a second credential format here.
    (define-values (status _out err)
      (run/capture security
                   (list "add-generic-password" "-U"
                         "-s" service "-a" account
                         "-X" (hex-encode secret))))
    (unless (zero? status)
      (raise (exn:fail (string-trim err) (current-continuation-marks)))))
  (define (delete service account)
    (when security
      (void (run/capture security
                         (list "delete-generic-password"
                               "-s" service "-a" account)))))
  (secure-store 'keychain read write delete (lambda () (and security #t))))

;; ---- Linux Secret Service ----

(define (linux-secret-service-store)
  (define secret-tool (find-executable-path "secret-tool"))
  (define (read service account)
    (and secret-tool
         (let-values ([(status out _err)
                       (run/capture secret-tool
                                    (list "lookup" "service" service
                                          "account" account))])
           (and (zero? status)
                (> (string-length out) 0)
                (string->bytes/utf-8 out)))))
  (define (write service account secret)
    (unless secret-tool
      (raise (exn:fail "Secret Service tool is unavailable" (current-continuation-marks))))
    (define-values (status _out err)
      (run/capture secret-tool
                   (list "store" "--label=Booklight provider key"
                         "service" service "account" account)
                   secret))
    (unless (zero? status)
      (raise (exn:fail (string-trim err) (current-continuation-marks)))))
  (define (delete service account)
    (when secret-tool
      (void (run/capture secret-tool
                         (list "clear" "service" service
                               "account" account)))))
  (secure-store 'secret-service read write delete (lambda () (and secret-tool #t))))

;; ---- Windows DPAPI ----

(define (windows-powershell)
  (or (find-executable-path "powershell.exe")
      (find-executable-path "pwsh.exe")
      (find-executable-path "pwsh")))

(define dpapi-encrypt-script
  (string-append
   "Add-Type -AssemblyName System.Security;"
   "$s=[Console]::In.ReadToEnd();"
   "$b=[Text.Encoding]::UTF8.GetBytes($s);"
   "$p=[Security.Cryptography.ProtectedData]::Protect($b,$null,'CurrentUser');"
   "[Convert]::ToBase64String($p)"))

(define dpapi-decrypt-script
  (string-append
   "Add-Type -AssemblyName System.Security;"
   "$s=[Console]::In.ReadToEnd().Trim();"
   "$b=[Convert]::FromBase64String($s);"
   "$p=[Security.Cryptography.ProtectedData]::Unprotect($b,$null,'CurrentUser');"
   "[Text.Encoding]::UTF8.GetString($p)"))

(define (windows-dpapi-store path)
  (define powershell (windows-powershell))
  (define (read _service _account)
    (and powershell
         (file-exists? path)
         (let-values ([(status out _err)
                       (run/capture powershell
                                    (list "-NoProfile" "-NonInteractive"
                                          "-Command" dpapi-decrypt-script)
                                    (file->bytes path))])
           (and (zero? status)
                (> (string-length out) 0)
                (string->bytes/utf-8 out)))))
  (define (write _service _account secret)
    (unless powershell
      (raise (exn:fail "PowerShell is unavailable for DPAPI" (current-continuation-marks))))
    (define-values (status out err)
      (run/capture powershell
                   (list "-NoProfile" "-NonInteractive"
                         "-Command" dpapi-encrypt-script)
                   secret))
    (unless (zero? status)
      (raise (exn:fail (string-trim err) (current-continuation-marks))))
    (make-parent-directory* (path-only path))
    (call-with-atomic-output-file
     path
     (lambda (out-port _temporary)
       (write-bytes (string->bytes/utf-8 (string-trim out)) out-port))))
  (define (delete _service _account)
    (when (file-exists? path)
      (with-handlers ([exn:fail? void])
        (delete-file path))))
  (secure-store 'dpapi read write delete (lambda () (and powershell (file-exists? path)))))

;; ---- platform selection with a safe fallback ----

(define (make-platform-secure-store config-dir)
  (define fallback (make-file-secure-store (build-path config-dir "api-key")))
  (define primary
    (case (system-type)
      [(macosx) (macos-keychain-store)]
      [(unix) (linux-secret-service-store)]
      [(windows) (windows-dpapi-store (build-path config-dir "api-key.dpapi"))]
      [else #f]))
  (define (read service account)
    (define primary-value
      (and primary
           (with-handlers ([exn:fail? (lambda (_) #f)])
             (secure-store-read primary service account))))
    (or primary-value (secure-store-read fallback service account)))
  (define (write service account secret)
    ;; Prefer the OS store. A locked/headless keychain must not lose the key,
    ;; so the owner-only file keeps the reader usable as an explicit fallback.
    (define stored?
      (and primary
           (with-handlers ([exn:fail? (lambda (_) #f)])
             (secure-store-write primary service account secret)
             #t)))
    (unless stored?
      (secure-store-write fallback service account secret)))
  (define (delete service account)
    (when primary
      (with-handlers ([exn:fail? void])
        (secure-store-delete primary service account)))
    (secure-store-delete fallback service account))
  (define (available?)
    (or (and primary (secure-store-available? primary))
        (secure-store-available? fallback)))
  (secure-store
   (if (and primary (secure-store-available? primary))
       (secure-store-id primary)
       'file)
   read write delete available?))

;; ---- current store ----
;;
;; `#f` means "choose the platform store for the active config directory".
;; Tests and headless tools can parameterize a memory store. The same
;; memory table is reused for `BOOKLIGHT_SECURE_STORE=memory` so subprocess
;; backends (the RVT1 wire test) never touch a real keychain.

(define memory-default (make-memory-secure-store))
(define configured-stores (make-hash))

(define current-secure-store (make-parameter #f))

(define (secure-store-current config-dir)
  (or (current-secure-store)
      (cond
        [(equal? (getenv "BOOKLIGHT_SECURE_STORE") "memory") memory-default]
        [else
         (hash-ref! configured-stores (path->string config-dir)
                    (lambda () (make-platform-secure-store config-dir)))])))
