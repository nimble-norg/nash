/*
 * Portable setmode/getmode — Linux port of 4.4BSD-Lite2 ash.
 */
#ifndef SETMODE_H
#define SETMODE_H

#include <sys/types.h>

void   *setmode(const char *);
mode_t  getmode(const void *, mode_t);

#endif /* SETMODE_H */
