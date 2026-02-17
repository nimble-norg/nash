/*
 * Copyright (C) 1989 by Kenneth Almquist.  All rights reserved.
 * This file is part of ash, which is distributed under the terms specified
 * by the Ash General Public License.  See the file named LICENSE.
 *
 * This file implements the input routines used by the parser.
 */

#include <stdio.h>	/* defines BUFSIZ */
#include "shell.h"
#include "syntax.h"
#include "input.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include <fcntl.h>
#include "myerrno.h"
#include <string.h>
#include <stdlib.h>
#include "redir.h"
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "var.h"

#define EOF_NLEFT -99		/* value of parsenleft when EOF pushed back */


/*
 * The parsefile structure pointed to by the global variable parsefile
 * contains information about the current file being read.
 */

MKINIT
struct parsefile {
      int linno;		/* current line */
      int fd;			/* file descriptor (or -1 if string) */
      int nleft;		/* number of chars left in buffer */
      char *nextc;		/* next char in buffer */
      struct parsefile *prev;	/* preceding file on stack */
      char *buf;		/* input buffer */
};


int plinno = 1;			/* input line number */
MKINIT int parsenleft;		/* copy of parsefile->nleft */
char *parsenextc;		/* copy of parsefile->nextc */
MKINIT struct parsefile basepf;	/* top level input file */
char basebuf[BUFSIZ];		/* buffer for top level input file */
struct parsefile *parsefile = &basepf;	/* current input file */
char *pushedstring;		/* copy of parsenextc when text pushed back */
int pushednleft;		/* copy of parsenleft when text pushed back */

#ifdef __STDC__
STATIC void pushfile(void);
#else
STATIC void pushfile();
#endif


#define HIST_MAX 50
#define HIST_LEN 256
static char hist[HIST_MAX][HIST_LEN];
static int hist_count = 0;
static int hist_pos = 0;

static char *get_ps1(void) {
      char *ps1 = vps1.text;
      if (ps1) {
            char *eq = strchr(ps1, '=');
            if (eq) return eq + 1;
      }
      return "$ ";
}

static void enable_raw_mode(struct termios *orig) {
      struct termios raw;
      tcgetattr(STDIN_FILENO, orig);
      raw = *orig;
      raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
      raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
      raw.c_cflag |= (CS8);
      raw.c_oflag &= ~(OPOST);
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disable_raw_mode(struct termios *orig) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, orig);
}

static void hist_add(const char *line) {
      if (hist_count < HIST_MAX) {
            strncpy(hist[hist_count], line, HIST_LEN - 1);
            hist[hist_count][HIST_LEN - 1] = '\0';
            hist_count++;
      } else {
            int i;
            for (i = 1; i < HIST_MAX; i++) {
                  strcpy(hist[i - 1], hist[i]);
            }
            strncpy(hist[HIST_MAX - 1], line, HIST_LEN - 1);
            hist[HIST_MAX - 1][HIST_LEN - 1] = '\0';
      }
      hist_pos = hist_count;
}

static void tab_completion(char *buf, int *pos, int *len, int maxlen) {
      int start = *pos;
      while (start > 0 && buf[start - 1] != ' ') start--;
      
      int word_len = *pos - start;
      if (word_len == 0) return;

      char prefix[256];
      strncpy(prefix, buf + start, word_len);
      prefix[word_len] = '\0';

      char *dir_to_open = ".";
      char *file_prefix = prefix;
      char *last_slash = strrchr(prefix, '/');
      char path_buf[256];

      if (last_slash) {
            strncpy(path_buf, prefix, last_slash - prefix);
            path_buf[last_slash - prefix] = (last_slash == prefix) ? '/' : '\0';
            if (path_buf[0] == '\0') strcpy(path_buf, "/");
            dir_to_open = path_buf;
            file_prefix = last_slash + 1;
      }

      char *matches[128];
      int match_count = 0;
      int is_cmd = (start == 0 && !last_slash);

      if (is_cmd) {
            char *path = pathval();
            if (path) {
                  char *path_copy = strdup(path);
                  char *dir_path = strtok(path_copy, ":");
                  while (dir_path && match_count < 128) {
                        DIR *d = opendir(dir_path);
                        if (d) {
                              struct dirent *de;
                              while ((de = readdir(d)) != NULL && match_count < 128) {
                                    if (strncmp(de->d_name, prefix, word_len) == 0 && de->d_name[0] != '.') {
                                          int exists = 0;
                                          for(int j=0; j<match_count; j++) 
                                                if(strcmp(matches[j], de->d_name) == 0) exists = 1;
                                          if(!exists) matches[match_count++] = strdup(de->d_name);
                                    }
                              }
                              closedir(d);
                        }
                        dir_path = strtok(NULL, ":");
                  }
                  free(path_copy);
            }
      } else {
            DIR *d = opendir(dir_to_open);
            if (d) {
                  struct dirent *de;
                  int f_pref_len = strlen(file_prefix);
                  while ((de = readdir(d)) != NULL && match_count < 128) {
                        if (strncmp(de->d_name, file_prefix, f_pref_len) == 0) {
                              if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                              matches[match_count++] = strdup(de->d_name);
                        }
                  }
                  closedir(d);
            }
      }

      if (match_count == 1) {
            char *to_add = matches[0] + strlen(file_prefix);
            int add_len = strlen(to_add);
            if (*len + add_len + 1 < maxlen) {
                  memmove(buf + *pos + add_len, buf + *pos, *len - *pos);
                  memcpy(buf + *pos, to_add, add_len);
                  *len += add_len;
                  *pos += add_len;
                  
                  struct stat st;
                  char full_p[512];
                  snprintf(full_p, sizeof(full_p), "%s/%s", dir_to_open, matches[0]);
                  if (stat(full_p, &st) == 0 && S_ISDIR(st.st_mode)) {
                        buf[*pos] = '/';
                  } else {
                        buf[*pos] = ' ';
                  }
                  (*len)++;
                  (*pos)++;
                  buf[*len] = '\0';
                  char *current_ps1 = get_ps1();
                  write(STDOUT_FILENO, "\r", 1);
                  write(STDOUT_FILENO, current_ps1, strlen(current_ps1));
                  write(STDOUT_FILENO, buf, *len);
            }
      } else if (match_count > 1) {
            write(STDOUT_FILENO, "\r\n", 2);
            for (int i = 0; i < match_count; i++) {
                  write(STDOUT_FILENO, matches[i], strlen(matches[i]));
                  write(STDOUT_FILENO, (i == match_count - 1) ? "" : "  ", 2);
            }
            char *current_ps1 = get_ps1();
            write(STDOUT_FILENO, "\r\n", 2);
            write(STDOUT_FILENO, current_ps1, strlen(current_ps1));
            write(STDOUT_FILENO, buf, *len);
            for (int i = 0; i < *len - *pos; i++) write(STDOUT_FILENO, "\b", 1);
      }

      for (int i = 0; i < match_count; i++) free(matches[i]);
}

static int input_readline(char *buf, int maxlen) {
      struct termios orig_termios;
      int pos = 0;
      int len = 0;
      char ch;
      int i;

      enable_raw_mode(&orig_termios);
      hist_pos = hist_count;

      memset(buf, 0, maxlen);

      while (1) {
            if (read(STDIN_FILENO, &ch, 1) != 1) break;

            if (ch == '\r' || ch == '\n') {
                  if (len > 0) hist_add(buf);
                  write(STDOUT_FILENO, "\r\n", 2);
                  buf[len] = '\n';
                  len++;
                  break;
            } else if (ch == 127 || ch == '\b') {
                  if (pos > 0) {
                        if (pos < len) {
                             memmove(buf + pos - 1, buf + pos, len - pos);
                        }
                        pos--;
                        len--;
                        buf[len] = '\0';
                        write(STDOUT_FILENO, "\b", 1);
                        write(STDOUT_FILENO, buf + pos, len - pos);
                        write(STDOUT_FILENO, " ", 1);
                        for (i = 0; i < len - pos + 1; i++) write(STDOUT_FILENO, "\b", 1);
                  }
            } else if (ch == '\t') {
                  tab_completion(buf, &pos, &len, maxlen);
            } else if (ch == '\033') {
                  char seq[3];
                  if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == '[') {
                              if (seq[1] == 'A') { /* Up */
                                    if (hist_pos > 0) {
                                          hist_pos--;
                                          while (pos > 0) {
                                                write(STDOUT_FILENO, "\b \b", 3);
                                                pos--;
                                          }
                                          strcpy(buf, hist[hist_pos]);
                                          len = strlen(buf);
                                          pos = len;
                                          write(STDOUT_FILENO, buf, len);
                                    }
                              } else if (seq[1] == 'B') { /* Down */
                                    if (hist_pos < hist_count) {
                                          hist_pos++;
                                          while (pos > 0) {
                                                write(STDOUT_FILENO, "\b \b", 3);
                                                pos--;
                                          }
                                          if (hist_pos < hist_count) {
                                                strcpy(buf, hist[hist_pos]);
                                                len = strlen(buf);
                                          } else {
                                                buf[0] = '\0';
                                                len = 0;
                                          }
                                          pos = len;
                                          write(STDOUT_FILENO, buf, len);
                                    }
                              } else if (seq[1] == 'C') { /* Right */
                                    if (pos < len) {
                                          write(STDOUT_FILENO, &buf[pos], 1);
                                          pos++;
                                    }
                              } else if (seq[1] == 'D') { /* Left */
                                    if (pos > 0) {
                                          write(STDOUT_FILENO, "\b", 1);
                                          pos--;
                                    }
                              } else if (seq[1] == 'H') { /* Home */
                                    while (pos > 0) {
                                          write(STDOUT_FILENO, "\b", 1);
                                          pos--;
                                    }
                              } else if (seq[1] == 'F') { /* End */
                                    while (pos < len) {
                                          write(STDOUT_FILENO, &buf[pos], 1);
                                          pos++;
                                    }
                              } else if (seq[1] >= '1' && seq[1] <= '4') {
                                    char tilde;
                                    read(STDIN_FILENO, &tilde, 1);
                                    if (seq[1] == '1') { /* Home */
                                          while (pos > 0) {
                                                write(STDOUT_FILENO, "\b", 1);
                                                pos--;
                                          }
                                    } else if (seq[1] == '4') { /* End */
                                          while (pos < len) {
                                                write(STDOUT_FILENO, &buf[pos], 1);
                                                pos++;
                                          }
                                    }
                              }
                        }
                  }
            } else if (!iscntrl(ch) && len < maxlen - 2) {
                  if (pos < len) {
                        memmove(buf + pos + 1, buf + pos, len - pos);
                  }
                  buf[pos] = ch;
                  write(STDOUT_FILENO, &buf[pos], len - pos + 1);
                  for (i = 0; i < len - pos; i++) write(STDOUT_FILENO, "\b", 1);
                  len++;
                  pos++;
            }
      }

      disable_raw_mode(&orig_termios);
      return len;
}


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

char *pfgets(char *line, int len) {
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

int pgetc() {
      return pgetc_macro();
}


/*
 * Refill the input buffer and return the next input character:
 *
 * 1) If a string was pushed back on the input, switch back to the regular
 * buffer.
 * 2) If an EOF was pushed back (parsenleft == EOF_NLEFT) or we are reading
 * from a string so we can't refill the buffer, return EOF.
 * 3) Call read to read in the characters.
 * 4) Delete all nul characters from the buffer.
 */

int preadbuffer() {
      register char *p, *q;
      register int i;

      if (pushedstring) {
	    parsenextc = pushedstring;
	    pushedstring = NULL;
	    parsenleft = pushednleft;
	    if (--parsenleft >= 0)
		  return *parsenextc++;
      }
      if (parsenleft == EOF_NLEFT || parsefile->buf == NULL)
	    return PEOF;
      flushout(&output);
      flushout(&errout);
retry:
      p = parsenextc = parsefile->buf;
      
      if (parsefile->fd == 0 && isatty(0)) {
            i = input_readline(p, BUFSIZ);
      } else {
            i = read(parsefile->fd, p, BUFSIZ);
      }

      if (i <= 0) {
	    if (i < 0) {
		  if (errno == EINTR)
			goto retry;
#ifdef BSD
		  if (parsefile->fd == 0 && errno == EWOULDBLOCK) {
			int flags = fcntl(0, F_GETFL, 0);
			if (flags >= 0 && flags & FNDELAY) {
			      flags &=~ FNDELAY;
			      if (fcntl(0, F_SETFL, flags) >= 0) {
				    out2str("ash: turning off NDELAY mode\n");
				    goto retry;
			      }
			}
		  }
#endif
	    }
#ifdef SYSV
	    if (i == 0 && parsefile->fd == 0) {
		  int flags = fcntl(0, F_GETFL, 0);
		  if (flags >= 0 && flags & O_NDELAY) {
			flags &=~ O_NDELAY;
			if (fcntl(0, F_SETFL, flags) >= 0) {
			      out2str("ash: turning off NDELAY mode\n");
			      goto retry;
			}
		  }
	    }
#endif
	    parsenleft = EOF_NLEFT;
	    return PEOF;
      }
      parsenleft = i - 1;

      /* delete nul characters */
      for (;;) {
	    if (*p++ == '\0')
		  break;
	    if (--i <= 0)
		  return *parsenextc++;		/* no nul characters */
      }
      q = p - 1;
      while (--i > 0) {
	    if (*p != '\0')
		  *q++ = *p;
	    p++;
      }
      if (q == parsefile->buf)
	    goto retry;			/* buffer contained nothing but nuls */
      parsenleft = q - parsefile->buf - 1;
      return *parsenextc++;
}


/*
 * Undo the last call to pgetc.  Only one character may be pushed back.
 * PEOF may be pushed back.
 */

void pungetc() {
      parsenleft++;
      parsenextc--;
}


/*
 * Push a string back onto the input.  This code doesn't work if the user
 * tries to push back more than one string at once.
 */

void ppushback(char *string, int length) {
      pushedstring = parsenextc;
      pushednleft = parsenleft;
      parsenextc = string;
      parsenleft = length;
}



/*
 * Set the input to take input from a file.  If push is set, push the
 * old input onto the stack first.
 */

void setinputfile(char *fname, int push) {
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

void setinputfd(int fd, int push) {
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

void setinputstring(char *string, int push) {
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

STATIC void pushfile() {
      struct parsefile *pf;

      parsefile->nleft = parsenleft;
      parsefile->nextc = parsenextc;
      parsefile->linno = plinno;
      pf = (struct parsefile *)ckmalloc(sizeof (struct parsefile));
      pf->prev = parsefile;
      pf->fd = -1;
      parsefile = pf;
}


void popfile() {
      struct parsefile *pf = parsefile;

      INTOFF;
      if (pf->fd >= 0)
	    close(pf->fd);
      if (pf->buf)
	    ckfree(pf->buf);
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

void popallfiles() {
      while (parsefile != &basepf)
	    popfile();
}



/*
 * Close the file(s) that the shell is reading commands from.  Called
 * after a fork is done.
 */

void closescript() {
      popallfiles();
      if (parsefile->fd > 0) {
	    close(parsefile->fd);
	    parsefile->fd = 0;
      }
}
