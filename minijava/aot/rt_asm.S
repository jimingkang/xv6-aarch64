// MiniJava runtime: the few pieces that must be written in assembly.
//
//  * main          -- process entry: build the main MJThread, put it in x28,
//                     run the program's static main, then wait for threads.
//  * mj_thread_entry -- first code of every Java thread created by clone().
//  * mj_throw      -- unwind to the innermost exception-handler record.
//
// x28 is the "current thread" register for compiled MiniJava code (HotSpot
// on AArch64 uses r28 = rthread).  It is callee-saved in AAPCS64, so C code
// and libc preserve it; runtime.c is built with -ffixed-x28.

#define MJT_ID       0
#define MJT_HANDLER  8
#define MJT_OBJ     32

        .text
        .globl main
        .type main, %function
main:
        stp x29, x30, [sp, #-16]!
        mov x29, sp
        bl mj_init                    // x0 = MJThread for "main" (argc/argv in x0/x1)
        mov x28, x0
        bl mjmain                     // compiled: public static void main(String[])
        bl mj_main_exit               // waits for non-daemon threads, exit(0)
        .size main, .-main

        .globl mj_thread_entry
        .type mj_thread_entry, %function
mj_thread_entry:                      // x0 = MJThread* (clone() argument)
        mov x28, x0
        mov x29, #0                   // end of the frame-pointer chain
        mov x30, #0
        ldr x0, [x28, #MJT_OBJ]       // the java.lang.Thread object
        ldr x9, [x0]                  // vtable
        ldr x9, [x9, #8]              // slot 1 = run()
        blr x9
        b mj_thread_finish            // never returns
        .size mj_thread_entry, .-mj_thread_entry

// void mj_throw(Object *exc) -- never returns
//   handler record (in the catching method's frame):
//     [0] prev record   [8] saved fp   [16] saved sp   [24] handler pc
        .globl mj_throw
        .type mj_throw, %function
mj_throw:
        ldr x1, [x28, #MJT_HANDLER]
        cbz x1, 1f
        ldr x2, [x1, #0]
        str x2, [x28, #MJT_HANDLER]   // pop the record
        ldr x29, [x1, #8]
        ldr x3, [x1, #16]
        mov sp, x3
        ldr x4, [x1, #24]
        br x4                         // handler sees the exception in x0
1:      b mj_uncaught                 // no handler left in this thread
        .size mj_throw, .-mj_throw

        .section .note.GNU-stack,"",%progbits
