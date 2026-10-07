; Code and data in the wrong places.
;
; Expected:
;   error: instructions may only appear in .text, not in .data
;   error: .byte cannot appear in .bss, which holds no data

.section .data
    ADD R1, R2

.section .bss
    .byte 1
