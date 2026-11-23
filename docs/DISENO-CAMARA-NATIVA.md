# Diseño: cámara nativa estilo Triforce para el parche invitado (Fritchii/Lichii)

Documento de trabajo del fork (Gothan15/oot-lichii-keyboard-mouse, rama `camara-nativa-triforce`).
Resume la auditoría del mod original (v1.4.3, de joaobyss/exgde) y del diseño de Triforce
(`host/mouse_native.c`, `host/nn_hid.c`, `nx2/oot_patches.py` de triforce/source), y fija cómo se porta
ese diseño al parche IPS que corre dentro del juego bajo el emulador.

## El problema (lo que reporta la gente y se ve en el código)

El parche actual tiene UN solo gancho: módulo `0x625370`, dentro de la función que convierte el stick
derecho en el giro del cuadro (yaw en s9, pitch en s0). Suma ahí los grados del ratón leídos con
`nn::hid::GetMouseStates`. Consecuencias:

1. **Al apuntar (tirachinas, arco) la vista va por otro camino** que no pasa por ese punto: el ratón
   no mueve nada → «la cámara se queda fija» (issues #5 y #3 del repo original).
2. **La cámara deriva sola** hacia la espalda de Link: el seguimiento automático del juego sigue
   actuando sobre los ángulos entre cuadros y el parche no lo compensa (el README original lo acepta
   como normal: «exactly as with the stick»).
3. **Techo en giros rápidos**: el emulador entrega la *posición* del puntero (~cada 8 ms, peor con la
   CPU ocupada) y el desenvolvimiento (unwrap) admite como máximo media vuelta de ventana por muestra
   (512×288 px del cuadro 1280×720). De ahí los tirones que describe la propuesta de mejora (PR #4).

## El diseño de Triforce (verificado en su código)

Triforce (heredado de Hookshot) engancha la **entrada de la actualización de la cámara del jugador**
(`f_80644c80`, el puntero a la cámara llega en x0) y, una vez por cuadro:

- Lee el **estado de la cámara** en `cam+0x204`. Solo actúa si es **3** (sigue a Link) o **4** (la mueve
  el jugador). Al apuntar en primera persona esa cámara queda en estado 0 mientras dibuja otra: el
  ratón vuelve a ser stick y el apuntado funciona con el camino normal del juego.
- Exige además que **no estén pulsados ZR, L ni R** (los botones con los que el juego se reserva el
  stick: apuntado con el arco, rueda de objetos). Lo calcula en cada lectura del mando
  (`hos_mouse_native_free` en nn_hid.c: `game_stick = ZR|L|R`).
- Guarda **sus propios ángulos objetivo** (yaw/pitch) y los **repone en la cámara cada cuadro**
  (pitch en `cam+0x18c`, yaw en `cam+0x1a4`, límites de pitch en `cam+0x178`): la actualización del
  juego sigue corriendo (suavizado, seguimiento, colisiones), pero lo que haga con los ángulos no se
  acumula → **no hay deriva**.
- Si el stick real (o las teclas de cámara) giró la cámara, adopta los ángulos resultantes como nuevo
  punto de partida (`stick_held`): ratón y stick no se pelean.
- Sensibilidad: 0,1° por cuenta a sensibilidad 1, sin zona muerta, sin curva, sin tope de velocidad.
  Yaw envuelto a ±180°, pitch limitado a los límites propios de la cámara.

Los desplazamientos `+0x178/+0x18c/+0x1a4/+0x204` son de la **estructura de la cámara del juego**, no
del anfitrión: valen igual dentro del emulador.

## El port al parche invitado

### Gancho nuevo
- Entrada de la función de la cámara: desplazamiento de módulo **`0x644C80`**, confirmado por
  triangulación entre fuentes: el recompilador trabaja con la imagen del `main` en la base
  `0x80020000` (constante `MAIN` en `nx2/oot_occlusion.py` y `nx2/oot_wide.py`; anclas
  `main+0x65234c = 0x8067234c` y `f_8010b380 = main+0xeb380`), así que `f_80644c80` cae en el
  desplazamiento `0x644C80`. La misma base convierte el «0x84e9f680» del readme del L3 Sprint
  Toggle en el desplazamiento `0x4E9F680`, a 0x7C0 del BSS libre que ya usa este parche
  (`0x4E9FE40`): coherente. Lo que sigue **A VERIFICAR** con el `main` descomprimido es la
  instrucción original en `0x644C80` (el script la comprueba con `--main` antes de publicar).
- El stub en hooks.S sigue el patrón existente: guarda todo, llama a la lógica en C con x0 = cámara,
  rehace la instrucción sustituida y vuelve.

### Lógica en la cueva (nueva función, p. ej. `oot_cam_native`)
1. Si la cámara no «parece cámara» (límites finitos y ordenados dentro de ±90, ángulos finitos,
   |yaw| ≤ 360 — el `looks_like_camera` de Triforce), salir.
2. Estado = u32 en `cam+0x204`. Si no es 3 ni 4: marcar «sin objetivo» (target_ok = 0) y salir.
   El ratón no se inyecta; el apuntado y demás vistas quedan en manos del juego (y del Mouse Panning
   del emulador, si el usuario lo usa, igual que el «mouse como stick» de Triforce).
3. Puerta de botones (ZR, L o R pulsados → target_ok = 0 y salir): **queda fuera de la v1**.
   Dentro del invitado no hay una fuente de botones confirmada: el recorrido del gestor de
   controladores (puntero en el BSS del módulo `0x4E9F680`, «hold» en `+0x114` por mando, según el
   readme del L3 Sprint Toggle) está cosido en su pchtxt en ~100 fragmentos de 2-3 instrucciones
   entre funciones, y el stub PLT de `nn::hid::GetNpadStates` solo se localiza con el ELF a mano.
   En su lugar, la v1 usa dos redes que cubren los casos de la puerta en la práctica:
   - **Adopción por giro del stick**: el gancho viejo (`0x625370`) deja de sumar el ratón y pasa a
     anotar los giros que el stick (o las teclas de cámara) hizo; al entrar, si hubo giro de stick,
     los ángulos actuales se adoptan como objetivo antes de sumar el ratón.
   - **Adopción por balanceo grande**: si al entrar los ángulos difieren de los últimos que
     escribimos en ≥ 3° sin explicación (fijado de objetivo que gira la cámara, cámaras de guion),
     se adoptan. El seguimiento automático mueve fracciones de grado por cuadro, así que la
     anti-deriva no se ve afectada. La puerta de botones exacta queda para la v1.1, con el ELF.
4. Si no hay objetivo válido, la cámara cambió o el stick giró la cámara: adoptar los ángulos actuales
   (yaw `cam+0x1a4`, pitch `cam+0x18c`) como objetivo. Detectar «el stick giró» sin anfitrión: comparar
   los ángulos al entrar con los que dejamos el cuadro anterior; si difieren sin que los moviéramos
   nosotros, alguien (el juego/el stick) los movió → readoptar. (Equivalente invitado del
   `stick_held` de Triforce.)
5. Leer el movimiento del ratón con el lector actual (`oot_mouse_delta`, se reutiliza tal cual,
   incluido el unwrap para el ayudante) y sumarlo al objetivo: yaw −= dx·k, pitch += dy·k
   (k según los ajustes OOTMCAM1 que escribe Apply-Settings; ojo: el parche actual usa 0,06°/paso a
   sensibilidad 5 — al portar se mantiene la escala de ajustes existente para no romper el
   configurador, no la de Triforce).
6. Envolver yaw a ±180 y limitar pitch a `[cam+0x178, cam+0x17C]`.
7. **Reponer** yaw y pitch en la cámara. Cada cuadro, haya o no movimiento: eso es lo que mata la
   deriva.

### Qué pasa con el gancho viejo (0x625370)
Se retira: con el gancho de entrada escribiendo los ángulos, sumar tasas en el punto del stick
duplicaría el giro. El modo «As the stick» no usa ningún gancho (es configuración del emulador), así
que apagar la cámara nativa sigue siendo renombrar el .ips a `.ips.off`, como hoy.

### El ayudante (OoT-MouseCapture) — fase siguiente
Se queda (en el emulador no hay entrada cruda de ratón: el juego solo ve la posición). Mejoras de la
propuesta comunitaria (PR #4): arranque automático esperando al juego, temporizador más preciso,
`mouse_step` menor para dar más aire a los giros rápidos, ocultar el cursor mientras controla.
El techo físico (posición cada ~8 ms) no desaparece: se mitiga, no se elimina.

### Iconos estilo Triforce — hecho (botones de la ocarina)
La definición de Triforce (nx2/prompt_keys.py): una tecla es «un cuadrado redondeado (más ancho
para una palabra) con su etiqueta recortada en Libertinus Sans Bold». En este mod los iconos no se
envían como texturas: se **generan al aplicar los ajustes** desde los archivos del propio juego.
El estilo se aplicó donde el mod posee el gráfico entero, las texturas `OcarinaButton*` (93×93,
las notas de la ocarina y la partitura): la silueta redonda del juego se sustituye por la tapa
redondeada oscura ocupando la misma caja (el anclaje del layout no se mueve) y la etiqueta se
recorta en claro. Elección en el configurador («Button icons (ocarina / song sheet)»: Game buttons
/ Key caps), ajuste `icon_style` en el ini; la herramienta suelta lo toma de `OOT_ICON_STYLE`.
Los botones del HUD (`KeyTexFont*`) no se tocaron: ahí la forma la pone el layout del juego y la
textura es solo el símbolo — meter una tapa dentro dibujaría dos veces. La fuente de iconos del
Drawer (avisos en menús) queda como paso siguiente si el aspecto convence en la partitura.

### Fritchii
- El parche es del juego, no del emulador: vale para Lichii y Fritchii igual (mismo build
  `E0F3CCFA…`). Lo específico de Fritchii va en el instalador/configurador: su carpeta de datos
  (`%APPDATA%\fritchii`) y su ejecutable (`fritchii.exe` desde la v6).
- Verificación final en la PC de Gothan: el sandbox no puede correr Fritchii con su volcado.

## Pendientes de verificación (necesitan el main descomprimido o la PC de Gothan)
1. Instrucción original en el desplazamiento del gancho de entrada (`0x644C80`, ya confirmado):
   hace falta para que el stub la rehaga al volver.
2. Para la v1.1: bits de ZR/L/R en el campo «hold» del gestor de controladores (o el stub PLT
   de GetNpadStates).
3. Que el área de la cueva (`0x3C46400`) sigue libre con el código nuevo (crece: hay ~3 KB hasta
   `0x3C47000`; el parche actual usa unos cientos de bytes).
