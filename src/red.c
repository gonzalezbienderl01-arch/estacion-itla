/* red.c - peticiones HTTP con libcurl */
#define _POSIX_C_SOURCE 200809L
#include "red.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_AGENT     "estacion-itla/1.0"
#define TIMEOUT_S      5L   /* tiempo máximo de toda la petición */
#define CONNECT_S      3L   /* tiempo máximo para conectar */
#define LARGO_TOKEN    32

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
    if (r->curl == NULL) {
        fprintf(stderr, "Error: curl_easy_init falló\n");
        curl_global_cleanup();
        return -1;
    }
    return 0;
}

void red_cerrar(Red *r)
{
    if (r->curl != NULL) {
        curl_easy_cleanup(r->curl);
        r->curl = NULL;
        curl_global_cleanup();
    }
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
