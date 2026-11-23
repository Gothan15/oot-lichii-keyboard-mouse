/* Cámara de ratón para el Fritchii parcheado (build E0F3CCFA...): la lógica
   que va en el hueco libre tras el código del juego.

   Pieza de juego del diseño nuevo, escrito de cero: Fritchii gana un modo
   «mouse_camera» en el que captura el ratón como un juego de PC y alimenta
   el ratón HID emulado con movimiento relativo de verdad (ver el fork de
   Fritchii, rama camara-raton). Con esa entrega garantizada, el parche solo
   tiene que hacer una cosa: en el punto donde el juego ya convirtió el
   stick derecho en el giro de este cuadro (grados de yaw y de pitch, en el
   módulo 0x625370), sumarle el giro del ratón. El juego aplica la suma por
   su camino normal de «girado a mano», con sus límites de pitch y todo lo
   demás. No hay ángulos propios, ni estados, ni segundo gancho: es el
   diseño v1 del mod original reducido a lo esencial, sin el ayudante
   externo ni los saltos de cursor que había que desenvolver.

   Signos (sacados por ingeniería inversa en su día): el stick a la derecha
   BAJA el yaw y hacia abajo SUBE el pitch; el ratón hace lo mismo. */

#include <stdint.h>

/* Ajustes: grados por unidad de delta del ratón (1 unidad ≈ 1 píxel a
   1080p, que es lo que entrega el Fritchii parcheado). Un pitch negativo
   invierte el eje vertical. El marcador permite localizar el bloque. */
struct FcamSettings { char marker[8]; float yaw_per_px, pitch_per_px; };
__attribute__((section(".rodata.settings"), used))
const volatile struct FcamSettings oot_fcam_settings = {{'O', 'O', 'T', 'F', 'C', 'A', 'M', '1'},
                                                        0.12f, 0.12f};

/* Estado del parche, en un hueco de BSS que el juego no usa (el script de
   construcción fija su dirección; ver build_fcam_ips.py). */
struct FcamState {
    int64_t last_sampling; /* número de muestra del último delta ya aplicado */
    int ready;             /* ya hubo una primera muestra con la que sincronizar */
    /* Diagnóstico, por si hay que mirar la memoria del juego: */
    unsigned calls;        /* veces que el juego calculó el giro de la cámara */
    unsigned moved;        /* de ellas, cuántas llevaron ratón */
    unsigned last_attr;    /* atributos de la última muestra (bit 1: conectado) */
};
extern volatile struct FcamState oot_fcam_state;

/* nn::hid::MouseState (0x28 bytes, la disposición es la del propio emulador)
   y su lectura por el stub PLT del juego. */
struct MouseState {
    int64_t sampling_number;
    int32_t x, y, delta_x, delta_y, delta_wheel_y, delta_wheel_x;
    uint32_t button, attribute;
};
int GetMouseStates(struct MouseState *out, int count)
    __asm__("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi");

#define CONECTADO 2u /* MouseAttribute: bit 1 = is_connected */

/* El movimiento del ratón desde la última llamada, en grados. Devuelve 0
   si no hay nada que aplicar. La contabilidad por número de muestra es la
   del diseño v1: las muestras llegan de más nueva a más vieja y solo se
   suman las que aún no se han aplicado; si entre medias hubo una pausa
   larga (menú, escena, carga) y se perdieron muestras, se tira lo acumulado
   y se resincroniza, para que la cámara no pegue un salto al volver. */
static int mouse_en_grados(float *yaw, float *pitch) {
    struct MouseState s[16];
    volatile struct FcamState *st = &oot_fcam_state;
    const int n = GetMouseStates(s, 16);
    *yaw = 0.0f;
    *pitch = 0.0f;
    if (n <= 0) { st->ready = 0; return 0; }
    st->last_attr = s[0].attribute;
    if (!(s[0].attribute & CONECTADO)) { st->ready = 0; return 0; }
    if (!st->ready) {
        st->last_sampling = s[0].sampling_number;
        st->ready = 1;
        return 0;
    }
    const int64_t last = st->last_sampling;
    if (s[n - 1].sampling_number > last + 1) {
        st->last_sampling = s[0].sampling_number;
        return 0;
    }
    int dx = 0, dy = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (s[i].sampling_number <= last) continue;
        dx += s[i].delta_x;
        dy += s[i].delta_y;
    }
    st->last_sampling = s[0].sampling_number;
    if (dx == 0 && dy == 0) return 0;
    *yaw = -(float)dx * oot_fcam_settings.yaw_per_px;
    *pitch = (float)dy * oot_fcam_settings.pitch_per_px;
    return 1;
}

/* El gancho (hooks_fcam.S) llama aquí con punteros a las copias guardadas
   de s9 (giro de yaw del stick) y s0 (giro de pitch): se les suma el ratón. */
void oot_fcam_rates(float *yaw_rate, float *pitch_rate) {
    float yaw, pitch;
    oot_fcam_state.calls++;
    if (!mouse_en_grados(&yaw, &pitch)) return;
    oot_fcam_state.moved++;
    *yaw_rate += yaw;
    *pitch_rate += pitch;
}
