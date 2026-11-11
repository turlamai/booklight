#lang racket/base

;; Updater tests (family pattern, ported from taskly/brainfuel): platform
;; mapping, manifest URL construction, the sticky rollout bucket (drawn
;; once from crypto-random-bytes and persisted in updater.json), the .partial
;; cleanup rivet's download-update performs on failed downloads, and the
;; offline trust chain — craft a manifest, sign it with a throwaway Ed25519
;; key, verify and select it the same way the live checker does, then drive
;; the real download path against a local TCP server.
;; (fetch-update-manifest only accepts HTTPS, so the network half of the
;; check is exercised in production; everything else runs offline here.)

(require crypto
         crypto/all
         json
         net/base64
         rackunit
         ;; Explicit only-in (see app/updater.rkt): rivet/distribution gained
         ;; platform-symbol / architecture-symbol / installer-extension
         ;; re-exports after the v0.11.0 pin — except-in of names an older
         ;; rivet lacks fails to expand, and this file imports those three
         ;; from ../../app/updater.rkt instead.
         (only-in rivet/distribution
                  base64-string->bytes
                  download-update
                  fetch-update-manifest
                  select-update
                  sha256-file/hex
                  update-artifact
                  update-artifact-size
                  update-candidate
                  update-candidate-artifact
                  update-candidate?
                  update-manifest
                  update-manifest-version
                  updater-config
                  verify-signed-manifest
                  write-signed-manifest)
         racket/file
         racket/path
         racket/tcp
         "../../app/updater.rkt"
         "../../app/version.rkt"
         "../booklight/settings.rkt")

(use-all-factories!)

;; rivet moved update-artifact to 8 fields (arguments + component) in the
;; middle of the v0.11.0 line; construct through this shim so the suite
;; runs on either shape.
(define (make-test-artifact platform architecture url sha256 size installer arguments)
  (with-handlers
      ([exn:fail:contract:arity?
        (lambda (_)
          (apply update-artifact
                 (append (list platform architecture url sha256 size installer arguments)
                         (list 'app))))])
    (update-artifact platform architecture url sha256 size installer arguments)))

;; Every state-file touch lands in a throwaway config dir, never the user's
;; (current-config-dir is the settings.rkt parameter the updater reads).
(parameterize ([current-config-dir
                (make-temporary-file "pdfgist-updater-home-~a" 'directory)])
  (define download-limit (* 800 1024 1024))

  ;; same tolerance for updater-config (component field added mid-v0.11.0)
  (define (make-test-config pub key-id bucket)
    (define args
      (list app-identifier app-version app-channel
            (platform-symbol) (architecture-symbol)
            pub key-id bucket download-limit))
    (with-handlers
        ([exn:fail:contract:arity?
          (lambda (_) (apply updater-config (append args (list 'app))))])
      (apply updater-config args)))

  (test-case "platform symbols match rivet release manifests"
    (case (system-type 'os)
      [(macosx) (check-equal? (platform-symbol) 'macos)]
      [(windows) (check-equal? (platform-symbol) 'windows)]
      [else (check-equal? (platform-symbol) 'linux)])
    (check-not-false (memq (architecture-symbol) '(arm64 x64))))

  (test-case "installer extension follows platform"
    (case (system-type 'os)
      [(macosx) (check-equal? (installer-extension) ".dmg")]
      [(windows) (check-equal? (installer-extension) ".msi")]
      [else (check-equal? (installer-extension) ".tar.gz")]))

  (test-case "appimage asset name maps the feed arch onto the AppImage arch"
    (check-equal?
     (appimage-asset-name "pdfgist-0.1.1-linux-x64.tar.gz" 'x64)
     "pdfgist-0.1.1-linux-x86_64.AppImage")
    (check-equal?
     (appimage-asset-name "pdfgist-0.1.1-linux-arm64.tar.gz" 'arm64)
     "pdfgist-0.1.1-linux-aarch64.AppImage")
    ;; anything but a .tar.gz feed name is rejected…
    (check-exn exn:fail?
               (lambda ()
                 (appimage-asset-name "pdfgist-0.1.1-linux-x64.zip" 'x64)))
    ;; …as is a feed name whose arch suffix does not match
    (check-exn exn:fail?
               (lambda ()
                 (appimage-asset-name "pdfgist-0.1.1-linux-arm.tar.gz" 'arm64))))

  (test-case "manifest url joins base and canonical feed name"
    (check-equal?
     (manifest-url)
     (string-append default-update-base-url "/update-manifest.json")))

  (test-case "rollout bucket is sticky and persists to updater.json"
    (check-true (exact-integer? (rollout-bucket)))
    (define bucket0 (rollout-bucket))
    (check-true (<= 0 bucket0 99))
    ;; a second read returns the persisted bucket, not a fresh draw
    (check-equal? (rollout-bucket) bucket0)
    (define doc
      (with-input-from-file (build-path (config-dir-path) "updater.json")
        read-json))
    (check-equal? (hash-ref doc 'rolloutBucket) bucket0)
    ;; a persisted bucket wins over any fresh draw (sticky per install)
    (with-output-to-file (build-path (config-dir-path) "updater.json")
      #:exists 'truncate/replace
      (lambda () (write-json (hasheq 'rolloutBucket 42))))
    (check-equal? (rollout-bucket) 42))

  (test-case "corrupt updater.json degrades to empty prefs, not a crash"
    (with-output-to-file (build-path (config-dir-path) "updater.json")
      #:exists 'truncate/replace
      (lambda () (display "not json{")))
    (check-equal? (rollout-bucket) (rollout-bucket))
    (check-true (<= 0 (rollout-bucket) 99))
    (check-equal? (last-update-check-at) 'null))

  ;; A minimal one-shot HTTP artifact server: get-pure-port insists on a
  ;; well-formed response head. The declared Content-Length and the bytes
  ;; actually sent are separate, so the truncated case can mimic a dropped
  ;; connection mid-download.
  (define (serve-artifact-payload! payload declared-length)
    (define listener (tcp-listen 0 4 #f "127.0.0.1"))
    (define-values (_bind-host port-number _peer-host _peer-port)
      (tcp-addresses listener #t))
    (define server
      (thread
       (lambda ()
         (let accept-loop ()
           (define-values (in out) (tcp-accept listener))
           (thread
            (lambda ()
              (let drain ()
                (define line (read-line in 'any))
                (unless (or (eof-object? line) (equal? line "") (equal? line "\r"))
                  (drain)))
              (write-string
               (format "HTTP/1.0 200 OK\r\nContent-Length: ~a\r\n" declared-length)
               out)
              (write-string "Content-Type: application/zip\r\n\r\n" out)
              (write-bytes payload out)
              (close-output-port out)
              (close-input-port in)))
         (accept-loop)))))
    (values port-number
            (lambda ()
              (kill-thread server)
              (tcp-close listener))))

  (define (candidate-for pub url payload)
    (define manifest
      (update-manifest
       app-identifier "9.9.9" 7 'stable
       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
       (list (make-test-artifact (platform-symbol) (architecture-symbol)
                                 url (bytes->hex-string (sha256-bytes payload))
                                 (bytes-length payload) 'zip '()))))
    (select-update
     (make-test-config pub update-key-id 0)
     manifest))

  (define pub
    (datum->pk-key (base64-string->bytes update-public-key-b64)
                   'SubjectPublicKeyInfo))

  (test-case "download-update stages the artifact and reports percent"
    (define payload (make-bytes 300000 42))
    (define-values (port-number stop-server!)
      (serve-artifact-payload! payload (bytes-length payload)))
    (define candidate
      (candidate-for pub
                     (format "http://127.0.0.1:~a/pdfgist.zip" port-number)
                     payload))
    (check-true (update-candidate? candidate))
    (reset-update-state!)
    (define destination
      (build-path (config-dir-path) "updates" "PDFGist-9.9.9.zip"))
    (check-equal?
     (download-with-progress!
     (make-test-config pub update-key-id 0)
      candidate destination)
     destination)
    (check-true (file-exists? destination))
    (check-equal? (file-size destination) (bytes-length payload))
    (check-equal? (hash-ref (update-state-snapshot) 'percent) 100)
    (check-false (file-exists? (path-add-extension destination #".partial")))
    (delete-file destination)
    (stop-server!))

  (test-case "a truncated download raises and leaves no .partial behind"
    (define payload (make-bytes 300000 42))
    ;; declare the full length but send half: the size gate must fail the
    ;; download and rivet's partial cleanup must remove the residue
    (define-values (port-number stop-server!)
      (serve-artifact-payload! (subbytes payload 0 150000)
                               (bytes-length payload)))
    (define candidate
      (candidate-for pub
                     (format "http://127.0.0.1:~a/pdfgist.zip" port-number)
                     payload))
    (define destination
      (build-path (config-dir-path) "updates" "PDFGist-9.9.9.zip"))
    (check-exn exn:fail?
               (lambda ()
                 (download-with-progress!
     (make-test-config pub update-key-id 0)
                  candidate destination)))
    (check-false (file-exists? destination))
    (check-false (file-exists? (path-add-extension destination #".partial")))
    (stop-server!))

  (test-case "signed manifest verifies and selects updates"
    ;; throwaway keypair: same DER formats the release pipeline uses
    (define priv (generate-private-key 'eddsa '((curve ed25519))))
    (define priv-der (pk-key->datum priv 'OneAsymmetricKey))
    (define priv-path (make-temporary-file "pdfgist-test-key-~a.der"))
    (with-output-to-file priv-path
      #:exists 'truncate
      (lambda () (write-bytes priv-der)))
    (define test-pub (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public))
    (define artifact-file (make-temporary-file "pdfgist-artifact-~a.zip"))
    (call-with-output-file artifact-file
      #:exists 'truncate
      (lambda (out) (write-bytes (make-bytes 128 3) out)))
    (define manifest
      (update-manifest
       app-identifier "9.9.9" 7 'stable
       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
       (list (make-test-artifact (platform-symbol) (architecture-symbol)
                                 "https://dl.example/pdfgist/pdfgist-9.9.9.zip"
                                 (sha256-file/hex artifact-file)
                                 (file-size artifact-file)
                                 'zip '()))))
    (define wrapped (open-output-bytes))
    (write-signed-manifest manifest priv "pdfgist-test" wrapped)
    (define verified
      (verify-signed-manifest (open-input-bytes (get-output-bytes wrapped))
                              test-pub
                              #:key-id "pdfgist-test"))
    (check-equal? (update-manifest-version verified) "9.9.9")
    ;; a different key-id fails closed
    (check-exn exn:fail?
               (lambda ()
                 (verify-signed-manifest
                  (open-input-bytes (get-output-bytes wrapped))
                  test-pub
                  #:key-id "some-other-key")))
    ;; select-update picks the artifact for this platform/architecture the
    ;; same way the live checker does
    (define config
      (make-test-config test-pub "pdfgist-test" 42))
    (define candidate (select-update config verified))
    (check-true (update-candidate? candidate))
    (check-equal? (update-artifact-size (update-candidate-artifact candidate))
                  (file-size artifact-file))
    ;; the destination layout rides the signed manifest's installer symbol
    (check-equal?
     (destination-path candidate)
     (build-path (config-dir-path) "updates"
                 (string-append "Booklight-9.9.9.zip")))
    (delete-file priv-path)
    (delete-file artifact-file))

  (test-case "embedded release identity is coherent"
    (check-equal? app-identifier "site.jrtx.booklight")
    (check-equal? app-channel 'stable)
    (check-equal? update-key-id "booklight-2026-10")
    (check-true (string? app-version))
    (check-equal? app-build 1))

  (test-case "make-updater-config selects the feed's component-less artifact"
    ;; Regression: the 9->10-field updater-config shim appended 'app as its
    ;; component fallback, but the published feed's artifacts carry no
    ;; component at all (#f), and select-update matches config component
    ;; against artifact component — so every real check answered
    ;; up-to-date forever, invisible to every offline test here because
    ;; they all construct their own artifacts. Build the config exactly the
    ;; way the live check builds it and select against a manifest shaped
    ;; like the published feed: the plain artifact must be offered.
    (define (feed-shape-artifact platform architecture url sha256 size installer arguments)
      (with-handlers
          ([exn:fail:contract:arity?
            (lambda (_)
              (apply update-artifact
                     (append (list platform architecture url sha256 size installer arguments)
                             (list #f))))])
        (update-artifact platform architecture url sha256 size installer arguments)))
    (define manifest
      (update-manifest app-identifier "9.9.9" 99 'stable
                       "2026-11-01T00:00:00Z" "0.0.0" #f #t 100
                       (list (feed-shape-artifact (platform-symbol)
                                                  (architecture-symbol)
                                                  "https://downloads.example.com/booklight-9.9.9.tar.gz"
                                                  (make-string 64 #\a)
                                                  123456 'targz '()))))
    (check-true (update-candidate? (select-update (make-updater-config) manifest))
                "the shipped config must select the feed's component-less artifact")))
