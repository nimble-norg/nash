#ifndef NO_HISTORY

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <limits.h>

#include "lineread.h"

#define LBUF         4096
#define HIST_HARD    5000
#define HIST_DEFAULT 100
#define LR_MAX_CANDS 512




int lineread_enabled = 0;

static int lr_vi_mode = 0;
static int lr_allow_complete = 1;

static char **lr_hist;
static int    lr_hlen;
static int    lr_hcap;
static char  *lr_hfile;

static struct termios lr_orig;
static int            lr_saved;
static int            lr_raw;




static void lr_save_termios(void)
{
    if (!lr_saved && isatty(0)) { tcgetattr(0, &lr_orig); lr_saved = 1; }
}

static int lr_enter_raw(void)
{
    struct termios t;
    lr_save_termios();
    if (!lr_saved) return -1;
    t = lr_orig;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &t) < 0) return -1;
    lr_raw = 1;
    return 0;
}

static void lr_leave_raw(void)
{
    if (lr_raw && lr_saved) { tcsetattr(0, TCSAFLUSH, &lr_orig); lr_raw = 0; }
}

static void lr_atexit(void) { lr_leave_raw(); lineread_hist_save(); }




static int lr_get_histsize(void)
{
    const char *s = getenv("HISTSIZE");
    int n;
    if (s && (n = atoi(s)) > 0) return n < HIST_HARD ? n : HIST_HARD;
    return HIST_DEFAULT;
}




static void lr_hist_push(const char *line) { lineread_hist_push(line); }

void lineread_hist_push(const char *line)
{
    int   max;
    char *copy;

    if (!line || !*line) return;
    if (lr_hlen > 0 && strcmp(lr_hist[lr_hlen-1], line) == 0) return;

    max = lr_get_histsize();

    if (lr_hlen == lr_hcap) {
        int nc; char **p;
        if (lr_hcap == 0) {
            nc = 64 < max ? 64 : max;
            p  = malloc((size_t)nc * sizeof(char *));
        } else {
            nc = lr_hcap * 2;
            if (nc > HIST_HARD) nc = HIST_HARD;
            p  = realloc(lr_hist, (size_t)nc * sizeof(char *));
        }
        if (!p) return;
        lr_hist = p; lr_hcap = nc;
    }

    while (lr_hlen >= max) {
        free(lr_hist[0]);
        memmove(lr_hist, lr_hist+1, (size_t)(lr_hlen-1)*sizeof(char*));
        lr_hlen--;
    }

    copy = strdup(line);
    if (!copy) return;
    lr_hist[lr_hlen++] = copy;
}

void lineread_hist_pop(void)
{
    if (lr_hlen > 0) {
        free(lr_hist[lr_hlen - 1]);
        lr_hlen--;
    }
}

static char *lr_default_hist_file(void)
{
    static char path[PATH_MAX];
    char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return NULL;
    snprintf(path, sizeof path, "%s/.ash_history", home);
    return path;
}

int lineread_hist_len(void) { return lr_hlen; }

const char *lineread_hist_entry(int idx)
{
    if (idx < 0 || idx >= lr_hlen) return NULL;
    return lr_hist[idx];
}

void lineread_hist_load(const char *file)
{
    FILE *f; char line[LBUF];
    if (!file) { file = getenv("HISTFILE"); if (!file) file = lr_default_hist_file(); }
    if (!file) return;
    if (lr_hfile) free(lr_hfile);
    lr_hfile = strdup(file);
    f = fopen(file, "r");
    if (!f) return;
    while (fgets(line, (int)sizeof line, f)) {
        int n = (int)strlen(line);
        if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
        if (line[0]) lr_hist_push(line);
    }
    fclose(f);
    {
        int max = lr_get_histsize();
        while (lr_hlen > max) {
            free(lr_hist[0]);
            memmove(lr_hist, lr_hist+1, (size_t)(lr_hlen-1)*sizeof(char*));
            lr_hlen--;
        }
    }
}

void lineread_hist_save(void)
{
    FILE *f; int i, start, max;
    const char *file = lr_hfile;
    if (!file) { file = getenv("HISTFILE"); if (!file) file = lr_default_hist_file(); }
    if (!file || lr_hlen == 0) return;
    max   = lr_get_histsize();
    start = lr_hlen > max ? lr_hlen - max : 0;
    f = fopen(file, "w");
    if (!f) return;
    for (i = start; i < lr_hlen; i++) fprintf(f, "%s\n", lr_hist[i]);
    fclose(f);
}

void lineread_cleanup(void) { lr_leave_raw(); lineread_hist_save(); }

void lineread_init(void)
{
    char *hfile;
    if (lineread_enabled) return;
    lr_save_termios();
    if (!getenv("HISTFILE")) {
        hfile = lr_default_hist_file();
        if (hfile) setenv("HISTFILE", hfile, 0);
    }
    if (!getenv("HISTSIZE")) {
        char szbuf[32];
        snprintf(szbuf, sizeof szbuf, "%d", HIST_DEFAULT);
        setenv("HISTSIZE", szbuf, 0);
    }
    lineread_hist_load(NULL);
    atexit(lr_atexit);
    lineread_enabled = 1;
}

void lineread_set_mode(int vi)
{
    lr_vi_mode = vi;
}

void lineread_set_allow_complete(int allow)
{
    lr_allow_complete = allow;
}




static int lr_term_cols(void)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 4)
        return (int)ws.ws_col;
    return 80;
}

static int lr_display_width(const char *s, int len)
{
    int w = 0, i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\033') {
            i++;
            if (i < len && s[i] == '[') {
                i++;
                while (i < len && s[i] != 'm') i++;
                if (i < len) i++;
            }
            continue;
        }
        w++;
        i++;
    }
    return w;
}

static void lr_refresh(const char *prompt, const char *buf, int len, int pos)
{
    int cols = lr_term_cols();
    int plen = (int)strlen(prompt);
    int pwidth = lr_display_width(prompt, plen);
    int buf_avail = cols - pwidth - 1;
    char tmp[LBUF + 512];
    int n = 0, i;
    int view_start, view_end;
    int has_left, has_right;

    if (buf_avail < 8) buf_avail = 8;

    if (len <= buf_avail) {
        view_start = 0;
        view_end = len;
    } else {
        int half = buf_avail / 2;
        view_start = pos - half;
        if (view_start < 0) view_start = 0;
        view_end = view_start + buf_avail;
        if (view_end > len) {
            view_end = len;
            view_start = view_end - buf_avail;
            if (view_start < 0) view_start = 0;
        }
    }
    has_left  = (view_start > 0);
    has_right = (view_end < len);

    if (has_left)  buf_avail--;
    if (has_right) buf_avail--;

    if (has_left && has_right) {
        view_end = view_start + buf_avail;
        if (view_end > len) view_end = len;
    } else if (has_left) {
        view_end = view_start + buf_avail;
        if (view_end > len) view_end = len;
    } else if (has_right) {
        view_end = view_start + buf_avail;
        if (view_end > len) view_end = len;
    }

    tmp[n++] = '\r';
    for (i = 0; i < plen && n < (int)(sizeof tmp) - 4; i++)
        tmp[n++] = prompt[i];
    if (has_left)  tmp[n++] = '<';
    for (i = view_start; i < view_end && n < (int)(sizeof tmp) - 4; i++)
        tmp[n++] = buf[i];
    if (has_right) tmp[n++] = '>';
    tmp[n++] = '\033'; tmp[n++] = '['; tmp[n++] = 'K';
    {
        int cursor_col = pwidth + (has_left ? 1 : 0) + (pos - view_start);
        int line_end   = pwidth + (has_left ? 1 : 0) + (view_end - view_start)
                         + (has_right ? 1 : 0);
        int back = line_end - cursor_col;
        for (i = 0; i < back && n < (int)(sizeof tmp) - 2; i++)
            tmp[n++] = '\b';
    }
    write(1, tmp, n);
}

static void lr_bell(void) { write(1, "\a", 1); }




static void lr_extend_common(char *common, int *clen, const char *cand, int first)
{
    if (first) {
        strncpy(common, cand, LBUF-1); common[LBUF-1] = '\0';
        *clen = (int)strlen(common);
    } else {
        int i;
        for (i = 0; i < *clen && common[i] && cand[i] && common[i] == cand[i]; i++)
            ;
        common[i] = '\0'; *clen = i;
    }
}

static int lr_cand_cmp(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static void lr_show_list(const char *prompt, const char *buf, int len, int pos,
                         char **cands, int ncands)
{
    char line[PATH_MAX + 24];
    int  i, w;
    write(1, "\r\n", 2);
    for (i = 0; i < ncands; i++) {
        w = snprintf(line, sizeof line, "%3d) %s\r\n", i + 1, cands[i]);
        if (w > 0) write(1, line, (size_t)w);
    }
    lr_refresh(prompt, buf, len, pos);
}

static void lr_cands_free(char **cands, int ncands)
{
    int i;
    for (i = 0; i < ncands; i++) free(cands[i]);
}

static int lr_complete_command(const char *word, int wlen,
                                char *common, int *clen,
                                char ***cands, int *cap, int *ncands)
{
    char *path_env, *path_copy, *dir, *save;
    char  fullpath[PATH_MAX];
    struct stat st;
    DIR  *d; struct dirent *de;
    int   found = 0, first = 1;

    char  **seen = NULL;
    int     nseen = 0, seen_cap = 0;

    *clen = 0; common[0] = '\0'; *ncands = 0;
    path_env = getenv("PATH");
    if (!path_env || !*path_env) return 0;
    path_copy = strdup(path_env);
    if (!path_copy) return 0;

    save = NULL;
    for (dir = strtok_r(path_copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        d = opendir(dir);
        if (!d) continue;
        while ((de = readdir(d)) != NULL) {
            int i, dup;

            if (strncmp(de->d_name, word, (size_t)wlen) != 0) continue;

            snprintf(fullpath, sizeof fullpath, "%s/%s", dir, de->d_name);
            if (stat(fullpath, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) continue;
            if (!(st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))) continue;

            dup = 0;
            for (i = 0; i < nseen; i++) {
                if (strcmp(seen[i], de->d_name) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            if (nseen == seen_cap) {
                int nc = seen_cap == 0 ? 64 : seen_cap * 2;
                char **np = realloc(seen, (size_t)nc * sizeof(char *));
                if (!np) continue;
                seen = np; seen_cap = nc;
            }
            seen[nseen] = strdup(de->d_name);
            if (!seen[nseen]) continue;
            nseen++;

            lr_extend_common(common, clen, de->d_name, first);
            first = 0; found++;

            if (*ncands == *cap) {
                int nc = *cap * 2;
                char **np = realloc(*cands, (size_t)nc * sizeof(char *));
                if (!np) continue;
                *cands = np; *cap = nc;
            }
            {
                char *disp = malloc(strlen(fullpath) + 1);
                if (disp) { strcpy(disp, fullpath); (*cands)[(*ncands)++] = disp; }
            }
        }
        closedir(d);
    }
    free(path_copy);
    if (seen) {
        int i;
        for (i = 0; i < nseen; i++) free(seen[i]);
        free(seen);
    }
    if (*ncands > 1) qsort(*cands, (size_t)*ncands, sizeof(char *), lr_cand_cmp);
    return found;
}


static int lr_complete_path(const char *word, int wlen,
                             char *common, int *clen,
                             char ***cands, int *cap, int *ncands)
{
    char         dirpart[PATH_MAX], fullpath[PATH_MAX], candidate[PATH_MAX];
    const char  *prefix, *slash;
    int          plen, found = 0, first = 1, isdir;
    DIR         *d; struct dirent *de; struct stat st;

    *clen = 0; common[0] = '\0'; *ncands = 0;

    slash = strrchr(word, '/');
    if (slash) {
        
        int dl = (int)(slash - word) + 1;
        if (dl >= (int)sizeof dirpart) return 0;
        strncpy(dirpart, word, (size_t)dl); dirpart[dl] = '\0';
        prefix = slash + 1;
    } else {
        dirpart[0] = '\0';   
        prefix = word;
    }
    plen = (int)strlen(prefix);

    d = opendir(dirpart[0] ? dirpart : ".");
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        
        if (de->d_name[0] == '.' && (plen == 0 || prefix[0] != '.'))
            continue;
        if (plen > 0 && strncmp(de->d_name, prefix, (size_t)plen) != 0) continue;

        
        if (dirpart[0])
            snprintf(fullpath, sizeof fullpath, "%s%s", dirpart, de->d_name);
        else
            snprintf(fullpath, sizeof fullpath, "%s",   de->d_name);
        isdir = (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode));

        
        snprintf(candidate, sizeof candidate, "%s%s%s",
                 dirpart, de->d_name, isdir ? "/" : "");

        lr_extend_common(common, clen, candidate, first);
        first = 0; found++;

        if (*ncands == *cap) {
            int nc = *cap * 2;
            char **np = realloc(*cands, (size_t)nc * sizeof(char *));
            if (!np) continue;
            *cands = np; *cap = nc;
        }
        {
            char *disp = malloc(strlen(candidate) + 1);
            if (disp) { strcpy(disp, candidate); (*cands)[(*ncands)++] = disp; }
        }
    }
    closedir(d);
    if (*ncands > 1) qsort(*cands, (size_t)*ncands, sizeof(char *), lr_cand_cmp);
    return found;
}

static void lr_complete(char *buf, int *lenp, int *posp, const char *prompt)
{
    int  len = *lenp, pos = *posp;
    int  wstart, wlen, at_first, clen = 0, found, i;
    char common[PATH_MAX], word[LBUF];
    char **cands;
    int   cap = 256, ncands = 0;

    cands = malloc((size_t)cap * sizeof(char *));
    if (!cands) { lr_bell(); return; }

    wstart = pos;
    while (wstart > 0) {
        char pc = buf[wstart - 1];
        if (pc == ' ' || pc == '\t' ||
            pc == ';' || pc == '|'  || pc == '&')
            break;
        wstart--;
    }
    wlen = pos - wstart;

    at_first = 1;
    for (i = wstart - 1; i >= 0; i--) {
        char c = buf[i];
        if (c == ' ' || c == '\t') continue;
        if (c == ';' || c == '&' || c == '|') { at_first = 1; break; }
        at_first = 0;
        break;
    }

    if (wlen >= LBUF) { free(cands); return; }
    strncpy(word, buf + wstart, (size_t)wlen); word[wlen] = '\0';

    if (at_first && strchr(word, '/') == NULL)
        found = lr_complete_command(word, wlen, common, &clen, &cands, &cap, &ncands);
    else
        found = lr_complete_path(word, wlen, common, &clen, &cands, &cap, &ncands);

    found = ncands;

    if (found == 0) { lr_bell(); free(cands); return; }

    if (clen > wlen) {
        int newlen = len - wlen + clen;
        if (newlen >= LBUF) { lr_cands_free(cands, ncands); free(cands); lr_bell(); return; }
        memmove(buf + wstart + clen, buf + pos, (size_t)(len - pos + 1));
        memcpy(buf + wstart, common, (size_t)clen);
        len = newlen;
        pos = wstart + clen;
        buf[len] = '\0';
        *lenp = len; *posp = pos;
        lr_refresh(prompt, buf, len, pos);
    }

    if (found == 1) {
        if (pos > 0 && buf[pos-1] != '/' && len < LBUF-1) {
            memmove(buf+pos+1, buf+pos, (size_t)(len-pos));
            buf[pos] = ' '; len++; pos++; buf[len] = '\0';
            *lenp = len; *posp = pos;
            lr_refresh(prompt, buf, len, pos);
        }
        lr_cands_free(cands, ncands); free(cands);
        return;
    }

    lr_show_list(prompt, buf, len, pos, cands, ncands);
    lr_cands_free(cands, ncands); free(cands);
    *lenp = len; *posp = pos;
}




char *lineread(const char *prompt)
{
    static char buf[LBUF];
    char saved[LBUF];
    int  len = 0, pos = 0, hidx, modified = 0;
    unsigned char c;

    if (!prompt) prompt = "";

    if (!isatty(0)) {
        if (*prompt) write(1, prompt, strlen(prompt));
        if (!fgets(buf, sizeof buf, stdin)) return NULL;
        len = (int)strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        return buf;
    }

    if (lr_enter_raw() < 0) {
        write(1, prompt, strlen(prompt));
        if (!fgets(buf, sizeof buf, stdin)) return NULL;
        len = (int)strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        return buf;
    }

    hidx = lr_hlen; saved[0] = buf[0] = '\0';
    write(1, prompt, strlen(prompt));

    for (;;) {
        if (read(0, &c, 1) <= 0) {
            lr_leave_raw();
            if (len == 0) return NULL;
            break;
        }
        switch (c) {

        case '\r': case '\n':
            write(1, "\r\n", 2); goto done;

        case '\t':
            if (lr_allow_complete)
                lr_complete(buf, &len, &pos, prompt);
            else
                lr_bell();
            modified = 1; break;

        case 127: case 8:
            if (pos > 0) {
                memmove(buf+pos-1, buf+pos, (size_t)(len-pos));
                len--; pos--; buf[len] = '\0'; modified = 1;
                lr_refresh(prompt, buf, len, pos);
            }
            break;

        case 1:  pos = 0;   lr_refresh(prompt, buf, len, pos); break;
        case 2:  if (pos > 0)  { pos--; lr_refresh(prompt, buf, len, pos); } break;

        case 3:  
            if (len > 0 && modified) lr_hist_push(buf);
            lr_leave_raw(); write(1, "^C\r\n", 4); raise(SIGINT);
            if (lr_enter_raw() < 0) return NULL;
            len = pos = 0; buf[0] = saved[0] = '\0'; modified = 0;
            hidx = lr_hlen; write(1, prompt, strlen(prompt)); break;

        case 4:
            if (len == 0) { lr_leave_raw(); return NULL; }
            if (pos < len) {
                memmove(buf+pos, buf+pos+1, (size_t)(len-pos-1));
                len--; buf[len] = '\0'; modified = 1;
                lr_refresh(prompt, buf, len, pos);
            }
            break;

        case 5:  pos = len; lr_refresh(prompt, buf, len, pos); break;
        case 6:  if (pos < len) { pos++; lr_refresh(prompt, buf, len, pos); } break;

        case 11: len = pos; buf[len] = '\0'; modified = 1;
                 lr_refresh(prompt, buf, len, pos); break;

        case 12: write(1, "\033[2J\033[H", 7);
                 lr_refresh(prompt, buf, len, pos); break;

        case 14: goto hist_next;
        case 16: goto hist_prev;

        case 21: pos = len = 0; buf[0] = '\0'; modified = 1;
                 lr_refresh(prompt, buf, len, pos); break;

        case 23:
            while (pos > 0 && buf[pos-1] == ' ')
                { memmove(buf+pos-1, buf+pos, (size_t)(len-pos)); len--; pos--; }
            while (pos > 0 && buf[pos-1] != ' ')
                { memmove(buf+pos-1, buf+pos, (size_t)(len-pos)); len--; pos--; }
            buf[len] = '\0'; modified = 1;
            lr_refresh(prompt, buf, len, pos); break;

        case 26:
            lr_leave_raw(); write(1, "^Z\r\n", 4); raise(SIGTSTP);
            if (lr_enter_raw() < 0) return NULL;
            lr_refresh(prompt, buf, len, pos); break;

        case 27: {
            unsigned char seq[2];
            if (read(0, &seq[0], 1) <= 0) break;
            if (seq[0] == '[' || seq[0] == 'O') {
                
                if (read(0, &seq[1], 1) <= 0) break;
                if (seq[0] == '[') {
                    switch (seq[1]) {
                    case 'A': 
                    hist_prev:
                        if (hidx > 0) {
                            if (modified && len > 0) lr_hist_push(buf);
                            if (hidx == lr_hlen) strncpy(saved, buf, LBUF-1);
                            hidx--;
                            strncpy(buf, lr_hist[hidx], LBUF-1); buf[LBUF-1] = '\0';
                            len = pos = (int)strlen(buf); modified = 0;
                            lr_refresh(prompt, buf, len, pos);
                        }
                        break;
                    case 'B': 
                    hist_next:
                        if (hidx < lr_hlen) {
                            if (modified && len > 0) lr_hist_push(buf);
                            hidx++;
                            if (hidx == lr_hlen) strncpy(buf, saved, LBUF-1);
                            else strncpy(buf, lr_hist[hidx], LBUF-1);
                            buf[LBUF-1] = '\0';
                            len = pos = (int)strlen(buf); modified = 0;
                            lr_refresh(prompt, buf, len, pos);
                        }
                        break;
                    case 'C': if (pos < len) { pos++; lr_refresh(prompt,buf,len,pos); } break;
                    case 'D': if (pos > 0)  { pos--; lr_refresh(prompt,buf,len,pos); } break;
                    case 'H': pos = 0;   lr_refresh(prompt,buf,len,pos); break;
                    case 'F': pos = len; lr_refresh(prompt,buf,len,pos); break;
                    case '1': case '2': case '3':
                    case '4': case '5': case '6':
                    case '7': case '8': {
                        unsigned char tilde;
                        if (read(0, &tilde, 1) <= 0) break;
                        if (tilde != '~') break;
                        if (seq[1]=='1'||seq[1]=='7') { pos=0;   lr_refresh(prompt,buf,len,pos); }
                        else if (seq[1]=='4'||seq[1]=='8') { pos=len; lr_refresh(prompt,buf,len,pos); }
                        else if (seq[1]=='3' && pos < len) {
                            memmove(buf+pos,buf+pos+1,(size_t)(len-pos-1));
                            len--; buf[len]='\0'; modified=1; lr_refresh(prompt,buf,len,pos);
                        }
                        break;
                    }
                    }
                } else { 
                    if (seq[1]=='H') { pos=0;   lr_refresh(prompt,buf,len,pos); }
                    if (seq[1]=='F') { pos=len; lr_refresh(prompt,buf,len,pos); }
                }
            } else if (lr_vi_mode) {
                
                unsigned char vcmd = seq[0];
                int vi_ins = 0;   

                if (pos > 0) { pos--; lr_refresh(prompt, buf, len, pos); }

                for (;;) {
                    if (vi_ins) break;
                    switch (vcmd) {
                    
                    case 'h': case 127:
                        if (pos > 0) { pos--; lr_refresh(prompt,buf,len,pos); }
                        break;
                    case 'l': case ' ':
                        if (pos < len - 1) { pos++; lr_refresh(prompt,buf,len,pos); }
                        break;
                    case '0':
                        pos = 0; lr_refresh(prompt,buf,len,pos);
                        break;
                    case '$':
                        pos = len > 0 ? len - 1 : 0;
                        lr_refresh(prompt,buf,len,pos);
                        break;
                    case 'w': {
                        while (pos < len && buf[pos] != ' ') pos++;
                        while (pos < len && buf[pos] == ' ') pos++;
                        if (pos > len - 1 && len > 0) pos = len - 1;
                        lr_refresh(prompt,buf,len,pos);
                        break;
                    }
                    case 'b': {
                        if (pos > 0) pos--;
                        while (pos > 0 && buf[pos] == ' ') pos--;
                        while (pos > 0 && buf[pos-1] != ' ') pos--;
                        lr_refresh(prompt,buf,len,pos);
                        break;
                    }
                    case 'e': {
                        if (pos < len - 1) pos++;
                        while (pos < len - 1 && buf[pos] == ' ') pos++;
                        while (pos < len - 1 && buf[pos+1] != ' ') pos++;
                        lr_refresh(prompt,buf,len,pos);
                        break;
                    }
                    
                    case 'k': goto hist_prev;
                    case 'j': goto hist_next;
                    
                    case 'x':
                        if (pos < len) {
                            memmove(buf+pos, buf+pos+1, (size_t)(len-pos-1));
                            len--; buf[len] = '\0'; modified = 1;
                            if (pos >= len && pos > 0) pos = len - 1;
                            lr_refresh(prompt,buf,len,pos);
                        }
                        break;
                    case 'X':
                        if (pos > 0) {
                            memmove(buf+pos-1, buf+pos, (size_t)(len-pos));
                            len--; pos--; buf[len] = '\0'; modified = 1;
                            lr_refresh(prompt,buf,len,pos);
                        }
                        break;
                    case 'r': {
                        unsigned char rc;
                        if (read(0, &rc, 1) > 0 && rc >= 32 && rc < 127 && pos < len) {
                            buf[pos] = (char)rc; modified = 1;
                            lr_refresh(prompt,buf,len,pos);
                        }
                        break;
                    }
                    case 'd': {
                        unsigned char dm;
                        if (read(0, &dm, 1) <= 0) break;
                        if (dm == 'd') {
                            len = pos = 0; buf[0] = '\0'; modified = 1;
                            lr_refresh(prompt,buf,len,pos);
                        } else if (dm == '$') {
                            len = pos; buf[len] = '\0'; modified = 1;
                            lr_refresh(prompt,buf,len,pos);
                        } else if (dm == '0') {
                            memmove(buf, buf+pos, (size_t)(len-pos));
                            len -= pos; buf[len] = '\0'; pos = 0; modified = 1;
                            lr_refresh(prompt,buf,len,pos);
                        } else if (dm == 'w') {
                            int start = pos;
                            while (pos < len && buf[pos] != ' ') pos++;
                            while (pos < len && buf[pos] == ' ') pos++;
                            memmove(buf+start, buf+pos, (size_t)(len-pos));
                            len -= pos - start; buf[len] = '\0'; pos = start; modified = 1;
                            lr_refresh(prompt,buf,len,pos);
                        }
                        break;
                    }
                    case 'c': {
                        unsigned char cm;
                        if (read(0, &cm, 1) <= 0) break;
                        if (cm == 'c') {
                            len = pos = 0; buf[0] = '\0'; modified = 1;
                            lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        } else if (cm == '$') {
                            len = pos; buf[len] = '\0'; modified = 1;
                            lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        } else if (cm == 'w') {
                            int start = pos;
                            while (pos < len && buf[pos] != ' ') pos++;
                            memmove(buf+start, buf+pos, (size_t)(len-pos));
                            len -= pos - start; buf[len] = '\0'; pos = start; modified = 1;
                            lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        }
                        break;
                    }
                    case 'D':
                        len = pos; buf[len] = '\0'; modified = 1;
                        lr_refresh(prompt,buf,len,pos);
                        break;
                    case 'C':
                        len = pos; buf[len] = '\0'; modified = 1;
                        lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        break;
                    case 'i': vi_ins = 1; break;
                    case 'I':
                        pos = 0; lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        break;
                    case 'a':
                        if (pos < len) pos++;
                        lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        break;
                    case 'A':
                        pos = len; lr_refresh(prompt,buf,len,pos); vi_ins = 1;
                        break;
                    case '\r': case '\n':
                        write(1, "\r\n", 2);
                        goto done;
                    default: break;
                    }
                    if (vi_ins) break;
                    if (read(0, &vcmd, 1) <= 0) goto done;
                }
                
            }
            
            break;
        }

        default:
            if (c >= 32 && c < 127 && len < LBUF-1) {
                memmove(buf+pos+1, buf+pos, (size_t)(len-pos));
                buf[pos] = (char)c; len++; pos++; buf[len] = '\0';
                modified = 1; lr_refresh(prompt, buf, len, pos);
            }
            break;
        }
    }

done:
    lr_leave_raw();
    buf[len] = '\0';
    if (len > 0) lr_hist_push(buf);
    return buf;
}

#endif /* NO_HISTORY */
