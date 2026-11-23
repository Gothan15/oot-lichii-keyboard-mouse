/* Prueba de la cámara de ratón de Fritchii (cave_fcam.c) en el PC, sin el
   juego: un GetMouseStates falso sirve muestras guionizadas (de más nueva a
   más vieja, como el de verdad) y se comprueba la contabilidad por número
   de muestra y los signos del giro.
     gcc -O1 test_fcam.c cave_fcam.c -o test_fcam && ./test_fcam */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* Copia del struct FcamState de cave_fcam.c (la cueva lo recibe extern). */
struct FcamState { int64_t last_sampling; int ready; unsigned calls, moved, last_attr; };
volatile struct FcamState oot_fcam_state;

/* El ratón falso: un historial de muestras; la lectura devuelve las últimas
   pedidas, de más nueva a más vieja, como el nn::hid de verdad. */
typedef struct { int64_t sampling_number; int32_t x, y, delta_x, delta_y, delta_wheel_y, delta_wheel_x; uint32_t button, attribute; } MouseState;
static MouseState hist[64];
static int nhist = 0;
int GetMouseStates(MouseState *out, int count)
    __asm__("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi");
int GetMouseStates(MouseState *out, int count) {
    int n = nhist < count ? nhist : count;
    for (int i = 0; i < n; i++) out[i] = hist[nhist - 1 - i];
    return n;
}
static void muestra(int64_t num, int dx, int dy, uint32_t attr) {
    MouseState *s = &hist[nhist++];
    memset(s, 0, sizeof *s);
    s->sampling_number = num; s->delta_x = dx; s->delta_y = dy; s->attribute = attr;
}

void oot_fcam_rates(float *yaw_rate, float *pitch_rate);

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int cerca(float a, float b) { return fabsf(a - b) < 0.001f; }
static void reiniciar(void) {
    memset((void *)&oot_fcam_state, 0, sizeof oot_fcam_state);
    nhist = 0;
}

int main(void) {
    float yaw, pitch;

    /* 1. Sin muestras: las tasas quedan como estaban. */
    reiniciar();
    yaw = 1.0f; pitch = 2.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 1.0f) && cerca(pitch, 2.0f), "sin muestras no debería tocar las tasas");

    /* 2. Ratón desconectado (sin el bit de conectado): tampoco, y sin
       sincronizar. */
    reiniciar();
    muestra(1, 50, 50, 0);
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 0.0f) && cerca(pitch, 0.0f), "desconectado no debería girar");
    CHECK(oot_fcam_state.ready == 0, "desconectado no debería sincronizar");

    /* 3. La primera muestra conectada solo sincroniza: aunque traiga
       deltas grandes, la cámara no salta. */
    reiniciar();
    muestra(1, 500, 500, 2);
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 0.0f) && cerca(pitch, 0.0f), "la primera muestra no debería girar");

    /* 4. Movimiento normal: se suma a lo que traía el stick, con los signos
       del juego (derecha baja el yaw, abajo sube el pitch) y 0.12 por
       unidad. dx=10, dy=20: yaw -= 1.2, pitch += 2.4. */
    muestra(2, 10, 20, 2);
    yaw = 0.5f; pitch = 0.5f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 0.5f - 1.2f), "el yaw debería bajar 1.2 (dio %f)", yaw);
    CHECK(cerca(pitch, 0.5f + 2.4f), "el pitch debería subir 2.4 (dio %f)", pitch);

    /* 5. Sin muestras nuevas, no se reaplica nada. */
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 0.0f) && cerca(pitch, 0.0f), "sin novedad no debería girar");

    /* 6. Varias muestras nuevas de golpe: se suman una sola vez. */
    muestra(3, 5, 0, 2);
    muestra(4, 5, 0, 2);
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, -1.2f), "dos muestras de 5 deberían dar -1.2 (dio %f)", yaw);
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 0.0f), "lo ya aplicado no debería repetirse");

    /* 7. Tras una pausa larga se pierden muestras: la ventana de 16 que
       devuelve la lectura ya no alcanza a la última aplicada (la más vieja
       disponible es la 15 y la última aplicada era la 4), así que lo
       acumulado se tira y se resincroniza; la siguiente ya aplica normal. */
    for (int i = 5; i < 30; i++) muestra(i, 100, 100, 2);
    muestra(30, 100, 100, 2);
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, 0.0f) && cerca(pitch, 0.0f), "tras la pausa no debería saltar");
    muestra(31, 10, 0, 2);
    yaw = 0.0f; pitch = 0.0f;
    oot_fcam_rates(&yaw, &pitch);
    CHECK(cerca(yaw, -1.2f), "tras resincronizar debería volver a girar (dio %f)", yaw);

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
