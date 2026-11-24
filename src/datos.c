/* datos.c - base de datos estacion.db con SQLite */
#include "datos.h"

#include <stdio.h>
#include <string.h>

/* Las 4 tablas obligatorias. No llevan datos variables,
   así que aquí sí se puede usar sqlite3_exec. */
static const char *ESQUEMA =
    "CREATE TABLE IF NOT EXISTS config ("
    "  clave TEXT PRIMARY KEY,"
    "  valor TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS lecturas_planta ("
    "  id INTEGER PRIMARY KEY,"
    "  maquina TEXT, ts INTEGER, temp REAL, corriente REAL,"
    "  vibracion REAL, estado TEXT);"
    "CREATE TABLE IF NOT EXISTS telemetria ("
    "  seq INTEGER PRIMARY KEY,"
    "  ts INTEGER NOT NULL, temp REAL NOT NULL, corriente REAL NOT NULL,"
    "  enviado INTEGER NOT NULL DEFAULT 0 CHECK (enviado IN (0, 1)),"
    "  intentos INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS alarmas ("
    "  codigo TEXT PRIMARY KEY,"
    "  maquina TEXT, ts_servidor INTEGER,"
    "  ts_recibida INTEGER, ts_ack INTEGER);";

/* Ejecuta SQL fijo y muestra el error si lo hay. */
static int ejecutar(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Error SQLite: %s\n", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

sqlite3 *datos_abrir(const char *ruta)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(ruta, &db) != SQLITE_OK) {
        fprintf(stderr, "Error al abrir %s: %s\n", ruta, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    /* enviar y escuchar usan la base a la vez: WAL + espera de 2 s */
    sqlite3_busy_timeout(db, 2000);
    if (ejecutar(db, "PRAGMA journal_mode=WAL;") != 0 ||
        ejecutar(db, ESQUEMA) != 0) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void datos_cerrar(sqlite3 *db)
{
    sqlite3_close(db);
}

int datos_guardar_config(sqlite3 *db, char *texto)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT OR REPLACE INTO config (clave, valor) VALUES (?, ?);";

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    if (ejecutar(db, "BEGIN;") != 0) {
        sqlite3_finalize(st);
        return -1;
    }

    int ok = 0, guardadas = 0;
    char *linea = texto;
    while (linea != NULL && *linea != '\0') {
        /* separa la línea actual y avanza a la siguiente */
        char *fin = strchr(linea, '\n');
        if (fin != NULL)
            *fin = '\0';
        char *cr = strchr(linea, '\r');
        if (cr != NULL)
            *cr = '\0';

        char *igual = strchr(linea, '=');
        if (igual != NULL && igual != linea) {
            *igual = '\0';
            sqlite3_bind_text(st, 1, linea, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, igual + 1, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) != SQLITE_DONE) {
                fprintf(stderr, "Error al guardar '%s': %s\n", linea, sqlite3_errmsg(db));
                ok = -1;
                break;
            }
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            guardadas++;
        }
        linea = (fin != NULL) ? fin + 1 : NULL;
    }
    sqlite3_finalize(st);

    if (ok != 0 || guardadas == 0) {
        ejecutar(db, "ROLLBACK;");
        if (guardadas == 0)
            fprintf(stderr, "Error: /config no trajo líneas clave=valor\n");
        return -1;
    }
    return ejecutar(db, "COMMIT;");
}

int datos_leer_config(sqlite3 *db, const char *clave, char *valor, size_t n)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT valor FROM config WHERE clave = ?;";
    int res = -1;

    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(st, 1, clave, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        int k = snprintf(valor, n, "%s", v ? (const char *)v : "");
        res = (k >= 0 && (size_t)k < n) ? 0 : -1;
    }
    sqlite3_finalize(st);
    return res;
}
