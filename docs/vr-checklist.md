# VR: lista para probar en el visor (TotK 1.2.1)

Lo que sigue se probó solo contra fake_xr (el runtime OpenXR de prueba), no en un visor. Cada punto dice qué mirar en
el Quest 3 (Virtual Desktop / VDXR) y qué opción del menú (F1, pestaña **VR**, o `[vr]` en totk.ini) lo cambia. Si algo
falla, anotá el número del punto y qué viste; el log (`totk.log`, líneas `vr:`) ayuda.

Antes: Virtual Desktop conectado a la PC, `Jugar TOTK VR.bat` desde el escritorio de Virtual Desktop.

## Imagen

1. **Estéreo con profundidad.** El mundo se ve en 3D, sin doble imagen ni "fantasmas" al girar la cabeza. *Riesgo
   conocido:* el antialiasing (TAA) del propio juego mezcla un ojo con el otro (los ojos se dibujan por turnos): si al
   moverte ves bordes temblando o una imagen fantasma, anotalo; en BotW se resolvió con un historial por ojo, en TotK
   todavía no (docs/vr-parity.md).
2. **Campos descentrados** (opción *Off-centre eye fields*, `vr_asymmetric`). Con la opción encendida la imagen debe
   verse igual de alineada que apagada, pero más nítida. Si con la opción encendida ves los ojos desalineados (cuesta
   fusionar, dolor de ojos), apagala y anotalo: puede ser el signo vertical del desplazamiento.
3. **Ventana del escritorio.** En la PC (o en el escritorio de Virtual Desktop) la ventana del juego muestra un solo ojo,
   sin parpadear entre los dos.
4. **HUD.** Por ahora el HUD está dibujado dentro de cada ojo (no separado como en BotW): anotá si molesta o no se lee.

## Vista

5. **Primera persona** (`vr_view = first`, por defecto). Los ojos están en la cabeza de Link. Mirá hacia abajo: puede
   verse el interior del modelo de Link (su cabeza no se oculta todavía en TotK); anotá cuánto molesta.
6. **Altura de los ojos** (`vr_height_offset`): subirla y bajarla cambia la altura al instante.
7. **Tercera persona** (`vr_view = third`) y **distancia** (`vr_third_distance`): 50 % acerca la cámara, 200 % la aleja.
8. **Roomscale** (`vr_roomscale`, primera persona): dar un paso real en la habitación hace caminar a Link hacia
   donde fue tu cabeza, y se detiene cuando llega.

## Controles (esquema BetterVR, `vr_controls = bettervr`)

9. Mano derecha sobre el **hombro derecho** + grip corto: saca el arma (otra vez: la guarda). Grip mantenido 0,5 s:
   menú rápido de armas mientras lo mantenés; al soltar sale la elegida.
10. Mano izquierda sobre el **hombro izquierdo** + grip: el arco. Mantenido: escudos (con arma) o arcos.
11. Mano izquierda en la **cadera izquierda** + grip corto: usa la habilidad elegida (L). Mantenido: la rueda de
    habilidades (L mantenido); al soltar se cierra en la elegida.
12. Las dos manos en la **boca** con grip: silbido.
13. **Gatillo derecho**: con arma ataca, con arco tensa, con habilidad la usa. Sobre el hombro derecho con arma: lanzar.
14. **A** derecho: agarrar/interactuar; mantenido: correr. **B** derecho: saltar. **Y** izquierdo: habilidad / rueda.
    Clic stick izquierdo: agacharse (mantenido: catalejo). Clic stick derecho: inventario (mantenido: mapa).
15. Cada gesto vibra en esa mano.
16. **Zurdo** (`vr_left_handed`): los papeles de las manos se invierten.
17. **Giro**: suave (`vr_turn_speed` cambia la velocidad), por pasos (`vr_snap_degrees`) y "solo cabeza" (el stick
    derecho no gira en primera persona).
18. **Caminar según el control izquierdo** (`vr_walk = controller`): el stick izquierdo camina hacia donde apunta el
    control, no la cabeza.
19. **Vibración del juego** (`vr_rumble`, `vr_rumble_strength`): golpes, explosiones, etc. vibran en los controles.
20. **Combate con movimiento**: tajos y estocadas de la mano derecha atacan; mano izquierda levantada frente a la cara
    cubre; con ZL (gatillo izquierdo) y un empujón rápido de la mano izquierda: parry.

## Agregado después

21. **Viñeta de confort** (`vr_vignette`: off/low/medium/high): al caminar o girar con el stick se oscurecen los bordes;
    caminando en la habitación no.
22. **Controles visibles** (`vr_show_controllers`): un disco celeste en cada control, en menús y en juego.
23. **Posición de juego** (`vr_play_position = standing`): en primera persona, los ojos a tu altura real sobre el piso
    (recentrá parado). `seated`: a la altura de Link.
24. **Cinemáticas** (`vr_events`): en `auto` las cinemáticas (Dm_*) se ven desde su cámara en 3D y las charlas en
    primera persona; `vr_cutscene_bars = 1` las muestra planas en la pantalla virtual. Los videos pregrabados van a la
    pantalla virtual (`vr_movies = screen`).
25. **Sin parpadeos**: en juego no debería aparecer nunca, ni por un cuadro, la pantalla virtual o negro.
26. **Sombras una vez por par** (`vr_shadows_once`): las sombras del sol se ven igual en los dos ojos; si un ojo tiene
    sombras distintas o que tiemblan, ponelo en 0 y anotalo.
27. **DLAA** (`dlaa = 1`, con la DLL de DLSS que baja el instalador): cada ojo tiene su propio historial; no debería
    haber fantasmas de un ojo en el otro. No se pudo probar aquí (sin la DLL).

## Lo que todavía no está en TotK (no hace falta probarlo)

Arco apuntado con la mano y su arco de trayectoria, el arma siguiendo la mano y golpeando por movimiento, agarre a dos
manos, gesto de soltar, menús/carga/pausa en la pantalla virtual, cabeza de Link oculta y brazos siguiendo los
controles, HUD separado. Para encontrarlos hace falta una partida guardada con arma y arco. Cada uno necesita direcciones del
juego (1.2.1) o del renderizador que todavía no se encontraron: docs/vr-parity.md.
