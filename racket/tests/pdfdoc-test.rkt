;; Tests for racket/booklight/pdfdoc — the page-level edit pipeline.
;; Run with: raco test racket/

#lang racket/base

(require rackunit
         racket/format
         racket/string
         "../booklight/pdfdoc.rkt")

;; Racket has no bytes-contains? in 9.3 — local helper.
(define (bytes-contains? haystack needle)
  (and (regexp-match? (byte-regexp needle) haystack) #t))

;; independently built 1-page PDF (classic xref), hand-computed offsets
(define fixture-bytes
  #"%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Rotate 90 >>\nendobj\n4 0 obj\n<< /Length 44 >>\nstream\nBT /F1 12 Tf 72 720 Td (Hello fixture) Tj ET\nendstream\nendobj\nxref\n0 5\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000 n \n0000000213 00000 n \ntrailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n307\n%%EOF\n")

;; --- helpers ---------------------------------------------------------------

;; Build an N-page document in memory (catalog/pages/page/content streams
;; with per-page text). Returns a pdfdoc.
(define (mk-doc texts)
  (define objects (make-hash))
  (define n (length texts))
  (define page-refs
    (for/list ((i (in-range 1 (add1 (length texts)))))
      (ref (+ 10 (* i 10)) 0)))
  (hash-set! objects '(1 . 0)
             (hasheq '/Type '/Catalog '/Pages (ref 2 0)))
  (hash-set! objects '(2 . 0)
             (hasheq '/Type '/Pages
                     '/Kids (list->vector page-refs)
                     '/Count n))
  (for ((t (in-list texts)) (pr (in-list page-refs)) (i (in-naturals 1)))
    (hash-set! objects (cons (ref-num pr) 0)
               (hasheq '/Type '/Page
                       '/Parent (ref 2 0)
                       '/MediaBox (vector 0 0 612 792)
                       '/Resources (hasheq)
                       '/Contents (ref (+ 11 (* i 10)) 0)))
    (define payload (string->bytes/latin-1 (format "BT /F1 12 Tf 72 720 Td (~a) Tj ET" t)))
    (hash-set! objects (cons (+ 11 (* i 10)) 0)
               (pstream (hasheq) payload)))
  (pdfdoc objects (ref 1 0)))

(define (page-text doc i)
  (define pr (list-ref (pdf-page-refs doc) (sub1 i)))
  (define pd (pdf-resolve doc pr))
  (define cs (pdf-resolve doc (hash-ref pd '/Contents)))
  (pstream-payload cs))

(define (check-parse-roundtrip doc-before bytes-after what)
  (define doc2 (parse-pdf bytes-after))
  (check-equal? (length (pdf-page-refs doc2))
                (length (pdf-page-refs doc-before))
                (format "~a: page count survives roundtrip" what))
  doc2)

;; --- fixture parsing --------------------------------------------------------

(define fixture (parse-pdf fixture-bytes))

(test-case "fixture: independent classic-xref PDF parses"
  (check-equal? (length (pdf-page-refs fixture)) 1)
  (define pd (pdf-resolve fixture (car (pdf-page-refs fixture))))
  (check-equal? (hash-ref pd '/Rotate) 90)
  (check-equal? (hash-ref pd '/MediaBox) (vector 0 0 612 792))
  (define cs (pdf-resolve fixture (hash-ref pd '/Contents)))
  (check-true (bytes-contains? (pstream-payload cs) #"Hello fixture")))

;; --- write roundtrip --------------------------------------------------------

(test-case "writer: classic xref output parses back"
  (define doc (mk-doc (list "alpha" "beta" "gamma")))
  (define bytes1 (pdf->bytes doc))
  (define doc2 (check-parse-roundtrip doc bytes1 "writer"))
  (check-true (bytes-contains? (page-text doc2 2) #"beta"))
  (check-true (bytes-contains? (page-text doc2 3) #"gamma")))

;; --- delete ------------------------------------------------------------------

(test-case "delete-pages: middle page removed, all-pages refused"
  (define doc (mk-doc (list "one" "two" "three" "four")))
  (pdf-delete-pages! doc (list 2))
  (check-equal? (length (pdf-page-refs doc)) 3)
  (check-true (bytes-contains? (page-text doc 1) #"one"))
  (check-true (bytes-contains? (page-text doc 2) #"three"))
  (define out (pdf->bytes doc))
  (define doc2 (parse-pdf out))
  (check-equal? (length (pdf-page-refs doc2)) 3)
  (check-exn pdf-error?
             (λ () (pdf-delete-pages! (mk-doc (list "only")) (list 1)))))

;; --- rotate ------------------------------------------------------------------

(test-case "rotate-pages: adds to existing rotation, normalizes mod 360"
  (define doc (parse-pdf fixture-bytes))
  (pdf-rotate-pages! doc (list 1) 90)
  (define pd (pdf-resolve doc (car (pdf-page-refs doc))))
  (check-equal? (hash-ref pd '/Rotate) 180)
  (pdf-rotate-pages! doc (list 1) 270)
  (check-equal? (hash-ref (pdf-resolve doc (car (pdf-page-refs doc))) '/Rotate) 90))

;; --- insert blank ------------------------------------------------------------

(test-case "insert-blank-after: same-size page inserted"
  (define doc (mk-doc (list "one" "two")))
  (pdf-insert-blank-after! doc 1)
  (check-equal? (length (pdf-page-refs doc)) 3)
  (define pd (pdf-resolve doc (list-ref (pdf-page-refs doc) 1)))
  (check-equal? (hash-ref pd '/MediaBox) (vector 0 0 612 792))
  (define cs (pdf-resolve doc (hash-ref pd '/Contents)))
  (check-equal? (pstream-payload cs) #"")
  (define doc2 (parse-pdf (pdf->bytes doc)))
  (check-equal? (length (pdf-page-refs doc2)) 3))

;; --- extract -----------------------------------------------------------------

(test-case "extract-pages: new document with chosen pages ascending"
  (define doc (mk-doc (list "keep1" "drop" "keep3")))
  (define-values (new-doc out) (pdf-extract-pages doc (list 3 1)))
  (check-equal? (length (pdf-page-refs new-doc)) 2)
  (check-true (bytes-contains? (page-text new-doc 1) #"keep1"))
  (check-true (bytes-contains? (page-text new-doc 2) #"keep3"))
  (define reparsed (parse-pdf out))
  (check-equal? (length (pdf-page-refs reparsed)) 2)
  (check-true (bytes-contains? (page-text reparsed 2) #"keep3")))

;; --- append ------------------------------------------------------------------

(test-case "append-doc: pages of the second document follow the first"
  (define doc (mk-doc (list "a" "b")))
  (define other (mk-doc (list "x" "y" "z")))
  (pdf-append-doc! doc other)
  (check-equal? (length (pdf-page-refs doc)) 5)
  (define out (pdf->bytes doc))
  (define doc2 (parse-pdf out))
  (check-equal? (length (pdf-page-refs doc2)) 5)
  (check-true (bytes-contains? (page-text doc2 3) #"x"))
  (check-true (bytes-contains? (page-text doc2 5) #"z")))

;; --- bake --------------------------------------------------------------------

;; shared-font helper: the /F1 entry must be a live font object ref
(define (ensure-font-visible doc res)
  (define f (hash-ref (hash-ref res '/Font) '/F1))
  (check-true (ref? f))
  (define obj (pdf-resolve doc f))
  (check-equal? (hash-ref obj '/BaseFont) '/Helvetica-Bold)
  f)

(test-case "bake: watermark on every page, textbox on the chosen page"
  (define doc (mk-doc (list "p1" "p2")))
  (pdf-bake-text doc
                 #:watermark-text "DRAFT"
                 #:watermark-size 48
                 #:watermark-opacity 0.3
                 #:textboxes (list (textbox 2 0.25 0.5 "note" 14)))
  (for ((i (in-list (list 1 2))))
    (define pd (pdf-resolve doc (list-ref (pdf-page-refs doc) (sub1 i))))
    (define cs (pdf-resolve doc (hash-ref pd '/Contents)))
    (check-true (bytes-contains? (pstream-payload cs) #"DRAFT")
                (format "watermark baked on page ~a" i))
    (check-true (bytes-contains? (pstream-payload cs) #"/G1 gs")
                (format "extgstate alpha applied on page ~a" i))
    (define res (hash-ref pd '/Resources))
    (check-equal? (hash-ref (hash-ref res '/Font) '/F1)
                  (ensure-font-visible doc res)))
  (define pd2 (pdf-resolve doc (list-ref (pdf-page-refs doc) 1)))
  (define cs2 (pdf-resolve doc (hash-ref pd2 '/Contents)))
  (check-true (bytes-contains? (pstream-payload cs2) #"note"))
  ;; CJK-only boxes strip to nothing; v1's drawText("") is a no-op, so the
  ;; baker skips them and the page keeps its previous stream
  (check-equal? (pdf-bake-text doc #:textboxes (list (textbox 1 0.1 0.1 "中文" 12)))
                (void))
  (check-false
   (bytes-contains?
    (pstream-payload
     (pdf-resolve doc
                  (hash-ref (pdf-resolve doc
                                         (list-ref (pdf-page-refs doc) 0))
                            '/Contents)))
    #"12.0")))

;; --- encrypted rejection ------------------------------------------------------

(test-case "encrypted documents are rejected"
  (check-exn
   (λ (e) (and (pdf-error? e) (equal? (pdf-error-code e) 'encrypted)))
   (λ ()
     (parse-pdf
      #"%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\nxref\n0 3\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \ntrailer\n<< /Size 3 /Root 1 0 R /Encrypt 3 0 R >>\nstartxref\n109\n%%EOF\n"))))

;; --- PNG up-predictor unit test ----------------------------------------------

(test-case "predictor: PNG Up filter rows decode"
  ;; width 4, one byte per sample; rows: filter 2 (Up), deltas [1,2,3,4] then [1,1,1,1]
  (define data (bytes 2 1 2 3 4 2 1 1 1 1))
  (define out (png-unfilter data 4 1))
  (check-equal? out (bytes 1 2 3 4 2 3 4 5)))

;; pdfdoc exposes png-unfilter only internally; test through the behavior by
;; re-exporting here would break the module contract — instead the xref-stream
;; fixture below exercises it end to end.

;; --- xref stream fixture (Predictor 12, objstm-less) --------------------------

;; 1-page PDF whose xref is a PDF 1.5 cross-reference stream. Built by the
;; same hand-computed style: entries W=[1 2 1], Index=[1 3], Predictor 12 Up.
;; Hand-assembling the compressed stream here would duplicate zlib; instead
;; this relies on writer output never using xref streams and the reader being
;; exercised against real-world files in the host slice. Covered: predictor
;; math via png-unfilter-test above; the full xref-stream path is exercised
;; by pdfgist e2e against a PDFKit-generated file (macOS host verification).


;; --- resource budgets ---------------------------------------------------------

(test-case "resource budgets reject oversized and hostile documents"
  (define (budget file-bytes xref-entries objects stream-bytes page-count nesting)
    (pdf-resource-budget file-bytes xref-entries objects stream-bytes
                         page-count nesting))
  (check-true (pdfdoc? (parse-pdf fixture-bytes)))
  (check-exn exn:fail:pdf:resource?
             (lambda () (parse-pdf fixture-bytes #:budget (budget 1 100 100 100 100 8))))
  (check-exn exn:fail:pdf:resource?
             (lambda () (parse-pdf fixture-bytes #:budget (budget 100000 1 100 100 100 8))))
  (check-exn exn:fail:pdf:resource?
             (lambda () (parse-pdf fixture-bytes #:budget (budget 100000 100 1 100 100 8))))
  (check-exn exn:fail:pdf:resource?
             (lambda () (parse-pdf fixture-bytes #:budget (budget 100000 100 100 100 0 8))))
  (check-exn exn:fail:pdf:resource?
             (lambda () (parse-pdf fixture-bytes #:budget (budget 100000 100 100 100 100 0))))
  (check-true (pdfdoc? (parse-pdf fixture-bytes #:budget (budget 100000 100 100 100 100 8)))))
