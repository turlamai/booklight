#lang racket/base

;; Settings store: defaults, JSON round-trip in the v1 serde shape, the API
;; key seam, and the recents trim-to-12 policy.

(require json
         rackunit
         racket/file
         racket/runtime-path
         racket/string
         "../booklight/jsonw.rkt"
         "../booklight/providers.rkt"
         (prefix-in recents: "../booklight/recents.rkt")
         "../booklight/settings.rkt")

(define temp-root (make-temporary-file "pdfgist-settings-~a" 'directory))

;; Existing PDFGist preferences and annotations stay in their original
;; directory when no Booklight profile is present. No copy is created.
(define preferred-root (build-path temp-root "site.jrtx.booklight"))
(define legacy-root (build-path temp-root "site.jrtx.pdfgist"))
(check-equal? (resolve-config-dir preferred-root) preferred-root)
(make-directory legacy-root)
(check-equal? (resolve-config-dir preferred-root) legacy-root)
(check-false (directory-exists? preferred-root))
(make-directory preferred-root)
(check-equal? (resolve-config-dir preferred-root) preferred-root)


(dynamic-wind
 void
 (lambda ()
   (parameterize ([current-config-dir temp-root])
     ;; ---- defaults on a missing file ----
     (check-false (settings-file-exists?))
     (define defaults (load-settings))
     (check-equal? (provider-config-name (settings-provider defaults)) "deepseek")
     (check-equal? (provider-config-base-url (settings-provider defaults))
                   "https://api.deepseek.com/v1")
     (check-equal? (provider-config-api-key (settings-provider defaults)) "")
     (check-equal? (provider-config-model (settings-provider defaults)) "deepseek-chat")
     (check-equal? (settings-target-language defaults) "中文")
     (check-equal? (settings-recent-files defaults) '())
     (check-equal? (settings-view-mode defaults) "single")
     (check-true (settings-annotation-sidecar defaults))
     (check-equal? (settings-reading-theme defaults) "system")
     (check-equal? (settings-reading-font-scale defaults) 1.0)

     ;; ---- v1 serde shape of a default file (field order + pretty format;
     ;; reading prefs append after the v1 fields, so v1 installs parse as
     ;; defaults and stay drop-in) ----
     (check-equal?
      (json-value->bytes (settings->storage defaults))
      (string->bytes/utf-8
       (string-append
        "{\n"
        "  \"provider\": {\n"
        "    \"name\": \"deepseek\",\n"
        "    \"base_url\": \"https://api.deepseek.com/v1\",\n"
        "    \"api_key\": \"\",\n"
        "    \"model\": \"deepseek-chat\"\n"
        "  },\n"
        "  \"target_language\": \"中文\",\n"
        "  \"recent_files\": [],\n"
        "  \"view_mode\": \"single\",\n"
        "  \"annotation_sidecar\": true,\n"
        "  \"reading_theme\": \"system\",\n"
        "  \"reading_font_scale\": 1.0"
        "\n}")))

     ;; ---- round-trip ----
     (define custom
       (pdfgist-settings
        (provider-config "openai" "https://api.example.com/v1" "" "gpt-4o")
        "English"
        (list (recents:recent-file "/tmp/x.pdf" "X" 7 0.5 1700000000))
        "double"
        #t
        "night"
        1.25))
     (save-settings! custom)
     (check-true (settings-file-exists?))
     (define loaded (load-settings))
     (check-equal? (provider-config-name (settings-provider loaded)) "openai")
     (check-equal? (provider-config-model (settings-provider loaded)) "gpt-4o")
     (check-equal? (settings-target-language loaded) "English")
     (check-equal? (settings-view-mode loaded) "double")
     (check-true (settings-annotation-sidecar loaded))
     (check-equal? (settings-reading-theme loaded) "night")
     (check-equal? (settings-reading-font-scale loaded) 1.25)
     (define recents (settings-recent-files loaded))
     (check-equal? (length recents) 1)
     (check-equal? (recents:recent-file-path (car recents)) "/tmp/x.pdf")
     (check-equal? (recents:recent-file-page (car recents)) 7)
     (check-equal? (recents:recent-file-scroll-ratio (car recents)) 0.5)
     (check-equal? (recents:recent-file-last-read (car recents)) 1700000000)

     ;; Recents keep serde float formatting (0.5, 0.0).
     (check-true
      (string-contains?
       (bytes->string/utf-8 (file->bytes (settings-file-path)))
       "\"scroll_ratio\": 0.5"))

     ;; ---- api key seam ----
     (define with-key (set-api-key! loaded "sk-secret"))
     (check-equal? (api-key-of with-key) "sk-secret")
     ;; The other fields survive a key update.
     (check-equal? (settings-target-language with-key) "English")

     ;; update-settings! is read-modify-write and persists.
     (update-settings!
      (lambda (current) (set-api-key! current "sk-persisted")))
     (check-equal? (api-key-of (load-settings)) "sk-persisted")
     (check-equal? (settings-view-mode (load-settings)) "double")

     ;; ---- lenient parse: bad fields fall back per-field ----
     (define parsed
       (storage->settings
        (hasheq 'provider (hasheq 'name 42 'model "kept-model")
                'recent_files "not-a-list")))
     (check-equal? (provider-config-name (settings-provider parsed)) "deepseek")
     (check-equal? (provider-config-model (settings-provider parsed)) "kept-model")
     (check-equal? (settings-recent-files parsed) '())
     ;; A *missing settings file* gets a sidecar default. A legacy file
     ;; lacking this key must keep its pre-sidecar central annotation store.
     (check-false (settings-annotation-sidecar parsed))
     (check-false (settings-annotation-sidecar
                   (storage->settings (hasheq 'annotation_sidecar #f))))
     (check-equal? (settings-reading-theme parsed) "system")
     (check-equal? (settings-reading-font-scale parsed) 1.0)
     (check-exn exn:fail? (lambda () (storage->settings "not-an-object")))

     ;; ---- reading prefs normalization ----
     (define bad-reading
       (storage->settings
        (hasheq 'reading_theme "solarized" 'reading_font_scale "big")))
     (check-equal? (settings-reading-theme bad-reading) "system")
     (check-equal? (settings-reading-font-scale bad-reading) 1.0)
     (check-equal? (settings-reading-font-scale
                    (storage->settings (hasheq 'reading_font_scale 9)))
                   2.0)
     (check-equal? (settings-reading-font-scale
                    (storage->settings (hasheq 'reading_font_scale 0.1)))
                   0.7)
     (check-equal? (settings-reading-theme
                    (storage->settings (hasheq 'reading_theme "paper")))
                   "paper")
     (check-equal? (settings-reading-theme
                    (storage->settings (hasheq 'reading_theme "sepia")))
                   "sepia")

     ;; ---- language mapping ----
     (check-equal? (language-name->code "繁體中文") "zh-hant")
     (check-equal? (language-name->code "한국어") "ko")
     (check-equal? (language-name->code "未知语言") "zh")
     (check-equal? (language-code->name "fr") "Français")
     (check-equal? (language-code->name "zz") "中文")
     (check-equal? (length languages) 8)

     ;; ---- recents trim-to-12 ----
     (check-equal? recents:max-recents 12)
     (define entries
       (for/list ([i (in-range 20)])
         (recents:recent-file (format "/f/~a.pdf" i) "" 1 0.0 i)))
     (check-equal? (length (recents:trim-recents entries)) 12)
     (check-equal? (length (recents:trim-recents entries 5)) 5)
     (define upserted
       (recents:upsert-recent
        (recents:upsert-recent entries (recents:recent-file "/f/new.pdf" "" 3 0.25 99))
        (recents:recent-file "/f/19.pdf" "" 9 1.0 100)))
     (check-equal? (length upserted) 12)
     ;; Newest first; the re-upsert of /f/19.pdf moved it to the front.
     (check-equal? (recents:recent-file-path (car upserted)) "/f/19.pdf")
     (check-equal? (recents:recent-file-path (cadr upserted)) "/f/new.pdf")
     (check-equal? (recents:recent-file-page (car upserted)) 9)))
 (lambda ()
   (delete-directory/files temp-root)))
