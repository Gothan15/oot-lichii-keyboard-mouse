/* Prueba de correr con B (cave_sprint.c) en el PC, sin el juego: un Link
   falso en búferes de memoria (la cadena de punteros del estado) y unos
   GetNpadStates falsos alimentan la lógica, y se comprueban las reglas
   pedidas por Gothan: B mantenido en movimiento corre, A ya no corre tras
   la voltereta, B en parado no engancha, ZL cancela, y la postura sigue al
   movimiento.
     gcc -O1 test_sprint.c cave_sprint.c -o test_sprint && ./test_sprint */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Copia del struct SprintState de cave_sprint.c (la cueva lo recibe extern). */
struct SprintState { uint64_t botones_prev; int pestillo; int activo; };
volatile struct SprintState oot_sprint_state;

/* El estado del mando falso, con la forma del NpadState de cave_sprint.c. */
typedef struct { uint64_t sampling_number; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes; uint32_t reserved; } NpadState;
static uint64_t botones_fk, botones_hh;
static int fk_ok = 1, hh_ok = 0;
int GetNpadStatesFullKey(NpadState *out, int count, const uint32_t *id)
    __asm__("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj");
int GetNpadStatesFullKey(NpadState *out, int count, const uint32_t *id) {
    (void)count; (void)id;
    if (!fk_ok) return 0;
    out[0].buttons = botones_fk; out[0].attributes = 1;
    return 1;
}
int GetNpadStatesHandheld(NpadState *out, int count, const uint32_t *id)
    __asm__("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj");
int GetNpadStatesHandheld(NpadState *out, int count, const uint32_t *id) {
    (void)count; (void)id;
    if (!hh_ok) return 0;
    out[0].buttons = botones_hh; out[0].attributes = 1;
    return 1;
}

uint64_t oot_sprint_link(uint64_t link, uint64_t flags);
uint32_t oot_sprint_stance(uint32_t byte);

/* El Link falso: la cadena [[[[Link +0x18] +0x270] +0x188] +0x1c0] +0x28. */
static unsigned char buf_link[0x100], buf_p1[0x300], buf_p2[0x200], buf_p3[0x200], buf_p4[0x100];
#define LINK ((uint64_t)buf_link)
#define B (1ull << 1)
#define ZL (1ull << 8)
#define DASH 0x10ull

static void montar_link(void) {
    memset(buf_link, 0, sizeof buf_link); memset(buf_p4, 0, sizeof buf_p4);
    *(uint64_t *)(buf_link + 0x18) = (uint64_t)buf_p1;
    *(uint64_t *)(buf_p1 + 0x270) = (uint64_t)buf_p2;
    *(uint64_t *)(buf_p2 + 0x188) = (uint64_t)buf_p3;
    *(uint64_t *)(buf_p3 + 0x1c0) = (uint64_t)buf_p4;
}
static void estado(int32_t id) { *(int32_t *)(buf_p4 + 0x28) = id; }
static void reiniciar(void) {
    memset((void *)&oot_sprint_state, 0, sizeof oot_sprint_state);
    botones_fk = botones_hh = 0; fk_ok = 1; hh_ok = 0;
    montar_link(); estado(1);
}

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CORRE(f) (((f) & DASH) != 0)

int main(void) {
    /* 1. A ya no corre: andando, sin tocar B, el dash que el juego puso al
       salir de la voltereta se limpia; la postura también lo limpia. */
    reiniciar();
    CHECK(!CORRE(oot_sprint_link(LINK, DASH)), "el dash de A debería limpiarse");
    CHECK(oot_sprint_stance(0x10) == 0, "la postura debería limpiar el dash de A");

    /* 2. Mantener B en movimiento: corre mientras se mantiene. */
    reiniciar();
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "B en movimiento debería correr");
    CHECK(oot_sprint_stance(0) == 0x10, "la postura debería seguir al sprint");
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "B mantenido debería seguir corriendo");
    botones_fk = 0;
    CHECK(!CORRE(oot_sprint_link(LINK, 0)), "al soltar B debería parar");

    /* 3. B en parado no engancha, ni aunque luego empiece a andar con B
       todavía mantenido; hay que soltar y pulsar ya en movimiento. */
    reiniciar(); estado(40);
    botones_fk = B;
    CHECK(!CORRE(oot_sprint_link(LINK, 0)), "B en parado no debería correr");
    estado(1);
    CHECK(!CORRE(oot_sprint_link(LINK, 0)), "la pulsación de parado no debería enganchar al andar");
    botones_fk = 0; oot_sprint_link(LINK, 0);
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "pulsar B ya en movimiento sí debería correr");

    /* 4. ZL cancela mientras se mantiene; al soltarlo, B vuelve a mandar. */
    reiniciar();
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "antes de ZL debería correr");
    botones_fk = B | ZL;
    CHECK(!CORRE(oot_sprint_link(LINK, 0)), "con ZL no debería correr");
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "al soltar ZL debería volver a correr");

    /* 5. Un estado sin sprint (hablar, escena) lo apaga en el acto. */
    reiniciar();
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "antes del estado debería correr");
    estado(7);
    CHECK(!CORRE(oot_sprint_link(LINK, 0)), "en un estado sin sprint debería parar");

    /* 6. Parado un momento con B mantenido (chocar con una pared): el
       sprint sigue enganchado, como en el modo mantener de Triforce. */
    reiniciar();
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "debería correr");
    estado(40);
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "parado con B mantenido debería seguir enganchado");

    /* 7. El estado envuelto 0x11a lee el id real en +0x4c. */
    reiniciar();
    estado(0x11a); *(int32_t *)(buf_p4 + 0x4c) = 1;
    botones_fk = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "el estado envuelto debería resolver a Andar");

    /* 8. Cadena rota: sin sprint y sin cuelgue. */
    reiniciar();
    *(uint64_t *)(buf_link + 0x18) = 0;
    botones_fk = B;
    CHECK(!CORRE(oot_sprint_link(LINK, 0)), "con la cadena rota no debería correr");

    /* 9. Si el mando completo no responde, manda el portátil. */
    reiniciar();
    fk_ok = 0; hh_ok = 1; botones_hh = B;
    CHECK(CORRE(oot_sprint_link(LINK, 0)), "con el portátil, B debería correr");

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
