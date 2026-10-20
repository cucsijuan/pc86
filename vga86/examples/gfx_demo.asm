; gfx_demo.asm - mode 13h: palette through the DAC ports, a pattern drawn
; straight into A000:0000, and palette animation synced to vertical retrace.
org 100h

FRAMES equ 140              ; about 2 seconds at 70 Hz

    mov ax, 0013h           ; 320x200, 256 colors
    int 10h

    ; --- palette: entries 0..63 a gray ramp, 64..127 the ramp reversed ---
    mov dx, 3C8h
    xor al, al
    out dx, al              ; start writing at entry 0
    inc dx                  ; 3C9h: R, G, B for each entry
    xor bl, bl
ramp_up:
    mov al, bl
    out dx, al
    out dx, al
    out dx, al
    inc bl
    cmp bl, 64
    jb ramp_up
ramp_down:
    dec bl
    mov al, bl
    out dx, al
    out dx, al
    out dx, al
    test bl, bl
    jnz ramp_down

    ; --- pattern: color = (x xor y) and 127 ---
    mov ax, 0A000h
    mov es, ax
    xor di, di
    xor dx, dx              ; y
row:
    xor cx, cx              ; x
col:
    mov ax, cx
    xor ax, dx
    and al, 127
    stosb
    inc cx
    cmp cx, 320
    jb col
    inc dx
    cmp dx, 200
    jb row

    ; --- frame label in the corner using the BIOS 8x8 font ---
    mov ah, 02h
    xor bh, bh
    mov dx, 0101h           ; row 1, column 1
    int 10h
    mov si, label
print_label:
    lodsb
    test al, al
    jz animate
    mov ah, 0Eh
    mov bl, 63              ; white in our ramp
    int 10h
    jmp print_label

    ; --- rotate the 128 palette entries once per frame ---
animate:
    mov cx, FRAMES
frame:
    call wait_retrace
    push cx
    mov dx, 3C8h
    xor al, al
    out dx, al
    inc dx
    mov bx, [phase]
    mov cx, 128
.entry:
    mov al, bl
    and al, 127
    cmp al, 64
    jb .up
    not al
    and al, 63
.up:
    out dx, al
    out dx, al
    out dx, al
    inc bl
    loop .entry
    inc word [phase]
    pop cx
    loop frame

    mov ax, 4C00h
    int 21h

; waits for the start of the next vertical retrace (bit 3 of port 3DAh)
wait_retrace:
    push ax
    push dx
    mov dx, 3DAh
.in_retrace:
    in al, dx
    test al, 8
    jnz .in_retrace         ; if we are inside one, wait for it to end
.not_yet:
    in al, dx
    test al, 8
    jz .not_yet             ; then wait for the next one to begin
    pop dx
    pop ax
    ret

phase dw 0
label db 'mode 13h + retrace', 0
