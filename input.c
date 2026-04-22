/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static char sccsid[] = "@(#)input.c	8.3 (Berkeley) 6/9/95";
#endif /* not lint */

#include <stdio.h>	/* defines BUFSIZ */
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/*
 * This file implements the input routines used by the parser.
 */

#include "shell.h"
#include "redir.h"
#include "syntax.h"
#include "input.h"
#include "output.h"
#include "options.h"
#include "memalloc.h"
#include "error.h"
#include "alias.h"
#include "parser.h"
#ifndef NO_HISTORY
#include "myhistedit.h"
#endif

#define EOF_NLEFT -99		/* value of parsenleft when EOF pushed back */

MKINIT
struct strpush {
	struct strpush *prev;	/* preceding string on stack */
	char *prevstring;
	int prevnleft;
	struct alias *ap;	/* if push was associated with an alias */
};

/*
 * The parsefile structure pointed to by the global variable parsefile
 * contains information about the current file being read.
 */

MKINIT
struct parsefile {
	struct parsefile *prev;	/* preceding file on stack */
	int linno;		/* current line */
	int fd;			/* file descriptor (or -1 if string) */
	int nleft;		/* number of chars left in buffer */
	char *nextc;		/* next char in buffer */
	char *buf;		/* input buffer */
	struct strpush *strpush; /* for pushing strings at this level */
	struct strpush basestrpush; /* so pushing one is fast */
};


int plinno = 1;			/* input line number */
MKINIT int parsenleft;		/* copy of parsefile->nleft */
char *parsenextc;		/* copy of parsefile->nextc */
MKINIT struct parsefile basepf;	/* top level input file */
char basebuf[BUFSIZ];		/* buffer for top level input file */
struct parsefile *parsefile = &basepf;	/* current input file */
char *pushedstring;		/* copy of parsenextc when text pushed back */
int pushednleft;		/* copy of parsenleft when text pushed back */
int whichprompt;		/* 1 == PS1, 2 == PS2 */

STATIC void pushfile __P((void));

#ifdef mkinit
INCLUDE "input.h"
INCLUDE "error.h"

INIT {
	extern char basebuf[];

	basepf.nextc = basepf.buf = basebuf;
}

RESET {
	if (exception != EXSHELLPROC)
		parsenleft = 0;            /* clear input buffer */
	popallfiles();
}

SHELLPROC {
	popallfiles();
}
#endif


/*
 * Read a line from the script.
 */

char *
pfgets(line, len)
	char *line;
	int len;
{
	register char *p = line;
	int nleft = len;
	int c;

	while (--nleft > 0) {
		c = pgetc_macro();
		if (c == PEOF) {
			if (p == line)
				return NULL;
			break;
		}
		*p++ = c;
		if (c == '\n')
			break;
	}
	*p = '\0';
	return line;
}



/*
 * Read a character from the script, returning PEOF on end of file.
 * Nul characters in the input are silently discarded.
 */

int
pgetc() {
	return pgetc_macro();
}


/*
 * Refill the input buffer and return the next input character:
 *
 * 1) If a string was pushed back on the input, pop it;
 * 2) If an EOF was pushed back (parsenleft == EOF_NLEFT) or we are reading
 *    from a string so we can't refill the buffer, return EOF.
 * 3) Call read to read in the characters.
 * 4) Delete all nul characters from the buffer.
 */

int
preadbuffer() {
	register char *p, *q;
	register int i;

	if (parsefile->strpush) {
		popstring();
		if (--parsenleft >= 0)
			return (*parsenextc++);
	}
	if (parsenleft == EOF_NLEFT || parsefile->buf == NULL)
		return PEOF;
	flushout(&output);
	flushout(&errout);
retry:
	p = parsenextc = parsefile->buf;
#ifndef NO_HISTORY
	if (parsefile->fd == 0 && lineread_enabled) {
		const char *rl_cp;
		int len;

		lineread_set_allow_complete(whichprompt == 1 ? 1 : 0);
		rl_cp = lineread(whichprompt == 1 ? getprompt(NULL) : getprompt(NULL));
		if (rl_cp == NULL) {
			i = 0;
			goto eof;
		}
		{
			static char he_buf[BUFSIZ];
			const char *src = rl_cp;
			int hlen = lineread_hist_len();

			/* lineread() already pushed the raw line; exclude it */
			if (hlen > 0) hlen--;
			int expanded = 0;
			int si, di;

			di = 0;
			he_buf[0] = '\0';

			for (si = 0; src[si] != '\0' && di < BUFSIZ - 2; ) {
				char qch = 0;

				if (src[si] == '\'' || src[si] == '"') {
					qch = src[si];
					he_buf[di++] = src[si++];
					while (src[si] && di < BUFSIZ - 2) {
						he_buf[di++] = src[si];
						if (src[si] == qch) { si++; break; }
						si++;
					}
					continue;
				}

				if (src[si] == '\\' && src[si+1] != '\0') {
					he_buf[di++] = src[si++];
					he_buf[di++] = src[si++];
					continue;
				}

				if (src[si] == '!'
				    && src[si+1] != '\0'
				    && src[si+1] != '='
				    && src[si+1] != '(') {
					const char *entry = NULL;
					const char *after = NULL;
					const char *sp = src + si + 1;
					static char he_wdtmp[BUFSIZ];
					char he_wdsel[64];
					int  he_have_wdsel = 0;

					if (sp[0] == '#') {
						/* !# expands to the current typed line so far */
						he_buf[di] = '\0';
						if (di + di < BUFSIZ - 2) {
							memmove(he_buf + di, he_buf, (size_t)di);
							di += di;
						}
						si = (int)(sp + 1 - src);
						expanded = 1;
						continue;
					} else if (sp[0] == '?') {
						/* !?string? — substring search in any position */
						const char *qstart = sp + 1;
						const char *qend = strchr(qstart, '?');
						char needle[256];
						int nl;
						if (qend == NULL) {
							qend = qstart + strlen(qstart);
							after = qend;
						} else {
							after = qend + 1;
						}
						nl = (int)(qend - qstart);
						if (nl >= (int)sizeof needle) nl = (int)sizeof needle - 1;
						memcpy(needle, qstart, (size_t)nl);
						needle[nl] = '\0';
						if (nl > 0) {
							int j;
							for (j = hlen - 1; j >= 0; j--) {
								const char *h = lineread_hist_entry(j);
								if (h && strstr(h, needle)) {
									entry = h;
									break;
								}
							}
						}
					} else if (sp[0] == '!') {
						if (hlen > 0)
							entry = lineread_hist_entry(hlen - 1);
						after = sp + 1;
					} else if (sp[0] >= '0' && sp[0] <= '9') {
						int idx = atoi(sp) - 1;
						while (*sp >= '0' && *sp <= '9') sp++;
						after = sp;
						if (idx >= 0 && idx < hlen)
							entry = lineread_hist_entry(idx);
					} else if (sp[0] == '-' && sp[1] >= '1' && sp[1] <= '9') {
						int rel = atoi(sp + 1);
						int idx = hlen - rel;
						sp++;
						while (*sp >= '0' && *sp <= '9') sp++;
						after = sp;
						if (idx >= 0 && idx < hlen)
							entry = lineread_hist_entry(idx);
					} else if (sp[0] == '$' || sp[0] == '*' || sp[0] == '^') {
						/* !$  !*  !^  — word designator from prev cmd */
						if (hlen > 0)
							entry = lineread_hist_entry(hlen - 1);
						he_wdsel[0] = sp[0]; he_wdsel[1] = '\0';
						he_have_wdsel = 1;
						after = sp + 1;
					} else {
						const char *ep = sp;
						int nlen;
						while (*ep && *ep != ' ' && *ep != '\t'
						       && *ep != ';' && *ep != '|'
						       && *ep != '&' && *ep != ':') ep++;
						nlen = (int)(ep - sp);
						after = ep;
						if (nlen > 0) {
							int j;
							for (j = hlen - 1; j >= 0; j--) {
								const char *h = lineread_hist_entry(j);
								if (h && strncmp(h, sp, (size_t)nlen) == 0
								    && (h[nlen] == '\0' || h[nlen] == ' ')) {
									entry = h;
									break;
								}
							}
						}
					}

					/* word designator after ':' (e.g. !!:2 !!:1-3 !!:$ !!:^ !!:*) */
					if (entry && !he_have_wdsel && after && after[0] == ':') {
						const char *wd = after + 1;
						int wdlen = 0;
						while (wd[wdlen] && wd[wdlen] != ' '
						       && wd[wdlen] != '\t'
						       && wd[wdlen] != ';'
						       && wd[wdlen] != '|'
						       && wd[wdlen] != '&') wdlen++;
						if (wdlen > 0 && wdlen < (int)sizeof he_wdsel - 1) {
							memcpy(he_wdsel, wd, (size_t)wdlen);
							he_wdsel[wdlen] = '\0';
							he_have_wdsel = 1;
							after = wd + wdlen;
						}
					}

					/* apply word designator */
					if (entry && he_have_wdsel) {
						char he_ecopy[BUFSIZ];
						char *he_words[256];
						int  he_nwords = 0;
						char *he_wp;
						strncpy(he_ecopy, entry, sizeof he_ecopy - 1);
						he_ecopy[sizeof he_ecopy - 1] = '\0';
						he_wp = he_ecopy;
						while (*he_wp && he_nwords < 255) {
							while (*he_wp == ' ' || *he_wp == '\t') he_wp++;
							if (!*he_wp) break;
							he_words[he_nwords++] = he_wp;
							while (*he_wp && *he_wp != ' ' && *he_wp != '\t') he_wp++;
							if (*he_wp) *he_wp++ = '\0';
						}
						he_wdtmp[0] = '\0';
						if (he_wdsel[0] == '$') {
							if (he_nwords > 1)
								strncat(he_wdtmp, he_words[he_nwords-1], sizeof he_wdtmp - 1);
							else if (he_nwords == 1)
								strncat(he_wdtmp, he_words[0], sizeof he_wdtmp - 1);
						} else if (he_wdsel[0] == '^') {
							if (he_nwords > 1)
								strncat(he_wdtmp, he_words[1], sizeof he_wdtmp - 1);
							else if (he_nwords == 1)
								strncat(he_wdtmp, he_words[0], sizeof he_wdtmp - 1);
						} else if (he_wdsel[0] == '*') {
							int he_wi;
							int he_start = he_nwords > 1 ? 1 : 0;
							for (he_wi = he_start; he_wi < he_nwords; he_wi++) {
								if (he_wi > he_start)
									strncat(he_wdtmp, " ", sizeof he_wdtmp - 1);
								strncat(he_wdtmp, he_words[he_wi], sizeof he_wdtmp - 1);
							}
						} else {
							/* numeric: n  or  m-n */
							int he_m, he_n, he_wi;
							char *he_dash = strchr(he_wdsel, '-');
							he_m = atoi(he_wdsel);
							he_n = he_dash ? atoi(he_dash + 1) : he_m;
							if (he_m < 0) he_m = 0;
							if (he_n >= he_nwords) he_n = he_nwords - 1;
							for (he_wi = he_m; he_wi <= he_n && he_wi < he_nwords; he_wi++) {
								if (he_wi > he_m)
									strncat(he_wdtmp, " ", sizeof he_wdtmp - 1);
								strncat(he_wdtmp, he_words[he_wi], sizeof he_wdtmp - 1);
							}
						}
						entry = he_wdtmp;
					}

					if (entry && after) {
						int elen = (int)strlen(entry);
						if (di + elen < BUFSIZ - 2) {
							memcpy(he_buf + di, entry, (size_t)elen);
							di += elen;
						}
						si = (int)(after - src);
						expanded = 1;
						continue;
					}
				}

				he_buf[di++] = src[si++];
			}
			he_buf[di] = '\0';

			if (expanded) {
				out2str(he_buf);
				out2str("\n");
				flushout(&errout);
				len = di;
				if (len >= BUFSIZ - 2) len = BUFSIZ - 3;
				memcpy(p, he_buf, (size_t)len);
				p[len]     = '\n';
				p[len + 1] = '\0';
				i = len + 1;
				lineread_hist_pop();
				lineread_hist_push(he_buf);
			} else {
				strncpy(p, src, BUFSIZ - 2);
				p[BUFSIZ - 2] = '\0';
				len = (int)strlen(p);
				p[len]     = '\n';
				p[len + 1] = '\0';
				i = len + 1;
			}
		}
	} else {
		i = read(parsefile->fd, p, BUFSIZ - 1);
	}
eof:
#else
	i = read(parsefile->fd, p, BUFSIZ - 1);
#endif
	if (i <= 0) {
                if (i < 0) {
                        if (errno == EINTR)
                                goto retry;
                        if (parsefile->fd == 0 && errno == EWOULDBLOCK) {
                                int flags = fcntl(0, F_GETFL, 0);
                                if (flags >= 0 && flags & O_NONBLOCK) {
                                        flags &=~ O_NONBLOCK;
                                        if (fcntl(0, F_SETFL, flags) >= 0) {
						out2str("sh: turning off NDELAY mode\n");
                                                goto retry;
                                        }
                                }
                        }
                }
                parsenleft = EOF_NLEFT;
                return PEOF;
	}
	parsenleft = i - 1;	/* we're returning one char in this call */

	/* delete nul characters */
	for (;;) {
		if (*p == '\0')
			break;
		p++;
		if (--i <= 0) {
			*p = '\0';
			goto done;		/* no nul characters */
		}
	}
	/*
	 * remove nuls
	 */
	q = p++;
	while (--i > 0) {
		if (*p != '\0')
			*q++ = *p;
		p++;
	}
	*q = '\0';
	if (q == parsefile->buf)
		goto retry;			/* buffer contained nothing but nuls */
	parsenleft = q - parsefile->buf - 1;

done:
	if (vflag) {
		/*
		 * This isn't right.  Most shells coordinate it with
		 * reading a line at a time.  I honestly don't know if its
		 * worth it.
		 */
		i = parsenleft + 1;
		p = parsefile->buf;
		for (; i--; p++) 
			out2c(*p)
		flushout(out2);
	}
	return *parsenextc++;
}

/*
 * Undo the last call to pgetc.  Only one character may be pushed back.
 * PEOF may be pushed back.
 */

void
pungetc() {
	parsenleft++;
	parsenextc--;
}

/*
 * Push a string back onto the input at this current parsefile level.
 * We handle aliases this way.
 */
void
pushstring(s, len, ap)
	char *s;
	int len;
	void *ap;
	{
	struct strpush *sp;

	INTOFF;
/*dprintf("*** calling pushstring: %s, %d\n", s, len);*/
	if (parsefile->strpush) {
		sp = ckmalloc(sizeof (struct strpush));
		sp->prev = parsefile->strpush;
		parsefile->strpush = sp;
	} else
		sp = parsefile->strpush = &(parsefile->basestrpush);
	sp->prevstring = parsenextc;
	sp->prevnleft = parsenleft;
	sp->ap = (struct alias *)ap;
	if (ap)
		((struct alias *)ap)->flag |= ALIASINUSE;
	parsenextc = s;
	parsenleft = len;
	INTON;
}

void
popstring()
{
	struct strpush *sp = parsefile->strpush;

	INTOFF;
	parsenextc = sp->prevstring;
	parsenleft = sp->prevnleft;
/*dprintf("*** calling popstring: restoring to '%s'\n", parsenextc);*/
	if (sp->ap)
		sp->ap->flag &= ~ALIASINUSE;
	parsefile->strpush = sp->prev;
	if (sp != &(parsefile->basestrpush))
		ckfree(sp);
	INTON;
}

/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

void
setinputfile(fname, push)
	char *fname;
	int push;
{
	int fd;
	int fd2;

	INTOFF;
	if ((fd = open(fname, O_RDONLY)) < 0)
		error("Can't open %s", fname);
	if (fd < 10) {
		fd2 = copyfd(fd, 10);
		close(fd);
		if (fd2 < 0)
			error("Out of file descriptors");
		fd = fd2;
	}
	setinputfd(fd, push);
	INTON;
}


/*
 * Like setinputfile, but takes an open file descriptor.  Call this with
 * interrupts off.
 */

void
setinputfd(fd, push)
	int fd, push;
{
	if (push) {
		pushfile();
		parsefile->buf = ckmalloc(BUFSIZ);
	}
	if (parsefile->fd > 0)
		close(parsefile->fd);
	parsefile->fd = fd;
	if (parsefile->buf == NULL)
		parsefile->buf = ckmalloc(BUFSIZ);
	parsenleft = 0;
	plinno = 1;
}


/*
 * Like setinputfile, but takes input from a string.
 */

void
setinputstring(string, push)
	char *string;
	int push;
	{
	INTOFF;
	if (push)
		pushfile();
	parsenextc = string;
	parsenleft = strlen(string);
	parsefile->buf = NULL;
	plinno = 1;
	INTON;
}



/*
 * To handle the "." command, a stack of input files is used.  Pushfile
 * adds a new entry to the stack and popfile restores the previous level.
 */

STATIC void
pushfile() {
	struct parsefile *pf;

	parsefile->nleft = parsenleft;
	parsefile->nextc = parsenextc;
	parsefile->linno = plinno;
	pf = (struct parsefile *)ckmalloc(sizeof (struct parsefile));
	pf->prev = parsefile;
	pf->fd = -1;
	pf->strpush = NULL;
	pf->basestrpush.prev = NULL;
	parsefile = pf;
}


void
popfile() {
	struct parsefile *pf = parsefile;

	INTOFF;
	if (pf->fd >= 0)
		close(pf->fd);
	if (pf->buf)
		ckfree(pf->buf);
	while (pf->strpush)
		popstring();
	parsefile = pf->prev;
	ckfree(pf);
	parsenleft = parsefile->nleft;
	parsenextc = parsefile->nextc;
	plinno = parsefile->linno;
	INTON;
}


/*
 * Return to top level.
 */

void
popallfiles() {
	while (parsefile != &basepf)
		popfile();
}



/*
 * Close the file(s) that the shell is reading commands from.  Called
 * after a fork is done.
 */

void
closescript() {
	popallfiles();
	if (parsefile->fd > 0) {
		close(parsefile->fd);
		parsefile->fd = 0;
	}
}
