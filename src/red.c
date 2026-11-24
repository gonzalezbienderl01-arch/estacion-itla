/* red.c - peticiones HTTP con libcurl */
#define _POSIX_C_SOURCE 200809L
#include "red.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USER_AGENT     "estacion-itla/1.0"
#define TIMEOUT_S      5L   /* tiempo máximo de toda la petición */
#define CONNECT_S      3L   /* tiempo máximo para conectar */
#define LARGO_TOKEN    32
#define MAX_INTENTOS   6    /* intentos totales de una petición con reintentos */
#define ESPERA_MAX_S   30   /* tope de la espera creciente */

/* Copia una variable de entorno en dst. Devuelve -1 si no existe o no cabe. */
static int leer_env(const char *nombre, char *dst, size_t n)
{
    const char *v = getenv(nombre);
    if (v == NULL || v[0] == '\0') {
        fprintf(stderr, "Error: falta la variable de entorno %s\n", nombre);
        return -1;
    }
    int k = snprintf(dst, n, "%s", v);
    if (k < 0 || (size_t)k >= n) {
        fprintf(stderr, "Error: %s es demasiado larga\n", nombre);
        return -1;
    }
    return 0;
}

int red_iniciar(Red *r)
{
    memset(r, 0, sizeof *r);
    if (leer_env("ITLA_HOST", r->host, sizeof r->host) != 0 ||
        leer_env("ITLA_TOKEN", r->token, sizeof r->token) != 0 ||
        leer_env("ITLA_MATRICULA", r->matricula, sizeof r->matricula) != 0)
        return -1;

    /* No imprimimos el token: solo su largo, para diagnosticar un 401. */
    if (strlen(r->token) != LARGO_TOKEN) {
        fprintf(stderr, "Error: ITLA_TOKEN debe tener %d caracteres (tiene %zu)\n",
                LARGO_TOKEN, strlen(r->token));
        return -1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Error: curl_global_init falló\n");
        return -1;
    }
    r->curl = curl_easy_init();
    r->mqtt = curl_easy_init();
    if (r->curl == NULL || r->mqtt == NULL) {
        fprintf(stderr, "Error: curl_easy_init falló\n");
        curl_easy_cleanup(r->curl);     /* acepta NULL */
        curl_easy_cleanup(r->mqtt);
        r->curl = r->mqtt = NULL;
        curl_global_cleanup();
        return -1;
    }
    return 0;
}

void red_cerrar(Red *r)
{
    if (r->curl == NULL && r->mqtt == NULL)
        return;                     /* ya cerrada o nunca abierta */
    curl_easy_cleanup(r->curl);     /* acepta NULL */
    curl_easy_cleanup(r->mqtt);
    r->curl = NULL;
    r->mqtt = NULL;
    curl_global_cleanup();
}

void respuesta_liberar(Respuesta *resp)
{
    free(resp->datos);
    resp->datos = NULL;
    resp->len = 0;
}

/* Callback de escritura: libcurl llama aquí con cada trozo del cuerpo. */
static size_t al_recibir(char *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t n = size * nmemb;
    Respuesta *resp = ud;

    char *nuevo = realloc(resp->datos, resp->len + n + 1);
    if (nuevo == NULL)
        return 0;   /* sin memoria: libcurl aborta con CURLE_WRITE_ERROR */

    memcpy(nuevo + resp->len, ptr, n);
    resp->datos = nuevo;
    resp->len += n;
    resp->datos[resp->len] = '\0';
    return n;
}

/* Petición común a GET y POST. json == NULL significa GET. */
static long peticion(Red *r, const char *ruta, const char *json, Respuesta *resp)
{
    char url[512];
    int k = snprintf(url, sizeof url, "http://%s:4000%s", r->host, ruta);
    if (k < 0 || (size_t)k >= sizeof url) {
        fprintf(stderr, "Error: URL demasiado larga\n");
        return -1;
    }

    resp->datos = NULL;
    resp->len = 0;

    /* reset borra las opciones anteriores pero conserva la conexión abierta */
    curl_easy_reset(r->curl);
    curl_easy_setopt(r->curl, CURLOPT_URL, url);
    curl_easy_setopt(r->curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(r->curl, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(r->curl, CURLOPT_CONNECTTIMEOUT, CONNECT_S);
    curl_easy_setopt(r->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BEARER);
    curl_easy_setopt(r->curl, CURLOPT_XOAUTH2_BEARER, r->token);
    curl_easy_setopt(r->curl, CURLOPT_WRITEFUNCTION, al_recibir);
    curl_easy_setopt(r->curl, CURLOPT_WRITEDATA, resp);

    struct curl_slist *cabeceras = NULL;
    if (json != NULL) {
        cabeceras = curl_slist_append(NULL, "Content-Type: application/json");
        if (cabeceras == NULL) {
            fprintf(stderr, "Error: sin memoria para cabeceras\n");
            return -1;
        }
        curl_easy_setopt(r->curl, CURLOPT_HTTPHEADER, cabeceras);
        curl_easy_setopt(r->curl, CURLOPT_POSTFIELDS, json);
    }

    /* Nivel 1 de error: ¿llegó la petición? (CURLcode) */
    CURLcode rc = curl_easy_perform(r->curl);
    curl_slist_free_all(cabeceras);
    if (rc != CURLE_OK) {
        fprintf(stderr, "Error de red en %s: %s\n", ruta, curl_easy_strerror(rc));
        respuesta_liberar(resp);
        return -1;
    }

    /* Nivel 2 de error: ¿qué respondió el servidor? (código HTTP) */
    long codigo = 0;
    curl_easy_getinfo(r->curl, CURLINFO_RESPONSE_CODE, &codigo);

    curl_off_t espera = 0;
    if (curl_easy_getinfo(r->curl, CURLINFO_RETRY_AFTER, &espera) != CURLE_OK)
        espera = 0;
    r->retry_after = (long)espera;
    return codigo;
}

long red_get(Red *r, const char *ruta, Respuesta *resp)
{
    return peticion(r, ruta, NULL, resp);
}

long red_post(Red *r, const char *ruta, const char *json, Respuesta *resp)
{
    return peticion(r, ruta, json, resp);
}

void red_esperar_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    /* si una señal la interrumpe (EINTR), termina de esperar lo que falta */
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

/* Repite la petición mientras falle la red o el servidor pida esperar. */
static long con_reintentos(Red *r, const char *ruta, const char *json, Respuesta *resp)
{
    long espera_red = 1;
    long codigo = -1;

    for (int intento = 1; intento <= MAX_INTENTOS; intento++) {
        codigo = peticion(r, ruta, json, resp);
        if (codigo != -1 && codigo != 429 && codigo != 503)
            return codigo;              /* respuesta definitiva (buena o mala) */
        if (intento == MAX_INTENTOS)
            break;

        long s;
        if (codigo == -1) {             /* fallo de red: 1, 2, 4 ... 30 s */
            s = espera_red;
            espera_red = (espera_red * 2 > ESPERA_MAX_S) ? ESPERA_MAX_S : espera_red * 2;
        } else {                        /* 429/503: lo que pida el servidor */
            s = (r->retry_after > 0) ? r->retry_after : 1;
            respuesta_liberar(resp);
        }
        fprintf(stderr, "  %s: %s, reintento %d en %ld s\n", ruta,
                codigo == -1 ? "sin red" : (codigo == 429 ? "HTTP 429" : "HTTP 503"),
                intento, s);
        red_esperar_ms(s * 1000);
    }
    return codigo;
}

long red_get_reintentos(Red *r, const char *ruta, Respuesta *resp)
{
    return con_reintentos(r, ruta, NULL, resp);
}

long red_post_reintentos(Red *r, const char *ruta, const char *json, Respuesta *resp)
{
    return con_reintentos(r, ruta, json, resp);
}

int red_publicar_mqtt(Red *r, const char *topico, const char *mensaje)
{
    char url[512];
    int k = snprintf(url, sizeof url, "mqtt://%s:1883/%s", r->host, topico);
    if (k < 0 || (size_t)k >= sizeof url) {
        fprintf(stderr, "Error: URL MQTT demasiado larga\n");
        return -1;
    }

    curl_easy_reset(r->mqtt);
    curl_easy_setopt(r->mqtt, CURLOPT_URL, url);
    curl_easy_setopt(r->mqtt, CURLOPT_USERNAME, r->matricula);
    curl_easy_setopt(r->mqtt, CURLOPT_PASSWORD, r->token);
    curl_easy_setopt(r->mqtt, CURLOPT_POSTFIELDS, mensaje);   /* con cuerpo = publicar */
    curl_easy_setopt(r->mqtt, CURLOPT_TIMEOUT, TIMEOUT_S);
    curl_easy_setopt(r->mqtt, CURLOPT_CONNECTTIMEOUT, CONNECT_S);

    CURLcode rc = curl_easy_perform(r->mqtt);
    if (rc != CURLE_OK) {
        fprintf(stderr, "MQTT: no se pudo publicar en %s: %s\n",
                topico, curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

/* ---------- Suscripción MQTT ----------
 * libcurl no entrega texto: cada mensaje llega como 2 bytes con el largo del
 * tópico (primero el byte alto), el tópico y el contenido. Varios mensajes
 * pueden llegar pegados, o uno partido en dos llamadas. Como cada contenido
 * termina en '\n', acumulamos en buf y procesamos cada mensaje completo. */
#define MQTT_BUF        4096
#define MQTT_MAX_TOPICO 256
#define UPKEEP_MS       20000L   /* PINGREQ cada 20 s para que el broker no corte */

typedef struct {
    char          buf[MQTT_BUF];
    size_t        len;
    AlRecibirMqtt fn;
    void         *ud;
} Suscripcion;

static void procesar_mensaje(Suscripcion *m, const char *topico, size_t tlen, char *texto)
{
    char t[MQTT_MAX_TOPICO];
    if (tlen >= sizeof t)
        return;                         /* tópico absurdo: se ignora */
    memcpy(t, topico, tlen);
    t[tlen] = '\0';
    char *cr = strchr(texto, '\r');
    if (cr != NULL)
        *cr = '\0';
    m->fn(t, texto, m->ud);
}

static size_t al_recibir_mqtt(char *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t n = size * nmemb;
    Suscripcion *m = ud;

    if (n > sizeof m->buf - m->len) {   /* no cabe: se descarta lo acumulado */
        m->len = 0;
        if (n > sizeof m->buf)
            return n;
    }
    memcpy(m->buf + m->len, ptr, n);
    m->len += n;

    for (;;) {                          /* procesa todos los mensajes completos */
        if (m->len < 2)
            break;
        size_t tlen = ((size_t)(unsigned char)m->buf[0] << 8) | (unsigned char)m->buf[1];
        if (m->len < 2 + tlen)
            break;
        char *carga = m->buf + 2 + tlen;
        char *fin = memchr(carga, '\n', m->len - 2 - tlen);
        if (fin == NULL)
            break;                      /* el mensaje aún no llegó entero */
        *fin = '\0';
        procesar_mensaje(m, m->buf + 2, tlen, carga);
        size_t usado = (size_t)(fin - m->buf) + 1;
        memmove(m->buf, m->buf + usado, m->len - usado);
        m->len -= usado;
    }
    return n;                           /* siempre size * nmemb */
}

/* libcurl la llama cada poco tiempo; devolver 1 corta curl_easy_perform. */
static int al_progreso(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    DebeSalir salir = *(DebeSalir *)ud;
    return salir() ? 1 : 0;
}

int red_suscribir_mqtt(Red *r, const char *topico, AlRecibirMqtt fn, void *ud,
                       DebeSalir salir)
{
    char url[512];
    int k = snprintf(url, sizeof url, "mqtt://%s:1883/%s", r->host, topico);
    if (k < 0 || (size_t)k >= sizeof url) {
        fprintf(stderr, "Error: URL MQTT demasiado larga\n");
        return -1;
    }

    Suscripcion *m = calloc(1, sizeof *m);
    if (m == NULL) {
        fprintf(stderr, "Error: sin memoria para la suscripción\n");
        return -1;
    }
    m->fn = fn;
    m->ud = ud;

    curl_easy_reset(r->mqtt);
    curl_easy_setopt(r->mqtt, CURLOPT_URL, url);        /* sin POSTFIELDS = suscribir */
    curl_easy_setopt(r->mqtt, CURLOPT_USERNAME, r->matricula);
    curl_easy_setopt(r->mqtt, CURLOPT_PASSWORD, r->token);
    curl_easy_setopt(r->mqtt, CURLOPT_CONNECTTIMEOUT, CONNECT_S);
    /* sin CURLOPT_TIMEOUT: la suscripción debe quedarse abierta */
    curl_easy_setopt(r->mqtt, CURLOPT_WRITEFUNCTION, al_recibir_mqtt);
    curl_easy_setopt(r->mqtt, CURLOPT_WRITEDATA, m);
    curl_easy_setopt(r->mqtt, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(r->mqtt, CURLOPT_XFERINFOFUNCTION, al_progreso);
    curl_easy_setopt(r->mqtt, CURLOPT_XFERINFODATA, &salir);
    curl_easy_setopt(r->mqtt, CURLOPT_UPKEEP_INTERVAL_MS, UPKEEP_MS);

    CURLcode rc = curl_easy_perform(r->mqtt);
    free(m);
    if (rc == CURLE_OK || rc == CURLE_ABORTED_BY_CALLBACK)
        return 0;
    fprintf(stderr, "MQTT: la suscripción se cortó: %s\n", curl_easy_strerror(rc));
    return -1;
}
