/**********@@@SOFT@@@WARE@@@COPY@@@RIGHT@@@**********************************
* DIALOGIC CONFIDENTIAL
*
* Copyright (C) 1990-2007 Dialogic Corporation. All Rights Reserved.
* The source code contained or described herein and all documents related
* to the source code ("Material") are owned by Dialogic Corporation or its
* suppliers or licensors. Title to the Material remains with Dialogic Corporation
* or its suppliers and licensors. The Material contains trade secrets and
* proprietary and confidential information of Dialogic or its suppliers and
* licensors. The Material is protected by worldwide copyright and trade secret
* laws and treaty provisions. No part of the Material may be used, copied,
* reproduced, modified, published, uploaded, posted, transmitted, distributed,
* or disclosed in any way without Dialogic's prior express written permission.
*
* No license under any patent, copyright, trade secret or other intellectual
* property right is granted to or conferred upon you by disclosure or delivery
* of the Materials, either expressly, by implication, inducement, estoppel or
* otherwise. Any license under such intellectual property rights must be
* express and approved by Dialogic in writing.
*
***********************************@@@SOFT@@@WARE@@@COPY@@@RIGHT@@@**********/

#include <stdio.h>
#include <errno.h>
#include <curses.h>
#include <srllib.h>
#include <stdarg.h>
#include <string.h>
#include <dxxxlib.h>
#include <unistd.h>
#include <pthread.h>
#include "display.h"
#include "answer.h" // I'm not proud of this. This is only so altsig can be used; please replace with a smaller header file.
// We don't need all this other garbage bloating the display object file.

/* Screen co_ordinate offsets for window 2 */

#define CH_Y    3         /* Y offset for CH %d: message */
#define CH_X    3         /* X offset for CH %d: message */
#define MSG_Y   1         /* Y offset for primary message */
#define MSG_X   3         /* X offset for primary message */
#define OUT_Y   CH_Y      /* Y offset for channel message */
#define OUT_X   CH_X + 7  /* X offset for channel message */
#define ERR_Y   OUT_Y     /* Y offset for channel error message */
#define ERR_X   35        /* X offset for channel error message */

WINDOW *win1;
WINDOW *win2;

extern int errno;
extern short maxchans;

static char version[] = "13.37ST";
static char dialogic[] =
    "TP's Awesome Sooper Dooper Phone Thing";


static pthread_mutex_t g_UiLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_UiThread;
static int g_SelectedChan = 1;
static char g_ChanMessage[256][48] = { 0 };


static void update_chanstatus(int chnum) {
    pthread_mutex_lock(&g_UiLock);

    wmove(win2, OUT_Y + chnum - 1, OUT_X);
    wclrtoeol(win2);
    box(win2, ACS_VLINE, ACS_HLINE);

    if (chnum == g_SelectedChan) {
        wattron(win2, A_REVERSE);
    }
    
    mvwaddstr(win2, OUT_Y + chnum - 1, OUT_X, g_ChanMessage[chnum]);
    
    if (chnum == g_SelectedChan) {
        wattroff(win2, A_REVERSE);
    }
    
    wrefresh(win2);
   
    pthread_mutex_unlock(&g_UiLock);
}

static void ui_switch_selected(int new) {
    int old = g_SelectedChan;

    g_SelectedChan = new;
    
    // Refresh the UI, to turn the reverse video on the old one off,
    // and the reverse video on the new one on.
    update_chanstatus(old);
    update_chanstatus(new);
}

static void *ui_thread_callback(void *_unused_param) {
    int ch;
    //return NULL;
    while (true) {
        ch = getch();
        
        switch (ch) {
            case KEY_UP:
                if (g_SelectedChan > 1) {
                    ui_switch_selected(g_SelectedChan - 1);
                }
                break;
            case KEY_DOWN:
                if (g_SelectedChan < 24) {
                    ui_switch_selected(g_SelectedChan + 1);
                }
                break;
            case 'd':
                disp_msgf("here is where I would have dropped the call on channel %d", g_SelectedChan);
                break;
        }
    }
    
    return NULL;
}

/***************************************************************************
 *        NAME: disp_init()
 *      INPUTS: none
 * DESCRIPTION: Initialize display for this program
 *************************************************************************/
void disp_init() {
#ifndef NO_CURSES
    int i;

    /* Start curses and clear the screen */
    initscr();
    cbreak();
    keypad(stdscr, true);
    noecho();

    clear();
    refresh();

    /* Create windows */
    win1 = newwin(4, 80, 0, 0);
    win2 = newwin(40, 80, 4, 0);
    box(win1, ACS_VLINE, ACS_HLINE);
    box(win2, ACS_VLINE, ACS_HLINE);
    mvwprintw(win1, 1, 5, "%s Ver %s", dialogic, version);
    mvwaddstr(win1, 2, 5, "Copyright (c) 2024 Shadytel Development Co. Ltd. All Rights Reserved");
    
    mvwprintw(win2, 30, 5, "keys (selected channel): d - drop call");
    wrefresh(win1);
    // wrefresh(win2);
    // sleep(1);

    for (i = 0; i < maxchans; i++) {
        mvwprintw(win2, CH_Y + i, CH_X, "CH %2d:", i + 1);
    }

    wrefresh(win2);

    // Start up the UI Input thread
    pthread_create(&g_UiThread, NULL, ui_thread_callback, NULL);
#else
    printf("%s Ver %s\n", dialogic, version);
    printf("Copyright (c) 2018 Shadytel Development Co. Ltd. All Rights Reserved.\n\n");
#endif
    return;
}


/***************************************************************************
 *        NAME: disp_status(chnum, stringp)
 *      INPUTS: chno - channel number (1 - 12)
 *              stringp - pointer to string to display
 * DESCRIPTION: display the current activity on the channel in window 2
 *              (the string pointed to by stringp) using chno as a Y offset
 *************************************************************************/
void disp_status(int chnum, char *stringp)
{
#ifndef NO_CURSES
//   char * disp; // This variable is unused, so it has been removed

    /* Use chnum as an offset for the screen Y co-ordinate */
    if (chnum < 0 || chnum > maxchans) {
        /* display disabled if more than MAXCHANS channels are being used */
        return;
    }
    
    strncpy(g_ChanMessage[chnum], stringp, 256);
    update_chanstatus(chnum);

#else
    printf("Channel %d: %s\n", chnum, stringp);
#endif
}

void disp_statusf(int chnum, const char *fmt, ...) {
    char buffer[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, 256, fmt, args);

    disp_status(chnum, buffer);

    va_end(args);
}


/***************************************************************************
 *        NAME: disp_msg(stringp)
 *      INPUTS: stringp - pointer to string to display.
 * DESCRIPTION: display the string passed, in the primary message area of win2
 *************************************************************************/
void disp_msg(char *stringp)
{
    if (stringp == NULL) {
        return;
    }

    if ((altsig & 2) && (debugfile)) {
        fprintf(debugfile, "%s\n", stringp);
        fflush(debugfile);
    }

#ifndef NO_CURSES
    /* clear current line and display new line */
    wmove(win2, MSG_Y, MSG_X);
    wclrtoeol(win2);
    box(win2, ACS_VLINE, ACS_HLINE);
    mvwaddstr(win2, MSG_Y, MSG_X, stringp);
    wrefresh(win2);
#else
    printf("%s\n", stringp);
#endif
}
/*
void disp_statusf(int channum, const char *fmt, ...) {
    char buffer[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, 256, fmt, args);
    disp_status(channum, buffer);

    va_end(args);
    return;
}
*/
void disp_msgf(const char *fmt, ...) {
    char buffer[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, 256, fmt, args);

    disp_msg(buffer);

    va_end(args);
}


/***************************************************************************
 *        NAME: disp_err( chnum, chfd, state )
 * DESCRIPTION: This routine prints error information.
 *      INPUTS: chnum - channel number (1 - 12)
 *      chfd - channel descriptor
 *     OUTPUTS: The error code and error message are displayed
 *    CAUTIONS: none.
 ************************************************************************/
void disp_err(int chnum, int chfd, int state)
{
    long lasterr = ATDV_LASTERR(chfd);

#ifndef NO_CURSES

    if (lasterr == EDX_SYSTEM) {
        mvwprintw(win2, ERR_Y + chnum - 1, ERR_X,
                  "ERROR: errno %d, State = %d", errno, state);
    } else {
        mvwprintw(win2, ERR_Y + chnum - 1, ERR_X,
                  "ERROR: lasterr 0x0%x, State = %d", lasterr, state);
    }

    wrefresh(win2);
#else

    if (lasterr == EDX_SYSTEM) {
        printf("Channel %d: ERROR: errno %d, State = %d\n",
               chnum, errno, state);

        if (altsig & 2) {
            fprintf(debugfile, "Channel %d: ERROR: errno %d, State = %d\n", chnum, errno, state);
            fflush(debugfile);
        }
    }

    if (lasterr != EDX_SYSTEM) {
        printf("Channel %d: ERROR: lasterr 0x0%x, State = %d\n",
               chnum, lasterr, state);

        if (altsig & 2) {
            fprintf(debugfile, "Channel %d: ERROR: lasterr 0x0%x, State = %d\n", chnum, lasterr, state);
            fflush(debugfile);
        }
    }

#endif
    return;
}

