; The same label defined twice.
;
; Expected: error: duplicate label 'loop'
;   note: previously defined here

.section .text
.global _start

_start:
loop:
    NOP
loop:
    HALT
