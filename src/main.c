/* main.c - lee argv y llama al subcomando */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "red.h"
#include "datos.h"

#define RUTA_DB "estacion.db"

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

static void uso(const char *prog)
{
    fprintf(stderr,
            "Uso: %s <subcomando>\n"
            "  config     nivel 0: lee la configuración del servidor\n"
            "  descargar  nivel 1 (pendiente)\n"
            "  analizar   nivel 2 (pendiente)\n"
            "  enviar     niveles 3 y 4 (pendiente)\n"
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

    if (strcmp(argv[1], "descargar") == 0 || strcmp(argv[1], "analizar") == 0 ||
        strcmp(argv[1], "enviar") == 0 || strcmp(argv[1], "escuchar") == 0) {
        fprintf(stderr, "'%s' todavía no está implementado.\n", argv[1]);
        return 1;
    }
    uso(argv[0]);
    return 1;
}
