/* datos.c - base de datos estacion.db con SQLite */
#include "datos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAMPOS_CSV 7   /* id,maquina,ts,temp,corriente,vibracion,estado */

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

int datos_iniciar(sqlite3 *db)   { return ejecutar(db, "BEGIN;"); }
int datos_confirmar(sqlite3 *db) { return ejecutar(db, "COMMIT;"); }
int datos_cancelar(sqlite3 *db)  { return ejecutar(db, "ROLLBACK;"); }

/* Corta la línea en campos separados por coma SIN saltarse los vacíos
   (strtok trataría ",," como una sola coma). Devuelve cuántos campos hay. */
static int separar_campos(char *linea, char *campos[], int max)
{
    int n = 0;
    char *p = linea;
    while (n < max) {
        campos[n++] = p;
        char *coma = strchr(p, ',');
        if (coma == NULL)
            break;
        *coma = '\0';
        p = coma + 1;
    }
    return n;
}

/* Enlaza un número real; si el campo está vacío o no es número, NULL. */
static void bind_real(sqlite3_stmt *st, int i, const char *txt)
{
    char *fin;
    double v = strtod(txt, &fin);
    if (txt[0] == '\0' || *fin != '\0')
        sqlite3_bind_null(st, i);
    else
        sqlite3_bind_double(st, i, v);
}

/* Enlaza un entero; si el campo está vacío o no es número, NULL. */
static void bind_entero(sqlite3_stmt *st, int i, const char *txt)
{
    char *fin;
    long long v = strtoll(txt, &fin, 10);
    if (txt[0] == '\0' || *fin != '\0')
        sqlite3_bind_null(st, i);
    else
        sqlite3_bind_int64(st, i, v);
}

/* Enlaza un texto; si está vacío, NULL. */
static void bind_texto(sqlite3_stmt *st, int i, const char *txt)
{
    if (txt[0] == '\0')
        sqlite3_bind_null(st, i);
    else
        sqlite3_bind_text(st, i, txt, -1, SQLITE_TRANSIENT);
}

/* Inserta una línea ya separada. Devuelve 1 si era nueva, 0 si repetida, -1 error. */
static int insertar_lectura(sqlite3 *db, sqlite3_stmt *st, char *campos[], long long id)
{
    sqlite3_bind_int64(st, 1, id);
    bind_texto(st, 2, campos[1]);
    bind_entero(st, 3, campos[2]);
    bind_real(st, 4, campos[3]);
    bind_real(st, 5, campos[4]);
    bind_real(st, 6, campos[5]);
    bind_texto(st, 7, campos[6]);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Error al insertar id %lld: %s\n", id, sqlite3_errmsg(db));
        return -1;
    }
    return sqlite3_changes(db);   /* 0 si INSERT OR IGNORE la ignoró */
}

int datos_guardar_lecturas(sqlite3 *db, char *csv, Pagina *pag)
{
    const char *sql =
        "INSERT OR IGNORE INTO lecturas_planta "
        "(id, maquina, ts, temp, corriente, vibracion, estado) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    pag->filas = pag->nuevas = 0;
    pag->max_id = 0;
    int res = 0;
    char *linea = csv;
    while (linea != NULL && *linea != '\0') {
        char *fin = strchr(linea, '\n');
        if (fin != NULL)
            *fin = '\0';
        char *cr = strchr(linea, '\r');
        if (cr != NULL)
            *cr = '\0';

        char *campos[CAMPOS_CSV];
        char *fin_id;
        if (linea[0] != '\0' &&
            separar_campos(linea, campos, CAMPOS_CSV) == CAMPOS_CSV) {
            long long id = strtoll(campos[0], &fin_id, 10);
            if (campos[0][0] != '\0' && *fin_id == '\0') {  /* la cabecera no pasa */
                int r = insertar_lectura(db, st, campos, id);
                if (r < 0) {
                    res = -1;
                    break;
                }
                pag->filas++;
                pag->nuevas += r;
                if (id > pag->max_id)
                    pag->max_id = id;
            }
        }
        linea = (fin != NULL) ? fin + 1 : NULL;
    }
    sqlite3_finalize(st);
    return res;
}

int datos_resumen_lecturas(sqlite3 *db, long long *filas, long long *suma_ids)
{
    const char *sql = "SELECT COUNT(*), COALESCE(SUM(id), 0) FROM lecturas_planta;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    int res = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *filas = sqlite3_column_int64(st, 0);
        *suma_ids = sqlite3_column_int64(st, 1);
        res = 0;
    } else {
        fprintf(stderr, "Error al contar lecturas: %s\n", sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return res;
}
