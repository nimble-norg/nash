#ifndef LINEREAD_H
#define LINEREAD_H

extern int lineread_enabled;

void  lineread_init(void);
char *lineread(const char *prompt);
void  lineread_hist_load(const char *file);
void  lineread_hist_save(void);
void  lineread_cleanup(void);

#endif
