/*
 * Evaluate a command.
 *
 * Copyright (C) 1989 by Kenneth Almquist.  All rights reserved.
 * This file is part of ash, which is distributed under the terms specified
 * by the Ash General Public License.  See the file named LICENSE.
 */

#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "shell.h"
#include "nodes.h"
#include "syntax.h"
#include "expand.h"
#include "parser.h"
#include "jobs.h"
#include "eval.h"
#include "builtins.h"
#include "options.h"
#include "exec.h"
#include "redir.h"
#include "input.h"
#include "output.h"
#include "trap.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"

/* flags in argument to evaltree */
#define EV_EXIT 01		/* exit after evaluating tree */
#define EV_TESTED 02		/* exit status is checked; ignore -e flag */
#define EV_BACKCMD 04		/* command executing within back quotes */

/* reasons for skipping commands (see comment on breakcmd routine) */
#define SKIPBREAK 1
#define SKIPCONT 2
#define SKIPFUNC 3

MKINIT int evalskip;		/* set if we are skipping commands */
static int skipcount;		/* number of levels to skip */
MKINIT int loopnest;		/* current loop nesting level */
int funcnest;			/* depth of function calls */

char *commandname;
struct strlist *cmdenviron;
int exitstatus;			/* exit status of last command */

extern int pendingsig;          /* Renomeado para evitar conflito com POSIX sigpending() */
extern int exception;
extern struct jmploc *handler;

static void evalloop(union node *);
static void evalfor(union node *);
static void evalcase(union node *, int);
static void evalsubshell(union node *, int);
static void expredir(union node *);
static void evalpipe(union node *);
static void evalcommand(union node *, int, struct backcmd *);
static void prehash(union node *);

/*
 * Called to reset things after an exception.
 */

#ifdef mkinit
INCLUDE "eval.h"

RESET {
      evalskip = 0;
      loopnest = 0;
      funcnest = 0;
}

SHELLPROC {
      exitstatus = 0;
}
#endif

/*
 * The eval builtin.
 */

void trargs(char **ap);
void trputs(char *s);

#ifdef ELIGANT
int evalcmd(int argc, char **argv) {
      char **ap;
      for (ap = argv + 1; *ap; ap++) {
	    evalstring(*ap);
      }
      return exitstatus;
}
#else
int evalcmd(int argc, char **argv) {
      if (argc > 1) {
	    char *p = argv[1];
	    if (argc > 2) {
		  char *concat;
		  char **ap;
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
	    evalstring(p);
      }
      return exitstatus;
}
#endif

/*
 * Execute a command or commands contained in a string.
 */

void evalstring(char *s) {
      union node *n;
      struct stackmark smark;

      setstackmark(&smark);
      setinputstring(s, 1);
      while ((n = parsecmd(0)) != NEOF) {
	    evaltree(n, 0);
	    popstackmark(&smark);
      }
      popfile();
      popstackmark(&smark);
}

/*
 * Evaluate a parse tree.  The value is left in the global variable
 * exitstatus.
 */

void evaltree(union node *n, int flags) {
      if (n == NULL) {
	    TRACE(("evaltree(NULL) called\n"));
	    return;
      }
      TRACE(("evaltree(0x%x: %d) called\n", (int)n, n->type));
      
      switch (n->type) {
      case NSEMI:
	    evaltree(n->nbinary.ch1, 0);
	    if (!evalskip)
	          evaltree(n->nbinary.ch2, flags);
	    break;
      case NAND:
	    evaltree(n->nbinary.ch1, EV_TESTED);
	    if (!evalskip && exitstatus == 0)
	          evaltree(n->nbinary.ch2, flags);
	    break;
      case NOR:
	    evaltree(n->nbinary.ch1, EV_TESTED);
	    if (!evalskip && exitstatus != 0)
	          evaltree(n->nbinary.ch2, flags);
	    break;
      case NREDIR:
	    expredir(n->nredir.redirect);
	    redirect(n->nredir.redirect, REDIR_PUSH);
	    evaltree(n->nredir.n, flags);
	    popredir();
	    break;
      case NSUBSHELL:
      case NBACKGND:
	    evalsubshell(n, flags);
	    break;
      case NIF:
	    evaltree(n->nif.test, EV_TESTED);
	    if (!evalskip) {
		  if (exitstatus == 0) {
			evaltree(n->nif.ifpart, flags);
		  } else if (n->nif.elsepart) {
			evaltree(n->nif.elsepart, flags);
		  }
	    }
	    break;
      case NWHILE:
      case NUNTIL:
	    evalloop(n);
	    break;
      case NFOR:
	    evalfor(n);
	    break;
      case NCASE:
	    evalcase(n, flags);
	    break;
      case NDEFUN:
	    defun(n->narg.text, n->narg.next);
	    exitstatus = 0;
	    break;
      case NPIPE:
	    evalpipe(n);
	    break;
      case NCMD:
	    evalcommand(n, flags, NULL);
	    break;
      default:
	    out1fmt("Node type = %d\n", n->type);
	    flushout(&output);
	    break;
      }

      if (pendingsig)
	    dotrap();
      if ((flags & EV_EXIT) || (eflag && exitstatus && !(flags & EV_TESTED)))
	    exitshell(exitstatus);
}

static void evalloop(union node *n) {
      int status;

      loopnest++;
      status = 0;
      for (;;) {
	    evaltree(n->nbinary.ch1, EV_TESTED);
	    if (evalskip) {
		  if (evalskip == SKIPCONT && --skipcount <= 0) {
			evalskip = 0;
			continue;
		  }
		  if (evalskip == SKIPBREAK && --skipcount <= 0)
			evalskip = 0;
		  break;
	    }
	    if (n->type == NWHILE) {
		  if (exitstatus != 0)
			break;
	    } else {
		  if (exitstatus == 0)
			break;
	    }
	    evaltree(n->nbinary.ch2, 0);
	    status = exitstatus;
	    if (evalskip) {
		  if (evalskip == SKIPCONT && --skipcount <= 0) {
			evalskip = 0;
			continue;
		  }
		  if (evalskip == SKIPBREAK && --skipcount <= 0)
			evalskip = 0;
		  break;
	    }
      }
      loopnest--;
      exitstatus = status;
}

static void evalfor(union node *n) {
      struct arglist arglist;
      union node *argp;
      struct strlist *sp;
      struct stackmark smark;

      setstackmark(&smark);
      arglist.lastp = &arglist.list;
      for (argp = n->nfor.args; argp; argp = argp->narg.next) {
	    expandarg(argp, &arglist, 1);
	    if (evalskip) {
		  popstackmark(&smark);
		  return;
	    }
      }
      *arglist.lastp = NULL;

      exitstatus = 0;
      loopnest++;
      for (sp = arglist.list; sp; sp = sp->next) {
	    setvar(n->nfor.var, sp->text, 0);
	    evaltree(n->nfor.body, 0);
	    if (evalskip) {
		  if (evalskip == SKIPCONT && --skipcount <= 0) {
			evalskip = 0;
			continue;
		  }
		  if (evalskip == SKIPBREAK && --skipcount <= 0)
			evalskip = 0;
		  break;
	    }
      }
      loopnest--;
      popstackmark(&smark);
}

static void evalcase(union node *n, int flags) {
      union node *cp;
      union node *patp;
      struct arglist arglist;
      struct stackmark smark;

      setstackmark(&smark);
      arglist.lastp = &arglist.list;
      expandarg(n->ncase.expr, &arglist, 0);
      for (cp = n->ncase.cases; cp && evalskip == 0; cp = cp->nclist.next) {
	    for (patp = cp->nclist.pattern; patp; patp = patp->narg.next) {
		  if (casematch(patp, arglist.list->text)) {
			if (evalskip == 0) {
			      evaltree(cp->nclist.body, flags);
			}
			popstackmark(&smark);
			return;
		  }
	    }
      }
      popstackmark(&smark);
}

static void evalsubshell(union node *n, int flags) {
      struct job *jp;
      int backgnd = (n->type == NBACKGND);

      expredir(n->nredir.redirect);
      jp = makejob(n, 1);
      if (forkshell(jp, n, backgnd) == 0) {
	    if (backgnd)
		  flags &= ~EV_TESTED;
	    redirect(n->nredir.redirect, 0);
	    evaltree(n->nredir.n, flags | EV_EXIT);	/* never returns */
      }
      if (!backgnd) {
	    INTOFF;
	    exitstatus = waitforjob(jp);
	    INTON;
      }
}

static void expredir(union node *n) {
      union node *redir;

      for (redir = n; redir; redir = redir->nfile.next) {
	    if (redir->type == NFROM || redir->type == NTO || redir->type == NAPPEND) {
		  struct arglist fn;
		  fn.lastp = &fn.list;
		  expandarg(redir->nfile.fname, &fn, 0);
		  redir->nfile.expfname = fn.list->text;
	    }
      }
}

static void evalpipe(union node *n) {
      struct job *jp;
      struct nodelist *lp;
      int pipelen = 0;
      int prevfd;
      int pip[2];

      TRACE(("evalpipe(0x%x) called\n", (int)n));
      for (lp = n->npipe.cmdlist; lp; lp = lp->next)
	    pipelen++;
      
      INTOFF;
      jp = makejob(n, pipelen);
      prevfd = -1;
      
      for (lp = n->npipe.cmdlist; lp; lp = lp->next) {
	    prehash(lp->n);
	    pip[1] = -1;
	    if (lp->next) {
		  if (pipe(pip) < 0) {
			if (prevfd >= 0) close(prevfd);
			error("Pipe call failed");
		  }
	    }
	    if (forkshell(jp, lp->n, n->npipe.backgnd) == 0) {
		  INTON;
		  if (prevfd > 0) {
			close(0);
			copyfd(prevfd, 0);
			close(prevfd);
		  }
		  if (pip[1] >= 0) {
			close(pip[0]);
			if (pip[1] != 1) {
			      close(1);
			      copyfd(pip[1], 1);
			      close(pip[1]);
			}
		  }
		  evaltree(lp->n, EV_EXIT);
	    }
	    if (prevfd >= 0)
		  close(prevfd);
	    prevfd = pip[0];
	    if (pip[1] >= 0) close(pip[1]);
      }
      INTON;
      if (n->npipe.backgnd == 0) {
	    INTOFF;
	    exitstatus = waitforjob(jp);
	    TRACE(("evalpipe:  job done exit status %d\n", exitstatus));
	    INTON;
      }
}

void evalbackcmd(union node *n, struct backcmd *result) {
      int pip[2];
      struct job *jp;
      struct stackmark smark;

      setstackmark(&smark);
      result->fd = -1;
      result->buf = NULL;
      result->nleft = 0;
      result->jp = NULL;
      
      if (n->type == NCMD) {
	    evalcommand(n, EV_BACKCMD, result);
      } else {
	    if (pipe(pip) < 0)
		  error("Pipe call failed");
	    jp = makejob(n, 1);
	    if (forkshell(jp, n, FORK_NOJOB) == 0) {
		  FORCEINTON;
		  close(pip[0]);
		  if (pip[1] != 1) {
			close(1);
			copyfd(pip[1], 1);
			close(pip[1]);
		  }
		  evaltree(n, EV_EXIT);
	    }
	    close(pip[1]);
	    result->fd = pip[0];
	    result->jp = jp;
      }
      popstackmark(&smark);
      TRACE(("evalbackcmd done: fd=%d buf=0x%x nleft=%d jp=0x%x\n",
	    result->fd, result->buf, result->nleft, result->jp));
}

static void evalcommand(union node *cmd, int flags, struct backcmd *backcmd) {
      struct stackmark smark;
      union node *argp;
      struct arglist arglist;
      struct arglist varlist;
      char **argv;
      int argc;
      char **envp;
      int varflag;
      struct strlist *sp;
      char *p;
      int mode = 0;
      int pip[2];
      struct cmdentry cmdentry;
      struct job *jp;
      struct jmploc jmploc;
      struct jmploc *volatile savehandler;
      char *volatile savecmdname;
      volatile struct shparam saveparam;
      struct localvar *volatile savelocalvars;
      volatile int e;
      char *lastarg;

      TRACE(("evalcommand(0x%x, %d) called\n", (int)cmd, flags));
      setstackmark(&smark);
      arglist.lastp = &arglist.list;
      varlist.lastp = &varlist.list;
      varflag = 1;
      
      for (argp = cmd->ncmd.args; argp; argp = argp->narg.next) {
	    p = argp->narg.text;
	    if (varflag && is_name(*p)) {
		  do {
			p++;
		  } while (is_in_name(*p));
		  if (*p == '=') {
			expandarg(argp, &varlist, 0);
			continue;
		  }
	    }
	    expandarg(argp, &arglist, 1);
	    varflag = 0;
      }
      *arglist.lastp = NULL;
      *varlist.lastp = NULL;
      
      expredir(cmd->ncmd.redirect);
      
      argc = 0;
      for (sp = arglist.list; sp; sp = sp->next)
	    argc++;
      
      argv = stalloc(sizeof(char *) * (argc + 1));
      for (sp = arglist.list; sp; sp = sp->next)
	    *argv++ = sp->text;
      *argv = NULL;
      
      lastarg = NULL;
      if (iflag && funcnest == 0 && argc > 0)
	    lastarg = argv[-1];
      argv -= argc;

      if (xflag) {
	    outc('+', &errout);
	    for (sp = varlist.list; sp; sp = sp->next) {
		  outc(' ', &errout);
		  out2str(sp->text);
	    }
	    for (sp = arglist.list; sp; sp = sp->next) {
		  outc(' ', &errout);
		  out2str(sp->text);
	    }
	    outc('\n', &errout);
	    flushout(&errout);
      }

      if (argc == 0) {
	    cmdentry.cmdtype = CMDBUILTIN;
	    cmdentry.u.index = BLTINCMD;
      } else {
	    find_command(argv[0], &cmdentry, 1);
	    if (cmdentry.cmdtype == CMDUNKNOWN) {
		  exitstatus = 2;
		  flushout(&errout);
		  popstackmark(&smark);
		  return;
	    }
	    if (cmdentry.cmdtype == CMDBUILTIN && cmdentry.u.index == BLTINCMD) {
		  for (;;) {
			argv++;
			if (--argc == 0)
			      break;
			if ((cmdentry.u.index = find_builtin(*argv)) < 0) {
			      outfmt(&errout, "%s: not found\n", *argv);
			      exitstatus = 2;
			      flushout(&errout);
			      popstackmark(&smark);
			      return;
			}
			if (cmdentry.u.index != BLTINCMD)
			      break;
		  }
	    }
      }

      if (cmd->ncmd.backgnd || 
         (cmdentry.cmdtype == CMDNORMAL && (flags & EV_EXIT) == 0) || 
         ((flags & EV_BACKCMD) != 0 && (cmdentry.cmdtype != CMDBUILTIN || cmdentry.u.index == DOTCMD || cmdentry.u.index == EVALCMD))) {
	    
            jp = makejob(cmd, 1);
	    mode = cmd->ncmd.backgnd;
	    
            if (flags & EV_BACKCMD) {
		  mode = FORK_NOJOB;
		  if (pipe(pip) < 0)
			error("Pipe call failed");
	    }
            
	    if (forkshell(jp, cmd, mode) != 0) {
                  if (mode == 0) {
                        INTOFF;
                        exitstatus = waitforjob(jp);
                        INTON;
                  } else if (mode == 2) {
                        backcmd->fd = pip[0];
                        close(pip[1]);
                        backcmd->jp = jp;
                  }
                  if (lastarg)
                        setvar("_", lastarg, 0);
                  popstackmark(&smark);
                  return;
            }
            
	    if (flags & EV_BACKCMD) {
		  FORCEINTON;
		  close(pip[0]);
		  if (pip[1] != 1) {
			close(1);
			copyfd(pip[1], 1);
			close(pip[1]);
		  }
	    }
	    flags |= EV_EXIT;
      }

      if (cmdentry.cmdtype == CMDFUNCTION) {
	    trputs("Shell function:  ");  trargs(argv);
	    redirect(cmd->ncmd.redirect, REDIR_PUSH);
	    saveparam = shellparam;
	    shellparam.malloc = 0;
	    shellparam.nparam = argc - 1;
	    shellparam.p = argv + 1;
	    shellparam.optnext = NULL;
	    INTOFF;
	    savelocalvars = localvars;
	    localvars = NULL;
	    INTON;
	    if (setjmp(jmploc.loc)) {
		  if (exception == EXSHELLPROC)
			freeparam((struct shparam *)&saveparam);
		  else {
			freeparam(&shellparam);
			shellparam = saveparam;
		  }
		  poplocalvars();
		  localvars = savelocalvars;
		  handler = savehandler;
		  longjmp(handler->loc, 1);
	    }
	    savehandler = handler;
	    handler = &jmploc;
	    for (sp = varlist.list; sp; sp = sp->next)
		  mklocal(sp->text);
	    funcnest++;
	    evaltree(cmdentry.u.func, 0);
	    funcnest--;
	    INTOFF;
	    poplocalvars();
	    localvars = savelocalvars;
	    freeparam(&shellparam);
	    shellparam = saveparam;
	    handler = savehandler;
	    popredir();
	    INTON;
	    if (evalskip == SKIPFUNC) {
		  evalskip = 0;
		  skipcount = 0;
	    }
	    if (flags & EV_EXIT)
		  exitshell(exitstatus);
      } else if (cmdentry.cmdtype == CMDBUILTIN) {
	    trputs("builtin command:  ");  trargs(argv);
	    mode = (cmdentry.u.index == EXECCMD) ? 0 : REDIR_PUSH;
	    if (flags == EV_BACKCMD) {
		  memout.nleft = 0;
		  memout.nextc = memout.buf;
		  memout.bufsize = 64;
		  mode |= REDIR_BACKQ;
	    }
	    redirect(cmd->ncmd.redirect, mode);
	    savecmdname = commandname;
	    cmdenviron = varlist.list;
	    e = -1;
	    if (setjmp(jmploc.loc)) {
		  e = exception;
		  exitstatus = (e == EXINT) ? SIGINT + 128 : 2;
	    } else {
                  savehandler = handler;
                  handler = &jmploc;
                  commandname = argv[0];
                  argptr = argv + 1;
                  optptr = NULL;
                  exitstatus = (*builtinfunc[cmdentry.u.index])(argc, argv);
                  flushall();
            }

	    out1 = &output;
	    out2 = &errout;
	    freestdout();
	    if (e != EXSHELLPROC) {
		  commandname = savecmdname;
		  if (flags & EV_EXIT) {
			exitshell(exitstatus);
		  }
	    }
	    handler = savehandler;
	    if (e != -1) {
		  if (e != EXERROR || cmdentry.u.index == BLTINCMD || cmdentry.u.index == DOTCMD || cmdentry.u.index == EVALCMD || cmdentry.u.index == EXECCMD)
			raise(e);
		  FORCEINTON;
	    }
	    if (cmdentry.u.index != EXECCMD)
		  popredir();
	    if (flags == EV_BACKCMD) {
		  backcmd->buf = memout.buf;
		  backcmd->nleft = memout.nextc - memout.buf;
		  memout.buf = NULL;
	    }
      } else {
	    trputs("normal command:  ");  trargs(argv);
	    clearredir();
	    redirect(cmd->ncmd.redirect, 0);
	    if (varlist.list) {
		  p = stalloc(strlen(pathval()) + 1);
		  scopy(pathval(), p);
	    } else {
		  p = pathval();
	    }
	    for (sp = varlist.list; sp; sp = sp->next)
		  setvareq(sp->text, VEXPORT | VSTACK);
	    envp = environment();
	    shellexec(argv, envp, p, cmdentry.u.index);
      }

      if (lastarg)
	    setvar("_", lastarg, 0);
      popstackmark(&smark);
}

static void prehash(union node *n) {
      struct cmdentry entry;

      if (n->type == NCMD && goodname(n->ncmd.args->narg.text))
	    find_command(n->ncmd.args->narg.text, &entry, 0);
}

int bltincmd(int argc, char **argv) {
      listsetvar(cmdenviron);
      return exitstatus;
}

int breakcmd(int argc, char **argv) {
      int n = 1;

      if (argc > 1)
	    n = number(argv[1]);
      if (n > loopnest)
	    n = loopnest;
      if (n > 0) {
	    evalskip = (**argv == 'c') ? SKIPCONT : SKIPBREAK;
	    skipcount = n;
      }
      return 0;
}

int returncmd(int argc, char **argv) {
      int ret = exitstatus;

      if (argc > 1)
	    ret = number(argv[1]);
      if (funcnest) {
	    evalskip = SKIPFUNC;
	    skipcount = 1;
      }
      return ret;
}

int truecmd(int argc, char **argv) {
      return 0;
}

int execcmd(int argc, char **argv) {
      if (argc > 1) {
	    iflag = 0;		/* exit on error */
	    setinteractive(0);
#if JOBS
	    jflag = 0;
	    setjobctl(0);
#endif
	    shellexec(argv + 1, environment(), pathval(), 0);

      }
      return 0;
}
