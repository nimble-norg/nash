CC	= gcc
CFLAGS	= -std=c99 -pedantic -Wall -Wno-implicit-function-declaration \
	  -Wno-char-subscripts \
	  -fcommon \
	  -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
	  -DSHELL -DLINUX -I. -Dlint
LDFLAGS	=
LDADD	=

PROG	= ash

SRCS	= alias.c cd.c error.c eval.c exec.c expand.c \
	  input.c jobs.c mail.c main.c memalloc.c miscbltin.c \
	  mystring.c options.c parser.c redir.c show.c trap.c \
	  output.c var.c arith.c setmode.c lineread.c histedit.c

GENSRCS	= builtins.c nodes.c syntax.c init.c

GENHDRS	= token.def builtins.h nodes.h syntax.h

OBJS	= $(SRCS:.c=.o) $(GENSRCS:.c=.o) bltin/echo.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDADD)

mkinit: mkinit.c
	$(CC) $(CFLAGS) mkinit.c -o $@

mknodes: mknodes.c
	$(CC) $(CFLAGS) mknodes.c -o $@

mksyntax: mksyntax.c parser.h
	$(CC) $(CFLAGS) mksyntax.c -o $@

token.def: mktokens
	sh mktokens

builtins.h builtins.c: mkbuiltins builtins.def
	sh mkbuiltins .

nodes.h nodes.c: mknodes nodetypes nodes.c.pat
	./mknodes nodetypes nodes.c.pat

syntax.h syntax.c: mksyntax parser.h
	./mksyntax

init.c: mkinit $(SRCS) builtins.c nodes.c syntax.c
	./mkinit '$(CC) $(CFLAGS) -c init.c' \
	  $(SRCS) builtins.c nodes.c syntax.c

$(SRCS:.c=.o) builtins.o nodes.o syntax.o init.o bltin/echo.o: $(GENHDRS)

bltin/echo.o: bltin/echo.c bltin/bltin.h
	$(CC) $(CFLAGS) -Ibltin -c bltin/echo.c -o bltin/echo.o

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(PROG) $(OBJS) \
	  mkinit mknodes mksyntax \
	  builtins.c builtins.h \
	  init.c \
	  nodes.c nodes.h \
	  syntax.c syntax.h \
	  token.def
