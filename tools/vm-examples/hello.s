; HELLO: the first HALTZ VM program. Prints a greeting, the numbers 1 to 10 and their sum,
; then 7! from a recursive subroutine.
;   python3 tools/hzasm.py tools/vm-examples/hello.s
;   build-host/hzrun tools/vm-examples/hello.hzp

        .title  "HELLO"

start:  movi    r0, greeting
        sys     PRINT
        movi8   r1, 0           ; PRINT_NUM format 0: unsigned decimal (r1 stays set)
        movi8   r4, 1           ; i
        movi8   r5, 0           ; sum
        movi8   r6, 10          ; loop count
count:  mov     r0, r4
        sys     PRINT_NUM
        movi8   r0, ' '
        sys     PUTC
        add     r5, r4
        inc     r4
        dbnz    r6, count

        movi    r0, sum_text
        sys     PRINT
        mov     r0, r5
        sys     PRINT_NUM

        movi    r0, fact_text
        sys     PRINT
        movi8   r0, 7
        call    fact
        movi8   r1, 0           ; fact changed r1
        sys     PRINT_NUM
        movi8   r0, '\n'
        sys     PUTC

        movi8   r0, 0
        sys     EXIT

; fact: r0 = r0! (16 bits, so up to 8!). Changes r1.
fact:   cmpq    r0, 1
        bls     fact_one        ; n <= 1
        push    r0
        dec     r0
        call    fact            ; r0 = (n-1)!
        pop     r1              ; r1 = n
        mul     r0, r1
        ret
fact_one: movi8   r0, 1
        ret

greeting:  .asciz "HELLO FROM HALTZ\n"
sum_text:  .asciz "\nSUM "
fact_text: .asciz "\n7! = "
