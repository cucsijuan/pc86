; strings.asm - string instructions: length, copy, compare, fill
org 100h

    cld                         ; walk forward through memory

    ; 1) length of 'source' with REPNE SCASB (searches for the final 0)
    mov di, source
    mov cx, 0FFFFh
    xor al, al
    repne scasb
    mov ax, 0FFFFh
    sub ax, cx
    dec ax                      ; AX = length without the 0
    mov [length], ax

    ; 2) copy 'source' to 'dest' with REP MOVSB (including the 0)
    mov si, source
    mov di, dest
    mov cx, [length]
    inc cx
    rep movsb

    ; 3) compare with REPE CMPSB
    mov si, source
    mov di, dest
    mov cx, [length]
    repe cmpsb
    jne different
    mov dx, msg_equal
    jmp report
different:
    mov dx, msg_different
report:
    mov ah, 09h
    int 21h

    ; 4) print the copy one character at a time with LODSB
    mov si, dest
.next:
    lodsb
    test al, al
    jz .done
    mov dl, al
    mov ah, 02h
    int 21h
    jmp .next
.done:
    mov dl, 13
    int 21h
    mov dl, 10
    int 21h

    ; 5) fill with REP STOSB and print
    mov di, filler
    mov cx, 10
    mov al, '*'
    rep stosb
    mov dx, filler
    mov ah, 09h
    int 21h
    ret

source        db 'The 8086 has string instructions', 0
length        dw 0
msg_equal     db 'The copy is identical', 13, 10, '$'
msg_different db 'The copy is different', 13, 10, '$'
filler        times 10 db 0
              db 13, 10, '$'
dest          times 64 db 0
