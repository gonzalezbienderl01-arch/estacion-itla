/* enviar.h - niveles 3 y 4: telemetría resiliente y latido MQTT */
#ifndef ENVIAR_H
#define ENVIAR_H

/* Bucle principal de ./estacion enviar. Termina con Ctrl+C.
   Devuelve 0 si terminó bien, 1 si hubo un error fatal. */
int enviar_ejecutar(void);

#endif
