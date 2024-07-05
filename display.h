#ifndef _DISPLAY_H_INCLUDED
#define _DISPLAY_H_INCLUDED

void disp_status(int chnum, char *stringp);
void disp_statusf(int chnum, const char *fmt, ...);
void disp_err(int chnum, int chfd, int state);
void disp_msg(char *string);
void disp_msgf(const char *fmt, ...);
void disp_init();

#endif

