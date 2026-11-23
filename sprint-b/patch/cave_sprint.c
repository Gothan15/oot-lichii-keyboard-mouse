/* Correr con B (estilo BotW/TotK) para Ocarina of Time (build E0F3CCFA...):
   la lógica que va en el hueco libre tras el código del juego.

   Cómo corre Link en este juego (lo desarmó Triforce en host/oot_sprint.c,
   tras el runtime/sprint.c de NX2navi): su actualización de movimiento carga
   sus banderas de entrada — la palabra de 64 bits en *(Link +0x50) — y prueba
   el bit 4 (0x10), la «entrada dash»: para empezar a correr y para seguir
   corriendo. El juego solo pone ese bit al salir de una voltereta, y por eso
   de serie se corre manteniendo A tras rodar. Este parche se hace dueño del
   bit en el mismo punto de carga:

     - Mantener B con Link en movimiento = correr, con la velocidad, la
       animación y la cámara del propio juego (es su sprint, no uno inventado).
     - A deja de correr: el bit se limpia cuando no manda B, así que tras una
       voltereta ya no hay dash aunque se siga manteniendo A. La voltereta en
       sí, interactuar y todo lo demás de A no se tocan.
     - B no se le esconde al juego: la pulsación que empieza en movimiento le
       llega igual, así que guarda el arma y corre, como en BotW/TotK. En
       parado, B sigue siendo el de siempre (la pulsación no engancha).
     - Fijar la mira con ZL cancela el correr (la postura del propio juego),
       y en estados donde no se puede correr (hablar, menús, escenas) no hay
       sprint: la tabla de estados es la de Triforce.

   Los dos ganchos (hooks_sprint.S) llaman aquí:
     - oot_sprint_link: tras cargar las banderas en la actualización de
       movimiento (módulo 0x4AE798 en la 2.0.0); devuelve las banderas con el
       bit ya puesto o limpio, y deja el resultado en oot_sprint_state.activo.
     - oot_sprint_stance: tras cargar el byte bajo de las mismas banderas en
       la actualización de postura y cámara (módulo 0xC07F3C); aplica el
       mismo bit según ese activo, para que la postura siga al movimiento.

   Los botones se leen del propio mando con nn::hid::GetNpadStates (los
   stubs PLT del juego), jugador 1 en modo completo y, si no responde, el
   modo portátil. */

#include <stdint.h>

/* El estado del parche, en un hueco de BSS que el juego no usa (el script de
   construcción fija su dirección; ver build_sprint_b_ips.py). */
struct SprintState {
    uint64_t botones_prev; /* los botones de la última actualización (para el flanco de B) */
    int pestillo;          /* la pulsación de B en curso empezó con Link en movimiento */
    int activo;            /* el sprint manda en este momento (lo lee el gancho de postura) */
};
extern volatile struct SprintState oot_sprint_state;

/* nn::hid::GetNpadStates, en sus dos sabores que usa el juego. El estado del
   mando (NpadState): número de muestra, botones, dos sticks, atributos. */
struct NpadState {
    uint64_t sampling_number;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes; /* bit 0: conectado */
    uint32_t reserved;
};
int GetNpadStatesFullKey(struct NpadState *out, int count, const uint32_t *id)
    __asm__("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj");
int GetNpadStatesHandheld(struct NpadState *out, int count, const uint32_t *id)
    __asm__("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj");

#define BOTON_B (1ull << 1)  /* HidNpadButton: B */
#define BOTON_ZL (1ull << 8) /* HidNpadButton: ZL (fijar la mira) */
#define DASH 0x10ull         /* la entrada dash en las banderas de Link */

/* Qué hace el estado de Link con el sprint (la tabla de Triforce). */
enum { MOVER, MANTENER, ESPERAR, PARAR };

static int tipo_estado(int32_t id) {
    switch (id) {
    case 1:  /* Andar (y correr) */
    case 13: /* Saltar */
    case 32: /* Aterrizar */
    case 33: /* Caer */
    case 68: /* Trepar */
        return MOVER;
    case 253: /* StepOn */
    case 35:  /* Deslizarse */
    case 36:  /* Aterrizar de un desliz */
        return MANTENER;
    case 40: /* Esperar (parado) */
        return ESPERAR;
    default:
        return PARAR;
    }
}

/* El id del estado de Link, por la cadena de punteros que usa Triforce:
   [[[[Link +0x18] +0x270] +0x188] +0x1c0] +0x28 (y +0x4c si vale 0x11a, un
   estado que envuelve a otro). Cada paso se comprueba: un puntero a cero da
   -1, que cae en PARAR, y sin sprint. */
static int32_t id_estado(uint64_t link) {
    uint64_t p = *(const uint64_t *)(link + 0x18);
    if (!p) return -1;
    p = *(const uint64_t *)(p + 0x270);
    if (!p) return -1;
    p = *(const uint64_t *)(p + 0x188);
    if (!p) return -1;
    p = *(const uint64_t *)(p + 0x1c0);
    if (!p) return -1;
    int32_t id = *(const int32_t *)(p + 0x28);
    if (id == 0x11a) id = *(const int32_t *)(p + 0x4c);
    return id;
}

/* Los botones mantenidos ahora mismo: jugador 1 completo; si ese mando no
   responde (desconectado), el portátil. 0 si ninguno contesta. */
static uint64_t leer_botones(void) {
    struct NpadState st;
    const uint32_t id1 = 0; /* NpadIdType::No1 */
    if (GetNpadStatesFullKey(&st, 1, &id1) > 0 && (st.attributes & 1))
        return st.buttons;
    const uint32_t idh = 0x20; /* NpadIdType::Handheld */
    if (GetNpadStatesHandheld(&st, 1, &idh) > 0 && (st.attributes & 1))
        return st.buttons;
    return 0;
}

/* Tras la carga de las banderas en la actualización de movimiento: las
   devuelve con el bit dash mandado por B, y anota el activo compartido. */
uint64_t oot_sprint_link(uint64_t link, uint64_t flags) {
    volatile struct SprintState *s = &oot_sprint_state;
    const uint64_t botones = leer_botones();
    const int tipo = tipo_estado(id_estado(link));
    const int b = (botones & BOTON_B) != 0;

    /* El flanco de subida de B decide la pulsación entera: solo engancha si
       Link ya se movía (en parado, B queda para lo suyo). */
    if (b && !(s->botones_prev & BOTON_B)) s->pestillo = tipo == MOVER;
    if (!b) s->pestillo = 0;
    s->botones_prev = botones;

    const int on = s->pestillo && b && tipo != PARAR && !(botones & BOTON_ZL);
    s->activo = on;
    return on ? (flags | DASH) : (flags & ~DASH);
}

/* Tras la carga del byte bajo en la actualización de postura: el mismo bit,
   según el activo que dejó la actualización de movimiento. */
uint32_t oot_sprint_stance(uint32_t byte) {
    return oot_sprint_state.activo ? (byte | 0x10u) : (byte & ~0x10u);
}

/* Marca del parche (el configurador y la prueba la buscan en la cueva). */
const char oot_sprint_marca[] = "OOTSPRINT1";
