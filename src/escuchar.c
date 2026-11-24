/* escuchar.c - ./estacion escuchar (nivel 5)
 *
 * Se suscribe a itla/planta/<MATRICULA>/alarma. Por cada "ALARMA;<codigo>;<maquina>;<ts>":
 *   - si ya estaba confirmada en la base, se ignora (el broker la retiene y la
 *     vuelve a mandar al reconectar);
 *   - si no, se guarda en la tabla alarmas y se confirma con POST /api/v1/alarmas/ack,
 *     usando el handle HTTP (nunca el de la suscripción).
 * "NINGUNA;<ts>" se ignora. Si la conexión se corta, reconecta con espera creciente.
 */
#define _POSIX_C_SOURCE 200809L
#include "escuchar.h"
#include "datos.h"
#include "red.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUTA_DB          "estacion.db"
#define ESPERA_MIN_MS    1000
#define ESPERA_MAX_MS    30000
#define CONEXION_LARGA_S 30      /* si duró más que esto, la espera vuelve a 1 s */
#define MAX_CODIGO       32

static volatile sig_atomic_t salir = 0;

static void al_ctrl_c(int sig)
{
    (void)sig;
    salir = 1;
}

static int debe_salir(void)
{
    return salir != 0;
}

typedef struct {
    Red      red;
    sqlite3 *db;
    char     topico[96];
    long     confirmadas, ignoradas;
} Escucha;

/* Un código válido: solo letras y números, para armar el JSON sin riesgo. */
static int codigo_valido(const char *c)
{
    size_t n = strlen(c);
    if (n == 0 || n >= MAX_CODIGO)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)c[i]))
            return 0;
    return 1;
}

/* POST /api/v1/alarmas/ack. Devuelve 0 si quedó confirmada (200 o 409). */
static int confirmar(Escucha *e, const char *codigo)
{
    char json[64];
    snprintf(json, sizeof json, "{\"codigo\":\"%s\"}", codigo);

    Respuesta resp;
    long codigo_http = red_post_reintentos(&e->red, "/api/v1/alarmas/ack", json, &resp);
    int res = -1;
    if (codigo_http == 200) {
        if (resp.datos != NULL)
            resp.datos[strcspn(resp.datos, "\r\n")] = '\0';     /* solo la primera línea */
        printf("  alarma %s confirmada (%s)\n", codigo, resp.datos ? resp.datos : "");
        res = 0;
    } else if (codigo_http == 409) {
        printf("  alarma %s ya estaba confirmada (409)\n", codigo);
        res = 0;
    } else if (codigo_http == 404) {
        fprintf(stderr, "  alarma %s no existe en el servidor (404)\n", codigo);
        res = 0;    /* no tiene sentido reintentarla */
    } else {
        fprintf(stderr, "  no pude confirmar %s: HTTP %ld\n", codigo, codigo_http);
    }
    respuesta_liberar(&resp);
    return res;
}

/* Procesa "ALARMA;<codigo>;<maquina>;<ts>". */
static void atender_alarma(Escucha *e, char *texto)
{
    char *partes[4];
    int n = 0;
    char *p = texto;
    while (n < 4) {
        partes[n++] = p;
        char *pc = strchr(p, ';');
        if (pc == NULL)
            break;
        *pc = '\0';
        p = pc + 1;
    }
    if (n != 4 || !codigo_valido(partes[1])) {
        fprintf(stderr, "Alarma con formato raro, se ignora\n");
        return;
    }
    const char *codigo = partes[1], *maquina = partes[2];
    long long ts_servidor = strtoll(partes[3], NULL, 10);

    int ya = datos_alarma_confirmada(e->db, codigo);
    if (ya == 1) {
        e->ignoradas++;
        printf("Alarma %s (%s) ya confirmada antes: se ignora\n", codigo, maquina);
        return;
    }
    printf("ALARMA %s en %s\n", codigo, maquina);
    datos_guardar_alarma(e->db, codigo, maquina, ts_servidor, (long long)time(NULL));
    if (confirmar(e, codigo) == 0) {
        datos_marcar_ack(e->db, codigo, (long long)time(NULL));
        e->confirmadas++;
    }
}

/* Callback de cada mensaje MQTT completo. */
static void al_mensaje(const char *topico, const char *texto, void *ud)
{
    Escucha *e = ud;
    if (strcmp(topico, e->topico) != 0) {
        printf("(%s) %s\n", topico, texto);     /* otro tópico: solo se muestra */
        return;
    }
    if (strncmp(texto, "NINGUNA;", 8) == 0)
        return;                                 /* sin alarma pendiente */
    if (strncmp(texto, "ALARMA;", 7) != 0) {
        fprintf(stderr, "Mensaje desconocido: %s\n", texto);
        return;
    }
    char copia[128];
    snprintf(copia, sizeof copia, "%s", texto);
    atender_alarma(e, copia);
}

/* Espera ms milisegundos en pasos cortos, para que Ctrl+C responda rápido. */
static void esperar_atento(long long ms)
{
    for (long long t = 0; t < ms && !salir; t += 200)
        red_esperar_ms(200);
}

int escuchar_ejecutar(void)
{
    Escucha e;
    memset(&e, 0, sizeof e);
    if (red_iniciar(&e.red) != 0)
        return 1;
    e.db = datos_abrir(RUTA_DB);
    if (e.db == NULL) {
        red_cerrar(&e.red);
        return 1;
    }
    snprintf(e.topico, sizeof e.topico, "itla/planta/%s/alarma", e.red.matricula);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_ctrl_c;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("Escuchando %s (Ctrl+C para terminar).\n", e.topico);
    printf("Recuerda: el servidor solo manda alarmas si ./estacion enviar está corriendo.\n");

    long long espera = ESPERA_MIN_MS;
    while (!salir) {
        time_t inicio = time(NULL);
        red_suscribir_mqtt(&e.red, e.topico, al_mensaje, &e, debe_salir);
        if (salir)
            break;
        if (time(NULL) - inicio > CONEXION_LARGA_S)
            espera = ESPERA_MIN_MS;             /* la conexión iba bien: reconectar ya */
        printf("Conexión cerrada; reconecto en %lld s\n", espera / 1000);
        esperar_atento(espera);
        espera = espera * 2 > ESPERA_MAX_MS ? ESPERA_MAX_MS : espera * 2;
    }

    printf("\nFin: %ld alarmas confirmadas, %ld repetidas ignoradas\n",
           e.confirmadas, e.ignoradas);
    datos_cerrar(e.db);
    red_cerrar(&e.red);
    return 0;
}
