// Context switch
//
//   void swtch(struct context *old, struct context *new);
//
// Save current registers in old. Load from new.


.global swtch
swtch:
        mov x9, x0
        mov x10, sp

        stp x10/*sp*/, x18, [x9, #16 * 0]
        stp x19, x20, [x9, #16 * 1]
        stp x21, x22, [x9, #16 * 2]
        stp x23, x24, [x9, #16 * 3]
        stp x25, x26, [x9, #16 * 4]
        stp x27, x28, [x9, #16 * 5]
        stp x29, x30, [x9, #16 * 6]

        mov x9, x1

        ldp x10/*sp*/, x18, [x9, #16 * 0]
        ldp x19, x20, [x9, #16 * 1]
        ldp x21, x22, [x9, #16 * 2]
        ldp x23, x24, [x9, #16 * 3]
        ldp x25, x26, [x9, #16 * 4]
        ldp x27, x28, [x9, #16 * 5]
        ldp x29, x30, [x9, #16 * 6]

        mov sp, x10
        
        ret

// FP/SIMD state of a user task (struct fpsimd_state in proc.h):
//
//   void fpsimd_save(struct fpsimd_state *s);
//   void fpsimd_load(const struct fpsimd_state *s);
//
// The kernel is built with +nofp and never touches v0-v31, so between a
// task's trap into the kernel and the scheduler switching away from it the
// registers still hold that task's values.  scheduler() saves them right
// after swtch() returns and loads the next task's before swtch() to it.
        .arch armv8-a+fp+simd

.global fpsimd_save
fpsimd_save:
        stp q0, q1, [x0, #32 * 0]
        stp q2, q3, [x0, #32 * 1]
        stp q4, q5, [x0, #32 * 2]
        stp q6, q7, [x0, #32 * 3]
        stp q8, q9, [x0, #32 * 4]
        stp q10, q11, [x0, #32 * 5]
        stp q12, q13, [x0, #32 * 6]
        stp q14, q15, [x0, #32 * 7]
        stp q16, q17, [x0, #32 * 8]
        stp q18, q19, [x0, #32 * 9]
        stp q20, q21, [x0, #32 * 10]
        stp q22, q23, [x0, #32 * 11]
        stp q24, q25, [x0, #32 * 12]
        stp q26, q27, [x0, #32 * 13]
        stp q28, q29, [x0, #32 * 14]
        stp q30, q31, [x0, #32 * 15]
        mrs x1, fpsr
        mrs x2, fpcr
        add x3, x0, #512
        stp w1, w2, [x3]
        ret

.global fpsimd_load
fpsimd_load:
        ldp q0, q1, [x0, #32 * 0]
        ldp q2, q3, [x0, #32 * 1]
        ldp q4, q5, [x0, #32 * 2]
        ldp q6, q7, [x0, #32 * 3]
        ldp q8, q9, [x0, #32 * 4]
        ldp q10, q11, [x0, #32 * 5]
        ldp q12, q13, [x0, #32 * 6]
        ldp q14, q15, [x0, #32 * 7]
        ldp q16, q17, [x0, #32 * 8]
        ldp q18, q19, [x0, #32 * 9]
        ldp q20, q21, [x0, #32 * 10]
        ldp q22, q23, [x0, #32 * 11]
        ldp q24, q25, [x0, #32 * 12]
        ldp q26, q27, [x0, #32 * 13]
        ldp q28, q29, [x0, #32 * 14]
        ldp q30, q31, [x0, #32 * 15]
        add x3, x0, #512
        ldp w1, w2, [x3]
        msr fpsr, x1
        msr fpcr, x2
        ret
