/*********************************************************/
/*           Programación para mecatrónicos               */
/*  Nombre:    Bienderl Gonzalez                          */
/*  Matricula: 2024-1302                                  */
/*  Seccion:   Miercoles                                  */
/*  Practica:  Estacion de Telemetria                     */
/*  Fecha:     23/11/2026                                 */
/*  Link Practica: https://github.com/gonzalezbienderl01-arch/estacion-itla */
/*********************************************************/

/* sensor.c - sensor simulado (no hay DHT22 conectado) */
#include "sensor.h"

#include <math.h>
#include <stdlib.h>

#define PI          3.14159265358979323846
#define PERIODO_T   600.0   /* la temperatura sube y baja en 10 minutos */
#define PERIODO_I   300.0   /* la corriente, en 5 minutos */

/* Ruido uniforme entre -amp y +amp. */
static double ruido(double amp)
{
    return ((double)rand() / RAND_MAX * 2.0 - 1.0) * amp;
}

static double limitar(double v, double min, double max)
{
    return v < min ? min : (v > max ? max : v);
}

void sensor_iniciar(void)
{
    srand((unsigned)time(NULL));
}

LecturaSensor sensor_leer(time_t t)
{
    double x = (double)t;
    LecturaSensor l;
    /* centro 35 °C, amplitud 8 °C -> 27..43, más ruido de ±0.5 */
    l.temp = limitar(35.0 + 8.0 * sin(2.0 * PI * x / PERIODO_T) + ruido(0.5), 25.0, 45.0);
    /* centro 3.5 A, amplitud 1.2 A -> 2.3..4.7, más ruido de ±0.2 */
    l.corriente = limitar(3.5 + 1.2 * sin(2.0 * PI * x / PERIODO_I + 1.0) + ruido(0.2), 2.0, 5.0);
    /* redondeo a 2 decimales, como se envía */
    l.temp = round(l.temp * 100.0) / 100.0;
    l.corriente = round(l.corriente * 100.0) / 100.0;
    return l;
}
