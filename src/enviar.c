/* enviar.c - bucle de ./estacion enviar (niveles 3 y 4)
 *
 * Cada vuelta del bucle dura TICK_MS y hace, si le toca:
 *   1. tomar una lectura del sensor cada intervalo_s y guardarla con enviado = 0
 *   2. publicar el latido MQTT cada latido_s (si falla, se pierde: QoS 0)
 *   3. enviar UNA lectura pendiente (la de menor seq), salvo que estemos
 *      esperando por un 503/429 (Retry-After) o por un fallo de red.
 * Nunca se queda dormido esperando al servidor: las lecturas siguen
 * guardándose en SQLite y se envían en orden cuando el servidor vuelve.
 */
#define _POSIX_C_SOURCE 200809L
#include "enviar.h"
#include "datos.h"
#include "red.h"
#include "sensor.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUTA_DB          "estacion.db"
#define TICK_MS          250     /* como mucho 4 envíos por segundo (límite: 5) */
#define MARGEN_MS        500     /* margen extra sobre Retry-After */
#define ESPERA_RED_MIN   1000    /* fallo de red: 1, 2, 4 ... */
#define ESPERA_RED_MAX   30000   /* ... hasta 30 s */
#define RETRY_DEFECTO_S  5       /* si el 503 no trae Retry-After */

static volatile sig_atomic_t salir = 0;

static void al_ctrl_c(int sig)
{
    (void)sig;
    salir = 1;
}

/* Reloj que no salta aunque cambie la hora del sistema. */
static long long ahora_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

typedef struct {
    Red       red;
    sqlite3  *db;
    char      topico[96];
    long long intervalo_ms, latido_ms;
    long long prox_lectura, prox_latido;
    long long bloqueado_hasta;   /* no enviar telemetría antes de este instante */
    long long espera_red;        /* espera creciente ante fallos de red */
    long long ultimo_seq;
    long      enviadas, latidos_ok, latidos_mal;
} Estado;

/* Lee un entero de la tabla config, con valor por defecto. */
static long leer_config_entero(sqlite3 *db, const char *clave, long defecto)
{
    char txt[32];
    if (datos_leer_config(db, clave, txt, sizeof txt) != 0)
        return defecto;
    long v = strtol(txt, NULL, 10);
    return v > 0 ? v : defecto;
}

/* Pide /config para tener intervalo_s y latido_s al día. */
static int refrescar_config(Estado *e)
{
    Respuesta resp;
    long codigo = red_get_reintentos(&e->red, "/api/v1/config", &resp);
    if (codigo == 200) {
        if (datos_guardar_config(e->db, resp.datos) != 0)
            fprintf(stderr, "Aviso: no pude guardar /config; uso la guardada\n");
    } else {
        fprintf(stderr, "Aviso: /config respondió %ld; uso la config guardada\n", codigo);
    }
    respuesta_liberar(&resp);
    return codigo == 401 ? -1 : 0;      /* con token malo no tiene sentido seguir */
}

static int preparar(Estado *e)
{
    memset(e, 0, sizeof *e);
    if (red_iniciar(&e->red) != 0)
        return -1;
    e->db = datos_abrir(RUTA_DB);
    if (e->db == NULL || refrescar_config(e) != 0)
        return -1;

    e->intervalo_ms = leer_config_entero(e->db, "intervalo_s", 5) * 1000;
    e->latido_ms = leer_config_entero(e->db, "latido_s", 10) * 1000;
    snprintf(e->topico, sizeof e->topico, "itla/estudiantes/%s/latido", e->red.matricula);

    long long t = ahora_ms();
    e->prox_lectura = t;                  /* primera lectura ya */
    e->prox_latido = t + 1000;            /* primer latido al segundo */
    e->bloqueado_hasta = t;
    e->espera_red = ESPERA_RED_MIN;

    sensor_iniciar();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_ctrl_c;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("Enviando cada %lld s, latido cada %lld s. Ctrl+C para terminar.\n",
           e->intervalo_ms / 1000, e->latido_ms / 1000);
    return 0;
}

/* 1. Lectura del sensor -> tabla telemetria (enviado = 0). */
static void tomar_lectura(Estado *e)
{
    time_t ts = time(NULL);
    LecturaSensor l = sensor_leer(ts);
    long long seq;
    if (datos_nueva_telemetria(e->db, (long long)ts, l.temp, l.corriente, &seq) == 0) {
        e->ultimo_seq = seq;
        printf("[seq %lld] lectura %.2f °C %.2f A guardada\n", seq, l.temp, l.corriente);
    }
}

/* 2. Latido MQTT: LATIDO;<seq>;<pendientes>;<ts> */
static void publicar_latido(Estado *e)
{
    long long pendientes = datos_contar_pendientes(e->db);
    char msg[100];
    snprintf(msg, sizeof msg, "LATIDO;%lld;%lld;%lld\n",
             e->ultimo_seq, pendientes < 0 ? 0 : pendientes, (long long)time(NULL));
    if (red_publicar_mqtt(&e->red, e->topico, msg) == 0) {
        e->latidos_ok++;
        printf("  latido seq=%lld pendientes=%lld\n", e->ultimo_seq, pendientes);
    } else {
        e->latidos_mal++;       /* QoS 0: no se encola, se pierde */
    }
}

/* Decide cuánto esperar según la respuesta a un envío fallido. */
static void programar_espera(Estado *e, long codigo, long long pendientes)
{
    long long t = ahora_ms();
    if (codigo == 503 || codigo == 429) {
        long s = e->red.retry_after > 0 ? e->red.retry_after : RETRY_DEFECTO_S;
        e->bloqueado_hasta = t + s * 1000 + MARGEN_MS;
        printf("  HTTP %ld: espero %ld s (Retry-After). Pendientes: %lld\n",
               codigo, s, pendientes);
    } else {
        e->bloqueado_hasta = t + e->espera_red;
        printf("  %s: reintento en %lld s. Pendientes: %lld\n",
               codigo == -1 ? "Sin red" : "Respuesta inesperada",
               e->espera_red / 1000, pendientes);
        e->espera_red = e->espera_red * 2 > ESPERA_RED_MAX ? ESPERA_RED_MAX : e->espera_red * 2;
    }
}

/* 3. Envía la pendiente más antigua. Devuelve -1 si hay que parar. */
static int enviar_pendiente(Estado *e)
{
    Telemetria t;
    int hay = datos_siguiente_pendiente(e->db, &t);
    if (hay <= 0)
        return hay;     /* 0: cola vacía; -1: error de base */

    char json[160];
    snprintf(json, sizeof json, "{\"seq\":%lld,\"ts\":%lld,\"temp\":%.2f,\"corriente\":%.2f}",
             t.seq, t.ts, t.temp, t.corriente);

    Respuesta resp;
    long codigo = red_post(&e->red, "/api/v1/telemetria", json, &resp);
    respuesta_liberar(&resp);
    datos_sumar_intento(e->db, t.seq);

    if (codigo == 201 || codigo == 200) {       /* creada o duplicada: ya la tiene */
        datos_marcar_enviada(e->db, t.seq);
        e->enviadas++;
        e->espera_red = ESPERA_RED_MIN;
        long long atraso = (long long)time(NULL) - t.ts;
        if (atraso >= 30)
            printf("[seq %lld] recuperada de la cola (%lld s de atraso)\n", t.seq, atraso);
        return 0;
    }
    if (codigo == 401) {
        fprintf(stderr, "HTTP 401: token rechazado. Revisa ITLA_TOKEN.\n");
        return -1;
    }
    if (codigo == 400) {
        /* el servidor rechaza esta lectura: reintentarla solo daría más 400 */
        fprintf(stderr, "[seq %lld] HTTP 400, el servidor no la acepta: se descarta\n", t.seq);
        datos_marcar_enviada(e->db, t.seq);
        return 0;
    }
    programar_espera(e, codigo, datos_contar_pendientes(e->db));
    return 0;
}

static void cerrar(Estado *e)
{
    long long pendientes = e->db ? datos_contar_pendientes(e->db) : 0;
    printf("\nFin: %ld enviadas, %lld pendientes guardadas, latidos %ld ok / %ld fallidos\n",
           e->enviadas, pendientes, e->latidos_ok, e->latidos_mal);
    datos_cerrar(e->db);
    red_cerrar(&e->red);
}

int enviar_ejecutar(void)
{
    Estado e;
    if (preparar(&e) != 0) {
        cerrar(&e);
        return 1;
    }

    int res = 0;
    while (!salir) {
        long long t = ahora_ms();
        if (t >= e.prox_lectura) {
            tomar_lectura(&e);
            e.prox_lectura += e.intervalo_ms;
            if (e.prox_lectura < t)                 /* si nos atrasamos, no amontonar */
                e.prox_lectura = t + e.intervalo_ms;
        }
        if (t >= e.prox_latido) {
            publicar_latido(&e);
            e.prox_latido += e.latido_ms;
            if (e.prox_latido < t)
                e.prox_latido = t + e.latido_ms;
        }
        if (ahora_ms() >= e.bloqueado_hasta && enviar_pendiente(&e) != 0) {
            res = 1;
            break;
        }
        red_esperar_ms(TICK_MS);
    }
    cerrar(&e);
    return res;
}
