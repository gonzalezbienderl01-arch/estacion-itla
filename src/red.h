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
    CURL *curl;         /* handle para HTTP */
    CURL *mqtt;         /* handle aparte para publicar por MQTT */
    char  host[256];
    char  token[64];
    char  matricula[32];
    long  retry_after;  /* segundos de Retry-After de la última respuesta (0 si no vino) */
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

/* Igual que red_get/red_post, pero reintenta solo:
   - fallo de red: espera 1, 2, 4 ... hasta 30 s
   - HTTP 429 o 503: espera lo que diga Retry-After (o 1 s)
   Devuelve el último código obtenido (o -1). */
long red_get_reintentos(Red *r, const char *ruta, Respuesta *resp);
long red_post_reintentos(Red *r, const char *ruta, const char *json, Respuesta *resp);

/* Publica un mensaje MQTT (QoS 0) en un tópico, con usuario = matrícula y
   contraseña = token. Devuelve 0 si se publicó, -1 si falló (no reintenta). */
int red_publicar_mqtt(Red *r, const char *topico, const char *mensaje);

/* Pausa en milisegundos (para respetar el límite de 5 peticiones/s). */
void red_esperar_ms(long ms);

void respuesta_liberar(Respuesta *resp);

#endif
