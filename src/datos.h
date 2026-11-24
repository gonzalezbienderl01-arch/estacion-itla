/* datos.h - todo lo de SQLite */
#ifndef DATOS_H
#define DATOS_H

#include <stddef.h>
#include "sqlite3.h"

/* Abre (o crea) la base, activa WAL y busy_timeout y crea las tablas.
   Devuelve NULL si algo falla. */
sqlite3 *datos_abrir(const char *ruta);
void     datos_cerrar(sqlite3 *db);

/* Guarda las líneas "clave=valor" de /config en la tabla config,
   todo dentro de una transacción. Devuelve 0 si va bien. */
int datos_guardar_config(sqlite3 *db, char *texto);

/* Lee un valor de la tabla config. Devuelve 0 si lo encontró. */
int datos_leer_config(sqlite3 *db, const char *clave, char *valor, size_t n);

/* Transacciones: BEGIN / COMMIT / ROLLBACK. Devuelven 0 si van bien. */
int datos_iniciar(sqlite3 *db);
int datos_confirmar(sqlite3 *db);
int datos_cancelar(sqlite3 *db);

/* Resultado de guardar una página del CSV de /lecturas. */
typedef struct {
    int       filas;    /* filas de datos que traía la página */
    int       nuevas;   /* filas insertadas (las repetidas se ignoran) */
    long long max_id;   /* id más alto de la página */
} Pagina;

/* Guarda en lecturas_planta las filas del CSV (con cabecera).
   Los campos vacíos se guardan como NULL. Usa INSERT OR IGNORE.
   No abre transacción: la abre quien llama. Devuelve 0 si va bien. */
int datos_guardar_lecturas(sqlite3 *db, char *csv, Pagina *pag);

/* COUNT(*) y SUM(id) de lecturas_planta. Devuelve 0 si va bien. */
int datos_resumen_lecturas(sqlite3 *db, long long *filas, long long *suma_ids);

/* Estadísticas de una máquina (nivel 2), calculadas con SQL. */
typedef struct {
    char      maquina[16];
    long long validas;
    long long descartadas;
    double    temp_min;
    double    temp_max;
    double    temp_prom;     /* ya redondeado a 2 decimales por SQL */
    long long sobre_umbral;  /* lecturas válidas con temp > umbral_temp */
} Analisis;

/* Calcula las estadísticas de una máquina. Devuelve 0 si va bien. */
int datos_analizar_maquina(sqlite3 *db, const char *maquina, double umbral, Analisis *a);

#endif
