#ifndef _DIALER_LIST_H
#define _DIALER_LIST_H
#include <stdbool.h>

bool dialer_list_load(const char *path);
const char *dialer_list_next();
void dialer_list_cleanup();

#endif