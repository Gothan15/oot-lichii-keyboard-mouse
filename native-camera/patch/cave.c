/* Native mouse camera: code that runs inside the game (OOT, Switch 2, main build E0F3CCFAF50065AE...).
   Built freestanding for AArch64 and placed in the unused space after the game's code; see build_native_camera_ips.py.

   It reads the mouse through the game's own nn::hid::GetMouseStates (the game already initialises the mouse at boot)
   and turns the movement since the last call into camera angles, one to one: no stick curve, no top speed.

   --- v2 (fork de Gothan15): la lógica de Triforce dentro del invitado ---
   El parche original sumaba el ratón en el punto donde el juego convierte el stick en el giro del
   cuadro (gancho en 0x625370). Eso dejaba dos problemas: al apuntar en primera persona la vista va
   por otro camino y el ratón no movía nada (la «cámara congelada»), y el seguimiento automático del
   juego seguía actuando sobre los ángulos, así que la vista derivaba sola hacia la espalda de Link.

   Triforce (host/mouse_native.c) lo resuelve enganchándose a la ENTRADA de la actualización de la
   cámara del jugador (f_80644c80, desplazamiento de módulo 0x644C80) y guardando sus propios
   ángulos objetivo, que repone en la cámara cada cuadro. Esta versión porta ese diseño al parche:
     - Solo actúa si el estado de la cámara (cam+0x204) es 3 (sigue a Link) o 4 (la mueve el
       jugador). En cualquier otro estado (apuntado, tiendas, otras cámaras) no toca nada y el
       juego manda, igual que en Triforce el ratón vuelve a ser el stick.
     - Guarda sus ángulos objetivo (yaw en cam+0x1a4, pitch en cam+0x18c, límites en cam+0x178)
       y los repone cada cuadro: lo que la actualización del juego haga con los ángulos (suavizado,
       seguimiento, colisiones) no se acumula → no hay deriva.
     - Si el stick giró la cámara (lo anota el gancho viejo, que ya no suma el ratón), o si los
       ángulos cambiaron de golpe sin explicación (fijado de objetivo, cámaras de guion), adopta
       los ángulos actuales como nuevo objetivo en vez de pelearse con el juego.
   El diseño completo y las verificaciones pendientes están en docs/DISENO-CAMARA-NATIVA.md. */

typedef struct {
    long long sampling_number;
    int x, y;                 /* position in the 1280x720 picture */
    int delta_x, delta_y;
    int wheel_delta_x, wheel_delta_y;
    unsigned buttons;
    unsigned attributes;      /* bit 0: transferable, bit 1: connected (nn::hid::MouseAttribute) */
} MouseState;                 /* 40 bytes, newest entry first */

extern int nn_hid_GetMouseStates(MouseState *out, int count) __asm__("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi");

/* Settings block. Apply-Settings.ps1 finds the marker in the .ips and writes the two floats:
   degrees per mouse step for yaw and pitch (negative pitch = inverted up/down). */
struct Settings { char marker[8]; float yaw_per_step, pitch_per_step; };
__attribute__((section(".rodata.settings"), used))
const volatile struct Settings oot_mcam_settings = {{'O', 'O', 'T', 'M', 'C', 'A', 'M', '1'},
                                                   0.06f, 0.06f};   /* degrees per step; 0.06 = mouse_sens 5 */

struct State {
    long long last_sampling; int ready; float unused;
    /* diagnostics, readable in guest memory at state + 0x10 (cost: a few stores per frame) */
    unsigned yaw_calls, pitch_calls; int last_n; unsigned last_attributes;   /* calls, calls with movement */
    long long newest_sampling; int last_x, last_y;
    /* cámara nativa v2: ángulos propios y control de adopciones (ver oot_cam_native) */
    unsigned long long own_cam;        /* cámara de la que somos dueños (0 = ninguna) */
    float target_yaw, target_pitch;    /* nuestros ángulos objetivo */
    float last_out_yaw, last_out_pitch;/* lo último que escribimos en la cámara */
    unsigned stick_turns;              /* giros del stick anotados por el gancho de tasas */
    unsigned stick_adopted;            /* valor de stick_turns ya adoptado */
    int target_ok;                     /* el objetivo es válido (estamos en vista libre) */
    unsigned cam_calls, cam_gated, cam_rejected;   /* diagnostics: llamadas, fuera por estado, no parecía cámara */
};
extern volatile struct State oot_mcam_state;   /* placed in spare game BSS by the linker script */

#define WRAP_X 1024  /* must match mouse_capture.h: 80% of the 1280x720 picture */
#define WRAP_Y 576

static int unwrap(int d, int period) {
    while (d > period / 2) d -= period;
    while (d < -period / 2) d += period;
    return d;
}

/* Mouse movement since the previous call, in degrees (game camera units). Returns 0 when there is none. */
int oot_mouse_delta(float *yaw, float *pitch) {
    MouseState s[16];
    int n = nn_hid_GetMouseStates(s, 16);
    oot_mcam_state.last_n = n;
    if (n > 0) {
        oot_mcam_state.last_attributes = s[0].attributes;
        oot_mcam_state.newest_sampling = s[0].sampling_number;
        oot_mcam_state.last_x = s[0].x;
        oot_mcam_state.last_y = s[0].y;
    }
    *yaw = 0.0f;
    *pitch = 0.0f;
    if (n <= 0 || !(s[0].attributes & 2)) {   /* not connected (Lichii mouse_enabled is off) */
        oot_mcam_state.ready = 0;
        return 0;
    }
    if (!oot_mcam_state.ready) {           /* first sample: only remember where we are */
        oot_mcam_state.last_sampling = s[0].sampling_number;
        oot_mcam_state.ready = 1;
        return 0;
    }
    long long last = oot_mcam_state.last_sampling;
    if (s[n - 1].sampling_number > last + 1) {
        /* the camera did not run for a while (menu, cutscene, loading): drop what the mouse did meanwhile */
        oot_mcam_state.last_sampling = s[0].sampling_number;
        return 0;
    }
    int dx = 0, dy = 0;
    for (int i = n - 1; i >= 0; i--) {     /* oldest to newest */
        if (s[i].sampling_number <= last) continue;
        /* The mouse helper keeps the pointer near the middle of the picture by moving it back exactly
           WRAP_X (or WRAP_Y) game pixels whenever it gets more than half of that from the middle, like a
           clock hand going from 12 to 1. Any jump of about that size is such a move: unwrap it.
           The hand can move up to half of WRAP per sample (512 x 288 px) before a move is read as a jump back.
           Samples come every ~8 ms, but a busy emulator can leave tens of ms between position updates, so the
           helper also makes one mouse count worth only mouse_step (0.5) px. */
        dx += unwrap(s[i].delta_x, WRAP_X);
        dy += unwrap(s[i].delta_y, WRAP_Y);
    }
    oot_mcam_state.last_sampling = s[0].sampling_number;
    if (dx == 0 && dy == 0) return 0;
    /* Game camera (found by RE, see README): stick right lowers yaw, stick down raises pitch.
       Mouse right / mouse down must do the same. */
    *yaw = -(float)dx * oot_mcam_settings.yaw_per_step;
    *pitch = (float)dy * oot_mcam_settings.pitch_per_step;
    return 1;
}

/* Called by hooks.S each frame in the player camera update (FUN_81199c80, module 0x625370), right after the game
   turned the right stick into this frame's turn: yaw and pitch, in degrees, before it checks whether the camera
   is being turned by hand.
   v1 sumaba aquí el ratón; la v2 ya no: la cámara se mueve desde su entrada (oot_cam_native). Este gancho
   queda solo como testigo: anota que el stick (o las teclas de cámara del emulador, que llegan como stick)
   giró la cámara, para que la entrada adopte los ángulos resultantes en vez de sobrescribirlos. */
void oot_cam_rates(float *yaw_rate, float *pitch_rate) {
    oot_mcam_state.yaw_calls++;
    if (*yaw_rate != 0.0f || *pitch_rate != 0.0f)
        oot_mcam_state.stick_turns++;
}

/* ------------------------------------------------------------------ */
/* Cámara nativa v2: la entrada de la actualización de la cámara.      */
/* Los desplazamientos son los de Triforce (host/mouse_native.c):      */
/* límites de pitch +0x178 (mín, máx), pitch +0x18c, yaw +0x1a4,        */
/* estado +0x204. Son campos de la estructura de la cámara del juego,  */
/* así que valen igual dentro del emulador.                            */
/* ------------------------------------------------------------------ */
#define CAM_LIMITS 0x178
#define CAM_PITCH  0x18c
#define CAM_YAW    0x1a4
#define CAM_STATE  0x204

/* Si los ángulos cambiaron al menos esto desde lo último que escribimos, sin que el stick lo
   explique, el juego está dirigiendo la cámara (fijado de objetivo, guion): se adopta.
   El seguimiento automático se mueve en fracciones de grado por cuadro, muy por debajo. */
#define SWING_DEG 3.0f

static float cam_getf(unsigned long long cam, unsigned off) {
    return *(volatile float *)(cam + off);
}
static void cam_putf(unsigned long long cam, unsigned off, float v) {
    *(volatile float *)(cam + off) = v;
}
static unsigned cam_getu(unsigned long long cam, unsigned off) {
    return *(volatile unsigned *)(cam + off);
}
static int finite_f(float f) {   /* sin libm: el exponente no puede ser todo unos (inf/NaN) */
    union { float f; unsigned u; } v;
    v.f = f;
    return (v.u & 0x7f800000u) != 0x7f800000u;
}
static float wrap_deg(float d) {   /* diferencia de yaw llevada a [-180, 180] */
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

/* La misma criba que looks_like_camera en Triforce: la función enganchada es un método virtual
   y este objeto tiene que parecer la cámara del jugador antes de escribir en él. */
static int looks_like_camera(unsigned long long cam) {
    if (!cam) return 0;
    float lo = cam_getf(cam, CAM_LIMITS), hi = cam_getf(cam, CAM_LIMITS + 4);
    float pitch = cam_getf(cam, CAM_PITCH), yaw = cam_getf(cam, CAM_YAW);
    if (!finite_f(lo) || !finite_f(hi) || !finite_f(pitch) || !finite_f(yaw)) return 0;
    if (!(lo < hi) || lo < -90.5f || hi > 90.5f) return 0;
    if (pitch < lo - 1.0f || pitch > hi + 1.0f) return 0;
    if (yaw < -360.0f || yaw > 360.0f) return 0;
    return 1;
}

/* Called by hooks.S at the very entry of the player camera update (f_80644c80, module 0x644C80),
   with the camera object in x0, before the game reads its input for this frame. */
void oot_cam_native(unsigned long long cam) {
    volatile struct State *st = &oot_mcam_state;
    st->cam_calls++;
    if (!looks_like_camera(cam)) { st->cam_rejected++; return; }
    /* Solo en vista libre: siguiendo a Link (3) o movida por el jugador (4). Al apuntar en primera
       persona esta cámara queda en estado 0 mientras dibuja otra; en tiendas pasa lo mismo. Ahí no
       tocamos nada: el juego (y el «mouse como stick» del emulador, si está activo) manda, y el
       objetivo se invalida porque los ángulos pueden cambiar mientras otra cámara posee la vista. */
    unsigned state = cam_getu(cam, CAM_STATE);
    if (state != 3 && state != 4) { st->target_ok = 0; st->cam_gated++; return; }

    float cur_yaw = cam_getf(cam, CAM_YAW), cur_pitch = cam_getf(cam, CAM_PITCH);
    int adopt = 0;
    if (!st->target_ok || st->own_cam != cam) adopt = 1;              /* cámara nueva o vuelta de otra vista */
    else if (st->stick_turns != st->stick_adopted) adopt = 1;        /* el stick la giró: su resultado manda */
    else {                                                           /* balanceo grande sin explicación: el juego dirige */
        float dyaw = wrap_deg(cur_yaw - st->last_out_yaw);
        float dpitch = cur_pitch - st->last_out_pitch;
        if (dyaw >= SWING_DEG || dyaw <= -SWING_DEG || dpitch >= SWING_DEG || dpitch <= -SWING_DEG)
            adopt = 1;
    }
    if (adopt) {
        st->own_cam = cam;
        st->target_yaw = cur_yaw;
        st->target_pitch = cur_pitch;
        st->target_ok = 1;
        st->stick_adopted = st->stick_turns;
    }

    float dyaw, dpitch;
    if (oot_mouse_delta(&dyaw, &dpitch)) {   /* el lector de siempre: desenvuelve los saltos del ayudante */
        st->pitch_calls++;                   /* frames that had mouse movement */
        st->target_yaw += dyaw;
        st->target_pitch += dpitch;
    }
    /* Envolver el yaw y limitar el pitch a los límites propios de la cámara, como Triforce. */
    while (st->target_yaw > 180.0f) st->target_yaw -= 360.0f;
    while (st->target_yaw < -180.0f) st->target_yaw += 360.0f;
    float lo = cam_getf(cam, CAM_LIMITS), hi = cam_getf(cam, CAM_LIMITS + 4);
    if (st->target_pitch < lo) st->target_pitch = lo;
    if (st->target_pitch > hi) st->target_pitch = hi;

    /* Reponer cada cuadro, haya o no movimiento: la actualización del juego corre después
       (suavizado, seguimiento, colisiones), pero lo que haga con los ángulos no se acumula. */
    cam_putf(cam, CAM_YAW, st->target_yaw);
    cam_putf(cam, CAM_PITCH, st->target_pitch);
    st->last_out_yaw = st->target_yaw;
    st->last_out_pitch = st->target_pitch;
}
