#lang racket/base

;; Deterministic native-acceptance profile: Ollama needs no API key, but the
;; endpoint is intentionally unreachable.  The AI send-preview journey must
;; cancel before any request reaches it.
(require json
         racket/cmdline
         racket/file)

(define config-dir
  (command-line #:program "seed-ai-preview" #:args (config-dir) config-dir))
(make-directory* config-dir)
(call-with-output-file (build-path config-dir "settings.json")
  #:exists 'truncate/replace
  (lambda (out)
    (write-json
     (hasheq 'provider
             (hasheq 'name "ollama"
                     'base_url "http://127.0.0.1:1/v1"
                     'api_key ""
                     'model "acceptance-never-send")
             'target_language "English"
             'recent_files '()
             'view_mode "single"
             'annotation_sidecar #f
             'reading_theme "system"
             'reading_font_scale 1.0)
     out)))
