/* red.h - todo lo de libcurl (HTTP y, más adelante, MQTT) */
#ifndef RED_H
#define RED_H

#include <stddef.h>
#include <curl/curl.h>

/* Cuerpo de una respuesta HTTP, en memoria dinámica. */
typedef struct {
    char  *datos;   /* texto recibido, siempre terminado en '\0' */
    size_t len;     /* bytes recibidos (sin contar el '\0') */
} Respuesta;

/* Conexión con el servidor: un solo easy handle que se reutiliza. */
typedef struct {
    CURL *curl;
    char  host[256];
    char  token[64];
    char  matricula[32];
} Red;

/* Lee ITLA_HOST, ITLA_TOKEN e ITLA_MATRICULA y crea el handle.
   Devuelve 0 si todo va bien, -1 si falta algo. */
int  red_iniciar(Red *r);
void red_cerrar(Red *r);

/* Hace GET a una ruta (ej. "/api/v1/config").
   Devuelve el código HTTP (200, 401...) o -1 si falló la red (CURLcode). */
long red_get(Red *r, const char *ruta, Respuesta *resp);

/* Hace POST con un cuerpo JSON. Mismo valor de retorno que red_get. */
long red_post(Red *r, const char *ruta, const char *json, Respuesta *resp);

void respuesta_liberar(Respuesta *resp);

#endif
