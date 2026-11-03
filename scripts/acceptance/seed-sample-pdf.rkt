#lang racket/base

;; Deterministic two-page PDF fixture for the packaged close-guard journey:
;; the host opens it, rotates a page in memory, and the unsaved-edits dialog
;; must appear before close/quit. The file is plain PDF 1.4 with an exact
;; xref table, no compression, no external dependencies.

(require racket/string
         racket/format
         racket/file
         racket/path)

(module+ main
  (define target
    (path->string (path->complete-path (vector-ref (current-command-line-arguments) 0))))
  (define (stream-obj text)
    (string->bytes/utf-8
     (format "<< /Length ~a >>\nstream\nBT /F1 24 Tf 72 720 Td (~a) Tj ET\nendstream"
             (+ 40 (string-length text)) text)))
  (define header (string->bytes/utf-8 "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n"))
  (define objects
    (list
     ;; 1 catalog
     (string->bytes/utf-8 "<< /Type /Catalog /Pages 2 0 R >>")
     ;; 2 pages
     (string->bytes/utf-8 "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>")
     ;; 3 page one
     (string->bytes/utf-8
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 5 0 R /Resources << /Font << /F1 6 0 R >> >> >>")
     ;; 4 page two
     (string->bytes/utf-8
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 7 0 R /Resources << /Font << /F1 6 0 R >> >> >>")
     ;; 5 content one
     (stream-obj "Booklight page one")
     ;; 6 font
     (string->bytes/utf-8 "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
     ;; 7 content two
     (stream-obj "Booklight page two")))
  (define out (open-output-bytes))
  (write-bytes header out)
  (define offsets
    (for/list ([obj (in-list objects)]
               [n (in-naturals 1)])
      (define offset (bytes-length (get-output-bytes out)))
      (fprintf out "~a 0 obj\n" n)
      (write-bytes obj out)
      (fprintf out "\nendobj\n")
      offset))
  (define xref-offset (bytes-length (get-output-bytes out)))
  (fprintf out "xref\n0 ~a\n" (add1 (length objects)))
  (fprintf out "0000000000 65535 f \n")
  (for ([offset (in-list offsets)])
    (fprintf out "~a 00000 n \n" (~r offset #:min-width 10 #:pad-string "0")))
  (fprintf out "trailer\n<< /Size ~a /Root 1 0 R >>\nstartxref\n~a\n%%EOF\n"
           (add1 (length objects)) xref-offset)
  (define data (get-output-bytes out))
  (make-parent-directory* (string->path target))
  (call-with-output-file (string->path target)
    (lambda (port) (write-bytes data port))
    #:exists 'truncate/replace)
  (displayln target))
