#if !defined(BUILTIN) && !defined(SHELL)
#ifndef lint
static char copyright[] =
"@(#) Copyright (c) 1989, 1993\n\
\tThe Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */
#endif

#ifndef lint
static char sccsid[] = "@(#)printf.c\t8.2 (Berkeley) 3/22/95";
#endif /* not lint */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"
#include "output.h"
#include "error.h"

#define PF(f, func) { \
	char pf_buf[4096]; \
	if (fieldwidth) \
		if (precision) \
			snprintf(pf_buf, sizeof pf_buf, f, fieldwidth, precision, func); \
		else \
			snprintf(pf_buf, sizeof pf_buf, f, fieldwidth, func); \
	else if (precision) \
		snprintf(pf_buf, sizeof pf_buf, f, precision, func); \
	else \
		snprintf(pf_buf, sizeof pf_buf, f, func); \
	out1str(pf_buf); \
}

static int asciicode(void);
static void escape(char *fmt);
static int getchr(void);
static double getdouble(void);
static int getint(int *ip);
static int getlong(long *lp);
static char *getstr(void);
static char *mklong(char *str, int ch);
static void warnx_simple(const char *fmt, const char *s1);

static char **gargv;
static char *Number = "+-.0123456789";

int
printfcmd(int argc, char *argv[]) {
	static char *skip1, *skip2;
	int ch, end, fieldwidth, precision;
	char convch, nextch, *format, *fmt, *start;

	flushout(&output);

	argc--;
	argv++;

	if (argc < 1) {
		outfmt(&errout, "usage: printf format [arg ...]\n");
		flushout(&errout);
		return (1);
	}

	skip1 = "#-+ 0";
	skip2 = "*0123456789";

	escape(fmt = format = *argv);
	gargv = ++argv;

	for (;;) {
		end = 0;
next:		for (start = fmt;; ++fmt) {
			if (!*fmt) {
				if (end == 1) {
					warnx_simple("missing format character", NULL);
					return (1);
				}
				end = 1;
				if (fmt > start) {
					out1str(start);
				}
				if (!*gargv) {
					return (0);
				}
				fmt = format;
				goto next;
			}
			if (*fmt == '%') {
				if (*++fmt != '%') {
					break;
				}
				*fmt++ = '\0';
				out1str(start);
				goto next;
			}
		}

		for (; strchr(skip1, *fmt); ++fmt);
		if (*fmt == '*') {
			if (getint(&fieldwidth)) {
				return (1);
			}
		} else {
			fieldwidth = 0;
		}

		for (; strchr(skip2, *fmt); ++fmt);
		if (*fmt == '.') {
			++fmt;
		}
		if (*fmt == '*') {
			if (getint(&precision)) {
				return (1);
			}
		} else {
			precision = 0;
		}

		for (; strchr(skip2, *fmt); ++fmt);
		if (!*fmt) {
			warnx_simple("missing format character", NULL);
			return (1);
		}

		convch = *fmt;
		nextch = *++fmt;
		*fmt = '\0';
		switch(convch) {
		case 'c': {
			char p;
			p = (char)getchr();
			PF(start, p);
			break;
		}
		case 's': {
			char *p;
			p = getstr();
			PF(start, p);
			break;
		}
		case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': {
			long p;
			char *f;
			if ((f = mklong(start, convch)) == NULL) {
				return (1);
			}
			if (getlong(&p)) {
				return (1);
			}
			PF(f, p);
			break;
		}
		case 'e': case 'E': case 'f': case 'g': case 'G': {
			double p;
			p = getdouble();
			PF(start, p);
			break;
		}
		default:
			warnx_simple("illegal format character", NULL);
			return (1);
		}
		*fmt = nextch;
	}
}

static char *mklong(char *str, int ch) {
	static char copy[64];
	size_t len;

	len = strlen(str) + 2;
	if (len > sizeof(copy)) {
		return (NULL);
	}
	memmove(copy, str, len - 3);
	copy[len - 3] = 'l';
	copy[len - 2] = (char)ch;
	copy[len - 1] = '\0';
	return (copy);
}

static void escape(char *fmt) {
	char *store;
	int value, c, i;

	for (store = fmt; (c = *fmt) != '\0'; ++fmt, ++store) {
		if (c != '\\') {
			*store = (char)c;
			continue;
		}
		switch (*++fmt) {
		case '\0':
			*store = '\\';
			*++store = '\0';
			return;
		case '\\':
		case '\'':
			*store = *fmt;
			break;
		case 'a': *store = '\7'; break;
		case 'b': *store = '\b'; break;
		case 'f': *store = '\f'; break;
		case 'n': *store = '\n'; break;
		case 'r': *store = '\r'; break;
		case 't': *store = '\t'; break;
		case 'v': *store = '\v'; break;
		case '0': case '1': case '2': case '3':
		case '4': case '5': case '6': case '7':
			for (i = 3, value = 0;
			    i-- && *fmt >= '0' && *fmt <= '7'; ++fmt) {
				value <<= 3;
				value += *fmt - '0';
			}
			--fmt;
			*store = (char)value;
			break;
		default:
			*store = *fmt;
			break;
		}
	}
	*store = '\0';
}

static int getchr(void) {
	if (!*gargv) {
		return ('\0');
	}
	return ((int)**gargv++);
}

static char *getstr(void) {
	if (!*gargv) {
		return ("");
	}
	return (*gargv++);
}

static int getint(int *ip) {
	long val;

	if (getlong(&val)) {
		return (1);
	}
	if (val > INT_MAX || val < INT_MIN) {
		warnx_simple("out of range", *gargv);
		return (1);
	}
	*ip = (int)val;
	return (0);
}

static int getlong(long *lp) {
	long val;
	char *ep;

	if (!*gargv) {
		*lp = 0;
		return (0);
	}
	if (strchr(Number, **gargv)) {
		errno = 0;
		val = strtol(*gargv, &ep, 0);
		if (*ep != '\0') {
			warnx_simple("illegal number", *gargv);
			return (1);
		}
		if (errno == ERANGE) {
			warnx_simple(strerror(ERANGE), *gargv);
			return (1);
		}
		*lp = val;
		++gargv;
		return (0);
	}
	*lp = (long)asciicode();
	return (0);
}

static double getdouble(void) {
	if (!*gargv) {
		return ((double)0);
	}
	if (strchr(Number, **gargv)) {
		return (atof(*gargv++));
	}
	return ((double)asciicode());
}

static int asciicode(void) {
	int ch;

	ch = **gargv;
	if (ch == '\'' || ch == '"') {
		ch = (*gargv)[1];
	}
	++gargv;
	return (ch);
}

static void warnx_simple(const char *fmt, const char *s1) {
	if (s1) {
		outfmt(&errout, "printf: %s: %s\n", s1, fmt);
	} else {
		outfmt(&errout, "printf: %s\n", fmt);
	}
	flushout(&errout);
}
