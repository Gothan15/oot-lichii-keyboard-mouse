/* Prueba de la cámara nativa v2 (oot_cam_native de cave.c) en el PC, sin el juego: una cámara falsa
   en un búfer de memoria y un nn::hid::GetMouseStates falso alimentan la lógica, y se comprueba el
   diseño portado de Triforce: puerta de estado, ángulos propios sin deriva, adopción de los giros
   del stick y de los balanceos grandes, límites de pitch y envolvimiento del yaw.
     clang -O1 -target x86_64-pc-windows-msvc -fuse-ld=lld test_cam.c cave.c -o test_cam.exe && test_cam.exe
   (en Linux/macOS basta clang test_cam.c cave.c -o test_cam) */
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct { long long sampling_number; int x, y, delta_x, delta_y, wy, wx; unsigned buttons, attributes; } MS;
/* Copia exacta del struct State de cave.c (la cueva lo recibe como extern). */
struct State {
    long long last_sampling; int ready; float unused;
    unsigned yaw_calls, pitch_calls; int last_n; unsigned last_attributes;
    long long newest_sampling; int last_x, last_y;
    unsigned long long own_cam;
    float target_yaw, target_pitch;
    float last_out_yaw, last_out_pitch;
    unsigned stick_turns, stick_adopted;
    int target_ok;
    unsigned cam_calls, cam_gated, cam_rejected;
};
volatile struct State oot_mcam_state;
static MS hist[64]; static int nhist = 0; static long long seq = 100; static int px = 640, py = 360;
static void sample(int x, int y) { MS m = {0}; m.sampling_number = ++seq; m.x = x; m.y = y; m.delta_x = x - px; m.delta_y = y - py; m.attributes = 2; px = x; py = y; hist[nhist++ % 64] = m; }
static void move(int dx, int dy) { sample(px + dx, py + dy); }
int nn_hid_GetMouseStates(MS *out, int count) __asm__("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi");
int nn_hid_GetMouseStates(MS *out, int count) { int n = nhist < count ? nhist : count; for (int i = 0; i < n; i++) out[i] = hist[(nhist - 1 - i) % 64]; return n; }
void oot_cam_native(unsigned long long cam);
void oot_cam_rates(float *yaw_rate, float *pitch_rate);

/* La cámara falsa: los mismos desplazamientos que usa la cueva. */
static unsigned char cambuf[0x400];
#define CAM ((unsigned long long)cambuf)
#define F32(off) (*(float *)(cambuf + (off)))
#define U32(off) (*(unsigned *)(cambuf + (off)))
static void cam_setup(float yaw, float pitch, unsigned state) {
    memset(cambuf, 0, sizeof cambuf);
    F32(0x178) = -55.0f; F32(0x17c) = 55.0f;   /* límites de pitch */
    F32(0x1a4) = yaw; F32(0x18c) = pitch;
    U32(0x204) = state;
}
static int fails = 0;
#define K 0.06f
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    memset((void *)&oot_mcam_state, 0, sizeof oot_mcam_state);
    sample(640, 360);

    /* 1. Estado 0 (apuntando en primera persona): el ratón no toca la cámara. */
    cam_setup(10.0f, 5.0f, 0);
    move(50, 20);
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 10.0f) && NEAR(F32(0x18c), 5.0f), "aim: la camara no se mueve (yaw %f pitch %f)", F32(0x1a4), F32(0x18c));
    CHECK(oot_mcam_state.cam_gated >= 1, "aim: contado como gated");

    /* 2. Vista libre (3): el primer cuadro adopta los ángulos y no salta con lo movido en el menú. */
    U32(0x204) = 3;
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 10.0f) && NEAR(F32(0x18c), 5.0f), "reentrada sin salto (yaw %f pitch %f)", F32(0x1a4), F32(0x18c));

    /* 3. El ratón mueve la cámara 1:1 desde el objetivo adoptado. */
    move(10, 4);
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 10.0f - 10 * K) && NEAR(F32(0x18c), 5.0f + 4 * K), "raton 1:1 (yaw %f pitch %f)", F32(0x1a4), F32(0x18c));

    /* 4. Sin deriva: el seguimiento del juego la empuja un poco entre cuadros y la reposición lo anula. */
    F32(0x1a4) += 0.2f; F32(0x18c) -= 0.1f;      /* lo que haría la actualización del juego */
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 10.0f - 10 * K) && NEAR(F32(0x18c), 5.0f + 4 * K), "sin deriva (yaw %f pitch %f)", F32(0x1a4), F32(0x18c));

    /* 5. El stick gira la cámara (lo anota el gancho de tasas): se adopta, no se revierte. */
    { float yr = 1.5f, pr = 0.0f; oot_cam_rates(&yr, &pr); }
    F32(0x1a4) += 1.5f;                            /* el juego aplica el giro del stick */
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 10.0f - 10 * K + 1.5f), "giro de stick adoptado (yaw %f)", F32(0x1a4));

    /* 6. Balanceo grande sin explicación (fijado de objetivo): se adopta. */
    F32(0x1a4) += 12.0f;
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 10.0f - 10 * K + 1.5f + 12.0f), "balanceo adoptado (yaw %f)", F32(0x1a4));

    /* 7. El pitch se limita a los límites de la cámara. El movimiento llega en muestras de como
       mucho media ventana (el ayudante recentra el puntero), así que se alimenta por pasos. */
    for (int i = 0; i < 300; i++) { move(0, 200); oot_cam_native(CAM); }
    CHECK(NEAR(F32(0x18c), 55.0f), "pitch limitado arriba (%f)", F32(0x18c));
    for (int i = 0; i < 600; i++) { move(0, -200); oot_cam_native(CAM); }
    CHECK(NEAR(F32(0x18c), -55.0f), "pitch limitado abajo (%f)", F32(0x18c));

    /* 8. El yaw se envuelve a ±180. */
    cam_setup(179.9f, 0.0f, 4);
    oot_cam_native(CAM);                           /* adopta 179.9 */
    move(-100, 0);                                 /* ratón a la izquierda: yaw sube 100*K = 6 */
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 179.9f + 100 * K - 360.0f), "yaw envuelto (%f)", F32(0x1a4));

    /* 9. Un objeto que no parece cámara no se toca. */
    memset(cambuf, 0, sizeof cambuf);
    unsigned rejected = oot_mcam_state.cam_rejected;
    oot_cam_native(CAM);
    CHECK(oot_mcam_state.cam_rejected == rejected + 1, "objeto invalido rechazado");

    /* 10. Pausa larga en un menú (más muestras de las que caben en la ventana): al volver no hay salto. */
    cam_setup(0.0f, 0.0f, 3);
    oot_cam_native(CAM);                           /* adopta y sincroniza el ratón */
    for (int i = 0; i < 30; i++) move(5, 0);       /* el ratón se mueve con la cámara «apagada» */
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), 0.0f), "tras el menu no hay salto (yaw %f)", F32(0x1a4));
    move(3, 0);
    oot_cam_native(CAM);
    CHECK(NEAR(F32(0x1a4), -3 * K), "y reanuda 1:1 (yaw %f)", F32(0x1a4));

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails;
}
