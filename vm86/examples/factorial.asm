; factorial.asm - recursive factorial from 0 to 8, using the stack
org 100h

    xor cx, cx
next:
    mov ax, cx
    call factorial      ; AX = CX!
    call print_num
    mov dx, crlf
    mov ah, 09h
    int 21h
    inc cx
    cmp cx, 8
    jbe next
    ret

; factorial: AX = AX! (recursive; fits in 16 bits up to 8! = 40320)
factorial:
    cmp ax, 1
    ja .recurse
    mov ax, 1
    ret
.recurse:
    push bx
    mov bx, ax
    dec ax
    call factorial      ; AX = (n-1)!
    mul bx              ; DX:AX = (n-1)! * n
    pop bx
    ret

print_num:
    push ax
    push bx
    push cx
    push dx
    mov bx, 10
    xor cx, cx
.divide:
    xor dx, dx
    div bx
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

crlf db 13, 10, '$'
