; numbers.asm - print the numbers 1 to 20 in decimal
org 100h

    mov cx, 1
next:
    mov ax, cx
    call print_num
    mov dl, ' '
    mov ah, 02h
    int 21h
    inc cx
    cmp cx, 20
    jbe next
    call newline
    ret

; print_num: print AX as an unsigned decimal number. Preserves all registers.
print_num:
    push ax
    push bx
    push cx
    push dx
    mov bx, 10
    xor cx, cx          ; number of digits on the stack
.divide:
    xor dx, dx
    div bx              ; AX = quotient, DX = remainder (the digit)
    push dx
    inc cx
    test ax, ax
    jnz .divide
.print:
    pop dx
    add dl, '0'
    mov ah, 02h
    int 21h
    loop .print
    pop dx
    pop cx
    pop bx
    pop ax
    ret

newline:
    mov dx, crlf
    mov ah, 09h
    int 21h
    ret

crlf db 13, 10, '$'
