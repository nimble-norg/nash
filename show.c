/*
 * Copyright (C) 1989 by Kenneth Almquist.  All rights reserved.
 * This file is part of ash, which is distributed under the terms specified
 * by the Ash General Public License.  See the file named LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell.h"
#include "parser.h"
#include "nodes.h"
#include "mystring.h"

/*
 * Debugging stuff.
 */

FILE *tracefile;

void trputc(int c) {
#ifdef DEBUG
      if (tracefile == NULL)
	    return;
      putc(c, tracefile);
      if (c == '\n')
	    fflush(tracefile);
#endif
}

void trace(const char *fmt, ...) {
#ifdef DEBUG
      va_list ap;
      if (tracefile == NULL)
	    return;
      va_start(ap, fmt);
      vfprintf(tracefile, fmt, ap);
      va_end(ap);
      if (strchr(fmt, '\n'))
	    fflush(tracefile);
#endif
}

void trputs(const char *s) {
#ifdef DEBUG
      if (tracefile == NULL)
	    return;
      fputs(s, tracefile);
      if (strchr(s, '\n'))
	    fflush(tracefile);
#endif
}

void trstring(const char *s) {
      const char *p;
      char c;

#ifdef DEBUG
      if (tracefile == NULL)
	    return;
      putc('"', tracefile);
      for (p = s ; *p ; p++) {
	    switch (*p) {
	    case '\n':  c = 'n';  goto backslash;
	    case '\t':  c = 't';  goto backslash;
	    case '\r':  c = 'r';  goto backslash;
	    case '"':   c = '"';  goto backslash;
	    case '\\':  c = '\\';  goto backslash;
	    case CTLESC:  c = 'e';  goto backslash;
	    case CTLVAR:  c = 'v';  goto backslash;
	    case CTLVAR+CTLQUOTE:  c = 'V';  goto backslash;
	    case CTLBACKQ:  c = 'q';  goto backslash;
	    case CTLBACKQ+CTLQUOTE:  c = 'Q';  goto backslash;
backslash:	  putc('\\', tracefile);
		  putc(c, tracefile);
		  break;
	    default:
		  if (*p >= ' ' && *p <= '~')
			putc(*p, tracefile);
		  else {
			putc('\\', tracefile);
			putc((*p >> 6) & 03, tracefile);
			putc((*p >> 3) & 07, tracefile);
			putc(*p & 07, tracefile);
		  }
		  break;
	    }
      }
      putc('"', tracefile);
#endif
}

void trargs(char **ap) {
#ifdef DEBUG
      if (tracefile == NULL)
	    return;
      while (*ap) {
	    trstring(*ap++);
	    if (*ap)
		  putc(' ', tracefile);
	    else
		  putc('\n', tracefile);
      }
      fflush(tracefile);
#endif
}

void opentrace(void) {
      char s[100];
      const char *p;
      int flags;

#ifdef DEBUG
      if ((p = getenv("HOME")) == NULL)
	    p = "/tmp";
      scopy(p, s);
      strcat(s, "/trace");
      if ((tracefile = fopen(s, "a")) == NULL) {
	    fprintf(stderr, "Can't open %s\n", s);
	    exit(2);
      }
#ifdef O_APPEND
      if ((flags = fcntl(fileno(tracefile), F_GETFL, 0)) >= 0)
	    fcntl(fileno(tracefile), F_SETFL, flags | O_APPEND);
#endif
      fputs("\nTracing started.\n", tracefile);
      fflush(tracefile);
#endif
}
