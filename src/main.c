/* main.c - lee argv y llama al subcomando */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "red.h"
#include "datos.h"
#include "enviar.h"

#define RUTA_DB      "estacion.db"
#define LIMITE_PAG   200     /* filas por página (máximo que acepta el servidor) */
#define PAUSA_MS     250     /* entre páginas: 4 peticiones/s, bajo el límite de 5 */
#define MAX_PAGINAS  1000    /* tope de seguridad contra un bucle infinito */
#define MAX_MAQUINAS 8

/* Nivel 0: GET /api/v1/config, guardar en la tabla config e imprimir. */
static int cmd_config(void)
{
    Red red;
    if (red_iniciar(&red) != 0)
        return 1;

    Respuesta resp;
    long codigo = red_get(&red, "/api/v1/config", &resp);
    if (codigo != 200) {
        if (codigo == 401)
            fprintf(stderr, "HTTP 401: token rechazado. Revisa ITLA_TOKEN.\n");
        else if (codigo > 0)
            fprintf(stderr, "HTTP %ld inesperado en /config: %s\n",
                    codigo, resp.datos ? resp.datos : "");
        respuesta_liberar(&resp);
        red_cerrar(&red);
        return 1;
    }

    sqlite3 *db = datos_abrir(RUTA_DB);
    if (db == NULL || datos_guardar_config(db, resp.datos) != 0) {
        datos_cerrar(db);
        respuesta_liberar(&resp);
        red_cerrar(&red);
        return 1;
    }
    respuesta_liberar(&resp);
    red_cerrar(&red);

    /* Leemos de la base lo que acabamos de guardar */
    char umbral[32] = "?", intervalo[32] = "?", hora[32] = "0";
    datos_leer_config(db, "umbral_temp", umbral, sizeof umbral);
    datos_leer_config(db, "intervalo_s", intervalo, sizeof intervalo);
    datos_leer_config(db, "hora", hora, sizeof hora);
    datos_cerrar(db);

    time_t t = (time_t)strtoll(hora, NULL, 10);
    struct tm tm;
    char hora_txt[64] = "?";
    if (localtime_r(&t, &tm) != NULL)
        strftime(hora_txt, sizeof hora_txt, "%Y-%m-%d %H:%M:%S", &tm);

    printf("umbral_temp  = %s\n", umbral);
    printf("intervalo_s  = %s\n", intervalo);
    printf("hora servidor= %s (%s)\n", hora_txt, hora);
    return 0;
}

/* Pide todas las páginas de /lecturas y las guarda. Debe llamarse
   dentro de una transacción. Devuelve 0 si llegó a la última página. */
static int bajar_paginas(Red *red, sqlite3 *db)
{
    long long desde = 0;
    int total = 0, nuevas = 0;

    for (int n = 1; n <= MAX_PAGINAS; n++) {
        char ruta[128];
        snprintf(ruta, sizeof ruta, "/api/v1/lecturas?desde_id=%lld&limite=%d",
                 desde, LIMITE_PAG);

        Respuesta resp;
        long codigo = red_get_reintentos(red, ruta, &resp);
        if (codigo != 200) {
            fprintf(stderr, "Página %d: %s %ld\n", n,
                    codigo == -1 ? "sin red, código" : "HTTP", codigo);
            respuesta_liberar(&resp);
            return -1;
        }

        Pagina pag;
        int rc = datos_guardar_lecturas(db, resp.datos, &pag);
        respuesta_liberar(&resp);
        if (rc != 0)
            return -1;

        total += pag.filas;
        nuevas += pag.nuevas;
        printf("  página %2d: desde_id=%-5lld %3d filas, %3d nuevas\n",
               n, desde, pag.filas, pag.nuevas);

        if (pag.filas < LIMITE_PAG || pag.max_id <= desde) {
            printf("Recibidas %d filas (%d nuevas, %d repetidas ignoradas)\n",
                   total, nuevas, total - nuevas);
            return 0;                       /* última página */
        }
        desde = pag.max_id;
        red_esperar_ms(PAUSA_MS);
    }
    fprintf(stderr, "Error: demasiadas páginas, se detiene por seguridad\n");
    return -1;
}

/* Envía filas_unicas y suma_ids a /respuestas/descarga. */
static int enviar_resumen_descarga(Red *red, sqlite3 *db)
{
    long long filas = 0, suma = 0;
    if (datos_resumen_lecturas(db, &filas, &suma) != 0)
        return -1;
    printf("filas_unicas = %lld\nsuma_ids     = %lld\n", filas, suma);

    char json[128];
    snprintf(json, sizeof json, "{\"filas_unicas\":%lld,\"suma_ids\":%lld}", filas, suma);

    Respuesta resp;
    long codigo = red_post_reintentos(red, "/api/v1/respuestas/descarga", json, &resp);
    if (codigo != 200) {
        fprintf(stderr, "HTTP %ld al enviar la respuesta: %s\n",
                codigo, resp.datos ? resp.datos : "");
        respuesta_liberar(&resp);
        return -1;
    }
    int correcto = (strstr(resp.datos, "correcto=1") != NULL);
    printf("Servidor: %s", resp.datos);
    printf(correcto ? "Nivel 1 superado.\n" : "El servidor dice que no coincide.\n");
    respuesta_liberar(&resp);
    return correcto ? 0 : -1;
}

/* Nivel 1: bajar las lecturas por páginas, guardarlas y enviar el resumen. */
static int cmd_descargar(void)
{
    Red red;
    if (red_iniciar(&red) != 0)
        return 1;
    sqlite3 *db = datos_abrir(RUTA_DB);
    if (db == NULL) {
        red_cerrar(&red);
        return 1;
    }

    int res = -1;
    if (datos_iniciar(db) == 0) {
        /* todas las inserciones en una sola transacción */
        if (bajar_paginas(&red, db) == 0 && datos_confirmar(db) == 0)
            res = enviar_resumen_descarga(&red, db);
        else
            datos_cancelar(db);
    }

    datos_cerrar(db);
    red_cerrar(&red);
    return res == 0 ? 0 : 1;
}

/* Separa "M1,M2,M3,M4" en nombres. Devuelve cuántas máquinas hay. */
static int separar_maquinas(char *lista, char nombres[][16], int max)
{
    int n = 0;
    char *p = lista;
    while (p != NULL && *p != '\0' && n < max) {
        char *coma = strchr(p, ',');
        if (coma != NULL)
            *coma = '\0';
        size_t largo = strlen(p);
        if (largo > 0 && largo < 16) {          /* nombres vacíos o enormes se ignoran */
            memcpy(nombres[n], p, largo + 1);
            n++;
        }
        p = (coma != NULL) ? coma + 1 : NULL;
    }
    return n;
}

/* Arma el JSON {"maquinas":[...]} en buf. Devuelve 0 si cupo entero. */
static int armar_json_analisis(const Analisis *a, int n, char *buf, size_t tam)
{
    size_t usado = 0;
    int k = snprintf(buf, tam, "{\"maquinas\":[");
    if (k < 0 || (size_t)k >= tam)
        return -1;
    usado = (size_t)k;

    for (int i = 0; i < n; i++) {
        k = snprintf(buf + usado, tam - usado,
                     "%s{\"maquina\":\"%s\",\"validas\":%lld,\"descartadas\":%lld,"
                     "\"temp_min\":%.2f,\"temp_max\":%.2f,\"temp_prom\":%.2f,"
                     "\"sobre_umbral\":%lld}",
                     i > 0 ? "," : "", a[i].maquina, a[i].validas, a[i].descartadas,
                     a[i].temp_min, a[i].temp_max, a[i].temp_prom, a[i].sobre_umbral);
        if (k < 0 || (size_t)k >= tam - usado)
            return -1;
        usado += (size_t)k;
    }
    k = snprintf(buf + usado, tam - usado, "]}");
    return (k < 0 || (size_t)k >= tam - usado) ? -1 : 0;
}

/* Calcula las estadísticas de todas las máquinas de la config. */
static int calcular_analisis(sqlite3 *db, Analisis *a, int *n)
{
    char umbral_txt[32], lista[128];
    if (datos_leer_config(db, "umbral_temp", umbral_txt, sizeof umbral_txt) != 0 ||
        datos_leer_config(db, "maquinas", lista, sizeof lista) != 0) {
        fprintf(stderr, "Error: no hay config guardada. Corre primero ./estacion config\n");
        return -1;
    }
    double umbral = strtod(umbral_txt, NULL);

    char nombres[MAX_MAQUINAS][16];
    *n = separar_maquinas(lista, nombres, MAX_MAQUINAS);
    printf("umbral_temp = %.2f\n", umbral);
    printf("%-4s %7s %11s %9s %9s %9s %12s\n", "maq", "validas", "descartadas",
           "temp_min", "temp_max", "temp_prom", "sobre_umbral");
    for (int i = 0; i < *n; i++) {
        if (datos_analizar_maquina(db, nombres[i], umbral, &a[i]) != 0)
            return -1;
        printf("%-4s %7lld %11lld %9.2f %9.2f %9.2f %12lld\n", a[i].maquina,
               a[i].validas, a[i].descartadas, a[i].temp_min, a[i].temp_max,
               a[i].temp_prom, a[i].sobre_umbral);
    }
    return (*n > 0) ? 0 : -1;
}

/* Envía el JSON y muestra qué campos dio el servidor por buenos. */
static int enviar_analisis(const char *json)
{
    Red red;
    if (red_iniciar(&red) != 0)
        return -1;

    Respuesta resp;
    long codigo = red_post_reintentos(&red, "/api/v1/respuestas/analisis", json, &resp);
    int res = -1;
    if (codigo == 200) {
        int malos = 0;
        for (const char *p = resp.datos; (p = strstr(p, "=mal")) != NULL; p++)
            malos++;
        printf("Servidor:\n%s", resp.datos);
        if (malos == 0) {
            printf("Nivel 2 superado: 24 de 24 campos correctos.\n");
            res = 0;
        } else {
            printf("%d campos marcados como mal.\n", malos);
        }
    } else if (codigo == 403) {
        fprintf(stderr, "HTTP 403: ya no quedan intentos de análisis.\n");
    } else {
        fprintf(stderr, "HTTP %ld al enviar el análisis: %s\n",
                codigo, resp.datos ? resp.datos : "");
    }
    respuesta_liberar(&resp);
    red_cerrar(&red);
    return res;
}

/* Nivel 2: estadísticas por máquina con SQL. Con --ver solo muestra, no envía. */
static int cmd_analizar(int enviar)
{
    sqlite3 *db = datos_abrir(RUTA_DB);
    if (db == NULL)
        return 1;

    Analisis a[MAX_MAQUINAS];
    int n = 0;
    int rc = calcular_analisis(db, a, &n);
    datos_cerrar(db);
    if (rc != 0)
        return 1;

    char json[2048];
    if (armar_json_analisis(a, n, json, sizeof json) != 0) {
        fprintf(stderr, "Error: el JSON no cabe en el búfer\n");
        return 1;
    }
    printf("JSON: %s\n", json);
    if (!enviar) {
        printf("(modo --ver: no se envió nada, no se gastó ningún intento)\n");
        return 0;
    }
    return enviar_analisis(json) == 0 ? 0 : 1;
}

static void uso(const char *prog)
{
    fprintf(stderr,
            "Uso: %s <subcomando>\n"
            "  config     nivel 0: lee la configuración del servidor\n"
            "  descargar  nivel 1: baja las lecturas y envía el resumen\n"
            "  analizar   nivel 2: estadísticas por máquina (--ver: sin enviar)\n"
            "  enviar     niveles 3 y 4: telemetría + latido MQTT (Ctrl+C para parar)\n"
            "  escuchar   nivel 5 (pendiente)\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        uso(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "config") == 0)
        return cmd_config();
    if (strcmp(argv[1], "descargar") == 0)
        return cmd_descargar();

    if (strcmp(argv[1], "analizar") == 0)
        return cmd_analizar(!(argc > 2 && strcmp(argv[2], "--ver") == 0));

    if (strcmp(argv[1], "enviar") == 0)
        return enviar_ejecutar();

    if (strcmp(argv[1], "escuchar") == 0) {
        fprintf(stderr, "'%s' todavía no está implementado.\n", argv[1]);
        return 1;
    }
    uso(argv[0]);
    return 1;
}
