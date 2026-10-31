#lang racket/base

;; Release identity duplicated from rivet.rktd. The packaged app cannot read
;; the project file at runtime, so the updater embeds these constants. Keep
;; them in sync with rivet.rktd (AGENTS.md lists this as a release checklist
;; item).

(provide app-version
         app-build
         app-identifier
         app-channel
         app-display-name
         update-key-id
         update-public-key-b64
         default-update-base-url)

(define app-version "0.8.0")
(define app-build 1)
(define app-identifier "site.jrtx.booklight")
(define app-channel 'stable)
(define app-display-name "Booklight")

(define update-key-id "booklight-2026-10")

;; SubjectPublicKeyInfo DER, base64. Rotate by shipping a build that trusts
;; the next key before signing releases exclusively with it (rivet
;; docs/release-and-updates.md). The private half lives only in ~/Sync/Keys
;; plus the repo's UPDATE_ED25519_PRIVATE_KEY_B64 secret; it never ships.
(define update-public-key-b64
  "MCowBQYDK2VwAyEAglfp6BpekL4uFAh7mVR6hbnZJYe7hNX4QZujkUY2gxI=")

(define default-update-base-url
  "https://github.com/turlamai/booklight/releases/latest/download")
