#lang racket/base

;; Settings store, same file and shape as v1 (Tauri settings.rs).
;; JSON keys, same defaults (drop-in migration for v1 installs):
;;   <config-dir>/settings.json
;;     provider: {name, base_url, api_key, model}
;;     target_language: display name string (中文 by default)
;;     recent_files: [{path, title, page, scroll_ratio, last_read}] (<= 12)
;;     view_mode: "single" | "double"
;;     annotation_sidecar: bool
;;     reading_theme: "system" | "paper" | "night" (reader page surface)
;;     reading_font_scale: real in [0.7, 2.0] (EPUB typography scale)
;;
;; The config dir is a parameter so tests can point at a temp directory.
;;
;; API key storage seam: the key stays inside settings.json for byte-compat
;; with v1 installs, but every read/write goes through `api-key-of` /
;; `set-api-key!`, so a secure-store backend (keychain, DPAPI) can replace
;; them without touching the rest of the stack.

(require json
         racket/file
         racket/path
         racket/port
         racket/string
         "i18n.rkt"
         "jsonw.rkt"
         "providers.rkt"
         "recents.rkt")

(provide current-config-dir
         config-dir-path
         settings-file-path
         resolve-config-dir
         pdfgist-settings
         pdfgist-settings?
         settings-provider
         settings-target-language
         settings-recent-files
         settings-view-mode
         settings-annotation-sidecar
         settings-reading-theme
         settings-reading-font-scale
         valid-reading-themes
         default-reading-theme
         default-reading-font-scale
         min-reading-font-scale
         max-reading-font-scale
         normalize-reading-font-scale
         provider-config
         provider-config?
         provider-config-name
         provider-config-base-url
         provider-config-api-key
         provider-config-model
         default-settings
         load-settings
         save-settings!
         update-settings!
         api-key-of
         set-api-key!
         settings->storage
         storage->settings
         settings-file-exists?)

;; ---- config dir convention (Tauri app_config_dir parity) ----

(define app-identifier "site.jrtx.booklight")

(define (default-config-dir)
  (case (system-type)
    [(macosx)
     (build-path (find-system-path 'home-dir)
                 "Library" "Application Support" app-identifier)]
    [(windows)
     (define appdata (getenv "APPDATA"))
     (if (and appdata (non-empty-string? appdata))
         (build-path appdata app-identifier)
         (build-path (find-system-path 'home-dir)
                     "AppData" "Roaming" app-identifier))]
    [else
     ;; Match GLib's g_get_user_config_dir, which the GTK host uses for its
     ;; native preferences. Respecting XDG_CONFIG_HOME also keeps packaged and
     ;; sandboxed launches inside the profile chosen by the caller instead of
     ;; silently splitting Booklight state across two configuration roots.
     (define xdg-config-home (getenv "XDG_CONFIG_HOME"))
     (if (and xdg-config-home (non-empty-string? xdg-config-home))
         (build-path xdg-config-home app-identifier)
         (build-path (find-system-path 'home-dir) ".config" app-identifier))]))

;; Prefer an existing Booklight profile. On the first launch after the
;; old PDFGist rename, if ONLY the old profile exists, continue reading that
;; same preferences/annotations folder in place. No directory clone or
;; automatic change of storage mode occurs, so central notes remain visible.
;; A user-created Booklight profile always wins over the old location.
(define (resolve-config-dir preferred)
  (define parent (path-only preferred))
  (define legacy
    (and parent (build-path parent "site.jrtx.pdfgist")))
  (if (and legacy
           (not (directory-exists? preferred))
           (directory-exists? legacy))
      legacy
      preferred))

(define current-config-dir
  (make-parameter (resolve-config-dir (default-config-dir))))

;; Existing files in a historical config directory remain owned by the user.
;; Booklight never clones or deletes that directory during startup.

(define (config-dir-path)
  (current-config-dir))

(define (settings-file-path)
  (build-path (current-config-dir) "settings.json"))

;; ---- model ----

(struct provider-config (name base-url api-key model) #:transparent)

;; Reading-theme values (the reader page surface, independent of the app
;; chrome): "system" follows the effective app appearance, "paper" forces the
;; warm light surface, "night" forces the dark surface, "sepia" the deep
;; cream parchment.
(define valid-reading-themes '("system" "paper" "night" "sepia"))

(define default-reading-theme "system")
(define default-reading-font-scale 1.0)

;; The EPUB typography scale rides in settings.json; hosts keep their A+/A-
;; steps inside this clamp.
(define min-reading-font-scale 0.7)
(define max-reading-font-scale 2.0)

(struct pdfgist-settings (provider target-language recent-files view-mode annotation-sidecar
                          reading-theme reading-font-scale)
  #:transparent)

;; Short accessor aliases used across the stack and by the backend.
(define (settings-provider s) (pdfgist-settings-provider s))
(define (settings-target-language s) (pdfgist-settings-target-language s))
(define (settings-recent-files s) (pdfgist-settings-recent-files s))
(define (settings-view-mode s) (pdfgist-settings-view-mode s))
(define (settings-annotation-sidecar s) (pdfgist-settings-annotation-sidecar s))
(define (settings-reading-theme s) (pdfgist-settings-reading-theme s))
(define (settings-reading-font-scale s) (pdfgist-settings-reading-font-scale s))

(define (default-settings)
  (pdfgist-settings (provider-config default-provider-name
                                     default-provider-base-url
                                     ""
                                     default-provider-model)
                    default-language
                    '()
                    "single"
                    #t  ; new installs keep annotations beside their source
                    default-reading-theme
                    default-reading-font-scale))

;; Keeps a stored font scale meaningful across host versions: anything
;; outside the clamp (or not a real number) falls back to the nearest bound
;; rather than breaking the whole read.
(define (normalize-reading-font-scale value)
  (min max-reading-font-scale
       (max min-reading-font-scale (float-or default-reading-font-scale value))))

;; ---- secure-store seam ----

;; Reads the key out of a loaded settings value. The default keeps the key in
;; settings.json (v1 compatibility).
(define (api-key-of settings)
  (provider-config-api-key (settings-provider settings)))

;; Returns a settings value with the key replaced. Implementations of a
;; secure store would persist to the keychain here and return a value whose
;; settings.json field holds a sentinel (e.g. ""), keeping the file portable.
(define (set-api-key! settings key)
  (unless (string? key)
    (raise-argument-error 'set-api-key! "string?" key))
  (struct-copy pdfgist-settings
               settings
               [provider (struct-copy provider-config
                                      (settings-provider settings)
                                      [api-key key])]))

;; ---- storage <-> model ----

(define (string-or default value)
  (if (string? value) value default))

(define (bool-or default value)
  (if (boolean? value) value default))

(define (integer-or default value)
  (if (exact-integer? value) value default))

(define (float-or default value)
  (cond
    [(real? value) (exact->inexact value)]
    [else default]))

;; settings value -> ordered storage value (serde field order).
(define (settings->storage settings)
  (define provider (settings-provider settings))
  (jobj
   (list
    (cons "provider"
          (jobj (list (cons "name" (provider-config-name provider))
                      (cons "base_url" (provider-config-base-url provider))
                      (cons "api_key" (provider-config-api-key provider))
                      (cons "model" (provider-config-model provider)))))
    (cons "target_language" (settings-target-language settings))
    (cons "recent_files"
          (for/list ([recent (in-list (settings-recent-files settings))])
            (jobj (list (cons "path" (recent-file-path recent))
                        (cons "title" (recent-file-title recent))
                        (cons "page" (recent-file-page recent))
                        (cons "scroll_ratio" (recent-file-scroll-ratio recent))
                        (cons "last_read" (recent-file-last-read recent))))))
    (cons "view_mode" (settings-view-mode settings))
    (cons "annotation_sidecar" (settings-annotation-sidecar settings))
    (cons "reading_theme" (settings-reading-theme settings))
    (cons "reading_font_scale" (settings-reading-font-scale settings)))))

;; parsed jsexpr -> settings; missing fields fall back to defaults
;; (serde #[serde(default)] semantics). Type mismatches are tolerated per
;; field instead of failing the whole read; only a non-object document is a
;; hard parse error.
(define (storage->settings value)
  (unless (hash? value)
    (raise (exn:fail (tf "backend.error.parse-settings" "not a JSON object")
                     (current-continuation-marks))))
  (define fallback (default-settings))
  (define defaults-provider (settings-provider fallback))
  (define provider-hash (hash-ref value 'provider (hasheq)))
  (unless (hash? provider-hash)
    (set! provider-hash (hasheq)))
  (define recent-raw (hash-ref value 'recent_files '()))
  (pdfgist-settings
   (provider-config
    (string-or (provider-config-name defaults-provider)
               (hash-ref provider-hash 'name #f))
    (string-or (provider-config-base-url defaults-provider)
               (hash-ref provider-hash 'base_url #f))
    (string-or (provider-config-api-key defaults-provider)
               (hash-ref provider-hash 'api_key #f))
    (string-or (provider-config-model defaults-provider)
               (hash-ref provider-hash 'model #f)))
   (string-or (settings-target-language fallback)
              (hash-ref value 'target_language #f))
   (if (list? recent-raw)
       (filter recent-file?
               (for/list ([item (in-list recent-raw)])
                 (jsexpr->recent-file item)))
       '())
   (string-or (settings-view-mode fallback)
              (hash-ref value 'view_mode #f))
   (bool-or (settings-annotation-sidecar fallback)
            ;; Existing settings.json files with no sidecar key came from an era
            ;; where central annotations were the default. Preserve that
            ;; history; only truly fresh profiles default to sidecars.
            (hash-ref value 'annotation_sidecar #f))
   (let ((theme (string-or default-reading-theme
                           (hash-ref value 'reading_theme #f))))
     (if (member theme valid-reading-themes)
         theme
         default-reading-theme))
   (normalize-reading-font-scale (hash-ref value 'reading_font_scale #f))))

(define (jsexpr->recent-file item)
  (and (hash? item)
       (string? (hash-ref item 'path #f))
       (recent-file (hash-ref item 'path)
                    (string-or "" (hash-ref item 'title #f))
                    (max 1 (integer-or 1 (hash-ref item 'page #f)))
                    (float-or 0.0 (hash-ref item 'scroll_ratio #f))
                    (integer-or 0 (hash-ref item 'last_read #f)))))

;; ---- file IO ----

(define (settings-file-exists?)
  (file-exists? (settings-file-path)))

;; Missing file -> defaults (parity with settings.rs get_settings).
(define (load-settings)
  (define path (settings-file-path))
  (if (not (file-exists? path))
      (default-settings)
      (with-handlers
          ([exn:fail:filesystem?
            (lambda (e)
              (raise (exn:fail (tf "backend.error.read-settings" (exn-message e))
                               (current-continuation-marks))))])
        (define text
          (with-handlers
              ([exn:fail?
                (lambda (e)
                  (raise (exn:fail (tf "backend.error.read-settings" (exn-message e))
                                   (current-continuation-marks))))])
            (call-with-input-file path port->string)))
        (define value
          (with-handlers
              ([exn:fail?
                (lambda (e)
                  (raise (exn:fail (tf "backend.error.parse-settings" (exn-message e))
                                   (current-continuation-marks))))])
            (read-json (open-input-string text))))
        (storage->settings value))))

;; Pretty-writes in serde_json field order so the file stays diffable
;; against v1 installs.
(define (save-settings! settings)
  (unless (pdfgist-settings? settings)
    (raise-argument-error 'save-settings! "pdfgist-settings?" settings))
  (define path (settings-file-path))
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf "backend.error.write-settings" (exn-message e))
                           (current-continuation-marks))))])
    (make-directory* (current-config-dir))
    (define data (json-value->bytes (settings->storage settings)))
    ;; Stage the complete new preferences file and atomically rename it.
    ;; In particular, updating the last-open page must not truncate the
    ;; provider settings or recents if the process crashes during a write.
    (call-with-atomic-output-file
     path
     (lambda (out _temporary-path)
       (write-bytes data out)
       (flush-output out))))
  (void))

;; Read-modify-write under an in-process lock; callers sharing this module
;; (recents updates, key updates, saves) never interleave.
(define settings-lock (make-semaphore 1))

(define (with-settings-lock thunk)
  (call-with-semaphore settings-lock thunk))

(define (update-settings! updater)
  (unless (procedure? updater)
    (raise-argument-error 'update-settings! "procedure?" updater))
  (with-settings-lock
   (lambda ()
     (define updated (updater (load-settings)))
     (unless (pdfgist-settings? updated)
       (raise-argument-error 'update-settings! "procedure returning settings" updater))
     (save-settings! updated)
     updated)))
