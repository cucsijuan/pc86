; bounce.asm - a ball bouncing in mode 13h, one step per vertical retrace
org 100h

SIZE equ 12

    mov ax, 0013h
    int 10h
    mov ax, 0A000h
    mov es, ax

    ; frame around the screen in color 7 (light gray)
    mov al, 7
    xor di, di
    mov cx, 320
    rep stosb
    mov di, 199*320
    mov cx, 320
    rep stosb
    mov di, 320
    mov cx, 198
.sides:
    mov [es:di], al
    mov [es:di+319], al
    add di, 320
    loop .sides

main_loop:
    call wait_retrace
    mov al, 0               ; erase at the old position
    call draw_ball

    mov ax, [x]             ; move horizontally, bounce off the walls
    add ax, [dx_]
    cmp ax, 2
    jl .flip_x
    cmp ax, 318 - SIZE
    jg .flip_x
    jmp .x_ok
.flip_x:
    neg word [dx_]
    mov ax, [x]
.x_ok:
    mov [x], ax

    mov ax, [y]             ; same vertically
    add ax, [dy_]
    cmp ax, 2
    jl .flip_y
    cmp ax, 198 - SIZE
    jg .flip_y
    jmp .y_ok
.flip_y:
    neg word [dy_]
    mov ax, [y]
.y_ok:
    mov [y], ax

    mov al, 15              ; draw at the new position (white)
    call draw_ball
    jmp main_loop

; draw_ball: fills the SIZE x SIZE square at (x, y) with color AL
draw_ball:
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
    push ax
    push dx
    mov dx, 3DAh
.in:
    in al, dx
    test al, 8
    jnz .in
.out:
    in al, dx
    test al, 8
    jz .out
    pop dx
    pop ax
    ret

x   dw 40
y   dw 30
dx_ dw 2
dy_ dw 1
