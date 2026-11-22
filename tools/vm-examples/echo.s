; ECHO: prints the keys typed until Enter, then how many there were. Waits one frame
; (SYS FRAME) whenever no key is there, and gives up after 300 empty frames (5 s).
;   build-host/hzrun -k "hi there\n" tools/vm-examples/echo.hzp

        .title  "ECHO"

        movi    r0, prompt
        sys     PRINT
        movi8   r5, 0           ; keys typed
        movi    r6, 300         ; empty frames left
wait:   sys     KEY_GET         ; r0 char, r1 usage (0 = none), r2 modifiers
        cmpq    r1, 0
        beq     idle
        cmpi8   r1, 0x28        ; Enter
        beq     done
        sys     PUTC
        inc     r5
        bra     wait
idle:   sys     FRAME
        dbnz    r6, wait
        movi    r0, timeout
        sys     PRINT
        movi8   r0, 1
        sys     EXIT

done:   movi    r0, count_text
        sys     PRINT
        mov     r0, r5
        movi8   r1, 0
        sys     PRINT_NUM
        movi8   r0, '\n'
        sys     PUTC
        movi8   r0, 0
        sys     EXIT

prompt:     .asciz "TYPE> "
count_text: .asciz "\nKEYS: "
timeout:    .asciz "\nNO KEYS\n"
