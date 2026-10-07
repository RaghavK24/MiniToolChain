; Constants that do not fit where they are being put.
;
; Expected: an error for each, naming the field width rather than silently
; truncating the value.

.section .text
.global _start

_start:
    MOVI R1, 281474976710656    ; the immediate field is 48 bits signed
    HALT

.section .data
    .byte 256                   ; a byte holds -128..255
