#lang racket/base

;; Annotations store, byte-for-byte compatible with v1 (Tauri annotations.rs) so
;; v1 files keep working (drop-in migration):
;;   - path key = FNV-1a 64-bit of the absolute PDF path, 16 lowercase hex
;;     chars, file <config-dir>/annotations/<key>.json
;;   - sidecar mode: <pdf-path>.booklight.json next to the PDF (cloud-drive
;;     sync); the legacy <pdf-path>.pdfgist.json name stays readable so
;;     pre-booklight sidecars migrate transparently
;;   - file shape {"annotations":[...],"bookmarks":[...]}; 0.8.x bare arrays
;;     load as annotations with empty bookmarks
;;   - serde #[serde(default)] defaults are re-applied on load, matching the
;;     old typed-deserialize behavior
;; Rects stay page-space floats in the JSON (byte-compatible); they only
;; cross the RPC as opaque JSON bytes, so no coordinate scaling happens here.

(require json
         racket/date
         racket/file
         racket/path
         racket/port
         racket/string
         "fnv.rkt"
         "i18n.rkt"
         "jsonw.rkt"
         (only-in "settings.rkt" current-config-dir))

(provide annotation-storage-path
         sidecar-storage-path
         legacy-sidecar-path
         annotations-dir-path
         annotation-recovery-path
         annotation-recovery-pending?
         empty-document
         legacy-array?
         load-document-bytes
         store-document-bytes!
         normalize-document-value
         normalize-annotation
         document-bytes->markdown)

;; ---- paths ----

(define (annotations-dir-path)
  (build-path (current-config-dir) "annotations"))

;; Sidecar files carry the product name; the pdfgist name is the legacy
;; shape kept readable so pre-booklight sidecars migrate transparently.
(define (sidecar-storage-path pdf-path)
  (unless (string? pdf-path)
    (raise-argument-error 'sidecar-storage-path "string?" pdf-path))
  (string-append pdf-path ".booklight.json"))

(define (legacy-sidecar-path pdf-path)
  (and (string? pdf-path)
       (string-append pdf-path ".pdfgist.json")))

(define (annotation-storage-path pdf-path)
  (unless (string? pdf-path)
    (raise-argument-error 'annotation-storage-path "string?" pdf-path))
  (build-path (annotations-dir-path)
              (string-append (fnv1a64-hex pdf-path) ".json")))

;; A recovery copy lives under the app-owned config directory even when the
;; user selected a sidecar beside the document.  The mode suffix matters: a
;; central save and a sidecar save for the same book are distinct promises and
;; must never consume each other's pending draft.
(define (annotation-recovery-path pdf-path sidecar?)
  (build-path (annotations-dir-path)
              "recovery"
              (string-append (fnv1a64-hex pdf-path)
                             (if sidecar? "-sidecar.json" "-central.json"))))

(define (document-storage-read-path pdf-path sidecar?)
  (if sidecar?
      (or (and (file-exists? (sidecar-storage-path pdf-path))
               (sidecar-storage-path pdf-path))
          (let ((legacy (legacy-sidecar-path pdf-path)))
            (and legacy (file-exists? legacy) legacy))
          (sidecar-storage-path pdf-path))
      (annotation-storage-path pdf-path)))

;; A recovery file is pending only while it is the copy load would select.
;; An external tool may have written a newer primary after a failed save; in
;; that case the recovery journal is stale and must not produce a false alarm.
(define (recovery-selected? stored-file recovery-file)
  (and (file-exists? recovery-file)
       (or (not (file-exists? stored-file))
           (>= (file-or-directory-modify-seconds recovery-file)
               (file-or-directory-modify-seconds stored-file)))))

(define (annotation-recovery-pending? pdf-path sidecar?)
  (recovery-selected? (document-storage-read-path pdf-path sidecar?)
                      (annotation-recovery-path pdf-path sidecar?)))

;; ---- model defaults (serde #[serde(default)] on the v1 structs) ----
;;
;; Field order mirrors the v1 Rust structs, so re-serialized files keep the
;; same key order as serde_json output; unknown keys are dropped exactly like
;; the old typed deserialization.

(define annotation-field-order '(id page rects excerpt color kind note created anchor))
(define rect-field-order '(x y width height))
(define bookmark-field-order '(page label created))

(define annotation-defaults
  (hasheq 'id ""
          'page 1
          'rects '()
          'excerpt ""
          'color "yellow"
          'kind "highlight"
          'note ""
          'created 0
          ;; EPUB anchor (chapter + character offsets into the chapter's
          ;; plain text, the same text epub-chapter-text serves); absent
          ;; for PDF annotations, whose rects anchor them. A false value
          ;; serializes no key at all, so PDF files stay byte-identical.
          'anchor #f))

(define anchor-defaults
  (hasheq 'chapter 1 'start 0 'end 0))

(define rect-defaults
  (hasheq 'x 0.0 'y 0.0 'width 0.0 'height 0.0))

(define bookmark-defaults
  (hasheq 'page 1
          'label ""
          'created 0))

;; Projects a parsed JSON object onto `order`, applying defaults for missing
;; fields (serde #[serde(default)] semantics).
(define (ordered-object value order defaults)
  (define source (if (hash? value) value (hasheq)))
  (jobj
   (for/list ([key (in-list order)])
     (cons (symbol->string key)
           (hash-ref source key (hash-ref defaults key))))))

(define (normalize-rect value)
  (ordered-object value rect-field-order rect-defaults))

(define (normalize-anchor value)
  (ordered-object value '(chapter start end) anchor-defaults))

(define (normalize-annotation value)
  (define source (if (hash? value) value (hasheq)))
  (define rects (hash-ref source 'rects (hash-ref annotation-defaults 'rects)))
  (define anchor (hash-ref source 'anchor (hash-ref annotation-defaults 'anchor)))
  (define projected
    (ordered-object value annotation-field-order annotation-defaults))
  (jobj
   (filter
    (lambda (field) (not (and (string=? (car field) "anchor")
                              (not (cdr field)))))
    (for/list ([field (in-list (jobj-fields projected))])
      (cond
        [(string=? (car field) "rects")
         (cons "rects"
               (if (list? rects)
                   (map normalize-rect rects)
                   '()))]
        [(string=? (car field) "anchor")
         (if anchor
             (cons "anchor" (normalize-anchor anchor))
             field)]
        [else field])))))

(define (normalize-bookmark value)
  (ordered-object value bookmark-field-order bookmark-defaults))

;; Returns the normalized document as an ordered storage value:
;; {"annotations": [...], "bookmarks": [...]}
(define (normalize-document-value value)
  (cond
    ;; 0.8.x wrote a bare annotation array; 0.9+ writes {annotations, bookmarks}.
    [(list? value)
     (jobj (list (cons "annotations" (map normalize-annotation value))
                 (cons "bookmarks" '())))]
    [(hash? value)
     (define annotations (hash-ref value 'annotations '()))
     (define bookmarks (hash-ref value 'bookmarks '()))
     (unless (and (list? annotations) (list? bookmarks))
       (raise (exn:fail (tr "backend.error.bad-annotations")
                        (current-continuation-marks))))
     (jobj (list (cons "annotations" (map normalize-annotation annotations))
                 (cons "bookmarks" (map normalize-bookmark bookmarks))))]
    [else
     (raise (exn:fail (tr "backend.error.bad-annotations")
                      (current-continuation-marks)))]))

;; Kept exported for tests/hosts that need to detect the legacy shape.
(define (legacy-array? value)
  (list? value))

(define (empty-document)
  (jobj (list (cons "annotations" '()) (cons "bookmarks" '()))))

;; ---- file IO ----

(define (read-file-string file who-key)
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf who-key (exn-message e))
                           (current-continuation-marks))))])
    ;; Lossy decode keeps parity with Rust's from_utf8_lossy.
    (bytes->string/utf-8 (call-with-input-file file port->bytes) #\uFFFD)))

;; Returns normalized pretty JSON bytes; missing files yield an empty
;; document, exactly like load_document in annotations.rs.
(define (load-document-bytes pdf-path sidecar?)
  (define stored-file (document-storage-read-path pdf-path sidecar?))
  (define recovery-file (annotation-recovery-path pdf-path sidecar?))
  ;; A pending recovery draft is the last acknowledged edit. Prefer it to the
  ;; older primary copy; the host surfaces the pending state and retries the
  ;; normal save explicitly.  If an external tool produced a newer primary,
  ;; keep that newer file instead of resurrecting a stale draft.
  (define file
    (cond
      [(recovery-selected? stored-file recovery-file)
       recovery-file]
      [else stored-file]))
  (if (not (file-exists? file))
      (json-value->bytes (empty-document))
      (let ()
        (define text (read-file-string file "backend.error.read-annotations"))
        (define value
          (with-handlers
              ([exn:fail?
                (lambda (e)
                  (raise (exn:fail (tf "backend.error.parse-annotations" (exn-message e))
                                   (current-continuation-marks))))])
            (read-json (open-input-string text))))
        (json-value->bytes (normalize-document-value value)))))

;; Validates the shape, applies defaults, and stores normalized pretty JSON
;; (what the host gets back from load is exactly what is on disk).
(define (atomic-write-bytes! file data)
  (define parent (path-only file))
  (when parent (make-directory* parent))
  (call-with-atomic-output-file
   file
   (lambda (out _temporary-path)
     (write-bytes data out)
     (flush-output out))))

(define (store-document-bytes! pdf-path sidecar? data)
  (unless (bytes? data)
    (raise-argument-error 'store-document-bytes! "bytes?" data))
  (define value
    (with-handlers
        ([exn:fail?
          (lambda (e)
            (raise (exn:fail (tf "backend.error.parse-annotations" (exn-message e))
                             (current-continuation-marks))))])
      (read-json (open-input-bytes data))))
  ;; A bare array was never a valid save payload (only load tolerated it).
  (unless (and (hash? value)
               (hash-has-key? value 'annotations)
               (list? (hash-ref value 'annotations)))
    (raise (exn:fail (tr "backend.error.bad-annotations")
                     (current-continuation-marks))))
  (define normalized (normalize-document-value value))
  (define file
    (if sidecar?
        (sidecar-storage-path pdf-path)
        (annotation-storage-path pdf-path)))
  (define normalized-bytes (json-value->bytes normalized))
  (define recovery-file (annotation-recovery-path pdf-path sidecar?))
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf "backend.error.save-annotations" (exn-message e))
                           (current-continuation-marks))))])
    ;; Stage the complete canonical document before touching the primary. If
    ;; the sidecar is read-only, the RPC can fail honestly while the edit still
    ;; survives a close/reopen cycle. Atomic replacement also prevents a crash
    ;; or full disk from truncating the last good primary file.
    (atomic-write-bytes! recovery-file normalized-bytes)
    (atomic-write-bytes! file normalized-bytes)
    (with-handlers ([exn:fail:filesystem? void])
      (delete-file recovery-file)))
  (void))

;; ---- markdown digest (the reader keeps it beside the book) ----
;;
;; Renders the v1 document bytes — exactly what a host annotation pane
;; holds, unsaved edits included — as a Markdown file. Section/kind labels
;; follow the backend locale; excerpts and notes stay verbatim. The doc
;; shape is the shared storage shape, so the same bytes hosts save are the
;; bytes this digests.

(define (doc-field obj key default)
  (cond
    [(jobj? obj)
     (define pair (assoc key (jobj-fields obj)))
     (if pair (cdr pair) default)]
    [(hash? obj) (hash-ref obj (string->symbol key) default)]
    [else default]))

;; List items are single-line: collapse the user content's line breaks.
(define (one-line text)
  (string-replace (string-replace text "\r\n" " ") "\n" " "))

(define (iso-day)
  (define now (current-date))
  (define (two n)
    (if (< n 10)
        (string-append "0" (number->string n))
        (number->string n)))
  (string-append (number->string (date-year now))
                 "-"
                 (two (date-month now))
                 "-"
                 (two (date-day now))))

(define (annotation-less? a b)
  (define pa (doc-field a "page" 1))
  (define pb (doc-field b "page" 1))
  (if (equal? pa pb)
      (< (doc-field a "created" 0) (doc-field b "created" 0))
      (< pa pb)))

(define (document-bytes->markdown data title)
  (define value
    (with-handlers
        ([exn:fail?
          (lambda (e)
            (raise (exn:fail (tf "backend.error.parse-annotations" (exn-message e))
                             (current-continuation-marks))))])
      (read-json (open-input-bytes data))))
  (define doc (normalize-document-value value))
  (define annotations
    (sort (doc-field doc "annotations" '()) annotation-less?))
  (define bookmarks
    (sort (doc-field doc "bookmarks" '())
          (lambda (a b) (< (doc-field a "page" 1) (doc-field b "page" 1)))))
  (define lines
    (list (string-append "# " (tf "export.digest-title" title))
          (string-append "> " (tf "export.meta" (iso-day)))))
  (define annotation-lines
    (apply append
           (for/list ([anno (in-list annotations)])
             (define page (doc-field anno "page" 1))
             (define kind (doc-field anno "kind" "highlight"))
             (define excerpt (one-line (doc-field anno "excerpt" "")))
             (define note (one-line (doc-field anno "note" "")))
             (define heading
               (string-append "## "
                              (tf "export.page-heading"
                                  (number->string page))))
             (define bullet
               (string-append
                "- **" (tf (string-append "export.kind." kind)) "**"
                (if (string=? excerpt "")
                    ""
                    (string-append " 「" excerpt "」"))))
             (if (string=? note "")
                 (list "" heading bullet)
                 (list "" heading bullet
                       (string-append "  - " (tf "export.note-line" note)))))))
  (define bookmark-lines
    (if (null? bookmarks)
        '()
        (cons (string-append "## " (tf "export.bookmarks-heading"))
              (for/list ([mark (in-list bookmarks)])
                (define page (doc-field mark "page" 1))
                (define label (one-line (doc-field mark "label" "")))
                (string-append "- "
                               (tf "export.page-heading"
                                   (number->string page))
                               (if (string=? label "")
                                   ""
                                   (string-append " · " label)))))))
  (define body
    (if (and (null? annotations) (null? bookmarks))
        (list "" (tf "export.empty"))
        (append annotation-lines (cons "" bookmark-lines))))
  (string-join (append lines body) "\n"))
