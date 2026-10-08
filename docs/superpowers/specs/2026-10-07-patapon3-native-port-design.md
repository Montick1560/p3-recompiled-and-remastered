# Patapon 3 DxD — Port nativo por recompilación estática (diseño)

- **Fecha:** 2026-10-07
- **Estado:** aprobado por secciones en conversación; pendiente de revisión del documento escrito
- **Alcance:** proyecto completo (hoja de ruta M0–M7) + diseño detallado de M0 (base), M1 (audio) y M2 (overlays)

## 1. Objetivo

Obtener un ejecutable nativo de Windows que ejecute **"Patapon 3 - DxD Edition v1.1.4"** (mod del usuario, base UCES-01421 EUR) sin emulador, de principio a fin, para uso personal.

**Criterio de éxito final:** terminar el juego completo en el port, con gráficos, audio, videos, controles y guardado funcionando como en PPSSPP.

**Fuera de alcance (hasta después de M7):** multijugador ad-hoc/PSN, mejoras (resolución, 60 FPS, widescreen), Vulkan, Linux/macOS, redistribución. Los datos del juego nunca se distribuyen ni se versionan.

## 2. Hechos de partida (verificados en la prueba del 2026-10-07)

- ISO `INFN00001.iso`; EBOOT descifrado con PPSSPP → `INFN00001_EBOOT.BIN`: ELF ET_EXEC en 0x08804000, entrada 0x08869238. Su `.comment` dice "Custom ISO made by Madwig." → el código del EBOOT está modificado por el mod; por eso se recompila ese EBOOT y no el retail.
- Overlays de código en `USRDIR/overlay/`: `OL_Title.bin` (id 3), `OL_Mission.bin` (id 2), `OL_Azito.bin` (id 1). Formato `MWo3`: cabecera de 0x40 bytes `[magic, id, load_addr, text_size, data_size, bss_size, ctor_start, ctor_end, nombre…]`, seguida de texto y datos. **El archivo completo (cabecera incluida) se carga tal cual en 0x08ABB180**; la ventana de overlays es 0x08ABB180–0x08BC6480. Enlazados a dirección fija (sin relocaciones). El ELF los declara como secciones `OL_*.bin`.
- Cargador de overlays en el EBOOT: `FUN_08872374` construye `/PSP_GAME/USRDIR/overlay/`; tabla de nombres en 0x08A74D5C usada por `FUN_08a15b8c`.
- El EBOOT hace 858 saltos/llamadas estáticos a direcciones dentro de la ventana de overlays.
- Prueba con psprecomp en Windows: análisis Ghidra 11,069 funciones, 352 imports; recompilación 18,370 funciones; el `.exe` arranca, crea `user_main`, carga `DATA_CMN.BND`, `DATAMS.HED/BND`, presenta 4 frames y se detiene porque los hilos de audio SGX esperan a `sceAudio` (no implementado). 135/352 imports sin implementar.
- Música de batalla (`SOUND/BGM_xx/BGM.DAT`): contenedor BND con ~29 SGXD `ptpat_battle_{base,level1..3}_NN.sgd`, cada uno con un clip **ATRAC3plus** (RIFF); el juego encadena fragmentos al ritmo según combo/fiebre. Efectos (`.SGD`) en VAG/PS-ADPCM vía SAS.

## 3. Arquitectura

### 3.1 Organización

```
deco/
├─ psprecomp/            fork de wizardengineer/psprecomp (GPL-2.0), rama de trabajo
│  ├─ crates/            pipeline Rust: analyze → decode → emit C++
│  ├─ runtime/           runtime C++ genérico (HLE del sistema PSP, GE→OpenGL, scheduler)
│  ├─ games/patapon3/    manifiesto (game.toml) + hooks específicos del juego
│  └─ docs/superpowers/  specs y planes
├─ tools/                Ghidra 12.0.2 + ghidra-allegrex v21.3, SDL2 2.32.10 (mingw), zlib 1.3.1 — fuera de git
├─ disc0/                ISO extraído — fuera de git
├─ build/                C++ generado y compilación — desechable
└─ INFN00001.iso, INFN00001_EBOOT.BIN
```

### 3.2 Flujo

```
EBOOT.BIN ─┐                         ┌─ banco main
OL_*.bin  ─┴─► analyze ─► recompile ─┼─ banco OL_Title
                                     ├─ banco OL_Mission   ─► runtime ─► .exe ◄─ disc0/
                                     └─ banco OL_Azito
```

### 3.3 Reglas

1. Funcionalidad del sistema PSP (audio, ATRAC, MPEG, utility, red) → `runtime/src` (genérico). Solo lo propio de Patapon 3 → `games/patapon3/`.
2. El `.exe` lee los datos de `disc0/` en tiempo de ejecución; nunca se incrustan ni versionan datos del juego.
3. El C++ generado no se edita a mano; se corrige el emisor y se regenera.
4. Commits separados para cambios genéricos (port Windows, decodificador, dispatch) y para los específicos de Patapon 3.
5. Plataforma: Windows x64, clang (llvm-mingw), Ninja, SDL2 + OpenGL 3.3.

### 3.4 Referencias

- **PPSSPP**: oráculo de comportamiento y referencia de semántica HLE (GPL-2.0+; compatible).
- **efonte/patapon-re**: símbolos de UCES01421 (12,596 funciones), formatos (Kaitai), volcados PAC, cheats como mapa de memoria.
- Herramientas externas de verificación (no se integran al port; sin copiar código salvo licencia compatible):
  - LibBND2 (listar/extraer BND), PacViewer (`p3_instruction_set.bin`, GPL-3 → solo como herramienta), libP3Hash (referencia del cifrado del juego), LBRTPlayer (referencia de música P1/P2).
  - yaponmdl (BSD-2; visor GMO/GXX/GXP/GXT y docs de formatos) y GXXTool3 (parser GXX con constantes GE; sin licencia) → oráculos visuales para M3.
  - PataponAllocators (Nemoumbra; sin licencia): reimplementación de los heaps del juego (ListHeap, SemaHeap, VolatileHeap…) → referencia para validar la HLE de memoria (`sceKernelVolatileMem*`, particiones) en M1–M3.
  - PacEngine (Nemoumbra; sin licencia): reimplementación del motor de scripts PAC con depurador → referencia para diagnosticar bloqueos de misiones/eventos en M5–M7.

## 4. Hoja de ruta

| Hito | Contenido | Criterio de éxito (verificado ejecutando) |
|---|---|---|
| **M0** Base | Commit de los arreglos de la prueba; script único (extraer ISO → analyze → recompile → build → run); `game.toml` de patapon3 | Desde cero, un comando produce el `.exe` y llega al mismo punto que la prueba |
| **M1** Audio | `sceAudio` sobre SDL2, SAS con síntesis real, `sceAtrac` con ATRAC3/ATRAC3plus | Sin `STUCK` en hilos de audio; el juego avanza más allá del punto de la prueba; se oye música |
| **M2** Overlays | Bancos recompilados para los 3 overlays y selección por huella | El código de `OL_Title` se ejecuta sin `LOOKUP_MISS` en código real |
| **M3** Título | Corrección GE (texturas, CLUT/swizzle, modelos, fuentes); stub temporal de video ("terminado") | Título igual que en PPSSPP (comparación de capturas) |
| **M4** Guardado y diálogos | `sceUtility`: savedata, msgdialog, osk; compatibilidad con partidas de PPSSPP | Nueva partida, guardar/cargar; cargar una partida existente de PPSSPP |
| **M5** Jugar | Escondite, primera misión, controles teclado/gamepad, sincronía ritmo-audio | El usuario termina la primera misión tocando normalmente |
| **M6** Videos | `sceMpeg`/`scePsmf` con FFmpeg | Cinemáticas con audio |
| **M7** Completo | Red/PSN como "sin conexión", errores al avanzar, rendimiento, empaquetado | Terminar DxD de principio a fin |

Cada hito tiene su propio plan de implementación. Mejoras opcionales solo después de M7.

## 5. Diseño de M0 — Base

- Rama del fork con commits separados para lo hecho en la prueba:
  - Port Windows: `analyzeHeadless.bat` + `std::env::temp_dir()`; `VirtualAlloc`; Winsock; `std::signal`; `O_BINARY`; CMake WIN32 (SDL2 por config package, zlib vía `FETCHCONTENT_SOURCE_DIR_ZLIB`, sin SDL2main, `ws2_32`).
  - Emisor: tabla de dispatch como arreglo constante + bucle (evita que clang -O2 se atasque con ~20k asignaciones).
  - Decodificador: `wsbh`/`wsbw` en SPECIAL3/BSHFL (sub 2/3); `round/ceil/floor.w.s`.
  - HLE: `sceKernelMemcpy`, `sceKernelMemset`, `sceKernelSetCompiledSdkVersion603_605`.
- `games/patapon3/game.toml` (id `patapon3`, `boot_path` = EBOOT.BIN) y `games/patapon3/runtime/hooks_main.cpp` copiado de `games/TEMPLATE` (hooks vacíos); build con `-DPSPRECOMP_GAME=patapon3` para que no aparezca la advertencia de discrepancia de id.
- Script bash `games/patapon3/scripts/p3.sh` (Git Bash) con subcomandos idempotentes: `extract` (ISO → `disc0/`), `analyze`, `recompile`, `build`, `run` (timeout + log), y `all`.
- Pendiente detectado en la prueba (documentar, no necesariamente arreglar en M0): `cvt.w.s` se emite como truncamiento; en el PSP usa el modo de redondeo de FCR31 (por defecto al más cercano). Se revisa en M1/M3 si afecta.

## 6. Diseño de M1 — Audio

### 6.1 `sceAudio` → SDL2
- Canales normales (8): `sceAudioChReserve`, `ChRelease`, `OutputPannedBlocking`, `SetChannelDataLen`, `ChangeChannelConfig`, `ChangeChannelVolume`; canal `Output2*` (Reserve/OutputBlocking/Release).
- Mezcla de todos los canales a 44.1 kHz estéreo s16 en un búfer anillo consumido por el callback de SDL.
- Semántica bloqueante: la llamada `*Blocking` suspende el hilo PSP (integrado con el scheduler cooperativo de psprecomp) hasta que haya espacio en la cola del canal → el ritmo lo marca el dispositivo real.
- Latencia objetivo ~10–20 ms (búfer SDL 512–1024 muestras); ajuste fino en M5.
- Opción de volcado de la mezcla a `.wav` (variable de entorno) para comparar con PPSSPP.

### 6.2 SAS con síntesis
- Decodificación VAG/PS-ADPCM, pitch, ADSR (incl. `SetSimpleADSR`, `SetADSR`, `SetSL`), volúmenes, ruido, mezcla en el búfer que el juego pasa a `__sceSasCore`/`__sceSasCoreWithMix`; reverb según `SasReverb` si no bloquea el avance.
- Conserva la lógica actual de fin de voz (`GetEndFlag`) que ya funciona.
- Referencia: PPSSPP `Core/HW/SasAudio.cpp`, `SasReverb.cpp`, `Core/HLE/sceSas.cpp`.

### 6.3 `sceAtrac` (ATRAC3 / ATRAC3plus)
- Decodificador: `ext/at3_standalone` de PPSSPP (extracto LGPL de FFmpeg), vendorizado en el runtime con sus avisos de licencia.
- API completa usada por el juego: `GetAtracID`, `SetData`, `AddStreamData`, `GetStreamDataInfo`, `DecodeData`, `GetRemainFrame`, `GetNextSample`, `GetSoundSample`, `SetLoopNum`, `ResetPlayPosition`, `GetBufferInfoForResetting`, `Reinit`, `ReleaseAtracID`.
- Semántica de buffers/streaming/bucles siguiendo `Core/HLE/sceAtrac.cpp` de PPSSPP al pie de la letra; los reinicios de posición son críticos para el encadenado de fragmentos de música.

### 6.4 Pruebas de M1
- Unitarias C++: decodificar un clip VAG y un frame ATRAC3plus extraídos de `disc0/` y comparar con referencia (muestras de PPSSPP/vgmstream); mezclador de `sceAudio` con entradas sintéticas.
- En juego: log sin `sceKernelWaitSema STUCK` de semáforos `sgx-psp-*`; punto alcanzado posterior al de la prueba; volcado `.wav` audible y comparable.

## 7. Diseño de M2 — Overlays

- **Análisis:** cada `OL_*.bin` se importa en Ghidra como binario crudo Allegrex en 0x08ABB180 (bloques: cabecera, texto, datos). Semillas de funciones: destinos `jal` internos, tabla de constructores `[ctor_start, ctor_end)`, símbolos de efonte en el rango. Salida: un `analysis.json` por overlay.
- **Emisión:** un banco por overlay con nombres prefijados (`ovTitle_`, `ovMission_`, `ovAzito_`) y tabla de dispatch propia. Solo se emite código; los datos los copia el propio juego al leer el archivo.
- **Llamadas:** overlay→main por llamada directa; main/puntero→ventana de overlays vía `RECOMP_LOOKUP` resuelto en el banco activo; overlay↔overlay no existe.
- **Selección de banco (runtime genérico):** cuando una lectura de archivo escribe en la ventana declarada, el runtime lee la cabecera (`MWo3`, id) y compara una huella (hash) del contenido cargado contra la registrada al compilar. Coincide → activa el banco. No coincide → error explícito y detención (nunca ejecutar código desactualizado).
- **Configuración:** la lista de overlays (archivo, id, ventana) va en `games/patapon3/game.toml`; el mecanismo es genérico.
- **Destinos faltantes:** de los 2,442 reportados, 858 caen en la ventana (resueltos por bancos); ~1,450 provienen de datos que el escáner tomó por código (no alcanzables); 89 son funciones reales faltantes del main → se completan con los símbolos de efonte como `force_entries` verificadas.

## 8. Verificación (todos los hitos)

1. Pruebas Rust (314 actuales + nuevas).
2. Pruebas unitarias C++ con datos reales de `disc0/` (no versionados; se omiten si faltan).
3. Umbrales del reporte de `recompile`: 0 errores de decodificación en funciones reales; destinos faltantes solo de una lista conocida; fingerprint obligatorio.
4. Corridas automáticas con timeout y lectura de log: sin `STUCK`, sin `LOOKUP_MISS` en código real, lista de imports sin implementar monótonamente decreciente, punto alcanzado igual o posterior. Entrada de botones por el socket de depuración (puerto 9999) para corridas repetibles.
5. Oráculo PPSSPP: capturas en puntos fijos, comparación de memoria (depurador de PPSSPP vs socket del runtime), volcados de audio `.wav`.
6. Desde M5, cada hito cierra con el usuario jugando.

Nada se da por terminado sin ejecutar el juego y observar el resultado.

## 9. Riesgos

| Riesgo | Mitigación |
|---|---|
| Código de psprecomp sin revisión humana | Revisar cada módulo antes de depender de él; comparar contra PPSSPP |
| `sceAtrac` delicado (Patapon fue caso difícil en PPSSPP) | Replicar semántica de PPSSPP; pruebas con clips reales; volcado `.wav` |
| Sincronía ritmo-audio | Reloj de audio dirigido por SDL; latencia baja; validación del usuario en M5 |
| Funciones que Ghidra no detecta | Símbolos de efonte como `force_entries` verificadas; log de `LOOKUP_MISS` |
| Semántica FPU (`cvt.w.s`, NaN/overflow) | Revisar contra PPSSPP cuando aparezcan diferencias |
| Logs de varios GB | Timeouts, filtrado y borrado tras cada corrida |

## 10. Trabajo con ayudantes

- Haiku 5.5 (max): tareas repetitivas y verificables (stubs de red, cruces de símbolos, inventarios de NIDs).
- Sonnet 5.5 (high): tareas medianas acotadas (una función HLE, una batería de pruebas).
- Diseño, revisión y verificación final: modelo principal.
