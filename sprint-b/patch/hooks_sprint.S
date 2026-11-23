// Correr con B: los dos puntos de entrada a los que saltan las instrucciones
// parcheadas del juego. Corren en mitad de funciones del juego, así que
// guardan todo lo que el código C puede tocar (los registros generales, los
// 32 de vectores y las banderas) y solo cambian el registro que la
// instrucción sustituida iba a escribir. Las macros son las del parche de la
// cámara nativa, tal cual.

.macro SAVE_ALL
    sub     sp, sp, #(176 + 32*16)
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x30, [sp, #144]
    mrs     x9, nzcv
    str     x9, [sp, #160]
    stp     q0, q1, [sp, #176]
    stp     q2, q3, [sp, #208]
    stp     q4, q5, [sp, #240]
    stp     q6, q7, [sp, #272]
    stp     q8, q9, [sp, #304]
    stp     q10, q11, [sp, #336]
    stp     q12, q13, [sp, #368]
    stp     q14, q15, [sp, #400]
    stp     q16, q17, [sp, #432]
    stp     q18, q19, [sp, #464]
    stp     q20, q21, [sp, #496]
    stp     q22, q23, [sp, #528]
    stp     q24, q25, [sp, #560]
    stp     q26, q27, [sp, #592]
    stp     q28, q29, [sp, #624]
    stp     q30, q31, [sp, #656]
.endm

// restores everything except v0 (the result) when \keep_v0 is 1
.macro RESTORE_ALL keep_v0
    .if \keep_v0
    ldr     q1, [sp, #192]
    .else
    ldp     q0, q1, [sp, #176]
    .endif
    ldp     q2, q3, [sp, #208]
    ldp     q4, q5, [sp, #240]
    ldp     q6, q7, [sp, #272]
    ldp     q8, q9, [sp, #304]
    ldp     q10, q11, [sp, #336]
    ldp     q12, q13, [sp, #368]
    ldp     q14, q15, [sp, #400]
    ldp     q16, q17, [sp, #432]
    ldp     q18, q19, [sp, #464]
    ldp     q20, q21, [sp, #496]
    ldp     q22, q23, [sp, #528]
    ldp     q24, q25, [sp, #560]
    ldp     q26, q27, [sp, #592]
    ldp     q28, q29, [sp, #624]
    ldp     q30, q31, [sp, #656]
    ldr     x9, [sp, #160]
    msr     nzcv, x9
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x30, [sp, #144]
    add     sp, sp, #(176 + 32*16)
.endm

    .text
    .global oot_hook_sprint_link
    .global oot_hook_sprint_stance

// Sustituye a `ldr x10, [x10]` en el módulo 0x4AE798, en la actualización de
// movimiento de Link: x19 es Link, x10 el puntero a sus banderas de entrada
// y w9 (ya cargada) la otra palabra que la función prueba después; todo se
// conserva. La carga sustituida se hace aquí, la palabra pasa por el código
// C (que pone o limpia el bit dash según B) y el resultado ocupa el lugar
// del x10 guardado, así que al restaurar, x10 ya lleva las banderas buenas.
oot_hook_sprint_link:
    SAVE_ALL
    mov     x0, x19                  // Link (x19 es de los que el C conserva)
    ldr     x1, [sp, #80]            // el x10 guardado: el puntero a las banderas
    ldr     x1, [x1]                 // la carga sustituida: la palabra de banderas
    bl      oot_sprint_link
    str     x0, [sp, #80]            // las banderas ajustadas, al hueco del x10
    RESTORE_ALL 0
    ret

// Sustituye a `ldrb w8, [x8]` en el módulo 0xC07F3C, en la actualización de
// postura y cámara: x8 es el puntero a las mismas banderas y la siguiente
// instrucción del juego prueba el bit 4 del byte cargado. La carga se hace
// aquí y el byte vuelve ajustado (el bit según el sprint activo) al hueco
// del x8 guardado.
oot_hook_sprint_stance:
    SAVE_ALL
    ldr     x0, [sp, #64]            // el x8 guardado: el puntero a las banderas
    ldrb    w0, [x0]                 // la carga sustituida: el byte bajo
    bl      oot_sprint_stance
    and     x0, x0, #0xff
    str     x0, [sp, #64]            // el byte ajustado, al hueco del x8
    RESTORE_ALL 0
    ret
