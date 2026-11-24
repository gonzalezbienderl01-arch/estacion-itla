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
    "  ts_recibida INTEGER, ts_ack INTEGER);"
    /* Vista del nivel 2: marca cada lectura como válida (1) o inválida (0).
       Inválida si estado no es OK, si temp vale -999.0 o si falta algún campo. */
    "DROP VIEW IF EXISTS lecturas_clasificadas;"
    "CREATE VIEW lecturas_clasificadas AS"
    "  SELECT *, CASE WHEN estado = 'OK' AND temp <> -999.0"
    "    AND maquina IS NOT NULL AND ts IS NOT NULL AND temp IS NOT NULL"
    "    AND corriente IS NOT NULL AND vibracion IS NOT NULL"
    "    THEN 1 ELSE 0 END AS valida"
    "  FROM lecturas_planta;";

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

int datos_analizar_maquina(sqlite3 *db, const char *maquina, double umbral, Analisis *a)
{
    /* Todo el cálculo lo hace SQL; C solo lee una fila de resultado. */
    const char *sql =
        "SELECT"
        "  COALESCE(SUM(valida), 0),"
        "  COALESCE(SUM(1 - valida), 0),"
        "  MIN(CASE WHEN valida = 1 THEN temp END),"
        "  MAX(CASE WHEN valida = 1 THEN temp END),"
        "  ROUND(AVG(CASE WHEN valida = 1 THEN temp END), 2),"
        "  COALESCE(SUM(CASE WHEN valida = 1 AND temp > ? THEN 1 ELSE 0 END), 0)"
        " FROM lecturas_clasificadas WHERE maquina = ?;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_double(st, 1, umbral);
    sqlite3_bind_text(st, 2, maquina, -1, SQLITE_STATIC);

    int res = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        snprintf(a->maquina, sizeof a->maquina, "%s", maquina);
        a->validas      = sqlite3_column_int64(st, 0);
        a->descartadas  = sqlite3_column_int64(st, 1);
        a->temp_min     = sqlite3_column_double(st, 2);
        a->temp_max     = sqlite3_column_double(st, 3);
        a->temp_prom    = sqlite3_column_double(st, 4);
        a->sobre_umbral = sqlite3_column_int64(st, 5);
        res = 0;
    } else {
        fprintf(stderr, "Error al analizar %s: %s\n", maquina, sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return res;
}

/* Ejecuta una sentencia con un solo parámetro entero (seq). */
static int ejecutar_con_seq(sqlite3 *db, const char *sql, long long seq)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, seq);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Error SQLite (seq %lld): %s\n", seq, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int datos_nueva_telemetria(sqlite3 *db, long long ts, double temp, double corriente,
                           long long *seq)
{
    /* seq = MAX(seq) + 1 en la misma sentencia: no hay carrera ni contador en C */
    const char *sql =
        "INSERT INTO telemetria (seq, ts, temp, corriente, enviado, intentos) "
        "SELECT COALESCE(MAX(seq), 0) + 1, ?, ?, ?, 0, 0 FROM telemetria;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, ts);
    sqlite3_bind_double(st, 2, temp);
    sqlite3_bind_double(st, 3, corriente);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Error al guardar la lectura: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    *seq = sqlite3_last_insert_rowid(db);   /* seq INTEGER PRIMARY KEY = rowid */
    return 0;
}

int datos_siguiente_pendiente(sqlite3 *db, Telemetria *t)
{
    const char *sql =
        "SELECT seq, ts, temp, corriente FROM telemetria "
        "WHERE enviado = 0 ORDER BY seq LIMIT 1;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    int res = 0;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        t->seq = sqlite3_column_int64(st, 0);
        t->ts = sqlite3_column_int64(st, 1);
        t->temp = sqlite3_column_double(st, 2);
        t->corriente = sqlite3_column_double(st, 3);
        res = 1;
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr, "Error al leer la cola: %s\n", sqlite3_errmsg(db));
        res = -1;
    }
    sqlite3_finalize(st);
    return res;
}

int datos_marcar_enviada(sqlite3 *db, long long seq)
{
    return ejecutar_con_seq(db, "UPDATE telemetria SET enviado = 1 WHERE seq = ?;", seq);
}

int datos_sumar_intento(sqlite3 *db, long long seq)
{
    return ejecutar_con_seq(db, "UPDATE telemetria SET intentos = intentos + 1 WHERE seq = ?;", seq);
}

long long datos_contar_pendientes(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM telemetria WHERE enviado = 0;",
                           -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    long long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

int datos_alarma_confirmada(sqlite3 *db, const char *codigo)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT ts_ack IS NOT NULL FROM alarmas WHERE codigo = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(st, 1, codigo, -1, SQLITE_STATIC);
    int res = 0;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        res = sqlite3_column_int(st, 0);
    else if (rc != SQLITE_DONE)
        res = -1;
    sqlite3_finalize(st);
    return res;
}

int datos_guardar_alarma(sqlite3 *db, const char *codigo, const char *maquina,
                         long long ts_servidor, long long ts_recibida)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT OR IGNORE INTO alarmas (codigo, maquina, ts_servidor, ts_recibida) "
        "VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(st, 1, codigo, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, maquina, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, ts_servidor);
    sqlite3_bind_int64(st, 4, ts_recibida);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Error al guardar la alarma %s: %s\n", codigo, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int datos_marcar_ack(sqlite3 *db, const char *codigo, long long ts_ack)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "UPDATE alarmas SET ts_ack = ? WHERE codigo = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error al preparar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, ts_ack);
    sqlite3_bind_text(st, 2, codigo, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Error al confirmar %s: %s\n", codigo, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}
