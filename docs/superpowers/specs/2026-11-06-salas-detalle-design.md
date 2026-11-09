# Salas con misión, jugadores y héroes + servidor con un solo .bat

Fecha: 6 nov 2026. Meta: actualización publicada hoy a las 20:00.

## Qué pidió el usuario

1. En modo infraestructura, ver en las salas la misión, quién está dentro, su héroe y su nivel.
2. Si se puede, un botón «Unirse a sala» (la cereza del pastel).
3. Dejar el servidor en un solo `.bat`.

## Camino elegido

**Cada juego le dice al servidor qué héroe lleva y dónde está, y el servidor lo publica en `salas.json`.**

El juego ya lee clase, nivel y misión de su memoria para Discord (`p3_presence_read`), con las mismas direcciones en las
tres versiones. Basta reutilizarlo y mandarlo en el `ping` que ya sale cada 2 s, solo cuando cambia.

| Alternativa | Por qué no |
|---|---|
| Leer los atributos de sala que manda el juego | Solo llevan al dueño, y no hay misión |
| Espiar los streams del juego desde el servidor | Formato sin documentar y frágil |
| Canal nuevo en el protocolo | El `ping` ya existe, y un servidor viejo ignora el campo extra |

## Qué verá el jugador en `beta.montick.app/salas`

```
Coop  MONTICK   3/4  EN MISIÓN
      Torre de la Nevisca
      Montick · Yarida Nv 35     Edson · Taterazay Nv 28     P3B · Sin datos
```

- **Misión:** el nombre que muestra el juego, en el idioma de quien la manda. Sin imagen: el pack HD no se puede publicar.
- **Héroes:** clase y nivel del héroe de cada jugador. El resto del escuadrón no se muestra.
- **Nombre:** el apodo online que ya es público en el dueño de la sala. Nunca direcciones ni el nombre del héroe.
- **Cambio de privacidad:** hoy solo sale el dueño; ahora saldrán todos los miembros. Va en el anuncio.

## Por capas, para llegar a las 20:00

1. **Solo servidor y página:** nombres de todos los miembros. Funciona con cualquier versión, sin actualizar el juego.
2. **Juego:** clase, nivel y misión. Requiere la actualización.
3. **Unirse:** solo si sobra tiempo.

Si la capa 2 falla, la 1 sale igual y el juego queda como la u18.

## Unirse a sala

Un botón que meta al jugador directo **no es viable hoy**: el juego tiene que recorrer sus propios menús para entrar, y
hace falta un reconocimiento que no está hecho (D4 en `PLAN_ONLINE_2026-11-05.md`).

Lo posible hoy es un botón «Copiar ID» si la búsqueda por ID del juego funciona contra nuestro servidor. Eso está en duda:
el `CHANGELOG` dice que no encuentra escondites y el plan del 5 nov dice que está hecha y sin probar. Se comprueba con un
límite de 45 minutos. Si no funciona, no hay botón hoy.

## Un solo .bat

`servidor.bat` en `tester` sustituye a `servidor_beta.bat`, `reportes_beta.bat` y la ventana del túnel:

- **Primer doble clic:** arranca servidor online, página y reportes y, si no está ya corriendo, el túnel. Todo en una ventana.
- **Doble clic con todo corriendo:** para actualizar. Copias los archivos nuevos, le das doble clic y el servidor se
  reinicia con el código nuevo cuando ya no quede ninguna partida, sin cortar a nadie.
- **Si un servicio se cae,** se reinicia solo. Si falla 3 veces seguidas, se detiene y dice cuál y por qué.
- El túnel solo se toca si lo arrancó él: si `cloudflared` ya corre (servicio o ventana), se deja en paz.

Un script de Python hace el trabajo, porque un `.bat` no puede vigilar procesos con fiabilidad.
La primera vez corta a quien esté jugando, porque los servidores viejos no saben drenar.

## La actualización de hoy

Sale como **u19 (`0.3.0-beta.14`, obligatoria)**, sobre la u18, que nunca se publicó. El zip de la u18 queda intacto como
respaldo.

- **Zip A:** con el launcher nuevo (`launcher-perf`).
- **Zip B:** idéntico, con el launcher `online-power`. Se usa si el launcher nuevo no pasa las pruebas del usuario.

## Fuera de hoy

Medidor de desincronización (A2), versión del juego en salas (D1), ping en salas (D2), TURN (B2), misión que sobrevive a la
caída del servidor (C1) y unirse con un clic (D4).
