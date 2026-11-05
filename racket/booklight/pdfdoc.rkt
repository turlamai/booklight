;; Minimal PDF reader/writer for page-level operations (delete, insert,
;; rotate, extract, append) and text baking (watermark, text boxes).
;;
;; Object model:
;;   number -> number          name   -> symbol
;;   string -> bytes           array  -> vector
;;   dict   -> immutable eq? hash keyed by symbol
;;   stream -> (pstream dict payload-bytes)
;;   ref    -> (ref num gen)
;;
;; Reading walks the xref chain (classic tables and PDF 1.5 xref streams,
;; including object streams and PNG predictors). Encrypted documents are
;; rejected — string/stream decryption is out of scope for the edit
;; pipeline (the viewer handles unlock separately).
;;
;; Baking draws with base-14 Helvetica only; non-latin characters are
;; stripped (v1's no-CJK-font fallback). Text widths are approximated.
;; CJK baking needs a TTF-embedding slice (issue #1 follow-up).

#lang racket/base

(require ffi/unsafe
         ffi/unsafe/define
         racket/format
         racket/list
         racket/match
         racket/bytes
         racket/port
         racket/set
         racket/string)

(provide (struct-out pdf-error)
         (struct-out exn:fail:pdf:resource)
         (struct-out pdf-resource-budget)
         default-pdf-resource-budget
         (struct-out ref)
         (struct-out pstream)
         (struct-out pdfdoc)
         parse-pdf
         pdf-resolve
         png-unfilter
         pdf-page-refs
         pdf-delete-pages!
         pdf-rotate-pages!
         pdf-insert-blank-after!
         pdf-extract-pages
         pdf-append-doc!
         pdf-bake-text
         (struct-out textbox)
         pdf->bytes)

(struct pdf-error (code message) #:transparent)
(struct ref (num gen) #:transparent)
(struct pstream (dict payload) #:transparent)
(struct pdfdoc (objects root-ref) #:transparent)

;; Hostile or damaged PDFs are bounded before they can exhaust memory or the
;; stack. Limits are deliberately far above ordinary documents (a 1000-page
;; scan is still tiny) while stopping xref storms, decompression bombs and
;; cyclic page/Length references.
(struct exn:fail:pdf:resource exn:fail (code limit actual) #:transparent)
(struct pdf-resource-budget
  (file-bytes xref-entries objects stream-bytes page-count nesting-depth)
  #:transparent
  #:guard
  (lambda (file-bytes xref-entries objects stream-bytes page-count nesting-depth _)
    (for ([value (in-list (list file-bytes xref-entries objects stream-bytes
                                page-count nesting-depth))])
      (unless (exact-nonnegative-integer? value)
        (raise-argument-error 'pdf-resource-budget
                              "exact-nonnegative-integer?" value)))
    (values file-bytes xref-entries objects stream-bytes page-count nesting-depth)))

(define default-pdf-resource-budget
  (pdf-resource-budget (* 256 1024 1024) ; file bytes
                       2000000          ; xref entries
                       2000000          ; materialized objects
                       (* 1024 1024 1024) ; expanded stream bytes
                       100000           ; pages
                       64))             ; tree/reference nesting

(define (pdf-resource-error code limit actual)
  (raise
   (exn:fail:pdf:resource
    (format "PDF_RESOURCE_LIMIT[~a]: limit ~a, observed at least ~a"
            code limit actual)
    (current-continuation-marks)
    code limit actual)))

(define current-pdf-expanded-used (make-parameter #f))
(define current-pdf-expanded-limit (make-parameter #f))
(define current-pdf-nesting-limit
  (make-parameter (pdf-resource-budget-nesting-depth default-pdf-resource-budget)))

;; ------------------------------------------------------------------ zlib

;; Windows names the official binary zlib1.dll, and ffi-lib joins variants
;; as libz-<variant>.dll there, so the unix/mac variant list can never reach
;; it — the platform picks the library form.
(define zlib-ffi-lib
  (if (eq? (system-type) 'windows)
      (ffi-lib "zlib1" (list #f))
      (ffi-lib "libz" '("1" "2" #f "1.2" "libz" "zlib1"))))

(define-ffi-definer define-zlib zlib-ffi-lib)

(define _zulong _ulong)
(define-zlib uncompress
  (_fun (dest : (_bytes o dest-len)) (dest-len : (_ptr i _zulong))
        (src : _bytes) (src-len : _zulong)
        -> (rc : _int)
        -> (values rc dest dest-len)))

;; Inflate a zlib (RFC 1950) stream. Buffer grows until Z_OK (0).
(define (zlib-inflate data)
  (define len (bytes-length data))
  (define (accept out)
    (define used (current-pdf-expanded-used))
    (define limit (current-pdf-expanded-limit))
    (when (and used limit)
      (define next (+ (unbox used) (bytes-length out)))
      (when (> next limit)
        (pdf-resource-error 'stream-bytes limit next))
      (set-box! used next))
    out)
  (let loop ((cap (max 4096 (* 4 len))))
    (define-values (rc out used) (uncompress cap data len))
    (cond
      ((= rc 0) (accept out))
      ((= rc -5) ; Z_BUF_ERROR: try a bigger buffer
       (if (> cap (expt 2 30))
           (raise (pdf-error 'parse "zlib output too large"))
           (loop (* cap 2))))
      (else (raise (pdf-error 'parse "zlib stream error"))))))

;; Undo a PNG predictor (type 12 Up and friends) on decoded stream bytes.
;; row-width = ceil(bits-per-column * colors / 8); xref streams use 8-bit
;; components, so bpp == colors.
(define (png-unfilter data row-width colors)
  (define bpp (max 1 colors))
  (define stride (add1 row-width)) ; filter byte + row
  (unless (= (remainder (bytes-length data) stride) 0)
    (raise (pdf-error 'parse "bad predictor stream length")))
  (define rows (quotient (bytes-length data) stride))
  (define out (make-bytes (* rows row-width) 0))
  (for ((r (in-range rows)))
    (define base (* r stride))
    (define obase (* r row-width))
    (define filter (bytes-ref data base))
    (for ((c (in-range row-width)))
      (define raw (bytes-ref data (+ base c 1)))
      (define left (if (>= c bpp) (bytes-ref out (+ obase (- c bpp))) 0))
      (define up (if (> r 0) (bytes-ref out (+ obase (- row-width) c)) 0))
      (define ul (if (and (> r 0) (>= c bpp))
                     (bytes-ref out (+ obase (- row-width) (- c bpp)))
                     0))
      (bytes-set! out (+ obase c)
                  (case filter
                    ((0) raw)
                    ((1) (bitwise-and (+ raw left) 255))
                    ((2) (bitwise-and (+ raw up) 255))
                    ((3) (bitwise-and (+ raw (quotient (+ left up) 2)) 255))
                    ((4)
                     (define p (+ left up (- ul)))
                     (define pa (abs (- p left)))
                     (define pb (abs (- p up)))
                     (define pc (abs (- p ul)))
                     (define pred (cond ((and (<= pa pb) (<= pa pc)) left)
                                        ((<= pb pc) up)
                                        (else ul)))
                     (bitwise-and (+ raw pred) 255))
                    (else (raise (pdf-error 'parse "bad filter type")))))))
  out)

;; ------------------------------------------------------------------ lexer

(struct lex (port) #:transparent)

(define (lex-peek l (off 0))
  (define b (peek-byte (lex-port l) off))
  (if (eof-object? b) #f b))
(define (lex-read l) (read-byte (lex-port l)))

(define delimiters (list 40 41 60 62 91 93 123 125 47 37))

(define (skip-ws! l)
  (let loop ()
    (define b (lex-peek l))
    (when (and b (<= b 32))
      (lex-read l)
      (loop)))
  (when (equal? (lex-peek l) 37) ; % comment
    (let loop ()
      (define b (lex-read l))
      (when (and b (not (memv b (list 10 13)))) (loop)))
    (skip-ws! l)))

;; Consume `kw` if the input matches it followed by a delimiter/whitespace.
(define (keyword? l kw)
  (define len (bytes-length kw))
  (define buf (make-bytes len))
  (define n (peek-bytes! buf 0 (lex-port l)))
  (and (equal? n len)
       (bytes=? buf kw)
       (let ((nxt (lex-peek l len)))
         (or (not nxt) (<= nxt 32) (memv nxt delimiters))
         (begin (for ((i (in-range len))) (lex-read l)) #t))))

(define (digit? b) (and b (<= 48 b 57)))

(define (read-number! l)
  (define buf (open-output-bytes))
  (let loop ()
    (define b (lex-peek l))
    (when (and b (or (digit? b) (memv b (list 43 45 46))))
      (write-byte (lex-read l) buf)
      (loop)))
  (define s (bytes->string/latin-1 (get-output-bytes buf)))
  (cond
    ((string=? s "") (raise (pdf-error 'parse "expected number")))
    ((string-contains? s ".") (or (string->number s)
                                  (raise (pdf-error 'parse (format "bad number ~a" s)))))
    (else (define n (string->number s))
          (or n (raise (pdf-error 'parse (format "bad number ~a" s)))))))

(define (read-literal-string! l)
  (lex-read l) ; (
  (define buf (open-output-bytes))
  (let loop ((depth 0))
    (define b (lex-read l))
    (cond
      ((not b) (raise (pdf-error 'parse "unterminated string")))
      ((equal? b 40) (write-byte b buf) (loop (add1 depth)))
      ((equal? b 41)
       (if (zero? depth)
           (get-output-bytes buf)
           (begin (write-byte b buf) (loop (sub1 depth)))))
      ((equal? b 92)
       (define e (lex-read l))
       (cond
         ((not e) (raise (pdf-error 'parse "unterminated string")))
         ((equal? e 110) (write-byte 10 buf) (loop depth))  ; \n
         ((equal? e 114) (write-byte 13 buf) (loop depth))  ; \r
         ((equal? e 116) (write-byte 9 buf) (loop depth))   ; \t
         ((equal? e 98) (write-byte 8 buf) (loop depth))    ; \b
         ((equal? e 102) (write-byte 12 buf) (loop depth))  ; \f
         ((equal? e 40) (write-byte 40 buf) (loop depth))
         ((equal? e 41) (write-byte 41 buf) (loop depth))
         ((equal? e 92) (write-byte 92 buf) (loop depth))
         ((digit? e)
          (define digits
            (let more ((acc (list e)) (count 1))
              (define b2 (lex-peek l))
              (if (and (digit? b2) (< count 3))
                  (more (cons (lex-read l) acc) (add1 count))
                  (reverse acc))))
          (write-byte
           (for/fold ((acc 0)) ((d (in-list digits))) (+ (* acc 8) (- d 48)))
           buf)
          (loop depth))
         ((memv e (list 10 13))
          ;; line continuation: \<LF> or \<CR><LF>
          (when (and (equal? e 13) (equal? (lex-peek l) 10)) (lex-read l))
          (loop depth))
         (else (write-byte e buf) (loop depth))))
      (else (write-byte b buf) (loop depth)))))

(define (hexval b)
  (cond ((and b (<= 48 b 57)) (- b 48))
        ((and b (<= 97 b 102)) (- b 87))
        ((and b (<= 65 b 70)) (- b 55))
        (else #f)))

(define (read-hex-string! l)
  (lex-read l) ; <
  (define buf (open-output-bytes))
  (let loop ((nib '()))
    (define b (lex-read l))
    (cond
      ((not b) (raise (pdf-error 'parse "unterminated hex string")))
      ((equal? b 62)
       (unless (null? nib)
         ;; nib holds converted nibble values (0-15)
         (write-byte
          (if (= (length nib) 2)
              (+ (* (car nib) 16) (cadr nib))
              (* (car nib) 16))
          buf))
       (get-output-bytes buf))
      ((memv b (list 10 13 32 9 0 12)) (loop nib))
      (else
       (define v (hexval b))
       (unless v (raise (pdf-error 'parse "bad hex digit")))
       (if (= (length nib) 1)
           (begin (write-byte (+ (* (car nib) 16) v) buf) (loop '()))
           (loop (list v)))))))

(define (read-name! l)
  ;; consume the leading '/', scan the name, return it slash-prefixed
  ;; ('/Root) to match the dict keys used across the module
  (lex-read l) ; /
  (define buf (open-output-bytes))
  (let loop ()
    (define b (lex-peek l))
    (cond
      ((not b) (void))
      ((<= b 32) (void))
      ((memv b delimiters) (void))
      ((equal? b 35)
       (lex-read l)
       (define h1 (lex-read l))
       (define h2 (lex-read l))
       (write-byte (+ (* (hexval h1) 16) (hexval h2)) buf)
       (loop))
      (else (write-byte (lex-read l) buf) (loop))))
  (string->symbol
   (string-append "/" (bytes->string/utf-8 (get-output-bytes buf)))))

(define (read-dict! l)
  (skip-ws! l)
  (unless (keyword? l #"<<") (raise (pdf-error 'parse "expected <<")))
  (let loop ((acc (hasheq)))
    (skip-ws! l)
    (cond
      ((keyword? l #">>") acc)
      ((equal? (lex-peek l) 47)
       (define key (read-name! l))
       (skip-ws! l)
       (define v
         (cond
           ((keyword? l #">>") (hasheq)) ; name key with immediate close? dict value expected; treat as empty
           (else (read-obj! l))))
       (loop (hash-set acc key v)))
      (else (raise (pdf-error 'parse (format "bad dict entry at offset ~a"
                                             (file-position (lex-port l)))))))))

(define (read-array! l)
  (lex-read l) ; [
  (let loop ((acc '()))
    (skip-ws! l)
    (cond
      ((equal? (lex-peek l) 93)
       (lex-read l) ; consume ]
       (list->vector (reverse acc)))
      (else (loop (cons (read-obj! l) acc))))))

(define (read-obj! l)
  (skip-ws! l)
  (define b (lex-peek l))
  (cond
    ((not b) (raise (pdf-error 'parse "unexpected EOF")))
    ((equal? b 47) (read-name! l))
    ((equal? b 91) (read-array! l))
    ((equal? b 40) (read-literal-string! l))
    ((equal? b 60)
     (if (equal? (lex-peek l 1) 60)
         (read-dict! l)
         (read-hex-string! l)))
    ((equal? b 116)
     (if (keyword? l #"true") #t (raise (pdf-error 'parse "unexpected token (t)"))))
    ((equal? b 102)
     (if (keyword? l #"false") #f (raise (pdf-error 'parse "unexpected token (f)"))))
    ((equal? b 110)
     (if (keyword? l #"null") 'null (raise (pdf-error 'parse "unexpected token (n)"))))
    (else
     ;; number or "N G R" indirect ref — with pushback: restore the port
     ;; when the R doesn't follow, so "5 0" in an array parses as two
     ;; numbers and "/Size 5 /Root ..." doesn't swallow the next key.
     (define v (read-number! l))
     (skip-ws! l)
     (if (and (exact? v) (digit? (lex-peek l)))
         (let ((g-start (file-position (lex-port l))))
           (define g (read-number! l))
           (skip-ws! l)
           (cond
             ((equal? (lex-peek l) 82)
              (lex-read l)
              (ref v (inexact->exact g)))
             (else
              (file-position (lex-port l) g-start)
              v)))
         v))))

;; -------------------------------------------------------------- parsing

;; Read whole document: xref chain walk -> object offset table -> parse.
(define (parse-pdf data #:budget [budget default-pdf-resource-budget])
  (define file-bytes (bytes-length data))
  (when (> file-bytes (pdf-resource-budget-file-bytes budget))
    (pdf-resource-error 'file-bytes (pdf-resource-budget-file-bytes budget) file-bytes))
  (parameterize ([current-pdf-expanded-used (box 0)]
                 [current-pdf-expanded-limit (pdf-resource-budget-stream-bytes budget)]
                 [current-pdf-nesting-limit (pdf-resource-budget-nesting-depth budget)])
    (define-values (entries trailer) (parse-xref-chain data budget))
    (parse-pdf-with-entries data entries trailer budget)))

(define (parse-pdf-with-entries data entries trailer budget)
  (define objects (make-hash))             ; (cons num gen) -> obj
  (define object-limit (pdf-resource-budget-objects budget))
  (define root-ref (dict-get trailer '/Root))
  (unless (ref? root-ref) (raise (pdf-error 'parse "no /Root in trailer")))
  (when (dict-get trailer '/Encrypt)
    (raise (pdf-error 'encrypted "encrypted PDFs are not supported for editing")))
  ;; materialize every object (skip free entries)
  (for (((num info) (in-hash entries)))
    (match-define (list gen offset kind) info)
    (cond
      ((eq? kind 'free) (void))
      ((eq? kind 'objstm)
       (void)) ; handled below, batched per object stream
      (else
       (hash-set! objects (cons num gen)
                  (parse-object-at data offset num gen objects))
       (when (> (hash-count objects) object-limit)
         (pdf-resource-error 'objects object-limit (hash-count objects))))))
  ;; object streams: group type-2 entries by stream object number
  (define by-stm (make-hash)) ; stm-num -> list of (num gen)
  (for (((num info) (in-hash entries)))
    (match-define (list gen offset kind) info)
    (when (eq? kind 'objstm)
      (hash-update! by-stm offset (λ (l) (cons (list num gen) l)) '())))
  (for (((stm-num pairs) (in-hash by-stm)))
    (define stm-info (hash-ref entries stm-num #f))
    (unless stm-info (raise (pdf-error 'parse "object stream missing from xref")))
    (match-define (list gen offset kind) stm-info)
    (define stm (parse-object-at data offset stm-num 0 objects))
    (unless (pstream? stm) (raise (pdf-error 'parse "ObjStm is not a stream")))
    (define payload
      (cond
        ((equal? (dict-get (pstream-dict stm) '/Filter) '/FlateDecode)
         (zlib-inflate (pstream-payload stm)))
        (else (pstream-payload stm))))
    (define count (inexact->exact (dict-get (pstream-dict stm) '/N 0)))
    (define first-off (inexact->exact (dict-get (pstream-dict stm) '/First 0)))
    (define hl (lex (open-input-bytes payload)))
    (define index
      (for/hash ((i (in-range count)))
        (define n (read-number! hl))
        (define off (read-number! hl))
        (values n off)))
    (for ((pair (in-list pairs)))
      (match-define (list num gen) pair)
      (define off (hash-ref index num #f))
      (unless off (raise (pdf-error 'parse "object missing from ObjStm index")))
      (define op (open-input-bytes payload))
      (file-position op (+ first-off off))
      (hash-set! objects (cons num gen) (read-obj! (lex op)))
      (when (> (hash-count objects) object-limit)
        (pdf-resource-error 'objects object-limit (hash-count objects)))))
  (define doc (pdfdoc objects root-ref))
  (pdf-page-refs doc #:budget budget)
  doc)

(define (dict-get d key (default #f))
  (if (and (hash? d) (hash-has-key? d key))
      (hash-ref d key)
      default))

;; Walk /Prev chains: newest wins. Returns (values entries trailer).
(define (parse-xref-chain data budget)
  (define xref-limit (pdf-resource-budget-xref-entries budget))
  (define (check-xref!)
    (when (> (hash-count entries) xref-limit)
      (pdf-resource-error 'xref-entries xref-limit (hash-count entries))))
  (define start (find-startxref data))
  (define entries (make-hash)) ; num -> (list gen offset kind)
  (define trailer #f)
  (define seen (mutable-set))
  (let loop ((pos start))
    (when (set-member? seen pos)
      (raise (pdf-error 'parse "xref chain loop")))
    (set-add! seen pos)
    (define l (lex (open-input-bytes data)))
    (file-position (lex-port l) pos)
    (skip-ws! l)
    (cond
      ((keyword? l #"xref")
       ;; classic table: section headers, then 20-byte entries, then trailer
       (let read-sections ()
         (skip-ws! l)
         (define line (peek-line (lex-port l)))
         (define m (regexp-match #rx"^([0-9]+) ([0-9]+)[ \t\r]*$" line))
         (when m
           (define start-num (string->number (bytes->string/latin-1 (cadr m))))
           (define count (string->number (bytes->string/latin-1 (caddr m))))
           (read-line (lex-port l) 'any)
           (for ((i (in-range count)))
             (define entry (read-bytes-n (lex-port l) 20))
             (define em (regexp-match #px"^([0-9]{10}) ([0-9]{5}) ([fn])" entry))
             (unless em (raise (pdf-error 'parse "bad xref entry")))
             (define gen (string->number (bytes->string/latin-1 (caddr em))))
             (define kind (if (equal? (cadddr em) #"n") 'plain 'free))
             (define num (+ start-num i))
             (unless (hash-has-key? entries num)
               (hash-set! entries num
                          (list gen (string->number (bytes->string/latin-1 (cadr em))) kind))
               (check-xref!)))
           (read-sections)))
       ;; the trailer dict follows the entry sections
       (skip-ws! l)
       (keyword? l #"trailer")
       (unless trailer (set! trailer (read-obj! l))))
      (else
       ;; xref stream object
       (skip-ws! l)
       (define num (read-number! l))
       (skip-ws! l)
       (define gen (read-number! l))
       (skip-ws! l)
       (unless (keyword? l #"obj") (raise (pdf-error 'parse "expected xref stream")))
       (skip-ws! l)
       (define dict (read-obj! l))
       (unless (hash? dict) (raise (pdf-error 'parse "xref stream must be a dict")))
       (skip-ws! l)
       (unless (keyword? l #"stream") (raise (pdf-error 'parse "expected stream")))
       (skip-eol! l)
       (define raw (read-bytes-n (lex-port l) (inexact->exact (dict-get dict '/Length 0))))
       (define payload
         (cond
           ((equal? (dict-get dict '/Filter) '/FlateDecode) (zlib-inflate raw))
           (else (raise (pdf-error 'parse "unsupported xref stream filter")))))
       (define payload2
         (let ((pred (dict-get dict '/DecodeParms)))
           (define p (cond ((hash? pred) pred)
                           ((and (vector? pred) (= (vector-length pred) 1) (hash? (vector-ref pred 0)))
                            (vector-ref pred 0))
                           (else #f)))
           (define ptype (if p (dict-get p '/Predictor 1) 1))
           (cond
             ((>= ptype 10) ; PNG predictors: 12 (Up) is what writers emit
              (define colors (dict-get p '/Colors 1))
              (define bpc (dict-get p '/BitsPerComponent 8))
              (unless (= bpc 8)
                (raise (pdf-error 'parse "unsupported predictor bit depth")))
              (define columns (dict-get p '/Columns 1))
              (png-unfilter payload columns colors))
             ((= ptype 1) payload)
             (else (raise (pdf-error 'parse "unsupported predictor"))))))
       (parse-xref-stream-entries! entries dict payload2)
       (check-xref!)
       (unless trailer (set! trailer dict))))
    (define prev (dict-get trailer '/Prev))
    (cond
      ((number? prev) (loop (inexact->exact prev)))
      (else (void))))
  (values entries trailer))


(define (parse-xref-stream-entries! entries dict payload)
  (define w (dict-get dict '/W))
  (unless (and (vector? w) (= (vector-length w) 3))
    (raise (pdf-error 'parse "bad /W in xref stream")))
  (define w1 (inexact->exact (vector-ref w 0)))
  (define w2 (inexact->exact (vector-ref w 1)))
  (define w3 (inexact->exact (vector-ref w 2)))
  (define wlen (+ w1 w2 w3))
  (define idx (dict-get dict '/Index))
  (define ranges
    (if (vector? idx)
        (for/list ((i (in-range 0 (vector-length idx) 2)))
          (cons (inexact->exact (vector-ref idx i))
                (inexact->exact (vector-ref idx (add1 i)))))
        (list (cons 0 (inexact->exact (dict-get dict '/Size 0))))))
  (define total (for/sum ((r (in-list ranges))) (cdr r)))
  (unless (>= (bytes-length payload) (* total wlen))
    (raise (pdf-error 'parse "xref stream data too short")))
  (let loop ((pos 0) (r (car ranges)) (remaining (cdr ranges)) (k 0))
    (when (< k (cdr r))
      (define type (if (= w1 0) 1 (read-wint payload pos w1)))
      (define f2 (read-wint payload (+ pos w1) w2))
      (define f3 (read-wint payload (+ pos w1 w2) w3))
      (define num (+ (car r) k))
      (unless (hash-has-key? entries num)
        (hash-set! entries num
                   (case type
                     ((0) (list 0 0 'free))
                     ((1) (list f3 f2 'plain))
                     ((2) (list 0 f2 'objstm)) ; f2=stm num, f3=index
                     (else (list 0 0 'free)))))
      (loop (+ pos wlen) r remaining (add1 k)))
    (unless (null? remaining)
      (loop pos (car remaining) (cdr remaining) 0))))

(define (read-wint b off width)
  (for/fold ((acc 0)) ((i (in-range width)))
    (+ (* acc 256)
       (let ((v (if (< (+ off i) (bytes-length b)) (bytes-ref b (+ off i)) 0)))
         v))))

(define (find-startxref data)
  ;; regexp-match-positions* drops groups; loop with an offset instead
  (let loop ((from 0) (best #f))
    (define m (regexp-match-positions #rx"startxref[ \t\r\n]+([0-9]+)" data from))
    (cond
      (m
       (define pair (list-ref m 1))
       (loop (cdr pair) pair))
      (best
       (string->number
        (bytes->string/latin-1 (subbytes data (car best) (cdr best)))))
      (else (raise (pdf-error 'parse "no startxref marker"))))))

(define (parse-object-at data offset num gen objects (depth 0))
  (define nesting-limit (current-pdf-nesting-limit))
  (when (> depth nesting-limit)
    (pdf-resource-error 'nesting-depth nesting-limit depth))
  (define l (lex (open-input-bytes data)))
  (file-position (lex-port l) offset)
  (skip-ws! l)
  (define n (read-number! l))
  (skip-ws! l)
  (define g (read-number! l))
  (skip-ws! l)
  (unless (and (= n num) (= g gen) (keyword? l #"obj"))
    (raise (pdf-error 'parse (format "bad object header at ~a (want ~a ~a)" offset num gen))))
  (skip-ws! l)
  (define obj (read-obj! l))
  (skip-ws! l)
  (cond
    ((and (hash? obj) (keyword? l #"stream"))
     (skip-eol! l)
     (define lenv (dict-get obj '/Length))
     (define len
       (cond
         ((exact-integer? lenv) lenv)
         ((ref? lenv)
          (define target (parse-object-at data
                                          (xref-offset objects data lenv)
                                          (ref-num lenv) (ref-gen lenv) objects
                                          (add1 depth)))
          (inexact->exact target))
         (else (raise (pdf-error 'parse "bad /Length")))))
     (pstream obj (read-bytes-n (lex-port l) len)))
    (else obj)))

;; offset lookup for a not-yet-parsed object (used for indirect /Length)
(define (xref-offset objects data lenref)
  ;; the object table may not hold it yet; find via a bounded scan of the
  ;; xref chain built from scratch is expensive — instead locate the header
  ;; pattern directly (indirect /Length is rare and points at a small object)
  (define pat
    (regexp (string-append
             "\\b" (number->string (ref-num lenref))
             " " (number->string (ref-gen lenref))
             " obj[ \t\r\n]+([0-9]+)[ \t\r\n]+endobj")))
  (define m (regexp-match pat data))
  (unless m (raise (pdf-error 'parse "indirect /Length target not found")))
  (string->number (bytes->string/latin-1 (cadr m))))

(define (skip-eol! l)
  (when (equal? (lex-peek l) 13) (lex-read l))
  (when (equal? (lex-peek l) 10) (lex-read l)))

(define (read-bytes-n port n)
  (define b (make-bytes (max 0 n)))
  (when (> n 0)
    (define got (read-bytes! b port))
    (when (< got n) (set! b (subbytes b 0 got))))
  b)

(define (peek-line port)
  (define pos (file-position port))
  (define buf (open-output-bytes))
  (let loop ()
    (define b (read-byte port))
    (unless (or (eof-object? b) (memv b (list 10 13)))
      (write-byte b buf)
      (loop)))
  (file-position port pos)
  (get-output-bytes buf))

;; -------------------------------------------------------------- resolve

(define (pdf-resolve doc v)
  (cond
    ((ref? v)
     (hash-ref (pdfdoc-objects doc)
               (cons (ref-num v) (ref-gen v))
               'null))
    (else v)))

;; ----------------------------------------------------------- page tree

;; Ordered page refs (flattens the tree via /Kids DFS).
(define (pdf-page-refs doc #:budget [budget default-pdf-resource-budget])
  (define page-limit (pdf-resource-budget-page-count budget))
  (define depth-limit (pdf-resource-budget-nesting-depth budget))
  (define root (pdf-resolve doc (pdfdoc-root-ref doc)))
  (unless (hash? root) (raise (pdf-error 'parse "catalog missing")))
  (define pages-ref (dict-get root '/Pages))
  (unless (ref? pages-ref) (raise (pdf-error 'parse "no /Pages in catalog")))
  (define acc '())
  (define page-total 0)
  (define visited (mutable-set))
  (let walk ((v pages-ref) (depth 0))
    (when (> depth depth-limit)
      (pdf-resource-error 'nesting-depth depth-limit depth))
    (define node (pdf-resolve doc v))
    (unless (and (hash? node) (ref? v))
      (raise (pdf-error 'parse "bad page tree node")))
    (cond
      ((equal? (dict-get node '/Type) '/Page)
       (set! acc (cons v acc))
       (set! page-total (add1 page-total))
       (when (> page-total page-limit)
         (pdf-resource-error 'page-count page-limit page-total)))
      (else
       (set-add! visited (cons (ref-num v) (ref-gen v)))
       (define kids (dict-get node '/Kids))
       (unless (vector? kids) (raise (pdf-error 'parse "/Kids missing")))
       (for ((k (in-vector kids)))
         (when (ref? k)
           (unless (set-member? visited (cons (ref-num k) (ref-gen k)))
             (walk k (add1 depth))))))))
  (reverse acc))

;; --------------------------------------------------------------- writing

;; Write a name symbol INCLUDING its leading '/': the first char is the
;; PDF name introducer, subsequent chars are escaped per spec.
(define (name-out out sym)
  (display #\/ out)
  (for ((b (in-bytes (string->bytes/latin-1 (substring (symbol->string sym) 1)))))
    (if (and (>= b 33) (<= b 126) (not (memv b (list 35 40 41 60 62 91 93 123 125 47 37))))
        (write-byte b out)
        (let ((hx (number->string b 16)))
          (write-byte 35 out)
          (when (< b 16) (write-byte 48 out))
          (display hx out)))))

(define (serialize-value out v numbering)
  (let ser ((v v))
    (cond
      ((exact-integer? v) (display v out))
      ((real? v) (display (~r v #:precision '(= 6)) out))
      ((boolean? v) (display (if v "true" "false") out))
      ((eq? v 'null) (display "null" out))
      ((symbol? v) (name-out out v))
      ((bytes? v) (serialize-string out v))
      ((ref? v)
       (define n (hash-ref numbering (cons (ref-num v) (ref-gen v))))
       (display (format "~a 0 R" n) out))
      ((vector? v)
       (display "[" out)
       (for ((x (in-vector v))) (ser x) (display " " out))
       (display "]" out))
      ((hash? v)
       (display "<< " out)
       (for ((k (in-list (sort (hash-keys v) symbol<?))))
         (name-out out k)
         (display " " out) (ser (hash-ref v k))
         (display " " out))
       (display ">>" out))
      ((pstream? v)
       (define d (pstream-dict v))
       (define payload (pstream-payload v))
       (display "<< " out)
       (for ((k (in-list (sort (hash-keys d) symbol<?))))
         (unless (eq? k '/Length)
           (name-out out k)
           (display " " out) (ser (hash-ref d k))
           (display " " out)))
       (display "/Length " out)
       (display (bytes-length payload) out)
       (display " >>\nstream\n" out)
       (write-bytes payload out)
       (display "\nendstream" out))
      (else (raise (pdf-error 'write (format "cannot serialize ~a" v)))))))

(define (serialize-string out b)
  (display #\( out)
  (for ((c (in-bytes b)))
    (cond
      ((memv c (list 40 41 92)) (write-byte 92 out) (write-byte c out))
      ((and (>= c 32) (<= c 126)) (write-byte c out))
      ((equal? c 10) (display "\\n" out))
      ((equal? c 13) (display "\\r" out))
      ((equal? c 9) (display "\\t" out))
      ((equal? c 8) (display "\\b" out))
      (else
       (define s (number->string c 8))
       (display #\\ out)
       (when (< (string-length s) 3) (display (make-string (- 3 (string-length s)) #\0) out))
       (display s out))))
  (display #\) out))

;; Write a document: renumber the reachable closure, classic xref table.
(define (pdf->bytes doc)
  (define objects (pdfdoc-objects doc))
  (define numbering (make-hash)) ; (cons num gen) -> new number
  (define order '())
  (define (touch v)
    (cond
      ((ref? v)
       (define key (cons (ref-num v) (ref-gen v)))
       (unless (hash-has-key? numbering key)
         (hash-set! numbering key 0)
         (set! order (cons key order))
         (touch (hash-ref objects key 'null))))
      ((pstream? v)
       (for ((x (in-hash-values (pstream-dict v)))) (touch x)))
      ((hash? v) (for ((x (in-hash-values v))) (touch x)))
      ((vector? v) (for ((x (in-vector v))) (touch x)))
      (else (void))))
  (touch (pdfdoc-root-ref doc))
  (set! order (reverse order))
  (for ((key (in-list order)) (i (in-naturals 1)))
    (hash-set! numbering key i))
  (define buf (open-output-bytes))
  (display "%PDF-1.7\n" buf)
  (define offsets (make-hash))
  (for ((key (in-list order)))
    (define num (hash-ref numbering key))
    (hash-set! offsets num (file-position buf))
    (fprintf buf "~a 0 obj\n" num)
    (serialize-value buf (hash-ref objects key 'null) numbering)
    (display "\nendobj\n" buf))
  (define size (add1 (length order)))
  (define xref-start (file-position buf))
  (fprintf buf "xref\n0 ~a\n" size)
  (display "0000000000 65535 f \n" buf)
  (for ((key (in-list order)))
    (define num (hash-ref numbering key))
    (define off (hash-ref offsets num))
    (fprintf buf "~a 00000 n \n"
             (~a off #:width 10 #:align 'right #:pad-string "0")))
  (fprintf buf "trailer\n")
  (serialize-value buf (hasheq '/Size size '/Root (pdfdoc-root-ref doc)) numbering)
  (display "\nstartxref\n" buf)
  (display xref-start buf)
  (display "\n%%EOF\n" buf)
  (get-output-bytes buf))

;; ------------------------------------------------------- page operations

(define (dict-with d key val) (hash-set d key val))

;; Replace an object's value in place (doc is mutable at the hash level).
(define (put-object! doc key obj)
  (hash-set! (pdfdoc-objects doc) key obj))

(define (page-count doc)
  (length (pdf-page-refs doc)))

;; Delete 1-based page numbers; refuses to delete every page (v1 semantics).
(define (pdf-delete-pages! doc pages)
  (define refs (pdf-page-refs doc))
  (define total (length refs))
  (define sorted (remove-duplicates (map inexact->exact pages)))
  (when (>= (length sorted) total)
    (raise (pdf-error 'pages "不能删除全部页面")))
  (for ((p (in-list sorted)))
    (unless (and (>= p 1) (<= p total))
      (raise (pdf-error 'pages (format "page ~a out of range" p)))))
  (define doomed
    (for/list ((p (in-list sorted))) (list-ref refs (sub1 p))))
  (for ((dref (in-list doomed)))
    (hash-remove! (pdfdoc-objects doc) (cons (ref-num dref) (ref-gen dref))))
  (rebuild-page-tree! doc (for/list ((r (in-list refs))
                                    (i (in-naturals 1))
                                    #:unless (member i sorted))
                            r)))

;; Rotate pages by delta (multiple of 90, normalized mod 360).
(define (pdf-rotate-pages! doc pages delta)
  (define refs (pdf-page-refs doc))
  (define total (length refs))
  (define d (modulo (inexact->exact delta) 360))
  (for ((p (in-list (remove-duplicates (map inexact->exact pages)))))
    (unless (and (>= p 1) (<= p total))
      (raise (pdf-error 'pages (format "page ~a out of range" p))))
    (define r (list-ref refs (sub1 p)))
    (define key (cons (ref-num r) (ref-gen r)))
    (define pd (pdf-resolve doc r))
    (define cur (dict-get pd '/Rotate 0))
    (define new-rot (modulo (+ (inexact->exact cur) d) 360))
    (put-object! doc key (dict-with pd '/Rotate new-rot))))

;; Insert a blank page (same size as the target page) after 1-based `page`.
(define (pdf-insert-blank-after! doc page)
  (let ((page (if (exact-integer? page) page (inexact->exact page))))
   (define refs (pdf-page-refs doc))
  (define total (length refs))
  (unless (and (>= page 1) (<= page total))
    (raise (pdf-error 'pages "page out of range")))
  (define anchor (pdf-resolve doc (list-ref refs (sub1 page))))
  (define box (dict-get anchor '/MediaBox #(0 0 612 792)))
  (define next-num
    (add1 (for/fold ((m 0)) (((k v) (in-hash (pdfdoc-objects doc))))
            (max m (car k)))))
  (define contents-ref (ref next-num 0))
  (define page-ref (ref (add1 next-num) 0))
  (define blank-stream (pstream (hasheq) #""))
  (define blank-page
    (hasheq '/Type '/Page
            '/MediaBox box
            '/Resources (hasheq)
            '/Contents contents-ref))
  (put-object! doc (cons next-num 0) blank-stream)
  (define refs2
    (append (take refs page)
            (list page-ref)
            (drop refs page)))
  (put-object! doc (cons (add1 next-num) 0) blank-page)
  (rebuild-page-tree! doc refs2)))

;; Rebuild a flat /Pages tree with the given page refs; fixes /Parent.
(define (rebuild-page-tree! doc page-refs)
  (define next-num
    (add1 (for/fold ((m 0)) (((k v) (in-hash (pdfdoc-objects doc))))
            (max m (car k)))))
  (define pages-ref (ref next-num 0))
  (define pages-dict
    (hasheq '/Type '/Pages
            '/Kids (list->vector page-refs)
            '/Count (length page-refs)))
  (put-object! doc (cons next-num 0) pages-dict)
  (for ((r (in-list page-refs)))
    (define pd (pdf-resolve doc r))
    (when (hash? pd)
      (put-object! doc (cons (ref-num r) (ref-gen r))
                   (dict-with pd '/Parent pages-ref))))
  (define root (pdf-resolve doc (pdfdoc-root-ref doc)))
  (put-object! doc (cons (ref-num (pdfdoc-root-ref doc)) (ref-gen (pdfdoc-root-ref doc)))
               (dict-with root '/Pages pages-ref)))

;; Extract ascending pages into a new document; returns (values doc bytes).
(define (pdf-extract-pages doc pages)
  (define refs (pdf-page-refs doc))
  (define total (length refs))
  (define sorted (sort (remove-duplicates (map inexact->exact pages)) <))
  (unless (pair? sorted) (raise (pdf-error 'pages "no pages selected")))
  (for ((p (in-list sorted)))
    (unless (and (>= p 1) (<= p total))
      (raise (pdf-error 'pages (format "page ~a out of range" p)))))
  (define chosen
    (for/list ((p (in-list sorted))) (list-ref refs (sub1 (inexact->exact p)))))
  (define new-objects (make-hash))
  (define (copy v)
    (cond
      ((ref? v)
       (define src-key (cons (ref-num v) (ref-gen v)))
       (define src (hash-ref (pdfdoc-objects doc) src-key 'null))
       (define new-ref (ref (ref-num v) (ref-gen v))) ; keep numbering stable
       (unless (hash-has-key? new-objects src-key)
         ;; placeholder to break cycles
         (hash-set! new-objects src-key 'null)
         (define copied
           (cond
             ((pstream? src)
              (pstream (copy-hash (pstream-dict src) copy)
                       (pstream-payload src)))
             ((hash? src) (copy-hash src copy))
             ((vector? src) (for/vector ((x (in-vector src))) (copy x)))
             (else src)))
         (hash-set! new-objects src-key copied))
       new-ref)
      ((vector? v) (for/vector ((x (in-vector v))) (copy x)))
      ((hash? v) (copy-hash v copy))
      (else v)))
  (define (copy-hash h copy-fn)
    (for/hash (((k x) (in-hash h))) (values k (copy-fn x))))
  (for ((r (in-list chosen)))
    (copy r)
    ;; fix /Type marker lost? nothing needed; pages copied with full dicts
    (void))
  ;; flat page tree in the new document
  (define new-pages-key (cons 999999999 0))
  (define copied-page-refs
    (for/list ((r (in-list chosen))) (ref (ref-num r) (ref-gen r))))
  (hash-set! new-objects new-pages-key
             (hasheq '/Type '/Pages
                     '/Kids (list->vector copied-page-refs)
                     '/Count (length copied-page-refs)))
  (define new-root-key (cons 999999998 0))
  (hash-set! new-objects new-root-key
             (hasheq '/Type '/Catalog '/Pages (ref 999999999 0)))
  (for ((r (in-list copied-page-refs)))
    (define pd (hash-ref new-objects (cons (ref-num r) (ref-gen r))))
    (when (hash? pd)
      (hash-set! new-objects (cons (ref-num r) (ref-gen r))
                 (dict-with pd '/Parent (ref 999999999 0)))))
  (define new-doc (pdfdoc new-objects (ref 999999998 0)))
  (values new-doc (pdf->bytes new-doc)))

;; Append every page of `other` into `doc` (v1 appendDocument).
(define (pdf-append-doc! doc other)
  (define refs (pdf-page-refs doc))
  (define other-refs (pdf-page-refs other))
  (define copied
    (for/list ((r (in-list other-refs)))
      (copy-object-into! doc other r)))
  (rebuild-page-tree! doc (append refs copied)))

;; Deep-copy an object graph from `src-doc` into `dst-doc`, memoized.
;; Returns the new ref in dst-doc.
(define (copy-object-into! dst src v)
  (define memo (make-hash)) ; src key -> dst ref
  (define (copy-ref r)
    (define key (cons (ref-num r) (ref-gen r)))
    (cond
      ((hash-ref memo key #f))
      (else
       (define src-obj (hash-ref (pdfdoc-objects src) key 'null))
       (define next-num
         (add1 (for/fold ((m 0)) (((k x) (in-hash (pdfdoc-objects dst))))
                 (max m (car k)))))
       (define new-ref (ref next-num 0))
       (hash-set! memo key new-ref)
       ;; reserve the number NOW so children numbering doesn't collide
       (hash-set! (pdfdoc-objects dst) (cons next-num 0) 'null)
       (define copied
         (cond
           ((pstream? src-obj)
            (pstream (copy-value (pstream-dict src-obj))
                     (pstream-payload src-obj)))
           ((hash? src-obj) (copy-value src-obj))
           ((vector? src-obj) (copy-value src-obj))
           (else src-obj)))
       (hash-set! (pdfdoc-objects dst) (cons next-num 0) copied)
       new-ref)))
  (define (copy-value v)
    (cond
      ((ref? v) (copy-ref v))
      ((vector? v) (for/vector ((x (in-vector v))) (copy-value x)))
      ((hash? v)
       (for/hash (((k x) (in-hash v))) (values k (copy-value x))))
      (else v)))
  (copy-ref v))

;; -------------------------------------------------------------- baking

;; v1 bakeText semantics: diagonal watermark centered
;; on every page (gray, rotate 45°) + text boxes at ratio coordinates.
;; Base-14 Helvetica-Bold; non-latin chars stripped; widths approximated.
(struct textbox (page x-ratio y-ratio text size) #:transparent)
(struct watermark-op (text size opacity) #:transparent)
(struct textbox-op (x-ratio y-ratio text size) #:transparent)
(define (strip-nonlatin s)
  (list->string (for/list ((c (in-string s)) #:when (<= 0 (char->integer c) 127)) c)))

(define (approx-width text size)
  ;; Helvetica average ~0.55em; tight chars lighter — good enough for centering
  (* (string-length text) 0.55 size))

(define (esc-content-text s)
  (apply string-append
         (for/list ((c (in-string s)))
           (case c
             ((#\\) "\\\\")
             ((#\() "\\(")
             ((#\)) "\\)")
             (else (string c))))))

;; Ensure page /Resources has /ExtGState /G1 (alpha) and /Font /F1 entries
;; pointing at shared objects; the alpha object carries the watermark opacity.
(define (ensure-resources! doc page-ref opacity)
  (define key (cons (ref-num page-ref) (ref-gen page-ref)))
  (define pd (pdf-resolve doc page-ref))
  (define alpha-ref (ensure-shared-object! doc 'alpha
                                           (hasheq '/Type '/ExtGState
                                                   '/ca opacity '/CA opacity)))
  (define font-ref (ensure-shared-object! doc 'font
                                          (hasheq '/Type '/Font
                                                  '/Subtype '/Type1
                                                  '/BaseFont '/Helvetica-Bold)))
  (define res (dict-get pd '/Resources))
  (define res-dict
    (cond
      ((hash? res) res)
      ((ref? res) (pdf-resolve doc res))
      (else (hasheq))))
  (define extg (dict-get res-dict '/ExtGState (hasheq)))
  (define fonts (dict-get res-dict '/Font (hasheq)))
  (define new-res
    (hash-set (hash-set res-dict
                        '/ExtGState (hash-set extg '/G1 alpha-ref))
              '/Font (hash-set fonts '/F1 font-ref)))
  (put-object! doc key (dict-with pd '/Resources new-res)))

;; One shared object per doc keyed by tag (created on first use).
(define (ensure-shared-object! doc tag template)
  (define found
    (for/first (((k v) (in-hash (pdfdoc-objects doc)))
                #:when (hash? v)
                #:when (equal? (dict-get v '/PdfdocTag) tag))
      (ref (car k) 0)))
  (or found
      (let ((next-num
             (add1 (for/fold ((m 0)) (((k v) (in-hash (pdfdoc-objects doc))))
                     (max m (car k))))))
        (put-object! doc (cons next-num 0)
                     (dict-with template '/PdfdocTag tag))
        (ref next-num 0))))

(define (pdf-bake-text doc
                       #:watermark-text (wm-text "")
                       #:watermark-size (wm-size 48)
                       #:watermark-opacity (wm-opacity 0.3)
                       #:textboxes (boxes '()))
  (define page-refs (pdf-page-refs doc))
  (define total (length page-refs))
  (define wm (strip-nonlatin wm-text))
  (define has-wm (and (non-empty-string? (string-trim wm)) (> wm-size 0)))
  (define page-ops (make-hash)) ; 1-based page -> list of content ops
  (when has-wm
    (for ((i (in-range 1 (add1 total))))
      (hash-update! page-ops i (λ (l) (cons (watermark-op wm wm-size wm-opacity) l)) '())))
  (for ((b (in-list boxes)))
    (define p (inexact->exact (textbox-page b)))
    (when (and (>= p 1) (<= p total))
      (define text (strip-nonlatin (textbox-text b)))
      ;; v1's pdf-lib drawText of an empty string is a no-op — skip quietly
      (when (non-empty-string? text)
        (hash-update! page-ops p
                    (λ (l)
                      (cons (textbox-op (textbox-x-ratio b) (textbox-y-ratio b)
                                        text (textbox-size b))
                            l))
                    '()))))
  ;; Build ops with each page's MediaBox; attach a fresh content stream.
  (for (((i raw-ops) (in-hash page-ops)))
    (define pr (list-ref page-refs (sub1 i)))
    (define pd (pdf-resolve doc pr))
    (define box (dict-get pd '/MediaBox #(0 0 612 792)))
    (define x0 (inexact->exact (vector-ref box 0)))
    (define y0 (inexact->exact (vector-ref box 1)))
    (define w (inexact->exact (vector-ref box 2)))
    (define h (inexact->exact (vector-ref box 3)))
    (ensure-resources! doc pr
                       (or (for/first ((op (in-list raw-ops)) #:when (watermark-op? op))
                             (watermark-op-opacity op))
                           1.0))
    ;; re-read: ensure-resources! may have rewritten the page dict
    (set! pd (pdf-resolve doc pr))
    (define (render-op op)
      (cond
        ((watermark-op? op)
         (match-define (watermark-op text size opacity) op)
         (define tw (approx-width text size))
         (format "q /G1 gs 0.55 0.55 0.55 rg ~a ~a ~a ~a ~a ~a cm BT /F1 ~a Tf ~a ~a Td (~a) Tj ET Q\n"
                 (~r 0.7071067811865476 #:precision '(= 4))
                 (~r 0.7071067811865476 #:precision '(= 4))
                 (~r -0.7071067811865476 #:precision '(= 4))
                 (~r 0.7071067811865476 #:precision '(= 4))
                 (~r (+ x0 (/ w 2) (* 0.5 size -0.7071067811865476)) #:precision '(= 2))
                 (~r (+ y0 (/ h 2) (* 0.5 size -0.7071067811865476)) #:precision '(= 2))
                 (~r size #:precision '(= 1))
                 (~r (- 0 (/ tw 2)) #:precision '(= 2))
                 "0"
                 (esc-content-text text)))
        (else
         (match-define (textbox-op xr yr text size) op)
         (format "BT /F1 ~a Tf 0.1 0.1 0.1 rg ~a ~a Td (~a) Tj ET\n"
                 (~r size #:precision '(= 1))
                 (~r (+ x0 4 (* xr (- w 8))) #:precision '(= 2))
                 (~r (+ y0 (- h (* yr h) size 2)) #:precision '(= 2))
                 (esc-content-text text)))))
    (define content-str
      (string-append*
       (for/list ((op (in-list (reverse raw-ops))))
         (render-op op))))
    (define next-num
      (add1 (for/fold ((m 0)) (((k v) (in-hash (pdfdoc-objects doc))))
              (max m (car k)))))
    (define contents-ref (ref next-num 0))
    (put-object! doc (cons next-num 0)
                 (pstream (hasheq) (string->bytes/latin-1 content-str)))
    (put-object! doc (cons (ref-num pr) (ref-gen pr))
                 (dict-with pd '/Contents contents-ref)))
  (void))

