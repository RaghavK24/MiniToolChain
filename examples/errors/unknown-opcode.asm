; An instruction that does not exist.
;
; Expected: error: unknown instruction 'MOVE'
;   ...pointing at the mnemonic, with the rest of the file still checked.

.section .text
.global _start

_start:
    MOVE R1, R2         ; the instruction is called MOV
    HALT
