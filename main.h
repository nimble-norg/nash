/*
 * Copyright (C) 1989 by Kenneth Almquist.  All rights reserved.
 * This file is part of ash, which is distributed under the terms specified
 * by the Ash General Public License.  See the file named LICENSE.
 */

#ifndef MAIN_H
#define MAIN_H

extern int rootpid;		/* pid of main shell */
extern int rootshell;		/* true if we aren't a child of the main shell */

void readcmdfile(const char *);
void cmdloop(int);

#endif
