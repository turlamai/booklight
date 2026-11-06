#lang racket/base

;; Backend-side user-visible strings. shared/i18n/{zh,en}.json is the single
;; source: this module loads both catalogs from ../../shared/i18n at startup
;; and falls back to the embedded zh table below when the files are missing
;; (e.g. a stripped packaged bundle). Old-stack UI strings were hardcoded
;; Chinese; en.json carries a faithful translation.

(require (for-syntax racket/base)
         json
         racket/file
         racket/path
         racket/runtime-path
         racket/string)

(provide set-locale!
         current-locale
         tr
         tf)

(define-runtime-path zh-json-path (build-path 'up 'up "shared" "i18n" "zh.json"))
(define-runtime-path en-json-path (build-path 'up 'up "shared" "i18n" "en.json"))

;; Embedded fallback: identical to shared/i18n/zh.json at the time of the
;; port. Only used if the JSON single source cannot be read. Equal-based
;; `hash`, not `hasheq`: the lookup keys arrive as string literals from
;; other modules, and eq? on strings across compilation units is luck.
(define fallback-catalog-zh
  (hash "app.name" "Booklight"
          "backend.error.connect" "连接失败：{0}"
          "backend.error.connect-timeout" "连接失败：连接超时（{0} 秒）"
          "backend.error.http-status" "HTTP {0}"
          "backend.error.http-status-body" "HTTP {0}：{1}（POST {2}）"
          "backend.error.read-response" "读取响应失败：{0}"
          "backend.error.parse-response" "解析响应失败：{0}"
          "backend.error.sse-line-too-long" "响应数据异常（单行过长）"
          "backend.error.read-settings" "读取设置失败：{0}"
          "backend.error.parse-settings" "设置文件解析失败：{0}"
          "backend.error.write-settings" "写入设置失败：{0}"
          "backend.error.read-annotations" "读取批注失败：{0}"
          "backend.error.parse-annotations" "批注文件解析失败：{0}"
          "backend.error.save-annotations" "保存批注失败：{0}"
          "backend.error.bad-annotations" "批注数据格式不正确"
          "backend.stream.thinking" "思考中…"
          "backend.stream.stopped" "（已停止）"
          "backend.stream.no-content" "（无返回内容）"
          "backend.provider.glm-coding" "GLM Coding 套餐（智谱）"
          "backend.provider.zhipu" "智谱 GLM"
          "backend.provider.deepseek" "DeepSeek"
          "backend.provider.qwen" "通义千问 Qwen"
          "backend.provider.doubao" "豆包（火山方舟）"
          "backend.provider.doubao-coding" "豆包 Coding 套餐（火山方舟）"
          "backend.provider.moonshot" "Moonshot Kimi"
          "backend.provider.kimi-coding" "Kimi Code 套餐（Moonshot）"
          "backend.provider.siliconflow" "SiliconFlow 硅基流动"
          "backend.provider.openai" "OpenAI"
          "backend.provider.ollama" "Ollama（本地）"
          "backend.provider.custom" "自定义"))

(define fallback-catalog-en
  (hash "app.name" "Booklight"
          "backend.error.connect" "Connection failed: {0}"
          "backend.error.connect-timeout" "Connection failed: timed out after {0} s"
          "backend.error.http-status" "HTTP {0}"
          "backend.error.http-status-body" "HTTP {0}: {1} (POST {2})"
          "backend.error.read-response" "Failed to read response: {0}"
          "backend.error.parse-response" "Failed to parse response: {0}"
          "backend.error.sse-line-too-long" "Malformed response (single line too long)"
          "backend.error.read-settings" "Failed to read settings: {0}"
          "backend.error.parse-settings" "Failed to parse settings file: {0}"
          "backend.error.write-settings" "Failed to write settings: {0}"
          "backend.error.read-annotations" "Failed to read annotations: {0}"
          "backend.error.parse-annotations" "Failed to parse annotations file: {0}"
          "backend.error.save-annotations" "Failed to save annotations: {0}"
          "backend.error.bad-annotations" "Invalid annotations data format"
          "backend.stream.thinking" "Thinking…"
          "backend.stream.stopped" "(Stopped)"
          "backend.stream.no-content" "(No content returned)"
          "backend.provider.glm-coding" "GLM Coding Plan (Zhipu)"
          "backend.provider.zhipu" "Zhipu GLM"
          "backend.provider.deepseek" "DeepSeek"
          "backend.provider.qwen" "Qwen (Alibaba)"
          "backend.provider.doubao" "Doubao (Volcano Ark)"
          "backend.provider.doubao-coding" "Doubao Coding Plan (Volcano Ark)"
          "backend.provider.moonshot" "Moonshot Kimi"
          "backend.provider.kimi-coding" "Kimi Code Plan (Moonshot)"
          "backend.provider.siliconflow" "SiliconFlow"
          "backend.provider.openai" "OpenAI"
          "backend.provider.ollama" "Ollama (local)"
          "backend.provider.custom" "Custom"))

(define (load-catalog path fallback)
  (with-handlers ([exn:fail? (lambda (_) fallback)])
    (define value (with-handlers ([exn:fail? (lambda (_) eof)])
                    (call-with-input-file path read-json)))
    (if (hash? value)
        (for/fold ([table fallback])
                  ([key (in-list (hash-keys value))])
          (define item (hash-ref value key))
          (if (string? item)
              (hash-set table (symbol->string key) item)
              table))
        fallback)))

(define catalogs
  (hasheq "zh" (load-catalog zh-json-path fallback-catalog-zh)
          "en" (load-catalog en-json-path fallback-catalog-en)))

;; Locale is process-global state (a box, not a parameter): RPC handlers run
;; on fresh threads per request, so a parameter set by one request would not
;; be visible to the next.
(define locale-box (box "zh"))
(define locale-lock (make-semaphore 1))
(define known-locales '("zh" "en"))

;; (set-locale! "en") — invalid codes are ignored so a hostile or stale host
;; value cannot blank out the catalogs.
(define (set-locale! code)
  (unless (string? code)
    (raise-argument-error 'set-locale! "string?" code))
  (when (member code known-locales)
    (call-with-semaphore
     locale-lock
     (lambda () (set-box! locale-box code))))
  (void))

(define (current-locale)
  (unbox locale-box))

(define (catalog-for locale)
  (hash-ref catalogs locale (hash-ref catalogs "zh")))

;; (tr "backend.stream.thinking" fallback) -> translated string, falling back
;; to the zh catalog and finally to `fallback` (or the key itself).
(define (tr key [fallback #f])
  (unless (string? key)
    (raise-argument-error 'tr "string?" key))
  (or (hash-ref (catalog-for (current-locale)) key #f)
      (hash-ref (catalog-for "zh") key #f)
      fallback
      key))

;; (tf "backend.error.connect" message) -> translated template with {0},
;; {1}, ... placeholders substituted positionally.
(define (tf key . args)
  (for/fold ([template (tr key)])
            ([arg (in-list args)]
             [index (in-naturals)])
    (string-replace template
                    (string-append "{" (number->string index) "}")
                    (if (string? arg) arg (format "~a" arg)))))
