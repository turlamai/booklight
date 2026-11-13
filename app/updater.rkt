#lang racket/base

;; Online update support for Booklight, built on rivet/distribution's
;; managed updater — the family pattern (taskly/payback/syncpilot): the
;; backend verifies and downloads the signed update artifact; the native
;; host owns installation (rivet docs/release-and-updates.md). The managed
;; instance owns candidate selection, the background worker, progress, the
;; stall/total watchdog, cancellation and the terminal state. This module
;; supplies the product identity, feed, staging paths, and the AppImage
;; in-place swap, then maps the snapshot onto the JSON wire shape the UI
;; polls through `update-state` (RVT1 events are thread-local, so a
;; background thread cannot emit them directly).
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
         (only-in rivet/distribution
                  make-updater
                  updater-check!
                  updater-download!
                  updater-state
                  updater-cancel!
                  updater-wait
                  base64-string->bytes
                  update-artifact-installer
                  update-candidate-artifact
                  update-candidate-manifest
                  update-manifest-version)
         racket/file
         racket/path
         racket/string
         "../racket/booklight/settings.rkt"
         "version.rkt")

(provide platform-symbol
         architecture-symbol
         installer-extension
         appimage-download-target
         manifest-url
         destination-path
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

(define (installer-extension)
  (case (system-type 'os)
    [(macosx) ".dmg"]
    [(windows) ".msi"]
    [else ".tar.gz"]))

(define maximum-download-bytes (* 800 1024 1024))

;; ---------- shared update state (UI-visible) ----------

;; phase: idle | checking | downloading | downloaded | error
(define (idle-snapshot)
  (hasheq 'phase "idle"
          'percent 0
          'message 'null
          'downloadedPath 'null
          'availableVersion 'null))

(define update-state (box (idle-snapshot)))
(define updater-box (box #f))

(define (state-set! key value)
  (set-box! update-state (hash-set (unbox update-state) key value)))

;; The managed phases include transport/selection outcomes the UI treats as
;; idle: `available` carries its signal in availableVersion, `up-to-date`
;; means there is nothing to do, and `cancelled` returns the controls to
;; idle. Anything else passes through unchanged.
(define (managed-state->product state)
  (define phase (hash-ref state 'phase "idle"))
  (define mapped-phase
    (case phase
      [("available" "up-to-date" "cancelled") "idle"]
      [else phase]))
  (define idle? (string=? mapped-phase "idle"))
  (hasheq 'phase mapped-phase
          'percent (if idle? 0 (hash-ref state 'percent 0))
          'message (let ([message (and (not idle?)
                                       (hash-ref state 'message #f))])
                     (if message message 'null))
          'downloadedPath (let ([path (hash-ref state 'downloadedPath #f)])
                            (if path path 'null))
          'availableVersion (let ([version (hash-ref state 'availableVersion #f)])
                              (if version version 'null))))

(define (update-state-snapshot)
  (define instance (unbox updater-box))
  (if instance
      (managed-state->product (updater-state instance))
      (unbox update-state)))

(define (reset-update-state!)
  ;; Dropping an instance while its worker runs would orphan the download;
  ;; land it on the terminal cancelled phase first.
  (define instance (unbox updater-box))
  (when (and instance (equal? (hash-ref (updater-state instance) 'phase)
                              "downloading"))
    (updater-cancel! instance)
    (updater-wait instance 5.0))
  (set-box! updater-box #f)
  (set-box! update-state (idle-snapshot)))

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
  ;; Family-canonical feed name. The release also publishes the retired
  ;; update-stable.json as a byte-identical alias so pre-switch installs
  ;; keep updating (family transition policy) — this side stopped reading
  ;; it, so the alias can retire once those installs clear the field.
  (string-append (string-trim default-update-base-url "/" #:right? #t)
                 "/update-manifest.json"))

;; ---------- install flavor (Linux installs run from an AppImage) ----------

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

(define (update-install-kind)
  (and (appimage-flavor?) 'appimage))

(define (appimage-download-target)
  ;; Stage next to the running image so the final rename stays on one
  ;; device (atomic swap); return paths end to end — the state publishes
  ;; path->string of the result.
  (define self-string (getenv "APPIMAGE"))
  (unless (and self-string (non-empty-string? self-string))
    (error 'start-download "APPIMAGE is not set; not running as an AppImage"))
  (define self (string->path self-string))
  (values self
          (make-temporary-file "pdfgist-update-~a.AppImage"
                               #f
                               (path-only self))))

(define (install-appimage! verified self)
  ;; The replaced image must stay executable: rename carries the temp
  ;; file's modes, so set them explicitly before the swap. The setter only
  ;; takes integer bits — the vector form is a contract violation.
  (unless (eq? (system-type 'os) 'windows)
    (file-or-directory-permissions verified #o755))
  (rename-file-or-directory verified self #t)
  self)

;; ---------- download destination ----------

;; Container file extension for a manifest installer symbol. The family
;; feed serves portable zips; DMG/MSI/tar.gz manifests stay supported.
(define (installer-extension/symbol sym)
  (case sym
    [(zip) ".zip"]
    [(dmg) ".dmg"]
    [(msi) ".msi"]
    [(targz) ".tar.gz"]
    [else (installer-extension)]))

(define (destination-path candidate)
  (define artifact-version
    (update-manifest-version (update-candidate-manifest candidate)))
  (build-path (config-dir-path)
              "updates"
              (string-append app-display-name "-" artifact-version
                             (installer-extension/symbol
                              (update-artifact-installer
                               (update-candidate-artifact candidate))))))

;; ---------- managed updater ----------

(define (make-booklight-updater)
  ;; Set by the destination callback for an AppImage install and consumed
  ;; by the after-download hook in the same instance.
  (define appimage-self-box (box #f))
  (make-updater
   #:application-id app-identifier
   #:current-version app-version
   #:channel app-channel
   #:platform (platform-symbol)
   #:architecture (architecture-symbol)
   #:install-kind (update-install-kind)
   ;; The published feed's artifacts carry no component discriminator
   ;; (rivet #271); 'app would silently select nothing.
   #:component #f
   ;; DER bytes: the managed updater decodes them lazily during the check.
   #:public-key (base64-string->bytes update-public-key-b64)
   #:key-id update-key-id
   #:manifest-url (manifest-url)
   #:data-directory (config-dir-path)
   #:destination
   (lambda (candidate)
     (cond
       [(appimage-flavor?)
        (define-values (self staged) (appimage-download-target))
        (set-box! appimage-self-box self)
        staged]
       [else (destination-path candidate)]))
   #:after-download
   (lambda (verified candidate)
     (define self (unbox appimage-self-box))
     (if self
         (install-appimage! verified self)
         verified))
   #:rollout-bucket (rollout-bucket)
   #:maximum-download-bytes maximum-download-bytes))

;; ---------- check ----------

(define (instance-busy?)
  (define instance (unbox updater-box))
  (and instance
       (let ([phase (hash-ref (updater-state instance) 'phase "idle")])
         (or (equal? phase "checking")
             (equal? phase "downloading")))))

;; Returns a wire jsexpr describing the outcome; also refreshes
;; lastUpdateCheckAt (epoch seconds) in updater.json on every completed
;; check. The managed machine keeps the authenticated candidate after a
;; failed/cancelled download and clears it on a new check.
(define (perform-check!)
  (cond
    [(instance-busy?)
     (hasheq 'status "error"
             'message "an update check or download is already running")]
    [else
     (set-box! update-state (idle-snapshot))
     (with-handlers
         ([exn:fail?
           (lambda (e)
             (state-set! 'phase "error")
             (state-set! 'message (exn-message e))
             (hasheq 'status "error" 'message (exn-message e)))])
       (define instance (make-booklight-updater))
       (set-box! updater-box instance)
       (define result (updater-check! instance))
       (unless (equal? (hash-ref result 'status #f) "error")
         (save-updater-prefs!
          (pref-with (load-updater-prefs)
                     'lastUpdateCheckAt (current-seconds))))
       result)]))

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

;; ---------- download ----------

(define (start-download!)
  (define instance (unbox updater-box))
  (unless instance
    (error 'start-download! "no update is available; run a check first"))
  (updater-download! instance))
