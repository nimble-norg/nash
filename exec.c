/*
 * When commands are first encountered, they are entered in a hash table.
 * This ensures that a full path search will not have to be done for them
 * on each invocation.
 *
 * We should investigate converting to a linear search, even though that
 * would make the command name "hash" a misnomer.
 */

#include "shell.h"
#include "main.h"
#include "nodes.h"
#include "parser.h"
#include "redir.h"
#include "eval.h"
#include "exec.h"
#include "builtins.h"
#include "var.h"
#include "options.h"
#include "input.h"
#include "output.h"
#include "syntax.h"
#include "memalloc.h"
#include "error.h"
#include "init.h"
#include "mystring.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "myerrno.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CMDTABLESIZE 31		/* should be prime */
#define ARB 1			/* actual size determined at run time */

struct tblentry {
      struct tblentry *next;	/* next entry in hash chain */
      union param param;	/* definition of builtin function */
      short cmdtype;		/* index identifying command */
      char rehash;		/* if set, cd done since entry created */
      char cmdname[ARB];	/* name of command */
};

static struct tblentry *cmdtable[CMDTABLESIZE];
static int builtinloc = -1;	/* index in path of %builtin, or -1 */

static void tryexec(char *, char **, char **);
static void execinterp(char **, char **);
static void printentry(struct tblentry *);
static void clearcmdentry(int);
static struct tblentry *cmdlookup(const char *, int);
static void delete_cmd_entry(void);

/* Declarações externas corrigidas para compatibilidade com main.h e C99 */
extern void readcmdfile(const char *);
extern union node *copyfunc(union node *);
extern void freefunc(union node *);
extern int pgetc(void);
extern void pungetc(void);
extern int find_builtin(char *);

/*
 * Exec a program.  Never returns.  If you change this routine, you may
 * have to change the find_command routine as well.
 */

void shellexec(char **argv, char **envp, char *path, int index) {
      char *cmdname;
      int e;

      if (strchr(argv[0], '/') != NULL) {
	    tryexec(argv[0], argv, envp);
	    e = errno;
      } else {
	    e = ENOENT;
	    while ((cmdname = padvance(&path, argv[0])) != NULL) {
		  if (--index < 0 && pathopt == NULL) {
			tryexec(cmdname, argv, envp);
			if (errno != ENOENT && errno != ENOTDIR)
			      e = errno;
		  }
		  stunalloc(cmdname);
	    }
      }
      error2(argv[0], errmsg(e, E_EXEC));
}

static void tryexec(char *cmd, char **argv, char **envp) {
      int e;
      char *p;

#ifdef SYSV
      do {
	    execve(cmd, argv, envp);
      } while (errno == EINTR);
#else
      execve(cmd, argv, envp);
#endif
      e = errno;
      if (e == ENOEXEC) {
	    initshellproc();
	    setinputfile(cmd, 0);
	    commandname = arg0 = savestr(argv[0]);
#ifndef BSD
	    pgetc(); pungetc();		/* fill up input buffer */
	    p = parsenextc;
	    if (parsenleft > 2 && p[0] == '#' && p[1] == '!') {
		  argv[0] = cmd;
		  execinterp(argv, envp);
	    }
#endif
	    setparam(argv + 1);
	    exraise(EXSHELLPROC);
	    /*NOTREACHED*/
      }
      errno = e;
}

#ifndef BSD
/*
 * Execute an interpreter introduced by "#!", for systems where this
 * feature has not been built into the kernel.  If the interpreter is
 * the shell, return (effectively ignoring the "#!").  If the execution
 * of the interpreter fails, exit.
 */

#define NEWARGS 5

static void execinterp(char **argv, char **envp) {
      int n;
      char *inp;
      char *outp;
      char c;
      char *p;
      char **ap;
      char *newargs[NEWARGS];
      int i;
      char **ap2;
      char **new;

      n = parsenleft - 2;
      inp = parsenextc + 2;
      ap = newargs;
      for (;;) {
	    while (--n >= 0 && (*inp == ' ' || *inp == '\t'))
		  inp++;
	    if (n < 0)
		  goto bad;
	    if ((c = *inp++) == '\n')
		  break;
	    if (ap == &newargs[NEWARGS])
bad:		  error("Bad #! line");
	    STARTSTACKSTR(outp);
	    do {
		  STPUTC(c, outp);
	    } while (--n >= 0 && (c = *inp++) != ' ' && c != '\t' && c != '\n');
	    STPUTC('\0', outp);
	    n++, inp--;
	    *ap++ = grabstackstr(outp);
      }
      if (ap == newargs + 1) {
	    p = newargs[0];
	    for (;;) {
		  if (strcmp(p, "sh") == 0 || strcmp(p, "ash") == 0) {
			return;
		  }
		  while (*p != '/') {
			if (*p == '\0')
			      goto break2;
			p++;
		  }
		  p++;
	    }
break2:;
      }
      i = (int)((char *)ap - (char *)newargs);
      if (i == 0)
	    error("Bad #! line");
      for (ap2 = argv ; *ap2++ != NULL ; );
      new = ckmalloc((size_t)i + (size_t)((char *)ap2 - (char *)argv));
      ap = newargs, ap2 = new;
      while ((i -= (int)sizeof (char **)) >= 0)
	    *ap2++ = *ap++;
      ap = argv;
      while ((*ap2++ = *ap++));
      shellexec(new, envp, pathval(), 0);
}
#endif

char *pathopt;

char *padvance(char **path, char *name) {
      char *p, *q;
      char *start;
      int len;

      if (*path == NULL)
	    return NULL;
      start = *path;
      for (p = start ; *p && *p != ':' && *p != '%' ; p++);
      len = (int)(p - start + (int)strlen(name) + 2);
      while (stackblocksize() < len)
	    growstackblock();
      q = stackblock();
      if (p != start) {
	    memcpy(q, start, (size_t)(p - start));
	    q += p - start;
	    *q++ = '/';
      }
      strcpy(q, name);
      pathopt = NULL;
      if (*p == '%') {
	    pathopt = ++p;
	    while (*p && *p != ':')  p++;
      }
      if (*p == ':')
	    *path = p + 1;
      else
	    *path = NULL;
      return stalloc(len);
}

int hashcmd(int argc, char **argv) {
      struct tblentry **pp;
      struct tblentry *cmdp;
      int c;
      int verbose;
      struct cmdentry entry;
      char *name;

      if (argc <= 1) {
	    for (pp = cmdtable ; pp < &cmdtable[CMDTABLESIZE] ; pp++) {
		  for (cmdp = *pp ; cmdp ; cmdp = cmdp->next) {
			printentry(cmdp);
		  }
	    }
	    return 0;
      }
      verbose = 0;
      while ((c = nextopt("rv")) != '\0') {
	    if (c == 'r') {
		  clearcmdentry(0);
	    } else if (c == 'v') {
		  verbose++;
	    }
      }
      while ((name = *argptr) != NULL) {
	    if ((cmdp = cmdlookup(name, 0)) != NULL
	     && (cmdp->cmdtype == CMDNORMAL
		|| (cmdp->cmdtype == CMDBUILTIN && builtinloc >= 0)))
		  delete_cmd_entry();
	    find_command(name, &entry, 1);
	    if (verbose) {
		  if (entry.cmdtype != CMDUNKNOWN) {
			cmdp = cmdlookup(name, 0);
			printentry(cmdp);
		  }
		  flushall();
	    }
	    argptr++;
      }
      return 0;
}

static void printentry(struct tblentry *cmdp) {
      int index;
      char *path;
      char *name;

      if (cmdp->cmdtype == CMDNORMAL) {
	    index = cmdp->param.index;
	    path = pathval();
	    do {
		  name = padvance(&path, cmdp->cmdname);
		  stunalloc(name);
	    } while (--index >= 0);
	    out1str(name);
      } else if (cmdp->cmdtype == CMDBUILTIN) {
	    out1fmt("builtin %s", cmdp->cmdname);
      } else if (cmdp->cmdtype == CMDFUNCTION) {
	    out1fmt("function %s", cmdp->cmdname);
#ifdef DEBUG
      } else {
	    error("internal error: cmdtype %d", cmdp->cmdtype);
#endif
      }
      if (cmdp->rehash)
	    out1c('*');
      out1c('\n');
}

void find_command(char *name, struct cmdentry *entry, int printerr) {
      struct tblentry *cmdp;
      int index;
      int prev;
      char *path;
      char *fullname;
      struct stat statb;
      int e;
      int i;

      if (strchr(name, '/') != NULL) {
	    entry->cmdtype = CMDNORMAL;
	    entry->u.index = 0;
	    return;
      }

      if ((cmdp = cmdlookup(name, 0)) != NULL && cmdp->rehash == 0)
	    goto success;

      if (builtinloc < 0 && (i = find_builtin(name)) >= 0) {
	    INTOFF;
	    cmdp = cmdlookup(name, 1);
	    cmdp->cmdtype = CMDBUILTIN;
	    cmdp->param.index = i;
	    INTON;
	    goto success;
      }

      prev = -1;
      if (cmdp) {
	    if (cmdp->cmdtype == CMDBUILTIN)
		  prev = builtinloc;
	    else
		  prev = cmdp->param.index;
      }

      path = pathval();
      e = ENOENT;
      index = -1;
loop:
      while ((fullname = padvance(&path, name)) != NULL) {
	    stunalloc(fullname);
	    index++;
	    if (pathopt) {
		  if (prefix("builtin", pathopt)) {
			if ((i = find_builtin(name)) < 0)
			      goto loop;
			INTOFF;
			cmdp = cmdlookup(name, 1);
			cmdp->cmdtype = CMDBUILTIN;
			cmdp->param.index = i;
			INTON;
			goto success;
		  } else if (prefix("func", pathopt)) {
			/* handled below */
		  } else {
			goto loop;
		  }
	    }
	    if (fullname[0] == '/' && index <= prev) {
		  if (index < prev)
			goto loop;
		  goto success;
	    }
	    while (stat(fullname, &statb) < 0) {
#ifdef SYSV
		  if (errno == EINTR)
			continue;
#endif
		  if (errno != ENOENT && errno != ENOTDIR)
			e = errno;
		  goto loop;
	    }
	    e = EACCES;
            /* Corrigido para usar macros POSIX de teste de arquivo se disponíveis, 
               mantendo compatibilidade com a lógica de bits original */
	    if (!S_ISREG(statb.st_mode))
		  goto loop;
	    if (pathopt) {
		  stalloc(strlen(fullname) + 1);
		  readcmdfile(fullname);
		  if ((cmdp = cmdlookup(name, 0)) == NULL || cmdp->cmdtype != CMDFUNCTION)
			error("%s not defined in %s", name, fullname);
		  stunalloc(fullname);
		  goto success;
	    }
	    if (statb.st_uid == geteuid()) {
		  if ((statb.st_mode & 0100) == 0)
			goto loop;
	    } else if (statb.st_gid == getegid()) {
		  if ((statb.st_mode & 010) == 0)
			goto loop;
	    } else {
		  if ((statb.st_mode & 01) == 0)
			goto loop;
	    }
	    INTOFF;
	    cmdp = cmdlookup(name, 1);
	    cmdp->cmdtype = CMDNORMAL;
	    cmdp->param.index = index;
	    INTON;
	    goto success;
      }

      if (cmdp)
	    delete_cmd_entry();
      if (printerr)
	    outfmt(out2, "%s: %s\n", name, errmsg(e, E_EXEC));
      entry->cmdtype = CMDUNKNOWN;
      return;

success:
      cmdp->rehash = 0;
      entry->cmdtype = cmdp->cmdtype;
      entry->u = cmdp->param;
}

int find_builtin(char *name) {
      const struct builtincmd *bp;

      for (bp = builtincmd ; bp->name ; bp++) {
	    if (*bp->name == *name && strcmp(bp->name, name) == 0)
		  return bp->code;
      }
      return -1;
}

void hashcd(void) {
      struct tblentry **pp;
      struct tblentry *cmdp;

      for (pp = cmdtable ; pp < &cmdtable[CMDTABLESIZE] ; pp++) {
	    for (cmdp = *pp ; cmdp ; cmdp = cmdp->next) {
		  if (cmdp->cmdtype == CMDNORMAL
		   || (cmdp->cmdtype == CMDBUILTIN && builtinloc >= 0))
			cmdp->rehash = 1;
	    }
      }
}

void changepath(char *newval) {
      char *old, *new;
      int index;
      int firstchange;
      int bltin;

      old = pathval();
      new = newval;
      firstchange = 9999;
      index = 0;
      bltin = -1;
      for (;;) {
	    if (*old != *new) {
		  firstchange = index;
		  if ((*old == '\0' && *new == ':')
		   || (*old == ':' && *new == '\0'))
			firstchange++;
		  old = new;
	    }
	    if (*new == '\0')
		  break;
	    if (*new == '%' && bltin < 0 && prefix("builtin", new + 1))
		  bltin = index;
	    if (*new == ':') {
		  index++;
	    }
	    new++, old++;
      }
      if (builtinloc < 0 && bltin >= 0)
	    builtinloc = bltin;
      if (builtinloc >= 0 && bltin < 0)
	    firstchange = 0;
      clearcmdentry(firstchange);
      builtinloc = bltin;
}

static void clearcmdentry(int firstchange) {
      struct tblentry **tblp;
      struct tblentry **pp;
      struct tblentry *cmdp;

      INTOFF;
      for (tblp = cmdtable ; tblp < &cmdtable[CMDTABLESIZE] ; tblp++) {
	    pp = tblp;
	    while ((cmdp = *pp) != NULL) {
		  if ((cmdp->cmdtype == CMDNORMAL && cmdp->param.index >= firstchange)
		   || (cmdp->cmdtype == CMDBUILTIN && builtinloc >= firstchange)) {
			*pp = cmdp->next;
			ckfree(cmdp);
		  } else {
			pp = &cmdp->next;
		  }
	    }
      }
      INTON;
}

#ifdef mkinit
MKINIT void deletefuncs(void);

SHELLPROC {
      deletefuncs();
}
#endif

void deletefuncs(void) {
      struct tblentry **tblp;
      struct tblentry **pp;
      struct tblentry *cmdp;

      INTOFF;
      for (tblp = cmdtable ; tblp < &cmdtable[CMDTABLESIZE] ; tblp++) {
	    pp = tblp;
	    while ((cmdp = *pp) != NULL) {
		  if (cmdp->cmdtype == CMDFUNCTION) {
			*pp = cmdp->next;
			freefunc(cmdp->param.func);
			ckfree(cmdp);
		  } else {
			pp = &cmdp->next;
		  }
	    }
      }
      INTON;
}

struct tblentry **lastcmdentry;

static struct tblentry *cmdlookup(const char *name, int add) {
      int hashval;
      const char *p;
      struct tblentry *cmdp;
      struct tblentry **pp;

      p = name;
      hashval = (int)(*p << 4);
      while (*p)
	    hashval += *p++;
      hashval &= 0x7FFF;
      pp = &cmdtable[hashval % CMDTABLESIZE];
      for (cmdp = *pp ; cmdp ; cmdp = cmdp->next) {
	    if (strcmp(cmdp->cmdname, name) == 0)
		  break;
	    pp = &cmdp->next;
      }
      if (add && cmdp == NULL) {
	    INTOFF;
	    cmdp = *pp = ckmalloc(sizeof (struct tblentry) - ARB
			      + strlen(name) + 1);
	    cmdp->next = NULL;
	    cmdp->cmdtype = CMDUNKNOWN;
	    cmdp->rehash = 0;
	    strcpy(cmdp->cmdname, name);
	    INTON;
      }
      lastcmdentry = pp;
      return cmdp;
}

static void delete_cmd_entry(void) {
      struct tblentry *cmdp;

      INTOFF;
      cmdp = *lastcmdentry;
      if (cmdp) {
          *lastcmdentry = cmdp->next;
          ckfree(cmdp);
      }
      INTON;
}

void addcmdentry(char *name, struct cmdentry *entry) {
      struct tblentry *cmdp;

      INTOFF;
      cmdp = cmdlookup(name, 1);
      if (cmdp->cmdtype == CMDFUNCTION) {
	    freefunc(cmdp->param.func);
      }
      cmdp->cmdtype = entry->cmdtype;
      cmdp->param = entry->u;
      INTON;
}

void defun(char *name, union node *func) {
      struct cmdentry entry;

      INTOFF;
      entry.cmdtype = CMDFUNCTION;
      entry.u.func = copyfunc(func);
      addcmdentry(name, &entry);
      INTON;
}

void unsetfunc(char *name) {
      struct tblentry *cmdp;

      if ((cmdp = cmdlookup(name, 0)) != NULL && cmdp->cmdtype == CMDFUNCTION) {
	    freefunc(cmdp->param.func);
	    delete_cmd_entry();
      }
}

