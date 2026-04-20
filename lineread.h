#ifndef LINEREAD_H
#define LINEREAD_H

extern int lineread_enabled;

void  lineread_init(void);
void  lineread_set_mode(int vi);
char *lineread(const char *prompt);
void  lineread_hist_load(const char *file);
void  lineread_hist_save(void);
void  lineread_cleanup(void);
void  lineread_hist_push(const char *line);

int          lineread_hist_len(void);
const char  *lineread_hist_entry(int idx);

#endif
