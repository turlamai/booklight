#lang racket/base
;; Glaze carrier for the pdf.js spike (#75 S0). Static spike dir + two typed
;; report routes + one raw route serving PDF bytes edited by the KEPT domain
;; core (parse-pdf + pdf-rotate-pages!): pdf.js stays render-only, page
;; surgery never moves into the web layer. Dev entry: racket spike/pdfjs/host.rkt
;; Real-window evidence: racket spike/pdfjs/verify.rkt
(require glaze/app
         glaze/api
         glaze/api-macros
         glaze/types
         racket/file
         racket/runtime-path
         web-server/http/response-structs
         (only-in web-server/http/request-structs header)
         (only-in "../../racket/booklight/pdfdoc.rkt"
                  parse-pdf pdf-rotate-pages! pdf->bytes))

(define-runtime-path spike-dir ".")
(define-runtime-path fixture-path "fixture.pdf")

(provide spike-dir api)

(define report-box (box #f))

(define-api-routes api-typed
  [(POST "api/spike/report" #:returns GBool)
   (spikeReport [payload GValue])
   (set-box! report-box payload)
   #t]
  [(POST "api/spike/report-get" #:returns GValue)
   (spikeReportGet)
   (or (unbox report-box) (hasheq 'phase "waiting"))])

;; The edited bytes are produced in-memory by the domain core on every
;; request: rotate page 1 by 90 degrees, exactly the PageEditMenu operation
;; the production PDF surface will drive through the same module.
(define (edited-pdf-response req)
  (define doc (parse-pdf (file->bytes fixture-path)))
  (pdf-rotate-pages! doc (list 1) 90)
  (define bytes (pdf->bytes doc))
  (response/full 200 #"OK" (current-seconds)
                 #"application/pdf"
                 (list (header #"Content-Length"
                               (string->bytes/latin-1
                                (format "~a" (bytes-length bytes))))
                       (header #"Cache-Control" #"no-store"))
                 (list bytes)))

(define api
  (append api-typed (list (GET "api/spike/edited-pdf" edited-pdf-response))))

(module+ main
  (define-values (_kind stop)
    (run-app
     #:public-dir spike-dir
     #:api api
     #:transport 'http
     #:api-token #t
     #:title "Booklight pdf.js spike"
     #:width 1100
     #:height 800
     #:app-id "booklight-spike"))
  (stop))