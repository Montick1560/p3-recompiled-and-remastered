# Salas con misión, jugadores y héroes + un solo .bat: plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans, en modo nativo (el usuario lo pidió). Código con ponytail; página con impeccable. Pasos con casillas (`- [ ]`).

**Goal:** publicar hoy a las 20:00 una actualización cuya página de salas muestre misión, jugadores, clase y nivel, con el servidor arrancando desde un solo `.bat`.

**Architecture:** el módulo de Patapon 3 construye un JSON con clase, nivel y misión a partir de `p3_presence_read`. `infra::Client` lo manda en el `ping` solo cuando cambia. El servidor lo sanea, lo guarda en la conexión y lo publica en `salas.json`, y la página lo pinta. `servidor.py` vigila los tres servicios.

**Tech Stack:** C++17 (runtime, nlohmann::json), Python 3 stdlib (servidor), HTML/JS sin dependencias (página), ctest.

**Spec:** `docs/superpowers/specs/2026-11-06-salas-detalle-design.md`

## Global Constraints

- Rama `salas-detail` desde `online-power` 32aeaa9. Launcher: versión en `launcher-perf` y en `online-power`.
- **Sin push, despliegue ni subida a R2:** los hace el usuario. No tocar `tester`.
- Commits en inglés, autor «Patapon Community», sin trailer de Claude y sin cambiar `git config`.
- Laboratorio en puertos 9995-9998, nunca 9999. Comprobar instancias abiertas antes de cada prueba. Sin inyectar teclas en ventanas del usuario.
- `salas.json` nunca lleva IP, IP virtual, ID de consola ni código.
- Prueba del runtime: `ninja` + `ctest` en `build/rtw` + `rt_regress.py` con `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`.
- Sin datos del juego en `runtime/src`: lo específico va en `games/patapon3/runtime`.
- Python del lab: `python -I`. Puede hacer falta recargar `PATH` desde el registro.

## Review Focus

1. **Status hostil o roto** (HTML en la misión, textos enormes, tipos erróneos): el `ping` sigue respondiendo `ok`, el campo se descarta y la página no interpreta HTML. Pruebas: tareas 1 y 2.
2. **Jugador con cliente sin status (u17):** sale su nombre con «Sin datos», nunca `undefined`. Pruebas: tareas 1 y 2.
3. **Una excepción en el hilo de red** (JSON con UTF-8 inválido o lectura a medias): no puede matar el `ticker`. Prueba: tarea 3.
4. **Misión vieja al volver al escondite:** la sala solo muestra misión en estado `in_mission`. Pruebas: tareas 1 y 4.
5. **Puerto ocupado por el servidor viejo o segundo doble clic en plena partida:** mensaje claro, sin bucle de reinicios y sin cortar la partida. Prueba: tarea 5.

## Horario y puntos de corte

| Hora | Hito | Si no se llega |
|---|---|---|
| 10:00 | Tareas 1, 2 y 5 | Quitar el pulido de la página |
| 12:30 | Tareas 3 y 4 | A las 14:00 se abandona la capa del juego; sale solo la capa 1 |
| 14:00 | Tarea 6 (máx. 45 min) | Se omite: sin botón hoy |
| 15:00 | Tarea 7: paquetes y kit | Es lo único que no se recorta |
| 15:00-18:30 | **Pruebas del usuario** | |
| 18:30 | El usuario elige zip A o B | Sin decisión: zip B |
| 19:00-20:00 | El usuario actualiza `tester`, R2 y publica | |

---

### Task 1: el servidor guarda el status y publica la lista de jugadores

**Files:**
- Modify: `tools/infra_server.py` (`Conn.__init__`, `op_ping` ~1213, `lobby_status` ~1492, `lobby_status_selftest` ~2179)

**Interfaces:**
- Produces, petición: `ping` admite `status`: `{"class": str, "level": int, "mission": str}`, todos opcionales.
- Produces, `salas.json`: cada sala añade `roster`: `[{"name": str, "class": str|null, "level": int|null}]` en orden de entrada, y `mission_name: str`, solo si hay nombre (coop únicamente en `in_mission`; VS en cualquier estado). Los campos actuales no cambian.

- [ ] **Step 1: Escribir las pruebas en `lobby_status_selftest`**

Casos, con estos nombres en comentarios como el resto del selftest:
- `status_roster`: A manda `status={"class":"Yarida","level":35}`, B no manda nada. `roster == [{"name":"P3A","class":"Yarida","level":35},{"name":"P3B","class":None,"level":None}]`.
- `status_mission_only_in_mission`: con `mission` enviada y la sala abierta no hay `mission_name`; con `FLAG_CLOSED`, `mission_name == "Torre"`. Se toma la del dueño y, si no tiene, la del primer miembro que la tenga.
- `status_bad_fields`: `level=500`, `level="9"`, `class=7`, `class` de 25 caracteres y `mission` de 65 caracteres descartan solo ese campo; `status="x"` se ignora. En todos, `ping` responde `ok`.
- `status_control_chars`: `mission="A\u0000B\nC"` llega como `"ABC"`. `<b>` se guarda tal cual: escapar es trabajo de la página.
- Actualizar las igualdades existentes de `s["rooms"][0]` y `[1]` para incluir `roster`.
- `status_no_addresses` sigue pasando con `roster`.

- [ ] **Step 2: Comprobar que fallan**

Run: `python -I tools/infra_server.py --selftest`
Expected: `AssertionError` en `status_roster`.

- [ ] **Step 3: Implementar**

`def clean_status(raw) -> dict` a nivel de módulo: acepta solo un `dict`; `class` hasta 24 caracteres, `mission` hasta 64, `level` `int` (no `bool`) de 1 a 99; elimina los caracteres de control (`unicodedata.category(c) == "Cc"`); omite lo inválido sin lanzar excepción. `Conn.status = {}`. `op_ping`: `if "status" in msg: conn.status = clean_status(msg["status"])`. `lobby_status` construye `roster` y `mission_name`.

- [ ] **Step 4: Comprobar que pasan**

Run: `python -I tools/infra_server.py --selftest`
Expected: termina sin errores.

- [ ] **Step 5: Commit** `feat(infra-server): lobby summary lists each member's hero and the mission`

### Task 2: la página muestra misión y jugadores

**Files:**
- Modify: `tools/beta_report_server.py` (`paintRoom` ~524, lista accesible ~576, CSS de `.det`, prueba `lobby_page` ~762)

**Interfaces:**
- Consumes: `roster` y `mission_name` de la tarea 1.

- [ ] **Step 1: Escribir la prueba en `lobby_page`**

`salas.json` de ejemplo con `roster` y `mission_name`. Comprobar que `/salas.json` los devuelve intactos y que el HTML contiene `roster`, `mission_name` y `Sin datos`. Comprobar que `paintRoom` no usa `innerHTML` sobre esos campos: `"roster" not in` la línea que asigna `innerHTML`.

- [ ] **Step 2: Comprobar que falla**

Run: `python -I tools/beta_report_server.py --selftest`
Expected: AssertionError en `lobby_page`.

- [ ] **Step 3: Implementar con impeccable**

Dentro de `.det`: línea con la misión (si hay) y, debajo, un bloque por jugador: `Nombre · Clase Nv N`, o `Nombre · Sin datos`. Todo con `textContent`. Se mantiene el tablero de letras: el detalle no anima. `key` de fila sin cambios. La lista para lectores de pantalla (`#list`) añade misión y jugadores. VS: `mission_name` si existe, si no `Arena N`.

- [ ] **Step 4: Comprobar que pasa**

Run: `python -I tools/beta_report_server.py --selftest`
Expected: termina sin errores.

- [ ] **Step 5: Ver la página con datos falsos**

`salas.json` de prueba en el scratchpad con 4 jugadores, misión larga de 64 caracteres, un `<img src=x onerror=alert(1)>` como misión y un jugador sin datos. Servir en el puerto 9997 y capturar a 1280 px y 390 px con `msedge --headless --screenshot`.
Expected: nada se sale de la fila, el `<img>` aparece como texto y «Sin datos» se lee.

- [ ] **Step 6: Commit** `feat(beta-page): lobby rooms show the mission, players, class and level`

### Task 3: el juego manda su status

**Files:**
- Modify: `runtime/include/net/infra_client.h:62,79`, `runtime/src/net/infra_client.cpp` (ticker ~887, opciones ~938)
- Modify: `runtime/include/psp_presence_run.h`, `runtime/src/psp_presence_run.cpp`, `runtime/src/main.cpp:279`
- Modify: `games/patapon3/runtime/p3_presence_text.{h,cpp}`, `games/patapon3/runtime/hooks_main.cpp:1534`
- Test: `runtime/tests/test_infra_client.cpp`, `runtime/tests/test_p3_presence.cpp`

**Interfaces:**
- Produces: `void infra::set_status_source(std::function<nlohmann::json()>)`, junto a `set_lab_hooks`; `Client::Options::status`, función opcional que se copia del global igual que `lab_hooks`.
- Produces: campo `nlohmann::json (*online_status)(const uint8_t* rdram) = nullptr` en `PspPresenceSource`, y `nlohmann::json psp_presence_online_status(const uint8_t* rdram)`, que devuelve null sin fuente registrada.
- Produces: `nlohmann::json p3_online_status(const P3Presence&)` y `p3_online_status_from_ram(const uint8_t*)`: `{"class": kClassNames[id], "level": n, "mission": utf8}`, omitiendo lo desconocido (clase o nivel 0, misión vacía); null si no queda nada.

- [ ] **Step 1: Escribir las pruebas**

`test_p3_presence.cpp`, `online_status`:
- Clase 2 y nivel 35: `{"class":"Yarida","level":35}`.
- Con misión `u"Torre"`: incluye `"mission":"Torre"`.
- Clase y nivel 0 sin misión: null.
- Misión con surrogate suelto: `.dump()` no lanza excepción.

`test_infra_client.cpp`:
- `status_sent_when_changed`: con la fuente devolviendo A, A, B en pings seguidos, solo dos llevan `status`: el primero con A y el tercero con B.
- `status_resent_after_reconnect`: una conexión nueva vuelve a mandarlo.
- `status_source_throwing`: una fuente que lanza `std::runtime_error` no detiene los pings y los siguientes llegan.

- [ ] **Step 2: Comprobar que fallan**

Run: `ninja -C build/rtw test_infra_client test_p3_presence`
Expected: error de compilación por funciones no definidas.

- [ ] **Step 3: Implementar**

En el `ticker`, antes del `ping`, llamar a `opt_.status` dentro de `try/catch (...)`. Si el resultado no es null y difiere del último enviado en esta conexión, poner `ping["status"]` y recordarlo. El último enviado se guarda en una variable local de `ticker`: cada conexión tiene su hilo, así que un reintento lo reenvía.

En `main.cpp`, justo antes de `psp_presence_start`: `infra::set_status_source([rdram] { return psp_presence_online_status(rdram); });`. Va fuera de `psp_presence_start` para que funcione con Discord apagado. `hooks_main.cpp:1534` registra `p3_online_status_from_ram`.

- [ ] **Step 4: Comprobar que pasan**

Run: `ninja -C build/rtw; ctest --test-dir build/rtw`
Expected: todo pasa, 135 pruebas o más.

- [ ] **Step 5: Commit** `feat(infra): the game reports its hero and mission to the lobby summary`

### Task 4: comprobación en vivo

**Files:**
- Create: `build/infra/salas_live.sh`, partiendo de `build/infra/host_join.sh` y `build/infra/mission.sh`
- Create: `build/infra/salas/REPORT.md`

- [ ] **Step 1: Dos instancias con servidor de laboratorio y `--status`**

Puertos 9996 y 9997, con save de laboratorio. Copiar `salas.json` en cada fase: escondite, sala llena, misión y vuelta al escondite.
Expected:
- En el escondite: `roster` con los 2, clase y nivel correctos según `p3_presence`, sin `mission_name`.
- En misión: `mission_name` igual al que muestra la pantalla (comparar con captura).
- De vuelta: sin `mission_name`.

- [ ] **Step 2: Repetir con 1.1.5b y vanilla**

`build/infra/one.py` con cada exe, solo escondite y misión si el arranque del lab lo permite.
Expected: la clase y el nivel coinciden. Si una versión lee mal: `online_status` devuelve null para ella y se anota. Si no se puede arrancar, se anota como no probado.

- [ ] **Step 3: Regresión y coste**

Run: `rt_regress.py` con RTC fijo contra `relevo_base`.
Expected: PASS. El `ping` sigue llegando cada 2 s, y `status` solo aparece al cambiar (registro del servidor de lab).

- [ ] **Step 4: Comprobar direcciones y escribir el informe**

Run: `python -I build/infra/ipgrep.py` sobre `salas.json` y los registros.
Expected: limpio. `REPORT.md` con lo visto y lo no probado.

- [ ] **Step 5: Commit** de los scripts, si cambió algo del repo; `build/` queda fuera de git.

### Task 5: un solo .bat

**Files:**
- Create: `tools/servidor.py`, `build/beta/servidor.bat`
- Modify: `build/beta/RUNBOOK.md`

**Interfaces:**
- Produces: `python -I servidor.py` y `python -I servidor.py --selftest`.
- Produces: `class Supervisor` con `commands: dict[str, list[str]]`, `drain_path`, `lock_port=27329`, `fast_fail_s=30`, `max_fast_fails=3`; método `run() -> int`.

Comandos fijos, los mismos que los `.bat` de hoy:
- `infra`: `python -I infra_server.py --host 127.0.0.1 --port 27320 --ws-port 27321 --beta beta_config.json --db beta.db --log infra_server.log --status salas.json --drain-file drain.flag`
- `reportes`: `python -I beta_report_server.py --host 127.0.0.1 --port 27322 --db beta.db --beta beta_config.json`
- `tunel`: `cloudflared tunnel run patapon-online`, solo si `tasklist` no ve ya `cloudflared.exe`.

- [ ] **Step 1: Escribir el selftest, con hijos falsos en Python**

- `selftest_starts_all`: arranca los hijos y todos corren.
- `selftest_drain_restart`: `infra` termina con `drain.flag` presente: borra el flag y reinicia `infra` y `reportes`, sin tocar `tunel`.
- `selftest_crash_restart`: `reportes` cae tras más de `fast_fail_s`: reinicia solo ese.
- `selftest_fast_fail`: un hijo que muere 3 veces en menos de `fast_fail_s`: `run()` para a los demás y devuelve 1 con un mensaje que nombra al hijo.
- `selftest_second_instance`: con el puerto de bloqueo ocupado, `main` crea `drain.flag`, imprime «se reinicia cuando no quede ninguna partida» y sale sin lanzar nada.
- `selftest_tunnel_present`: `cloudflared` ya corriendo: no se lanza y `run()` no lo termina.
- `selftest_no_tunnel_binary`: `cloudflared` no existe: avisa y sigue con los otros dos.

- [ ] **Step 2: Comprobar que falla**

Run: `python -I tools/servidor.py --selftest`
Expected: no existe la clase `Supervisor`.

- [ ] **Step 3: Implementar**

`subprocess.Popen` sin redirigir: todo sale en la misma ventana. Bucle cada 0,5 s. Ctrl+C termina a los hijos propios. `servidor.bat`: `cd /d "%~dp0"`, `python -I servidor.py`, `pause`. Comentario `shortcut:`: los registros se mezclan en una ventana, y los archivos de registro de cada servicio no cambian.

- [ ] **Step 4: Comprobar que pasa**

Run: `python -I tools/servidor.py --selftest`
Expected: sin errores.

- [ ] **Step 5: Prueba real en esta PC con servicios reales**

Copia en el scratchpad, con los puertos de `commands` cambiados a 9995-9997 y sin `beta_config` real. Arrancar y lanzarlo una segunda vez, primero con una sala de 2 clientes de prueba y luego sin ella.
Expected: con la sala, no se reinicia; al vaciarse, se reinicia con el código nuevo.

- [ ] **Step 6: Actualizar `RUNBOOK.md`**

Sección nueva al principio: un doble clic para arrancar, copiar y otro doble clic para actualizar. Cómo retirar los dos `.bat` viejos una vez probado.

- [ ] **Step 7: Commit** `feat(tools): one supervisor starts and restarts every server service`

### Task 6: reconocimiento de «Unirse» (máximo 45 min; solo si hay tiempo a las 14:00)

> **Resultado, 6 nov:** la búsqueda no encontraba nada, pero la causa resultó pequeña y sobraba tiempo, así que se arregló (5875611) en lugar de omitir el botón. Detalle: `build/infra/salas/UNIRSE.md`.

**Files:**
- Create: `build/infra/salas/UNIRSE.md`
- Modify (solo si funciona): `tools/beta_report_server.py`

- [ ] **Step 1: Probar la búsqueda por ID en el lab**

Dos instancias: A abre escondite y B usa Heropuerta → Buscar por ID online con el apodo de A.
Expected: se anota si la encuentra, qué petición llega al servidor y qué falta.

- [ ] **Step 2: Decidir**

- **La encuentra:** botón «Copiar ID» en salas abiertas, que copia el apodo y muestra la ruta del menú. Con prueba en `lobby_page`. Se llama «Copiar ID», no «Unirse», porque no une a nadie.
- **No la encuentra:** sin botón. `UNIRSE.md` dice qué faltaría y cuánto costaría.

- [ ] **Step 3: Commit**, si hubo botón: `feat(beta-page): copy the host's id from an open room`

### Task 7: paquetes y kit de pruebas

**Files:**
- Modify: `tools/beta_report_server.py` (`DOWNLOAD_URL` a u19)
- Modify: versión `0.3.0-beta.14` en `package.json`, `src-tauri/Cargo.toml` y `src-tauri/tauri.conf.json`, en ambas ramas del launcher
- Create: `build/beta/patapon3-beta1-u19.zip` (A), `build/beta/u19_launcher_online_power/patapon3-beta1-u19.zip` (B), con su `latest_u19.json`, `notas_u19.txt`, `u19_servidor/`
- Create: `build/beta/ACTUALIZACION_19.md`, `build/beta/ANUNCIO_U19_DISCORD.md`
- Create: `build/plata/PRUEBAS_U19.md` y un `.bat` por prueba
- Modify: `build/beta/CHANGELOG.md`

- [ ] **Step 1: Puerta final**

Run: `ninja` + `ctest` en `build/rtw`, `--selftest` de `infra_server.py`, `beta_report_server.py` y `servidor.py`, y `cargo test` + `vitest` + `tsc` en cada rama del launcher.
Expected: todo pasa. Los fallos se reportan con su salida.

- [ ] **Step 2: Construir las 3 versiones y los 2 zips siguiendo `PAQUETE.md` y `PUBLICAR_ACTUALIZACION.md`**

Firmar con `--version 0.3.0-beta.14 --min-version 0.3.0-beta.14`. La lista de archivos sale igual que la u18.

- [ ] **Step 3: Humo**

Descomprimir cada zip en `E:\p3_u19_smoke_A` y `_B`. Arrancan las 3 versiones, y el launcher A muestra la versión correcta con 7,x MB durante el juego. `P3_REPORT_URL` apunta a un puerto local cerrado.

- [ ] **Step 4: `PRUEBAS_U19.md`**

Una lista corta, con qué mirar en cada caso:
1. Launcher nuevo: volver del juego, mando, música y segundo monitor.
2. Página de salas con el servidor de laboratorio.
3. `servidor.bat`.
4. La misión entre dos PCs.
5. Búsqueda por ID.

- [ ] **Step 5: Escribir `ACTUALIZACION_19.md`**

Pasos en orden, servidor primero, con los hashes reales. Incluye qué zip subir según la decisión de las 18:30, el aviso de la primera vez que corta y el anuncio de que los apodos de todos los miembros salen en la página.

- [ ] **Step 6: Commit** `chore(beta): update 19` y actualizar `EN_QUE_NOS_QUEDAMOS_2026-11-06.md` y la memoria.
