#include "dialer_list.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char **g_dialerList = NULL;
static size_t g_dialerListCapacity = 0;
static size_t g_dialerListSize = 0;
static size_t g_dialerListIdx = 0;

static void dialer_list_append(const char *number) {
    /* Initial allocation */
    if (g_dialerList == NULL) {
        g_dialerList = (char **) calloc(16, sizeof(char *));
    } else if ((g_dialerListSize + 1) < g_dialerListCapacity) {
        g_dialerListCapacity *= 2;
        g_dialerList = (char **) realloc(g_dialerList, sizeof(char *) * g_dialerListCapacity);
    }

    /* Bad alloc */
    if (g_dialerList == NULL) {
        // Abort? lol
    }

    g_dialerList[g_dialerListSize] = strdup(number);
    g_dialerListSize++;
}

bool dialer_list_load(const char *path) {
    FILE *fp;
    char buffer[1024];
    bool error = false;
    int nRead;

    if ((fp = fopen(path, "r")) == NULL) {
        return false;
    }

    while (fgets(buffer, 1024, fp) != NULL) {
        nRead = strlen(buffer);

        /* Trim off any terminating newline/whitespace */
        while (isspace(buffer[nRead - 1])) {
            buffer[nRead - 1] = 0;
            nRead--;
        }

        dialer_list_append(buffer);
    }

    if (ferror(fp)) {
        error = true;
    }

    fclose(fp);

    return !error;
}

const char *dialer_list_next() {
    const char *number;

    if (g_dialerListIdx == g_dialerListSize) {
        return NULL;
    }

    number = g_dialerList[g_dialerListIdx];
    g_dialerListIdx++;

    return number;
}

void dialer_list_cleanup() {
    size_t i;

    if (g_dialerList != NULL) {
        for (i = 0; i < g_dialerListSize; i++) {
            free(g_dialerList[i]);
        }

        free(g_dialerList);
    }
}

/*
int main(int argc, char *argv[]) {
    if (!dialer_list_load("dialer_test_list.txt")) {
        perror("dialer_list_load()");
        return 1;
    }

    const char *c;

    while ((c = dialer_list_next()) != NULL) {
        printf("%s\n", c);
    }

    dialer_list_cleanup();

    return 0;
}*/
