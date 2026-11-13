#ifndef ORBIT_PROBE_H
#define ORBIT_PROBE_H

/* Game thread (script commands, see orbit_probe.c) */
void orbit_probe_circ(void);
void orbit_probe_start(int n, int every);
void orbit_probe_poll(void);

#endif
