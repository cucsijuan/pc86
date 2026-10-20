; music.asm - plays a tune on the PC speaker: Greensleeves (traditional).
; The timer interrupt (INT 1Ch, 18.2 Hz) walks a table of notes and programs
; the timer's channel 2 for each one; the main program just sleeps. Esc quits.
;
; The song is a table of (divisor, ticks) pairs: divisor = 1193182 / Hz,
; 0 for a rest; ticks of 55 ms. Ticks 0 ends the table and the song repeats.
;
;   asm86 music.asm -o music.com && pcrun music.com

org 100h

; note divisors
E4  equ 3620
Fs4 equ 3225
G4  equ 3044
Gs4 equ 2873
A4  equ 2712
B4  equ 2416
C5  equ 2280
D5  equ 2032
E5  equ 1810
Fs5 equ 1612
G5  equ 1522
REST equ 0

; lengths in ticks (a 6/8 bar is 24 ticks)
N16 equ 2                       ; sixteenth
N8  equ 4                       ; eighth
N8D equ 6                       ; dotted eighth
N4  equ 8                       ; quarter
N4D equ 12                      ; dotted quarter

    mov ax, 0003h               ; 80x25 text, clears the screen
    int 10h
    mov ah, 01h                 ; hide the cursor
    mov cx, 2000h
    int 10h

    mov dx, 0A1Ah               ; row 10, column 26
    mov si, title
    call print
    mov dx, 0C17h               ; row 12, column 23
    mov si, help
    call print

    xor ax, ax                  ; install the tick handler, keeping the old one
    mov es, ax
    cli
    mov ax, [es:1Ch*4]
    mov [old_tick], ax
    mov ax, [es:1Ch*4+2]
    mov [old_tick+2], ax
    mov word [es:1Ch*4], tick
    mov [es:1Ch*4+2], cs
    sti

main_loop:
    hlt                         ; the music plays from the interrupt
    mov ah, 01h                 ; key waiting?
    int 16h
    jz main_loop
    xor ah, ah
    int 16h
    cmp al, 27
    jne main_loop

    xor ax, ax                  ; Esc: give INT 1Ch back, then silence
    mov es, ax
    cli
    mov ax, [old_tick]
    mov [es:1Ch*4], ax
    mov ax, [old_tick+2]
    mov [es:1Ch*4+2], ax
    sti
    call silence
    int 20h

; INT 1Ch: counts down the current note; its last tick is silent so that
; repeated notes are heard separately.
tick:
    push ax
    push bx
    dec word [cs:left]
    jz .next
    cmp word [cs:left], 1
    jne .done
    call silence
    jmp .done
.next:
    mov bx, [cs:pos]
    mov ax, [cs:song+bx+2]
    test ax, ax                 ; end of the table: from the top
    jnz .play
    xor bx, bx
    mov ax, [cs:song+2]
.play:
    mov [cs:left], ax
    mov ax, [cs:song+bx]
    add bx, 4
    mov [cs:pos], bx
    test ax, ax
    jnz .tone
    call silence                ; a rest
    jmp .done
.tone:
    push ax
    mov al, 0B6h                ; channel 2, low then high byte, mode 3
    out 43h, al
    pop ax
    out 42h, al
    mov al, ah
    out 42h, al
    in al, 61h                  ; gate and speaker on
    or al, 03h
    out 61h, al
.done:
    pop bx
    pop ax
    iret

silence:
    push ax
    in al, 61h
    and al, 0FCh
    out 61h, al
    pop ax
    ret

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
    mov bl, 07h
    int 10h
    jmp .next
.done:
    ret

title    db "Greensleeves (traditional)", 0
help     db "PC speaker, timer channel 2.  Esc quits", 0
left     dw 1
pos      dw 0
old_tick dw 0, 0

song:
    ; verse
    dw A4, N8
    dw C5, N4,  D5, N8,  E5, N8D, Fs5, N16, E5, N8
    dw D5, N4,  B4, N8,  G4, N8D, A4, N16,  B4, N8
    dw C5, N4,  A4, N8,  A4, N8D, Gs4, N16, A4, N8
    dw B4, N4,  Gs4, N8, E4, N4,  A4, N8
    dw C5, N4,  D5, N8,  E5, N8D, Fs5, N16, E5, N8
    dw D5, N4,  B4, N8,  G4, N8D, A4, N16,  B4, N8
    dw C5, N8D, B4, N16,  A4, N8,  Gs4, N8D, Fs4, N16, Gs4, N8
    dw A4, N4D, REST, N4D
    ; chorus
    dw G5, N4D, G5, N8D, Fs5, N16, E5, N8
    dw D5, N4,  B4, N8,  G4, N8D, A4, N16,  B4, N8
    dw C5, N4,  A4, N8,  A4, N8D, Gs4, N16, A4, N8
    dw B4, N4,  Gs4, N8, E4, N4D
    dw G5, N4D, G5, N8D, Fs5, N16, E5, N8
    dw D5, N4,  B4, N8,  G4, N8D, A4, N16,  B4, N8
    dw C5, N8D, B4, N16,  A4, N8,  Gs4, N8D, Fs4, N16, Gs4, N8
    dw A4, N4D, REST, N4D
    dw REST, N4 + N4 + N4       ; a breath before it starts again
    dw 0, 0
