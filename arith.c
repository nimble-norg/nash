/*-
 * Copyright (c) 1993
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
 *
 *	@(#)arith.y	8.3 (Berkeley) 5/4/95
 */

#include <stdlib.h>
#include <ctype.h>

#include "shell.h"
#include "arith.h"
#include "error.h"
#include "output.h"
#include "memalloc.h"
#include "expand.h"
#include "var.h"

char *arith_buf, *arith_startbuf;

/*
 * Recursive-descent parser replacing the original yacc/lex grammar.
 * Operator precedence (low to high):
 *   ||  &&  |  ^  &  == !=  < > <= >=  << >>  + -  * / %  unary
 */

static long parse_or(void);
static long parse_and(void);
static long parse_bor(void);
static long parse_bxor(void);
static long parse_band(void);
static long parse_eq(void);
static long parse_cmp(void);
static long parse_shift(void);
static long parse_add(void);
static long parse_mul(void);
static long parse_unary(void);
static long parse_primary(void);

static void
skipws(void)
{
	while (*arith_buf == ' ' || *arith_buf == '\t' || *arith_buf == '\n')
		arith_buf++;
}

static long
parse_primary(void)
{
	long val;

	skipws();
	if (*arith_buf == '(') {
		arith_buf++;
		val = parse_or();
		skipws();
		if (*arith_buf != ')')
			error("arithmetic expression: missing ')': \"%s\"",
			    arith_startbuf);
		arith_buf++;
		return val;
	}
	if (isdigit((unsigned char)*arith_buf)) {
		char *end;
		val = strtol(arith_buf, &end, 0);
		arith_buf = end;
		return val;
	}
	/* bare variable name: look up in shell variables */
	if (isalpha((unsigned char)*arith_buf) || *arith_buf == '_') {
		char varname[256];
		int  vlen = 0;
		char *varval;
		while ((isalnum((unsigned char)*arith_buf) || *arith_buf == '_')
		       && vlen < (int)sizeof(varname) - 1)
			varname[vlen++] = *arith_buf++;
		varname[vlen] = '\0';
		varval = lookupvar(varname);
		if (varval == NULL || *varval == '\0')
			return 0;
		return strtol(varval, NULL, 0);
	}
	error("arithmetic expression: syntax error: \"%s\"", arith_startbuf);
	return 0;
}

static long
parse_unary(void)
{
	skipws();
	if (*arith_buf == '!') {
		arith_buf++;
		return !parse_unary();
	}
	if (*arith_buf == '~') {
		arith_buf++;
		return ~parse_unary();
	}
	if (*arith_buf == '-' && *(arith_buf+1) != '-') {
		arith_buf++;
		return -parse_unary();
	}
	if (*arith_buf == '+' && *(arith_buf+1) != '+') {
		arith_buf++;
		return parse_unary();
	}
	return parse_primary();
}

static long
parse_mul(void)
{
	long left = parse_unary();
	for (;;) {
		skipws();
		if (*arith_buf == '*') {
			arith_buf++;
			left *= parse_unary();
		} else if (*arith_buf == '/') {
			arith_buf++;
			long r = parse_unary();
			if (r == 0)
				error("arithmetic expression: division by zero: \"%s\"",
				    arith_startbuf);
			left /= r;
		} else if (*arith_buf == '%') {
			arith_buf++;
			long r = parse_unary();
			if (r == 0)
				error("arithmetic expression: division by zero: \"%s\"",
				    arith_startbuf);
			left %= r;
		} else
			break;
	}
	return left;
}

static long
parse_add(void)
{
	long left = parse_mul();
	for (;;) {
		skipws();
		if (*arith_buf == '+' && *(arith_buf+1) != '+') {
			arith_buf++;
			left += parse_mul();
		} else if (*arith_buf == '-' && *(arith_buf+1) != '-') {
			arith_buf++;
			left -= parse_mul();
		} else
			break;
	}
	return left;
}

static long
parse_shift(void)
{
	long left = parse_add();
	for (;;) {
		skipws();
		if (arith_buf[0] == '<' && arith_buf[1] == '<') {
			arith_buf += 2;
			left <<= parse_add();
		} else if (arith_buf[0] == '>' && arith_buf[1] == '>') {
			arith_buf += 2;
			left >>= parse_add();
		} else
			break;
	}
	return left;
}

static long
parse_cmp(void)
{
	long left = parse_shift();
	for (;;) {
		skipws();
		if (arith_buf[0] == '<' && arith_buf[1] == '=') {
			arith_buf += 2; left = left <= parse_shift();
		} else if (arith_buf[0] == '>' && arith_buf[1] == '=') {
			arith_buf += 2; left = left >= parse_shift();
		} else if (arith_buf[0] == '<' && arith_buf[1] != '<') {
			arith_buf++; left = left < parse_shift();
		} else if (arith_buf[0] == '>' && arith_buf[1] != '>') {
			arith_buf++; left = left > parse_shift();
		} else
			break;
	}
	return left;
}

static long
parse_eq(void)
{
	long left = parse_cmp();
	for (;;) {
		skipws();
		if (arith_buf[0] == '=' && arith_buf[1] == '=') {
			arith_buf += 2; left = left == parse_cmp();
		} else if (arith_buf[0] == '!' && arith_buf[1] == '=') {
			arith_buf += 2; left = left != parse_cmp();
		} else
			break;
	}
	return left;
}

static long
parse_band(void)
{
	long left = parse_eq();
	for (;;) {
		skipws();
		if (arith_buf[0] == '&' && arith_buf[1] != '&') {
			arith_buf++;
			left &= parse_eq();
		} else
			break;
	}
	return left;
}

static long
parse_bxor(void)
{
	long left = parse_band();
	for (;;) {
		skipws();
		if (*arith_buf == '^') {
			arith_buf++;
			left ^= parse_band();
		} else
			break;
	}
	return left;
}

static long
parse_bor(void)
{
	long left = parse_bxor();
	for (;;) {
		skipws();
		if (arith_buf[0] == '|' && arith_buf[1] != '|') {
			arith_buf++;
			left |= parse_bxor();
		} else
			break;
	}
	return left;
}

static long
parse_and(void)
{
	long left = parse_bor();
	for (;;) {
		skipws();
		if (arith_buf[0] == '&' && arith_buf[1] == '&') {
			arith_buf += 2;
			long r = parse_bor();
			left = left ? (r ? r : 0) : 0;
		} else
			break;
	}
	return left;
}

static long
parse_or(void)
{
	long left = parse_and();
	for (;;) {
		skipws();
		if (arith_buf[0] == '|' && arith_buf[1] == '|') {
			arith_buf += 2;
			long r = parse_and();
			left = left ? left : (r ? r : 0);
		} else
			break;
	}
	return left;
}

int
arith(s)
	char *s;
{
	long result;

	arith_buf = arith_startbuf = s;

	INTOFF;
	result = parse_or();
	skipws();
	if (*arith_buf != '\0')
		error("arithmetic expression: syntax error: \"%s\"",
		    arith_startbuf);
	INTON;

	return (int)result;
}

void
arith_lex_reset()
{
	/* nothing to reset in the recursive-descent implementation */
}

/*
 *  The exp(1) builtin.
 */
int
expcmd(argc, argv)
	int argc;
	char **argv;
{
	char *p;
	char *concat;
	char **ap;
	long i;

	if (argc > 1) {
		p = argv[1];
		if (argc > 2) {
			/*
			 * concatenate arguments
			 */
			STARTSTACKSTR(concat);
			ap = argv + 2;
			for (;;) {
				while (*p)
					STPUTC(*p++, concat);
				if ((p = *ap++) == NULL)
					break;
				STPUTC(' ', concat);
			}
			STPUTC('\0', concat);
			p = grabstackstr(concat);
		}
	} else
		p = "";

	i = arith(p);

	out1fmt("%d\n", (int)i);
	return (! i);
}
