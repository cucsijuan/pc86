; keys_game.asm - game-style input: our own INT 09h handler reads scan codes
; from port 60h and keeps a table of which keys are down. Arrow keys or WASD
; move the square; Esc exits.
org 100h

STEP equ 2
SIZE equ 10

    ; install the handler, keeping the BIOS vector to restore it on exit
    xor ax, ax
    mov es, ax
    cli
    mov ax, [es:9*4]
    mov [old_ip], ax
    mov ax, [es:9*4+2]
    mov [old_cs], ax
    mov word [es:9*4], key_handler
    mov [es:9*4+2], cs
    sti

    mov ax, 0013h
    int 10h
    mov ax, 0A000h
    mov es, ax

game_loop:
    call wait_retrace
    mov al, 0                   ; erase
    call draw
    cmp byte [down+48h], 0      ; Up arrow (also keypad 8)
    jne .up
    cmp byte [down+11h], 0      ; W
    je .no_up
.up:
    cmp word [y], STEP
    jb .no_up
    sub word [y], STEP
.no_up:
    cmp byte [down+50h], 0      ; Down
    jne .down
    cmp byte [down+1Fh], 0      ; S
    je .no_down
.down:
    cmp word [y], 200 - SIZE - STEP
    ja .no_down
    add word [y], STEP
.no_down:
    cmp byte [down+4Bh], 0      ; Left
    jne .left
    cmp byte [down+1Eh], 0      ; A
    je .no_left
.left:
    cmp word [x], STEP
    jb .no_left
    sub word [x], STEP
.no_left:
    cmp byte [down+4Dh], 0      ; Right
    jne .right
    cmp byte [down+20h], 0      ; D
    je .no_right
.right:
    cmp word [x], 320 - SIZE - STEP
    ja .no_right
    add word [x], STEP
.no_right:
    mov al, 15
    call draw
    cmp byte [down+01h], 0      ; Esc
    je game_loop

    ; restore the BIOS handler and text mode
    xor ax, ax
    mov es, ax
    cli
    mov ax, [old_ip]
    mov [es:9*4], ax
    mov ax, [old_cs]
    mov [es:9*4+2], ax
    sti
    mov ax, 0003h
    int 10h
    ret

; IRQ 1: one scan code per interrupt. E0 prefixes are skipped, so the gray
; arrows set the same entries as the keypad arrows.
key_handler:
    push ax
    push bx
    in al, 60h
    cmp al, 0E0h
    je .eoi
    mov bl, al
    and bx, 7Fh
    test al, 80h                ; break code: key released
    jz .pressed
    mov byte [cs:down+bx], 0
    jmp .eoi
.pressed:
    mov byte [cs:down+bx], 1
.eoi:
    mov al, 20h                 ; end of interrupt for the PIC
    out 20h, al
    pop bx
    pop ax
    iret

; draw: fills the square at (x, y) with color AL
draw:
    push ax
    mov ax, [y]
    mov bx, 320
    mul bx
    add ax, [x]
    mov di, ax
    pop ax
    mov dx, SIZE
.row:
    mov cx, SIZE
    push di
    rep stosb
    pop di
    add di, 320
    dec dx
    jnz .row
    ret

wait_retrace:
    mov dx, 3DAh
.in:
    in al, dx
    test al, 8
    jnz .in
.out:
    in al, dx
    test al, 8
    jz .out
    ret

x      dw 155
y      dw 95
old_ip dw 0
old_cs dw 0
down   times 128 db 0
