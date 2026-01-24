/*
 * Copyright (C) 1989 by Kenneth Almquist.  All rights reserved.
 * This file is part of ash, which is distributed under the terms specified
 * by the Ash General Public License.  See the file named LICENSE.
 */

#include "../shell.h"
#include <stdlib.h>

void error();

char *stalloc(size_t nbytes) {
      register pointer p;

      if ((p = malloc(nbytes)) == NULL)
	    error("Out of space");
      return p;
}
