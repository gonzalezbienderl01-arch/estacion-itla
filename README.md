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
| `./estacion descargar` | 1 · Descarga completa | pendiente |
| `./estacion analizar` | 2 · Análisis SQL | pendiente |
| `./estacion enviar` | 3 y 4 · Telemetría y latido | pendiente |
| `./estacion escuchar` | 5 · Alarmas MQTT | pendiente |

## Estructura

- `src/main.c` — lee `argv` y llama al subcomando.
- `src/red.c/.h` — libcurl: un solo *easy handle* reutilizado, Bearer, User-Agent `estacion-itla/1.0`, timeouts de 5 s y revisión de los dos niveles de error (CURLcode y código HTTP).
- `src/datos.c/.h` — SQLite: WAL, `busy_timeout`, las 4 tablas, sentencias preparadas y transacciones.

## Decisiones

- **Token solo por entorno**: se valida que tenga 32 caracteres y nunca se imprime.
- **`curl_easy_reset` antes de cada petición**: limpia las opciones de la anterior (por ejemplo, un POST), pero conserva la conexión abierta.
- **`INSERT OR REPLACE` en `config`**: volver a correr `config` actualiza los valores en vez de fallar por la PRIMARY KEY.
