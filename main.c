/*
 * Copyright (C) 1989 by Kenneth Almquist.  All rights reserved.
 * This file is part of ash, which is distributed under the terms specified
 * by the Ash General Public License.  See the file named LICENSE.
 */

#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "shell.h"
#include "main.h"
#include "mail.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
#include "eval.h"
#include "jobs.h"
#include "input.h"
#include "trap.h"
#if ATTY
#include "var.h"
#endif
#include "memalloc.h"
#include "error.h"
#include "init.h"
#include "mystring.h"
#include "exec.h"

#define PROFILE 0

const char copyright[] = "@(#)Copyright 1989 by Kenneth Almquist";
int rootpid;
int rootshell;
static union node *curcmd;
static union node *prevcmd;

#if PROFILE
short profile_buf[16384];
extern int etext();
#endif

static void read_profile(const char *);

int main(int argc, char **argv) {
      struct jmploc jmploc;
      struct stackmark smark;
      volatile int state;
      char *shinit;

#if PROFILE
      monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif
      state = 0;
      if (setjmp(jmploc.loc)) {
	    if (exception == EXSHELLPROC) {
		  rootpid = getpid();
		  rootshell = 1;
		  minusc = NULL;
		  state = 3;
	    } else if (state == 0 || iflag == 0 || !rootshell) {
		  exitshell(2);
	    }
	    reset();
#if ATTY
	    if (exception == EXINT && (!attyset() || equal(termval(), "emacs"))) {
#else
	    if (exception == EXINT) {
#endif
		  out2c('\n');
		  flushout(&errout);
	    }
	    popstackmark(&smark);
	    FORCEINTON;
	    
	    if (state == 1)
		  goto state1;
	    else if (state == 2)
		  goto state2;
	    else
		  goto state3;
      }
      
      handler = &jmploc;

#ifdef DEBUG
      opentrace();
      trputs("Shell args:  ");  
      trargs(argv);
#endif

      rootpid = getpid();
      rootshell = 1;
      init();
      setstackmark(&smark);
      procargs(argc, argv);
      
      if (argv[0] && argv[0][0] == '-') {
	    state = 1;
	    read_profile("/etc/profile");
state1:
	    state = 2;
	    read_profile(".profile");
      } else if ((sflag || minusc) && (shinit = getenv("SHINIT")) != NULL) {
	    state = 2;
	    evalstring(shinit);
      }
state2:
      state = 3;
      if (minusc) {
	    evalstring(minusc);
      }
      if (sflag || minusc == NULL) {
state3:
	    cmdloop(1);
      }
#if PROFILE
      monitor(0);
#endif
      exitshell(exitstatus);
      return 0;
}

void cmdloop(int top) {
      union node *n;
      struct stackmark smark;
      int inter;
      int numeof;

      TRACE(("cmdloop(%d) called\n", top));
      setstackmark(&smark);
      numeof = 0;
      for (;;) {
	    if (sigpending)
		  dotrap();
	    inter = 0;
	    if (iflag && top) {
		  inter++;
		  showjobs(1);
		  chkmail(0);
		  flushout(&output);
	    }
	    n = parsecmd(inter);
	    if (n == NEOF) {
		  if (Iflag == 0 || numeof >= 50)
			break;
		  out2str("\nUse \"exit\" to leave ash.\n");
		  numeof++;
	    } else if (n != NULL && nflag == 0) {
		  if (inter) {
			INTOFF;
			if (prevcmd)
			      freefunc(prevcmd);
			prevcmd = curcmd;
			curcmd = copyfunc(n);
			INTON;
		  }
		  evaltree(n, 0);
	    }
	    popstackmark(&smark);
      }
      popstackmark(&smark);
}

static void read_profile(const char *name) {
      int fd;

      INTOFF;
      if ((fd = open(name, O_RDONLY)) >= 0)
	    setinputfd(fd, 1);
      INTON;
      if (fd < 0)
	    return;
      cmdloop(0);
      popfile();
}

void readcmdfile(const char *name) {
      int fd;

      INTOFF;
      if ((fd = open(name, O_RDONLY)) >= 0)
	    setinputfd(fd, 1);
      else
	    error("Can't open %s", name);
      INTON;
      cmdloop(0);
      popfile();
}

int dotcmd(int argc, char **argv) {
      exitstatus = 0;
      if (argc >= 2) {
	    setinputfile(argv[1], 1);
	    commandname = argv[1];
	    cmdloop(0);
	    popfile();
      }
      return exitstatus;
}

int exitcmd(int argc, char **argv) {
      if (argc > 1)
	    exitstatus = number(argv[1]);
      exitshell(exitstatus);
      return exitstatus;
}

int lccmd(int argc, char **argv) {
      if (argc > 1) {
	    defun(argv[1], prevcmd);
	    return 0;
      } else {
	    INTOFF;
	    freefunc(curcmd);
	    curcmd = prevcmd;
	    prevcmd = NULL;
	    INTON;
	    evaltree(curcmd, 0);
	    return exitstatus;
      }
}
