.text
.arm
.align 2
.global fail_test, timeout_test, timeout_rw

@ §19 autorun: stock upstream waited here for the B button to be pressed
@ and released before returning to the menu -- headless has no button, so
@ go straight back to main_post_init (main.s). cpp_fail_test() has already
@ recorded the result into slot-2 by the time we get here.

@ these are called as functions but do not return (the test failed anyway)
@ -> show that the test failed, then reset back to the menu
@ r0 contains the number of the test that failed (for fail_test, timeout_test)
fail_test:
    push {r0}
    bl disable_trace
    ldr r0, =txt_fail
    mov r1, #5
    pop {r2}
    bl cpp_fail_test
    b main_post_init

timeout_test:
    push {r0}
    bl disable_trace
    ldr r0, =txt_timeout
    mov r1, #8
    pop {r2}
    bl cpp_fail_test
    b main_post_init

timeout_rw:
    bl disable_trace
    ldr r0, =txt_timeout_rw
    mov r1, #12
    mov r2, #-1
    bl cpp_fail_test
    b main_post_init


txt_fail:       .asciz "FAIL "
txt_timeout:    .asciz "TIMEOUT "
txt_timeout_rw: .asciz "TIMEOUT R/W "
