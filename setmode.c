/*
 * Portable implementation of BSD setmode(3)/getmode(3).
 * Parses symbolic mode strings (e.g. "u+x,go-w", "a=rx") and applies them.
 *
 * Public domain / no copyright claimed — written for the Linux port of
 * the 4.4BSD-Lite2 Almquist Shell.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "setmode.h"

/*
 * Each clause in a mode string is stored as one of these.
 * We allow up to 32 clauses (more than enough for any real usage).
 */
#define MAX_CLAUSES 32

struct modeclause {
	int who;	/* bitmask: 04=user 02=group 01=other 0=all */
	int op;		/* '+', '-', '=' */
	mode_t bits;	/* permission bits affected */
};

struct modeset {
	int		nclauses;
	struct modeclause clauses[MAX_CLAUSES];
};

/*
 * setmode: parse a symbolic mode string.
 * Returns malloc'd opaque pointer, or NULL on error.
 * Caller must free() it.
 */
void *
setmode(const char *mode_str)
{
	struct modeset *ms;
	const char *p;
	int n;

	ms = malloc(sizeof *ms);
	if (ms == NULL)
		return NULL;
	ms->nclauses = 0;

	p = mode_str;
	n = 0;

	while (*p && n < MAX_CLAUSES) {
		struct modeclause *c = &ms->clauses[n];
		c->who  = 0;
		c->op   = 0;
		c->bits = 0;

		/* parse who */
		while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
			switch (*p++) {
			case 'u': c->who |= 04; break;
			case 'g': c->who |= 02; break;
			case 'o': c->who |= 01; break;
			case 'a': c->who |= 07; break;
			}
		}

		/* parse op */
		if (*p != '+' && *p != '-' && *p != '=') {
			free(ms);
			return NULL;
		}
		c->op = *p++;

		/* parse permissions */
		while (*p == 'r' || *p == 'w' || *p == 'x' ||
		       *p == 's' || *p == 't') {
			int who = c->who ? c->who : 07;
			switch (*p++) {
			case 'r':
				if (who & 04) c->bits |= S_IRUSR;
				if (who & 02) c->bits |= S_IRGRP;
				if (who & 01) c->bits |= S_IROTH;
				break;
			case 'w':
				if (who & 04) c->bits |= S_IWUSR;
				if (who & 02) c->bits |= S_IWGRP;
				if (who & 01) c->bits |= S_IWOTH;
				break;
			case 'x':
				if (who & 04) c->bits |= S_IXUSR;
				if (who & 02) c->bits |= S_IXGRP;
				if (who & 01) c->bits |= S_IXOTH;
				break;
			case 's':
				if (who & 04) c->bits |= S_ISUID;
				if (who & 02) c->bits |= S_ISGID;
				break;
			case 't':
				c->bits |= S_ISVTX;
				break;
			}
		}

		n++;
		if (*p == ',')
			p++;
		else if (*p != '\0') {
			free(ms);
			return NULL;
		}
	}

	if (*p != '\0') {
		free(ms);
		return NULL;
	}

	ms->nclauses = n;
	return ms;
}

/*
 * getmode: apply the parsed mode clauses to omode.
 * Returns the new mode.
 */
mode_t
getmode(const void *bbox, mode_t omode)
{
	const struct modeset *ms = bbox;
	mode_t mode = omode;
	int i;

	for (i = 0; i < ms->nclauses; i++) {
		const struct modeclause *c = &ms->clauses[i];
		mode_t mask = 0;
		int who = c->who ? c->who : 07;

		/* build the full set of bits this clause can touch */
		if (who & 04) mask |= S_IRUSR | S_IWUSR | S_IXUSR | S_ISUID;
		if (who & 02) mask |= S_IRGRP | S_IWGRP | S_IXGRP | S_ISGID;
		if (who & 01) mask |= S_IROTH | S_IWOTH | S_IXOTH;

		switch (c->op) {
		case '+':
			mode |= c->bits;
			break;
		case '-':
			mode &= ~c->bits;
			break;
		case '=':
			mode = (mode & ~mask) | c->bits;
			break;
		}
	}
	return mode;
}
