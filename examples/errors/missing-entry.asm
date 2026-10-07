; A valid program with no entry point.
;
; Expected: error: link failed: entry point '_start' is not defined in any
; object. Use --entry to name a different one.

.section .text

.global helper

helper:
    NOP
    RET
