; clock.asm - a stopwatch driven by the timer interrupt.
; Hooks INT 1Ch (called by the BIOS 18.2 times per second), sleeps in HLT
; between ticks and redraws the elapsed time. Esc quits.
;
;   asm86 clock.asm -o clock.com && pcrun clock.com --seconds 3

org 100h

    mov ax, 0003h               ; 80x25 text, clears the screen
    int 10h
    mov ah, 01h                 ; hide the cursor
    mov cx, 2000h
    int 10h

    mov dx, 0A1Eh               ; row 10, column 30
    mov si, title
    call print

    xor ax, ax                  ; install the tick handler
    mov es, ax
    cli
    mov word [es:1Ch*4], tick
    mov [es:1Ch*4+2], cs
    sti

main_loop:
    hlt                         ; sleep until the next interrupt
    mov ah, 01h                 ; key waiting?
    int 16h
    jz redraw
    xor ah, ah
    int 16h
    cmp al, 27
    je quit
redraw:
    mov ax, [ticks]             ; seconds = ticks * 10 / 182
    mov bx, 10
    mul bx
    mov bx, 182
    div bx
    mov dx, 0C22h               ; row 12, column 34
    call print_number
    jmp main_loop

quit:
    int 20h

tick:
    inc word [cs:ticks]
    iret

; prints the zero-terminated string at SI at row DH, column DL
print:
    mov ah, 02h
    xor bh, bh
    int 10h
.next:
    lodsb
    or al, al
    jz .done
    mov ah, 0Eh
    mov bl, 0Fh
    int 10h
    jmp .next
.done:
    ret

; prints AX in decimal, 5 digits, at row DH, column DL
print_number:
    push ax
    mov ah, 02h
    xor bh, bh
    int 10h
    pop ax
    mov di, digits + 4
    mov bx, 10
    mov cx, 5
.digit:
    xor dx, dx
    div bx
    add dl, '0'
    mov [di], dl
    dec di
    loop .digit
    mov si, digits
    mov cx, 5
.out:
    lodsb
    mov ah, 0Ah                 ; write character, keep attribute
    push cx
    mov cx, 1
    int 10h
    mov ah, 03h                 ; advance the cursor by hand
    int 10h
    inc dl
    mov ah, 02h
    int 10h
    pop cx
    loop .out
    mov si, seconds_label
    call print_tail
    ret

print_tail:
    lodsb
    or al, al
    jz .done
    mov ah, 0Eh
    mov bl, 07h
    int 10h
    jmp print_tail
.done:
    ret

ticks dw 0
title db 'pc86 timer demo', 0
digits db '00000'
seconds_label db ' s', 0
