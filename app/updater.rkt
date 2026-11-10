#lang racket/base

;; Online update support for PDFGist, built on rivet/distribution — the
;; family pattern (taskly/brainfuel/payback/syncpilot): the backend verifies
;; and downloads the signed update artifact; the native host owns
;; installation (rivet docs/release-and-updates.md). A check fetches and
;; verifies the Ed25519-signed channel manifest through rivet's
;; fetch-update-manifest — its 10-hop redirect budget covers the GitHub
;; release-asset 302 onto the CDN. The artifact download runs on a
;; background thread through rivet's download-update, which owns the
;; redirect-following fetch, the signed-size cap, the .partial cleanup on
;; every failure, the SHA-256 + size verification, and the final atomic
;; rename; this module only maps the byte progress onto the integer percent
;; published to a state box that the UI polls through the `update-state`
;; RPC (RVT1 events are thread-local, so a background thread cannot emit
;; them directly).
;;
;; Updater bookkeeping (lastUpdateCheckAt, rolloutBucket) does NOT ride the
;; settings.json document: that file must stay byte-compatible with v1
;; installs (AGENTS.md data contract). It lives in a small sibling file,
;; <config-dir>/updater.json, owned entirely by this module.
;;
;; Note on crypto factories: rivet/distribution pins the provider set to
;; libcrypto at module load and deliberately avoids crypto/all — factory
;; FFIs load at import time and the gmp factory kills embedded runtimes on
;; hosts without libgmp. This module runs inside the embedded CS runtime,
;; so it must not require crypto/all either (tests may, they are headless).

(require crypto
         json
         net/base64
         net/url
         ;; Explicit only-in: rivet/distribution gained platform-symbol /
         ;; architecture-symbol / installer-extension re-exports after v0.11.0
         ;; (the CI pin). An except-in of names a rivet build does not export
         ;; fails to expand, and a plain require collides with the local
         ;; defines below on rivets that do export them — listing exactly what
         ;; this module uses works on both.
         (only-in rivet/distribution
                  base64-string->bytes
                  download-update
                  fetch-update-manifest
                  select-update
                  sha256-file/hex
                  update-artifact-installer
                  update-artifact-size
                  update-artifact-url
                  update-candidate-artifact
                  update-candidate-manifest
                  update-manifest-build
                  update-manifest-published-at
                  update-manifest-version
                  updater-config)
         racket/file
         racket/list
         racket/path
         racket/port
         racket/string
         "../racket/booklight/settings.rkt"
         "version.rkt")

(provide platform-symbol
         architecture-symbol
         appimage-asset-name
         installer-extension
         manifest-url
         destination-path
         download-with-progress!
         make-updater-config
         update-state-snapshot
         reset-update-state!
         perform-check!
         start-download!
         rollout-bucket
         last-update-check-at)

;; rivet release tooling emits these exact symbols into update manifests
(define (platform-symbol)
  (case (system-type 'os)
    [(macosx) 'macos]
    [(windows) 'windows]
    [else 'linux]))

(define (architecture-symbol)
  (case (system-type 'arch)
    [(aarch64 arm64) 'arm64]
    [else 'x64]))

;; rivet moved updater-config to 10 fields (component) mid-v0.11.0; the
;; updater constructs through this shim so production runs on either shape.
;; The component must be #f: the published feed's artifacts carry no
;; component (the plain whole-app artifact) and select-update matches
;; config component against artifact component — 'app would silently
;; select nothing from the real feed (racket/tests/updater-test.rkt pins
;; this against a feed-shaped manifest).
(define (make-updater-config)
  (define args
    (list app-identifier
          app-version
          app-channel
          (platform-symbol)
          (architecture-symbol)
          (embedded-public-key)
          update-key-id
          (rollout-bucket)
          maximum-download-bytes))
  (with-handlers
      ([exn:fail:contract:arity?
        (lambda (_)
          (apply updater-config (append args (list #f))))])
    (apply updater-config args)))

(define (installer-extension)
  (case (system-type 'os)
    [(macosx) ".dmg"]
    [(windows) ".msi"]
    [else ".tar.gz"]))

(define maximum-download-bytes (* 800 1024 1024))

;; ---------- public key ----------

;; The embedded DER is the public half of the update key whose private half
;; never ships.
(define (embedded-public-key)
  (datum->pk-key (base64-string->bytes update-public-key-b64)
                 'SubjectPublicKeyInfo))

;; ---------- shared update state (UI-visible) ----------

;; phase: idle | checking | downloading | downloaded | error
(define update-state
  (box (hasheq 'phase "idle"
               'percent 0
               'message 'null
               'downloadedPath 'null
               'availableVersion 'null)))

(define candidate-box (box #f))
(define worker-thread-box (box #f))

;; Download watchdog bounds. rivet's copy loop waits on read-bytes-avail!
;; with no timeout, so a connection that stalls without RST/FIN parks the
;; worker thread forever — visibly "stuck at 100%" (all bytes had arrived;
;; the loop was still waiting for EOF). Two finite bounds keep the phase
;; machine honest: no byte progress for the stall limit, or wall clock past
;; the total limit, breaks the worker (rivet's partial-file cleanup runs,
;; and the worker reports phase=error itself) so the UI always gets a
;; terminal state.
(define download-stall-limit-seconds 120)
(define download-total-limit-seconds (* 30 60))
(define last-progress-seconds (box 0))

(define (state-set! key value)
  (set-box! update-state (hash-set (unbox update-state) key value)))

(define (update-state-snapshot)
  (unbox update-state))

(define (reset-update-state!)
  (set-box! candidate-box #f)
  (set-box! update-state
            (hasheq 'phase "idle"
                    'percent 0
                    'message 'null
                    'downloadedPath 'null
                    'availableVersion 'null)))

;; ---------- updater bookkeeping (updater.json; v1 settings stay untouched) --

(define (updater-state-path)
  (build-path (config-dir-path) "updater.json"))

;; missing/corrupt file -> empty prefs (the updater never blocks on its own
;; bookkeeping; a fresh install simply has neither key)
(define (load-updater-prefs)
  (define path (updater-state-path))
  (if (not (file-exists? path))
      (hasheq)
      (with-handlers
          ([exn:fail?
            (lambda (e) (hasheq))])
        (define value (read-json (open-input-string (file->string path))))
        (if (hash? value) value (hasheq)))))

(define (save-updater-prefs! prefs)
  (make-directory* (config-dir-path))
  (with-output-to-file (updater-state-path)
    (lambda () (write-json prefs))
    #:exists 'replace))

(define (pref-with prefs key value)
  (hash-set prefs key value))

(define (last-update-check-at)
  (hash-ref (load-updater-prefs) 'lastUpdateCheckAt 'null))

;; ---------- manifest URL ----------

(define (manifest-url)
  (string-append (string-trim default-update-base-url "/" #:right? #t)
                 "/update-"
                 (symbol->string app-channel)
                 ".json"))

;; ---------- check ----------

;; Returns a wire jsexpr describing the outcome; also refreshes
;; lastUpdateCheckAt (epoch seconds) in updater.json on every completed check.
(define (perform-check!)
  (state-set! 'phase "checking")
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (state-set! 'phase "error")
          (state-set! 'message (exn-message e))
          (hasheq 'status "error" 'message (exn-message e)))])
    ;; fetch-update-manifest follows the GitHub 302 onto the CDN (10-hop
    ;; budget), caps the body at 1 MiB, and verifies the Ed25519 signature
    ;; before anything else looks at the bytes.
    (define manifest
      (fetch-update-manifest (manifest-url)
                             (embedded-public-key)
                             #:key-id update-key-id))
    (define config
      (make-updater-config))
    (define candidate (select-update config manifest))
    (save-updater-prefs!
     (pref-with (load-updater-prefs)
                'lastUpdateCheckAt (current-seconds)))
    (cond
      [candidate
       (set-box! candidate-box candidate)
       (define artifact (update-candidate-artifact candidate))
       (state-set! 'phase "idle")
       (state-set! 'availableVersion
                   (update-manifest-version (update-candidate-manifest candidate)))
       (hasheq 'status "available"
               'currentVersion app-version
               'availableVersion
               (update-manifest-version (update-candidate-manifest candidate))
               'build (update-manifest-build (update-candidate-manifest candidate))
               'publishedAt
               (update-manifest-published-at (update-candidate-manifest candidate))
               'installer (symbol->string (update-artifact-installer artifact))
               'sizeBytes (update-artifact-size artifact))]
      [else
       (state-set! 'phase "idle")
       (state-set! 'availableVersion 'null)
       (hasheq 'status "up-to-date" 'currentVersion app-version)])))

;; rollout bucket: stable random 0..99 assigned on first check so staged
;; rollouts are sticky per installation. crypto-random-bytes, not `random`:
;; racket/base's PRNG has a fixed seed, which would put every fresh install
;; in the same lockstep bucket.
(define (rollout-bucket)
  (define prefs (load-updater-prefs))
  (define existing (hash-ref prefs 'rolloutBucket 'null))
  (cond
    [(and (exact-integer? existing) (<= 0 existing 99)) existing]
    [else
     (define bucket
       (modulo (integer-bytes->integer (crypto-random-bytes 4) #t #t) 100))
     (save-updater-prefs! (pref-with prefs 'rolloutBucket bucket))
     bucket]))

;; ---------- AppImage flavor (Linux installs run from an AppImage) ----------
;; The feed's linux entry is the tar.gz; AppImage installs update through the
;; sibling .AppImage release asset (taskly family pattern). Asset families
;; disagree on arch names — feed/deb/rpm use x64|arm64, AppImage builds use
;; x86_64|aarch64 — so the derivation maps the arch before appending. The
;; download lands NEXT TO the running image (same filesystem → atomic
;; rename) and replaces it after the sidecar checksum verifies; the host
;; re-execs the replaced image on user consent.

(define (install-flavor)
  ;; Mirrors the host's install_shape(): $APPIMAGE (set by the AppImage
  ;; runtime) points at the running image; deb lands under /opt/pdfgist and
  ;; rpm under /usr — those upgrade through the package manager; everything
  ;; else is the portable tarball layout. The embedded runtime's own process
  ;; is the host executable, so /proc/self/exe is the install prefix —
  ;; Linux-only, which is exactly the flavor this check matters for.
  (define self (getenv "APPIMAGE"))
  (if (and self (non-empty-string? self))
      "appimage"
      (let ([exe (with-handlers ([exn:fail? (lambda (_) #f)])
                   (resolve-path (string->path "/proc/self/exe")))])
        (if (and exe
                 (let ([s (path->string exe)])
                   (or (string-prefix? s "/opt/pdfgist/")
                       (string-prefix? s "/usr/"))))
            "package"
            "tarball"))))


(define (appimage-flavor?)
  (and (eq? (platform-symbol) 'linux)
       (equal? (install-flavor) "appimage")))

;; Pure so tests can pin the derivation — the arch-name mapping is the trap.
(define (appimage-asset-name tar-gz-filename architecture)
  (unless (string-suffix? tar-gz-filename ".tar.gz")
    (error 'appimage-asset-name "not a tar.gz filename: ~a" tar-gz-filename))
  (define stem
    (substring tar-gz-filename 0 (- (string-length tar-gz-filename) 7)))
  (define feed-arch (string-append "-" (symbol->string architecture)))
  (unless (string-suffix? stem feed-arch)
    (error 'appimage-asset-name
           "feed filename does not end in ~a: ~a" feed-arch stem))
  (define appimage-arch
    (case architecture [(arm64) "aarch64"] [else "x86_64"]))
  (string-append (substring stem 0 (- (string-length stem)
                                      (string-length feed-arch)))
                 "-"
                 appimage-arch
                 ".AppImage"))

(define (appimage-artifact-urls candidate)
  (define url (update-artifact-url (update-candidate-artifact candidate)))
  (define filename
    (last (string-split (car (string-split url "?")) "/")))
  (define appimage-name
    (appimage-asset-name filename (architecture-symbol)))
  (values (string-replace url filename appimage-name)
          (string-append (string-replace url filename appimage-name)
                         ".sha256")))

(define (appimage-download-target)
  ;; Stage next to the running image so the final rename stays on one
  ;; device (atomic swap).
  (define self (getenv "APPIMAGE"))
  (unless (and self (non-empty-string? self))
    (error 'start-download "APPIMAGE is not set; not running as an AppImage"))
  (values self
          (make-temporary-file "pdfgist-update-~a.AppImage"
                               #f
                               (path-only self))))

(define (fetch-sidecar-sha256! url)
  ;; sha256sum format: "<hex>  <filename>"; the first token is the digest.
  (define in (get-pure-port (string->url url)
                            '("User-Agent: Booklight-Updater/1")
                            #:redirections 10))
  (dynamic-wind
    void
    (lambda ()
      (define digest
        (let ([body (string-trim (port->string in))])
          (if (string=? body "") "" (first (string-split body)))))
      (unless (regexp-match? #px"^[0-9a-fA-F]{64}$" digest)
        (error 'start-download "appimage checksum sidecar has an unexpected shape"))
      (string-downcase digest))
    (lambda () (close-input-port in))))

(define (open-download-with-total! url)
  (define in (get-impure-port (string->url url)
                              '("User-Agent: Booklight-Updater/1"
                                "Connection: close")))
  (let loop ([total 0])
    (define line (read-line in 'return-linefeed))
    (cond
      [(eof-object? line) (values in 0)]
      [(non-empty-string? (string-trim line))
       (define match
         (regexp-match #px"(?i:^content-length:\\s*(\\d+))" line))
       (loop (if match (string->number (second match)) total))]
      [else (values in total)])))

(define (copy-with-progress! in out total)
  (define buffer (make-bytes 65536))
  (let loop ([done 0] [last-percent -1])
    (define count (read-bytes-avail! buffer in))
    (cond
      [(eof-object? count) done]
      [else
       ;; Every successful read feeds the watchdog's stall clock (not just
       ;; integer percent changes, so a slow link never false-trips it).
       (set-box! last-progress-seconds (current-seconds))
       (write-bytes buffer out 0 count)
       (define next (+ done count))
       (define percent
         (if (> total 0)
             (min 100 (quotient (* next 100) total))
             0))
       (when (> percent last-percent)
         (state-set! 'percent percent))
       (loop next percent)])))

(define (download-appimage-with-progress! candidate)
  (define-values (appimage-url sidecar-url)
    (appimage-artifact-urls candidate))
  (define-values (self temporary) (appimage-download-target))
  (define expected (fetch-sidecar-sha256! sidecar-url))
  (with-handlers ([exn:fail?
                   (lambda (e)
                     (when (file-exists? temporary)
                       (delete-file temporary))
                     (raise e))])
    (define-values (in total) (open-download-with-total! appimage-url))
    (dynamic-wind
      void
      (lambda ()
        (call-with-output-file temporary
          #:exists 'truncate/replace
          #:mode 'binary
          (lambda (out) (copy-with-progress! in out total)))
        (define actual (string-downcase (sha256-file/hex temporary)))
        (unless (string=? actual expected)
          (raise-arguments-error 'start-download
                                 "downloaded appimage does not match its checksum sidecar"
                                 "expected" expected
                                 "actual" actual))
        ;; The replaced image must stay executable: rename carries the temp
        ;; file's modes, so set them explicitly before the swap. The setter
        ;; only takes integer bits — the vector form is a contract violation
        ;; (the same trap taskly 63c1f00 and movebit 2f5632f fixed; this is
        ;; the fourth copy of the swap, so pin the portable form).
        (file-or-directory-permissions temporary #o755)
        (rename-file-or-directory temporary self #t))
      (lambda () (close-input-port in))))
  self)

;; ---------- download ----------

;; Container file extension for a manifest installer symbol. The family
;; feed serves portable zips; DMG (and later MSI/targz) manifests stay
;; supported. Unknown symbols fall back to the platform default so a
;; future installer kind cannot crash the download path.
(define (installer-extension/symbol sym)
  (case sym
    [(zip) ".zip"]
    [(dmg) ".dmg"]
    [(msi) ".msi"]
    [(targz) ".tar.gz"]
    [else (installer-extension)]))

(define (destination-path candidate)
  (define artifact (update-candidate-artifact candidate))
  (define version
    (update-manifest-version (update-candidate-manifest candidate)))
  (build-path (config-dir-path)
              "updates"
              (string-append app-display-name "-" version
                             (installer-extension/symbol
                              (update-artifact-installer artifact)))))

;; rivet's download-update owns the whole artifact path — redirect-following
;; fetch, signed-size cap, .partial cleanup on any failure, SHA-256 + size
;; verification against the signed manifest, atomic rename — so the wrapper
;; only maps its byte progress onto the integer percent the UI polls, and
;; feeds the watchdog's stall clock on every report.
(define (download-with-progress! config candidate destination)
  (define total (update-artifact-size (update-candidate-artifact candidate)))
  (download-update
   config candidate destination
   #:on-progress
   (lambda (done)
     (set-box! last-progress-seconds (current-seconds))
     (state-set! 'percent
                 (if (> total 0)
                     (min 100 (quotient (* done 100) total))
                     0)))))

(define (start-download!)
  (define worker (unbox worker-thread-box))
  (when (and worker (thread-running? worker))
    (error 'start-download! "an update download is already running"))
  (define candidate (unbox candidate-box))
  (unless candidate
    (error 'start-download! "no update is available; run a check first"))
  (state-set! 'phase "downloading")
  (state-set! 'percent 0)
  (state-set! 'message 'null)
  (define config
    (make-updater-config))
  (define destination
    (if (appimage-flavor?)
        ;; the staged temp beside the running image; the final swap is an
        ;; atomic rename onto $APPIMAGE (see download-appimage-with-progress!)
        (let-values ([(_self temporary) (appimage-download-target)])
          temporary)
        (destination-path candidate)))
  (set-box! last-progress-seconds (current-seconds))
  (define worker-thread
    (thread
     (lambda ()
       ;; The watchdog breaks a stalled worker; exn:break is not an
       ;; exn:fail, so it needs its own arm — either way rivet's partial
       ;; cleanup removes the .partial (the appimage path cleans its own
       ;; temp) and the phase lands on error (never mid-flight).
       (with-handlers
           ([(lambda (e) (or (exn:fail? e) (exn:break? e)))
             (lambda (e)
               (when (file-exists? (path-add-extension destination #".partial"))
                 (delete-file (path-add-extension destination #".partial")))
               (state-set! 'phase "error")
               (state-set! 'message
                           (if (exn:break? e)
                               "update download stalled or timed out"
                               (exn-message e))))])
         (define path
           (if (appimage-flavor?)
               (download-appimage-with-progress! candidate)
               (download-with-progress! config candidate destination)))
         (state-set! 'phase "downloaded")
         (state-set! 'percent 100)
         (state-set! 'downloadedPath (path->string path))))))
  (set-box! worker-thread-box worker-thread)
  ;; Watchdog: exits once the worker finishes on its own; otherwise a stall
  ;; (no byte progress) or the total wall-clock limit breaks the worker so
  ;; phase never rests on "downloading" indefinitely.
  (define download-started (current-seconds))
  (thread
   (lambda ()
     (let loop ()
       (cond
         [(sync/timeout download-stall-limit-seconds
                        (thread-dead-evt worker-thread))
          (void)]
         [(> (- (current-seconds) (unbox last-progress-seconds))
             download-stall-limit-seconds)
          (break-thread worker-thread)]
         [(> (- (current-seconds) download-started)
             download-total-limit-seconds)
          (break-thread worker-thread)]
         [else (loop)]))))
  (void))
