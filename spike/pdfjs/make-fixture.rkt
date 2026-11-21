#lang racket/base
;; Generates spike/pdfjs/fixture.pdf: two US-Letter pages with three lines of
;; Helvetica text each (base-14 font, uncompressed streams) so pdf.js builds a
;; real text layer from embedded-standard fonts (vendored standard_fonts).
;; Hand-built xref keeps the fixture dependency-free; the domain core parses
;; it in the host's edited-pdf route (parse-pdf + pdf-rotate-pages!).
;; Run: racket spike/pdfjs/make-fixture.rkt
(require racket/file
         racket/format
         racket/runtime-path
         racket/string)

(define-runtime-path out-path "fixture.pdf")

(define (stream-obj n text)
  (define lines
    (string-append "BT /F1 12 Tf 72 720 Td 16 TL\n"
                   (string-join
                    (for/list ([line (in-list (string-split text "\n"))])
                      (format "(~a) Tj T*" line))
                    "\n")
                   "\nET"))
  (format "~a 0 obj\n<< /Length ~a >>\nstream\n~a\nendstream\nendobj\n"
          n (string-length lines) lines))

(define objects
  (list
   "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
   "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n"
   (string-append
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    "/Resources << /Font << /F1 7 0 R >> >> /Contents 5 0 R >>\nendobj\n")
   (string-append
    "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
    "/Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>\nendobj\n")
   (stream-obj 5 "Booklight spike fixture page one\nSelection and highlight live here\nPdf.js renders, pdfdoc edits")
   (stream-obj 6 "Booklight spike fixture page two\nRotation proof after domain edit\nPaper stays paper")
   "7 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"))

(define header "%PDF-1.4\n")
(define-values (body offsets)
  (for/fold ([acc header] [offs '()]) ([o (in-list objects)])
    (values (string-append acc o) (cons (string-length acc) offs))))
(define xref-pos (string-length body))
(define xref
  (string-append
   "xref\n0 8\n0000000000 65535 f \n"
   (string-join
    (for/list ([off (in-list (reverse offsets))])
      (define s (~a off))
      (format "~a~a 00000 n "
              (make-string (max 0 (- 10 (string-length s))) #\0) s))
    "\n")
   "\n"))
(define trailer
  (format "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n~a\n%%%%EOF\n"
          xref-pos))
(display-to-file (string->bytes/latin-1 (string-append body xref trailer))
                 out-path #:exists 'truncate/replace)
(printf "wrote ~a (~a bytes)\n" out-path (file-size out-path))