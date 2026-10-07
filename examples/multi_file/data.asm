; data.asm: read-only data, compiled separately.
;
; `message` lands in .rodata and `message_len` in .data, so this file exercises
; relocations into two different output regions.

.section .rodata

.global message

message:
    .asciz "sum computed across three object files\n"

.section .data

.global message_len

message_len:
    .qword 39
