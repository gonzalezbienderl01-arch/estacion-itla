/* sensor.h - lectura simulada de temperatura y corriente */
#ifndef SENSOR_H
#define SENSOR_H

#include <time.h>

typedef struct {
    double temp;        /* °C, entre 25 y 45 */
    double corriente;   /* A, entre 2 y 5 */
} LecturaSensor;

/* Prepara el generador de números aleatorios (una vez al empezar). */
void sensor_iniciar(void);

/* Simula una lectura para el instante t: onda lenta + ruido. */
LecturaSensor sensor_leer(time_t t);

#endif
