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

#endif
