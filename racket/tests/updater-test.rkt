#lang racket/base

;; Updater tests (family pattern): platform mapping, manifest URL
;; construction, the sticky rollout bucket (drawn once from
;; crypto-random-bytes and persisted in updater.json), the product phase
;; mapping over the managed state machine, and the offline trust chain —
;; craft a manifest, sign it with a throwaway Ed25519 key, verify it the
;; same way the live checker does, then drive the managed download worker
;; and the AppImage in-place swap against a local TCP server.
;; (fetch-update-manifest only accepts HTTPS, so a live network check is
;; exercised in production and the error path runs here against a closed
;; local port; selected checks inject the manifest through the managed
;; updater's test-support parameter.)

(require crypto
         crypto/all
         json
         rackunit
         (only-in rivet/distribution
                  base64-string->bytes
                  download-update
                  sha256-file/hex
                  select-update
                  update-artifact
                  update-artifact-installer
                  update-artifact-size
                  update-artifact-url
                  update-candidate
                  update-candidate-artifact
                  update-candidate?
                  update-candidate-manifest
                  update-manifest
                  update-manifest-version
                  updater-config
                  verify-signed-manifest
                  write-signed-manifest)
         ;; Transport injection for the managed worker: tests only.
         (submod (lib "rivet/distribution/managed-updater.rkt") test-support)
         racket/file
         racket/path
         racket/string
         racket/tcp
         "../../app/updater.rkt"
         "../../app/version.rkt"
         "../booklight/settings.rkt")

(use-all-factories!)

(define download-limit (* 800 1024 1024))

(define (wait-until ready? who [timeout-ms 10000])
  (let loop ([deadline (+ (current-inexact-milliseconds) timeout-ms)])
    (cond
      [(ready?) (void)]
      [(> (current-inexact-milliseconds) deadline)
       (fail (string-append "timed out waiting for " who))]
      [else (sleep 0.05) (loop deadline)])))

;; sha256 over in-memory bytes through the same file-level function the
;; verifier uses.
(define (sha256-bytes/hex bytes)
  (define scratch (make-temporary-file "booklight-sha-~a"))
  (dynamic-wind
    (lambda ()
      (call-with-output-file scratch #:exists 'truncate
        (lambda (out) (write-bytes bytes out))))
    (lambda () (sha256-file/hex scratch))
    (lambda () (delete-file scratch))))

;; A managed check over a crafted manifest: the transport parameter is
;; captured when perform-check! builds the instance.
(define (check-with-manifest! manifest)
  (parameterize ([current-managed-fetch-manifest
                  (lambda (_url _key
                          #:key-id [_key-id #f]
                          #:trusted-keys [_trusted-keys '()]
                          #:maximum-bytes [_maximum-bytes (* 1024 1024)])
                    manifest)])
    (perform-check!)))

(define (platform-artifact url bytes installer)
  (update-artifact (platform-symbol) (architecture-symbol)
                   url (sha256-bytes/hex bytes) (bytes-length bytes)
                   installer '()
                   ;; rivet #271: component #f = the plain whole-app artifact.
                   #f))

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

;; Every state-file touch lands in a throwaway config dir, never the user's
;; (current-config-dir is the settings.rkt parameter the updater reads).
(parameterize ([current-config-dir
                (make-temporary-file "pdfgist-updater-home-~a" 'directory)])
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

  (test-case "a failing feed fetch reports error state"
    (parameterize ([current-managed-fetch-manifest
                    (lambda (_url _key
                            #:key-id [_key-id #f]
                            #:trusted-keys [_trusted-keys '()]
                            #:maximum-bytes [_maximum-bytes (* 1024 1024)])
                      (error 'fetch-update-manifest "connection refused"))])
      (reset-update-state!)
      (define result (perform-check!))
      (check-equal? (hash-ref result 'status) "error")
      (check-equal? (hash-ref (update-state-snapshot) 'phase) "error")
      (check-true (string-contains? (hash-ref (update-state-snapshot) 'message)
                                    "connection refused"))))

  (test-case "managed check maps onto the wire phase vocabulary"
    (reset-update-state!)
    (define payload (make-bytes 128 7))
    (define manifest
      (update-manifest
       app-identifier "9.9.9" 7 'stable
       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
       (list (platform-artifact "https://dl.example/booklight-9.9.9.zip"
                                payload 'zip))))
    (define checked (check-with-manifest! manifest))
    (check-equal? (hash-ref checked 'status) "available")
    (check-equal? (hash-ref checked 'availableVersion) "9.9.9")
    (check-equal? (hash-ref checked 'installer) "zip")
    ;; The UI treats "available" as idle; the version carries the signal.
    (check-equal? (hash-ref (update-state-snapshot) 'phase) "idle")
    (check-equal? (hash-ref (update-state-snapshot) 'availableVersion) "9.9.9")
    (check-equal? (hash-ref (update-state-snapshot) 'percent) 0)
    ;; The completed check refreshed lastUpdateCheckAt in updater.json.
    (check-true (exact-integer? (last-update-check-at)))
    (reset-update-state!))

  (test-case "download-update stages the artifact and reports percent"
    (define payload (make-bytes 300000 42))
    (define-values (port-number stop-server!)
      (serve-artifact-payload! payload (bytes-length payload)))
    (define manifest
      (update-manifest
       app-identifier "9.9.9" 7 'stable
       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
       (list (platform-artifact
              (format "http://127.0.0.1:~a/booklight-9.9.9.zip" port-number)
              payload 'zip))))
    (reset-update-state!)
    (check-equal? (hash-ref (check-with-manifest! manifest) 'status) "available")
    (start-download!)
    (wait-until (lambda ()
                  (equal? (hash-ref (update-state-snapshot) 'phase)
                          "downloaded"))
                "the downloaded phase")
    (define destination
      (build-path (config-dir-path) "updates" "Booklight-9.9.9.zip"))
    (check-equal? (hash-ref (update-state-snapshot) 'downloadedPath)
                  (path->string destination))
    (check-equal? (hash-ref (update-state-snapshot) 'percent) 100)
    (check-true (file-exists? destination))
    (check-equal? (file-size destination) (bytes-length payload))
    (check-false (file-exists? (path-add-extension destination #".partial")))
    (delete-file destination)
    (reset-update-state!)
    (stop-server!))

  (test-case "a truncated download lands on the error phase with no .partial"
    (define payload (make-bytes 300000 42))
    ;; declare the full length but send half: the size gate must fail the
    ;; download and rivet's partial cleanup must remove the residue
    (define-values (port-number stop-server!)
      (serve-artifact-payload! (subbytes payload 0 150000)
                               (bytes-length payload)))
    (define manifest
      (update-manifest
       app-identifier "9.9.9" 7 'stable
       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
       (list (platform-artifact
              (format "http://127.0.0.1:~a/booklight-9.9.9.zip" port-number)
              payload 'zip))))
    (reset-update-state!)
    (check-equal? (hash-ref (check-with-manifest! manifest) 'status) "available")
    (start-download!)
    (wait-until (lambda ()
                  (equal? (hash-ref (update-state-snapshot) 'phase) "error"))
                "the error phase")
    (define destination
      (build-path (config-dir-path) "updates" "Booklight-9.9.9.zip"))
    (check-false (file-exists? destination))
    (check-false (file-exists? (path-add-extension destination #".partial")))
    (reset-update-state!)
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
       (list (update-artifact (platform-symbol) (architecture-symbol)
                              "https://dl.example/pdfgist/pdfgist-9.9.9.zip"
                              (sha256-file/hex artifact-file)
                              (file-size artifact-file)
                              'zip '() #f))))
    (define wrapped (open-output-bytes))
    (write-signed-manifest manifest priv update-key-id wrapped)
    (define verified
      (verify-signed-manifest (open-input-bytes (get-output-bytes wrapped))
                              test-pub
                              #:key-id update-key-id))
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
      (updater-config app-identifier app-version app-channel
                      (platform-symbol) (architecture-symbol)
                      test-pub update-key-id 42 download-limit #f))
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
    (check-equal? update-key-id "booklight-2026-11")
    (check-true (string? app-version))
    (check-equal? app-build 1))

  (test-case "the shipped updater selects the feed's component-less artifact"
    ;; Regression from the pre-managed migration: the config shim appended
    ;; 'app as its component fallback while the published feed's artifacts
    ;; carry no component (#f), so every live check answered up-to-date.
    ;; The shipped instance must select the feed-shaped artifact.
    (define manifest
      (update-manifest app-identifier "9.9.9" 99 'stable
                       "2026-11-01T00:00:00Z" "0.0.0" #f #t 100
                       (list (update-artifact (platform-symbol)
                                              (architecture-symbol)
                                              "https://downloads.example.com/booklight-9.9.9.tar.gz"
                                              (make-string 64 #\a)
                                              123456 'targz '() #f))))
    (reset-update-state!)
    (check-equal? (hash-ref (check-with-manifest! manifest) 'status) "available"
                  "the shipped updater must select the component-less artifact")
    (reset-update-state!))

  ;; The AppImage target is read from $APPIMAGE; the swap must keep paths
  ;; end to end (a string leaking into path->string crashed a successful
  ;; in-place update once).
  (test-case "the AppImage target stays a path end to end"
    (define image (make-temporary-file "booklight-appimage-target-~a.AppImage"))
    (define had-appimage (getenv "APPIMAGE"))
    (putenv "APPIMAGE" (path->string image))
    (dynamic-wind
      void
      (lambda ()
        (define-values (self staged) (appimage-download-target))
        (check-true (path? self))
        (check-true (path? staged))
        (when (file-exists? staged) (delete-file staged)))
      (lambda ()
        (when (file-exists? image) (delete-file image))
        (if (and had-appimage (not (equal? had-appimage "")))
            (putenv "APPIMAGE" had-appimage)
            (putenv "APPIMAGE" "")))))

  ;; Backend-owned install arm: Linux only — the AppImage in-place swap,
  ;; now driven by the signed manifest's appimage artifact and the managed
  ;; #:after-download hook.
  (when (eq? (platform-symbol) 'linux)
    (test-case "the AppImage swap replaces the running image"
      (define running-image
        (make-temporary-file "booklight-release-self-~a.AppImage"))
      (call-with-output-file running-image
        #:exists 'truncate
        (lambda (out) (write-bytes (make-bytes 64 1) out)))
      (define new-image-bytes (make-bytes 2048 5))
      (define-values (port-number stop-server!)
        (serve-artifact-payload! new-image-bytes (bytes-length new-image-bytes)))
      (define manifest
        (update-manifest
         app-identifier "9.9.9" 7 'stable
         "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
         (list (platform-artifact
                (format "http://127.0.0.1:~a/booklight-9.9.9-linux-x86_64.AppImage"
                        port-number)
                new-image-bytes 'appimage))))
      (define had-appimage (getenv "APPIMAGE"))
      (putenv "APPIMAGE" (path->string running-image))
      (dynamic-wind
        void
        (lambda ()
          (reset-update-state!)
          (define checked (check-with-manifest! manifest))
          (check-equal? (hash-ref checked 'status) "available")
          (check-equal? (hash-ref checked 'installer) "appimage")
          (start-download!)
          (wait-until (lambda ()
                        (equal? (hash-ref (update-state-snapshot) 'phase)
                                "downloaded"))
                      "the appimage downloaded phase")
          (check-equal? (hash-ref (update-state-snapshot) 'downloadedPath)
                        (path->string running-image)
                        "the swap must replace the running image in place")
          (check-equal? (file->bytes running-image) new-image-bytes)
          ;; 'bits answers an integer mask (#o755 = 493), not a symbol list.
          (check-true
           (not (zero? (bitwise-and
                        #o100
                        (file-or-directory-permissions running-image 'bits))))
           "the replaced image must stay executable")
          (reset-update-state!))
        (lambda ()
          (if (and had-appimage (not (equal? had-appimage "")))
              (putenv "APPIMAGE" had-appimage)
              (putenv "APPIMAGE" ""))))
      (stop-server!))))
