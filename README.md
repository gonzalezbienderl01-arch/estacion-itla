# estacion — Estación de Telemetría ITLA (C11 + libcurl + SQLite)

## Requisitos (Ubuntu / Raspberry Pi OS)

```bash
sudo apt install gcc make libcurl4-openssl-dev
```

SQLite va como amalgamation en `lib/sqlite/` (`sqlite3.c` y `sqlite3.h`) y se compila aparte en `build/sqlite3.o`.

## Configuración

El programa lee la conexión de variables de entorno; nada sensible va en el código:

```bash
export ITLA_HOST=itla-meca.duckdns.org   # o 10.0.0.200 desde el laboratorio
export ITLA_MATRICULA=<tu matrícula>
export ITLA_TOKEN=<tu token de 32 caracteres>
```

## Compilar y usar

```bash
make            # compila con -std=c11 -Wall -Wextra
make run        # equivale a ./estacion config
make clean
```

| Subcomando | Nivel | Estado |
|---|---|---|
| `./estacion config` | 0 · Hola servidor | listo |
| `./estacion descargar` | 1 · Descarga completa | listo |
| `./estacion analizar` | 2 · Análisis SQL | listo (`--ver` muestra sin enviar) |
| `./estacion enviar` | 3 y 4 · Telemetría y latido | listo (Ctrl+C para parar) |
| `./estacion escuchar` | 5 · Alarmas MQTT | listo (en otra terminal, junto con `enviar`) |

## Estructura

- `src/main.c` — lee `argv` y llama al subcomando.
- `src/red.c/.h` — libcurl: un solo *easy handle* reutilizado, Bearer, User-Agent `estacion-itla/1.0`, timeouts de 5 s y revisión de los dos niveles de error (CURLcode y código HTTP).
- `src/datos.c/.h` — SQLite: WAL, `busy_timeout`, las 4 tablas, sentencias preparadas y transacciones.
- `src/sensor.c/.h` — sensor simulado: temperatura 25–45 °C (onda lenta de 10 min + ruido) y corriente 2–5 A.
- `src/enviar.c/.h` — bucle de `enviar` (niveles 3 y 4).
- `src/escuchar.c/.h` — suscripción a alarmas (nivel 5).

## Sesión de evaluación (niveles 3, 4 y 5)

El servidor solo manda alarmas a quien tiene un latido reciente, así que se usan dos terminales a la vez:

```bash
# terminal 1
./estacion enviar
# terminal 2
./estacion escuchar
```

Dejar ambas corriendo al menos 15 minutos y terminar con Ctrl+C.

## Decisiones

- **Token solo por entorno**: se valida que tenga 32 caracteres y nunca se imprime.
- **`curl_easy_reset` antes de cada petición**: limpia las opciones de la anterior (por ejemplo, un POST), pero conserva la conexión abierta.
- **`INSERT OR REPLACE` en `config`**: volver a correr `config` actualiza los valores en vez de fallar por la PRIMARY KEY.
- **Descarga paginada**: pide `desde_id=<id más alto recibido>&limite=200` hasta que una página traiga menos de 200 filas. El servidor repite filas entre páginas a propósito; `id PRIMARY KEY` + `INSERT OR IGNORE` las absorbe.
- **Una sola transacción** para toda la descarga: si falla una página, `ROLLBACK` y la base queda como estaba.
- **CSV con campos vacíos**: se separa con `strchr` (no `strtok`, que se salta `,,`). Un campo vacío o no numérico se guarda como `NULL`.
- **`filas_unicas` y `suma_ids` se calculan con SQL** (`COUNT(*)`, `SUM(id)`), no con contadores en C.
- **Vista `lecturas_clasificadas`**: añade la columna `valida` (1/0) con un `CASE`. Una fila es inválida si `estado` no es `OK`, si `temp = -999.0` o si falta cualquier campo (`NULL`). Las estadísticas se calculan sobre esa vista con `COUNT/SUM`, `MIN`, `MAX`, `ROUND(AVG(temp), 2)` y `temp > umbral_temp`, todo en SQL.
- **`analizar --ver`**: calcula y muestra el JSON sin enviarlo, para revisar antes de gastar uno de los 10 intentos.
- **Cola en SQLite (nivel 3)**: cada lectura se guarda primero en `telemetria` con `enviado = 0` y después se intenta enviar. Se envía siempre la de menor `seq`, así salen en orden. El `seq` se calcula con `MAX(seq) + 1` en el mismo `INSERT`, por eso sobrevive a un reinicio del programa.
- **Bucle que no se bloquea**: cada 250 ms revisa si toca leer el sensor, publicar el latido o enviar **una** pendiente. Ante un 503 no se duerme: marca "no enviar hasta" (Retry-After + 0,5 s) y sigue guardando lecturas. Así no se hacen peticiones antes de que venza Retry-After (M7) y nunca más de 4 por segundo (sin 429, M11).
- **Latido MQTT (nivel 4)**: cada `latido_s` publica `LATIDO;<seq>;<pendientes>;<ts>` en `itla/estudiantes/<MATRICULA>/latido` con un *easy handle* aparte. Si el broker no responde, el latido se pierde (QoS 0) y el bucle sigue.
- **Ctrl+C**: `sigaction` pone una bandera; el bucle termina la vuelta, cierra la base y los handles y muestra un resumen.
- **Suscripción MQTT (nivel 5)**: libcurl entrega bytes, no texto: 2 bytes con el largo del tópico, el tópico y el contenido. Se acumulan en un búfer y se procesa cada mensaje completo (terminado en `\n`), aunque lleguen varios pegados o uno partido.
- **Alarmas sin duplicar**: `codigo PRIMARY KEY` en `alarmas`. Si una alarma ya tiene `ts_ack`, se ignora (el broker la retiene y la reenvía al reconectar). `NINGUNA;<ts>` se ignora.
- **Confirmación con otro handle**: el ack (`POST /api/v1/alarmas/ack`) usa el *easy handle* HTTP; la suscripción usa el de MQTT. 200 y 409 cuentan como confirmada.
- **Reconexión**: `curl_easy_perform` no regresa mientras la conexión viva; cuando el broker la cierra se reconecta con espera creciente (1, 2, 4 ... 30 s), que vuelve a 1 s si la conexión duró más de 30 s. `CURLOPT_UPKEEP_INTERVAL_MS` manda PINGREQ cada 20 s para que no la corte.
- **Ctrl+C en `escuchar`**: `CURLOPT_NOPROGRESS = 0` con una `CURLOPT_XFERINFOFUNCTION` que devuelve 1 cuando la bandera de SIGINT está puesta; así `perform` se corta aunque esté bloqueado.
- **Dos procesos, una base**: `PRAGMA journal_mode=WAL` y `sqlite3_busy_timeout(db, 2000)` evitan `database is locked`.
- **Límite de ritmo**: pausa de 250 ms entre páginas (4 peticiones/s). Ante 429/503 se espera lo que diga `Retry-After`; ante fallo de red, 1, 2, 4 ... hasta 30 s.

## Resultados

Progreso final en el servidor del reto (`/api/v1/mi-progreso`): **70/70**.

| Nivel | Puntos | Métricas |
|---|---|---|
| 0 · Hola servidor | 5/5 | M1 autenticación correcta |
| 1 · Descarga | 10/10 | M2 = exacto (2000 filas únicas) |
| 2 · Análisis | 15/15 | M3 = 24/24 en el primer intento |
| 3 · Telemetría | 15/15 | M5 = 100 %, M6 = 2/2 caídas, M7 = 0 |
| 4 · Latido | 10/10 | M8 = 100 % |
| 5 · Alarmas | 15/15 | M9 = 100 %, M10 = 0.26 s |

M11 (peticiones con 400/429) = 0 %.
