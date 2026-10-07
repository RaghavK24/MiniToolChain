; A call to something that is never defined, here or anywhere.
;
; This one assembles cleanly (the assembler assumes an unknown name comes from
; another object file) and fails at link time instead:
;
;   error: link failed: undefined symbol 'never_defined' referenced by ...

.section .text
.global _start

_start:
    CALL never_defined
    HALT
