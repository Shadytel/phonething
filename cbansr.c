/*********************************
*  _____
* /_,-,_\
*  / @ \   [  s h a d y t e l  ]
* +-----+
*      In spirit of revenue
**********************************/

/*
* Wishlist of features:
* - IVR-adjustable CPN
* - Eventual support for Asterisk-esque function parsing in the config file(s)
* - Less hardcoded crap. Seriously. The config file is your friend.
* - JCT FXS support via robbed bit T1 configuration. This has been moved to another project, and should be merged eventually.
* - More ISDN IE support within parser; currently has a limited number.
* - E1 support
* - Add time stretching capabilities to the media playback application. Seriously - that shit is funny.
* - Add JCT T1 support to dialer
* - Try to figure out how to change JCT tone qualification templates? The Sangoma monkeys claim if you pay for a maintenance contract, they *might* be able to fix it. This feature doesn't seem to exist; they might
*   be confusing it for PAMD/PVD/etc qualification adjustment. As it is, JCT single tone reception performance is unacceptable, so this is worth trying to look for.
* - Dynamic Project Upstage dialplan config options
* - Multiple boards can be supported presently, but only of a single architecture/media type. There should be a way to handle analog/ISDN/RBS T1/DM3/media-only boards all on the same executable.
*
* Wishlist of code improvements:
* - Chage the dialer to pause on the starting channel after releasing the starting call, so it can be used for dialing instead of sitting idly.
* - Remove the debug logger from the altsig bitmask and instead assign a null pointer to it upon declaration; the bitmask is unnecessary at this point, since we're checking for a null pointer anyway.
* - Consolidate a bunch of this crap into functions instead of repeating code
* - Some day there will need to be discrete event handlers for different functions, so the code is a little more legible
* - Have a dedicated quit routine; if the program exits uncleanly, it currently relies on the compiler to do garbage collection. That, sir, is bad news bears.
* - Eventually break apart into multiple C files? This file alone is huge of code now.
* - Go back and add error handlers to early code portions that do not have them; some of this was fixed, but not all.
* - The winkstart, uh, "stack" for JCT stuff was done a million years ago and should be replaced. Or at least get rid of the function that indefinitely waits for distant winkback.
* - Extremely fast paced (like, 8/+ per second) ISDN call reception/origination activity will freak JCT cards the fuck out, and they tend to crash the host process.
*   Meet those poor little 486es on the board half-way. <-- Was this fixed? Verify.
* - Audit IE reception function security/stability with out of spec behavior. For one, note the length of the information element buffer and make sure the parser never, ever exceeds it.
* - Reduce total length of called/calling party number buffers to 261 (LAPD frame max size + 1). The board won't retrieve out of spec numbers anyway. Actually, first, make sure q.931 setup messages can't span
*   multiple LAPD frames.
*/

#include "common.h"
/*
 * System Header Files
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

/**
 Dialogic Header Files
 **/
#include <srllib.h>
#include <dxxxlib.h>
#include <dtilib.h>
#include <msilib.h> // This is stupid, but necessary for conferencing. Iirc, it was first on FXS boards. Someday we might as well support the FXS boards.
#include <dcblib.h>

/**
 ISDN Header Files
 **/
#include <gclib.h>
#include <gcisdn.h>

/**
 SQL Header Files
 **/
#include <sqlite3.h>


#include "sctools.h"

#include "answer.h"
#include "display.h"
#include "configuration.h"
#include "sql.h"
#include "dialer_list.h"

/*
 * Cast type for event handler functions to avoid warning from SVR4 compiler.
 */
typedef long int (*EVTHDLRTYP)();

/**
 ** Global data
 **/

/*
 * File Descriptors for VOX Files
 */
//int invalidfd;
int goodbyefd;
//int errorfd;
int end = 0;

/*
 * Sqlite descriptors
 */
//sqlite3 * activationdb;

short maxchans = 24;        /* Default Number of D/4x Channels to use */
int frontend = CT_GCISDN;   /* Default network frontend is ISDN */
int scbus    = TRUE;        /* Default Bus mode is SCbus */
int routeag  = FALSE;       /* Route analog frontend to resource ??? */

// char connchan[MAXCHANS+1];
static short anncnum[ MAXCHANS + 1 ];
static short anncnum2[ MAXCHANS + 1 ];
static short minannc[ MAXCHANS + 1 ];
static short maxannc[ MAXCHANS + 1 ];
static short newmsg[ MAXCHANS + 1 ];
static short oldmsg[ MAXCHANS + 1 ];
DV_TPT dialtpt[ 1 ];
static char bookmark[ MAXCHANS + 1 ][ 5 ];
static unsigned char errcnt[ MAXCHANS + 1 ]; /* Error counter */
static unsigned char ownies[ MAXCHANS + 1 ];
static unsigned char termmask[ MAXCHANS + 1] = {0}; // By popular request, a bitmask to tell the software how to clean up the call!
static unsigned short filecount[ MAXCHANS + 1 ];
static char filerrcnt[ MAXCHANS + 1 ];
//static char filetmp[ MAXCHANS + 1 ][ MAXMSG + 1 ];
//static char filetmp2[ MAXCHANS + 1 ][ MAXMSG + 1 ];
static char filetmp3[ MAXCHANS + 1 ][ MAXMSG + 1 ];
static char readback[ MAXCHANS + 1 ][ 7 ]; // For ANAC/other readback functions
static char passcode[ MAXCHANS + 1 ][ 7 ];
static char causecode[MAXCHANS + 1];
static int confid[2]; // For conference
static bool conf = 0;
//unsigned char participants = 0; // For conference
static unsigned char msgnum[ MAXCHANS + 1 ];
static unsigned long playoffset[ MAXCHANS + 1 ];
static unsigned long trollconf_offset;
static unsigned char ligmain [ MAXCHANS + 1 ]; /* Nowhere on the variable does it say I wrote this code! It just has my name on the code! */
static unsigned char lig_if_followup [ MAXCHANS + 1 ]; // Can probably get rid of this, replace with ligmain
static bool loggedin[ MAXCHANS + 1 ]; // Yeah, a single bit is all that stands between you and being owned. You feel secure now?
static char vmattrib[ MAXCHANS + 1 ]; // Bitmask for voicemail status
static bool userposts[ MAXCHANS + 1 ];
static char time1[ MAXCHANS + 1 ];
static char time2[ MAXCHANS + 1 ];
//FILE *resumefile[ MAXCHANS + 1 ];
FILE *passwordfile[ MAXCHANS + 1 ];
//static int multiplay[MAXCHANS + 1][6];
static unsigned char closecount[ MAXCHANS ];
static int file[25]; // For readback function.

static struct channel_info {
    struct {
        bool using_list;
    } dialer;
} chaninfo[MAXCHANS + 1];

# define TID_1 101 // Temporary tone alias for bridge greeter
# define TID_MODEM 102 // For dialer
# define TID_MODEM2 103 // For dialer
# define TID_FAX   104 // For dialer
# define TID_MBEEP_1000 105 // For dialer (1000 hertz message tone)
# define TID_MBEEP_440  106 // For dialer (440 hertz message tone)
# define TID_MBEEP_790  107 // For dialer (790 hertz/Mitel/some Toshiba systems message tone)
# define TID_MBEEP_950  108 // For dialer (950 hertz/older IP Office/Rolm Phonemail message tone)
# define TID_MBEEP_2000 109 // For dialer (2000 hertz/Shoretel/weird AM message tone)
# define TID_MBEEP_500  110 // For dialer (500 hertz/Nortel message tone)
# define TID_MBEEP_1400 111 // For dialer (1400 hertz/Glenayre message tone)
# define TID_MBEEP_1330  112 // For dialer (1330 hertz/GTE Glenayre message tone)
# define TID_MBEEP_800  113 // For dialer (800 hertz/Express Messenger/newer Nupoint/Compilot message tone)
# define TID_MBEEP_425 114 // For dialer (425 hertz/Cisco Unity message tone)
# define TID_MBEEP_745  115 // For dialer (745 hertz/Cogecovision cablephones)
# define TID_MBEEP_850  116 // For dialer (850 hertz/Anypath/non-Dialogic Audix message tone)
# define TID_MBEEP_1050 117 // For dialer (1050 hertz/Middle-aged Panasonic message tone)
# define TID_2600 118 // For Project Upstage, the world's pettiest 2600 wink-start/R1 emulation

# define RANDOM 0 // For ISDN callout with random ANI

#define NOREC 0
#define REC 1
#define ONEWAY 2


#define CONF_PARTIES 15 // This is to support all DM3 media loads; the absolute minimum on the first gen cards is 15, so we're sticking with that.
// Since we have two conferences in the build right now, that might eventually bite us in the ass. The software should query the card for capacity.
int confchan[2][CONF_PARTIES];
int confattr = MSPA_MODEFULLDUPLX | MSPA_NOAGC; // Conference user attributes

struct stat sts;
time_t rawtime;
struct tm *timeinfo;

// For dialer code:

char dialerdest[MAXCHANS + 1][18];
char pubdest[MAXCHANS + 1][18];
unsigned char length[ MAXCHANS ]; // I'll make this multi-channel for now, though it doesn't technically need to be.
unsigned char chans[ MAXCHANS ]; // Do we need to allocate this? Or more than 255 channels for that matter?
unsigned char initchan[ MAXCHANS ];
int scancount[ MAXCHANS ];
FILE *logdescriptor;

// TO DO: Struct for dialer variables?

FILE *calllog; // For ISDN traffic

//configuration_t config;


// Function declarations

bool establishconf(int confchan1, int confchan2, unsigned char confnum);
char set_rdnis(short channum, char *number, char reason);
char isdn_prep(int maxchans);
int bddev[24];
bool callprog(short channum, bool setting);
int isdn_answer(short channum);
char isdn_drop(short channum, int cause);
int isdn_waitcall(short channum);
char makecall(short channum, char *destination, char *callingnum, char rec);
int set_hkstate(short channum, int state);
int play(short channum, int filedesc, int format, unsigned long offset, char options);
int playtone_cad(short channum, int toneval, int toneval2, int time);
int playtone_rep(short channum, int toneval, int toneval2, int amp1, int amp2, int ontime, int pausetime);
extern __sighandler_t sigset(int __sig, __sighandler_t __disp) __THROW;
char set_cpname(short channum, char *name, char *callingnum);
int endwin(void);
bool routed_cleanup(short channum);
void file_error(short channum, char *file_name);
short get_channum_ts(long sctimeslot);
void isdn_close(int maxchan);

// SQL callback function declarations

int userpass_cb(void * chanptr, int argc, char **argv, char **coldata);
int count_cb(void * chanptr, int argc, char **argv, char **coldata);
int admincount_cb(void * chanptr, int argc, char **argv, char **coldata);
int adminadd_cb(void * chanptr, int argc, char **argv, char **coldata);
int npa_cb(void * chanptr, int argc, char **argv, char **coldata);
/*
 * Externals
 */
extern char *optarg;    /* Pointer to Argument Parameter (getopt)  */
extern int  optind;     /* Next Index into argv[] (getopt)     */

//#define random_at_most(x) (rand() % (x))

short idle_trunkhunt( short channum, short low, short high, bool sigreset ) {
    short counter = low;
    //participants = 0; // How the fuck did this get in here?
    
    loop:
    while( (dxinfox[counter].state != ST_WTRING) && (counter <= high) ){
        counter++;
    }
    
    if ( counter % 24  == 0) {
        counter++;
        goto loop; 
    }// Don't use D-channel plz.
    // That being said, we shouldn't have to evaluate this every single time
    
    if( counter > high ) {
        ownies[channum] = 0;
        // In cases where we're expecting MF, we should reset the digit receiver back to DTMF if the request failed.
        if (sigreset) dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset receivers back to DTMF
        dxinfox[ channum ].state = ST_ISDNACB;
        dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);
        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1 ) {
            set_hkstate( channum, DX_ONHOOK);
        }
        
        disp_msg("Error: all circuits are busy");
        return -1;
    }
    
    return counter;
}

char routecall ( short channum, short startchan, short endchan, char * destination, char * callingnum, bool rec) {
        dxinfox[ channum ].state = ST_ROUTED; // Do this first; we don't want it selecting a channel it erroneously thinks is idle

        while (dxinfox[startchan].state > ST_WTRING) {
            startchan++;
            // Don't place a call on the D channel plz :(
            if (startchan == 24 || startchan == 48 || startchan == 72 || startchan == 96) {
                startchan++;
                continue;
            }
        }

        disp_msgf("Dest. channel is %d", startchan);

        if (startchan > endchan) {
            // Error handling for all circuits being busy
            startchan = 0;
            dxinfox[ channum ].state = ST_ISDNACB;
            dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                set_hkstate(channum, DX_ONHOOK);
            }

            disp_msg("Error: all circuits are busy");
            return (-1);
        }

        disp_status(channum, "Establishing tandem call");
        connchan[channum] = startchan;
        connchan[startchan] = channum;
        if(rec) {
            sprintf(dxinfox[ startchan ].msg_name, "%s-%s-%lu.pcm", destination, callingnum, (unsigned long)time(NULL));
            dxinfox[ startchan ].msg_fd = open(dxinfox[ startchan ].msg_name, O_RDWR | O_TRUNC | O_CREAT, 0666);
            return( makecall(startchan, destination, callingnum, TRUE) );
        }
        
        else return (makecall(startchan, destination, callingnum, FALSE) );
}

char voicemail_xfer( int channum, char *extension ) {
    if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
    }

        anncnum[ channum ] = 0;
        filecount[ channum ] = 0;
        ownies[channum] = 0; // This is our quick and dirty variable for on-hook message handling.
        //There's a better way to do this (^) so, well, do it. Later, I mean. Never now.
        disp_status(channum, "Accessing voicemail...");
        sprintf(filetmp[channum], "sounds/vmail/%s", extension);  // Stick the path for the VMB somewhere useful

        if (stat(filetmp[channum], &sts) == -1) {
            mkdir(filetmp[channum], 0700);    // Create the directory if it's not there
        }

        sprintf(filetmp2[channum], "%s/old", filetmp[channum]);

        if (stat(filetmp2[channum], &sts) == -1) {
            mkdir(filetmp2[channum], 0700);    // Does the old directory exist?
        }

        sprintf(filetmp2[channum], "%s/new", filetmp[channum]);

        if (stat(filetmp2[channum], &sts) == -1) {
            mkdir(filetmp2[channum], 0700);    // Does the new directory exist?
        }

        sprintf(filetmp2[channum], "%s/temp", filetmp[channum]);

        if (stat(filetmp2[channum], &sts) == -1) {
            mkdir(filetmp2[channum], 0700);    // Make sure the temporary directory exists too.
        }

        sprintf(filetmp3[channum], "%s/attrib", filetmp[channum]);

        sprintf(dxinfox[ channum ].msg_name, "%s/greeting.pcm", filetmp[channum]);

        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
            sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/generic_greeting.pcm");
        }

        if ((passwordfile[ channum ] = fopen(filetmp3[ channum ], "r")) != NULL) {
            fscanf(passwordfile[ channum ], "%s %c", passcode[ channum ], &vmattrib[ channum ]);
            fclose(passwordfile[ channum ]);
        }

        dxinfox[ channum ].state = ST_VMAIL1;
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
        }

        return (0);

}

int set_confparm(int dev, unsigned char parm, int val) {
    // This is simply a wrapper for dcb_setbrdparm so instead of expecting a pointer, we can simply send the function an immediate value.
    return (dcb_setbrdparm(dev, parm, &val));
}

void file_error(short channum, char *file_name) {
    disp_msgf("Cannot open %s (%s) for playback/recording, state %d", dxinfox[ channum ].msg_name, file_name, dxinfox[ channum ].state);
    dxinfox[channum].state = ST_GOODBYE;
    play(channum, errorfd, 0, 0, 1);
    return;
}

int transaction_rec(short channum1, short channum2, int filedesc) {

    DX_IOTT iott[1];
    DX_XPB  xpb[1];
    DV_TPT  tpt[1];
    // Can we just do a null DV_TPT?
    long rec_timeslots;
    long rec_tsarray[2];
    SC_TSINFO rec_tsinfo;
    rec_tsinfo.sc_numts = 1;
    rec_tsinfo.sc_tsarrayp = &rec_timeslots;

    iott[0].io_fhandle = filedesc;
    iott[0].io_offset = 0;
    iott[0].io_length = -1;
    iott[0].io_type = IO_DEV | IO_EOT;

    xpb[0].wFileFormat = FILE_FORMAT_VOX;
    xpb[0].wDataFormat = DATA_FORMAT_MULAW;
    xpb[0].nSamplesPerSec = DRT_8KHZ;
    xpb[0].wBitsPerSample = 8;

    tpt[0].tp_type = IO_EOT;
    tpt[0].tp_termno = DX_LCOFF;
    tpt[0].tp_length = 1;
    tpt[0].tp_flags = TF_LCOFF;

    if (dt_getxmitslot(dxinfox[ channum1 ].tsdev, &rec_tsinfo) == -1) {
        disp_err(channum1, dxinfox[ channum1 ].tsdev, dxinfox[ channum1 ].state);
        return (-1);
    }

    rec_tsarray[0] = rec_timeslots;

    if (dt_getxmitslot(dxinfox[ channum2 ].tsdev, &rec_tsinfo) == -1) {
        disp_err(channum2, dxinfox[ channum2 ].tsdev, dxinfox[ channum1 ].state);
        return (-1);
    }

    rec_tsarray[1] = rec_timeslots;

    rec_tsinfo.sc_numts = 2;
    rec_tsinfo.sc_tsarrayp = &rec_tsarray[0];

// Timeslot info struct should contain xmit timeslots for both channels
// The DV_TPT should specify just a DX_LCOFF parameter (or perhaps NULL)

// fill the SC_TSINFO structure with dt_getxmitslot() (or dx for analog) before calling the following functions:

// for Springware:

    if (!dm3board) {
// Check the voice_api_v2 documentation on dx_recm before using this garbage
        if (dx_recm(dxinfox[channum1].chdev, iott, tpt, MD_PCM | PM_SR8 | MD_NOGAIN | EV_ASYNC, &rec_tsinfo) == -1) {
            disp_err(channum1, dxinfox[ channum1 ].chdev, dxinfox[ channum1 ].state);
            return (-1);
        }
    }

    else {

        if (dx_mreciottdata(dxinfox[ channum1 ].chdev, iott, tpt, xpb, MD_NOGAIN | EV_ASYNC, &rec_tsinfo) == -1) {
            disp_err(channum1, dxinfox[ channum1 ].chdev, dxinfox[ channum1 ].state);
            return (-1);
        }
    }

    return (0);

}

const char *timeoutput(short channum) {
    time_t rawtime;
    struct tm *info;
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(filetmp3[channum], 30, "%x - %I:%M %p", info);
    return (filetmp3[channum]);
}

void vmtimeoutput(short channum) {
    time_t rawtime;
    struct tm *info;
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(filetmp3[channum], 3, "%H", info);

    if (filetmp3[channum][0] == 0x30) {
        time1[channum] = (filetmp3[channum][1] - 0x30);
    } else {
        time1[channum] = ((filetmp3[channum][1] - 0x30) | ((filetmp3[channum][0] - 0x30) << 4));
    }

    strftime(filetmp3[channum], 3, "%M", info);

    if (filetmp3[channum][0] == 0x30) {
        time2[channum] = (filetmp3[channum][1] - 0x30);
    } else {
        time2[channum] = ((filetmp3[channum][1] - 0x30) | ((filetmp3[channum][0] - 0x30) << 4));
    }

    return;
}


void confparse() {
    if (!config_parse_file("main.conf", &config)) {
        fprintf(stderr, "Unable to parse main.conf, using defaults...");
        config_load_defaults(&config);
    }
    else {
        disp_msgf("emtanon1 is %s", config.extensions.emtanon1);
        config_dump(&config);
    }
}


char playmulti(short channum, char numprompts, uint8_t format, int sounddescriptor[numprompts])
// Shouldn't we be using a BOOL instead and just returning TRUE or FALSE?
// That may be something to modify this for in the future. In the meantime,
// I'm just going to stick with the usual way of formatting things for this
// program. Too much TRUE/FALSE and 0/-1 interoperating just gets confusing.
{
    // Heh, this was _way_ smaller than I thought it'd be. This looks easy :D .

    /*
    FORMAT BITMASK:

    00000000
    ||||||||_________ 0 = No header, 1 = WAV header
    |||||||__________ 0 = 8 khz, 1 = 6 khz sample rate
    ||||||___________ 0 = Ignore, 1 = G.726 ADPCM
    |||||____________ 0 = Ignore, 1 = Dialogic ADPCM
    ||||_____________ 0 = Mu-law, 1 = 16-bit linear PCM (ignored if third/fourth most significant bit is 1, or not a DM3 card; 16-bit PCM is exclusive to them)
    |||______________ (nothing yet)
    ||_______________ (also nothing)
    |________________ 0 = Ignore touchtones, 1 = terminate on any touchtone reception
    */



    unsigned short fileformat;
    unsigned short dataformat;
    unsigned long samplerate;
    unsigned long bitspersample;

    bool touchtones = FALSE;

    if (format & 1) {
        fileformat = FILE_FORMAT_WAVE;
    } else {
        fileformat = FILE_FORMAT_VOX;
    }

    if (format & 2) {
        samplerate = DRT_6KHZ;
    } else {
        samplerate = DRT_8KHZ;
    }

    if (format & 4) {
        bitspersample = 4; // Sorry kids, we're just going with 4-bit G.726 . It's the most common, and all JCT has anyway.
        dataformat = DATA_FORMAT_G726;
    }

    else if (format & 8) {
        bitspersample = 4;
        dataformat = DATA_FORMAT_DIALOGIC_ADPCM;
    }

    else if (format & 16) {
        // Check to make sure it's a DM3 before proceeding
        bitspersample = 16;
        dataformat = DATA_FORMAT_PCM;
    }

    else {
        bitspersample = 8;
        dataformat = DATA_FORMAT_MULAW;
    }

    if (format & 128) {
        // Modify termination condition for touchtone reception
        touchtones = TRUE;
    }

    // Added one to the array to avoid an off-by-one overflow condition. Is this right, or are we needlessly allocating more?
    DX_IOTT multi_iott[(numprompts + 1)];
    DV_TPT multi_tptp[2];
    DX_XPB multi_xpbp[(numprompts + 1)];
    unsigned char currentprompt = 0;

    memset(&multi_iott, 0x00, sizeof(DX_IOTT));

    if (numprompts > 0) {
        closecount[channum] = numprompts;
        numprompts--;
    }

    else {
        closecount[channum] = 1;
    }

    if (numprompts == 1) {

        multi_iott[ currentprompt ].io_type = IO_DEV;
        multi_iott[ currentprompt ].io_fhandle = (int) sounddescriptor[ currentprompt ];
        multi_iott[ currentprompt ].io_offset = 0;
        multi_iott[ currentprompt ].io_length = -1;

        currentprompt++;

        multi_iott[ currentprompt ].io_type = IO_DEV | IO_EOT;
        multi_iott[ currentprompt ].io_fhandle = (int) sounddescriptor[ currentprompt ];
        multi_iott[ currentprompt ].io_offset = 0;
        multi_iott[ currentprompt ].io_length = -1;

    }

    if (numprompts == 0) {

        multi_iott[ currentprompt ].io_type = IO_DEV | IO_EOT;
        multi_iott[ currentprompt ].io_fhandle = (int) sounddescriptor[ currentprompt ];
        multi_iott[ currentprompt ].io_offset = 0;
        multi_iott[ currentprompt ].io_length = -1;

    }

    if (numprompts > 1) {

        // We should check the number of prompts just in case someone decides to pass something stupid to the function
        while (currentprompt < numprompts) {

            multi_iott[currentprompt].io_type = IO_DEV;
            multi_iott[currentprompt].io_fhandle = (int) sounddescriptor[ currentprompt ];
            multi_iott[currentprompt].io_offset = 0;
            multi_iott[currentprompt].io_length = -1;

            currentprompt++;
        }

        multi_iott[ currentprompt ].io_type = IO_DEV | IO_EOT;
        multi_iott[ currentprompt ].io_fhandle = (int) sounddescriptor[ currentprompt ];
        multi_iott[ currentprompt ].io_offset = 0;
        multi_iott[ currentprompt ].io_length = -1;

    }

    // This last one isn't part of the while loop since we need to indicate the end of the IOTT table.
    // This seems like the most efficient way to do that.

    memset(&multi_tptp, 0x00, (sizeof(DV_TPT) * 2));

    if (!touchtones) {
        multi_tptp[0].tp_type = IO_EOT;
    } else {
        multi_tptp[0].tp_type = IO_CONT;
    }

    multi_tptp[0].tp_termno = DX_LCOFF;
    multi_tptp[0].tp_length = 1;
    multi_tptp[0].tp_flags = TF_LCOFF;

    if (touchtones) {
        multi_tptp[1].tp_type = IO_EOT;
        multi_tptp[1].tp_termno = DX_MAXDTMF;
        multi_tptp[1].tp_length = 1;
        multi_tptp[1].tp_flags = TF_MAXDTMF;
    }

    memset(&multi_xpbp, 0x00, (sizeof(DX_XPB) * (numprompts + 1)));

    currentprompt = 0;

    while (currentprompt <= numprompts) {
        multi_xpbp[currentprompt].wFileFormat = fileformat;
        multi_xpbp[currentprompt].wDataFormat = dataformat;
        multi_xpbp[currentprompt].nSamplesPerSec = samplerate;
        multi_xpbp[currentprompt].wBitsPerSample = bitspersample;

        currentprompt++;
    }


    if (dx_playiottdata(dxinfox[ channum ].chdev, multi_iott, multi_tptp, multi_xpbp, EV_ASYNC) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
        return (-1);
    }

    return (0);
}


bool addtoconf(short channum, unsigned char confnum) {
    // Checking for -1 isn't necessary in this function, so we'll skip it. For now.
    SC_TSINFO tsinfo;
    long scts; // H.100 bus transmit slot
    MS_CDT cdt;

    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = &scts;

    if (dt_getxmitslot(dxinfox[channum].tsdev, &tsinfo) == -1) {
        disp_msg("dt_getxmitslot returned an error!");
        return (FALSE);
    }

    cdt.chan_num = (int)scts; // Why is this being cast as an integer? I mean, the manual said to do it...
    cdt.chan_sel = MSPN_TS;
    cdt.chan_attr = confattr;
    // cdt[confchan].chan_attr = MSPA_ECHOXCLEN | MSPA_MODEFULLDUPLX; // Turn echo cancellation on
    // Maybe some day we'll put something in here to specify whether or not to echo cancel

    if (dcb_addtoconf(confdev, confid[confnum], &cdt) == -1) {
        disp_msgf("Adding participant to conference failed! Error %s", ATDV_ERRMSGP(confdev));

        // Since we can't do this, we need to invoke nr_scroute to put the channel back where it
        // should be and play an error recording.
        if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
            disp_msg("Holy shit! SCroute_conf threw an error!");
            disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
        }

        dxinfox[ channum ].state = ST_GOODBYE;
        play(channum, errorfd, 0, 0, 1);
        return (FALSE);
    }

    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = (long *) &cdt.chan_lts;

    if (dt_listen(dxinfox[ channum ].tsdev, &tsinfo) == -1) {
        disp_msg("Add to conference timeslot routing failed!");

        if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
            disp_msg("Holy shit! SCroute_conf threw an error!");
            disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
        }

        dxinfox[ channum ].state = ST_GOODBYE;
        play(channum, errorfd, 0, 0, 1);
        return (FALSE);
    }

    if (isdninfo[ channum ].cpn[0] != 0x00)
        disp_statusf(channum, "Conf bridge %d | %s", confnum, isdninfo[ channum ].cpn);
    else disp_statusf(channum, "Conf bridge %d | Channel %d", confnum, participants[confnum]);

    return (TRUE);
}

bool conf_returntomoh( short channum, unsigned char confnum ) {

    if (dt_unlisten(dxinfox[ channum ].tsdev) == -1) {
        disp_msg("dt_unlisten for conference failed!");
        return (FALSE);
    }

    if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
        disp_msg("Holy shit! SCroute_conf threw an error!");
        disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
        return (FALSE);
    }

    disp_status(channum, "Conf bridge | Waiting for callers");

    anncnum[ channum ] = 0;
    errcnt[ channum ] = 1;

    while (errcnt[ channum ] == 1) {
        sprintf(dxinfox[ channum ].msg_name, "sounds/confhold/%d.pcm", anncnum[ channum ]);

        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
            errcnt[ channum ] = 0;
        } else {
            anncnum[ channum ]++;
        }
    }

    maxannc[ channum ] = anncnum[ channum ];
    anncnum[channum] = 0;
    multiplay[ channum ][0] = open("sounds/conf_onlyparty.pcm", O_RDONLY);

    if (maxannc[channum] == -1) {
        play(channum, multiplay[channum][0], 1, 0, 0);
        dxinfox[ channum ].state = ST_CONFWAITSIL;
    }

    else {
        srandom(time(NULL));
        dxinfox[ channum ].state = ST_CONFWAIT;
        sprintf(dxinfox[ channum ].msg_name, "sounds/confhold/%d.pcm", random_at_most(maxannc[channum]));
        multiplay[ channum ][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
        ownies[channum] = 15;
        playmulti(channum, 2, 128, multiplay[channum]);
    }

    confchan[confnum][0] = channum;

    return TRUE;

}

bool dropfromconf(short channum, unsigned char confnum) {
    SC_TSINFO tsinfo;
    long scts;
    MS_CDT cdt; // Can we allocate this crap later in the function?
    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = &scts;

    participants[confnum]--;

    // If someone is just waiting for the conf, no need to tear it down or whatever

    if ((dxinfox[ channum ].state == ST_CONFWAIT) || (dxinfox[ channum ].state == ST_CONFWAITSIL)) {
        return (TRUE);
    }

    /*
    // If someone is just waiting, we no longer have to route them back to the DSP; they're unrouted upon conference setup
    if  ( ( dxinfox[ channum ].state == ST_CONFWAIT ) || ( dxinfox[ channum ].state == ST_CONFWAITSIL ) ) {
         if ( nr_scroute( dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP ) == -1 ) {
              disp_msgf("Holy shit! SCroute_conf threw an error!");
              disp_err( channum, dxinfox[channum].chdev, dxinfox[channum].state );
              return(FALSE);
        }
        return(TRUE);
    }

    */

    if (dt_getxmitslot(dxinfox[channum].tsdev, &tsinfo) == -1) {
        disp_msg("dt_getxmitslot returned an error!");
        return (FALSE);
    }

    cdt.chan_num = (int)scts; // Why is this being cast as an integer? I mean, the manual said to do it...
    cdt.chan_sel = MSPN_TS;
    cdt.chan_attr = confattr; // Conference attributes are defined elsewhere.

    // Unroute timeslot from conference

    if (dt_unlisten(dxinfox[ channum ].tsdev) == -1) {
        disp_msg("dt_unlisten for conference failed!");
        return (FALSE);
    }

    // Return timeslot to voice device for normal operation

    if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
        disp_msg("Holy shit! SCroute_conf threw an error!");
        disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
        return (FALSE);
    }

    int partycount; // This is just here so the dcb_getcnflist() function has a place to stick its own count

    switch (participants[confnum]) {
        // Some really cranky compilers don't like case stacking. So, uhh, don't port this to some life hating 16-bit DOS machine :P .
        // case 1:
        case 0:
            if (dcb_delconf(confdev, confid[confnum]) == -1) {
                disp_msgf("Unable to delete conference bridge! Error %s", ATDV_ERRMSGP(confdev));
                return (FALSE);
            }

            break;

        case 1:
            if (dcb_remfromconf(confdev, confid[confnum], &cdt) == -1) {
                disp_msgf("Channel %d failed to be removed from conf! Error %s", channum, ATDV_ERRMSGP(confdev));
                return (FALSE);
            }
            if (confnum != 1) {
                // Conference 1 gets the silent treatment for now
                if (dcb_getcnflist(confdev, confid[confnum], &partycount, &cdt) == -1) {
                    disp_msg("ERROR: Couldn't get conference list in single party drop routine!");
                }
                else {
                    //disp_msgf("DEBUG: Last remaining conference participant exists on timeslot %d", get_channum_ts((long) cdt.chan_num));
                    conf_returntomoh(get_channum_ts((long) cdt.chan_num), confnum);
                    if (dcb_delconf(confdev, confid[confnum]) == -1) {
                        disp_msgf("Unable to delete conference bridge! Error %s", ATDV_ERRMSGP(confdev));
                        return (FALSE);
                    }
                }
            }
            break;

        default:
            if (dcb_remfromconf(confdev, confid[confnum], &cdt) == -1) {
                disp_msgf("Channel %d failed to be removed from conf! Error %s", channum, ATDV_ERRMSGP(confdev));
                return (FALSE);
            }

            break;

    }

    //disp_msg("Participant dropped successfully");
    return (TRUE);

}

bool conf_init(short channum, unsigned char confnum, unsigned char parm) {

    disp_msg("Initiating conference program...");

    if (participants[confnum] == 0) {
        disp_status(channum, "Conf bridge | Waiting for callers");

        if (!(parm & 1)) {
            anncnum[ channum ] = 0;
            errcnt[ channum ] = 1;

            while (errcnt[ channum ] == 1) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/confhold/%d.pcm", anncnum[ channum ]);

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    errcnt[ channum ] = 0;
                }
                else{ 
                    anncnum[channum]++;
                }
            }

            maxannc[ channum ] = anncnum[ channum ];
            anncnum[channum] = 0;
            multiplay[ channum ][0] = open("sounds/conf_onlyparty.pcm", O_RDONLY);

            if (maxannc[channum] == -1) {
                play(channum, multiplay[channum][0], 1, 0, 0);
                dxinfox[ channum ].state = ST_CONFWAITSIL;
            }

            else {
                srandom(time(NULL));
                dxinfox[ channum ].state = ST_CONFWAIT;
                sprintf(dxinfox[ channum ].msg_name, "sounds/confhold/%d.pcm", random_at_most(maxannc[channum]));
                multiplay[ channum ][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                ownies[channum] = 15;
                playmulti(channum, 2, 128, multiplay[channum]);
            }
        }

        else {
            multiplay[ channum ][0] = open("sounds/conf_onlyparty.pcm", O_RDONLY);
            play(channum, multiplay[channum][0], 1, 0, 0);
            dxinfox[ channum ].state = ST_CONFWAITSIL;
        }

        confchan[confnum][0] = channum;
        connchan[channum] = confnum; // connchan is being reused here to indicate what conf we're on. Why? Because, that's why.

        participants[confnum]++;
        return (0);
    }

    if (((participants[confnum] == 1) && ((dxinfox[ confchan[confnum][0] ].state == ST_CONFWAIT) || (dxinfox[ confchan[confnum][0] ].state == ST_CONFWAITSIL)))) {
        // _Wow_, that's a lot of parentheses...

        // Careful. There's a bug in handling of single callers on a conference.

        dx_stopch(dxinfox[confchan[confnum][0]].chdev, EV_ASYNC);
        ownies[confchan[confnum][0]] = 0;

        if (nr_scunroute(dxinfox[ confchan[confnum][0] ].tsdev, SC_DTI, dxinfox[ confchan[confnum][0] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
            disp_msg("Holy shit! Conf SCUnroute threw an error!");
            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
            return (-1);
        }

        if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP) == -1) {
            disp_msg("Holy shit! Conf SCUnroute2 threw an error!");
            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
            return (-1);
        }

        confchan[confnum][1] = channum;
        dxinfox[ confchan[confnum][0] ].state = ST_CONFCONF;
        dxinfox[ confchan[confnum][1] ].state = ST_CONFCONF;

        // Have an array for conference timeslots stored somewhere

        if (!establishconf(confchan[confnum][0], confchan[confnum][1], confnum)) {
            // Defer to the error messages in the function for the moment
            return -1;
        }


        participants[confnum]++;

        if (isdninfo[ confchan[confnum][0] ].cpn[0] != 0x00)
            disp_statusf(confchan[confnum][0], "Conf bridge %d | %s", confnum, isdninfo[ confchan[confnum][0] ].cpn);
        else disp_statusf(confchan[confnum][0], "Conf bridge %d | Channel 1", confnum);

        if (isdninfo[ confchan[confnum][1] ].cpn[0] != 0x00)
            disp_statusf(confchan[confnum][1], "Conf bridge %d | %s", confnum, isdninfo[ confchan[confnum][1] ].cpn);
        else disp_statusf(confchan[confnum][1], "Conf bridge %d | Channel 2", confnum);

        connchan[channum] = confnum;
        return 0;
    }

    else {


        if (participants[confnum] >= CONF_PARTIES) {
            // If the conference is full, announce that and let them go.
            dxinfox[ channum ].state = ST_GOODBYE;
            dxinfox[ channum ].msg_fd = open("sounds/conffull.pcm", O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1)  == -1) {
                file_error(channum, "sounds/conffull.pcm");
            }

            return (0);
        }

        // There's people in the bridge. Add the new caller in.
        participants[confnum]++;

        disp_msgf("New caller is conference participant %d", participants[0]);
        confchan[confnum][participants[confnum]] = channum;

        dxinfox[ channum ].state = ST_CONFCONF;

        if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP) == -1) {
            disp_msg("Holy shit! Conf SCUnroute threw an error!");
            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
            return (-1);
        }
        connchan[channum] = confnum;

        addtoconf(channum, confnum);
        return (0);
    }

}

bool establishconf(int confchan1, int confchan2, unsigned char confnum)

// This function relates nr_scroute for DCB functions
{
    SC_TSINFO tsinfo;
    long scts; // H.100 bus transmit slot
    MS_CDT cdt[2];

    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = &scts;

    if (dt_getxmitslot(dxinfox[confchan1].tsdev, &tsinfo) == -1) {
        disp_msg("dt_getxmitslot1 returned an error!");
        return (FALSE);
    }

    cdt[0].chan_num = (int)scts; // Why is this being cast as an integer? I mean, the manual said to do it...
    cdt[0].chan_sel = MSPN_TS;
    cdt[0].chan_attr = confattr; // This is for the DM/V1200BTEP; no echo cancellation.
    // cdt[0].chan_attr = MSPA_ECHOXCLEN | MSPA_MODEFULLDUPLX; // Turn echo cancellation on
    // Maybe some day we'll put something in here to specify whether or not to echo cancel


    if (dt_getxmitslot(dxinfox[confchan2].tsdev, &tsinfo) == -1) {
        disp_msg("dt_getxmitslot2 returned an error!");
        return (FALSE);
    }

    cdt[1].chan_num = (int)scts; // Why is this being cast as an integer? I mean, the manual said to do it...
    cdt[1].chan_sel = MSPN_TS;
    cdt[1].chan_attr = confattr; // This is for the DM/V1200BTEP; no echo cancellation.

    if (dcb_estconf(confdev, cdt, 2, MSCA_ND, &confid[confnum]) == -1) {
        // Handle Error
        disp_msg("dcb_estconf error");
        return (FALSE);
    }

    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = (long *) &cdt[0].chan_lts;

    if (dt_listen(dxinfox[confchan1].tsdev, &tsinfo) == -1) {
        disp_msg("dt_listen1 error!");
        return (FALSE);
    }

    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = (long *) &cdt[1].chan_lts;

    if (dt_listen(dxinfox[confchan2].tsdev, &tsinfo) == -1) {
        disp_msg("dt_listen1 error!");
        return (FALSE);
    }

    disp_msg("Conference established successfully!") ;

    /*
    if (dcb_setcde( confdev, confid, &cdt[0] ) == -1 ) {
        disp_msg( "Cannot set bridge parameters!");
        return(FALSE);
    }
    */

    return (TRUE);
}

// dialprep should be executed for starting a wardial
char dialprep(short channum) {
    int channel;

    disp_status(channum, "Doing dialprep");
    // int chdev = dxinfox[ channum ].chdev;
    dx_setgtdamp(-23, 0, -23, 0);   // This may negatively impact other functions, like Chucktone detection. Please test.

    if (dx_bldst(TID_MODEM, 2250, 60, TN_LEADING) == -1) {
        disp_msg("Couldn't add modem tone! Exiting...");
        disp_status(channum, "Error building modem tone!");
        return (-1);
    }

    channel = 0;

    while (channel <= chans[ channum ]) {
        if ((channel % 24) == 0) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel].chdev, 'G', DG_USER1) == -1) {     // Must be DG_USER1 to interoperate with DM3
            disp_msgf("Unable to add wardial modem tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channum ].chdev));
            disp_status(channum, "Error adding wardial modem tone!");
            return (-1); // This should be a proper error handling routine.
        }

        // We don't need the tone immediately, so let's disable it.

        if (dx_distone(dxinfox[channel].chdev, TID_MODEM, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("Modem detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error disabling tone!");
            return (-1);
        }

        channel++;
    }

    // Catch the V.21 handshake; 1850 and 1650 hertz tones are used. Let's try just 1650 for the moment
    // and see how reliable detection is.

    if (dx_bldst(TID_FAX, 1650, 50, TN_LEADING) == -1) {
        disp_msg("Couldn't add fax tone! Exiting...");
        disp_status(channum, "Error adding fax tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[channum]) {
        if ((channel % 24) == 0) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel].chdev, 'F', DG_USER1) == -1) {
            disp_msgf("Unable to add wardial fax tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error adding wardial fax tone!");
            return (-1); // This should be a proper error handling routine.
        }

        if (dx_distone(dxinfox[channel].chdev, TID_FAX, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("Fax detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error disabling fax tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_blddt(TID_MODEM2, 3000, 20, 600, 20, TN_LEADING) == -1) {      // Some modems play this after 2100.
        disp_msg("Couldn't add modem tone! Exiting...");
        disp_status(channum, "Error adding modem tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if ((channel % 24) == 0) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel].chdev, 'H', DG_USER1) == -1) {
            disp_msgf("Unable to add wardial modem2 tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error adding modem2 tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[channel].chdev, TID_MODEM2, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("Modem2 detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error disabling modem2 tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldst(TID_MBEEP_1000, 1000, 5, TN_LEADING) == -1) {      // 1000 hertz beep tone
        disp_msg("Couldn't add 1000 hertz tone! Exiting...");
        disp_status(channum, "Error adding 1000 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if ((channel % 24) == 0) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel].chdev, 'I', DG_USER1) == -1) {
            disp_msgf("Unable to add 1000 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error adding 1000 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[channel].chdev, TID_MBEEP_1000, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("1000 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[channel].chdev));
            disp_status(channum, "Error disabling 1000 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_440, 440, 5, 25, 13, 0, 0, 0) == -1) {
        // if ( dx_bldst ( TID_MBEEP_440, 440, 4, TN_LEADING ) == -1 ) { // 440 hertz beep tone
        disp_msg("Couldn't add 440 hertz tone! Exiting...");
        disp_status(channum, "Error adding 440 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'J', DG_USER1) == -1) {
            disp_msgf("Unable to add 440 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 440 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_440, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("440 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 440 hertz tone!");
            return (-1);
        }

        channel++;
    }

    // if ( dx_blddtcad( TID_MBEEP_390, 385, 6, 0, 0, 23, 19, 4, 3, 0) == -1 ) {
    if (dx_bldst(TID_MBEEP_790, 790, 5, TN_LEADING) == -1) {
        // if ( dx_bldstcad( TID_MBEEP_390, 385, 5, 20, 15, 500, 250, 0 ) == -1 ) {
        // if ( dx_bldst ( TID_MBEEP_390, 385, 4, TN_LEADING ) == -1 ) { // 390 hertz beep tone
        disp_msg("Couldn't build 790 hertz tone! Exiting...");
        disp_status(channum, "Error building 790 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'K', DG_USER1) == -1) {
            disp_msgf("Unable to add 790 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error build 790 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_790, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("790 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 790 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_950, 950, 5, 12, 2, 0, 0, 0) == -1) {
        // if ( dx_bldst ( TID_MBEEP_950, 950, 5, TN_LEADING ) == -1 ) { // 950 hertz beep tone
        disp_msg("Couldn't building 950 hertz tone! Exiting...");
        disp_status(channum, "Error building 950 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'L', DG_USER1) == -1) {
            disp_msgf("Unable to add 950 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 950 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_950, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("950 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 950 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldst(TID_MBEEP_2000, 2000, 5, TN_LEADING) == -1) {      // 2000 hertz beep tone
        disp_msg("Couldn't build 2000 hertz tone! Exiting...");
        disp_status(channum, "Error building 2000 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        // Should this just be a single check for %24?
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'M', DG_USER1) == -1) {
            disp_msgf("Unable to add 2000 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 2000 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_2000, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("2000 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 2000 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_500, 500, 4, 55, 10, 0, 0, 0) == -1) {
        // if ( dx_bldst ( TID_MBEEP_500, 500, 5, TN_LEADING ) == -1 ) { // 500 hertz beep tone
        disp_msg("Couldn't build 500 hertz tone! Exiting...");
        disp_status(channum, "Error building 500 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'N', DG_USER1) == -1) {
            disp_msgf("Unable to add 500 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 500 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_500, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("500 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 500 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_1400, 1400, 5, 90, 85, 0, 0, 0) == -1) {     // Make sure this works
        // if ( dx_bldst ( TID_MBEEP_1400, 1400, 5, TN_LEADING ) == -1 ) { // 1400 hertz beep tone
        disp_msg("Couldn't build 1400 hertz tone! Exiting...");
        disp_status(channum, "Error building 1400 hertz tone!");
        dxinfox[channum].state = ST_GOODBYE;
        play(channum, errorfd, 0, 0, 1);
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'O', DG_USER1) == -1) {
            disp_msgf("Unable to add 1400 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 1400 hertz tone!");
            return (-1); // This should be a proper error handling routine.
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_1400, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("1400 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 1400 hertz tone!");
            return (-1);
        }

        channel++;
    }

    //if (dx_bldstcad(TID_MBEEP_1330, 1333, 10, 40, 35, 0, 0, 0) == -1) {
         if ( dx_bldst ( TID_MBEEP_1330, 1330, 10, TN_LEADING ) == -1 ) { // 1330 hertz beep tone
        disp_msg("Couldn't build 1330 hertz tone! Exiting...");
        disp_status(channum, "Error building 1330 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'P', DG_USER1) == -1) {
            disp_msgf("Unable to add 1330 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 1330 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_1330, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("1330 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 1330 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldst(TID_MBEEP_800, 800, 5, TN_LEADING) == -1) {      // 800 hertz beep tone
        disp_msg("Couldn't build 800 hertz tone! Exiting...");
        disp_status(channum, "Error building 800 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'Q', DG_USER1) == -1) {
            disp_msgf("Unable to add 800 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 800 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_800, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("800 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 800 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_425, 425, 5, 50, 10, 0, 0, 0) == -1) {
//    if ( dx_bldst ( TID_MBEEP_425, 425, 5, TN_LEADING ) == -1 ) { // 425 hertz beep tone
        disp_msg("Couldn't build 425 hertz tone! Exiting...");
        disp_status(channum, "Error building 425 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'R', DG_USER1) == -1) {
            disp_msgf("Unable to add 425 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 425 hertz tone!");
            return (-1); // This should be a proper error handling routine.
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_425, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("425 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 425 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_745, 745, 5, 35, 10, 0, 0, 0) == -1) {
        // if ( dx_bldst ( TID_MBEEP_650, 650, 5, TN_LEADING ) == -1 ) { // 650 hertz beep tone
        disp_msg("Couldn't build 745 hertz tone! Exiting...");
        disp_status(channum, "Error building 745 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'S', DG_USER1) == -1) {
            disp_msgf("Unable to add 745 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 745 hertz tone!");
            return (-1); // This should be a proper error handling routine.
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_745, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("745 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 745 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldst(TID_MBEEP_850, 850, 5, TN_LEADING) == -1) {      // 850 hertz beep tone
        disp_msg("Couldn't build 850 hertz tone! Exiting...");
        disp_status(channum, "Error building 850 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'T', DG_USER1) == -1) {
            disp_msgf("Unable to add 850 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 850 hertz tone!");
            return (-1);
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_850, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("850 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 850 hertz tone!");
            return (-1);
        }

        channel++;
    }

    if (dx_bldstcad(TID_MBEEP_1050, 1045, 7, 75, 10, 0, 0, 0) == -1) {
        disp_msg("Couldn't build 1050 hertz tone! Exiting...");
        disp_status(channum, "Error building 1050 hertz tone!");
        return (-1);
    }

    channel = 1;

    while (channel <= chans[ channum ]) {
        if (channel == 24 || channel == 48 || channel == 72 || channel == 96) {
            channel++;
            continue;
        }

        if (dx_addtone(dxinfox[channel ].chdev, 'U', DG_USER1) == -1) {
            disp_msgf("Unable to add 1050 hertz tone to channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error adding 1050 hertz tone!");
            return (-1); // This should be a proper error handling routine.
        }

        if (dx_distone(dxinfox[ channel ].chdev, TID_MBEEP_1050, DM_TONEON | DM_TONEOFF) == -1) {
            disp_msgf("1050 hertz detection tone disabling failed on channel %d. Error %s", channel, ATDV_ERRMSGP(dxinfox[ channel ].chdev));
            disp_status(channum, "Error disabling 1050 hertz tone!");
            return (-1);
        }

        channel++;
    }

    channel = 0; // Reset the variable.

    return (0);

}

/***************************************************************************
 *        NAME: random_at_most()
 * DESCRIPTION: Random number generator
 *       INPUT: max (highest possible number)
 *
***************************************************************************
long random_at_most(long max) {
  unsigned long
    // max <= RAND_MAX < ULONG_MAX, so this is okay.
    num_bins = (unsigned long) max + 1,
    num_rand = (unsigned long) RAND_MAX + 1,
    bin_size = num_rand / num_bins,
    defect   = num_rand % num_bins;

  long x;
  do {
   x = random();
  }
  // This is carefully written not to overflow
  while (num_rand - defect <= (unsigned long)x);

  // Truncated division is intentional
  return x/bin_size;
}
*/

/***************************************************************************
 *        NAME: void intr_hdlr()
 * DESCRIPTION: Handler called when one of the following signals is
 *      received: SIGHUP, SIGINT, SIGQUIT, SIGTERM.
 *       INPUT: None
 *      OUTPUT: None.
 *     RETURNS: None.
 *    CAUTIONS: None.
 ***************************************************************************/
void intr_hdlr() {
    disp_msg("Process Terminating ....");

    end = 1;
}

/***************************************************************************
 *        NAME: void end_app()
 * DESCRIPTION: This function stops I/O activity on all channels and
 *      closes all the channels.
 *       INPUT: None
 *      OUTPUT: None.
 *     RETURNS: None.
 *    CAUTIONS: None.
 ***************************************************************************/
void end_app() {
    short channum;
    long chstate;

    /*
     * Close all the channels opened after stopping all I/O activity.
     * It is okay to stop the I/O on a channel as the program is
     * being terminated.
     */
    if (conf) {
        dcb_close(confdev);
        dcb_close(confbrd);
    }

    /*
    if (dm3board == TRUE) {
        while (bdnum <= 2) {
            if (dx_close(bddev[bdnum]) == -1) {
                disp_msgf("Holy shit, dx_close failed! Board number %d, error %lu", bdnum, ATDV_LASTERR(bddev[bdnum]));
            }

            bdnum++;
        }
    }
    */
    for (channum = 1; channum <= maxchans; channum++) {

        if (frontend == CT_GCISDN) {
            // If it's ISDN, we do timeslot unrouting first for efficiency reasons.
            // It won't hurt anything, and we should only have the if statement
            // (and certainly the modulo operations) once.

            if ((channum % 24) == 0) {
                // Is there a better way to do this? I'd rather not do so many division operations...
                //channum++;
                continue;
            }


            // This is probably an inappropriate place for this line. It should be in
            // a more ISDN-centric place probably. But let's put it here anyway.

            // To-do: Make an opposite to gc_openex happen
            /*
            if (scbus) {
              disp_msgf("Unrouting timeslot for channel %i", channum);
              nr_scunroute( dxinfox[ channum ].tsdev, SC_DTI,
                            dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP );

            }
            */
        }

        dx_deltones(dxinfox[ channum ].chdev);   // Get rid of any remaining user-defined tones
        chstate = ATDX_STATE(dxinfox[ channum ].chdev);
        disp_msgf("Setting channel %i onhook. Currently in state %li", channum, chstate);
        if (chstate != CS_IDLE) {
            dxinfox[ channum ].state = ST_GOODBYE;
            dx_stopch(dxinfox[ channum ].chdev, EV_SYNC);
        }
        set_hkstate(channum, DX_ONHOOK);
        disp_msg("Exiting set_hkstate");
        if (frontend == CT_GCISDN) {
            isdn_close(channum);
        }

        else if (frontend == CT_NTANALOG) {
            /*
             * If analog frontend timeslots had been routed to the resource,
             * then unroute them.
             */
            if (scbus && routeag) {
                nr_scunroute(dxinfox[ channum ].chdev, SC_LSI,
                             dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP);
            }
        } else {        /* Digital Frontend */
            /*
             * Unroute the digital timeslots from their resource channels.
             */
            if (scbus) {
                disp_msgf("Unrouting timeslot for channel %i", channum);
                nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI,
                             dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP);
            }

            /*
             * If E1 frontend, reset to "Blocking state"
             */
            if (frontend == CT_NTE1) {
                dt_settssigsim(dxinfox[ channum ].tsdev, DTB_BON);
            }


            dt_close(dxinfox[ channum ].tsdev);
        }

        dx_close(dxinfox[ channum ].chdev);
    }

    if (frontend == CT_GCISDN) {
        gc_Stop();
    }
    if (altsig & 2) {
        altsig = 0;
        fclose(debugfile);
        debugfile = NULL;
    }

    close(invalidfd);
    close(errorfd);
    close(goodbyefd);
    sqlite3_close(activationdb);
    sqlite3_close(tc_blacklist);

    QUIT(0);
}

/***************************************************************************
 *        NAME: short get_channum( chtsdev )
 * DESCRIPTION: Get the index into the dxinfox[] for the logical timeslot
 *      descriptor
 *       INPUT: long chtsdev;    - Logical channel timeslot from
 *              dt_getxmislot().
 *      OUTPUT: None.
 *     RETURNS: Returns the index into dxinfox[]
 *    CAUTIONS: None.
 ***************************************************************************/
short get_channum_ts(long sctimeslot) {
    // Consider whether or not this should be using a logical timeslot
    // that's stuck in the channel struct on startup instead
    short channum = 1;
    SC_TSINFO tsinfo;
    long scts = -1;
    tsinfo.sc_numts = 1;
    tsinfo.sc_tsarrayp = &scts;
    while (channum <= maxchans) {
        if (dt_getxmitslot(dxinfox[ channum ].tsdev, &tsinfo) == -1) {
            disp_msg("dt_getxmitslot() failed!");
            channum++;
            continue;
        }
        //disp_msgf("DEBUG: Timeslot returned for channel %d is %li", channum, scts);
        if (sctimeslot == scts) return channum;
        channum++;
    }
    return -1;
}

/***************************************************************************
 *        NAME: int get_channum( chtsdev )
 * DESCRIPTION: Get the index into the dxinfox[] for the channel or timeslot
 *      device descriptor, chtsdev.
 *       INPUT: int chtsdev;    - Channel/Timeslot Device Descriptor
 *      OUTPUT: None.
 *     RETURNS: Returns the index into dxinfox[]
 *    CAUTIONS: None.
 ***************************************************************************/
short get_channum(int chtsdev) {

    short channum = 1;
    // Keep on your toes. This code could be trouble.

    while (channum <= maxchans) {
        if ((dxinfox[ channum ].chdev == chtsdev) ||
                (dxinfox[ channum ].tsdev == chtsdev)) {
            return (channum);
        } else {
            channum++;
        }
    }

    /*
     * Not Found in the Array, print error and return -1
     */
    disp_msgf("Device %d got unIDed event - Ignored", chtsdev);

    return (-1);
}

/***************************************************************************
 * NAME: int playtonerep ( chdev, toneval, toneval2, ontime, pausetime )
 * DESCRPTION: Plays a repeating tone.
 *
 * INPUT: short channum; - Channel number
 *        toneval - Hertz value of tone
 *        toneval2 - Hertz value of second tone (use 0 for single tone)
 *        amp1 - Amplitude of tone 1 in decibels
 *        amp2 - Amplitude of tone 2 in decibels
 *        int ontime - Time to play the tone
 *        int offtime - Time to pause between tone repetitions
 * OUTPUT: None.
 *
 * CAUTIONS: I dunno, radioactive?
 ***************************************************************************/
int playtone_rep(short channum, int toneval, int toneval2, int amp1, int amp2, int ontime, int pausetime) {
    int          errcode;
    DV_TPT       tpt[ 1 ];
    TN_GENCAD cadtone;
    cadtone.cycles = 40;
    cadtone.numsegs = 1;
    cadtone.offtime[0] = pausetime;
    dx_bldtngen(&cadtone.tone[0], toneval, toneval2, amp1, amp2, ontime);

    memset(&tpt, 0x00, (sizeof(DV_TPT)));

    /* Terminate Play on Loop Current Drop */
    tpt[ 0 ].tp_type = IO_EOT;
    tpt[ 0 ].tp_termno = DX_LCOFF;
    tpt[ 0 ].tp_length = 1;
    tpt[ 0 ].tp_flags = TF_LCOFF;

    if ((errcode = dx_playtoneEx(dxinfox[ channum ].chdev, &cadtone, tpt, EV_ASYNC)) == -1)

    {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
        disp_msg("There's an error in the cadenced tone generator function. Or maybe a pidgeon.");

    }

    return (errcode);
}

/***************************************************************************
 * NAME: int playtone_cad ( channum, toneval, toneval2, time )
 * DESCRPTION: Plays a cadenced dual frequency tone
 *
 * INPUT: short channum; - Channel number
 *        int toneval - Hertz value of tone
 *        int toneval2 - Hertz value of second tone (use 0 for single tone)
 *        int time - Time in milliseconds divided by ten for the tone to play
 * OUTPUT: None.
 *
 * CAUTIONS: I dunno, radioactive?
 ***************************************************************************/
int playtone_cad(short channum, int toneval, int toneval2, int time) {
    int          errcode;
    DV_TPT       tpt[ 1 ];
    TN_GEN cadtone;
    dx_bldtngen(&cadtone, toneval, toneval2, -15, -18, time);

    memset(&tpt, 0x00, (sizeof(DV_TPT)));

    /* Terminate Play on Loop Current Drop */
    tpt[ 0 ].tp_type = IO_EOT;
    tpt[ 0 ].tp_termno = DX_LCOFF;
    tpt[ 0 ].tp_length = 1;
    tpt[ 0 ].tp_flags = TF_LCOFF;

    if ((errcode = dx_playtone(dxinfox[ channum ].chdev, &cadtone, tpt, EV_ASYNC)) == -1)

    {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
        disp_msg("There's an error in the tone generator function. Or maybe a pidgeon.");

    }

    return (errcode);
}

/***************************************************************************
 * NAME: int playtone ( chdev, toneval, toneval2 )
 * DESCRPTION: Plays a single frequency tone. Eventually I'll make it
 * play dual tones or something more fun.
 * INPUT: short channum; - Channel number
 *        toneval - Hertz value of tone
 *        toneval2 - Hertz value of second tone (use 0 for single tone)
 * OUTPUT: None.
 *
 * CAUTIONS: I dunno, radioactive?
 ***************************************************************************/
int playtone(short channum, int toneval, int toneval2) {
    int          errcode;
    DV_TPT       tpt[ 2 ];
    TN_GEN singletone;
    dx_bldtngen(&singletone, toneval, toneval2, -11, -14, 1000);

    memset(&tpt, 0x00, (sizeof(DV_TPT) * 2));

    /* Terminate Play on Receiving any DTMF tone */
    tpt[ 0 ].tp_type = IO_CONT;
    tpt[ 0 ].tp_termno = DX_MAXDTMF;
    tpt[ 0 ].tp_length = 1;
    tpt[ 0 ].tp_flags = TF_MAXDTMF;

    /* Terminate Play on Loop Current Drop */
    tpt[ 1 ].tp_type = IO_EOT;
    tpt[ 1 ].tp_termno = DX_LCOFF;
    tpt[ 1 ].tp_length = 1;
    tpt[ 1 ].tp_flags = TF_LCOFF;

    if ((errcode = dx_playtone(dxinfox[ channum ].chdev, &singletone, tpt, EV_ASYNC)) == -1)

    {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
        disp_msg("There's an error in the tone generator function. Or maybe a pidgeon.");

    }

    return (errcode);
}

/***************************************************************************
 *      NAME: int playwav( channum, filedesc )
 *      DESCRIPTION: Hardcoded to play mu-law .wav files at the moment for
 *      The Evans Effect and such. Only terminates record on disconnect,
 *      no fancy arguments.
 *      INPUT: channum - Channel number
 *             filedesc - File descriptor of wav to play
 *      OUTPUT: Plays shit
 *      RETURNS: -1 on error, 0 on GREAT SUCCESS!
 **************************************************************************/

int playwav(short channum, int filedesc) {

    DV_TPT  tpt[1];
    DX_XPB  xpb[1];
    DX_IOTT iott[1];
    int errcode;

    memset(&tpt, 0x00, (sizeof(DV_TPT)));
    memset(&xpb, 0x00, sizeof(DX_XPB));

    /* Currently only set to stop playback on loop disconnect */

    tpt[0].tp_type = IO_EOT;
    tpt[0].tp_termno = DX_LCOFF;
    tpt[0].tp_length = 1;
    tpt[0].tp_flags = TF_LCOFF;

    iott[0].io_fhandle = filedesc;
    iott[0].io_offset = 0;
    iott[0].io_length = -1;
    iott[0].io_type = IO_DEV | IO_EOT; // IO_EOT means it's the last definition; no more PCM files are being defined.

    xpb[0].wFileFormat = FILE_FORMAT_WAVE;
    xpb[0].wDataFormat = DATA_FORMAT_MULAW;
    xpb[0].nSamplesPerSec = DRT_8KHZ;
    xpb[0].wBitsPerSample = 8;

    if ((errcode = dx_playiottdata(dxinfox[ channum ].chdev, iott, tpt, xpb, EV_ASYNC)) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
    }

    return (errcode);

}

/*****************************************************************************
 *        NAME: int play( channum, filedesc, format, offset, options )
 * DESCRIPTION: Set up IOTT and TPT's and Initiate the Play-Back
 *       INPUT: short channum; - Index into dxinfo structure
 *      int filedesc;         - File Descriptor of VOX file to Play-Back
 *      int format;           - See source for format identifiers
 *      unsigned long offset; - File offset in bytes
 *      char options          - Playback options. Currently, set to 1 to
 *                              ignore DTMF during playback or anything else
 *                              to not do that.
 *      OUTPUT: Starts the play-back
 *     RETURNS: -1 = Error
 *       0 = Success
 *    CAUTIONS: None
 ***************************************************************************/
int play(short channum, int filedesc, int format, unsigned long offset, char options)

{
    unsigned char     tptnum = 0;
    int     errcode;
    DV_TPT  tpt[ 3 ];
    DX_XPB   xpb[1];

    /*
     * Rewind the file
     */

    if (lseek(filedesc, offset, SEEK_SET) == -1) {
        disp_msgf("Seek operation failed on %s with offset %lu", ATDV_NAMEP(dxinfox[ channum ].chdev), offset);
        dxinfox[ channum ].state = ST_GOODBYE;
        if (play(channum, errorfd, 0, 0, 0) == -1) set_hkstate(channum, DX_ONHOOK);
        return (-1);
    }

    /*
       else {
        disp_msgf("Playing file with offset %lu, result %d, errno %d", offset, res, errno);
       }
    */

    /*
     * Clear and Set-Up the IOTT strcuture
     */
    memset(&dxinfox[ channum ].iott, 0x00, sizeof(DX_IOTT));

    dxinfox[ channum ].iott[ 0 ].io_type = IO_DEV | IO_EOT;
    dxinfox[ channum ].iott[ 0 ].io_fhandle = filedesc;
    dxinfox[ channum ].iott[ 0 ].io_offset = offset;
    dxinfox[ channum ].iott[ 0 ].io_length = -1;

    /*
     * Clear and then Set the DV_TPT structures
     */

    memset(&xpb, 0x00, sizeof(DX_XPB));
    xpb[0].wFileFormat = FILE_FORMAT_VOX;

    switch (format & 0xFF) {
        // Codec selection should only be the first eight bits, if even that

        case 0:
            xpb[0].wDataFormat = DATA_FORMAT_DIALOGIC_ADPCM;
            xpb[0].nSamplesPerSec = DRT_6KHZ;
            xpb[0].wBitsPerSample = 4;

            break;

        case 1:
            xpb[0].wDataFormat = DATA_FORMAT_MULAW;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 8;
            break;

        case 2:
            xpb[0].wDataFormat = DATA_FORMAT_ALAW;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 8;
            break;

        case 3:
            xpb[0].wDataFormat = DATA_FORMAT_G726;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 4;
            break;

        case 4:
            xpb[0].wDataFormat = DATA_FORMAT_DIALOGIC_ADPCM;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 4;
            break;

        case 5:
            xpb[0].wDataFormat = DATA_FORMAT_PCM;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 8;
            break;

        // Formats as of this point are DM3 only. Most of them are weird anyway.

        case 6:
            if (!dm3board) {
                disp_msg("ERROR: DM3-only playback format specified on JCT card! Cannot play.");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            xpb[0].wDataFormat = DATA_FORMAT_PCM;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 16;
            break;

        case 7:
            if (!dm3board) {
                disp_msg("ERROR: DM3-only playback format specified on JCT card! Cannot play.");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            xpb[0].wDataFormat = DATA_FORMAT_G721;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 4;
            break;

        case 8:
            if (!dm3board) {
                disp_msg("ERROR: DM3-only playback format specified on JCT card! Cannot play.");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            xpb[0].wFileFormat = FILE_FORMAT_WAVE;
            xpb[0].wDataFormat = DATA_FORMAT_IMA_ADPCM;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 4;
            break;

        case 9:
            if (!dm3board) {
                disp_msg("ERROR: DM3-only playback format specified on JCT card! Cannot play.");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            xpb[0].wDataFormat = DATA_FORMAT_TRUESPEECH;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 0;
            break;

        // Yeah, seriously, TrueSpeech. Hello 1999, we missed you. Should we bother with GSM support? I think the only reason to put that in there would be to make fun of Asterisk users.

        default:
            // Did we get something weird? It's probably mu-law.
            disp_msg("ERROR: Unidentified format specified for playback! Assuming mu-law");
            xpb[0].wDataFormat = DATA_FORMAT_MULAW;
            xpb[0].nSamplesPerSec = DRT_8KHZ;
            xpb[0].wBitsPerSample = 8;
            break;

    }

    memset(&tpt, 0x00, (sizeof(DV_TPT) * 3));

    if (format & 0x100) {
        tpt[ tptnum ].tp_type = IO_CONT;
        tpt[ tptnum ].tp_termno = DX_TONE;
        tpt[ tptnum ].tp_length = TID_2600;
        tpt[ tptnum ].tp_flags = TF_TONE;

        tptnum++;
    }


    /* Terminate Play on Loop Current Drop */
    if (options != 1) {
    //if (dxinfox[channum].state != ST_FAKECONF3) {
        tpt[ 0 ].tp_type = IO_CONT;
    } else {
        tpt[ tptnum ].tp_type = IO_EOT;
    }

    tpt[ tptnum ].tp_termno = DX_LCOFF;
    tpt[ tptnum ].tp_length = 1;
    tpt[ tptnum ].tp_flags = TF_LCOFF;

    tptnum++;

    if (options != 1) {
    //if (dxinfox[channum].state != ST_FAKECONF3) {

        /* Terminate Play on Receiving any DTMF tone */
        tpt[ tptnum ].tp_type = IO_EOT;
        tpt[ tptnum ].tp_termno = DX_MAXDTMF;
        tpt[ tptnum ].tp_length = 1;
        tpt[ tptnum ].tp_flags = TF_MAXDTMF;

    }

    /*
     * Play VOX File on D/4x Channel, Normal Play Back
     */
    if ((errcode = dx_playiottdata(dxinfox[ channum ].chdev, dxinfox[ channum ].iott,
                                   tpt, xpb, EV_ASYNC)) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
    }

    return (errcode);
}

/***************************************************************************
 *    NAME: int recordwav
 *   INPUT: short channum; - Channel number
 *      int filedesc; - File to record to
 *  OUTPUT: Starts the recording
 * RETURNS: -1 = CLWNBOAT!111
 *       0 = Ownies
 *   NOTES: Currently hardcoded to produce wave files. This might change in
 *      the future.
 **************************************************************************/

int recordwav(short channum, int filedesc) {
    DV_TPT  tpt[2];
    DX_IOTT iott[1];
    DX_XPB  xpb[1];
    int errcode;

    memset(&tpt, 0x00, (sizeof(DV_TPT) * 2));
    memset(&xpb, 0x00, sizeof(DX_XPB));

    if (dxinfox[ channum ].state != ST_CRUDEDIAL) {
        /* Terminate record if someone presses * */
        tpt[0].tp_type   = IO_CONT;
        tpt[0].tp_termno = DX_DIGMASK;
        tpt[0].tp_length = DM_S;
        tpt[0].tp_flags  = TF_DIGMASK;
    }

    /* If we're using your shitty wardialer, stop recording after 25 seconds so we can move on. */

    else {
        tpt[ 0 ].tp_type = IO_CONT;
        tpt[ 0 ].tp_termno = DX_MAXTIME;
        tpt[ 0 ].tp_length = 220;
        tpt[ 0 ].tp_flags = TF_MAXTIME;
    }

    /* Terminate Record on Loop Current Drop */
    tpt[1].tp_type = IO_EOT;
    tpt[1].tp_termno = DX_LCOFF;
    tpt[1].tp_length = 1;
    tpt[1].tp_flags = TF_LCOFF;


    iott[0].io_fhandle = filedesc;
    iott[0].io_offset = 0;
    iott[0].io_length = -1;
    iott[0].io_type = IO_DEV | IO_EOT; // IO_EOT means it's the last definition; no more PCM files are being defined.

    xpb[0].wFileFormat = FILE_FORMAT_WAVE;
    xpb[0].wDataFormat = DATA_FORMAT_MULAW;
    xpb[0].nSamplesPerSec = DRT_8KHZ;
    xpb[0].wBitsPerSample = 8;

    if ((errcode = dx_reciottdata(dxinfox[ channum ].chdev, iott, tpt, xpb, MD_NOGAIN | EV_ASYNC)) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
    }

    return (errcode);
}

/***************************************************************************
 *        NAME: int record( channum, filedesc, format )
 * DESCRIPTION: Set up IOTT and TPT's and Initiate the record
 *       INPUT: short channum;  - Index into dxinfo structure
 *      int filedesc;   - File Descriptor of VOX file to Record to
 *      int format;     - 0 for ADPCM/6 Khz/AGC, 1 for Mu-Law/8 Khz/
 *              no AGC, 2 for same except no silence detection.
 *      OUTPUT: Starts the Recording
 *     RETURNS: -1 = Error
 *       0 = Success
 *    CAUTIONS: None
 ***************************************************************************/
int record(short channum, int filedesc, int format, unsigned short termmsk) {
    int     errcode;
    DV_TPT  tpt[ 4 ];
    int recmode = RM_TONE | MD_NOGAIN | EV_ASYNC; // Re-OR MD_NOGAIN for no AGC.

    if ((format == 1) || (format == 2)) {
        recmode |= MD_PCM | RM_SR8;
    }

    /*
     * Clear and Set-Up the IOTT strcuture
     */
    memset(&dxinfox[ channum ].iott, 0x00, sizeof(DX_IOTT));

    dxinfox[ channum ].iott[ 0 ].io_type = IO_DEV | IO_EOT;
    dxinfox[ channum ].iott[ 0 ].io_fhandle = filedesc;
    dxinfox[ channum ].iott[ 0 ].io_length = -1;

    /*
     * Clear and then Set the DV_TPT structures
     */
    memset(&tpt, 0x00, (sizeof(DV_TPT) * 4));

    /* Terminate Record() when someone presses star */
    tpt[ 0 ].tp_type = IO_CONT;
    tpt[ 0 ].tp_termno = DX_DIGMASK;
    tpt[ 0 ].tp_length = termmsk;
    tpt[ 0 ].tp_flags = TF_DIGMASK;

    /* Terminate Record on Loop Current Drop */
    tpt[ 1 ].tp_type = IO_CONT;
    tpt[ 1 ].tp_termno = DX_LCOFF;
    tpt[ 1 ].tp_length = 1;
    tpt[ 1 ].tp_flags = TF_LCOFF;


    /* Terminate Record After 10 seconds of recording if you're in the recording category state. Otherwise, forever! */
    if (format == 2) {
        tpt[ 2 ].tp_type = IO_EOT;
    } else {
        tpt[ 2 ].tp_type = IO_CONT;
    }

    tpt[ 2 ].tp_termno = DX_MAXTIME;

    if (dxinfox[ channum ].state == ST_CATREC) {
        tpt[ 2 ].tp_length = 100;
    } else {
        tpt[ 2 ].tp_length = 0;
    }

    tpt[ 2 ].tp_flags = TF_MAXTIME;

    if (format != 2) {

        /* Terminate Record on 20 Seconds of Silence */
        tpt[ 3 ].tp_type = IO_EOT;
        tpt[ 3 ].tp_termno = DX_MAXSIL;
        tpt[ 3 ].tp_length = 200;
        tpt[ 3 ].tp_flags = TF_MAXSIL;

    }

    /*
     * Record VOX File on D/4x Channel
     */
    if ((errcode = dx_rec(dxinfox[ channum ].chdev, dxinfox[ channum ].iott,
                          tpt, recmode)) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
    }

    return (errcode);
}

bool openLog() {
    logdescriptor = fopen("scans.log", "a+");

    if (fcntl(fileno(logdescriptor), F_GETFD) == -1) {
        disp_msg("Log opening operation failed");
        return (false);
    } else {
        return (true);
    }
}


/***************************************************************************
 *        NAME: char countup()
 * DESCRIPTION: Simple function that counts upward by a single digit and
 *              performs carrying within a string.
 *       INPUT: numstring (pointer to string)
 *      OUTPUT: None.
 *     RETURNS: TRUE on success, FALSE on failure.
 *    CAUTIONS: None.
 ***************************************************************************/
// I'd LIKE to have this return a bool. That's not happening, though. Fucking C89.
// Since this file is C99 now, maybe this should be fixed. Y'know, to save all of seven bits or something. So if we ever port this to an Atari 2600...
char countup(char *numstring) {
    char strlength;
    strlength = strlen(numstring);

    if (strlength < 5) {  // For testing
        disp_msg("countup() failed!");
        return (-1);
    }

    if (numstring[(strlength - 1)] < 0x39) {
        numstring[(strlength - 1)]++;
    }

    else {

        if (numstring[(strlength - 2)] < 0x39) {
            numstring[(strlength - 2)]++;
            numstring[(strlength - 1)] = 0x30;
        }

        else {
            if (numstring[(strlength - 3)] < 0x39) {
                numstring[(strlength - 3)]++;
                numstring[(strlength - 2)] = 0x30;
                numstring[(strlength - 1)] = 0x30;
            }

            else {
                if (numstring[(strlength - 4)] < 0x39) {
                    numstring[(strlength - 4)]++;
                    numstring[(strlength - 3)] = 0x30;
                    numstring[(strlength - 2)] = 0x30;
                    numstring[(strlength - 1)] = 0x30;
                }

                else {
                    if (numstring[(strlength - 5)] < 0x39) {
                        numstring[(strlength - 5)]++;
                        numstring[(strlength - 4)] = 0x30;
                        numstring[(strlength - 3)] = 0x30;
                        numstring[(strlength - 2)] = 0x30;
                        numstring[(strlength - 1)] = 0x30;
                    }

                    else {
                        if (numstring[(strlength - 6)] < 0x39) {
                            numstring[(strlength - 6)]++;
                            numstring[(strlength - 5)] = 0x30;
                            numstring[(strlength - 4)] = 0x30;
                            numstring[(strlength - 3)] = 0x30;
                            numstring[(strlength - 2)] = 0x30;
                            numstring[(strlength - 1)] = 0x30;
                        }

                        else {
                            if (numstring[(strlength - 7)] < 0x39) {
                                numstring[(strlength - 7)]++;
                                numstring[(strlength - 6)] = 0x30;
                                numstring[(strlength - 5)] = 0x30;
                                numstring[(strlength - 4)] = 0x30;
                                numstring[(strlength - 3)] = 0x30;
                                numstring[(strlength - 2)] = 0x30;
                                numstring[(strlength - 1)] = 0x30;
                            } else {
                                if (numstring[(strlength - 8)] < 0x39) {
                                    numstring[(strlength - 8)]++;
                                    numstring[(strlength - 7)] = 0x30;
                                    numstring[(strlength - 6)] = 0x30;
                                    numstring[(strlength - 5)] = 0x30;
                                    numstring[(strlength - 4)] = 0x30;
                                    numstring[(strlength - 3)] = 0x30;
                                    numstring[(strlength - 2)] = 0x30;
                                    numstring[(strlength - 1)] = 0x30;
                                }
                            }
                        }
                    }
                }

            }

        }

    }

    return (0);

}

/***************************************************************************
*       NAME: int digread
*       DESCRIPTION: Plays back a string of numbers. This is both to
*       encourage CPU efficiency and to stop DM3 cards from poking around
*       at the speed of molasses.
*       INPUT: channum - Channel number
*            digstring - String of digits to play
*       OUTPUT: Your digits - just add water.
*       RETURNS: -1 on WHAT THA FAHCK!?, number of FDs to close on success.
*
***************************************************************************/
int digread(short channum, char *digstring) {
    unsigned char numdigs; // Is a maximum of 255 digits going to be a problem? I hope not...
    numdigs = strlen(digstring);
    unsigned char numdigs2 = numdigs; // I'd rather do this with just one variable, but I think we need two to close all the FDs

    if (numdigs == 0) {   // What happens if strlen returns -1? That might get awkward.
        disp_msg("Could not perform digread function; empty string given");
        return (-1);
    }

    char digfile[21]; // Keep in mind, we only allocated enough characters for the filename in the sprintf below.

    DX_IOTT dig_table[numdigs];
    DX_XPB  xpb_table[numdigs];
    DV_TPT  tpt_table[1];
    memset(&dig_table, 0x00, (sizeof(DX_IOTT) * numdigs));
    memset(&xpb_table, 0x00, (sizeof(DX_XPB) * numdigs));
    memset(&tpt_table, 0x00, sizeof(DV_TPT));

    tpt_table[0].tp_type = IO_EOT;
    tpt_table[0].tp_termno = DX_LCOFF;
    tpt_table[0].tp_length = 1;
    tpt_table[0].tp_flags = TF_LCOFF;

    numdigs--; // We count from zero from here on out.

    // This is a little crude, but whatever. No extra resources were used.

    sprintf(digfile, "sounds/digits1/%c.pcm", digstring[numdigs]);
    file[numdigs] = open(digfile, O_RDONLY);

    dig_table[numdigs].io_type = IO_DEV | IO_EOT;
    dig_table[numdigs].io_fhandle = file[numdigs];
    dig_table[numdigs].io_offset = 0;
    dig_table[numdigs].io_length = -1;

    xpb_table[numdigs].wFileFormat = FILE_FORMAT_VOX;
    xpb_table[numdigs].wDataFormat = DATA_FORMAT_MULAW;
    xpb_table[numdigs].nSamplesPerSec = DRT_8KHZ;
    xpb_table[numdigs].wBitsPerSample = 8;

    numdigs--;

    while (numdigs != 255) {  // The counter will eventually roll over
        sprintf(digfile, "sounds/digits1/%c.pcm", digstring[numdigs]);
        file[numdigs] = open(digfile, O_RDONLY);

        dig_table[numdigs].io_type = IO_DEV;
        dig_table[numdigs].io_fhandle = file[numdigs];
        dig_table[numdigs].io_offset = 0;
        dig_table[numdigs].io_length = -1;

        xpb_table[numdigs].wFileFormat = FILE_FORMAT_VOX;
        xpb_table[numdigs].wDataFormat = DATA_FORMAT_MULAW;
        xpb_table[numdigs].nSamplesPerSec = DRT_8KHZ;
        xpb_table[numdigs].wBitsPerSample = 8;

        numdigs--;

    }

    if (dx_playiottdata(dxinfox[ channum ].chdev, dig_table, tpt_table, xpb_table, EV_ASYNC) != -1) {
        return (numdigs2);
    } else {
        return (-1);
    }
}

/***************************************************************************
*       NAME: int send_bell202
*       DESCRIPTION: Sends an ADSI-formatted file using Bell 202 FSK
*       INPUT: channum - channel number
*              filedesc - File descriptor of data to be sent
*       OUTPUT: BYAH-TIFUL EFF ESS KAY!!!!!1111
*       RETURNS: -1 on error, 0 on success
*
***************************************************************************/
int send_bell202(short channum, int filedesc) {

    DV_TPT tpt[1];
    /*
    if (dx_clrtpt(&tpt[1], 1) == -1 ) {
        disp_msg("Couldn't clear TPT for ADSI sender!");
    }
    */

    DX_IOTT iott[1];
    ADSI_XFERSTRUC adsimode;
    memset(&tpt, 0x00, (sizeof(DV_TPT)));

    tpt[0].tp_type = IO_EOT;
    tpt[0].tp_termno = DX_MAXTIME;
    tpt[0].tp_length = 2400;
    tpt[0].tp_flags = TF_MAXTIME;

    iott[0].io_fhandle = filedesc;
    iott[0].io_type = IO_DEV | IO_EOT;
    iott[0].io_bufp = 0;
    iott[0].io_offset = 0;
    iott[0].io_length = -1;

    adsimode.cbSize = sizeof(adsimode);
    adsimode.dwTxDataMode = ADSI_ALERT; // Set to ADSI_NOALERT for no CAS tone

    if (dx_TxIottData(dxinfox[ channum ].chdev, iott, tpt, DT_ADSI, &adsimode, EV_ASYNC) < 0) {
        disp_msg("dx_TxIottData returned an error!");
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
        return (-1);
    }

    return (0);

}

/***************************************************************************************
 *        NAME: int get_digs( channum, digbufp, numdigs, intermax, termflags)
 * DESCRIPTION: New digit capturing function. This will gradually replace get_digits.
 *       INPUT: short channum             - Channel number; index into dxinfox struct,
 *              assorted arrays
 *              DV_DIGIT *digbufp       - Pointer to Digit Buffer
 *              unsigned short numdigs  - Number of digits to collect (10 ms resolution)
 *              unsigned short intermax - Maximum time between digits. This includes
 *              between zero and one digit.
 *              unsigned char termflags - A bitmask; used to determine extra options
 *              for the function. They are defined as:
 *
 *              0x0 - Normal operation
 *              0x100 - Do not terminate collection upon loop current disconnect.
 *              0x200 - Invoke default error handling procedure if collection
 *              is not successful; play error recording and hang up.
 *              0x400 - Terminate detection on 2600 receipt
 *              0x800 -
 *              0x1000 - Terminate collection upon receiving one particular digit.
 *                     If this is selected, the 4 LSBs indicates the digit in question.
 *                     E = *, F = #, ABCD are normal.
 *              0x2000 - Terminate collection upon a bitmask received as the intermax
 *                     argument. Only this and 0x1000 can be invoked at the same time.
 *                     If found, an inter-digit time of 5 seconds will be used.
 *              0x4000 -
 *              0x8000 -
 *
 *    COMMENTS: I switched numdigs to an unsigned short for numdigs since that's what the
 *              DV_TPT structure wants. It probably wastes, like, .0005 clock cycles to
 *              cast a char to a 16-bit integer, and it probably doesn't save any RAM
 *              anyway. That's on the designers, though; this function won't collect more
 *              than 31 digits.
 *
 *
 **************************************************************************************/
int get_digs(short channum, DV_DIGIT *digbufp, unsigned short numdigs, unsigned short intermax, short termflags) {
    unsigned char tptnum = 0;
    DV_TPT tpt[ 5 ];
    memset(&tpt, 0x00, (sizeof(DV_TPT) * 5));
    int errcode;

    /* The function should always terminate when we receive the maximum amount of digits we call it for. */
    tpt[ tptnum ].tp_type = IO_CONT;
    tpt[ tptnum ].tp_termno = DX_MAXDTMF;
    tpt[ tptnum ].tp_length = numdigs;
    tpt[ tptnum ].tp_flags = TF_MAXDTMF;

    tptnum++;

    if (termflags & 0x1000) {
        // Terminate on digit
        tpt[ tptnum ].tp_type   = IO_CONT;
        tpt[ tptnum ].tp_termno = DX_DIGMASK;

        switch (termflags & 0xF) {
            case 0x1:
                tpt[ tptnum ].tp_length = DM_1;
                break;

            case 0x2:
                tpt[ tptnum ].tp_length = DM_2;
                break;

            case 0x3:
                tpt[ tptnum ].tp_length = DM_3;
                break;

            case 0x4:
                tpt[ tptnum ].tp_length = DM_4;
                break;

            case 0x5:
                tpt[ tptnum ].tp_length = DM_5;
                break;

            case 0x6:
                tpt[ tptnum ].tp_length = DM_6;
                break;

            case 0x7:
                tpt[ tptnum ].tp_length = DM_7;
                break;

            case 0x8:
                tpt[ tptnum ].tp_length = DM_8;
                break;

            case 0x9:
                tpt[ tptnum ].tp_length = DM_9;
                break;

            case 0xA:
                tpt[ tptnum ].tp_length = DM_A;
                break;

            case 0xB:
                tpt[ tptnum ].tp_length = DM_B;
                break;

            case 0xC:
                tpt[ tptnum ].tp_length = DM_C;
                break;

            case 0xD:
                tpt[ tptnum ].tp_length = DM_D;
                break;

            case 0xE:
                tpt[ tptnum ].tp_length = DM_S;
                break;

            case 0xF:
                tpt[ tptnum ].tp_length = DM_P;
                break;

            default:
                tpt[ tptnum ].tp_length = DM_0;
        }

        tpt[ tptnum ].tp_flags  = TF_DIGMASK;
        tptnum++;
    }

    else if (termflags & 0x2000) {
        // Terminate on bitmask
        tpt[ tptnum ].tp_type   = IO_CONT;
        tpt[ tptnum ].tp_termno = DX_DIGMASK;
        tpt[ tptnum ].tp_length = intermax;
        tpt[ tptnum ].tp_flags = TF_DIGMASK;

        // Set inter-digit timing to 5 seconds
        intermax = 50;
        tptnum++;
    }

    // This is technically not allowed, and should be replaced.
    /*
    if ( termflags & 0x400 ) {
        tpt[ tptnum ].tp_type = IO_CONT;
        tpt[ tptnum ].tp_termno = DX_TONE;
        tpt[ tptnum ].tp_length = TID_2600;
        tpt[ tptnum ].tp_flags = TF_TONE;

        tptnum++;
    }
    */

    // Ignore the least significant eight bits. If 0x100 is used, we'll stop the table with this TPT.
    if (termflags & 0x100) {
        tpt[ tptnum ].tp_type = IO_EOT;
    } else {
        tpt[ tptnum ].tp_type = IO_CONT;
    }

    tpt[ tptnum ].tp_termno = DX_IDDTIME;
    tpt[ tptnum ].tp_length = intermax;
    tpt[ tptnum ].tp_flags = TF_IDDTIME;

    tptnum++;

    if (!(termflags & 0x100)) {
        // Can we not check twice? That'd be pretty cool :/
        tpt[ tptnum ].tp_type = IO_EOT;
        tpt[ tptnum ].tp_termno = DX_LCOFF;
        tpt[ tptnum ].tp_length = 1;
        tpt[ tptnum ].tp_flags = TF_LCOFF;
    }

    if ((errcode = dx_getdig(dxinfox[ channum ].chdev, tpt, digbufp,
                             EV_ASYNC)) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);

        if (termflags & 0x200) {
            dxinfox[channum].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
        }
    }

    return (errcode);

}

/***************************************************************************
 *        NAME: int get_digits( channum, digbufp, numdigs )
 * DESCRIPTION: Set up TPT's and Initiate get-digits function
 *       INPUT: short channum;      - Index into dxinfo structure
 *      DV_DIGIT *digbufp;  - Pointer to Digit Buffer
 *      int numdigs;        - Number of digits to collect
 *      OUTPUT: Starts to get the DTMF Digits
 *     RETURNS: -1 = Error
 *       0 = Success
 *    CAUTIONS: None
 ***************************************************************************/
int get_digits(short channum, DV_DIGIT *digbufp, unsigned short numdigs) {
    // This whole function needs to be overhauled. It needs more arguments, is
    // unnecessarily hard to read, and has an unnecessary amount of checks
    int     errcode;
    char        tptnum;

    if (dxinfox[ channum ].state == ST_GETCAT) {
        tptnum = 4;
    }

    if (dxinfox[ channum ].state == ST_OUTDIAL2) {
        tptnum = 4;
    } else {
        tptnum = 3;
    }

    DV_TPT   tpt[ tptnum ];

    /*
     * Clear and then Set the DV_TPT structures
     */
    // dx_clrtpt(tpt,tptnum); // This just generates a shitton of errors.
    memset(&tpt, 0x00, (sizeof(DV_TPT) * tptnum));

    /* Terminate GetDigits on Receiving MAXDTMF Digits */
    tpt[ 0 ].tp_type = IO_CONT;
    tpt[ 0 ].tp_termno = DX_MAXDTMF;
    tpt[ 0 ].tp_length = numdigs;
    tpt[ 0 ].tp_flags = TF_MAXDTMF;

    /* Terminate GetDigits on Loop Current Drop */
    if ((dxinfox[ channum ].state != ST_OUTDIAL2) || (dxinfox[ channum ].state != ST_SASTROLL)) {
        tpt[ 1 ].tp_type = IO_CONT;
    } else {
        tpt[ 1 ].tp_type = IO_EOT;
    }

    tpt[ 1 ].tp_termno = DX_LCOFF;
    tpt[ 1 ].tp_length = 1;
    tpt[ 1 ].tp_flags = TF_LCOFF;

    if ((dxinfox[ channum ].state != ST_OUTDIAL2) || (dxinfox[ channum ].state != ST_SASTROLL))

    {
        /* Terminate GetDigits after 5 Seconds for short digit length. For anything longer, eleven. */
        if (dxinfox[ channum ].state == ST_GETCAT) {
            tpt[ 2 ].tp_type = IO_CONT;
        } else {
            tpt[ 2 ].tp_type = IO_EOT;
        }

        tpt[ 2 ].tp_termno = DX_MAXTIME;

        if (dxinfox[ channum ].state == ST_VMBDETECT) {
            tpt[ 2 ].tp_length = 450;
        } else if ((numdigs > 6) || (dxinfox[ channum ].state == ST_MODEMDETECT)) {
            tpt[ 2 ].tp_length = 130;
        } else {
            tpt[ 2 ].tp_length = 50;
        }

        tpt[ 2 ].tp_flags = TF_MAXTIME;
    }

    if (dxinfox[ channum ].state == ST_GETCAT) {
        tpt[ 3 ].tp_type   = IO_EOT;
        tpt[ 3 ].tp_termno = DX_DIGMASK;
        tpt[ 3 ].tp_length = DM_P;
        tpt[ 3 ].tp_flags  = TF_DIGMASK;
    }

    if ((errcode = dx_getdig(dxinfox[ channum ].chdev, tpt, digbufp,
                             EV_ASYNC)) == -1) {
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
    }

    return (errcode);
}

char dialer_next(short channum) {
    const char *nextNum;

    // This is for list handling code
    if (chaninfo[initchan[channum]].dialer.using_list) {
        if (scancount[ initchan[ channum ] ] != 0) {
            dx_deltones(dxinfox[ channum ].chdev);
            callprog(channum, FALSE);
            dxinfox[ channum ].state = ST_GOODBYE;
            return (0);
        }

        if ((nextNum = dialer_list_next()) != NULL) {
            strcpy(dialerdest[channum], nextNum);
        } else {
            disp_msg("dialer_list_next returned NULL");
            scancount[ initchan[ channum ] ] = 1;
        }
    } else {
        if (scancount[initchan[ channum ] ] <= 0) {
            if (scancount[initchan[ channum ] ] == -2000 ) {
                // Quick & Dirty forever dialing function
                disp_statusf(channum,  "Dialer - dest: %s", dialerdest[ channum ] );
                makecall( channum, dialerdest[ channum ], config.dialercpn, FALSE ); //was 19096611234
                return(0);
            }

            dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
            callprog(channum, FALSE);

            if (isdn_waitcall(channum) != 0) {
                disp_msg("isdn_waitcall failed!");
            }

            return (0);
        }

        scancount[initchan[ channum ] ]--;

    }

    // This is so the ISDN stack can call a new number in the dialer routine once it's finished it's release routine
    disp_statusf(channum, "Dialer - dest: %s", dialerdest[ channum ]);
    makecall(channum, dialerdest[ channum ], config.dialercpn, FALSE);   //was 19096611234
    return (0);
}

bool isdn_listdial(short channum) {
    const char *nextNum;
    
    // Check to see if we're accepting new calls. If not, reset the channel to an idle mode.
    if (scancount[ initchan[ channum ] ] != 0) {
        dx_deltones(dxinfox[ channum ].chdev);
        callprog(channum, FALSE);
        dxinfox[ channum ].state = ST_GOODBYE;
        return (TRUE);
    }

    if ((nextNum = dialer_list_next()) != NULL) {
        strcpy(dialerdest[channum], nextNum);
    } else {
        disp_msg("dialer_list_next returned NULL");
        scancount[ initchan[ channum ] ] = 1;
    }

    disp_statusf(channum, "List dialer - dest: %s, state %d", dialerdest[ channum ], dxinfox[ channum ].state);
    return (TRUE);

}


bool isdn_newdial(short channum) {
    countup(dialerdest[ initchan[ channum ] ]);   // Increase the string by one number
    sprintf(dialerdest[ channum ], "%s", dialerdest[ initchan[ channum ] ]);
    disp_statusf(channum, "New dialer - dest: %s, state %d", dialerdest[ channum ], dxinfox[ channum ].state);
    return (TRUE);
}

// This is for the ISDN event handler, so we can log cause codes coming back to the card.
// Since it's considered part of the dialer program and changing it would imply more global variables, we're keeping it here for now.
char causelog(short channum, int cause) {
    /*
        if(!fprintf( logdescriptor, "%lu;%s;SCANCOUNT: %d;CHANNEL: %d;\n", timeoutput(), dialerdest[ channum ], scancount[ initchan[ channum ] ], channum )){
            disp_msg("Failed to log");
        }
    */
    const char *dest = dialerdest[channum];

    switch (cause) {
        
        case 0x201:
            // Cause code 1 - Number unallocated
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;NUMBER UNALLOCATED\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }
            break;
        
        case GCRV_TIMEOUT:

            // Should this be replaced with a cause code instead? "Congestion" is vague.
            if (!fprintf(logdescriptor, "%s;%s;ERROR;CALL TIMED OUT;\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_REJECT:

            // Should this be replaced with a cause code instead? "Congestion" is vague.
            if (!fprintf(logdescriptor, "%s;%s;ERROR;CALL REJECTED;\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case 0xc4:
        case GCRV_CONGESTION:

            // Should this be replaced with a cause code instead? "Congestion" is vague.
            if (!fprintf(logdescriptor, "%s;%s;ERROR;CONGESTION;\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_PROTOCOL:

            // This is redundant. Should it be removed?
            if (!fprintf(logdescriptor, "%s;%s;PROTOCOL ERROR - SEE CAUSE CODE;\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_NOANSWER:
            if (!fprintf(logdescriptor, "%s;%s;NO ANSWER;\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_SIT_UNKNOWN:
            if (!fprintf(logdescriptor, "%s;%s;SIT;Unknown\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_NO_CIRCUIT_INTERLATA:
            if (!fprintf(logdescriptor, "%s;%s;SIT;InterLATA no circuit found\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_REORDER_INTERLATA:
            if (!fprintf(logdescriptor, "%s;%s;SIT;InterLATA reorder\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_INEFFECTIVE_OTHER:
            if (!fprintf(logdescriptor, "%s;%s;SIT;Ineffective other\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_VACANT_CIRCUIT:
            if (!fprintf(logdescriptor, "%s;%s;SIT;Vacant circuit\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;
            
        case 0x22F:
            // Cause code 47 - Number unallocated
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;RESOURCE UNAVAILABLE, UNSPECIFIED\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }
            break;        

        case 0x21F:
            // Cause code 31 - Normal, unspecified
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;NORMAL, UNSPECIFIED\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }
            break;

        case 0x27F:

            // Cause code 128 - Interworking, unspecified
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;INTERWORKING, UNSPECIFIED\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case 0x229:

            // Cause code 41 - Temporary Failure
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;TEMPORARY FAILURE\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case 0x210:

            // Cause code 16 - Normal call disconnect
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;NORMAL TERMINATION\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case 0x222:

            // Cause code 34 - No circuit/channel available
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;NO CIRCUIT/CHANNEL AVAILABLE\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_NOT_INSERVICE:
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;NOT IN SERVICE\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_UNALLOCATED:
            if (!fprintf(logdescriptor, "%s;%s;CAUSE;NUMBER UNALLOCATED\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_CEPT:
            if (!fprintf(logdescriptor, "%s;%s;SIT;Operator intercept\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCRV_BUSY:
            if (!fprintf(logdescriptor, "%s;%s;BUSY\n", timeoutput(channum), dest)) {
                // Unlike the JCT cards, we can't return the cadence value on here, so we may be hosed :X . Research later.
                disp_msg("Failed to log");
            }

            break;

        case GCCT_UNKNOWN:
            if (!fprintf(logdescriptor, "%s;%s;UNKNOWN\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
                // If this comes up, well, time to debug...
            }

            break;

            if (!fprintf(logdescriptor, "%s;%s;DISCARDED;FIX YOUR CODE NOOBLET\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
                // This should only come up if we can't catch a cause code.
            }

            break;

        case GCCT_CAD:
            if (!fprintf(logdescriptor, "%s;%s;DISCONNECT;CB\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCCT_LPC:
            if (!fprintf(logdescriptor, "%s;%s;DISCONNECT;LCD\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCCT_PVD:
            if (!fprintf(logdescriptor, "%s;%s;CONNECT;PVD\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            if (config.dialersound[0] != '\0') {
                dxinfox[ channum ].msg_fd = open(config.dialersound, O_RDONLY);

                if (dxinfox[ channum ].msg_fd != -1) {
                    disp_status(channum, "Playing PVD sound");
                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1);
                }
            }

            break;

        case GCCT_PAMD:
            disp_status(channum, "Receiving VMBDETECT digits...");

            // Please verify whether or not this is necessary. I *think* it is...
            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 1000 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));

                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                // dx_distone( dxinfox[ channum ].chdev, TID_FAX, DM_TONEON );
                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR1;%s\n", timeoutput(channum), dest, ATDV_ERRMSGP(dxinfox[ channum ].chdev))) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 440 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR2\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 790 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR3\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 950 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR4\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 2000 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR5\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 500 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR6\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 1400 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR7\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 1330 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR8\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 800 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR9\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_425, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 425 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR10\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 745 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR11\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1050, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 1050 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR12\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_850, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 850 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1050, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR13\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            // At this point, we'll do digit collection to look for these tones.

            dxinfox[ channum ].state = ST_VMBDETECT;

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits from dialer channel %s", ATDV_NAMEP(dxinfox[channum].chdev));
                disp_status(channum, "get_digits error!");
            }

            return (0);

        case GCCT_FAX2:
            if (!fprintf(logdescriptor, "%s;%s;DIALTONE!\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }

            break;

        case GCCT_FAX:
            disp_status(channum, "Receiving MODEMDETECT digits...");

            // Please verify whether or not this is necessary. I *think* it is...
            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON) == -1) {
                disp_msgf("Uhh, shit, the fax detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));

                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                if (!fprintf(logdescriptor, "%s;%s;FAX;ERR1\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the modem detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;FAX;ERR2\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON) == -1) {
                disp_msgf("Uhh, shit, the second modem detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the other tones since we won't be doing tone detection.
                dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;FAX;ERR3\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 1400 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));

                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the other tones since we won't be doing tone detection.
                dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;FAX;ERR4\n", timeoutput(channum), dest)) {
                    disp_msg("Failed to log");
                }

                break;
            }

            dxinfox[ channum ].state = ST_MODEMDETECT;

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits from dialer channel %s", ATDV_NAMEP(dxinfox[channum].chdev));
                disp_status(channum, "get_digits error!");
            }

            return (0);
            
        case 0xd7:
        
        // SIT No Circuit cause via firmware
            if (!fprintf(logdescriptor, "%s;%s;SIT;No Circuit\n", timeoutput(channum), dest)) {
                disp_msg("Failed to log");
            }
            break;

        default:
            if (!fprintf(logdescriptor, "%s;%s;RAWCAUSE;0x%hx;\n", timeoutput(channum), dest, cause)) {
                disp_msg("Failed to log");
                return (-1);
            }

            if (cause & 0x500) {
                if (!fprintf(logdescriptor, "%s;%s;RESULT;0x%hx;\n", timeoutput(channum), dest, (cause & 0x7F))) {
                    disp_msg("Failed to log");
                    return (-1);
                }
            }
            // The card ORs the cause code with 0x200 to indicate a cause code coming back from the network instead of a hardware error on our end.
            else if (cause & 0x200) {
                if (!fprintf(logdescriptor, "%s;%s;CAUSE;0x%hx;\n", timeoutput(channum), dest, (cause & 0x7F))) {
                    disp_msg("Failed to log");
                    return (-1);
                }
            }

            else {
                if (!fprintf(logdescriptor, "%s;%s;ERROR;0x%hx;\n", timeoutput(channum), dest, cause)) {
                    disp_msg("Failed to log");
                    return (-1);
                }

            }

            break;

    }



    fflush(logdescriptor);
    return (0);

}

void tl_reroute(short channum) 
    { // This is a stopgap function for returning to the ISDN test line after one end hangs up
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }
        dxinfox[channum].state = ST_ISDNTEST;
        disp_status(channum, "Running ISDN test line");
        sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_testgreet.pcm");

        if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
            disp_msg("Failure playing ISDN testline greeting");

            if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                disp_msg("Holy shit! isdn_drop() failed! D:");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return;
        }

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
            disp_msg("HOLY FUCKING HATBOWLS! Play on tl_reroute failed!");
        }
        return;
    }

void outcall_inroute(short channum) {
    dxinfox[ channum ].state = ST_PLAYMULTI;
    multiplay[channum][0] = open("sounds/tc24/tc_callout.pcm", O_RDONLY);
    multiplay[channum][1] = open("sounds/tc24/tc_callout.pcm", O_RDONLY);
    multiplay[channum][2] = open("sounds/tc24/tc_callout.pcm", O_RDONLY);

    if (playmulti(channum, 3, 0, multiplay[channum]) == -1) {
        disp_msg("Callout message passed bad data to the playmulti function.");
        dxinfox[ channum ].state = ST_GOODBYE;
        play(channum, goodbyefd, 0, 0, 0);
    }
    return;
}


char isdn_inroute(short channum) {
    // This is a new routine for inbound ISDN call routing. DNIS is retreived and
    // put into a global char array by the ISDN offer handler before the call is
    // accepted.

    if (altsig & 4) {
        if (isdninfo[channum].forwardednum[0] != 0x00) {
           if (!fprintf(calllog, "%s;Incoming Destination %s;CPN %s;CPName %s;RDNIS %s\n", timeoutput(channum), isdninfo[channum].dnis, isdninfo[channum].cpn, isdninfo[channum].displayie, isdninfo[channum].forwardednum)) {
               disp_msg("Failed to log");
           }
        }
        else {
           if (!fprintf(calllog, "%s;Incoming Destination %s;CPN %s;CPName %s\n", timeoutput(channum), isdninfo[channum].dnis, isdninfo[channum].cpn, isdninfo[channum].displayie)) {
               disp_msg("Failed to log");
           }
        }
        fflush(calllog);
    }

    if (strcmp("31337", isdninfo[ channum ].dnis) == 0) {
            if (isdn_drop(channum, GC_USER_BUSY) == -1) {
                set_hkstate(channum, DX_ONHOOK);
            }
            return (0);
    }

   if ( (strcmp(isdninfo[channum].dnis, config.extensions.telechallenge) == 0) ){
         set_hkstate(channum, DX_OFFHOOK);
         dx_clrdigbuf(dxinfox[channum].chdev);
         dxinfox[ channum ].state = ST_TC24MENU;
         // Zero this out, just in case.
         memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
         errcnt[ channum ] = 0;
         disp_status(channum, "Running Telechallenge IVR");
         dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
         if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
             file_error( channum, "sounds/tc24/greeting.pcm" );
             return -1;
         }
         return 0;
   }

   if ( (strcmp(isdninfo[channum].dnis, config.extensions.phreakspots) == 0) ){
            srandom(time(NULL));
            disp_status(channum, "Playing Phreakspots recording...");
            anncnum[ channum ] = 0;
            errcnt[ channum ] = 1;
            while (errcnt[ channum ] == 1) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/phreakspots/%d.pcm", anncnum[ channum ]);
                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    errcnt[ channum ] = 0;
                } else {
                    anncnum[ channum ]++;
                }
            }
            maxannc[ channum ] = anncnum[ channum ];
            anncnum[channum] = 0;
            dxinfox[ channum ].state = ST_RDNISREC;
            // srandom(time(NULL));
            sprintf(dxinfox[ channum ].msg_name, "sounds/phreakspots/%d.pcm", random_at_most(maxannc[channum]));
            //sprintf(tmpbuff, "Playing %s", dxinfox[ channum ].msg_name);
            //disp_msg(tmpbuff);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                file_error(channum, dxinfox[ channum ].msg_name);
                return -1;
            }
          return (0);
    }

    if (strcmp(config.extensions.anac, isdninfo[ channum ].dnis) == 0) {
        unsigned char length = strlen(isdninfo[channum].cpn);
        if ((length > 0) && (length < 32)) {
            dxinfox[ channum ].state = ST_PLAYMULTI1;
            closecount[channum] = digread(channum, isdninfo[channum].cpn);
        }
        else {
            disp_msg("ERROR: Too many digits for ANAC!");
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
        } 
        return 0;
    }

    if (strcmp("3920", isdninfo[ channum ].dnis) == 0) {
        // Loop - low end
        set_hkstate(channum, DX_OFFHOOK);
        dxinfox[ channum ].state = ST_LOOP1;
        loopchan = channum;
        playtone_rep(channum, 502, 0, -10, 0, 1000, 100);
        return 0;
    }

    if (strcmp("3921", isdninfo[ channum ].dnis) == 0) {
        // Loop - high end
        if (loopchan == 0) {
            dxinfox[ channum ].state = ST_GOODBYE;
            playtone_rep(channum, 480, 620, -24, -26, 25, 25);
            return 0;
        }
        if (playtone_cad(channum, 502, 0, 300) == 0) {
            set_hkstate(channum, DX_OFFHOOK);
            connchan[loopchan] = channum;
            connchan[channum] = loopchan;
            dxinfox[ channum ].state = ST_LOOP2;
        }
        else {
            disp_msg("Loop2 tone generation routine encountered error!");
            dxinfox[ channum ].state = ST_GOODBYE;
            playtone_rep(channum, 480, 620, -24, -26, 25, 25);
        }
        return 0;
    }

    if ((strcmp(config.extensions.activation, isdninfo[ channum ].dnis) == 0) || (strcmp(config.extensions.altactivation, isdninfo[ channum ].dnis) == 0)) {
        // Zero out the filetmp strings
        memset(filetmp[channum], 0x00, sizeof(filetmp[channum])); // For general SQL queries
        memset(filetmp2[channum], 0x00, sizeof(filetmp2[channum])); // For outbound provisioning orders
        //memset(filetmp3[channum], 0x00, sizeof(filetmp3[channum]));
        disp_statusf(channum, "Running Activation IVR: Ext. %s", isdninfo[channum].cpn);
        srandom(time(NULL));
        errcnt[channum] = 0;
        set_hkstate(channum, DX_OFFHOOK);
        dxinfox[ channum ].state = ST_ACTIVATION;
        dxinfox[ channum ].msg_fd = open("sounds/activation/activation_intro.pcm", O_RDONLY);
        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
            file_error(channum, "sounds/activation/activation_intro.pcm");
            return -1;
        }
        return 0;
    }

   if ( (strcmp(isdninfo[channum].dnis, config.extensions.music) == 0) ){
            srandom(time(NULL));
            disp_status(channum, "Playing music...");
            anncnum[ channum ] = 0;
            errcnt[ channum ] = 1;
            while (errcnt[ channum ] == 1) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/music/%d.pcm", anncnum[ channum ]);

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    errcnt[ channum ] = 0;
                } else {
                    anncnum[ channum ]++;
                }
            }
            maxannc[ channum ] = anncnum[ channum ];
            anncnum[channum] = 0;
            dxinfox[ channum ].state = ST_MUSIC;
            // srandom(time(NULL));
            sprintf(dxinfox[ channum ].msg_name, "sounds/music/%d.pcm", random_at_most(maxannc[channum]));
            //sprintf(tmpbuff, "Playing %s", dxinfox[ channum ].msg_name);
            //disp_msg(tmpbuff);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
            }
          return (0);
    }

    if ((strcmp(config.extensions.altconf, isdninfo[ channum ].dnis) == 0) && conf) {
        set_hkstate(channum, DX_OFFHOOK);
        return conf_init(channum, 1, 1);
    }
    if ((strcmp(config.extensions.confbridge, isdninfo[ channum ].dnis) == 0) && conf) {
        set_hkstate(channum, DX_OFFHOOK);
        return conf_init(channum, 0, 0);
    }

    if (strcmp(config.login, isdninfo[ channum ].dnis) == 0) {
        dxinfox[ channum ].state = ST_DISALOGIN;
        disp_status(channum, "Login prompt");
        set_hkstate(channum, DX_OFFHOOK);
        playtone(channum, 400, 0);
        return (0);
    }

    if (strcmp(config.extensions.collectcall, isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);
        errcnt[ channum ] = 0;
        disp_status(channum, "Accessing collect call IVR");
        dxinfox[channum].state = ST_COLLCALL;
        sprintf(dxinfox[ channum ].msg_name, "sounds/collect/menu.pcm");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
            file_error(channum, "sounds/collect/menu.pcm");
        }

        return (0);
    }

    if (strcmp(config.extensions.normalivr, isdninfo[ channum ].dnis) == 0) {
        // Access normal IVR functions
        ownies[channum] = 0;
        errcnt[channum] = 0;

        if (dx_clrdigbuf(dxinfox[channum].chdev) == 1) {
            disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(dxinfox[channum].chdev));
            disp_err(channum, dxinfox[channum].chdev, dxinfox[ channum ].state);
        }

        set_hkstate(channum, DX_OFFHOOK);
        dxinfox[ channum ].state = ST_DIGPROC;
        playtone(channum, 400, 0);
        return (0);
    }

    if (strcmp(config.extensions.origtest, isdninfo[ channum ].dnis) == 0) {
        ownies[channum] = 0;
        set_hkstate(channum, DX_OFFHOOK);
        errcnt[channum] = 0;

        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            disp_err(channum, dxinfox[channum].chdev, dxinfox[ channum ].state);
        }

        // Call origination test function
        dxinfox[ channum ].state = ST_ISDNTEST;
        playtone(channum, 350, 0);
        return (0);
    }

    if (strcmp(config.extensions.voicemail, isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        anncnum[ channum ] = 0;
        filecount[ channum ] = 0;
        ownies[channum] = 0; // This is our quick and dirty variable for on-hook message handling.
        //There's a better way to do this (^) so, well, do it. Later, I mean. Never now.
        set_hkstate(channum, DX_OFFHOOK);
        disp_status(channum, "Accessing voicemail...");
        sprintf(filetmp[channum], "sounds/vmail/1114");  // Stick the path for the VMB somewhere useful

        if (stat(filetmp[channum], &sts) == -1) {
            mkdir(filetmp[channum], 0700);    // Create the directory if it's not there
        }

        sprintf(filetmp2[channum], "%s/old", filetmp[channum]);

        if (stat(filetmp2[channum], &sts) == -1) {
            mkdir(filetmp2[channum], 0700);    // Does the old directory exist?
        }

        sprintf(filetmp2[channum], "%s/new", filetmp[channum]);

        if (stat(filetmp2[channum], &sts) == -1) {
            mkdir(filetmp2[channum], 0700);    // Does the new directory exist?
        }

        sprintf(filetmp2[channum], "%s/temp", filetmp[channum]);

        if (stat(filetmp2[channum], &sts) == -1) {
            mkdir(filetmp2[channum], 0700);    // Make sure the temporary directory exists too.
        }

        sprintf(filetmp3[channum], "%s/attrib", filetmp[channum]);

        sprintf(dxinfox[ channum ].msg_name, "%s/greeting.pcm", filetmp[channum]);

        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
            sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/generic_greeting.pcm");
        }

        if ((passwordfile[ channum ] = fopen(filetmp3[ channum ], "r")) != NULL) {
            fscanf(passwordfile[ channum ], "%s %c", passcode[ channum ], &vmattrib[ channum ]);
            fclose(passwordfile[ channum ]);
        }

        dxinfox[ channum ].state = ST_VMAIL1;
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
        }

        return (0);

    }

    if (strcmp(config.extensions.soundplayer, isdninfo[ channum ].dnis) == 0) {
        set_hkstate(channum, DX_OFFHOOK);

        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Accessing Sound Player");
        ownies[ channum ] = 0;
        errcnt[ channum ] = 0;
        loggedin[ channum ] = 0;
        passwordfile[channum] = NULL;

        dxinfox[ channum ].state = ST_GETCAT;
        sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_entercat.pcm");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
            file_error(channum, "sounds/ivr_entercat.pcm");
        }

        return (0);

    }

    if (strcmp(config.extensions.projectupstage, isdninfo[ channum ].dnis) == 0) {

        // Do 2600 tone adding first. If it dun goofs, let's GTFO.

        disp_status(channum, "Running Project Upstage");
        // Try these new values for JCT compatibility.

        if (!dm3board) {

            if (dx_bldstcad(TID_2600, 2600, 65, 35, 0, 10, 0, 0) == -1) {
                disp_msg("Couldn't add 2600 tone! Exiting...");
                disp_status(channum, "Error adding 2600 tone!");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

        }

        else if (dm3board) {

            dx_deltones(dxinfox[channum].chdev); // This is sort of a crude bugfix, and it shouldn't be in here long term. 
            if (dx_bldstcad(TID_2600, 2600, 60, 60, 60, 0, 0, 0) == -1) {
                disp_msg("Couldn't add 2600 tone! Exiting...");
                disp_status(channum, "Error adding 2600 tone!");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

        }

        if (dx_addtone(dxinfox[ channum ].chdev, '\0', 0) == -1) {
            disp_msgf("Unable to add 2600 hertz tone to channel %d. Error %s", ownies[channum], ATDV_ERRMSGP(dxinfox[ channum ].chdev));
            disp_status(channum, "Error adding 2600 hertz tone!");
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return (-1);
        }

        if (dx_setdigtyp(dxinfox[ channum ].chdev, D_MF) == -1) {
            // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
            // This really, *really* shouldn't fail...
            disp_msg("Unable to set digit type to MF!");
            disp_status(channum, "Unable to set digit type to MF!");
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return (-1);
        }

        set_hkstate(channum, DX_OFFHOOK); // To be fair to people, send the trunk offhook
        ownies[ channum ] = 9;
        dxinfox[ channum ].state = ST_2600STOP;
        playtone_cad(channum, 0, 0, 40);
        return (0);
    }

    if (strcmp(config.extensions.evansbot, isdninfo[ channum ].dnis) == 0) {
        srandom(time(NULL));
        set_hkstate(channum, DX_OFFHOOK);

        disp_status(channum, "Running the Evans Effect DM3 patch...");
        lig_if_followup[ channum ] = (random_at_most(3) + 1);

        dxinfox[ channum ].state = ST_EVANSDM3;
        ownies[ channum ] = 0;

        if (lig_if_followup[ channum ] >= 3) {
            ligmain[ channum ] = random_at_most(15);
            sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-init%d.wav", ligmain[ channum ]);
            disp_msg("Opening init file");
            multiplay[ channum ][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);
            ownies[channum]++;
        }

        ligmain[ channum ] = random_at_most(97);
        dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, SV_ADD8DB);   // Reset volume to normal
        sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-%d.wav", ligmain[ channum ]);
        disp_msg("Opening main file");
        multiplay[channum][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (multiplay[channum][ownies[channum]] == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
            return (-1);
        }

        if (lig_if_followup[channum] > 1) {
            ownies[channum]++;
            ligmain[ channum ] = random_at_most(56);
            sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-followup%d.wav", ligmain[ channum ]);
            disp_msg("Opening followup file");
            multiplay[channum][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);
        }

        disp_msg("Playing back...");
        playmulti(channum, lig_if_followup[channum], 1, multiplay[channum]);
        return (0);
    }

    if (strcmp(config.extensions.newsfeed, isdninfo[ channum ].dnis) == 0) {

        set_hkstate(channum, DX_OFFHOOK);
        dxinfox[ channum ].state = ST_PLAY;

        dxinfox[ channum ].msg_fd = open("sounds/impression.pcm", O_RDONLY);

        if (dxinfox[ channum ].msg_fd == -1) {
            file_error(channum, "sounds/impression.pcm");
            return (-1);
        }

        else {
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
        }

        return (0);
    }

    if ((strcmp(config.extensions.emtanon1, isdninfo[ channum ].dnis) == 0) || (strcmp(config.extensions.emtanon2, isdninfo[ channum ].dnis) == 0)) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);

        // This'll be a one time only thing
        if (strlen(isdninfo[channum].cpn) > 4) {
            if ((strcmp("8134695930", isdninfo[channum].cpn) != 0) && (strcmp("18134695930", isdninfo[channum].cpn) != 0)) {
                voicemail_xfer(channum, "1114");
                return 0;
            }

        }

        if (termmask[ channum ] & 1) {
        //if ((ownies[channum] == 100) || (ownies[channum] == 200) ) {
            disp_msg("Removing custom tone with incoming call");
            dx_distone(dxinfox[channum].chdev, TID_1, DM_TONEON);
            dx_deltones(dxinfox[channum].chdev);
            termmask[ channum ] ^= 1;
        }

        if (dx_blddt(TID_1, 1880, 15, 697, 15, TN_TRAILING) == -1) {
            disp_msg("Shit we couldn't build the Chucktone!");
        }

        if (dx_addtone(dxinfox[channum].chdev, 'E', DG_USER1) == -1) {
            disp_msgf("Unable to add Chucktone. %s", ATDV_ERRMSGP(dxinfox[channum].chdev));
        }


        // srandom(time(NULL)); // Seed the random number generator for the thing
        disp_status(channum, "Accessing Voice BBS");

        dxinfox[ channum ].state = ST_EMPLAY1;
        ownies[ channum ] = 200;
        dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

        dxinfox[ channum ].msg_fd = open("sounds/emtanon/greeting_new.pcm", O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
            file_error(channum, "sounds/emtanon/greeting_new.pcm");
        } else {
            return (0);
        }

    }

    if (strcmp(config.extensions.shameshameshame, isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);
        disp_status(channum, "SHAME SHAME SHAME SHAAAAAAME");
        dxinfox[ channum ].state = ST_GOODBYE;
        dxinfox[ channum ].msg_fd = open("sounds/confshame.pcm", O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1) == -1) {
            file_error(channum, "sounds/confshame.pcm");
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp(config.extensions.mtnschumer, isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        srandom(time(NULL));
        set_hkstate(channum, DX_OFFHOOK);
        disp_status(channum, "Mtn. Schumer playback");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/amy%d.pcm", random_at_most(7));
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1) == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("9761444", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);
        disp_status(channum, "Accessing Fakeconf");
        errcnt[channum] = 0;
        dxinfox[ channum ].state = ST_FAKECONF1;
        dxinfox[ channum ].msg_fd = open("sounds/meetme-welcome.pcm", O_RDONLY);

        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
            file_error(channum, "sounds/meetme-welcome.pcm");
            return (-1);
        } else {
            return (0);
        }
    }


    
    if (strcmp(config.extensions.callintercept, isdninfo[ channum ].dnis) == 0) {
        // Execution of The Thing happens here.
        if ((strcmp("16312763409", isdninfo[ channum ].cpn) == 0) || (strcmp("6312763409", isdninfo[ channum ].cpn) == 0)) {
            disp_status(channum, "Playing fake ring...");
            dxinfox[channum].state = ST_GOODBYE;
            sprintf(dxinfox[ channum ].msg_name, "sounds/2min_ring.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1)  == -1) {
                if (isdn_drop(channum, GC_USER_BUSY) == -1) {
                    set_hkstate(channum, DX_ONHOOK);
                }
            }

            return (0);
        }
        
        return(routecall( channum, 1, 23, config.interceptdest, isdninfo[channum].cpn, TRUE));
    }
    

    if (strcmp(config.extensions.echotest, isdninfo[ channum ].dnis) == 0) {

        dxinfox[channum].state = ST_ECHOTEST;

        if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[channum].tsdev, SC_DTI, SC_FULLDUP) == -1) {
            disp_msg("NO ECHOTEST NR_SCROUTE!? WHAAAAAAT!!??");
            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
            return (-1);
        }

        disp_status(channum, "Channel loopback enabled");
        return (0);
    }

    // These states are for the dialer testing application. Remove for standard builds.

    if (strcmp("90000", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - AVST");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/avst.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90001", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Amtelco MOH");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/moh.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90002", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Norstar key system");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/norstar-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90003", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Fake PAMD;390");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/fake390.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90004", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - APMax");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/apmax-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90005", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - PHONEMAAAAAAIL!!!");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/phonemail-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90006", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Fake PAMD;440");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/fake440.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90007", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - CALLPILOT!!");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/callpilot-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90008", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - AT&T UM");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/attum-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90009", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - GTE/Glenayre");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/gte-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90010", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Innline");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/innline-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90011", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Tellme");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/tellme_menu.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90012", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Weird error");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/weirderror.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90013", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Mayfair 1");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/mayfair1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90014", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Qwest UM");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/qwestum-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90015", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - GTE 2");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/gte2-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90016", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Panasonic w/1050 hertz tone");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/panasonic1050-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90017", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Unknown Verizon VMB");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/vzvmb-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90018", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Mayfair 2");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/mayfair2.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90019", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Old Cisco VMB");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/oldcisco-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90020", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - IP Office VMB");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/ipoffice-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90021", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Compilot VMB");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/compilot-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90022", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Cellular VMB (Comverse?)");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/cellvmb-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90023", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - Octel");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/octel-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90024", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - POTS MOH");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/potsmoh-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90025", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        disp_status(channum, "Dialer test - IP MOH");
        dxinfox[ channum ].state = ST_DIALSUPE; // Proceed to the playback_hdlr to go offhook and play the rest of this
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/ipmoh-1.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90026", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Mayfair via IP trunk");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/mayfair3.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (strcmp("90027", isdninfo[ channum ].dnis) == 0) {
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);   // Go offhook immediately for this one.
        disp_status(channum, "Dialer test - Phonemail 2");
        dxinfox[ channum ].state = ST_GOODBYE;
        sprintf(dxinfox[ channum ].msg_name, "sounds/dialertest/phonemail2.wav");
        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

        if (playwav(channum, dxinfox[ channum ].msg_fd) == -1) {
            return (-1);
        } else {
            return (0);
        }
    }

    if (config.cnetintercept) {
        disp_status(channum, "Non-working number dialed");
        dxinfox[channum].state = ST_ISDNNWN;
        dxinfox[ channum ].msg_fd = open("sounds/edram_nwn.pcm", O_RDONLY);

        if (dxinfox[ channum ].msg_fd == -1) {
            file_error(channum, "sounds/edram_nwn.pcm");
        } else {
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1);
        }

        return 0;
    }


    else { // Make this a catch-all if no other destination matches
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        set_hkstate(channum, DX_OFFHOOK);
        dxinfox[channum].state = ST_ISDNTEST;
        disp_status(channum, "Running ISDN test line");
        sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_testgreet.pcm");

        if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
            disp_msg("Failure playing ISDN testline greeting");

            if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                disp_msg("Holy shit! isdn_drop() failed! D:");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (-1);
        }

        play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
        return (0);
    }

    disp_msg("Something fell to the bottom of the ISDN destination handler. That's... that's really bad.");
    // If something escapes the catch-all handler and winds up here, this is really the best we can do.
    dxinfox[ channum ].state = ST_GOODBYE;
    play(channum, errorfd, 0, 0, 1);
    return (-1);

}


/***************************************************************************
 *        NAME: int set_hkstate( channum, state )
 * DESCRIPTION: Set the channel to the appropriate hook status
 *       INPUT: short channum;      - Index into dxinfo structure
 *      int state;      - State to set channel to
 *      OUTPUT: None.
 *     RETURNS: -1 = Error
 *       0 = Success
 *    CAUTIONS: None.
 ***************************************************************************/
int set_hkstate(short channum, int state) {
    int chdev = dxinfox[ channum ].chdev;
    int tsdev = dxinfox[ channum ].tsdev;
    int channel;
    const char *nextNum;

    /*
     * Make sure you are in CS_IDLE state before setting the
     * hook status
     */

    if (ATDX_STATE(chdev) != CS_IDLE) {
        dx_stopch(chdev, EV_ASYNC);

        while (ATDX_STATE(chdev) != CS_IDLE);
    }

    switch (frontend) {
        case CT_NTANALOG:
            if (dx_sethook(chdev, (state == DX_ONHOOK) ? DX_ONHOOK : DX_OFFHOOK,
                           EV_ASYNC) == -1) {
                disp_err(channum, chdev, dxinfox[ channum ].state);
                disp_msgf("Cannot set channel %s to %s-Hook (%s)", ATDV_NAMEP(chdev), (state == DX_ONHOOK) ? "On" : "Off",
                        ATDV_ERRMSGP(chdev));
                return (-1);
            }

            break;

        case CT_GCISDN:

            if (state == DX_OFFHOOK) {
                // Accept a call!
                disp_msgf("Call being answered on channel %i", channum);

                if (isdn_answer(channum) == -1) {
                    disp_msg("Holy shit! isdn_answer() failed! D:");
                    return (-1);
                }
            }

            if (state == DX_ONHOOK) {
                // Tear one down
                disp_msgf("Call being terminated on channel %i", channum);

                if (isdn_drop(channum, GC_NORMAL_CLEARING) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    return (-1);
                }

                if (dxinfox[ channum ].state == ST_CALLPTEST5) {

                    if (chaninfo[initchan[channum]].dialer.using_list) {
                        // Is this check necessary?
                        if (scancount[ initchan[ channum ] ] > 0) {
                            dx_deltones(dxinfox[ channum ].chdev);
                            callprog(channum, FALSE);
                            dxinfox[ channum ].state = ST_GOODBYE;
                            return (0);
                        }

                        isdn_listdial(channum);
                    } 
                    
                    if (scancount[initchan[channum] ] == -2000) {
                        return 0;
                    }

                    isdn_newdial(channum);
                    return 0;
                }
            }

            break;

        case CT_NTT1:
            if (dt_settssigsim(tsdev, (state == DX_ONHOOK) ? DTB_AOFF | DTB_BOFF :
                               DTB_AON | DTB_BON) == -1) {
                disp_err(channum, tsdev, dxinfox[ channum ].state);
                disp_msgf("Cannot set bits to %s on %s (%s)", (state == DX_ONHOOK) ? "AOFF-BOFF" : "AON-BON", ATDV_NAMEP(tsdev),
                        ATDV_ERRMSGP(tsdev));
                return (-1);
            }

            break;

        case CT_NTE1:
            if (dt_settssigsim(tsdev, (state == DX_ONHOOK) ? DTB_AON : DTB_AOFF)
                    == -1) {
                disp_err(channum, tsdev, dxinfox[ channum ].state);
                disp_msgf("Cannot set bits to %s on %s (%s)", (state == DX_ONHOOK) ? "AON-BOFF" : "AOFF", ATDV_NAMEP(tsdev),
                        ATDV_ERRMSGP(tsdev));
                return (-1);
            }
    }

    if (frontend != CT_NTANALOG) {
        switch (state) {
            case DX_ONHOOK:

                if (frontend == CT_GCISDN) {
                    disp_status(channum, "ISDN channel ready!");


                    if (dxinfox[ channum ].state == ST_CALLPTEST4) {
                        // The ST_WTRING thing may be redundant now since isdn_drop does it; please evaluate removal to get away from redundancy
                        dxinfox[ channum ].state = ST_WTRING;

                        // dialprep only needs to be called once. Don't do it a million times, or it'll fuck things up.

                        if (dialprep(channum) == -1) {
                            disp_msg("dialprep() failed! Returning to idle state.");
                            return (-1);
                        }

                        // Was 49; for third span workaround
                        channel = 2;

                        if (chaninfo[channum].dialer.using_list) {
                            disp_msg("Performing list dialer initialization routine...");

                            while ((channel <= maxchans) && (channel <= chans[ channum ]) && (scancount[channum] == 0)) {
                                if ((channel % 24) == 0) {
                                    channel++;
                                    continue;
                                }

                                if (dxinfox[channel].state == ST_WTRING) {
                                    initchan[channel] = channum;
                                    dxinfox[channel].state = ST_CALLPTEST5;
                                    //dx_initcallp( dxinfox[channel].chdev); // Initialize call progress detection. This is unnecessary on DM3 boards and will return an error.
                                    // For testing, this is going to be DM3 only
                                    callprog(channel, TRUE); // This is the DM3-exclusive CPA enabler
                                    sprintf(dialerdest[channel], "%s", dialerdest[channum]);
                                    disp_statusf(channel, "List Dialer Init - dest: %s", dialerdest[channel]);
                                    chaninfo[channel].dialer.using_list = TRUE; // This shouldn't be necessary, but if it is...

                                    // This if statement is probably just unreachable code, and you can get rid of it.
                                    if (scancount[ initchan[channel] ] > 0) {
                                        dx_deltones(dxinfox[ channum ].chdev);
                                        callprog(channum, FALSE);
                                        dxinfox[ channum ].state = ST_GOODBYE;
                                        return (0);
                                    }

                                    if ((nextNum = dialer_list_next()) != NULL) {
                                        strcpy(dialerdest[channel], nextNum);
                                    } else {
                                        disp_msg("dialer_list_next returned NULL");
                                        scancount[initchan[channel]] = 1;

                                        dxinfox[ channum ].state = ST_GOODBYE;
                                        play(channum, errorfd, 0, 0, 1);
                                        return (0);
                                    }

                                    disp_msg("Performing first makecall");
                                    makecall(channel, dialerdest[channel], config.dialercpn, FALSE); // was 909-661-1234
                                }

                                else {
                                    disp_msgf("Channel %d looks like it's still in use. Attempting to move to other channel...", channel);
                                }

                                channel++;
                            }

                        } else { // Non-list dialer initialization
                            while(( channel <= maxchans ) && ( channel <= chans[ channum ] ) &&  ((scancount[channum] > 0 ) || scancount[channum] == -2000 )) {
                                if ((channel % 24) == 0) {
                                   channel++;
                                    continue;
                                }

                                if (dxinfox[channel].state == ST_WTRING) {
                                    initchan[channel] = channum;
                                    dxinfox[channel].state = ST_CALLPTEST5;
                                    //dx_initcallp( dxinfox[channel].chdev); // Initialize call progress detection. This is unnecessary on DM3 boards and will return an error.
                                    // For testing, this is going to be DM3 only
                                    callprog(channel, TRUE); // This is the DM3-exclusive CPA enabler
                                    sprintf(dialerdest[channel], "%s", dialerdest[channum]);
                                    disp_statusf(channel, "Dialer - dest: %s", dialerdest[channel]);

                                    if (scancount[channum] != -2000 ) {
                                        countup(dialerdest[channum]);   // Increase the string by one number
                                        disp_msg("Performing scancount");
                                        scancount[channum]--;
                                    }

                                    disp_msg("Performing first makecall");
                                    makecall(channel, dialerdest[ channum ], config.dialercpn, FALSE);
                                }

                                else {
                                    disp_msgf("Channel %d looks like it's still in use. Attempting to move to other channel...", channel);
                                }

                                channel++;
                            }
                        }

                    }

                } else {
                    disp_status(channum, "Ready to accept a call");
                }

                // The ST_WTRING thing may be redundant now since isdn_drop does it; please evaluate removal to get away from redundancy.
                dxinfox[ channum ].state = ST_WTRING; // This may be conflicting with the dialer. Uncomment if it's not; non-ISDN stacks use it.

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                break;

            case DX_OFFHOOK:

                if (dxinfox[ channum ].state == ST_ROUTED2) {
                    return (0);    // If this is an outgoing call, don't try and run the welcome wagon.
                }

                if (dxinfox[ channum ].state == ST_ROUTEDREC) {
                    return (0);    // Not on an outgoing recorded call either.
                }

                if (dxinfox[ channum ].state == ST_2600ROUTE) {
                    return (0);
                }

                ownies[channum] = 0; // Initialize ownies. Just in case.
                errcnt[ channum ] = 0;
                // loggedin[channum] = 0; // Make sure this is necessary. I don't think it is.
                passcode[channum][0] = '\0'; // Just in case...
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                if (altsig & 1) {
                    return (0);
                }

                // Why is this here twice? Pay attention >:(
                if (dxinfox[ channum ].state == ST_ROUTED2) {
                    return (0);    // If this is an outgoing call, don't try and run the welcome wagon.
                }

                if (dxinfox[ channum ].state == ST_ROUTEDREC) {
                    return (0);    // Not on an outgoing recorded call either.
                }

                errcnt[channum] = 0;

                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                break;
        }
    }

    return (0);
}

/***************************************************************************
 *        NAME: int wink_hdlr()
 * DESCRIPTION: DT_XMITWINK event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int wink_hdlr() {

    int  chdev = sr_getevtdev();
    // int  event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on wink chdev");
    }

    int  channum = get_channum(chdev);
    int  curstate;
    // int  errcode = 0;

    if (channum == -1) {
        disp_msg("Invalid channel number for wink handler!");
        return (0);               /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;          /* Current State */
    disp_msgf("Executing xmitwink handler code in state %i", curstate);

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */
    /*
    switch ( frontend ) {

    case CT_NTT1:
      if ( ( ATDT_TSSGBIT( dxinfox[ channum ].tsdev ) & DTSG_RCVA ) == 0 ) {
         return( 0 );
      }
      break;

    case CT_NTE1:
      if ( ( ATDT_TSSGBIT( dxinfox[ channum ].tsdev ) & DTSG_RCVA ) != 0 ) {
         return( 0 );
      }
      break;
    }
    */
    switch (curstate) {
        case ST_WINK:

//      dx_setdigtyp( dxinfox[ channum ].chdev, D_MF );
            //if ( get_digits( channum, &(dxinfox[ channum ].digbuf), 4 ) == -1 ) {
            if (get_digs(channum, &dxinfox[ channum ].digbuf, 4, 110, 0) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                break;
            }

            dxinfox[ channum ].state = ST_WTRING;

            return (0);

    }

    return (0);
}

/***************************************************************************
 *        NAME: int cst_hdlr()
 * DESCRIPTION: TDX_CST event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int cst_hdlr() {
    int     chdev = sr_getevtdev();
    unsigned char reason[3];
    // int     event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on cst chdev");
    }

    int     channum = get_channum(chdev);
    int     curstate;
    DX_CST  *cstp;

    /* RCS commented this out to fix PT 21133 since there was no reason
    for the customer to see this debug statement...
    disp_msg("In the cst_hdlr");
    */

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */
    cstp = (DX_CST *) sr_getevtdatap();

    switch (cstp->cst_event) {

        case DE_RINGS:      /* Incoming Rings Event */
            if (curstate == ST_WTRING) {
                /*
                 * Set Channel to Off-Hook
                 */

                if ((altsig & 8) && (cstp->cst_data & 0x0001)) {
                    disp_msg("Accessing caller ID functionality...");

                    if (dx_gtextcallid(dxinfox[ channum ].chdev, CLIDINFO_CALLID, (unsigned char *) &(isdninfo[channum].cpn[0])) == -1) {
                        // Handle error here
                        switch (ATDV_LASTERR(dxinfox[ channum ].chdev)) {
                            case EDX_CLIDBLK:
                                if (dx_gtextcallid(dxinfox[ channum ].chdev, MCLASS_ABSENCE1, &reason) == -1) {
                                    disp_msg("ERROR: Caller ID info blocked, reason retrieval failed.");
                                    break;
                                } else {
                                    if (reason[0] == 'O') {
                                        disp_msg("Out of area caller ID message received.");

                                        if (altsig & 4) {
                                            if (!fprintf(calllog, "%s;Incoming analog call on channel %d: out of area\n", timeoutput(channum), channum)) {
                                                disp_msg("Failed to log");
                                            }

                                            fflush(calllog);
                                        }

                                        break;
                                    }

                                    if (reason[0] == 'P') {
                                        disp_msg("Private caller ID message received.");

                                        if (altsig & 4) {
                                            if (!fprintf(calllog, "%s;Incoming analog call on channel %d: private\n", timeoutput(channum), channum)) {
                                                disp_msg("Failed to log");
                                            }

                                            fflush(calllog);
                                        }

                                        break;
                                    }

                                    break;
                                }

                            case EDX_CLIDINFO:
                                disp_msg("ERROR: Caller ID info requested, but not received.");
                                break;

                            case EDX_CLIDOOA:
                                disp_msg("Out of area caller ID message received.");

                                if (altsig & 4) {
                                    if (!fprintf(calllog, "%s;Incoming analog call on channel %d: out of area\n", timeoutput(channum), channum)) {
                                        disp_msg("Failed to log");
                                    }

                                    fflush(calllog);
                                }

                                break;

                            default:
                                disp_msg("Unknown error condition occurred when retrieving caller ID data!");
                                break;
                        }
                    }

                    /*
                    if ( dx_gtextcallid( dxinfox[ channum ].chdev, MCLASS_QUALIFIER, &ownies[channum] ) == -1 ) {
                         disp_msg("ERROR: MCLASS qualifier could not be retrieved!");
                    }
                    // This should be part of an else statement. Add that, please.
                    if ( ownies[channum] == 'L' ) disp_msg("I'M ON A LONG DISTANCE CALL MOTHER!!!1");

                    if ( dx_gtextcallid( dxinfox[ channum ].chdev, MCLASS_REDIRECT, &ownies[channum] ) == -1 ) {
                         disp_msg("ERROR: MCLASS redirect could not be retrieved!");
                    }
                    if ( ownies[channum] == 0x30 ) disp_msg("Call forward reported - universal");
                    if ( ownies[channum] == 0x31 ) disp_msg("Call forward reported - busy");
                    if ( ownies[channum] == 0x32 ) disp_msg("Call forward reported - no answer");
                    ownies[channum] = 0x0; // Reset the variable
                    */
                    if (dx_gtextcallid(dxinfox[ channum ].chdev, MCLASS_NAME, (unsigned char *) &(isdninfo[channum].displayie[0])) == 0) {     // This shouldn't be hardcoded.
                        if (altsig & 4) {
                            if (!fprintf(calllog, "%s;Incoming analog call;CPN %s;CPName %s\n", timeoutput(channum), isdninfo[channum].cpn, isdninfo[channum].displayie)) {
                                disp_msg("Failed to log");
                            }

                        fflush(calllog);
                        }
                    }

                    else {
                        if (altsig & 4) {
                            if (!fprintf(calllog, "%s;Incoming analog call;CPN %s;CPName unavailable\n", timeoutput(channum), isdninfo[channum].cpn)) {
                                disp_msg("Failed to log");
                            }

                            fflush(calllog);
                        }
                    }
                }

                dxinfox[ channum ].state = ST_OFFHOOK;
                set_hkstate(channum, DX_OFFHOOK);

                disp_status(channum, "Incoming Call");
            }

            break;

        case DE_TONEON:
            disp_msg("Hitting tone on handler");

            if (cstp->cst_data == TID_2600) {
                dx_stopch(chdev, EV_ASYNC);

                if (curstate == ST_2600ROUTE) {
                    disp_msg("Preparing to reset pseudo-trunk");

                    // Drop the currently active call and reset the detector to include MF

                    if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                    }

                    if (dx_setdigtyp(dxinfox[ channum ].chdev, D_MF) == -1) {
                        // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
                        // This really, *really* shouldn't fail...
                        disp_msg("Unable to set digit type to MF!");
                        disp_status(channum, "Unable to set digit type to MF!");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                        disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                        isdn_drop(channum, 41);
                        return (-1);
                    }

                    if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                        disp_msg("Holy shit! SCRoute1 threw an error!");
                        disp_err(channum, dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                        isdn_drop(connchan[channum], 41);
                        return (-1);
                    }

                    if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                        disp_msg("Holy shit! SCRoute2 threw an error!");
                        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                        isdn_drop(channum, 41);
                        return (-1);
                    }

                    set_hkstate(connchan[channum], DX_ONHOOK);
                }

                return (0);
            }

            break;

        case DE_TONEOFF:
            disp_msg("Toneoff event received!");

            if (cstp->cst_data == TID_2600) {
                if (ATDX_STATE(dxinfox[channum].chdev) != CS_IDLE) {
                    dxinfox[channum].state = ST_2600STOP;
                    dx_stopch(dxinfox[channum].chdev, EV_ASYNC | EV_NOSTOP);
                } else {
                    dxinfox[ channum ].state = ST_2600_1;
                    playtone_cad(channum, 2600, 0, 4);
                }

                return (0);

            }

            break;

        //DX_ONHOOK and DX_OFFHOOK are returned if a TDX_SETHOOK termination event is received.
        // case DX_OFFHOOK:
        // disp_msgf("DX_OFFHOOK Event (%d) Received on %s",
        // cstp->cst_event, ATDV_NAMEP( chdev ) );
        // disp_msgf("DX_OFFHOOK Event - %d", cstp->cst_event );
        case DX_ONHOOK:
            disp_msgf("DX_ONHOOK Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "DX_ONHOOK Event - %d", cstp->cst_event);
            break;

        case CST_BUSY:
            disp_msgf("CST_BUSY Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_BUSY Event - %d", cstp->cst_event);
            break;

        case CST_NOANS:
            disp_msgf("CST_NOANS Event (%d) Received on %s, state %d", cstp->cst_event, ATDV_NAMEP(chdev), dxinfox[channum].state);
            disp_statusf(channum, "CST_NOANS Event - %d", cstp->cst_event);
            break;

        case CST_NORB:
            disp_msgf("CST_NORB Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_NORB Event - %d", cstp->cst_event);
            break;

        case CST_CNCT:
            disp_msgf("CST_CNCT Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_CNCT Event - %d", cstp->cst_event);
            break;

        case CST_CEPT:
            disp_msgf("CST_CEPT Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_CEPT Event - %d", cstp->cst_event);
            break;

        case CST_STOPD:
            disp_msgf("CST_STOPD Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_STOPD Event - %d", cstp->cst_event);
            break;

        // These cases are conflicting. Somehow. DE_TONEON is the same value. TO DO: Move to a different switch?
        /*
        case CST_NO_DIALTONE:
        disp_msgf("CST_NO_DIALTONE Event (%d) Received on %s",
            cstp->cst_event, ATDV_NAMEP( chdev ) );
             disp_statusf(channum, "CST_NO_DIALTONE Event - %d", cstp->cst_event );
          break;
          case CST_FAXTONE:
        disp_msgf("CST_FAXTONE Event (%d) Received on %s",
            cstp->cst_event, ATDV_NAMEP( chdev ) );
             disp_statusf(channum, "CST_FAXTONE Event - %d", cstp->cst_event );
          break;
        */

        case CST_DISCONNECT:
            disp_msgf("CST_DISCONNECT Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_DISCONNECT Event - %d", cstp->cst_event);
            break;

        case CST_HKFLSHRCVD:
            disp_msgf("CST_HKFLSHRCVD Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "CST_HKFLSHRCVD Event - %d", cstp->cst_event);
            break;

        default:
            disp_msgf("Unknown CST Event (%d) Received on %s", cstp->cst_event, ATDV_NAMEP(chdev));
            disp_statusf(channum, "Unknown CST Event - %d", cstp->cst_event);
    }

    return (0);
}

/***************************************************************************
 *        NAME: int callprog_hdlr()
 * DESCRIPTION: TDX_CALLP event handler. Returned by dx_dial() or dx_dialtpt()
 *              to indicate that dialing with call progress analysis completed.
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 **************************************************************************/
int callprog_hdlr() {
    disp_msg("Entering callprog_hdlr()");

    if (fcntl(fileno(logdescriptor), F_GETFD) == -1) {
        disp_msg("Log file is closed, reopening");

        if (!openLog()) {
            disp_msg("Log opening operation failed");
        } else {
            disp_msg("Log file reopened");
        }
    }

    int chdev = sr_getevtdev();
    short channum = get_channum(chdev);

    if (channum == -1) {
        return (0);  // Message discarded; is STUPID!
    }

    // int curstate = dxinfox[channum].state;

    switch (ATDX_CPTERM(chdev)) {
        case CR_BUSY:
            if (!fprintf(logdescriptor, "%s;%s;BUSY;%ld;\n", timeoutput(channum), dialerdest[ channum ], ATDX_SIZEHI(chdev))) {
                disp_msg("Failed to log");
            }

            fflush(logdescriptor);
            // disp_msg( "Test call returned busy status" );
            break;

        // need the below semicolon after the label to allow compile
        case CR_CEPT:
            ;
            long tone_id = ATDX_CRTNID(chdev);

            switch (tone_id) {
                case TID_SIT_NC:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;No circuit found\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_IC:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;Operator intercept\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_VC:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;Vacant circuit\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_RO:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;Reorder\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_NC_INTERLATA:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;InterLATA no circuit found\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_RO_INTERLATA:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;InterLATA reorder\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_IO:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;Ineffective other\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case TID_SIT_ANY:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;Catch all\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                default:
                    if (!fprintf(logdescriptor, "%s;%s;SIT;%ld_%ld;%ld_%ld;%ld_%ld;%ld\n", timeoutput(channum), dialerdest[ channum ], ATDX_FRQHZ(chdev), ATDX_FRQDUR(chdev), ATDX_FRQHZ2(chdev), ATDX_FRQDUR2(chdev), ATDX_FRQHZ3(chdev), ATDX_FRQDUR3(chdev), ATDX_SIZEHI(chdev))) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;
            }

            break;

        case CR_CNCT:
            switch (ATDX_CONNTYPE(chdev)) {
                case CON_CAD:

                    // disp_msg( "Test call returned connection due to cadence break" );
                    if (!fprintf(logdescriptor, "%s;%s;DISCONNECT;CB\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case CON_LPC:

                    // disp_msg( "Test call returned connection due to loop current drop" );
                    if (!fprintf(logdescriptor, "%s;%s;DISCONNECT;LCD\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case CON_PVD:

                    // disp_msg( "Test call returned connection due to positive voice detection" );
                    // The last field is the length of whatever is heard (presumably voice) on the other end. This is partially for debug purposes.
                    if (!fprintf(logdescriptor, "%s;%s;CONNECT;PVD;%ld\n", timeoutput(channum), dialerdest[ channum ], ATDX_ANSRSIZ(chdev))) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case CON_PAMD:

                    // Is there any way to do this without so much crap?
                    // disp_msg( "Test call returned connection due to positive AM detection" );

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 1000 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));

                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        // dx_distone( dxinfox[ channum ].chdev, TID_FAX, DM_TONEON );
                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR1000\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 440 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR440\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 790 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR790\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 950 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR950\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 2000 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR2000\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 500 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR500\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 1400 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR1400\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 1330 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR1330\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 800 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR800\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_425, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 425 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR425\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 745 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR745\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_850, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 850 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR850\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1050, DM_TONEON)  == -1) {
                        disp_msgf("Uhh, shit, the 850 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                        // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                        // Remove the fax tone since we won't be doing tone detection
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);
                        dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_850, DM_TONEON);

                        if (!fprintf(logdescriptor, "%s;%s;CONNECT;PAMD;ERR1050\n", timeoutput(channum), dialerdest[ channum ])) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                        break;
                    }

                    // At this point, we'll do digit collection to look for these tones.

                    dxinfox[ channum ].state = ST_VMBDETECT;

                    if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                        disp_msgf("Cannot get digits from dialer channel %s", ATDV_NAMEP(chdev));
                    }

                    return (0);

            }

            break;

        case CR_ERROR:

            // disp_msg( "Error condition returned!");
            if (!fprintf(logdescriptor, "%s;%s;ERROR;CODE %li;\n", timeoutput(channum), dialerdest[ channum ], ATDX_CPERROR(dxinfox[ channum ].chdev))) {
                disp_msg("Failed to log");
            }

            fflush(logdescriptor);
            break;

        case CR_FAXTONE:

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON) == -1) {
                disp_msgf("Uhh, shit, the fax detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));

                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                if (!fprintf(logdescriptor, "%s;%s;FAX;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the modem detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the fax tone since we won't be doing tone detection
                dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;FAX;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON) == -1) {
                disp_msgf("Uhh, shit, the second modem detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the other tones since we won't be doing tone detection.
                dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;FAX;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
                break;
            }

            if (dx_enbtone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON)  == -1) {
                disp_msgf("Uhh, shit, the 1400 hertz detection tone was fucked. Error %s", ATDV_ERRMSGP(dxinfox[ channum ].chdev));

                // Since we can't do tone detection, let's just continue processing stuff semi-normally.
                // Remove the other tones since we won't be doing tone detection.
                dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);
                dx_distone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON);

                if (!fprintf(logdescriptor, "%s;%s;FAX;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                break;
            }

            // At this point, we'll do digit collection to look for these tones.

            dxinfox[ channum ].state = ST_MODEMDETECT;

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits from dialer channel %s", ATDV_NAMEP(chdev));
            }

            return (0);

        case CR_NOANS:

            // disp_msg( "Test call returned no answer condition");
            if (!fprintf(logdescriptor, "%s;%s;NO ANSWER;\n", timeoutput(channum), dialerdest[ channum ])) {
                disp_msg("Failed to log");
            }

            fflush(logdescriptor);
            break;

        case CR_NODIALTONE:

            // disp_msg( "Test call returned no dialtone condition");
            if (!fprintf(logdescriptor, "%s;%s;ERROR;NO DIALTONE\n", timeoutput(channum), dialerdest[ channum ])) {
                disp_msg("Failed to log");
            }

            fflush(logdescriptor);
            break;

        case CR_NORB:

            // disp_msg( "Test call returned no ringback condition");
            if (!fprintf(logdescriptor, "%s;%s;ERROR;NO RINGBACK\n", timeoutput(channum), dialerdest[ channum ])) {
                disp_msg("Failed to log");
            }

            fflush(logdescriptor);
            break;

        case CR_STOPD:

            // disp_msg( "Test call returned user abort condition");
            if (!fprintf(logdescriptor, "%s;%s;ABORT;USER ABORTED\n", timeoutput(channum), dialerdest[ channum ])) {
                disp_msg("Failed to log");
            }

            fflush(logdescriptor);
            break;

    }

    // Deal with scanning logic here; increment destination, etc
    scancount[ initchan[ channum ] ]--;

    if (scancount[ initchan[ channum ] ] <= 0) {
        dxinfox[ channum ].state = ST_GOODBYE;
        dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
    } else {
        dxinfox[ channum ].state = ST_CALLPTEST5;
        disp_status(channum, "Hanging up...");
    }

    set_hkstate(channum, DX_ONHOOK);

    return (0);
}

/***************************************************************************
 *        NAME: int dial_hdlr()
 * DESCRIPTION: TDX_DIAL event handler. Returned by dx_dial() or dx_dialtpt()
 *              to indicate that dialing without call progress analysis
 *              completed. 
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int dial_hdlr() {
    int channel;
    int chdev = sr_getevtdev();
    // int  event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on dial chdev");
    }

    int  channum = get_channum(chdev);
    int  curstate;
    // int  errcode = 0;

    if (channum == -1) {
        return (0);               /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;          /* Current State */
    disp_msgf("Attempting to handle dial event, terminated in state %i", curstate);
//   close( dxinfox[ channum ].msg_fd );

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */
    switch (frontend) {
        case CT_NTANALOG:
            if (ATDX_TERMMSK(chdev) & TM_LCOFF) {
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            break;

        case CT_NTT1:

            // Make sure it doesn't fuck up the call if we're outpulsing digits to an on-hook trunk
            if (((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) == 0) && dxinfox[ channum ].state != ST_ROUTED2) {
                return (0);
            }

            break;

        case CT_NTE1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) != 0) {
                return (0);
            }

            break;

        case CT_GCISDN:
            if (isdnstatus[channum] == 1) {
                return 0;
            }

            break;
    }

    switch (curstate) {
        case ST_CALLPTEST5:
            countup(dialerdest[ initchan[ channum ] ]);   // Increase the string by one number
            sprintf(dialerdest[ channum ], "%s", dialerdest[ initchan[ channum ] ]);
            disp_statusf(channum, "New dialer - dest: %s", dialerdest[ channum ]);
            set_hkstate(channum, DX_OFFHOOK);
            return (0);

        case ST_CALLPTEST4:

            disp_msg("Preparing to start wardial...");
            dxinfox[ channum ].state = ST_WTRING;

            if (dialprep(channum) == -1) {
                disp_msg("dialprep() failed! Returning to idle state.");
                return (-1);
            }

            channel = 1;

            if (!chaninfo[channum].dialer.using_list) {
                while ((channel <= maxchans) && (channel <= chans[ channum ]) && (scancount[ channum ] > 0)) {
                    if (dxinfox[channel].state == ST_WTRING) {
                        initchan[ channel] = channum;
                        dxinfox[ channel].state = ST_CALLPTEST4;
                        dx_initcallp(dxinfox[channel].chdev);  // Initialize call progress detection
                        sprintf(dialerdest[channel], "%s", dialerdest[channum]);   // Damn, you could just use channum...
                        disp_statusf(channel, "Dialer - dest: %s", dialerdest[channel]);
                        countup(dialerdest[channum]);   // Increase the string by one number
                        scancount[initchan[ ownies[ channum ] ] ]--;
                        set_hkstate(channel, DX_OFFHOOK);
                    }

                    else {
                        disp_msgf("Channel %d looks like it's still in use. Attempting to move to other channel...", channel);
                    }

                    channel++;
                }
            } else {
                const char *target;
                while ((channel <= maxchans) && (channel <= chans[ channum ]) && (scancount[ channum ] == 0)) {
                    if (dxinfox[channel].state == ST_WTRING) {
                        initchan[channel] = channum;
                        dxinfox[channel].state = ST_CALLPTEST4;
                        dx_initcallp(dxinfox[channel].chdev);  // Initialize call progress detection
                        chaninfo[channel].dialer.using_list = TRUE;
                        sprintf(dialerdest[channel], "%s", dialerdest[channum]);   // Damn, you could just use channum...
                        disp_statusf(channel, "List dialer - dest: %s", dialerdest[channel]);

                        target = dialer_list_next();

                        if (target == NULL) {
                            disp_msg("dialer_list_next(): out of numbers");
                            scancount[initchan[channum]] = 1;
                        } else {
                            strcpy(dialerdest[channel], target);
                        }

                        //countup( dialerdest[ initchan[ ownies [ channum ] ] ] ); // Increase the string by one number
                        //scancount[initchan[ ownies[ channum ] ] ]--;
                        set_hkstate(channel, DX_OFFHOOK);
                    } else {
                        disp_msgf("Channel %d looks like it's still in use. Attempting to move to other channel...", channel);
                    }

                    channel++;
                }

            }

            return (0);

        case ST_CRUDEDIAL2: {
            int length = strlen(filetmp[ channum ]);

            // Wow, this is *ugly*. Maybe make some more arrays...
            if (filetmp[channum][(length - 1)] < 0x39) {
                filetmp[channum][(length - 1)]++;

            }

            else {

                if (filetmp[channum][(length - 2)] < 0x39) {
                    filetmp[channum][(length - 2)]++;
                    filetmp[channum][(length - 1)] = 0x30;
                }

                else {
                    if (filetmp[channum][(length - 3)] < 0x39) {
                        filetmp[channum][(length - 3)]++;
                        filetmp[channum][(length - 2)] = 0x30;
                        filetmp[channum][(length - 1)] = 0x30;
                    }

                    else {
                        // We're done here.

                        dxinfox[ channum ].state = ST_ONHOOK;
                        set_hkstate(channum, DX_ONHOOK);
                        return (0);

                    }
                }

            }

            disp_msgf("New destination is %s", filetmp[channum]);
            dxinfox[ channum ].state = ST_CRUDEDIAL;

            if (dx_dial(dxinfox[ channum ].chdev, filetmp[channum], NULL, EV_ASYNC) != 0) {
                disp_msgf("Oh, shit! Crudedial with string %s failed!", filetmp[channum]);
                return (-1);
            }

            return (0);
        }

        case ST_CRUDEDIAL:

            sprintf(dxinfox[ channum ].msg_name, "sounds/crudedial/%s.wav", filetmp[channum]);
// Change back to .pcm if reverting back to record() function

            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name,
                                             O_RDWR | O_TRUNC | O_CREAT, 0644);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                return (-1);
            }

            if (recordwav(channum, dxinfox[ channum ].msg_fd) == 0) {
                return (0);
            }

            disp_msg("Coudldn't start recordwav() function");
            return (-1);

        case ST_OUTDIAL2:
            disp_msg("Dial op completed successfully");

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 3) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
            }

            return (0);

        case ST_OUTDIAL3:

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("DTMF Buffer clear fail on %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
                return (-1);
            }

            dxinfox[ channum ].state = ST_OUTDIAL;
            dxinfox[ channum ].msg_fd = open("sounds/barbe_dialnum.pcm", O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, "sounds/barbe_dialnum.pcm");
            }

            return (0);

        case ST_ROUTED2:
            // Cut-through magic goes here for tandem calls

            disp_msgf("Tandem chans are %i, %i", connchan[channum], channum);

            if (nr_scunroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                disp_msg("Holy shit! SCUnroute1 threw an error!");
                disp_err(channum, dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                return (-1);
            }

            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                disp_msg("Holy shit! SCUnroute2 threw an error!");
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            disp_status(connchan[channum], "Performing NMS tandem operation");
            dxinfox[channum].state = ST_ROUTED;
            dxinfox[ connchan[channum] ].state = ST_ROUTED; // Make sure the software knows to tear this down properly

    }

    return (0);


}


/***************************************************************************
 *        NAME: int play_hdlr()
 * DESCRIPTION: TDX_PLAY event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int play_hdlr() {
    int chdev = sr_getevtdev();
    // int event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on wink chdev");    // This looks like debug code. Should we remove it?
    }

    int channum = get_channum(chdev);
    int curstate;
    int errcode[MAXCHANS];
    errcode[channum] = 0;

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */

    /* Using a signed short here so that it can hold -1 if necessary. */
    for (short i = (closecount[channum] - 1); i >= 0; i--) {
        close(multiplay[channum][i]);
    }

    closecount[channum] = 0;
    
    /* Above for loop should be equivalent to this, leaving this to be sure just in case - AppleDash
       if (closecount[channum] != 0) {
        closecount[channum]--;

        while (closecount[ channum ] >= 0) {
            close(multiplay[channum][closecount[channum]]);
            closecount[channum]--;
        }

        closecount[channum] = 0;
    }*/

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */
    switch (frontend) {

        case CT_NTANALOG:
            if (ATDX_TERMMSK(chdev) & TM_LCOFF) {

                close(dxinfox[ channum ].msg_fd);
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            break;

        case CT_NTT1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) == 0) {
                // This code is for handling hangup events. We should probably make
                // it work for analog and E1 interfaces too. Just saying...

                if (termmask[channum] & 1) {
                //if ((ownies[channum] == 100) || (ownies[channum] == 200)) {
                    disp_msg("Removing custom tone with playback handler hangup");
                    dx_distone(chdev, TID_1, DM_TONEON);
                    dx_deltones(chdev);
                    ownies[channum] = 0; // Is this extraneous?
                    termmask[ channum ] ^= 1;
                    return (0);
                }

                if ((ownies[ channum ] == 5) && (strlen(bookmark[ channum ]) == 4)) {         // Are we doing category playback? Write the user's last position so they can pick up there later.
                    sprintf(filetmp2[ channum ], "resume/%s", bookmark[ channum ]);

                    if (stat(filetmp2[ channum ], &sts) == 0) {
                        resumefile[ channum ] = fopen(filetmp2[ channum ], "w");
                        playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);

                        if (playoffset[ channum ] > 16000) {
                            playoffset[ channum ] = (playoffset[ channum ] - 16000);    //Subtract a few seconds from the offset if it won't underflow; sometimes call teardown takes a sec
                        }

                        disp_msgf("Hangup at offset %lu with file %s", playoffset[ channum ], dxinfox[ channum ].msg_name);
                        fprintf(resumefile[ channum ], "%lu %s %d", playoffset[ channum ], filetmp[ channum ], anncnum[ channum ]);
                        fclose(resumefile[ channum ]);
                        ownies[ channum ] = 0;
                    }

                    close(dxinfox[ channum ].msg_fd);
                    return (0);
                }

                close(dxinfox[ channum ].msg_fd);   // Close any active file that remains

            }

            break;

        case CT_NTE1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) != 0) {
                return (0);
            }

            break;

        case CT_GCISDN:
            if (isdnstatus[channum] == 1) {

                if ((ownies[ channum ] == 5) && (strlen(bookmark[ channum ]) == 4)) {         // Are we doing category playback? Write the user's last position so they can pick up there later.
                    sprintf(filetmp2[ channum ], "resume/%s", bookmark[ channum ]);

                    if (stat(filetmp2[ channum ], &sts) == 0) {
                        resumefile[ channum ] = fopen(filetmp2[ channum ], "w");
                        playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);

                        if (playoffset[ channum ] > 16000) {
                            playoffset[ channum ] = (playoffset[ channum ] - 16000);    //Subtract a few seconds from the offset if it won't underflow; sometimes call teardown takes a sec
                        }

                        disp_msgf("Hangup at offset %lu with file %s", playoffset[ channum ], dxinfox[ channum ].msg_name);
                        fprintf(resumefile[ channum ], "%lu %s %d", playoffset[ channum ], filetmp[ channum ], anncnum[ channum ]);
                        fclose(resumefile[ channum ]);
                        ownies[ channum ] = 0;
                    }

                    close(dxinfox[ channum ].msg_fd);
                    return (0);
                }

                if (ownies[ channum ] == 9) {
                    dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);
                    dx_deltones(dxinfox[ channum ].chdev);
                }

                if (ownies[ channum ] == 66) {
                    // The caller hung up while leaving a message. Put it in the mailbox for them.
                    disp_msg("We're hitting the voicemail on-hook handler now.");
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "%s/new/%d.pcm", filetmp[channum], anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }

                    if (rename(filetmp2[ channum ], dxinfox[ channum ].msg_name) == 0) {
                        anncnum[ channum ] = 0;
                        errcnt[ channum ] = 0;
                    } else {
                        disp_msg("Holy shit, rename failed. You should fix this.");
                    }

                    // Added since we we were occasionally leaking file descriptors
                    close(dxinfox[channum].msg_fd);

                    return (0);
                }

                if ((ownies[ channum ] == 6) && (strlen(bookmark[ channum ]) == 4)) {         // Are we doing category playback? Write the user's last position so they can pick up there later.
                    sprintf(filetmp3[ channum ], "resume/%s", bookmark[ channum ]);

                    if (stat(filetmp3[ channum ], &sts) == 0) {
                        resumefile[ channum ] = fopen(filetmp3[ channum ], "w");

                        if (playoffset[ channum ] > 16000) {
                            playoffset[ channum ] = (playoffset[ channum ] - 16000);    //Subtract a few seconds from the offset if it won't underflow; sometimes call teardown takes a sec
                        }

                        disp_msgf("Hangup at offset %lu with file %s", playoffset[ channum ], filetmp2[ channum ]);
                        fprintf(resumefile[ channum ], "%lu %s %d", playoffset[ channum ], filetmp[ channum ], anncnum[ channum ]);
                        fclose(resumefile[ channum ]);
                        ownies[ channum ] = 0;
                    }

                    close(dxinfox[ channum ].msg_fd);
                    return (0);
                }

                if (ownies[ channum ] == 15) {
                    close(multiplay[ channum ][0]);
                    close(multiplay[ channum ][1]);
                    ownies[channum] = 0;
                    return (0);
                }

                if (ownies[ channum ] == 55) {
                    trollconf_offset = (ATDX_TRCOUNT(chdev) + 160000);
                    playoffset[channum] = lseek(dxinfox[ channum ].msg_fd, 0, SEEK_END);

                    if (trollconf_offset >= playoffset[channum]) {
                        trollconf_offset = 0;
                    }

                    playoffset[channum] = 0;
                    ownies[channum] = 0;
                    close(dxinfox[ channum ].msg_fd);
                }

                close(dxinfox[ channum ].msg_fd);

                return (0);

            }

            break;

    }

    switch (curstate) {
        case ST_INTRO:
                /* Need to Record a Message */
                dxinfox[ channum ].state = ST_RECORD;

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                msgnum[channum] = 0;
                msgcheck:
                sprintf(dxinfox[ channum ].msg_name, "message%d.wav", msgnum[channum]);

// Change back to .pcm if reverting back to record() function
                if (stat(dxinfox[ channum ].msg_name, &sts) == 0) {
                    msgnum[channum]++;
                    goto msgcheck;
                }

                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name,
                                                 O_RDWR | O_TRUNC | O_CREAT, 0666);

                if (dxinfox[ channum ].msg_fd == -1) {
                    disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                    errcode[channum] = -1;
                }

                if (errcode[channum] == 0) {

                    errcode[channum] = recordwav(channum, dxinfox[ channum ].msg_fd);
                    /*         if ( dx_clrdigbuf( chdev ) == -1 ) {
                                disp_msgf("Cannot clear DTMF Buffer for %s",
                                            ATDV_NAMEP( chdev ) );
                                            } */
            }

            break;

        case ST_ISDNNWN:
            close(dxinfox[channum].msg_fd);
            if (isdn_drop(channum, UNASSIGNED_NUMBER) == -1) {
                set_hkstate(channum, DX_ONHOOK);
            }
            return 0;


        case ST_TC24ARG:
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][0] = open(dxinfox[ channum ].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][1] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][2] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][3] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][4] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][5] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][6] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][7] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][8] = open(dxinfox[channum].msg_name, O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
            multiplay[channum][9] = open(dxinfox[channum].msg_name, O_RDONLY);
            if (playmulti( channum, 10, 0x00, multiplay[channum]) == -1) {
                unsigned char counter;
                for (counter = 0; counter < 10; counter++) {
                    close(multiplay[channum][counter]);
                }
                dxinfox[ channum ].state = ST_GOODBYE;
                disp_msg("Couldn't play Riverito message!");
                play(channum, errorfd, 0, 0, 0);
                return -1;
             }
             return 0;

        case ST_TC24CALLDB:
            close(dxinfox[channum].msg_fd);
        case ST_TC24CALLE:
            if (errcnt[channum] > 2) {
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return 0;
            }
            dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_enternum.pcm", O_RDONLY);
            dxinfox[ channum ].state = ST_TC24CALL;
            if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                file_error(channum, "sounds/tc24/tc_enternum.pcm");
                return -1;
            }
            return 0;

        case ST_TC24CALL2E:
            if (errcnt[channum] > 2) {
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return 0;
            }
            else {
                // Read back for confirmation
                //unsigned short length = strlen(dialout_prefix);
                multiplay[channum][0] = open("sounds/tc24/challenging.pcm", O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][0]);
                multiplay[channum][1] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][1]);
                multiplay[channum][2] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][2]);
                multiplay[channum][3] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][3]);
                multiplay[channum][4] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][4]);
                multiplay[channum][5] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][5]);
                multiplay[channum][6] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][6]);
                multiplay[channum][7] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][7]);
                multiplay[channum][8] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][8]);
                multiplay[channum][9] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", filetmp[channum][9]);
                multiplay[channum][10] = open(dxinfox[channum].msg_name, O_RDONLY);
                multiplay[channum][11] = open("sounds/tc24/right.pcm", O_RDONLY);
                dxinfox[ channum ].state = ST_TC24CALL2;
                if (playmulti( channum, 12, 0x80, multiplay[channum]) == -1) {
                    unsigned char counter;
                    for (counter = 0; counter < 12; counter++) {
                        close(multiplay[channum][counter]);
                    }
                    dxinfox[ channum ].state = ST_GOODBYE;
                    disp_msg("Couldn't play confirmation message!");
                    play(channum, errorfd, 0, 0, 0);
                    return -1;
                }
                return 0;
            }

        case ST_TC24CALL2:
            get_digs(channum, &dxinfox[ channum ].digbuf, 1, 50, 0x200);
            return 0;

        case ST_TC24CALL:
            close(dxinfox[ channum ].msg_fd);
            get_digs(channum, &dxinfox[ channum ].digbuf, 10, 50, 0x120F);
            return 0;

        case ST_TC24MENU2:
            close(dxinfox[ channum ].msg_fd);
            dx_clrdigbuf(dxinfox[channum].chdev);
            dxinfox[ channum ].state = ST_TC24MENU;
            dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error( channum, "sounds/tc24/greeting.pcm" );
                return -1;
            }
            return 0;

        case ST_TC24MENU1E:
            if (errcnt[channum] > 2) {
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_TC24MENU1;
                dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_howtomenu.pcm", O_RDONLY);
                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    file_error( channum, "sounds/tc24/tc_howtomenu.pcm" );
                    return -1;
                }
                return 0;
            }

        case ST_TC24MENUE:
            if (errcnt[channum] > 2) {
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_TC24MENU;
                dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    file_error( channum, "sounds/tc24/greeting.pcm" );
                    return -1;
                }
                return 0;
            }

        case ST_TC24MENU:
            close(dxinfox[ channum ].msg_fd);
            // Digis 1-8 should be considered terminating, but we should collect a maximum of two digits so ** can work.
            get_digs(channum, &dxinfox[ channum ].digbuf, 2, 0x1FE, 0x2200);
            return 0;

        case ST_ACTIVATIONOP:
            close(dxinfox[ channum ].msg_fd);
            connchan[ channum ] = idle_trunkhunt( channum, 1, maxchans, false );
            if (connchan[ channum ] == -1) {
                // Hang the call up w/ACB condition
                if (isdn_drop(channum, 34) == -1) { // Q.850 34 == All Circuits Busy
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }
                return -1;
            }
            connchan[connchan[channum]] = channum; // Should this be done in the trunk hunting function?
            set_cpname(connchan[channum], "ASSHOLE WITHOUT PHONE SVC", isdninfo[channum].cpn);
            makecall(connchan[channum], "3000", "", FALSE);
            // makecall(connchan[channum], "3000", "", FALSE);
            dxinfox[channum].state = ST_ROUTED;
            if (altsig & 4) {
                if (!fprintf(calllog, "%s;Outgoing call to Shadytel Op;CPN %s\n", timeoutput(channum), isdninfo[channum].cpn)) {
                    disp_msg("Failed to log");
                }
                fflush(calllog);
            }
            return 0;

        case ST_ACTIVATIONPS:
            close(dxinfox[ channum ].msg_fd);
            // Dispatch order to busy out the line here
            if (strcmp(config.extensions.activation, isdninfo[ channum ].dnis) == 0) {
                sprintf(filetmp2[channum], "%s", config.provisiondn);
            }

            else {
                sprintf(filetmp2[channum], "%s", config.altprovisiondn);
            }

            if (stat(filetmp2[channum], &sts) == -1) {
                mkdir(filetmp2[channum], 0666);    // Create the directory if it's not there
            }

            sprintf(filetmp2[channum], "%s/%s.ord", filetmp2[channum], isdninfo[ channum ].cpn);
            resumefile[ channum ] = fopen(filetmp2[ channum ], "w");
            if (resumefile[channum] == NULL) {
                disp_msgf("ERROR: Couldn't create order file for subscriber %s!", isdninfo[ channum ].cpn);
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                memset(filetmp2[channum], 0x00, sizeof(filetmp2[channum]));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return -1;
            }
            fprintf(resumefile[ channum ], "%s,BUSY,%s,%s,", isdninfo[ channum ].cpn, config.provisiondn, config.altprovisiondn);
            fclose(resumefile[channum]);
            dxinfox[ channum ].state = ST_ONHOOK;
            set_hkstate(channum, DX_ONHOOK);
            return 0;

        case ST_TC24BBSREC:
        case ST_TC24BBS:
        case ST_TC24MENU1:
        case ST_ADMINADD2:
            close(dxinfox[ channum ].msg_fd);
            get_digs(channum, &dxinfox[ channum ].digbuf, 1, 50, 0x200);
            return 0;

        case ST_ADMINADD3E:
                if (errcnt[channum] > 2) {
                ownies[channum] = 0;
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_ADMINADD3;
                dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterpass.pcm", O_RDONLY);
                if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                    file_error(channum, "sounds/activation/activationivr_admin_enterpass.pcm");
                    return -1;
                }
                return 0;
            }


        case ST_ADMINADD2E:
                if (errcnt[channum] > 2) {
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_ADMINADD2;
                dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_addmenu.pcm", O_RDONLY);
                if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                    file_error(channum, "sounds/activation/activationivr_admin_addmenu.pcm");
                    return -1;
                }
                return 0;
            }


        case ST_ADMINADDF:
            close(dxinfox[ channum ].msg_fd);
        case ST_ADMINADDE:
            if (errcnt[channum] > 2) {
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_ADMINADD;
                dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
                if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                    file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                    return -1;
                }
                return 0;
            }

        case ST_ADMINACT3E:
            if (errcnt[channum] > 2) {
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_ADMINACT3;
                dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_newpass.pcm", O_RDONLY);
                if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                    file_error(channum, "sounds/activation/activationivr_admin_newpass.pcm");
                    return -1;
                }
                return 0;
            }

        case ST_ADMINACT2E:
            if (errcnt[channum] > 2) {
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return 0;
            }
            else {
                dxinfox[ channum ].state = ST_ADMINACT2A;
                dxinfox[channum].msg_fd = open("sounds/activation/activationivr_adminmenu.pcm", O_RDONLY);
                if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                    file_error(channum, "sounds/activation/activationivr_adminmenu.pcm");
                    return -1;
                }
                return 0;
            }

        case ST_ADMINACT2A:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_ADMINACT2;
            get_digs(channum, &dxinfox[ channum ].digbuf, 1, 50, 0x200);
            return 0;

        case ST_ADMINACT2:
            close(multiplay[channum][0]);
            close(multiplay[channum][1]);
            get_digs(channum, &dxinfox[ channum ].digbuf, 1, 50, 0x200);
            return 0;

        case ST_ADMINACTE:
            if (errcnt[channum] > 2) {
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 1);
                return (0);
            }
            dxinfox[ channum ].state = ST_ADMINACT;
            dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
            if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                return -1;
            }
            return 0;


        case ST_MUSIC:
        close(dxinfox[channum].msg_fd);
        dx_clrdigbuf(dxinfox[channum].chdev);
        sprintf(dxinfox[ channum ].msg_name, "sounds/music/%d.pcm", random_at_most(maxannc[channum]));
        dxinfox[channum].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
        }
        return (0);

        case ST_ACTIVATION4:
            // Clean up
            close(multiplay[channum][0]);
            close(multiplay[channum][1]);
            set_hkstate(channum, DX_ONHOOK);
            return 0;

        case ST_ADMINACT3:
        case ST_ADMINACT:
        case ST_ACTIVATION3:
        case ST_ADMINADD:
        case ST_ADMINADD3:
            // Get passcode
            close(dxinfox[channum].msg_fd);
            //dx_clrdigbuf(dxinfox[channum].chdev);
            get_digs(channum, &dxinfox[ channum ].digbuf, 4, 50, 0x200);
            return 0;

        case ST_ACTIVATION2E:
            dxinfox[ channum ].state = ST_ACTIVATION2;
            dxinfox[ channum ].msg_fd = open("sounds/activation/activation_enterext.pcm", O_RDONLY);
            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                file_error(channum, "sounds/activation/activation_enterext.pcm");
                return -1;
            }
            return 0;

        case ST_ACTIVATION2:
            // Get extension number
            close(dxinfox[channum].msg_fd);
            get_digs(channum, &dxinfox[ channum ].digbuf, 4, 50, 0x200);
            return 0;

        case ST_ACTIVATIONF:
            close(dxinfox[channum].msg_fd);
        case ST_ACTIVATIONE:
            dxinfox[channum].state = ST_ACTIVATION;
            dxinfox[ channum ].msg_fd = open("sounds/activation/activation_intro.pcm", O_RDONLY);
            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                file_error(channum, "sounds/activation/activation_intro.pcm");
                return -1;
            }
        return 0;

        case ST_ACTIVATION:
            close(dxinfox[ channum].msg_fd);
            get_digs(channum, &dxinfox[ channum ].digbuf, 1, 50, 0x200);
            return 0;

        case ST_RDNISREC:
        close(dxinfox[channum].msg_fd);
        dx_clrdigbuf(dxinfox[channum].chdev);
        sprintf(dxinfox[ channum ].msg_name, "sounds/phreakspots/%d.pcm", random_at_most(maxannc[channum]));
        dxinfox[channum].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1)  == -1) {
            file_error(channum, dxinfox[ channum ].msg_name);
        }
        return (0);       

        case ST_CONFWAITSIL:
            close(multiplay[ channum ][0]);
            return (0);

        case ST_CONFCONF:
        case ST_CONFWAIT:
            // I don't want to allocate new variables. Will an unsigned long work here?
            playoffset[channum] = (unsigned long) ATDX_TERMMSK(dxinfox[ channum ].chdev);

            if (playoffset[channum] & TM_MAXDTMF) {
                dx_clrdigbuf(dxinfox[channum].chdev);
            }

            if (playoffset[channum] & TM_USRSTOP) {
                close(multiplay[channum][0]);
                close(multiplay[channum][1]);
                return (0);
            }

            else {
                close(multiplay[channum][1]);
                sprintf(dxinfox[ channum ].msg_name, "sounds/confhold/%d.pcm", random_at_most(maxannc[channum]));
                multiplay[channum][1] = open(dxinfox[channum].msg_name, O_RDONLY);
                play(channum, multiplay[channum][1], 1, 0, 0);
                disp_msgf("Playing %s", dxinfox[channum].msg_name);
                return (0);
            }

            return (0);

        case ST_2600STOP:
            dxinfox[ channum ].state = ST_2600_1;
            playtone_cad(channum, 2600, 0, 4);
            return (0);

        case ST_SOUNDTEST:
            close(dxinfox[ channum ].msg_fd);
            filerrcnt[channum]++;

            switch (filerrcnt[ channum ]) {
                case 1:
                    // 6 KHz Dialogic ADPCM
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0);
                    break;

                case 2:
                    // 8 KHz Dialogic ADPCM
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 4, 0, 0);
                    break;

                case 3:
                    // a-Law
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 2, 0, 0);
                    break;

                case 4:
                    // 8-bit signed
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 5, 0, 0);
                    break;

                case 5:
                    // g.726
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 3, 0, 0);
                    break;

                case 6:

                    // g.721
                    if (!dm3board) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, goodbyefd, 0, 0, 1);
                        return (0);
                    }

                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 7, 0, 0);
                    break;

                case 7:
                    // 16-bit signed
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 6, 0, 0);
                    break;

                case 8:
                    // IMA ADPCM
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 8, 0, 0);
                    break;

                default:
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);

            }

            return (0);

        case ST_2600_1:
            close(dxinfox[ channum ].msg_fd);

            if (ATDX_TERMMSK(chdev) & TM_USRSTOP) {
                // Seeeeeize!
                if (dx_setdigtyp(dxinfox[ channum ].chdev, D_MF) == -1) {
                    // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
                    // This really, *really* shouldn't fail...
                    disp_msg("Unable to set digit type to MF!");
                    disp_status(channum, "Unable to set digit type to MF!");
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                return (0);
            }

            else {

                ownies[channum] = 0;
                dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);
                dx_deltones(dxinfox[ channum ].chdev);
                set_hkstate(channum, DX_ONHOOK);

            }

            return (0);

        case ST_CATPAUSE:
            close(dxinfox[ channum ].msg_fd);
            // Wait for digits to come back indefinitely.
            get_digs(channum, &dxinfox[ channum ].digbuf, 1, 0, 0x200);
            return (0);

        case ST_DIALSUPE:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_GOODBYE;
            set_hkstate(channum, DX_OFFHOOK);
            dxinfox[channum].msg_name[(strlen(dxinfox[channum].msg_name) - 5)]++;  //Increase relevant portion of the filename by 1
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (dxinfox[channum].msg_fd == -1) {
                play(channum, errorfd, 0, 0, 1);
            } else {
                playwav(channum, dxinfox[ channum ].msg_fd);
            }

            return (0);

        case ST_ISDNACB:

            if (isdn_drop(channum, NO_CIRCUIT_AVAILABLE) == -1) {
                set_hkstate(channum, DX_ONHOOK);
            }

            return (0);



        case ST_EVANSDM3:

            lig_if_followup[channum]--;

            // Close outstanding files from last playback. lig_if_followup can only be a maximum of 2, so this should be sufficient.
            close(multiplay[ channum ][0]);

            if (lig_if_followup[channum] > 0) {
                close(multiplay[ channum ][ lig_if_followup[channum] ]);

                if (lig_if_followup[channum] > 1) {
                    close(multiplay[ channum ][ 1 ]);
                }
            }

            lig_if_followup[ channum ] = (random_at_most(3) + 1);
            ownies[ channum ] = 0;

            if (lig_if_followup[ channum ] >= 3) {
                ligmain[ channum ] = random_at_most(15);
                sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-init%d.wav", ligmain[ channum ]);
                multiplay[ channum ][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                ownies[channum]++;
            }

            ligmain[ channum ] = random_at_most(97);
            dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, SV_ADD8DB);   // Reset volume to normal
            sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-%d.wav", ligmain[ channum ]);
            multiplay[channum][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (multiplay[channum][ownies[channum]] == -1) {
                file_error(channum, dxinfox[ channum ].msg_name);
                return (-1);
            }

            if (lig_if_followup[channum] > 1) {
                ownies[channum]++;
                ligmain[ channum ] = random_at_most(56);
                sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-followup%d.wav", ligmain[ channum ]);
                multiplay[channum][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);
            }

            disp_msg("Playing back Evans script continuation");
            errcode[channum] = playmulti(channum, lig_if_followup[channum], 1, multiplay[channum]);
            return (errcode[channum]);

        case ST_ISDNROUTE:
            close(dxinfox[ channum ].msg_fd);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            dxinfox[ channum ].state = ST_ISDNTEST;
            playtone(channum, 350, 0);
            return (0);

        case ST_CALLPTEST3E:

            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_CALLPTEST3;
            // sprintf( dxinfox[ channum ].msg_name, "sounds/enter_numbers.pcm" );
            dxinfox[ channum ].msg_fd = open("sounds/enter_numbers.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

            if (errcode[channum] == -1) {
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (errcode[channum]);

        case ST_CALLPTEST4:

            close(dxinfox[ channum ].msg_fd);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            // Initialize the logging function

//   if ( ( logdescriptor = fopen( "scans.log", "a+" ) ) == -1 ) {
//              disp_msg( "Log opening operation failed");
//              return(-1);
//        }
            openLog();

            if (!fprintf(logdescriptor, "%s;SCAN;%s;%d;%d\n", timeoutput(channum), dialerdest[ channum ], chans[ channum ], scancount[channum])) {
                disp_msg("Failed to log");
                openLog();
            }

            fflush(logdescriptor);

            dx_clrcap(&cap); //Clears any previous changes to the default CAP
            cap.ca_pamd_failtime = 1500; //* 10ms - default 4s
            cap.ca_noanswer = 3000; //* 10ms - default 30s
            //DX_PVDOPTNOCON SIT Voice and Fax - DX_PAMDOPTEN SIT AM Voice and Fax
            cap.ca_intflg = DX_PAMDOPTEN; //DX_PVDOPTNOCON;
            cap.ca_pamd_spdval = PAMD_ACCU;
            // These settings are for SIT tones.
            cap.ca_mxtimefrq = 50;

            cap.ca_lower2frq = 1350;
            cap.ca_upper2frq = 1450;
            cap.ca_time2frq = 5;
            cap.ca_mxtime2frq = 50;

            cap.ca_lower3frq = 1725;
            cap.ca_upper3frq = 1825;
            cap.ca_time3frq = 5;
            cap.ca_mxtime3frq = 50;

            ownies[ channum ] = 1;
            set_hkstate(channum, DX_ONHOOK);

            return (0);

        case ST_CALLPTEST3:

            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 7, 110, 0) == -1) {
                //if ( get_digits( channum, &(dxinfox[ channum ].digbuf), 7 ) == -1 ) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        case ST_CALLPTEST2:

            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 2, 50, 0) == -1) {
                //if ( get_digits( channum, &(dxinfox[ channum ].digbuf), 2 ) == -1 ) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        case ST_CALLPTEST2E:

            dxinfox[ channum ].state = ST_CALLPTEST2;
            sprintf(dxinfox [ channum ].msg_name, "sounds/enter_channels.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

            if (errcode[channum] == -1) {
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (errcode[ channum ]);

        case ST_CALLPTESTE:

            dxinfox[ channum ].state = ST_CALLPTEST;
            sprintf(dxinfox[ channum ].msg_name, "sounds/enter_startnum.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (errcode[channum]);


        case ST_CALLPTEST:

            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 17, 70, 0x200) == -1) {
                // if ( get_digits( channum, &(dxinfox[ channum ].digbuf), 14 ) == -1 ) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        case ST_PLAYMULTI1:
            while (ownies[channum] < 255) {
                close(file[ownies[channum]]);
                ownies[channum]--;
            }

            ownies[channum] = 0;
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, goodbyefd, 0, 0, 0);
            return (0);

        case ST_PLAYMULTI:
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, goodbyefd, 0, 0, 0);
            return (0);

        case ST_COLLCALL3:
            close(dxinfox[ channum ].msg_fd);

            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            dxinfox[channum].state = ST_COLLCALL;
            sprintf(dxinfox[ channum ].msg_name, "sounds/collect/menu.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                file_error(channum, "sounds/collect/menu.pcm");
            }

            return (0);

        case ST_VMAILSETMP:

            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILSETM;
            sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_settingsmenu.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);



        case ST_VMAILCOMP:

            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 4, 50, 0) == -1) {
                //if (get_digits( channum, &(dxinfox[ channum ].digbuf), 4 ) == -1 ) {
                disp_msgf("Cannot get digits for voicemail composition thing, channel %s", ATDV_NAMEP(chdev));
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        case ST_VMAILRNEW4:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILRNEW2;
            sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_messagemenu.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILRNEW3:
            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], anncnum[ channum ]);

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                close(dxinfox[ channum ].msg_fd);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/endofmsg.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                anncnum[channum] = 0;
                dxinfox[ channum ].state = ST_VMAILCHECK1;
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            dxinfox[ channum ].state = ST_VMAILRNEW;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILHEADER4:

            close(dxinfox[ channum ].msg_fd);
            ownies[ channum ] = 1; // This is necessary for readback
            dxinfox[ channum ].state = ST_VMREADBACK;
            sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", filetmp3[channum][0]);

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILHEADER3:

            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILHEADER4;
            sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_fromnum.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILHEADER2:
            // Consider consolidating this with ST_VMAILHEADER and using the ownies variable

            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILHEADER3;
            sprintf(dxinfox[ channum ].msg_name, "sounds/time/%x.pcm", time2[channum]);

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILHEADER:
            // Consider consolidating this with ST_VMAILHEADER2 and using the ownies variable
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILHEADER2;
            sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%x.pcm", time1[channum]);

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILRNEW:
            // Consider consolidating this with the saved message function and using ownies to indicate the message type.
            close(dxinfox[channum].msg_fd);
            sprintf(filetmp3[ channum ], "%s/%d.atr", filetmp2[channum], anncnum[channum]);

            if ((resumefile[ channum ] = fopen(filetmp3[ channum ], "r")) != NULL) {
                // Yeah, that's right - I'm reusing the resumefile pointer for voicemail stuff.
                // Big whoop, wanna fight about it?
                fscanf(resumefile[ channum ], "%c %c %s", &time1[ channum ], &time2[ channum ], filetmp3[channum]);
                // fscanf( resumefile[ channum ], "%x %x %s", &time1[ channum ], &time2[ channum ], filetmp3[channum]);
                // Should we reuse a variable on this one too? Using a single int would be more CPU intensive yet
                // use the same amount of RAM as a couple chars
                fclose(resumefile[ channum ]);
                dxinfox[ channum ].state = ST_VMAILHEADER;

                if ((dxinfox[ channum ].msg_fd = open("sounds/vmail/msgreceived_at.pcm", O_RDONLY)) == -1) {
                    disp_msg("Failure playing sounds/vmail/msgreceived_at.pcm");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                // Go straight to the options for repeat/delete/whatever
                dxinfox[ channum ].state = ST_VMAILRNEW2;
                strcpy(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_messagemenu.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }


        case ST_VMAILRSAVED:
            // Temporary code

            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return (0);

        case ST_VMAILPASS1:
            dxinfox[ channum ].state = ST_VMAILPASS;
            dxinfox[ channum ].msg_fd = open("sounds/ivr_enterpass.pcm", O_RDONLY);
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILMENU:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILMENU2;
            strcpy(dxinfox[ channum].msg_name, "sounds/vmail/vmb_mainmenu.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILCHECK1:
            // Voicemail message checker function begins here.
            close(dxinfox[ channum ].msg_fd);
            sprintf(filetmp2[channum], "%s/new", filetmp[channum]);
            errcnt[ channum ] = 1;
            anncnum[ channum ] = 0;

            while (errcnt[ channum ] == 1) {
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[channum], anncnum[channum]);

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    errcnt[ channum ] = 0;
                } else {
                    anncnum[ channum ]++;
                }
            }

            newmsg[channum] = anncnum[channum];
            anncnum[channum] = 0;
            ownies[ channum ] = 110;

            //strcpy(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_youhave.pcm");
            multiplay[channum][0] = open("sounds/vmail/vmb_youhave.pcm", O_RDONLY);
            sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.pcm", newmsg[channum]);
            multiplay[channum][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
            //strcpy(dxinfox[ channum ].msg_name, "sounds/vmail/newmsg.pcm");
            multiplay[channum][2] = open("sounds/vmail/newmsg.pcm", O_RDONLY);
            //strcpy(dxinfox[ channum ].msg_name, "sounds/vmail/and.pcm");
            multiplay[channum][3] = open("sounds/vmail/and.pcm", O_RDONLY);

            sprintf(filetmp2[channum], "%s/old", filetmp[channum]);
            errcnt[ channum ] = 1;
            anncnum[ channum ] = 0;

            while (errcnt[ channum ] == 1) {
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[channum], anncnum[channum]);

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    errcnt[ channum ] = 0;
                } else {
                    anncnum[ channum ]++;
                }
            }

            oldmsg[channum] = anncnum[channum];
            anncnum[channum] = 0;
            sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.pcm", oldmsg[channum]);

            multiplay[channum][4] = open(dxinfox[ channum ].msg_name, O_RDONLY);
            //strcpy(dxinfox[ channum ].msg_name, "sounds/vmail/savedmsg.pcm");
            multiplay[channum][5] = open("sounds/vmail/savedmsg.pcm", O_RDONLY);

            dxinfox[channum].state = ST_VMAILMENU;

            if ((errcode[channum] = playmulti(channum, 6, 128, multiplay[channum])) == -1) {
                disp_msg("VMAILCHECK1 function passed bad data to the playmulti function.");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (0);

        case ST_VMAILGRECEDIT1E:
            dxinfox[channum].state = ST_VMAILGRECEDIT1;
            close(dxinfox[ channum ].msg_fd);
            strcpy(dxinfox[ channum ].msg_name, "sounds/ivr_recmenu.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);


        case ST_VMAILSETGREC:
        case ST_VMAILSETGREC2:
            close(dxinfox[ channum ].msg_fd);

            // dxinfox[ channum ].state = ST_VMAILGRECEDIT2;
            // Start greeting recording function here.
            if (dxinfox[ channum ].state == ST_VMAILSETGREC) {
                sprintf(dxinfox[ channum ].msg_name, "%s/greeting.pcm", filetmp[channum]);
            } else {
                sprintf(dxinfox[ channum ].msg_name, "%s/temp/greeting.pcm", filetmp[channum]);
            }

            dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name,  O_RDWR | O_TRUNC | O_CREAT, 0666);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                errcode[channum] = (-1) ;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = record(channum, dxinfox[ channum ].msg_fd, 1, DM_P);
            }

            return (errcode[channum]);

        case ST_VMAILGRECEDIT2:
            dxinfox[ channum ].state = ST_VMAILSETGREC;

        case ST_VMAILRNEW2:
        case ST_VMAILMENU2:
        case ST_VMAILGRECEDIT1:
        case ST_VMAILGRECEDIT3:
        case ST_VMAILSETUP2:
        case ST_VMAILSETM:
        case ST_VMAILSETUP1C:
        case ST_VMAILTYP:
        case ST_VMAILTYP2:
        case ST_VMAILNPASS1C:
        case ST_COLLCALL:
            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 1, 50, 0) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        case ST_VMAILNPASS1E:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_VMAILNPASS1;
            strcpy(dxinfox[ channum ].msg_name, "sounds/ivr_enterpass.pcm");

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, dxinfox[ channum ].msg_name);
            }

            return (0);


        case ST_VMAILSETUP1E:
            dxinfox[ channum ].state = ST_VMAILSETUP1;
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, dxinfox[ channum ].msg_name);
            }

            return (0);

        case ST_VMAILNPASS1:
        case ST_VMAILSETUP1:
            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 6, 70, 0) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        case ST_FAKECONF_ERR:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_FAKECONF1;
            dxinfox[ channum ].msg_fd = open("sounds/meetme-welcome.pcm", O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, "sounds/meetme-welcome.pcm");
                return (-1);
            } else {
                return (0);
            }

        case ST_FAKECONF3:
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1);
            return (0);

        case ST_FAKECONF2:

            close(dxinfox[ channum ].msg_fd);
            strcpy(dxinfox[ channum ].msg_name, "sounds/fakeconf.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            dxinfox[channum].state = ST_FAKECONF3;
            ownies[channum] = 55;
            play(channum, dxinfox[ channum ].msg_fd, 1, trollconf_offset, 1);
            return (0);

        case ST_FAKECONF1:

            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 6, 70, 0) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            return (0);

        case ST_PLAYLOOP:

            playwav(channum, dxinfox[ channum ].msg_fd);
            return (0);


        case ST_ISDNTEST_TEMPMENU:
            // This is a workaround for the under-construction state of things.
            close(dxinfox[ channum ].msg_fd);
            strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_testgreet.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msg("Failure playing ISDN testline greeting");

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state = ST_ISDNTEST;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNTEST_NUTYPE2:
            close(dxinfox[ channum ].msg_fd);

            switch ((isdninfo[channum].callingtype & 0x0F)) {

                case 0x0F:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/custom.pcm");
                    break;

                case 0x09:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/private.pcm");
                    break;

                case 0x08:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/national.pcm");
                    break;

                case 0x04:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/telex.pcm");
                    break;

                case 0x03:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/data.pcm");
                    break;

                case 0x01:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/telephony.pcm");
                    break;

                default:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/unknown.pcm");

            }

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state++;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);


        case ST_ISDNTEST_NUTYPE:
            close(dxinfox[ channum ].msg_fd);
            strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/numplantype.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state++;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNTEST_CPTYPE2:
            close(dxinfox[ channum ].msg_fd);

            switch ((isdninfo[channum].callingtype & 0x70)) {

                case 0x70:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/custom.pcm");
                    break;

                case 0x60:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/abbreviated.pcm");
                    break;

                case 0x40:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/subscriber.pcm");
                    break;

                case 0x30:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/netspecific.pcm");
                    break;

                case 0x20:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/national.pcm");
                    break;

                case 0x10:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/international.pcm");
                    break;

                default:
                    strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/unknown.pcm");

            }

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state++;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);


        case ST_ISDNTEST_CPTYPE:
            close(dxinfox[ channum ].msg_fd);
            strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/cnumtype_is.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state++;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);



        case ST_ISDNTEST_CPNREAD3:
            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", isdninfo[channum].cpn[ownies[channum]]);
            ownies[channum]++;

            if ((isdninfo[channum].cpn[ownies[channum]]) == '\0') {
                ownies[channum] = 0;
                dxinfox[channum].state++;

            }

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNTEST_CPNREAD2:
            close(dxinfox[ channum ].msg_fd);

            if (isdninfo[channum].cpn[0] == '\0') {  // Is this a null character? If so, we're done here.
                strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/notreceived.pcm");
                dxinfox[channum].state = (dxinfox[channum].state + 2);
            }

            else {
                sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", isdninfo[channum].cpn[0]);
                dxinfox[channum].state++;
                ownies[channum] = 1;
            }

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNTEST_CPNREAD:
            close(dxinfox[ channum ].msg_fd);
            strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/callingpartynumis.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state++;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);


        case ST_ISDNTEST_CPNDREAD2:
            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", isdninfo[channum].dnis[ownies[channum]]);
            ownies[channum]++;

            if ((isdninfo[channum].dnis[ownies[channum]]) == '\0') {
                ownies[channum] = 0;
                dxinfox[channum].state++;
            }


            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);


        case ST_ISDNTEST_CPNDREAD:
            close(dxinfox[ channum ].msg_fd);

            if (isdninfo[channum].dnis[0] == '\0') {  // Is this a null character?
                strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/notreceived.pcm");
                dxinfox[channum].state = (dxinfox[channum].state + 2);
            }

            else {
                sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", isdninfo[channum].dnis[0]);
                dxinfox[channum].state++;
                ownies[channum] = 1;
            }

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNERR2:

            strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_entercause.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state = ST_ISDNTEST_ENDCAUSE;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNTEST_ENDCAUSE2:
            if (isdn_drop(channum, causecode[channum]) == -1) {
                disp_msg("Holy shit! isdn_drop() failed! D:");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            disp_status(channum, "ISDN channel ready!");
            return (0);

        case ST_ISDNERR:
            strcpy(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_testgreet.pcm");

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msg("Failure playing ISDN testline greeting");

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            dxinfox[channum].state = ST_ISDNTEST;
            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_ISDNTEST_ENDCAUSE:
            close(dxinfox[channum].msg_fd);

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 3) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            return (0);

        case ST_ISDNTEST:
            close(dxinfox[channum].msg_fd);

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));

                if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                    disp_msg("Holy shit! isdn_drop() failed! D:");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (-1);
            }

            return (0);

        case ST_SASTROLL:
            close(dxinfox[ channum ].msg_fd);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msg("Cannot get digits for bridge troll");
                return (-1);
            }

            return (0);

        case ST_VMAILPASS:
            close(dxinfox[ channum ].msg_fd);

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 6, 70, 0) == -1) {
                //if ( get_digits( channum, &(dxinfox[ channum ].digbuf), 6 ) == -1 ) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                return (-1);
            }


            return (0);

        case ST_VMAIL4:
            dxinfox[ channum ].state = ST_VMAIL3;
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].msg_fd = open("sounds/vmail/recmenu.pcm", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, "sounds/vmail/recmenu.pcm");
            }

            return (0);


        case ST_VMAIL2:

            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].msg_fd = open(filetmp2[channum],  O_RDWR | O_TRUNC | O_CREAT, 0666);

            if (dxinfox[ channum ].msg_fd == -1) {
                file_error(channum, filetmp2[channum]);
            }

            if (errcode[channum] == 0) {
                if (ownies[channum] == 66) {
                    ownies[channum] = 6;
                }

                errcode[channum] = record(channum, dxinfox[ channum ].msg_fd, 1, DM_P);
            }

            return (errcode[channum]);

        case ST_VMAIL1:

            close(dxinfox[ channum ].msg_fd);


            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {


                if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                    disp_msg("Cannot get digits from voicemail greeting handler");
                    return (-1);
                }

                return (0);
            }


            msgnum[channum] = 0;
            errcnt[channum] = 0; // Let's reset the error counter. For... reasons.
            ownies[channum] = 0; // This'll hold the result of the stat() operation.

            if (vmattrib[ channum ] & 2) {

                // The second bit indicates announce only.
                dxinfox[ channum ].state = ST_GOODBYE;

                if ((errcode[channum] = play(channum, goodbyefd, 0, 0, 0)) == -1) {
                    disp_msg("Experienced a problem dismissing a caller");
                }

                return (0);

            }

            dxinfox[ channum ].state = ST_VMAIL2;

            disp_msgf("Current file prefix is %s", filetmp[channum]);

            while ((ownies[channum] == 0) && (msgnum[channum] != 255)) {
                msgnum[channum]++;
                sprintf(filetmp2[channum], "%s/temp/%d.pcm", filetmp[channum], msgnum[channum]);
                ownies[channum] = stat(filetmp2[channum], &sts);

            }

            disp_msgf("Current filename is %s", filetmp2[channum]);
            ownies[channum] = 6;

            if (msgnum[channum] == 255) {
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 0);
                return -1;    // Temporary file queue is full. Something isn't working right. Let's stop.
            }

            dxinfox[ channum ].msg_fd = open("sounds/vmail/beginmsg.pcm", O_RDONLY);

            if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                file_error(channum, "sounds/vmail/beginmsg.pcm");
            }

            return (0);

        case ST_VMAIL5:
        case ST_EMREC3:
        case ST_DCBBSREC3:
        case ST_TC24BBSREC4:
            // This is to stop this naughty code from leaking file descriptors.
            if (fcntl(dxinfox[channum].msg_fd, F_GETFD) != -1) close(dxinfox[ channum ].msg_fd);

            if (errcnt[ channum ] >= 3) {
                errcnt[ channum ] = 0;

                if (msgnum[channum] != 0) { // Make sure this wasn't reset. We don't want to delete anything useful.

                    if (dxinfox[ channum ].state == ST_EMREC3) {
                        sprintf(filetmp[channum], "sounds/emtanon/temp/%d.pcm", msgnum[channum]);   // Delete user's recording; they hung up or something.
                        remove(filetmp[ channum ]);
                    }

                    else if (dxinfox[ channum ].state == ST_DCBBSREC3) {
                        sprintf(filetmp[channum], "sounds/tc24/dcbbs/temp/%d.pcm", msgnum[channum]);   // Delete user's recording; they hung up or something.
                        remove(filetmp[ channum ]);
                    }

                    else if (dxinfox[ channum ].state == ST_TC24BBSREC4) {
                        sprintf(filetmp[channum], "sounds/tc24/bbs/temp/%d.pcm", msgnum[channum]);   // Delete user's recording; they hung up or something.
                        remove(filetmp[ channum ]);
                    }

                    else {
                        // Actual voicemail? Save it.
                        anncnum[ channum ] = 0;
                        errcnt[ channum ] = 1;

                        while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "%s/new/%d.pcm", filetmp[channum], anncnum[ channum ]);

                            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                                errcnt[ channum ] = 0;
                            } else {
                                anncnum[ channum ]++;
                            }
                        }

                        sprintf(filetmp3[channum], "%s/new/%d.atr", filetmp[channum], anncnum[channum]);
                        resumefile[channum] = fopen(filetmp3[channum], "w+");
                        vmtimeoutput(channum);

                        if (resumefile[channum] != NULL) {
                            if (strlen(isdninfo[channum].cpn) > 0 ) fprintf(resumefile[channum], "%c %c %s", time1[channum], time2[channum], isdninfo[channum].cpn);
                            else fprintf(resumefile[channum], "%c %c e", time1[channum], time2[channum] );
                            fclose(resumefile[channum]);
                        }

                        if (rename(filetmp2[ channum ], dxinfox[ channum ].msg_name) == 0) {
                            anncnum[ channum ] = 0;
                            errcnt[ channum ] = 0;
                        } else {
                            disp_msg("Holy shit, rename failed. You should fix this.");
                        }
                    }

                }

                dxinfox[ channum ].state = ST_GOODBYE;
                dx_clrdigbuf(chdev);

                if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                }

                return (0);
            }

            else {
                if (dxinfox[ channum ].state == ST_EMREC3) {
                    dxinfox[ channum ].state = ST_EMREC2;
                    strcpy(dxinfox[ channum ].msg_name, "sounds/emtanon/message_options.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording isn't working");
                    }
                }
                else if (dxinfox[ channum ].state == ST_DCBBSREC3) {
                    dxinfox[ channum ].state = ST_DCBBSREC2;
                    strcpy(dxinfox[ channum ].msg_name, "sounds/emtanon/message_options.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording isn't working");
                    }
                }
                else if (dxinfox[ channum ].state == ST_TC24BBSREC4) {
                    dxinfox[ channum ].state = ST_TC24BBSREC3;
                    strcpy(dxinfox[ channum ].msg_name, "sounds/emtanon/message_options.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording isn't working");
                    }
                } else {
                    dxinfox[ channum ].state = ST_VMAIL3;
                    strcpy(dxinfox[ channum ].msg_name, "sounds/vmail/recmenu.pcm");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording isn't working");
                    }

                }
            }

            return (0);

        case ST_VMAIL3:
        case ST_EMREC2:
        case ST_DCBBSREC2:
        case ST_TC24BBSREC3:
            close(dxinfox[ channum ].msg_fd);

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits in Emtanon BBS recorder, channel %s", ATDV_NAMEP(chdev));
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return -1;
            }

            return (0);

        case ST_DCBBSREC:
        case ST_TC24BBSREC2:
        case ST_EMREC1:
            close(dxinfox[ channum ].msg_fd);
            switch(dxinfox[ channum ].state) {
                case ST_TC24BBSREC2:
                    sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/temp/%d.pcm", msgnum[channum]);
                    break;
                case ST_DCBBSREC:
                    sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/temp/%d.pcm", msgnum[channum]);
                    break;
                default:
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/temp/%d.pcm", msgnum[channum]);
            }
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDWR | O_TRUNC | O_CREAT, 0666);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return -1;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = record(channum, dxinfox[ channum ].msg_fd, 1, DM_P);
            }

            return (errcode[channum]);

        case ST_EMPLAY3:
            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            dxinfox[ channum ].state = ST_EMPLAY1;

            if (dxinfox[ channum ].msg_fd != '\0') {
                close(dxinfox[ channum ].msg_fd);    //I'd rather not have this check, but it stops unclosed file descriptor problems.
            }

            dxinfox[ channum ].msg_fd = open("sounds/emtanon/greeting_new.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

            if (errcode[channum] != 0) {
                disp_msg("There's a problem with the alt greeting");
            }

            return (errcode[channum]);

        case ST_EMPLAY1:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_EMPLAY1;

            if ((errcode[channum] = get_digits(channum, &(dxinfox[ channum ].digbuf), 1)) == -1) {
                // So, uhh, you ever think about putting a variable in for state and not modifying this every damn time you make a new function?
                disp_msgf("Oh, shit! Digit collecting for the OTP nerds failed on channel %s !", ATDV_NAMEP(chdev));
            }

            return (errcode[channum]);


        case ST_RESUMEMARK3:
            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
            errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if (errcode[channum] != -1) {
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
            }

            dxinfox[ channum ].state = ST_GETCAT3;
            return (errcode[channum]);

        case ST_RESUMEMARK:
        case ST_MAKEMARK: // Initial state for bookmark creation/resumption
            close(dxinfox[ channum ].msg_fd);

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 4) == -1) {
                disp_msgf("Cannot get digits in makemark state, channel %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            return (0);

        case ST_RESUMEMARK2: // Return to category number prompt
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_GETCAT;
            strcpy(dxinfox[ channum ].msg_name, "sounds/ivr_entercat.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (errcode[channum]);

        case ST_OUTDIALSB:
            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            dxinfox[ channum ].state = ST_OUTDIAL2;

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 3) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                return (-1);
            }

            return (0);

        case ST_OUTDIAL:
            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 11) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
            }

            return (0);

        case ST_MSGREC3:
            if (ownies[ channum ] != 3) {
                close(dxinfox[ channum ].msg_fd);
            } else {
                ownies[ channum ] = 0;
            }

            disp_msg("Going to msgrec2. So much MSG...");
            strcpy(dxinfox[ channum ].msg_name, "sounds/ivr_recmenu.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                disp_msg("The topic recording menu dun goofed, bro");
                return (-1);
            }

            dxinfox[ channum ].state = ST_MSGREC2;
            return (0);

        case ST_MSGREC:

            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "%s", filetmp3[ channum ]);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name,  O_RDWR | O_TRUNC | O_CREAT, 0666);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                errcode[channum] = (-1) ;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = record(channum, dxinfox[ channum ].msg_fd, 2, DM_S);    // No silence detection here.
            }

            return (errcode[channum]);

        case ST_CATREC2:
            if (ownies[ channum ] != 3) {
                close(dxinfox[ channum ].msg_fd);
            } else {
                ownies[ channum ] = 0;
            }

            disp_msg("Going to catrec2. So many cats...");
            sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_recmenu.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                disp_msg("The topic recording menu dun goofed, bro");
                return (-1);
            }

            dxinfox[ channum ].state = ST_CATREC3;
            return (0);

        case ST_CATREC:
            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "%s/temp/title.pcm", filetmp[ channum ]);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name,  O_RDWR | O_TRUNC | O_CREAT, 0666);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                errcode[channum] = (-1) ;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = record(channum, dxinfox[ channum ].msg_fd, 1, DM_S);
            }

            return (errcode[channum]);

        case ST_ENTERPASS:
            close(dxinfox[ channum ].msg_fd);

            if ((errcode[channum] = get_digs(channum, &dxinfox[ channum ].digbuf, 6, 70, 0)) == -1) {
                disp_msgf("Cannot get digits in catresume or catcreate, channel %s", ATDV_NAMEP(chdev));
            }

            return (0);

        case ST_CATMENU2:
            close(dxinfox[ channum ].msg_fd);

            if ((errcode[channum] = get_digits(channum, &(dxinfox[ channum ].digbuf), 1)) == -1) {
                disp_msgf("Cannot get digits in catresume or catcreate, channel %s", ATDV_NAMEP(chdev));
            }

            return (errcode[channum]);

        case ST_CATMENU:
            if (ownies[ channum ] != 3) {
                close(dxinfox[ channum ].msg_fd);
            } else {
                ownies[ channum ] = 0;
            }

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if ((loggedin[ channum ] == 0) && (userposts[ channum ] == 0)) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_usermenu2.pcm");
            }

            if ((loggedin[ channum ] == 0) && (userposts[ channum ] == 1)) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_usermenu1.pcm");
            }

            if ((loggedin[ channum ] == 1) && (userposts[ channum ] == 0)) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_ownermenu1.pcm");
            }

            if ((loggedin[ channum ] == 1) && (userposts[ channum ] == 1)) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_ownermenu2.pcm");
            }

            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                disp_msg("The menu recording dun goofed, bro");
                return (-1);
            }

            dxinfox[ channum ].state = ST_CATMENU2;
            return (0);


        case ST_PASSCREATE2:
            close(dxinfox[ channum ].msg_fd);
            sprintf(readback[ channum ], "%s", dxinfox[ channum ].digbuf.dg_value);

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits in passcreate2 state, channel %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            return (0);

        case ST_PASSCREATE:
            close(dxinfox[ channum ].msg_fd);   // Make sure this is needed!

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 6) == -1) {
                disp_msgf("Cannot get digits in passcreate state, channel %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            return (0);

        //Just get one digit and move back over to the digit handler. Press 1 for yes/2 for no sorta deal.
        case ST_CATCREATE:
        case ST_CATRESUME:
        case ST_PASSCREATE3:
        case ST_CATREC3:
        case ST_MSGREC2:
            // if ( dxinfox[ channum ].state == ST_CATREC3 ) dxinfox[ channum ].state = ST_CATREC;
            close(dxinfox[ channum ].msg_fd);

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits in catresume or catcreate, channel %s", ATDV_NAMEP(chdev));
            }

            return (0);


        case ST_GETCAT3:
            close(dxinfox[ channum ].msg_fd);

            // dx_adjsv( dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0 ); // Reset volume to normal
            if ((ATDX_TERMMSK(chdev) & TM_MAXDTMF) || ownies[ channum ] == 1) {
                if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                    disp_msgf("Cannot get digits from clownboat %s", ATDV_NAMEP(chdev));
                }

                if (ownies[ channum ] == 1) {
                    ownies[ channum ] = 0;
                }

                // The following was written by someone who didn't *quite* grasp C. Maaaaybe revise this at some point. Or just get rid of it.
                /* The ownies variable is important; after receiving another playback event, it'll fall
                into the else function below. Otherwise, there's no way to make it do *just* that. */
                return (0);
            }

            else {
                if (errcnt[channum] == 0) {
                    playoffset[channum] = 0; // Reset play offset counter if we just hit the end of a file.
                }

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                ownies[ channum ] = 1;
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                dxinfox[ channum ].msg_fd = open("sounds/ivr_backfwd.pcm", O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Couldn't play the back/forward IVR message on channel %s, error was %s", ATDV_NAMEP(chdev), ATDV_ERRMSGP(chdev));
                }

                return (0);
            }


        case ST_GETCAT2:
            close(dxinfox[ channum ].msg_fd);
            sprintf(dxinfox[ channum ].msg_name, "%s/title.pcm", filetmp[ channum ]);

            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_doesnotexist.pcm");
                // Use the generic title if none is available ^
            }

            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                disp_msgf("Cannot play category title %s", dxinfox[ channum ].msg_name);
                return (-1);
            }

            dxinfox[ channum ].state = ST_CATMENU;

            return (0);

        case ST_GETCAT:
            if (ownies[ channum ] == 3) {
                ownies[ channum ] = 0;
            } else {
                close(dxinfox[ channum ].msg_fd);    // Make an exception so we don't close the file descriptor for the invalid recording
            }

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 3) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            return (0);

        case ST_VMREADBACK:
            close(dxinfox[ channum ].msg_fd);

            if (filetmp3[channum][ownies[ channum ] ] == '\0') {   // '\0' is a null character; if the variable is empty, execute this.
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                ownies[ channum ] = 0;

                dxinfox[ channum ].state = ST_VMAILRNEW2;
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_messagemenu.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", filetmp3[channum][ ownies[ channum ] ]);
            ownies[ channum ]++;
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, dxinfox[channum].msg_name);
            }

            return (0);


        case ST_ANAC:
        case ST_PASSREADBACK:
        case ST_MSGREAD:
            close(dxinfox[ channum ].msg_fd);

            if (dxinfox[ channum ].digbuf.dg_value[ ownies[ channum ] ] == '\0') {   // '\0' is a null character; if the variable is empty, execute this.
                if (dxinfox[ channum ].state == ST_ANAC) {
                    disp_msg("Exiting ANAC state...");
                    dxinfox[ channum ].state = ST_ONHOOK;
                    set_hkstate(channum, DX_ONHOOK);
                    return (0);
                }


                else {
                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    /*
                    if ( dxinfox[ channum ].state == ST_VMREADBACK ) {
                        ownies[ channum ] = 0;

                        dxinfox[ channum ].state = ST_VMAILRNEW2;
                        sprintf( dxinfox[ channum ].msg_name, "sounds/vmail/vmb_messagemenu.pcm" );
                        if ( (dxinfox[ channum ].msg_fd = open( dxinfox[ channum ].msg_name, O_RDONLY ) ) == -1 ) {
                           disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name );
                           dxinfox[channum].state = ST_GOODBYE;
                           play( channum, goodbyefd, 0, 0 );
                           return(-1);
                        }
                        play( channum, dxinfox[ channum ].msg_fd, 1, 0 );
                        return(0);
                        }
                     */


                    if (dxinfox[ channum ].state == ST_MSGREAD) {
                        dxinfox[ channum ].msg_fd = open("sounds/ivr_startrec.pcm", O_RDONLY);
                        dxinfox[ channum ].state = ST_MSGREC;

                        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                            file_error(channum, dxinfox[channum].msg_name);
                            return (-1);
                        }

                        return (0);
                    }

                }

                dxinfox[ channum ].msg_fd = open("sounds/ivr_accept12.pcm", O_RDONLY);
                dxinfox[ channum ].state = ST_PASSCREATE3;

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    file_error(channum, dxinfox[channum].msg_name);
                    return (-1);
                }

                ownies[ channum ] = 0;
                return (0);

            }

            sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", dxinfox[ channum ].digbuf.dg_value[ ownies[ channum ] ]);
            ownies[ channum ]++;
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, dxinfox[channum].msg_name);
            }

            return (0);

        case ST_PLAY:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_GOODBYE;

            if (dx_clrdigbuf(chdev) == -1) {

                disp_msgf("DTMF Buffer clear fail on %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
            }

            break;

        case ST_EVANS1:
            close(dxinfox[ channum ].msg_fd);
            ligmain[ channum ] = random_at_most(97);
            lig_if_followup[ channum ] = random_at_most(3);

            if (lig_if_followup[ channum ] == 0) {
                dxinfox[ channum ].state = ST_EVANS1;    // If we draw 0 from the prng, bring it back here
            }

            if (lig_if_followup[ channum ] == 1) {
                dxinfox[ channum ].state = ST_EVANS2;    // Send it to the follow phrase bank if it's 1
            }

            if (lig_if_followup[ channum ] == 2) {
                dxinfox[ channum ].state = ST_EVANS3;
            }

            disp_msgf("Clip number is %d", ligmain[ channum ]);
            sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-%d.wav", ligmain[ channum ]);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                errcode[channum] = -1;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = playwav(channum, dxinfox[ channum ].msg_fd);
            } else {
                disp_msgf("Shit, Evans state %d threw an error", dxinfox[ channum ].state);
            }

            return (errcode[channum]);

        case ST_EVANS2: // Round two. FIGHT!
            close(dxinfox[ channum ].msg_fd);
            lig_if_followup[ channum ] = random_at_most(2);

            if (lig_if_followup [ channum ] == 0) {
                dxinfox[ channum ].state = ST_EVANS1;
            }

            if (lig_if_followup [ channum ] == 1) {
                dxinfox[ channum ].state = ST_EVANS3;
            }

            ligmain[ channum ] = random_at_most(56);
            disp_msgf("Followup clip number is %d", ligmain[ channum ]);
            sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-followup%d.wav", ligmain[ channum ]);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                errcode[channum] = -1;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = playwav(channum, dxinfox[ channum ].msg_fd);
            } else {
                disp_msgf("Shit, Evans state %d threw an error", dxinfox[ channum ].state);
            }

            return (errcode[channum]);

        case ST_EVANS3: // This generates the last random file number and plays it for The Evans Effect
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_EVANS1;
            ligmain[ channum ] = random_at_most(15);
            disp_msgf("Init clip number is %d", ligmain[ channum ]);
            sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-init%d.wav", ligmain[ channum ]);
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                errcode[channum] = -1;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = playwav(channum, dxinfox[ channum ].msg_fd);
            } else {
                disp_msgf("Shit, Evans state %d threw an error", dxinfox[ channum ].state);
            }

            return (errcode[channum]);

        case ST_PLAYNWN:
            disp_msg("Okay, so we got to the thing after the recording....");
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_ONHOOK;

            if (set_hkstate(channum, DX_ONHOOK) == -1) {
                disp_msg("Error setting the system on-hook.");
            }

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("DTMF Buffer clear fail on %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            if (altsig & 1) {
                if (dt_xmitwink(dxinfox[ channum ].tsdev, 0) == -1) {
                    disp_msgf("Something went wrong with winkback. Error %s", ATDV_ERRMSGP(chdev));
                }
            }

            break;

        case ST_ENIGMAREC:
            close(dxinfox[ channum ].msg_fd);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("DTMF Buffer clear fail on %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            time(&rawtime);
            timeinfo = localtime(&rawtime);
            strftime(dxinfox[ channum ].msg_name, 32, "enigmarec_%m-%d_%H_%M.wav", timeinfo);
            filecount[ channum ] = (stat(dxinfox[ channum ].msg_name, &sts));
            filerrcnt[ channum ] = 0;

            while (filecount[ channum ] == 0) {
                filecount[ channum ] = (stat(dxinfox[ channum ].msg_name, &sts));

                if (filecount[ channum ] == 0) {
                    filerrcnt[ channum ]++;
                    sprintf(dxinfox[ channum ].msg_name, "%s_%i", dxinfox[ channum ].msg_name, filerrcnt[ channum ]);
                }

                disp_msgf("Enigmarec file is %s", dxinfox[ channum ].msg_name);

                if (filerrcnt[ channum ] == 5) {
                    // Has stat returned 0 five times in a row? It's lost it's mind. I'm outta here, you amateurs!
                    dxinfox[ channum ].state = ST_GOODBYE;
                    return (-1);

                }
            }

            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDWR | O_TRUNC | O_CREAT, 0666);

            if (dxinfox[ channum ].msg_fd == -1) {
                disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                return (-1);
            }

            if ((recordwav(channum, dxinfox[ channum ].msg_fd)) == -1) {
                disp_msg("Couldn't initiate record process in the Enigmarec function.");
                return (-1);
            }

            break;

        case ST_ENIGMAREC2:
            close(dxinfox[ channum ].msg_fd);

            if (ownies[ channum ] == 1) {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dx_stopch(chdev, EV_ASYNC);

                while (ATDX_STATE(chdev) != CS_IDLE);     // For some reason, the channel isn't idle immediately after digit collection, so this is needed

                dxinfox[ channum ].msg_fd = open("sounds/longrec_menu.pcm", O_RDONLY);
                ownies[ channum ] = 0;

                /* The ownies variable is important; after receiving another playback event, it'll fall
                into the else function below. Otherwise, there's no way to make it do *just* that. */
                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Couldn't play longrec message on channel %s, error %s", ATDV_NAMEP(chdev), ATDV_ERRMSGP(chdev));
                }

                break;
            }

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                break;
            }

        case ST_TC24BBS2:
        case ST_DCBBS:
        case ST_TCTUTORIAL:
        case ST_EMPLAY2:
        case ST_DYNPLAY:

            close(dxinfox[ channum ].msg_fd);

            // dx_adjsv( dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0 ); // Reset volume to normal
            if ((ATDX_TERMMSK(chdev) & TM_MAXDTMF) || ownies[ channum ] == 1) {
                if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                    disp_msgf("Cannot get digits from clownboat %s", ATDV_NAMEP(chdev));
                }

                if (ownies[ channum ] == 1) {
                    ownies[ channum ] = 0;
                }

                /* The ownies variable is important; after receiving another playback event, it'll fall
                into the else function below. Otherwise, there's no way to make it do *just* that. */
                return (0);
                break;
            }

            else {

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                ownies[ channum ] = 1;
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                if (dxinfox[ channum ].state == ST_DYNPLAY) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_betabackfwd.pcm");
                } else {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_embackfwd.pcm");
                }

                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Couldn't play the back/forward IVR message on channel %s, error was %s", ATDV_NAMEP(chdev), ATDV_ERRMSGP(chdev));

                }

                break;
            }

        case ST_CALLPTEST5:
            close(dxinfox[ channum ].msg_fd);
            set_hkstate(channum, DX_ONHOOK);
            break;
        case ST_INVALID:
        case ST_GOODBYE:
        default:
            close(dxinfox[ channum ].msg_fd);
            dxinfox[ channum ].state = ST_ONHOOK;
            set_hkstate(channum, DX_ONHOOK);
            break;
    }

    return (0);
}


/***************************************************************************
 *        NAME: int record_hdlr()
 * DESCRIPTION: TDX_RECORD event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int record_hdlr() {
    int chdev = sr_getevtdev();
    // int event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on record chdev");
    }

    int channum = get_channum(chdev);
    int curstate;

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */

    close(dxinfox[ channum ].msg_fd);

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */
    switch (frontend) {
        case CT_NTANALOG:
            if (ATDX_TERMMSK(chdev) & TM_LCOFF) {
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            break;

        case CT_NTT1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) == 0) {
                // This code is for handling hangup events. We should probably make
                // it work for analog and E1 interfaces too. Just saying...


                if (ownies[channum] == 6) {
                    // The caller hung up while leaving a message. Put it in the mailbox for them.
                    disp_msg("We're hitting the voicemail on-hook handler now.");
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "%s/new/%d.pcm", filetmp[channum], anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }

                    //  sprintf( filetmp[channum], "%s", filetmp2[channum]); // This is sloppy. Sloppy, sloppy, sloppy. Sorry.
                    sprintf(filetmp3[channum], "%s/new/%d.atr", filetmp[channum], anncnum[channum]);
                    resumefile[channum] = fopen(filetmp3[channum], "w+");
                    vmtimeoutput(channum);

                    if (resumefile[channum] != NULL) {
                        if (strlen(isdninfo[channum].cpn) > 0 ) fprintf(resumefile[channum], "%c %c %s", time1[channum], time2[channum], isdninfo[channum].cpn);
                        else fprintf(resumefile[channum], "%c %c e", time1[channum], time2[channum] );
                        fclose(resumefile[channum]);
                    }

                    if (rename(filetmp2[ channum ], dxinfox[ channum ].msg_name) == 0) {
                        anncnum[ channum ] = 0;
                        errcnt[ channum ] = 0;
                    } else {
                        disp_msg("Holy shit, rename failed. You should fix this.");
                    }
                }

                return (0);
            }

            break;

        case CT_NTE1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) != 0) {
                return (0);
            }

            break;

        case CT_GCISDN:
            if (isdnstatus[channum] == 1) {

                if (ownies[channum] == 6) {
                    // The caller hung up while leaving a message. Put it in the mailbox for them.
                    disp_msg("We're hitting the voicemail on-hook handler now.");
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "%s/new/%d.pcm", filetmp[channum], anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }

                    //  sprintf( filetmp[channum], "%s", filetmp2[channum]); // This is sloppy. Sloppy, sloppy, sloppy. Sorry.
                    sprintf(filetmp3[channum], "%s/new/%d.atr", filetmp[channum], anncnum[channum]);
                    resumefile[channum] = fopen(filetmp3[channum], "w+");
                    vmtimeoutput(channum);

                    if (resumefile[channum] != NULL) {
                        if (strlen(isdninfo[channum].cpn) > 0 ) fprintf(resumefile[channum], "%c %c %s", time1[channum], time2[channum], isdninfo[channum].cpn);
                        else fprintf(resumefile[channum], "%c %c e", time1[channum], time2[channum] );
                        fclose(resumefile[channum]);
                    }

                    if (rename(filetmp2[ channum ], dxinfox[ channum ].msg_name) == 0) {
                        anncnum[ channum ] = 0;
                        errcnt[ channum ] = 0;
                    } else {
                        disp_msg("Holy shit, rename failed. You should fix this.");
                    }

                    return (0);
                }

                if (ownies[channum] == 7) {
                    // If someone hangs up on the greeting record function, we go here
                }

                return (0);

            }

            break;
    }

//   switch ( curstate ) {
    switch (dxinfox[ channum ].state) {

        case ST_2600STOP:
            dxinfox[ channum ].state = ST_2600_1;
            playtone_cad(channum, 2600, 0, 4);
            return (0);

        case ST_ROUTEDREC2:
        case ST_ROUTEDREC:
            // For transaction recording. Removed close(), considering the handler does that before the switch statement now. No segfaulting for me, thank you.
            dxinfox[channum].state = ST_ONHOOK;
            return (0);

        case ST_CRUDEDIAL:

            dx_stopch(chdev, EV_ASYNC);

            while (ATDX_STATE(chdev) != CS_IDLE);

            dxinfox[ channum ].state = ST_CRUDEDIAL2;

            if (dx_dial(dxinfox[ channum ].chdev, "T*,", NULL, EV_ASYNC) != 0) {
                disp_msgf("Oh, shit! Crudedial with string %s failed!", filetmp[channum]);
                return (-1);
            }

            return (0);

        case ST_TC24BBSREC2:

            dxinfox[ channum ].state = ST_TC24BBSREC3;
            close(dxinfox[ channum ].msg_fd);
            // sprintf( dxinfox[ channum ].msg_name, "sounds/emtanon/message_options.vox");
            dxinfox[ channum ].msg_fd = open("sounds/emtanon/message_options.vox", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0) == -1) {
                disp_msg("Couldn't play the message recorder menu. Whoops...");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 0);
                return -1;
            }

            return 0;

        case ST_DCBBSREC:

            dxinfox[ channum ].state = ST_DCBBSREC2;
            close(dxinfox[ channum ].msg_fd);
            // sprintf( dxinfox[ channum ].msg_name, "sounds/emtanon/message_options.vox");
            dxinfox[ channum ].msg_fd = open("sounds/emtanon/message_options.vox", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0) == -1) {
                disp_msg("Couldn't play the message recorder menu. Whoops...");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 0);
                return -1;
            }

            return 0;

        case ST_EMREC1:

            dxinfox[ channum ].state = ST_EMREC2;
            close(dxinfox[ channum ].msg_fd);
            // sprintf( dxinfox[ channum ].msg_name, "sounds/emtanon/message_options.vox");
            dxinfox[ channum ].msg_fd = open("sounds/emtanon/message_options.vox", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0) == -1) {
                disp_msg("Couldn't play the message recorder menu. Whoops...");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 0);
                return -1;
            }

            return 0;

        case ST_VMAILSETGREC:
        case ST_VMAILSETGREC2:
            if (dxinfox[ channum ].state == ST_VMAILSETGREC) {
                dxinfox[ channum ].state = ST_VMAILGRECEDIT1;
            } else {
                dxinfox[ channum ].state = ST_VMAILGRECEDIT3;
            }

            close(dxinfox[ channum ].msg_fd);
            // sprintf( dxinfox[ channum ].msg_name, "sounds/ivr_recmenu.pcm");
            dxinfox[ channum ].msg_fd = open("sounds/ivr_recmenu.pcm", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                disp_msg("Couldn't play the message recorder menu. Whoops...");
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 0);
                return (-1);
            }

            return (0);


        case ST_VMAIL2:
        case ST_MSGREC:

            // This should be rewritten to have less checks
            if (dxinfox[ channum ].state == ST_VMAIL2) {
                dxinfox[ channum ].state = ST_VMAIL3;
            } else {
                dxinfox[ channum ].state = ST_MSGREC2;
            }

            close(dxinfox[ channum ].msg_fd);

            if (dxinfox[ channum ].state == ST_VMAIL3) {
                dxinfox[ channum ].msg_fd = open("sounds/vmail/recmenu.pcm", O_RDONLY);
                // sprintf( dxinfox[ channum ].msg_name, "sounds/vmail/recmenu.pcm");
                ownies[ channum ] = 66;
            } else {
                dxinfox[ channum ].msg_fd = open("sounds/ivr_recmenu.pcm", O_RDONLY);
            }

            // else sprintf( dxinfox[ channum ].msg_name, "sounds/ivr_recmenu.pcm");
            // dxinfox[ channum ].msg_fd = open( dxinfox[ channum ].msg_name, O_RDONLY );
            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, dxinfox[ channum ].msg_name);
                return (-1);
            }

            return (0);

        case ST_CATREC:

            if (ownies[ channum ] != 3) {
                close(dxinfox[ channum ].msg_fd);
            } else {
                ownies[ channum ] = 0;
            }

            dxinfox[ channum ].state = ST_CATREC3;
            sprintf(filetmp3[ channum ], "%s", dxinfox[ channum ].msg_name);   // Copy the name of the file we're recording to a temporary place
            // sprintf( dxinfox[ channum ].msg_name, "sounds/ivr_recmenu.pcm");
            dxinfox[ channum ].msg_fd = open("sounds/ivr_recmenu.pcm", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                return (-1);
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, "sounds/ivr_recmenu.pcm");
                disp_msg("Couldn't play the category recorder menu. Whoops...");
            }

            return (0);

        case ST_ENIGMAREC:
            dxinfox[ channum ].state = ST_ENIGMAREC2;
            sprintf(filetmp[ channum ], "%s", dxinfox[ channum ].msg_name);   // Copy the name of the file we're recording to a temporary place
            close(dxinfox[ channum ].msg_fd);
            // sprintf( dxinfox[ channum ].msg_name, "sounds/longrec_menu.pcm");
            dxinfox[ channum ].msg_fd = open("sounds/longrec_menu.pcm", O_RDONLY);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, "sounds/longrec_menu.pcm");
                disp_msgf("OHHNOES!!!11 Couldn't play the Enigmarec edit message on channel %s", ATDV_NAMEP(chdev));
            }

            return (0);
    }

    if (curstate != ST_ONHOOK) {
        dxinfox[ channum ].state = ST_GOODBYE;

        if (play(channum, goodbyefd, 0, 0, 0) == -1) {
            disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
        }
    }

    return (0);
}


/***************************************************************************
 *        NAME: int getdig_hdlr()
 * DESCRIPTION: TDX_GETDIG event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int getdig_hdlr() {
    int chdev = sr_getevtdev();
    int digcount;
    // int event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on getdig chdev");
    }

    int channum = get_channum(chdev);
    int curstate;
    int errcode[MAXCHANS];


    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    errcode[channum] = 0;

    curstate = dxinfox[ channum ].state;        /* Current State */

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */

    switch (frontend) {

        case CT_NTANALOG:

            if (ATDX_TERMMSK(chdev) & TM_LCOFF) {

                if (dxinfox[ channum ].state == ST_MODEMDETECT)  {
                    if (!fprintf(logdescriptor, "%s;%s;FAX;NODETECT - %s\n", timeoutput(channum), dialerdest[ channum ], dxinfox[ channum ].digbuf.dg_value)) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);

                    dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);

                    if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                    }

                    scancount[initchan[ channum ] ]--;

                    if (scancount[ initchan[ channum ] ] <= 0) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
                    } else {
                        dxinfox[ channum ].state = ST_CALLPTEST5;
                        disp_status(channum, "Hanging up...");
                    }

                    set_hkstate(channum, DX_ONHOOK);

                    return (0);

                }

                if (dxinfox[ channum ].state == ST_VMBDETECT) {
                    if (!fprintf(logdescriptor, "%s;%s;PAMD;NOTONES;%s\n", timeoutput(channum), dialerdest[ channum ], dxinfox[ channum ].digbuf.dg_value)) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);

                    if (dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON) == -1) {
                        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                        disp_msg("Couldn't disable 1000 hertz tone!");
                    }

                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_425, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_850, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1050, DM_TONEON);

                    if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                    }

                    scancount[initchan[ channum ] ]--;

                    if (scancount[initchan[ channum ] ] <= 0) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
                    } else {
                        dxinfox[ channum ].state = ST_CALLPTEST5;
                        disp_status(channum, "Hanging up...");
                    }
                } else {
                    dxinfox[ channum ].state = ST_ONHOOK;
                }

                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            break;

        case CT_NTT1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) == 0) {

                if (termmask[ channum ] & 1) {
                //if ((ownies[channum] == 100) || (ownies[channum] == 200) ) {
                    disp_msg("Removing custom tone with playback handler hangup");
                    dx_distone(chdev, TID_1, DM_TONEON);
                    dx_deltones(chdev);
                    ownies[channum] = 0; // Is this extraneous?
                    termmask[ channum ] ^= 1;
                }

                close(dxinfox[ channum ].msg_fd);
                return (0);
            }

            close(dxinfox[ channum ].msg_fd);   // Close any active file that remains

            break;

        case CT_NTE1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) != 0) {
                return (0);
            }

            break;

        case CT_GCISDN:
            if (isdnstatus[channum] == 1) {
                // Since we're making this for multiple channel types, this should ideally be in it's
                // own function instead of being pasted over and over again. For the moment though,
                // that's just how I roll.

                // If performing Project Upstage, set the digit receivers back to DTMF
                if (ownies[ channum ] == 9) {
                    dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);
                    dx_deltones(dxinfox[ channum ].chdev);
                }

                if (dxinfox[ channum ].state == ST_MODEMDETECT)  {
                    if (!fprintf(logdescriptor, "%s;%s;FAX;NODETECT;%s;HANGUP;\n", timeoutput(channum), dialerdest[ channum ], dxinfox[ channum ].digbuf.dg_value)) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);

                    if (dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON) == -1) {
                        if (!fprintf(logdescriptor, "%s;%s;FAX;DISTONE_ERR - %s\n", timeoutput(channum), dialerdest[ channum ], ATDV_ERRMSGP(dxinfox[channum].chdev))) {
                            disp_msg("Failed to log");
                        }

                        fflush(logdescriptor);
                    }

                    dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);

                    if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                    }

                    if (chaninfo[initchan[ownies[channum]]].dialer.using_list) {
                        dxinfox[ channum ].state = ST_CALLPTEST5;
                        set_hkstate(channum, DX_ONHOOK);
                        return (0);
                    }

                    // scancount[initchan[ channum ] ]--;

                    // This and the below check should be unnecessary now

                    if (scancount[ initchan[ channum ] ] <= 0) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
                        callprog(channum, FALSE);
                        set_hkstate(channum, DX_ONHOOK);
                        return (0);
                    }

                    dxinfox[ channum ].state = ST_CALLPTEST5;
                    // isdn_newdial( channum ); // Does this call isdn_newdial twice? Let's find out.
                    set_hkstate(channum, DX_ONHOOK);
                    // Hanging up shouldn't be necessary since we're already on-hook if we're reaching this section of code.
                    //Andrew uncommended the onhook set

                    return (0);

                }

                if (dxinfox[ channum ].state == ST_VMBDETECT) {
                    // The DM3 is sometimes hitting this instead of returning digits like it's supposed to. Investigate the
                    // event termination criteria for dx_getdigits, and more importantly, make it write the events out like
                    // it's supposed to
                    if (!fprintf(logdescriptor, "%s;%s;PAMD;NOTONES;%s;HANGUP;\n", timeoutput(channum), dialerdest[ channum ], dxinfox[ channum ].digbuf.dg_value)) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);

                    if (dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON) == -1) {
                        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                        disp_msg("Couldn't disable 1000 hertz tone!");
                    }

                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_425, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_850, DM_TONEON);
                    dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1050, DM_TONEON);

                    if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                    }

                    if (chaninfo[initchan[ownies[channum]]].dialer.using_list) {
                        dxinfox[ channum ].state = ST_CALLPTEST5;
                        set_hkstate(channum, DX_ONHOOK);
                        return (0);
                    }

                    // scancount[initchan[ channum ] ]--;

                    // This and the below check should be unnecessary now

                    if ( (scancount[initchan[ channum ] ] <= 0) && (scancount[initchan[ channum ] ] != -2000) ) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
                        callprog(channum, FALSE);
                        return (0);
                    }

                    dxinfox[ channum ].state = ST_CALLPTEST5;
                    // isdn_newdial( channum );
                    // In theory, the call should not still be considered connected if it gets here. Reality seems to be a little different.
                    set_hkstate(channum, DX_ONHOOK);
                    return (0);
                }

                // This return(0) prevents errors from occuring; the break below would make the card potentially restart dx_getdig or another function.
                return (0);
            }

            break;
    }
    switch (dxinfox[ channum ].state) {

        case ST_TC24CALL2:
            switch(dxinfox[ channum ].digbuf.dg_value[0]) {
                case 0x31:
                    dxinfox[ channum ].state = ST_TC24MENU2;
                    // SQL queries go here.
                    char * err_msg;
                    char query[59];
                    char npa[4];
                    snprintf(npa, 4, "%s", filetmp[channum]);
                    npa[3] = 0x00; // Insert null terminator
                    if (altsig & 4) {
                        if (!fprintf(calllog, "%s;Telechallenge Outdial Attempt;CPN %s;Dest %s\n", timeoutput(channum), isdninfo[channum].cpn, filetmp[channum])) {
                            disp_msg("Failed to log");
                        }
                        fflush(calllog);
                    }
                    snprintf(query, 58, "SELECT COUNT(*) FROM area_codes WHERE area_code = '%s';", npa);
                    if (sqlite3_exec( tc_blacklist, query, npa_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
                        disp_msgf("SQL SELECT ERROR: %s", err_msg);
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return -1;
                    }
                    return 0;
                case 0x32:
                    errcnt[channum] = 0;
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                    dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_enternum.pcm", O_RDONLY);
                    dxinfox[ channum ].state = ST_TC24CALL;
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/tc24/tc_enternum.pcm");
                        return -1;
                    }
                    return 0;

                default:
                    dxinfox[ channum ].state = ST_TC24CALL2E;
                    errcnt[channum]++;
                    play(channum, invalidfd, 0, 0, 0);
                    return 0;
            }

        case ST_TC24CALL:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                // Read back for confirmation
                unsigned char length = strlen(dxinfox[channum].digbuf.dg_value);
                unsigned char counter;
                for (counter = 0; counter < length; counter++) {
                    // Is this a digit, or did some wiseass press * or something?
                    if ((dxinfox[channum].digbuf.dg_value[counter] < 0x30) ||
                        (dxinfox[channum].digbuf.dg_value[counter] > 0x40)) {
                        dxinfox[ channum ].state = ST_TC24CALLE;
                        errcnt[channum]++;
                        play(channum, invalidfd, 0, 0, 0);
                        return 0;
                    }
                }
                multiplay[channum][0] = open("sounds/tc24/challenging.pcm", O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[0]);
                multiplay[channum][1] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[1]);
                multiplay[channum][2] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[2]);
                multiplay[channum][3] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[3]);
                multiplay[channum][4] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[4]);
                multiplay[channum][5] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[5]);
                multiplay[channum][6] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[6]);
                multiplay[channum][7] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[7]);
                multiplay[channum][8] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[8]);
                multiplay[channum][9] = open(dxinfox[channum].msg_name, O_RDONLY);
                sprintf(dxinfox[channum].msg_name, "sounds/digits1/%c.pcm", dxinfox[channum].digbuf.dg_value[9]);
                multiplay[channum][10] = open(dxinfox[channum].msg_name, O_RDONLY);
                multiplay[channum][11] = open("sounds/tc24/right.pcm", O_RDONLY);
                dxinfox[ channum ].state = ST_TC24CALL2;
                if (playmulti( channum, 12, 0x80, multiplay[channum]) == -1) {
                    unsigned char counter;
                    for (counter = 0; counter < 12; counter++) {
                        close(multiplay[channum][counter]);
                    }
                    dxinfox[ channum ].state = ST_GOODBYE;
                    disp_msg("Couldn't play confirmation message!");
                    play(channum, errorfd, 0, 0, 0);
                    return -1;
                }
                strncpy(filetmp[channum], dxinfox[ channum ].digbuf.dg_value, MAXMSG);
                //snprintf(filetmp[channum], MAXMSG, "%s1%s", config.dialout_prefix, dxinfox[channum].digbuf.dg_value);
                return 0;

            }
            else {
                // Invalid
                dxinfox[ channum ].state = ST_TC24CALLE;
                errcnt[channum]++;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }

        case ST_TC24BBSREC:
            // Record
            switch(dxinfox[channum].digbuf.dg_value[0]) {

                case 0x31:
                    // Ice Bucket Recordings
                    msgnum[channum] = 0;
                    errcnt[channum] = 0; // Let's borrow the error counter for this. It needs to be reset anyway.

                    while ((errcnt[channum] == 0) && (msgnum[channum] != 255)) {
                        msgnum[channum]++;
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/temp/%d.pcm", msgnum[channum]);
                        errcnt[channum] = stat(dxinfox[ channum ].msg_name, &sts);
                    }
                    errcnt[channum] = 0;

                    if (msgnum[channum] == 255) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 0);
                        return -1;    // Temporary file queue is full. Something isn't working right. Let's stop.
                    }
                    dxinfox[ channum ].state = ST_TC24BBSREC2;
                    dxinfox[ channum ].msg_fd = open("sounds/emtanon/beginmsg.vox", O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording isn't working");
                        file_error( channum, "sounds/emtanon/beginmsg.vox");
                    }

                    return (errcode[channum]);

                case 0x32:
                    // Defcon Voice BBS
                    msgnum[channum] = 0;
                    errcnt[channum] = 0; // Let's borrow the error counter for this. It needs to be reset anyway.

                    while ((errcnt[channum] == 0) && (msgnum[channum] != 255)) {
                        msgnum[channum]++;
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/temp/%d.pcm", msgnum[channum]);
                        errcnt[channum] = stat(dxinfox[ channum ].msg_name, &sts);
                    }
                    errcnt[channum] = 0;

                    if (msgnum[channum] == 255) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 0);
                        return -1;    // Temporary file queue is full. Something isn't working right. Let's stop.
                    }
                    dxinfox[ channum ].state = ST_DCBBSREC;
                    dxinfox[ channum ].msg_fd = open("sounds/emtanon/beginmsg.vox", O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording isn't working");
                        file_error( channum, "sounds/emtanon/beginmsg.vox");
                    }

                    return (errcode[channum]);

                case '*':
                    // Go back to the main menu
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_TC24MENU;
                    dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                        file_error( channum, "sounds/tc24/greeting.pcm" );
                        return -1;
                    }
                    return 0;

                default:
                    // Invalid
                    errcnt[channum]++;
                    dxinfox[ channum ].state = ST_TC24BBSRECE;
                    play(channum, invalidfd, 0, 0, 0);
                    return 0;
            }

        case ST_TC24BBS:
            // Playback
            switch(dxinfox[channum].digbuf.dg_value[0]) {

                case 0x31:
                    // Ice Bucket Recordings
                    dxinfox[ channum ].state = ST_TC24BBS2;
                    ownies[ channum ] = 0; // Initialize variables
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }

                    maxannc[ channum ] = (anncnum[ channum ]);
                    anncnum[ channum ] = 0;
                    minannc[ channum ] = -1;

                    sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back. Recovering...", dxinfox[ channum ].msg_name);
                        dxinfox[ channum ].msg_fd = open("sounds/emtanon/nomessages.vox", O_RDONLY);
                        if (dxinfox[ channum ].msg_fd == -1) {
                            file_error(channum, "sounds/emtanon/nomessages.vox");
                            return -1;
                        }
                        dxinfox[ channum ].state = ST_TC24MENU2;
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0);
                        return (errcode[channum]);
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }

                    return (errcode[channum]);

                case 0x32:
                    // Defcon Voice BBS
                    dxinfox[ channum ].state = ST_DCBBS;
                    ownies[ channum ] = 0; // Initialize variables
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }

                    maxannc[ channum ] = (anncnum[ channum ]);
                    anncnum[ channum ] = 0;
                    minannc[ channum ] = -1;

                    sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back. Recovering...", dxinfox[ channum ].msg_name);
                        dxinfox[ channum ].msg_fd = open("sounds/emtanon/nomessages.vox", O_RDONLY);
                        if (dxinfox[ channum ].msg_fd == -1) {
                            file_error(channum, "sounds/emtanon/nomessages.vox");
                            return -1;
                        }
                        dxinfox[ channum ].state = ST_TC24MENU2;
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0);
                        return (errcode[channum]);
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }

                    return (errcode[channum]);

                case '*':
                    // Go back to the main menu
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_TC24MENU;
                    dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                        file_error( channum, "sounds/tc24/greeting.pcm" );
                        return -1;
                    }
                    return 0;
                default:
                    // Invalid
                    errcnt[channum]++;
                    dxinfox[ channum ].state = ST_TC24BBSE;
                    play(channum, invalidfd, 0, 0, 0);
                    return 0;
            }

        case ST_TC24MENU1:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                switch(dxinfox[channum].digbuf.dg_value[0]) {
                    case 0x32:
                        // Go to Lion tutorial
                        dxinfox[ channum ].state = ST_TCTUTORIAL;
                        ownies[ channum ] = 0; // Initialize variables
                        anncnum[ channum ] = 0;
                        errcnt[ channum ] = 1;

                        while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                            }
                        }

                        maxannc[ channum ] = (anncnum[ channum ]);
                        anncnum[ channum ] = 0;
                        minannc[ channum ] = -1;

                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[ channum ]);
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                        if (dxinfox[ channum ].msg_fd == -1) {
                            disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                            file_error(channum, dxinfox[ channum ].msg_name);
                            return -1;
                        }
                        play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                        return 0;

                    case 0x31:
                        // Repeat recording
                        errcnt[ channum ] = 0;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_howtomenu.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/tc_howtomenu.pcm");
                            return -1;
                        }

                        return 0;

                    case '*':
                        // Return to main menu
                        errcnt[ channum ] = 0;
                        dxinfox[ channum ].state = ST_TC24MENU;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
                        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                            file_error( channum, "sounds/tc24/greeting.pcm" );
                            return -1;
                        }
                        return 0;
                    default:
                        errcnt[channum]++;
                        dxinfox[channum].state = ST_TC24MENU1E;
                        // The invalid recording shouldn't be pre-emptable with DTMF. The reason being if there's a big spill of invalid digits, it's more
                        // likely to be absorbed.
                        play(channum, invalidfd, 0, 0, 0);
                        return 0;
                }
            }
            else {
                errcnt[channum]++;
                dxinfox[channum].state = ST_TC24MENU1E;
                // The invalid recording shouldn't be pre-emptable with DTMF. The reason being if there's a big spill of invalid digits, it's more
                // likely to be absorbed.
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }
            return 0;


        case ST_TC24MENU:
            // JCT boards return TM_MAXDTMF and TM_DIGIT when both conditions are satisfied. DM3s only return the latter.
            // Thankfully since we're collecting two digits, this won't matter and we can treat both boards the same.

            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                // Check for ** - for the fake admin panel that reads off a bunch of useless shit.
                if (strcmp("**", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Admin status panel thing's here.
                    dxinfox[ channum ].state = ST_TC24MENU2; // For the placeholder recording
                    dxinfox[ channum ].msg_fd = open("sounds/tc24/placeholder.pcm", O_RDONLY);
                    // This should go to a placeholder recording for the moment; this routine's been relegated to a second-tier status.
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/tc24/placeholder.pcm");
                        return -1;
                    }
                    return 0;
                }

                // Not **? Fuck all that, man.
                errcnt[channum]++;
                dxinfox[channum].state = ST_TC24MENUE;
                // The invalid recording shouldn't be pre-emptable with DTMF. The reason being if there's a big spill of invalid digits, it's more
                // likely to be absorbed.
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }
            else if (ATDX_TERMMSK(chdev) & TM_DIGIT) {
                switch(dxinfox[channum].digbuf.dg_value[0]) {
                    case 0x33:
                        // Learn about/how to play the Telechallenge
                        // This should go to another menu with a few options - one to access the tutorial.
                        errcnt[channum] = 0;
                        dxinfox[ channum ].state = ST_TC24MENU1;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_howtomenu.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/tc_howtomenu.pcm");
                            return -1;
                        }

                        return 0;

                    case 0x31:
                        // How to play the hacker cooling contraption challenge
                        // This goes to a quick explanation and back to the main menu.
                        errcnt[channum] = 0;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_howtohccc.pcm", O_RDONLY);
                        dxinfox[ channum ].state = ST_TC24MENU2;
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/tc_howtohccc.pcm");
                            return -1;
                        }

                        return 0;

                    case 0x32:
                        // Nominate a hacker you want to be chilled
                        // This goes to a callout menu
                        errcnt[channum] = 0;
                        time_t rawtime;
                        struct tm *info;
                        time(&rawtime);
                        info = localtime(&rawtime);
                        strftime(filetmp3[channum], 3, "%H", info);
                        unsigned char timeout = atoi(filetmp3[channum]);
                        disp_msgf("DEBUG: Hour returned was %d", timeout);
                        #ifdef EASTERN
                            if ((timeout > 20) || (timeout < 13)) {
                        #else
                            if ((timeout > 17) || (timeout < 10)) {
                        #endif
                            dxinfox[ channum ].state = ST_TC24MENU2;
                            dxinfox[ channum ].msg_fd = open("sounds/tc24/outside_hours.pcm", O_RDONLY);
                            if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                                file_error(channum, "sounds/tc24/outside_hours.pcm");
                                return -1;
                            }
                            time1[channum] = 0;
                            return 0;
                        }
                        time1[channum] = 0;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/tc_enternum.pcm", O_RDONLY);
                        dxinfox[ channum ].state = ST_TC24CALL;
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/tc_enternum.pcm");
                            return -1;
                        }
                        return 0;

                    case 0x34:
                        // Voice BBS Playback
                        errcnt[channum] = 0;
                        dxinfox[ channum ].state = ST_TC24BBS;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/bbs_select.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/bbs_select.pcm");
                            return -1;
                        }

                        return 0;

                    case 0x35:
                        // Voice BBS Record
                        errcnt[channum] = 0;
                        dxinfox[ channum ].state = ST_TC24BBSREC;
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/bbs_select.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/bbs_select.pcm");
                            return -1;
                        }

                        return 0;


                    case 0x36:
                        // I FUCKING HATE VEGAS
                        // Single recording, return to main menu
                        errcnt[channum] = 0;
                        voicemail_xfer(channum, "1001");
                        /*
                        dxinfox[ channum ].msg_fd = open("sounds/tc24/fuckvegas.pcm", O_RDONLY);
                        dxinfox[ channum ].state = ST_TC24MENU2;
                        // We can combine the state with menu option 2's, since it's doing the same thing.
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/tc24/fuckvegas.pcm");
                            return -1;
                        }
                        */
                        return 0;

                    case 0x37:
                        // Argentine Lottery
                        // Loop piecing together Argentine lottery stuff
                        errcnt[channum] = 0;
                        dxinfox[ channum ].state = ST_TC24ARG;
                        srandom(time(NULL));
                        disp_status(channum, "Playing Riverito");
                        anncnum[ channum ] = 0;
                        errcnt[ channum ] = 1;
                        while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", anncnum[ channum ]);
            
                            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                                errcnt[ channum ] = 0;
                            } else {
                                anncnum[ channum ]++;
                            }
                        }
                        maxannc[ channum ] = anncnum[ channum ];
                        anncnum[channum] = 0;
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][0] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][1] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][2] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][3] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][4] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][5] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][6] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][7] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][8] = open(dxinfox[channum].msg_name, O_RDONLY);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/riverito/%d.pcm", random_at_most(maxannc[channum]));
                        multiplay[channum][9] = open(dxinfox[channum].msg_name, O_RDONLY);
                        if (playmulti( channum, 10, 0x00, multiplay[channum]) == -1) {
                            unsigned char counter;
                            for (counter = 0; counter < 10; counter++) {
                                close(multiplay[channum][counter]);
                            }
                            dxinfox[ channum ].state = ST_GOODBYE;
                            disp_msg("Couldn't play confirmation message!");
                            play(channum, errorfd, 0, 0, 0);
                            return -1;
                        }
                        return 0;

                    case 0x38:
                        errcnt[channum] = 0;
                        voicemail_xfer(channum, "1000");
                        return 0;


                    default:
                        // There should really be a function for handling invalid digits. We do this so, *so* often...

                        // Invalid selection. Since the termination mask is present, we should never actually get to this, but let's handle the error anyway.
                        errcnt[channum]++;
                        dxinfox[channum].state = ST_TC24MENUE;
                        // The invalid recording shouldn't be pre-emptable with DTMF. The reason being if there's a big spill of invalid digits, it's more
                        // likely to be absorbed.
                        play(channum, invalidfd, 0, 0, 0);
                        return 0;
                }
            }


            else {
                // Whatever you pressed, it's wrong.
                errcnt[channum]++;
                dxinfox[channum].state = ST_TC24MENUE;
                // The invalid recording shouldn't be pre-emptable with DTMF. The reason being if there's a big spill of invalid digits, it's more
                // likely to be absorbed.
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }

        case ST_ADMINADD3:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                // We have the extension, activation status, and password. Add it to the SQL database, say thanks.pcm, and return to the main menu
                // "INSERT INTO USERS VALUES('%s', '%s', %d);"
                char * err_msg;
                char query[52];
                snprintf(query, 51, "INSERT INTO USERS VALUES('%s', '%s', %d);", filetmp[channum], dxinfox[channum].digbuf.dg_value, (int) ownies[channum]);
                if (sqlite3_exec( activationdb, query, NULL, NULL, &err_msg) != SQLITE_OK) {
                    disp_msgf("SQL INSERT ERROR: %s", err_msg);
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }
                dxinfox[channum].state = ST_ADMINADDF;
                dxinfox[ channum ].msg_fd = open("sounds/thanks.pcm", O_RDONLY);
                if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                    file_error(channum, "sounds/thanks.pcm");
                    return -1;
                }
                return 0;
            }
            else {
                errcnt[channum]++;
                dxinfox[channum].state = ST_ADMINADD3E;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }

        case ST_ADMINADD2:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                switch(dxinfox[channum].digbuf.dg_value[0]) {
                    case 0x31:
                    // Not activated
                    ownies[channum] = 0;
                    dxinfox[channum].state = ST_ADMINADD3;
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterpass.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/activation/activationivr_admin_enterpass.pcm");
                        return -1;
                    }
                    return 0;
                    case 0x32:
                    // Activated
                    ownies[channum] = 1;
                    dxinfox[channum].state = ST_ADMINADD3;
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterpass.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/activation/activationivr_admin_enterpass.pcm");
                        return -1;
                    }
                    return 0;

                    default:
                    errcnt[channum]++;
                    dxinfox[channum].state = ST_ADMINADD2E;
                    play(channum, invalidfd, 0, 0, 0);
                    return 0;
                }
                return 0;
            }

            else {
                errcnt[channum]++;
                dxinfox[channum].state = ST_ADMINADD2E;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }

        case ST_ADMINADD:
            if (!(ATDX_TERMMSK(chdev) & TM_MAXDTMF)) {
                errcnt[channum]++;
                dxinfox[channum].state = ST_ADMINADDE;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }
            else {
                char query[52];
                char * err_msg;
                errcnt[ channum ] = 0;
                snprintf( query, 51, "SELECT COUNT(*) FROM USERS WHERE EXTENSION = %s;", dxinfox[channum].digbuf.dg_value );
                if (sqlite3_exec( activationdb, query, adminadd_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
                    disp_msgf("SQL ERROR: %s", err_msg);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }
                else return 0;
            }


        case ST_ADMINACT3:
            // Update user passcode
            if (!(ATDX_TERMMSK(chdev) & TM_MAXDTMF)) {
                errcnt[channum]++;
                dxinfox[channum].state = ST_ADMINACT3E;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }
            else {
                char query[59];
                char * err_msg;
                errcnt[ channum ] = 0;
                snprintf( query, 58, "UPDATE USERS SET PASSWORD = %s WHERE EXTENSION = %s;", dxinfox[channum].digbuf.dg_value, filetmp[channum]);
                if (sqlite3_exec( activationdb, query, NULL, NULL, &err_msg) != SQLITE_OK) {
                    disp_msgf("SQL UPDATE ERROR: %s", err_msg);
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }
                dx_clrdigbuf(dxinfox[channum].chdev);
                multiplay[channum][0] = open("sounds/thanks.pcm", O_RDONLY);
                multiplay[channum][1] = open("sounds/activation/activationivr_adminmenu.pcm", O_RDONLY);
                dxinfox[ channum ].state = ST_ADMINACT2;
                if (playmulti(channum, 2, 128, multiplay[channum]) == -1) {
                    disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                    disp_msg("ERROR: Couldn't play back admin menu recording!");
                    dxinfox[ channum ].state = ST_ERROR;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }
                return 0;
            }
        case ST_ADMINACT2:
            if (!(ATDX_TERMMSK(chdev) & TM_MAXDTMF)) {
                errcnt[channum]++;
                dxinfox[channum].state = ST_ADMINACT2E;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }
            else {
                char query[58];
                char * err_msg;
                switch(dxinfox[channum].digbuf.dg_value[0]) {
                    case 0x31:
                        // Passcode reset
                        errcnt[ channum ] = 0;
                        dxinfox[ channum ].state = ST_ADMINACT3;
                        dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_newpass.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/activation/activationivr_admin_newpass.pcm");
                            return -1;
                        }
                        return 0;
                    case 0x32:
                        // Set extension as not activated
                        errcnt[ channum ] = 0;
                        snprintf( query, 57, "UPDATE USERS SET REGISTERED = 0 WHERE EXTENSION = %s;", filetmp[channum]);
                        // TO DO: Make the callback something else if necessary
                        if (sqlite3_exec( activationdb, query, NULL, NULL, &err_msg) != SQLITE_OK) {
                            disp_msgf("SQL UPDATE ERROR: %s", err_msg);
                            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                            dxinfox[channum].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        multiplay[channum][0] = open("sounds/activation/activationivr_admin_extnotactivated.pcm", O_RDONLY);
                        multiplay[channum][1] = open("sounds/activation/activationivr_adminmenu.pcm", O_RDONLY);
                        dxinfox[ channum ].state = ST_ADMINACT2;
                        if (playmulti(channum, 2, 128, multiplay[channum]) == -1) {
                            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                            disp_msg("ERROR: Couldn't play back admin menu recording!");
                            dxinfox[ channum ].state = ST_ERROR;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        return 0;

                    case 0x33:
                        // Set extension as activated
                        errcnt[ channum ] = 0;
                        snprintf( query, 57, "UPDATE USERS SET REGISTERED = 1 WHERE EXTENSION = %s;", filetmp[channum]);
                        if (sqlite3_exec( activationdb, query, NULL, NULL, &err_msg) != SQLITE_OK) {
                            disp_msgf("SQL UPDATE ERROR: %s", err_msg);
                            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                            dxinfox[channum].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        multiplay[channum][0] = open("sounds/activation/activationivr_admin_extactivated.pcm", O_RDONLY);
                        multiplay[channum][1] = open("sounds/activation/activationivr_adminmenu.pcm", O_RDONLY);
                        dxinfox[ channum ].state = ST_ADMINACT2;
                        if (playmulti(channum, 2, 128, multiplay[channum]) == -1) {
                            disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                            disp_msg("ERROR: Couldn't play back admin menu recording!");
                            dxinfox[ channum ].state = ST_ERROR;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        return 0;

                    case 0x34:
                        // Baleet extension
                        errcnt[ channum ] = 0;
                        snprintf( query, 43, "DELETE FROM USERS WHERE EXTENSION = %s;", filetmp[channum]);
                        // No callback for this guy
                        if (sqlite3_exec( activationdb, query, NULL, NULL, &err_msg) != SQLITE_OK) {
                            disp_msgf("SQL UPDATE ERROR: %s", err_msg);
                            memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                            dxinfox[channum].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        dxinfox[ channum ].state = ST_ADMINACTE;
                        dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_deleted.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/activation/activationivr_admin_deleted.pcm");
                            return -1;
                        }
                        return 0;

                    case 0x2A: // Asterisk
                        // Go back to main menu
                        memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                        dxinfox[channum].state = ST_ADMINACT;
                        errcnt[channum] = 0;
                        dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
                        if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                            file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                            return -1;
                        }
                        return 0;

                    default:
                        errcnt[channum]++;
                        dxinfox[channum].state = ST_ADMINACT2E;
                        play(channum, invalidfd, 0, 0, 0);
                        return 0;
                }
            }

        case ST_ADMINACT:
            if (!(ATDX_TERMMSK(chdev) & TM_MAXDTMF)) {
                errcnt[channum]++;
                dxinfox[channum].state = ST_ADMINACTE;
                play(channum, invalidfd, 0, 0, 0);
                return 0;
            }
            else {
                char query[52];
                char * err_msg;
                errcnt[ channum ] = 0;
                snprintf( query, 51, "SELECT COUNT(*) FROM USERS WHERE EXTENSION = %s;", dxinfox[channum].digbuf.dg_value );
                if (sqlite3_exec( activationdb, query, admincount_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
                    disp_msgf("SQL ERROR: %s", err_msg);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }
                else return 0;
            }

        case ST_ACTIVATION3:
            if (!(ATDX_TERMMSK(chdev) & TM_MAXDTMF)) {
            //if (strlen(dxinfox[channum].digbuf.dg_value) == 4) {
                errcnt[channum] = 0;
                dxinfox[channum].state = ST_ACTIVATIONF;
                dxinfox[ channum ].msg_fd = open("sounds/activation/activation_invalid_extpass.pcm", O_RDONLY);
                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                    file_error(channum, "sounds/activation/activation_invalid_extpass.pcm");
                    return -1;
                }
                return 0;
            }
            else {
                // Do SQL query here
                char query[52];
                char * err_msg;
                //snprintf( query, 44, "SELECT * FROM USERS WHERE EXTENSION = %s;", filetmp[channum]);
                snprintf( query, 51, "SELECT COUNT(*) FROM USERS WHERE EXTENSION = %s;", filetmp[channum] );
                if (sqlite3_exec( activationdb, query, count_cb, (void*) &channum, &err_msg) != SQLITE_OK) {
                    disp_msgf("SQL ERROR: %s", err_msg);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return -1;
                }
                else return 0;
            }

        case ST_ACTIVATION2:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                // Store string here for SQL query. With MAXDTMF termination flag, we'll implicitly already have a four character string
                /*
                char query[44];
                char * err_msg;
                snprintf( query, 43, "SELECT * FROM USERS WHERE extension = %s", dxinfox[channum].digbuf.dg_value)
                if (sqlite3_exec( activationdb, query, userpass_cb, channum, *err_msg) != SQLITE_OK) { disp_msgf("SQL ERROR: %s", err_msg);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);

                }
                */

                // TO DO: Put this below in the callback handler for the SQL query
                errcnt[channum] = 0;
                strcpy(filetmp[channum], dxinfox[channum].digbuf.dg_value);
                dxinfox[channum].state = ST_ACTIVATION3;
                dxinfox[ channum ].msg_fd = open("sounds/activation/activation_enterpass.pcm", O_RDONLY);
                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                        file_error(channum, "sounds/activation/activation_enterpass.pcm");
                        return -1;
                }
                return 0;
            }

            else {
                errcnt[channum]++;

                if (errcnt[ channum ] <= 3) {
                        dxinfox[ channum ].state = ST_ACTIVATION2E;
                        play(channum, invalidfd, 0, 0, 0);
                        return (0);
                }

                else {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }
                return 0;
            }


        case ST_ACTIVATION:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                switch(dxinfox[channum].digbuf.dg_value[0]) {
                    case 0x30:
                        // Call the operator
                        dxinfox[ channum ].state = ST_ACTIVATIONOP;
                        dxinfox[ channum ].msg_fd = open("sounds/thx4shadytel.pcm", O_RDONLY);
                        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1)  == -1) {
                            file_error(channum, "sounds/thx4shadytel.pcm");
                            return -1;
                        }
                        return 0;


                    case 0x31:
                        errcnt[channum] = 0;
                        if (strlen(isdninfo[channum].cpn) == 0) {
                            disp_msg("ERROR: No number received! Cannot run activation IVR.");
                            dxinfox[channum].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        // Do the registration thing
                        if (strcmp(isdninfo[channum].cpn, "631") ==  0) {
                            dxinfox[channum].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return -1;
                        }
                        dxinfox[ channum ].state = ST_ACTIVATION2;
                        dxinfox[ channum ].msg_fd = open("sounds/activation/activation_enterext.pcm", O_RDONLY);
                        if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                            file_error(channum, "sounds/activation/activation_enterext.pcm");
                            return -1;
                        }
                        return 0;

                    case '*':
                        // ANAC
                        errcnt[channum] = 0;
                        if (strlen(isdninfo[channum].cpn) > 0) {
                            // Do the multiplay routine
                            closecount[channum] = digread(channum, isdninfo[channum].cpn);
                            dxinfox[channum].state = ST_ACTIVATIONE;
                            return 0;
                        }

                        else {
                            dxinfox[channum].state = ST_ACTIVATIONF; // This permits us to close the FD for the error before returning to the main menu
                            // Don't execute the ANAC routine; just play "no calling number received".
                            dxinfox[ channum ].msg_fd = open("sounds/activation/activation_nonumrcvd.pcm", O_RDONLY);
                            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                                file_error(channum, "sounds/activation/activation_intro.pcm");
                                return -1;
                            }
                        }
                        return 0;

                    default:
                        // Invalid entry
                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_ACTIVATIONE;
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }
                        return 0;
                }
            }

            else {
                // Don't evaluate the digits if the termination mask from the hardware indicates nothing was pressed.
                errcnt[channum]++;

                if (errcnt[ channum ] <= 3) {
                    dxinfox[ channum ].state = ST_ACTIVATIONE;
                    play(channum, invalidfd, 0, 0, 0);
                    return (0);
                }

                else {
                    disp_statusf(channum, "Activation IVR | Permanent Signal: Ext. %s", isdninfo[channum].cpn);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/activation/activationivr_permanentsig.pcm");
                    if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        disp_msg("Failure playing permanent signal!");
                        play(channum, goodbyefd, 0, 0, 0);
                    }
                    else {
                        dxinfox[ channum ].state = ST_ACTIVATIONPS;
                        play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1);
                    }
                    return (0);
                }

                return 0;
            }
        case ST_ROUTEDTEST:
        routed_cleanup(connchan[channum]);
        tl_reroute(channum);
        return(0);

        case ST_DISALOGIN:
            if (ATDX_TERMMSK(chdev) & TM_DIGIT) {
                dxinfox[channum].digbuf.dg_value[strlen(dxinfox[channum].digbuf.dg_value) - 1] = 0x00;    // Remove the # sign
            }

            if (strcmp(config.password, dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Do ISDN test line functionality
                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                dxinfox[channum].state = ST_ISDNTEST;
                disp_status(channum, "Running ISDN test line");
                sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_testgreet.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msg("Failure playing ISDN testline greeting");

                    if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                        disp_msg("Holy shit! isdn_drop() failed! D:");
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }

                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp(config.adminivr, dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Admin IVR - Edit
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_ADMINACT;
                    disp_statusf(channum, "Running Activation Admin IVR: Ext. %s", isdninfo[channum].cpn);
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                        return -1;
                    }
                    return 0;
            }
            

            if (strcmp(config.adminaddivr, dxinfox[ channum ].digbuf.dg_value) == 0) {
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_ADMINADD;
                    disp_statusf(channum, "Running Activation Admin Add IVR: Ext. %s", isdninfo[channum].cpn);
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                        return -1;
                    }
                    return 0;
            }

            if (strcmp("3115552368", dxinfox[ channum ].digbuf.dg_value) == 0) {
                
            // Do 2600 tone adding first. If it dun goofs, let's GTFO.

            disp_status(channum, "Running Project Upstage");
            // Try these new values for JCT compatibility.
            
            if (dm3board == FALSE) {

                if (dx_bldstcad(TID_2600, 2600, 65, 35, 0, 10, 0, 0) == -1) {
                    disp_msg("Couldn't add 2600 tone! Exiting...");
                    disp_status(channum, "Error adding 2600 tone!");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

            }

            else if (dm3board == TRUE) {

                dx_deltones(dxinfox[channum].chdev); // This is sort of a crude bugfix, and it shouldn't be in here long term. 
                if (dx_bldstcad(TID_2600, 2600, 60, 60, 60, 0, 0, 0) == -1) {
                    disp_msg("Couldn't add 2600 tone! Exiting...");
                    disp_status(channum, "Error adding 2600 tone!");
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

            }

            if (dx_addtone(dxinfox[ channum ].chdev, '\0', 0) == -1) {
                disp_msgf("Unable to add 2600 hertz tone to channel %d. Error %s", ownies[channum], ATDV_ERRMSGP(dxinfox[ channum ].chdev));
                disp_status(channum, "Error adding 2600 hertz tone!");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            if (dx_setdigtyp(dxinfox[ channum ].chdev, D_MF) == -1) {
                // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
                // This really, *really* shouldn't fail...
                disp_msg("Unable to set digit type to MF!");
                disp_status(channum, "Unable to set digit type to MF!");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            // We shouldn't need to treat the D channels specially since they never get set to state ST_WTRING
            connchan[ channum ] = idle_trunkhunt( channum, 1, maxchans, true);
            if (connchan[ channum ] == -1) return -1;
            dxinfox[channum].state = ST_2600ROUTE;
            disp_msgf("Dest. channel is %d", connchan[channum]);

            // Is there any way to make the while loop terminate before hitting maxchans without testing every time? This is going to waste CPU cycles...
            // Trunkhunt replacement
            /*
            while ((dxinfox[connchan[channum]].state != ST_WTRING) && (connchan[channum] <= maxchans)) {
                connchan[channum]++;
            }

            if (connchan[ channum ] > maxchans) {  // Error handling for all circuits being busy; make sure we're not trying to dial out on the D channel
                ownies[ channum ] = 0;
                dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset receivers back to DTMF
                // Error handling for all circuits being busy
                connchan[channum] = 0;
                dxinfox[ channum ].state = ST_ISDNACB;
                dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0)  == -1) {
                    set_hkstate(channum, DX_ONHOOK);
                }

                disp_msg("Error: all circuits are busy");
                return (-1);
                }
                */

            connchan[connchan[channum]] = channum;
            dxinfox[connchan[channum]].state = ST_2600ROUTE2;

            if (isdninfo[channum].cpn[0] == '\0') {
                sprintf(isdninfo[channum].cpn, "12525350002");
            }
            
            dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600

            //makecall(connchan[channum], "7818004880254", isdninfo[channum].cpn, FALSE);   // Call the number in the digit buffer
            makecall(connchan[channum], "754163630000", isdninfo[channum].cpn, FALSE);   // Call the number in the digit buffer

            return (0);
                
            }

            if (strcmp(config.extensions.confbridge, dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Do conf functionality

                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                if (!conf) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    playtone_rep(channum, 350, 440, -24, -26, 25, 25);
                    disp_status(channum, "Conference feature not available");
                    return (0);
                }

                return conf_init(channum, 0, 0);

            }

            else {
                dxinfox[ channum ].state = ST_GOODBYE;
                playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                disp_status(channum, "Invalid DISA password entered!");
                if (altsig & 4) {
                    if (!fprintf(calllog, "%s;Idiot dialed invalid password;CPN %s;CPName %s\n", timeoutput(channum), isdninfo[channum].cpn, isdninfo[channum].displayie)) {
                        disp_msg("Failed to log");
                    }

                    fflush(calllog);
                }
                return (0);
            }

            return (0);


        case ST_2600STOP:
            dxinfox[ channum ].state = ST_2600_1;
            playtone_cad(channum, 2600, 0, 4);
            return (0);
            
        case ST_2600_3:
        
        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
        }

        if (dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF) == -1) {
            // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
            // This really, *really* shouldn't fail...
            disp_msg("Unable to set digit type to MF!");
            disp_status(channum, "Unable to set digit type to MF!");
            dxinfox[ channum ].state = ST_GOODBYE;
            play(channum, errorfd, 0, 0, 1);
            return (-1);
        }

            return (0);

        case ST_2600_2:
            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            if (dx_setdigtyp(dxinfox[ channum ].chdev, D_MF) == -1) {
                // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
                // This really, *really* shouldn't fail...
                disp_msg("Unable to set digit type to MF!");
                disp_status(channum, "Unable to set digit type to MF!");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            return (0);

        /*
        case ST_2600ROUTE:

        //filecount[channum] = (strlen( dxinfox[ channum ].digbuf.dg_value ) - 1 );
        disp_msgf("Digit detected. Looking for 2600, digit buffer contains %s", dxinfox[channum].digbuf.dg_value);


        if (dxinfox[channum].digbuf.dg_value[0] == 'V' ) {
         disp_msg("Preparing to reset pseudo-trunk");
         // Drop the currently active call and reset the detector to include MF

         if ( dx_clrdigbuf( dxinfox[channum].chdev ) == -1 ) {
              disp_msgf( "Cannot clear DTMF Buffer for %s", ATDV_NAMEP( dxinfox[channum].chdev ) );
             }

         if ( dx_setdigtyp( dxinfox[ channum ].chdev, D_MF ) == -1 ) {
              // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
              // This really, *really* shouldn't fail...
              disp_msg("Unable to set digit type to MF!");
              disp_status( channum, "Unable to set digit type to MF!");
              dxinfox[ channum ].state = ST_GOODBYE;
              play( channum, errorfd, 0, 0 );
              return(-1);
         }

         if ( nr_scunroute( dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP ) == -1 ) {
              disp_msg( "Uhh, I hate to break it to you man, but SCRoute is shitting itself" );
              disp_err( channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state );
              isdn_drop( channum, 41);
              return(-1);
             }

         if ( nr_scroute( dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP ) == -1 ) {
              disp_msg( "Holy shit! SCRoute1 threw an error!" );
              disp_err( channum, dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state );
              isdn_drop( connchan[channum], 41);
              return(-1);
             }

         if ( nr_scroute( dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP ) == -1 ) {
              disp_msg( "Holy shit! SCRoute2 threw an error!" );
              disp_err( channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state );
              isdn_drop( channum, 41);
              return(-1);
             }

         dxinfox[ channum ].state = ST_2600_1;
         isdn_drop( connchan[channum], 16);
         playtone_cad( channum, 2600, 0, 5 );

        }

        return(0);
        */

        case ST_2600_1:

            if (ATDX_TERMMSK(chdev) & TM_USRSTOP) {
                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                return (0);
            }

            filecount[channum] = strlen(dxinfox[ channum ].digbuf.dg_value);

            /*
            if ( dxinfox[ channum ].digbuf.dg_value[filecount[channum]] == 'V' ) {
                if ( dx_clrdigbuf( dxinfox[channum].chdev ) == -1 ) {
                     disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP( dxinfox[channum].chdev ) );
                    }
                playtone_cad( channum, 2600, 0, 4 );
                return(0);
            }
            */

            if (filecount[ channum ] < 11) {
                // Less than eleven digits received. Let's give a general "what the fuck is this" error.
                disp_msgf("Invalid number received. Digits were %s", dxinfox[channum].digbuf.dg_value);
                // sprintf( dxinfox[ channum ].msg_name, "sounds/facilitytrouble.pcm" );
                dxinfox[ channum ].msg_fd = open("sounds/facilitytrouble.pcm", O_RDONLY);

                if (dxinfox[ channum ].msg_fd == -1) {
                    // Uhh, this is bad. Let's hang up.
                    dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset digit receiver to only hear DTMF
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                } else {
                    play(channum, dxinfox[ channum ].msg_fd, 0x101, 0, 0);
                }

                dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                return (0);
            }

            // KP2 and ST2 are the same frequency for some reason. When coming from the Dialogic card, it'll show up as 'b'.
            // Also, ST1 is 'a', ST3 is 'c'.

            switch (dxinfox[ channum ].digbuf.dg_value[0]) {
                /*
                case 'b':
                    disp_status(channum, "Attempting C*Net route...");

                    // This is pretty much all we can do other than attempt to route the call; the C-Net dialplan is variable length and weird.
                    if ((dxinfox[ channum ].digbuf.dg_value[filecount[channum]] != '#') &&
                            (dxinfox[ channum ].digbuf.dg_value[filecount[channum]] != 'a')) {

                        disp_msgf("Invalid number received. Digits were %s", dxinfox[channum].digbuf.dg_value);
                        dxinfox[ channum ].msg_fd = open("sounds/cbcad.pcm", O_RDONLY);

                        if (dxinfox[ channum ].msg_fd == -1) {
                            // Uhh, this is bad. Let's hang up.
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset digit receiver to only hear DTMF.
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0);
                            return (-1);
                        } else {
                            play(channum, dxinfox[ channum ].msg_fd, 0x101, 0);
                        }

                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                        return (0);
                    }

                    else {
                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                        dxinfox[ channum ].digbuf.dg_value[ filecount[ channum ]] = '\0';
                        connchan[ channum ] = 1;
                        dxinfox[channum].state = ST_2600ROUTE;

                        // Is there any way to make the while loop terminate before hitting maxchans without testing every time? This is going to waste CPU cycles...
                        while ((dxinfox[connchan[channum]].state != ST_WTRING) && (connchan[channum] <= maxchans)) {
                            connchan[channum]++;
                        }

                        disp_msgf("Dest. channel is %d", connchan[channum]);

                        if (connchan[ channum ] > maxchans) {  // Error handling for all circuits being busy; make sure we're not trying to dial out on the D channel
                            ownies[ channum ] = 0;
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset receivers back to DTMF
                            // Error handling for all circuits being busy
                            connchan[channum] = 0;
                            dxinfox[ channum ].state = ST_ISDNACB;
                            // sprintf( dxinfox[ channum ].msg_name, "sounds/acb_error.pcm" );
                            dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);

                            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0)  == -1) {
                                set_hkstate(channum, DX_ONHOOK);
                            }

                            disp_msg("Error: all circuits are busy");
                            return (-1);
                        }

                        connchan[connchan[channum]] = channum;
                        dxinfox[connchan[channum]].state = ST_2600ROUTE2;

                        if (isdninfo[channum].cpn[0] == '\0') {
                            sprintf(isdninfo[channum].cpn, "17097104");    // Replace this with a memcpy; no formatting will be needed.
                        }

                        sprintf(filetmp[channum], "62%s", (dxinfox[ channum ].digbuf.dg_value + 1));
                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600

                        if (altsig & 4) {
                            if (!fprintf(calllog, "%s;MF Destination %s;CPN %s;\n", timeoutput(channum), filetmp[channum], isdninfo[channum].cpn)) {
                                disp_msg("Failed to log");
                            }

                            fflush(calllog);
                        }

                        makecall(connchan[channum], filetmp[channum], isdninfo[channum].cpn, FALSE);   // Call the number in the digit buffer

                        // 'kay, we're done with the digit buffer. Let's blow it away.
                        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                        }

                        // get_digs( channum, &dxinfox[ channum ].digbuf, 1, 0, 0 ); // For 2600 detection
                        return (0);

                    }
                    */

                case '*':
                
                    if ((filecount[ channum ] == 16) && 
                        (dxinfox[ channum ].digbuf.dg_value[1] == '0') &&
                        (dxinfox[ channum ].digbuf.dg_value[2] == '1') &&
                        (dxinfox[ channum ].digbuf.dg_value[3] == '1') &&
                        (dxinfox[ channum ].digbuf.dg_value[4] == '8') &&
                        (dxinfox[ channum ].digbuf.dg_value[5] == '0') &&
                        (dxinfox[ channum ].digbuf.dg_value[6] == '0')) {
                            disp_status(channum, "Attempting UIFN ISDN route...");
                            dxinfox[ channum ].digbuf.dg_value[15] = '\0';
                        // We shouldn't need to treat the D channels specially since they never get set to state ST_WTRING
                        connchan[ channum ] = idle_trunkhunt( channum, 1, maxchans, true );
                        disp_msgf("Dest. channel is %d", connchan[channum]);
                        if (connchan[ channum ] == -1) return -1;
                        dxinfox[channum].state = ST_2600ROUTE;

                        // Is there any way to make the while loop terminate before hitting maxchans without testing every time? This is going to waste CPU cycles...
                        // Trunkhunt replacement
                        /*
                        while ((dxinfox[connchan[channum]].state != ST_WTRING) && (connchan[channum] <= maxchans)) {
                            connchan[channum]++;
                        }



                        if (connchan[ channum ] > maxchans) {  // Error handling for all circuits being busy; make sure we're not trying to dial out on the D channel
                            ownies[ channum ] = 0;
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset receivers back to DTMF
                            // Error handling for all circuits being busy
                            connchan[channum] = 0;
                            dxinfox[ channum ].state = ST_ISDNACB;
                            dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);

                            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0)  == -1) {
                                set_hkstate(channum, DX_ONHOOK);
                            }

                            disp_msg("Error: all circuits are busy");
                            return (-1);
                        }
                        */

                        connchan[connchan[channum]] = channum;
                        dxinfox[connchan[channum]].state = ST_2600ROUTE2;

                        if (isdninfo[channum].cpn[0] == '\0') {
                            sprintf(isdninfo[channum].cpn, "12525350002");
                        }

                        sprintf(filetmp[channum], "79%s", (dxinfox[channum].digbuf.dg_value + 1));    // Get rid of the KP, 1. Also, this should be memset.
                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600

                        if (altsig & 4) {
                            if (!fprintf(calllog, "%s;MF Destination %s;CPN %s;\n", timeoutput(channum), (dxinfox[channum].digbuf.dg_value + 1), isdninfo[channum].cpn)) {
                                disp_msg("Failed to log");
                            }

                            fflush(calllog);
                        }

                        makecall(connchan[channum], filetmp[channum], isdninfo[channum].cpn, FALSE);   // Call the number in the digit buffer

                        // 'kay, we're done with the digit buffer. Let's blow it away.
                        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                        }

                        return (0);
                    }

                    if ((filecount[ channum ] != 13) ||
                            (dxinfox[ channum ].digbuf.dg_value[1] != '1')  ||
                            // All numbers should use the routing prefix 1
                            (dxinfox[ channum ].digbuf.dg_value[2] != '8')  ||
                            (dxinfox[ channum ].digbuf.dg_value[3] != dxinfox[ channum ].digbuf.dg_value[4])  ||
                            (dxinfox[ channum ].digbuf.dg_value[3] == '1')  ||
                            (dxinfox[ channum ].digbuf.dg_value[3] == '9')  ||
                            ((dxinfox[ channum ].digbuf.dg_value[12] != '#') &&
                             (dxinfox [ channum ].digbuf.dg_value[12] != 'a'))) {
                        disp_status(channum, "MF string received w/invalid digits");
                        // # corresponds to ST. If there's no ST or ST1, it's not a valid MF sequence.
                        // To do: change the protocol so either ST or ST1 requests ANI.
                        // KP + 1 + 7D + ST is ten digits. If that's not received, give a general error
                        disp_msgf("Invalid number received. Digits were %s", dxinfox[channum].digbuf.dg_value);
                        dxinfox[ channum ].msg_fd = open("sounds/cbcad.pcm", O_RDONLY);

                        if (dxinfox[ channum ].msg_fd == -1) {
                            // Uhh, this is bad. Let's hang up.
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset digit receiver to only hear DTMF.
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return (-1);
                        } else {
                            play(channum, dxinfox[ channum ].msg_fd, 0x101, 0, 0);
                        }

                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                        return (0);

                    }

                    else {
                        disp_status(channum, "Attempting toll-free ISDN route...");
                        // Formatted number received. LET'S ROUTE!
                        // Erase the trailing ST or ST1; this is not part of an ISDN destination.
                        /*
                        if ( dxinfox[ channum ].digbuf.dg_value[10] == 'a' ) {
                            // Request ANI
                        }
                        */
                        dxinfox[ channum ].digbuf.dg_value[12] = '\0';
                        // We shouldn't need to treat the D channels specially since they never get set to state ST_WTRING
                        connchan[ channum ] = idle_trunkhunt( channum, 1, maxchans, true);
                        dxinfox[channum].state = ST_2600ROUTE;

                        disp_msgf("Dest. channel is %d", connchan[channum]);

                        // Is there any way to make the while loop terminate before hitting maxchans without testing every time? This is going to waste CPU cycles...
                        /*
                        while ((dxinfox[connchan[channum]].state != ST_WTRING) && (connchan[channum] <= maxchans)) {
                            connchan[channum]++;
                        }

                        // Trunkhunt replacement

                        if (connchan[ channum ] > maxchans) {  // Error handling for all circuits being busy; make sure we're not trying to dial out on the D channel
                            ownies[ channum ] = 0;
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset receivers back to DTMF
                            // Error handling for all circuits being busy
                            connchan[channum] = 0;
                            dxinfox[ channum ].state = ST_ISDNACB;
                            // sprintf( dxinfox[ channum ].msg_name, "sounds/acb_error.pcm" );
                            dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);

                            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0)  == -1) {
                                set_hkstate(channum, DX_ONHOOK);
                            }

                            disp_msg("Error: all circuits are busy");
                            return (-1);
                        }
                        */

                        connchan[connchan[channum]] = channum;
                        dxinfox[connchan[channum]].state = ST_2600ROUTE2;

                        if (isdninfo[channum].cpn[0] == '\0') {
                            sprintf(isdninfo[channum].cpn, "12525350002");
                        }

                        sprintf(filetmp[channum], "78%s", (dxinfox[channum].digbuf.dg_value + 1));    // Get rid of the KP, 1. Also, this should be memset.
                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600

                        if (altsig & 4) {
                            if (!fprintf(calllog, "%s;MF Destination %s;CPN %s;\n", timeoutput(channum), (dxinfox[channum].digbuf.dg_value + 1), isdninfo[channum].cpn)) {
                                disp_msg("Failed to log");
                            }

                            fflush(calllog);
                        }

                        makecall(connchan[channum], filetmp[channum], isdninfo[channum].cpn, FALSE);   // Call the number in the digit buffer

                        // 'kay, we're done with the digit buffer. Let's blow it away.
                        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                        }

                        // get_digs( channum, &dxinfox[ channum ].digbuf, 1, 0, 0 ); // For 2600 detection
                        return (0);
                    }

                case '1':
                    disp_status(channum, "Unformatted number. Attempting PRI-800 route...");

                    if ( (filecount[channum] != 11) ||
                         (dxinfox[ channum ].digbuf.dg_value[1] != '8') || 
                         (dxinfox[ channum ].digbuf.dg_value[2] != dxinfox[ channum ].digbuf.dg_value[3]) || 
                         (dxinfox[ channum ].digbuf.dg_value[2] == '1') ||
                         (dxinfox[ channum ].digbuf.dg_value[2] == '9') ) {
                        // 1 + 7D is eight digits. If that's not received, give a general error
                        disp_msgf("Invalid number received. Digits were %s", dxinfox[channum].digbuf.dg_value);
                        dxinfox[ channum ].msg_fd = open("sounds/cbcad.pcm", O_RDONLY);

                        if (dxinfox[ channum ].msg_fd == -1) {
                            // Uhh, this is bad. Let's hang up.
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset digit receiver to only hear DTMF.
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                            return (-1);
                        } else {
                            play(channum, dxinfox[ channum ].msg_fd, 0x101, 0, 0);
                        }

                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                    }

                    else {
                        // Route the call. It's pre-formatted, so this should be simple.
                        // We shouldn't need to treat the D channels specially since they never get set to state ST_WTRING
                        connchan[ channum ] = idle_trunkhunt(channum, 1, maxchans, true);
                        dxinfox[channum].state = ST_2600ROUTE;

                        disp_msgf("Dest. channel is %d", connchan[channum]);

                        // Is there any way to make the while loop terminate before hitting maxchans without testing every time? This is going to waste CPU cycles...
                        /*
                        while ((dxinfox[connchan[channum]].state != ST_WTRING) && (connchan[channum] <= maxchans)) {
                            connchan[channum]++;
                        }

                        disp_msgf("Dest. channel is %d", connchan[channum]);
                        // Trunkhunt replacement

                        if (connchan[ channum ] > maxchans) {  // Error handling for all circuits being busy; make sure we're not trying to dial out on the D channel
                            ownies[ channum ] = 0;
                            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset receivers back to DTMF
                            // Error handling for all circuits being busy
                            connchan[channum] = 0;
                            dxinfox[ channum ].state = ST_ISDNACB;
                            // sprintf( dxinfox[ channum ].msg_name, "sounds/acb_error.pcm" );
                            dxinfox[ channum ].msg_fd = open("sounds/acb_error.pcm", O_RDONLY);

                            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0)  == -1) {
                                set_hkstate(channum, DX_ONHOOK);
                            }

                            disp_msg("Error: all circuits are busy");
                            return (-1);
                        }
                        */

                        connchan[connchan[channum]] = channum;
                        dxinfox[connchan[channum]].state = ST_2600ROUTE2;

                        if (isdninfo[channum].cpn[0] == '\0') {
                            sprintf(isdninfo[channum].cpn, "12525350002");
                        }

                        // 78+ for Hatbowldash's ISDN ISDN PRI
                        dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                        sprintf(filetmp[channum], "78%s", dxinfox[ channum ].digbuf.dg_value);

                        if (altsig & 4) {
                            if (!fprintf(calllog, "%s;MF Destination %s;CPN %s;\n", timeoutput(channum), filetmp[channum], isdninfo[channum].cpn)) {
                                disp_msg("Failed to log");
                            }

                            fflush(calllog);
                        }

                        makecall(connchan[channum], filetmp[channum], isdninfo[channum].cpn, FALSE);   // Call the number in the digit buffer

                        // 'kay, we're done with the digit buffer. Let's blow it away.
                        if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                        }

                        // get_digs( channum, &dxinfox[ channum ].digbuf, 1, 0, 0 );
                        return (0);
                    }

                default:
                    disp_status(channum, "Unrecognized MF string received - treating as invalid.");
                    disp_msgf("Invalid number received. Digits were %s", dxinfox[channum].digbuf.dg_value);
                    dxinfox[ channum ].msg_fd = open("sounds/facilitytrouble.pcm", O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        // Uhh, this is bad. Let's hang up.
                        dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);   // Reset digit receiver to only hear DTMF.
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    } else {
                        play(channum, dxinfox[ channum ].msg_fd, 0x101, 0, 0);
                    }

                    dx_setdigtyp(dxinfox[ channum ].chdev, '\0');   // Only listen for 2600
                    return (0);

            }

            return (0);

        case ST_CATPAUSE:

            // If we arrive here, we should resume playback of the file we were using before.
            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            if (ownies[ channum ] == 6) {
                ownies[ channum ] = 5;
            }

            sprintf(dxinfox[channum].msg_name, "%s", filetmp2[channum]);
            dxinfox[ channum ].state = ST_GETCAT3;
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (dxinfox[ channum ].msg_fd == -1) {
                // Uhh, this is bad. Let's hang up.
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            } else {
                play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
            }

            return (0);

        case ST_ISDNROUTE2:
        case ST_ISDNROUTE1:
            filecount[ channum ] = (strlen(dxinfox[ channum ].digbuf.dg_value) - 1);     // We'll, uh, we'll just borrow this variable. Yes, I'm really going out of my way to save 24(ish) bytes.

            // Is there a # key (0x23) as the last digit? That shouldn't be part of the destination.
            if (dxinfox[ channum ].digbuf.dg_value[ filecount[ channum ] ] == 0x23) {
                dxinfox[ channum ].digbuf.dg_value[ filecount[ channum ] ] = '\0';
            }

            if (ownies[ channum ] == 0) {
                ownies[ channum ]++;    // If someone typed 0 for the port number, well, let's make it 1?
            }

            connchan[channum] = idle_trunkhunt( channum, (((ownies[ channum ] - 1) * 24) + 1), (ownies[channum] * 24), false);
            if (connchan[channum] == -1) return -1;
            disp_msgf("Dest. channel is %d", connchan[channum]);

            connchan[connchan[channum]] = channum;

            // set_rdnis( connchan[channum], "2127365000", 0x81);
            if (altsig & 4) {
                if (!fprintf(calllog, "%s;ISDN TL Destination %s;Outgoing CPN %s;In channel %d;Out channel %d\n", timeoutput(channum), dxinfox[ channum ].digbuf.dg_value, dm3board ? config.defaultcpn : config.origtestcpn, channum, connchan[channum])) {
                    disp_msg("Failed to log");
                }

                fflush(calllog);
            }

            //set_cpname( connchan[channum], "HATBOWL" );
            if (dxinfox[ channum ].state == ST_ISDNROUTE1) {
                if (dm3board == FALSE) {
                    makecall(connchan[channum], dxinfox[ channum ].digbuf.dg_value, config.origtestcpn, NOREC);    // Call the number in the digit buffer
                }

                if (dm3board == TRUE) {
                    makecall(connchan[channum], dxinfox[ channum ].digbuf.dg_value, config.defaultcpn, NOREC);    // Call the number in the digit buffer
                }
            } else {
                sprintf(dxinfox[ connchan[ channum ] ].msg_name, "%s-%s-%lu.pcm", isdninfo[channum].dnis, isdninfo[channum].cpn, (unsigned long)time(NULL));
                dxinfox[ connchan [ channum ] ].msg_fd = open(dxinfox[ connchan [ channum ] ].msg_name, O_RDWR | O_TRUNC | O_CREAT, 0666);

                if (dm3board == FALSE) {
                    makecall(connchan[channum], dxinfox[ channum ].digbuf.dg_value, config.origtestcpn, REC);    // Call the number in the digit buffer
                }

                if (dm3board == TRUE) {
                    makecall(connchan[channum], dxinfox[ channum ].digbuf.dg_value, config.defaultcpn, REC);    // Call the number in the digit buffer
                }
            }

            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // Since we don't need the digit buffer anymore, let's clear it; other things might need it.
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            dxinfox[ channum ].state = ST_ROUTEDTEST;
            // get_digs(channum, &dxinfox[ channum ].digbuf, 0, 0, 0x100F); // Moved to progress handlers
            return (0);


        case ST_ISDNROUTE:

            if ((dxinfox[ channum ].digbuf.dg_value[0] > 0x39) || (dxinfox[ channum ].digbuf.dg_value[0] < 0x30) || ((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) > (maxchans / 23)))
                // NOTE: The last part of the above statement will become problematic for E1 boards. You may want to revise it.
            {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                // sprintf( dxinfox [ channum ].msg_name, "sounds/porterror.pcm" );
                dxinfox[ channum ].msg_fd = open("sounds/porterror.pcm", O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                return (0);
            }

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            ownies[ channum ] = (dxinfox[ channum ].digbuf.dg_value[0] - 0x30);

            if (errcnt[ channum ] == 9) {
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_ISDNROUTE2;
                playtone(channum, 400, 350);
                return (0);
            }

            dxinfox[ channum ].state = ST_ISDNROUTE1;
            playtone(channum, 350, 440);
            return (0);

        case ST_VMBDETECT:

            if (strcmp("I", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 1000 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;1000;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("J", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 440 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;440;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("K", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 790 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;790;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("L", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 950 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;950;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("M", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 2000 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;2000;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("N", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 500 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;500;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("O", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 1400 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;1400;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("P", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 1330 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;1330;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("Q", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 800 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;800;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("R", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 425 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;425;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("S", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 745 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;745;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("T", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 850 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;850;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else if (strcmp("U", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // It's a 1050 hertz tone
                if (!fprintf(logdescriptor, "%s;%s;PAMD;1050;\n", timeoutput(channum), dialerdest[ channum ])) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            else {
                if (!fprintf(logdescriptor, "%s;%s;PAMD;NOTONES;%s\n", timeoutput(channum), dialerdest[ channum ], dxinfox[ channum ].digbuf.dg_value)) {
                    disp_msg("Failed to log");
                }

                fflush(logdescriptor);
            }

            if (dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1000, DM_TONEON) == -1) {
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                disp_msg("Couldn't disable 1000 hertz tone!");
            }

            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_440, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_790, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_950, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_2000, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_500, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1330, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_800, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_425, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_745, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_850, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1050, DM_TONEON);

            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            if (chaninfo[initchan[channum]].dialer.using_list) {
                // To do: put a function here that'll get a new number from the list
                dxinfox[ channum ].state = ST_CALLPTEST5;
                disp_status(channum, "Hanging up...");
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            if (frontend != CT_GCISDN) {
                scancount[initchan[ channum ] ]--; // This and the below check should be unnecessary now for ISDN

                if (scancount[initchan[ channum ] ] <= 0) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
                    set_hkstate(channum, DX_ONHOOK);
                    return (0);
                }
            }

            dxinfox[ channum ].state = ST_CALLPTEST5;

            if ((config.dialersound[0] != '\0') && (isdnstatus[channum] == 1)) {
                // isdnstatus[channum] == 1 is a fix for this thing trying to play sounds while on-hook. It really shouldn't.
                dxinfox[ channum ].msg_fd = open(config.dialersound, O_RDONLY);

                if (dxinfox[ channum ].msg_fd != -1) {
                    disp_status(channum, "Playing PAMD sound");
                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1);
                    return (0);
                }
            }

            disp_status(channum, "Hanging up...");
            set_hkstate(channum, DX_ONHOOK);
            return (0);

        case ST_MODEMDETECT:


            switch (dxinfox[ channum ].digbuf.dg_value[ 0 ]) {

                case 'F':

                    // It's a fax
                    if (!fprintf(logdescriptor, "%s;%s;FAX;FAX;\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case 'G':

                    // It's a modem
                    if (!fprintf(logdescriptor, "%s;%s;FAX;MODEM;\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case 'H':

                    // Still a modem
                    if (!fprintf(logdescriptor, "%s;%s;FAX;MODEM2;\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                case 'O':

                    // Still a modem. But this time, an alarm thing modem
                    if (!fprintf(logdescriptor, "%s;%s;FAX;ALARM_MODEM;\n", timeoutput(channum), dialerdest[ channum ])) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
                    break;

                default:
                    if (!fprintf(logdescriptor, "%s;%s;FAX;NODETECT - %s;\n", timeoutput(channum), dialerdest[ channum ], dxinfox[ channum ].digbuf.dg_value)) {
                        disp_msg("Failed to log");
                    }

                    fflush(logdescriptor);
            }


            dx_distone(dxinfox[ channum ].chdev, TID_FAX, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MODEM, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MODEM2, DM_TONEON);
            dx_distone(dxinfox[ channum ].chdev, TID_MBEEP_1400, DM_TONEON);

            if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // This really shouldn't be necessary...
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
            }

            if (chaninfo[initchan[channum]].dialer.using_list) {
                dxinfox[channum].state = ST_CALLPTEST5;
                disp_status(channum, "Hanging up...");
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            if (frontend != CT_GCISDN) {

                scancount[initchan[ channum ] ]--;

                if (scancount[ initchan[ channum ] ] <= 0) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    dx_deltones(dxinfox[ channum ].chdev);   // This is so re-adding the same tones won't fail
                }

                else {
                    dxinfox[ channum ].state = ST_CALLPTEST5;
                    disp_status(channum, "Hanging up...");
                }
            }

            // With the frontend check, does this code get executed twice if it's analog? Probably not, but we should make sure.

            else {
                dxinfox[ channum ].state = ST_CALLPTEST5;
                disp_status(channum, "Hanging up...");
            }

            set_hkstate(channum, DX_ONHOOK);

            return (0);

        case ST_CALLPTEST3:

            // Scancount/number of calls to dial
            switch (strlen(dxinfox[ channum ].digbuf.dg_value)) {

                case 7:

                case 6:

                case 5:

                    if ((dxinfox[ channum ].digbuf.dg_value[4] < 0x30) ||
                            (dxinfox[ channum ].digbuf.dg_value[4] > 0x39)) {
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST3E;
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }
                    }

                    scancount[ channum ] = (((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) * 10000)
                                            + ((dxinfox[ channum ].digbuf.dg_value[1] - 0x30) * 1000)
                                            + ((dxinfox[ channum ].digbuf.dg_value[2] - 0x30) * 100)
                                            + ((dxinfox[ channum ].digbuf.dg_value[3] - 0x30) * 10)
                                            + (dxinfox[ channum ].digbuf.dg_value[4] - 0x30));

                    break;

                case 4:

                    if ((dxinfox[ channum ].digbuf.dg_value[3] < 0x30) ||
                            (dxinfox[ channum ].digbuf.dg_value[3] > 0x39)) {
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST3E;
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }
                    }

                    scancount[ channum ] = (((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) * 1000)
                                            + ((dxinfox[ channum ].digbuf.dg_value[1] - 0x30) * 100)
                                            + ((dxinfox[ channum ].digbuf.dg_value[2] - 0x30) * 10)
                                            + (dxinfox[ channum ].digbuf.dg_value[3] - 0x30));
                    break;

                case 3:

                    if ((dxinfox[ channum ].digbuf.dg_value[2] < 0x30) ||
                            (dxinfox[ channum ].digbuf.dg_value[2] > 0x39)) {
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST3E;
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }
                    }

                    scancount[ channum ] = (((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) * 100)
                                            + ((dxinfox[ channum ].digbuf.dg_value[1] - 0x30) * 10)
                                            + (dxinfox[ channum ].digbuf.dg_value[2] - 0x30));
                    break;

                case 2:

                    if ((dxinfox[ channum ].digbuf.dg_value[1] < 0x30) ||
                            (dxinfox[ channum ].digbuf.dg_value[1] > 0x39)) {
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST3E;
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }
                    }

                    scancount[ channum ] = (((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) * 10)
                                            + (dxinfox[ channum ].digbuf.dg_value[1] - 0x30));
                    break;

                case 1:

                    if ((dxinfox[ channum ].digbuf.dg_value[0] < 0x30) ||
                            (dxinfox[ channum ].digbuf.dg_value[0] > 0x39)) {
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST3E;
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }
                    }

                    scancount[ channum ] = ((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) + scancount[ channum ]);
                    disp_msgf("scancount is %d", scancount[ channum ]);

                    if (scancount[ channum ] == 0) {
                        // This is old code to stop people from saying there were zero numbers to dial
                        /*
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST3E;
                            play(channum, invalidfd, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0);
                            return (0);
                        }
                        */
                    scancount[ channum ] = -2000;
                    disp_msg("Repeat dialer function invoked");
                    }

                    break;

                default:

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    errcnt[channum]++;

                    if (errcnt[ channum ] <= 3) {
                        dxinfox[ channum ].state = ST_CALLPTEST3E;
                        play(channum, invalidfd, 0, 0, 0);
                        return (0);
                    }

                    else {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, goodbyefd, 0, 0, 0);
                        return (0);
                    }

            }

            dxinfox[channum].state = ST_CALLPTEST4;

            sprintf(dxinfox [ channum ].msg_name, "sounds/start.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (errcode[ channum ]);

        case ST_CALLPTEST2: /* Got response to asking for number of dialer channels */
            // Number of channels
            // length[channum] = strlen( dxinfox[ channum ].digbuf.dg_value ); // This is unnecessary.

            chans[channum] = 0;

            switch (strlen(dxinfox[ channum ].digbuf.dg_value)) {
                case 2:

                    if ((dxinfox[ channum ].digbuf.dg_value[0] < 0x31) ||
                            (dxinfox[ channum ].digbuf.dg_value[0] > 0x39) ||
                            (chans[channum] = (((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) * 10) + (dxinfox[ channum ].digbuf.dg_value[1] - 0x30))) < 1) {

                        disp_msg("Error in digit collection");

                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST2E;
                            // Develop this state ^
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }

                    }

                    // chans[channum] = ( ( ( dxinfox[ channum ].digbuf.dg_value[0] - 0x30 ) * 10 ) + (dxinfox[ channum ].digbuf.dg_Value[1] - 0x30 ) );

                    break;

                case 1:

                    if ((dxinfox[ channum ].digbuf.dg_value[0] < 0x31) ||
                            (dxinfox[ channum ].digbuf.dg_value[0] > 0x39)) {
                        if (dx_clrdigbuf(chdev) == -1) {
                            disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        }

                        errcnt[channum]++;

                        if (errcnt[ channum ] <= 3) {
                            dxinfox[ channum ].state = ST_CALLPTEST2E;
                            // Develop this state ^
                            play(channum, invalidfd, 0, 0, 0);
                            return (0);
                        }

                        else {
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0, 0);
                            return (0);
                        }

                    }

                    chans[channum] = ((dxinfox[ channum ].digbuf.dg_value[0] - 0x30) + chans[channum]);

                    break;

                default:

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    errcnt[channum]++;

                    if (errcnt[ channum ] <= 3) {
                        dxinfox[ channum ].state = ST_CALLPTEST2E;
                        // Develop this state ^
                        play(channum, invalidfd, 0, 0, 0);
                        return (0);
                    }

                    else {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, goodbyefd, 0, 0, 0);
                        return (0);
                    }

            }

            disp_msgf("Number of channels is %d", chans[ channum ]);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if (chans[channum] > maxchans) {
                chans[channum] = maxchans;
                dxinfox[ channum ].state = ST_CALLPTEST3E;
                sprintf(dxinfox [ channum ].msg_name, "sounds/notenoughchannels.pcm");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

                if (errcode[channum] == -1) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (errcode[ channum ]);
            }

            // Are we using the list dialer function? Don't ask for how many numbers to scan; that should be implicit in the list length
            if (chaninfo[channum].dialer.using_list) {
                dxinfox[ channum ].state = ST_CALLPTEST4;

                dxinfox[ channum ].msg_fd = open("sounds/start.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[ channum ]);
            }

            dxinfox[channum].state = ST_CALLPTEST3;
            dxinfox[channum].msg_fd = open("sounds/enter_numbers.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

            if (errcode[channum] == -1) {
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (errcode[channum]);

        case ST_CALLPTEST:
            digcount = strlen(dxinfox[ channum ].digbuf.dg_value);

            if (digcount < 3) {
                if (dxinfox[ channum ].digbuf.dg_value[0] == 0x23) {
                    // List mode
                    // To do: make the list file a command line changeable argument. And maybe if we ever make a config
                    // file, something there instead.
                    if (!dialer_list_load("scanfile")) {
                        // Ideally, this should play a special recording, but let's just treat it like an error for now.
                        disp_msg("List file could not be found / opened. Please create and try again");
                        dxinfox[ channum ].state = ST_CALLPTESTE;
                        play(channum, errorfd, 0, 0, 1);
                        return (0);
                    }

                    chaninfo[channum].dialer.using_list = true;
                    /* load dialer_list_next into dialerdest[channum] and make sure it's not null. if it returns NULL, do the next code: */
                    /*
                                                disp_msg("dialer_next list error or EOF!");
                            dxinfox[ channum ].state = ST_GOODBYE;
                            play(channum, goodbyefd, 0, 0);
                    */
                    scancount[channum] = 0;
                    filecount[channum] = 0;

                    dxinfox[ channum ].state = ST_CALLPTEST2;
                    sprintf(dxinfox [ channum ].msg_name, "sounds/enter_channels.pcm");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

                    if (errcode[channum] == -1) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }

                    return (errcode[ channum ]);
                }

                errcnt[ channum ]++;

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcnt[ channum ] <= 3) {
                    dxinfox[ channum ].state = ST_CALLPTESTE;
                    // Develop this state ^
                    play(channum, invalidfd, 0, 0, 0);
                    return (0);
                }

                else {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

            }

            ownies[channum] = 0;

            while (ownies[channum] < digcount) {
                if ((dxinfox[ channum ].digbuf.dg_value[ownies[channum]] < 0x21) ||
                        (dxinfox[ channum ].digbuf.dg_value[ownies[channum]] > 0x39)) {
                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    if (errcnt[ channum ] <= 3) {
                        dxinfox[ channum ].state = ST_CALLPTESTE;
                        // Develop this state ^
                        play(channum, invalidfd, 0, 0, 0);
                        return (0);
                    } else {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, goodbyefd, 0, 0, 0);
                        return (0);
                    }
                }

                else {
                    ownies[channum]++;
                }
            }

            ownies[channum] = 0;
            // Ideally, we should just be able to do this with one variable instead of an array of them.
            // Let's look into that when we hit the logging portion of the software.
            sprintf(dialerdest[channum], "%s", dxinfox[ channum ].digbuf.dg_value);

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            errcnt[channum] = 0;
            dxinfox[ channum ].state = ST_CALLPTEST2;
            sprintf(dxinfox[ channum ].msg_name, "sounds/enter_channels.pcm");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

            if (errcode[channum] == -1) {
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (errcode[channum]);

        // This code below is old and should go elsewhere

        case ST_PLAYMULTI1:

            if (strcmp("74211", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[ channum ].state = ST_TXDATA;
                sprintf(dxinfox[ channum ].msg_name, "bell202test.txt");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                send_bell202(channum, dxinfox[ channum ].msg_fd);
                return (0);
            }

            ownies[ channum ] = digread(channum, dxinfox[channum].digbuf.dg_value);
            return (0);

        case ST_COLLCALL:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_COLLCALL2;
                sprintf(dxinfox[ channum ].msg_name, "sounds/collect/enterdest.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

            if (strcmp("*", dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_COLLCALL3;
                sprintf(dxinfox[ channum ].msg_name, "sounds/collect/rates.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                // Error handler
                errcnt[ channum ]++;

                if (errcnt[ channum ] < 3) {
                    dxinfox[ channum ].state = ST_COLLCALL3;
                    play(channum, invalidfd, 0, 0, 0);
                    return (0);
                }

                else {
                    errcnt[ channum ] = 0;
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }
            }


        case ST_VMAILTYP2:
        case ST_VMAILTYP:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (dxinfox[ channum ].state == ST_VMAILTYP2) {
                    vmattrib[channum] = (vmattrib[channum] | 2);
                } else {
                    vmattrib[channum] = (vmattrib[channum] & 0xFD);
                }

                dxinfox[ channum ].state = ST_VMAILMENU;
                sprintf(filetmp3[ channum ], "%s/attrib", filetmp[ channum ]);
                passwordfile[ channum ] = fopen(filetmp3[ channum ], "w+");
                fprintf(passwordfile[ channum ], "%s %c", passcode[ channum ], vmattrib[channum]);
                fclose(passwordfile[ channum ]);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_changesaved.pcm");
            }

            else {
                dxinfox[ channum ].state = ST_VMAILMENU2;
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_mainmenu.pcm");
            }

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
            }

            if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (0);

        case ST_VMAILSETM:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                // Change greeting
                dxinfox[ channum ].state = ST_VMAILSETGREC2;
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/begingreeting.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Change passcode
                sprintf(filetmp3[channum], "%s/attrib", filetmp[channum]);   // This shouldn't be a problem, but let's reassign just in case
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_VMAILNPASS1;
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_enterpass.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

            if (strcmp("*", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Go back to main menu
                dxinfox[ channum ].state = ST_VMAILMENU2;
                sprintf(dxinfox[ channum].msg_name, "sounds/vmail/vmb_mainmenu.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                // Error handling code
                if (errcnt[ channum ] >= 2) {
                    errcnt[ channum ] = 0;
                    anncnum[ channum ] = 0;
                    passcode[channum][0] = '\0'; // Make sure this is a good idea
                    dxinfox[ channum ].state = ST_GOODBYE;
                    filecount[ channum ] = 0;
                    ownies[ channum ] = 0;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

                else {
                    errcnt[ channum ]++;
                    // This should go to a state that plays back the menu again.
                    // dxinfox[ channum ].state = ?;
                    play(channum, invalidfd, 0, 0, 0);
                    return (0);
                }
            }

        case ST_VMAILCOMP:
            // Write voicemail handling stuff here
            // I'm going to borrow the message name string here. Hope you don't mind; if it doesn't exist, we don't want to overwrite any useful voicemail stuff.
            sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/%s/attrib", dxinfox[ channum ].digbuf.dg_value);

            if (stat(dxinfox[ channum].msg_name, &sts) == -1) {
                // Attributes file doesn't exist, so this isn't likely a valid mailbox. Return to the main menu.
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/mailbox_noexist.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                dxinfox[ channum ].state = ST_VMAILMENU;
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                // Let's do it.
                anncnum[ channum ] = 0;
                filecount[ channum ] = 0;
                passcode[channum][0] = '\0'; // Just in case...
                ownies[channum] = 0; // This is our quick and dirty variable for on-hook message handling.
                //There's a better way to do this (^) so, well, do it. Later, I mean. Never now.
                disp_status(channum, "Accessing voicemail...");
                sprintf(filetmp[channum], "sounds/vmail/%s", dxinfox[ channum ].digbuf.dg_value); // Stick the path for the VMB somewhere useful

                if (stat(filetmp[channum], &sts) == -1) {
                    mkdir(filetmp[channum], 0700);    // Create the directory if it's not there
                }

                sprintf(filetmp2[channum], "%s/new", filetmp[channum]);

                if (stat(filetmp2[channum], &sts) == -1) {
                    mkdir(filetmp2[channum], 0700);    // Does the new directory exist?
                }

                sprintf(filetmp2[channum], "%s/temp", filetmp[channum]);

                if (stat(filetmp2[channum], &sts) == -1) {
                    mkdir(filetmp2[channum], 0700);    // Make sure the temporary directory exists too.
                }

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                sprintf(dxinfox[ channum ].msg_name, "%s/greeting.pcm", filetmp[channum]);
                sprintf(filetmp3[channum], "%s/attrib", filetmp[channum]);

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/generic_greeting.pcm");
                }

                if ((passwordfile[ channum ] = fopen(filetmp3[ channum ], "r")) != NULL) {
                    fscanf(passwordfile[ channum ], "%s %c", passcode[ channum ], &vmattrib[ channum ]);
                    fclose(passwordfile[ channum ]);
                }

                dxinfox[ channum ].state = ST_VMAIL1;
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                    file_error(channum, dxinfox[ channum ].msg_name);
                }

                return (errcode[channum]);
            }

        case ST_VMAILRNEW2:

            if (strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Save
                errcnt[channum] = 0;

                // To do: Move to (the current value).pcm using variable savedmsg[channum] in old/


                if (filecount[channum] == 0) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], anncnum[ channum ]);
                    sprintf(filetmp3[ channum ], "%s/old/%d.pcm", filetmp[ channum ], oldmsg[ channum ]);

                    if (rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]) != 0) {
                        // Move new message to old folder
                        disp_msg("Unable to perform message save rename() function");
                        newmsg[channum] = 0;
                        oldmsg[channum] = 0;
                        errcnt[ channum ] = 0;
                        passcode[ channum ][0] = '\0';
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, goodbyefd, 0, 0, 0);
                        return (0);
                    }

                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.atr", filetmp2[ channum ], anncnum[ channum ]);
                    sprintf(filetmp3[ channum ], "%s/old/%d.atr", filetmp[ channum ], oldmsg[ channum ]);
                    rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);   // Move attributes file

                    oldmsg[ channum ]++; // Increase the total number of saved messages since we're adding one here.

                    // Remaining messages need to be put in their proper place; else there'll be a gap.

                    newmsg[ channum ] = (anncnum[ channum ] + 1);
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], newmsg[ channum ]);

                    while (stat(dxinfox[ channum ].msg_name, &sts) != -1) {
                        sprintf(filetmp3[ channum ], "%s/%d.pcm", filetmp2[ channum ], (newmsg[ channum ] - 1));
                        rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.atr", filetmp2[ channum ], newmsg[ channum ]);
                        sprintf(filetmp3[ channum ], "%s/%d.atr", filetmp2[ channum ], (newmsg[ channum ] - 1));
                        rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                        newmsg[ channum ]++;
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], newmsg[ channum ]);
                    }

                    newmsg[ channum ]--;
                    // newmsg[ channum ] should be back at the proper value
                }

                else {
                    // Old message? Just skip to the next one.
                    anncnum[ channum ]++;
                }

                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/msgsaved.pcm");

                // How does savedmsg.pcm compare?

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                dxinfox[ channum ].state = ST_VMAILRNEW3;
                // anncnum[ channum ]++; // Uncomment if this is necessary; it looks like it isn't
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

            if (strcmp("8", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                // Repeat
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], anncnum[ channum ]);

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    close(dxinfox[ channum ].msg_fd);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/endofmsg.pcm");

                    if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                        disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    anncnum[channum] = 0;
                    dxinfox[ channum ].state = ST_VMAILCHECK1;
                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (0);
                }

                dxinfox[ channum ].state = ST_VMAILRNEW;
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                // Erase
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], anncnum[ channum ]);
                remove(dxinfox[ channum ].msg_name);
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.atr", filetmp2[ channum ], anncnum[ channum ]);
                remove(dxinfox[ channum ].msg_name);

                // Remaining messages need to be put in their proper place; else there'll be a gap.


                // This isn't very efficient codewriting, is it?
                if (filecount[ channum ] == 0) {

                    newmsg[ channum ] = (anncnum[ channum ] + 1);
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], newmsg[ channum ]);

                    while (stat(dxinfox[ channum ].msg_name, &sts) != -1) {
                        sprintf(filetmp3[ channum ], "%s/%d.pcm", filetmp2[ channum ], (newmsg[ channum ] - 1));
                        rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.atr", filetmp2[ channum ], newmsg[ channum ]);
                        sprintf(filetmp3[ channum ], "%s/%d.atr", filetmp2[ channum ], (newmsg[ channum ] - 1));
                        rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                        newmsg[ channum ]++;
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], newmsg[ channum ]);
                    }

                    newmsg[ channum ]--;

                }

                if (filecount[ channum ] == 1) {

                    oldmsg[ channum ] = (anncnum[ channum ] + 1);
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], oldmsg[ channum ]);

                    while (stat(dxinfox[ channum ].msg_name, &sts) != -1) {
                        sprintf(filetmp3[ channum ], "%s/%d.pcm", filetmp2[ channum ], (oldmsg[ channum ] - 1));
                        rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.atr", filetmp2[ channum ], oldmsg[ channum ]);
                        sprintf(filetmp3[ channum ], "%s/%d.atr", filetmp2[ channum ], (oldmsg[ channum ] - 1));
                        rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                        oldmsg[ channum ]++;
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[ channum ], oldmsg[ channum ]);
                    }

                    oldmsg[ channum ]--;

                }

                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/msgerased.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                // Advance message counter up by 1. Is this necessary?
                // anncnum[ channum ]++;
                dxinfox[ channum ].state = ST_VMAILRNEW3;
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("*", dxinfox[ channum ].digbuf.dg_value) == 0) {
                sprintf(filetmp2[channum], "%s/new", filetmp[channum]);
                errcnt[ channum ] = 1;
                anncnum[ channum ] = 0;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[channum], anncnum[channum]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                newmsg[channum] = anncnum[channum];
                anncnum[channum] = 0;
                ownies[ channum ] = 110;

                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_youhave.pcm");
                multiplay[channum][0] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.pcm", newmsg[channum]);
                multiplay[channum][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/newmsg.pcm");
                multiplay[channum][2] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/and.pcm");
                multiplay[channum][3] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                sprintf(filetmp2[channum], "%s/old", filetmp[channum]);
                errcnt[ channum ] = 1;
                anncnum[ channum ] = 0;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[channum], anncnum[channum]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                oldmsg[channum] = anncnum[channum];
                anncnum[channum] = 0;
                sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.pcm", oldmsg[channum]);

                multiplay[channum][4] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/savedmsg.pcm");
                multiplay[channum][5] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                dxinfox[channum].state = ST_VMAILMENU;

                if ((errcode[channum] = playmulti(channum, 6, 128, multiplay[channum])) == -1) {
                    disp_msg("VMAILRNEW2 function passed bad data to the playmulti function.");
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (0);
            }

            else {
                // Error handler
                errcnt[ channum ]++;

                if (errcnt[ channum ] >= 3) {
                    // Dispense with them
                    anncnum[channum] = 0;
                    newmsg[channum] = 0;
                    oldmsg[channum] = 0;
                    errcnt[channum] = 0;
                    passcode[channum][0] = '\0';
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

                else {
                    dxinfox[channum].state = ST_VMAILRNEW4;// Uhh, what state do we use for this?
                    play(channum, invalidfd, 0, 0, 0);
                    return (0);
                }
            }


        case ST_VMAILMENU2:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;
                anncnum[ channum ] = 0;
                filecount[ channum ] = 0;

                // Listen to new messages

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (newmsg[ channum ] <= 0) {
                    // There's no messages. Don't enter the playback routine.
                }

                dxinfox[ channum ].state = ST_VMAILRNEW;
                anncnum[ channum ] = 0;
                sprintf(filetmp2[ channum ], "%s/new", filetmp[ channum ]);
                // To do: add feedback prompts. Like, "first message/next message"

                sprintf(dxinfox[ channum ].msg_name, "%s/0.pcm", filetmp2[ channum ]);

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    dxinfox[channum].state = ST_VMAILCHECK1;
                    dxinfox[channum].msg_fd = open("sounds/vmail/endofmsg.pcm", O_RDONLY);
                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1) == -1) {
                        disp_msg("Couldn't find the end of MSG!");
                        file_error(channum, "sounds/vmail/endofmsg.pcm");
                    }
                    return -1;
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;
                filecount[ channum ] = 1;

                // Listen to saved messages

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (oldmsg[ channum ] <= 0) {
                    // There's no messages. Don't enter the playback routine.
                }

                dxinfox[ channum ].state = ST_VMAILRNEW; // Rename this voicemail state, get rid of the one for old messages
                anncnum[ channum ] = 0;
                sprintf(filetmp2[ channum ], "%s/old", filetmp[ channum ]);

                sprintf(dxinfox[ channum ].msg_name, "%s/0.pcm", filetmp2[ channum ]);

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    dxinfox[channum].state = ST_VMAILCHECK1;
                    dxinfox[channum].msg_fd = open("sounds/vmail/endofmsg.pcm", O_RDONLY);
                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 1) == -1) {
                        disp_msg("Couldn't find the end of MSG!");
                        file_error(channum, "sounds/vmail/endofmsg.pcm");
                    }
                    return -1;
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;

                // Compose a message

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dxinfox[ channum ].state = ST_VMAILCOMP;

                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/enter_msgextension.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("6", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;

                // Change mailbox settings

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dxinfox[ channum ].state = ST_VMAILSETM;
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_settingsmenu.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;

                // Change mailbox type

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }


                if (vmattrib[ channum ] & 2) {
                    dxinfox[ channum ].state = ST_VMAILTYP;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_acceptmsg.pcm");
                }

                else {
                    dxinfox[ channum ].state = ST_VMAILTYP2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_announceonly.pcm");
                }

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Exit

                if (dx_clrdigbuf(chdev) == -1) {     // Should we do this upon exiting?
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                // Reset all the variables. They should be reset as necessary anyway, but there
                // could be some nasty bugs if we ever forget to do that. Better to be safe.
                newmsg[channum] = 0;
                oldmsg[channum] = 0;
                errcnt[ channum ] = 0;
                passcode[ channum ][0] = '\0';
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, goodbyefd, 0, 0, 0);
                return (0);
            }

            if (strcmp("0", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Hidden option to hit IVR menu
                newmsg[channum] = 0;
                oldmsg[channum] = 0;
                errcnt[channum] = 0;
                passcode[ channum ][0] = '\0';

                if (dx_clrdigbuf(chdev) == 1) {
                    disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                dxinfox[ channum ].state = ST_DIGPROC;
                playtone(channum, 400, 0);
                return (0);
            }

            else {
                // Error handling. Bleah.
                if (dx_clrdigbuf(chdev) == 1) {
                    disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                errcnt[channum]++;

                if (errcnt[ channum ] >= 3) {
                    newmsg[channum] = 0;
                    oldmsg[channum] = 0;
                    errcnt[ channum ] = 0;
                    passcode[ channum ][0] = '\0';
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

                dxinfox[ channum ].state = ST_VMAILMENU;
                play(channum, invalidfd, 0, 0, 0);
                return (0);

            }

            return (0);

        case ST_VMAILGRECEDIT3:
        case ST_VMAILGRECEDIT1:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Save
                errcnt[channum] = 0;

                if (dxinfox[ channum ].state == ST_VMAILGRECEDIT1) {
                    dxinfox[ channum ].state = ST_VMAILCHECK1;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/mailbox_set.pcm");
                }

                else {
                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        return (-1);
                    }

                    sprintf(dxinfox[ channum ].msg_name, "%s/temp/greeting.pcm", filetmp[ channum ]);
                    sprintf(filetmp3[ channum ], "%s/greeting.pcm", filetmp[ channum ]);
                    rename(dxinfox[ channum ].msg_name, filetmp3[ channum ]);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/greeting_changed.pcm");
                    dxinfox[ channum ].state = ST_VMAILMENU;
                }

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Erase
                errcnt[channum] = 0;

                if (dxinfox[ channum ].state == ST_VMAILGRECEDIT3) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/temp/greeting.pcm", filetmp[channum]);
                } else {
                    sprintf(dxinfox[ channum ].msg_name, "%s/greeting.pcm", filetmp[channum]);
                }

                dxinfox[ channum ].state = ST_VMAILSETGREC;

                if (remove(dxinfox[ channum ].msg_name) == -1) {
                    disp_msg("Uhh, something's wrong. Couldn't delete greeting.");
                }

                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/deleted_startmsg.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    file_error(channum, dxinfox[ channum ].msg_name);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);


            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Playback
                errcnt[channum] = 0;

                if (dxinfox[ channum ].state == ST_VMAILGRECEDIT3) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/temp/greeting.pcm", filetmp[channum]);
                } else {
                    sprintf(dxinfox[ channum ].msg_name, "%s/greeting.pcm", filetmp[channum]);
                }

                dxinfox[ channum ].state = ST_VMAILGRECEDIT1E;

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                dxinfox[ channum ].state = ST_VMAILGRECEDIT1E;

                if (errcnt[ channum ] > 2) {
                    errcnt[ channum ] = 0;
                    passcode[ channum ][0] = '\0';
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

                else {
                    errcnt[ channum ]++;
                    play(channum, invalidfd, 0, 0, 0);
                    return (0);

                }

            }

        case ST_VMAILSETUP2:

            vmattrib[channum] = (vmattrib[channum] & 0xFE); // Remove vmail setup tag
            passwordfile[ channum ] = fopen(filetmp3[ channum ], "w+");
            fprintf(passwordfile[ channum ], "%s %c", passcode[ channum ], vmattrib[channum]);
            fclose(passwordfile[ channum ]);

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                // Change greeting
                dxinfox[ channum ].state = ST_VMAILSETGREC;
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/begingreeting.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    file_error(channum, dxinfox[ channum ].msg_name);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                dxinfox[ channum ].state = ST_VMAILCHECK1;
                //sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/mailbox_set.pcm");

                if ((dxinfox[ channum ].msg_fd = open("sounds/vmail/mailbox_set.pcm", O_RDONLY)) == -1) {
                    file_error(channum, "sounds/vmail/mailbox_set.pcm");
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                // Bleah, I hate writing error handling code
                errcnt[channum]++;

                if (errcnt[channum] > 3) {
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_GOODBYE;

                    if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                        disp_msg("Couldn't play goodbye message in error handler.");
                        return (-1);
                    }

                    return (0);
                }

                if ((dxinfox[ channum ].msg_fd = open("sounds/vmail/vmb_intro2.pcm", O_RDONLY)) == -1) {
                    file_error(channum, "sounds/vmail/vmb_intro2.pcm");
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

        case ST_VMAILNPASS1C:
        case ST_VMAILSETUP1C:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (dxinfox[ channum ].state == ST_VMAILNPASS1C) {
                    passwordfile[ channum ] = fopen(filetmp3[ channum ], "w+");
                    fprintf(passwordfile[ channum ], "%s %c", passcode[ channum ], vmattrib[channum]);
                    fclose(passwordfile[ channum ]);
                    dxinfox[ channum ].state = ST_VMAILSETMP;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_passcodechanged.pcm");
                }

                else {
                    dxinfox[ channum ].state = ST_VMAILSETUP2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_intro2.pcm");
                }

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                passcode[channum][0] = '\0';
                dxinfox[ channum ].state = ST_VMAILSETUP1;
                sprintf(dxinfox[channum].msg_name, "sounds/vmail/vmb_intro1.pcm");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                    disp_msg("Uh, is the voicemail setup message working? It's not working here");
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (0);
            }

        case ST_VMAILNPASS1:
        case ST_VMAILSETUP1:

            if (strlen(dxinfox[ channum ].digbuf.dg_value) != 6) {
                errcnt[ channum ]++;

                // Error handling routine

                if (errcnt[channum] > 3) {
                    // Clear variables for voicemail stuff here
                    errcnt[ channum ] = 0;
                    loggedin[channum] = 0; // Make sure this is necessary
                    passcode[channum][0] = '\0'; // Just in case...
                    ownies[channum] = 0;
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

                if (dxinfox [ channum ].state == ST_VMAILSETUP1) {
                    dxinfox[ channum ].state = ST_VMAILSETUP1E;    // This is crude and wasteful of states...
                } else {
                    dxinfox[ channum ].state = ST_VMAILNPASS1E;
                }

                play(channum, invalidfd, 0, 0, 0);
                return (0);
            }

            else {

                errcnt[channum] = 0;
                sprintf(passcode[ channum ], "%s", dxinfox[ channum ].digbuf.dg_value);

                // I'm not proud of this code. Is there any better way to do this?

                if ((strcmp("123456", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("654321", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("111111", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("222222", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("333333", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("444444", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("555555", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("666666", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("777777", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("888888", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("999999", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("000000", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                        (strcmp("696969", dxinfox[ channum ].digbuf.dg_value) == 0)) {

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    if (dxinfox [ channum ].state == ST_VMAILSETUP1) {
                        dxinfox[channum].state = ST_VMAILSETUP1C;
                    } else {
                        dxinfox[ channum ].state = ST_VMAILNPASS1C;
                    }

                    sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_shittypass.pcm");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                        disp_msg("Error playing shittypasscode message");
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    return (0);

                }

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }


                if (dxinfox [ channum ].state == ST_VMAILSETUP1) {
                    dxinfox[channum].state = ST_VMAILSETUP2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_intro2.pcm");
                }

                else {
                    passwordfile[ channum ] = fopen(filetmp3[ channum ], "w+");
                    fprintf(passwordfile[ channum ], "%s %c", passcode[ channum ], vmattrib[channum]);
                    fclose(passwordfile[ channum ]);
                    dxinfox[ channum ].state = ST_VMAILSETMP;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_passcodechanged.pcm");
                }

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }


        case ST_FAKECONF1:

            if (strcmp("131337", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[channum].state = ST_FAKECONF2;
                sprintf(dxinfox[ channum ].msg_name, "sounds/meetme-inproog.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            else {
                errcnt[channum]++;

                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                if (errcnt[channum] >= 2) {
                    dxinfox[channum].state = ST_GOODBYE;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/meetme-invalid2.pcm");

                    if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                        disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (0);
                }

                dxinfox[channum].state = ST_FAKECONF_ERR;
                sprintf(dxinfox[ channum ].msg_name, "sounds/meetme-invalid1.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);

            }

        case ST_ISDNTEST_ENDCAUSE:
            ownies[channum] = strlen(dxinfox[ channum ].digbuf.dg_value);

            switch (ownies[channum]) {
                case 3:
                    if ((dxinfox[channum].digbuf.dg_value[0] > 0x31) || (dxinfox[channum].digbuf.dg_value[1] > 0x32)) {

                        dx_clrdigbuf(chdev);

                        if (errcnt[channum] >= 3) {
                            dxinfox[ channum ].state = ST_GOODBYE;

                            if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                                disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                            }

                            return (0);
                        }

                        dxinfox[channum].state = ST_ISDNERR2;
                        errcnt[channum]++;

                        if (play(channum, invalidfd, 0, 0, 0) == -1) {
                            disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));

                            if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                                disp_msg("Holy shit! isdn_drop() failed! D:");
                                dxinfox[channum].state = ST_GOODBYE;
                                play(channum, errorfd, 0, 0, 1);
                            }

                            return (-1);
                        }

                        return (0);
                    }

                    errcnt[channum] = 0;
                    causecode[channum] = (((dxinfox[channum].digbuf.dg_value[0] - 0x30) * 100) + ((dxinfox[channum].digbuf.dg_value[1] - 0x30) * 10) + (dxinfox[channum].digbuf.dg_value[2] - 0x30));

                    dxinfox[channum].state = ST_ISDNTEST_ENDCAUSE2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/thanks.pcm");

                    if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                        disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (0);

                case 2:
                    if (dxinfox[channum].digbuf.dg_value[1] > 0x39) {

                        dx_clrdigbuf(chdev);

                        if (errcnt[channum] >= 3) {
                            dxinfox[ channum ].state = ST_GOODBYE;

                            if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                                disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                            }

                            return (0);
                        }

                        dxinfox[channum].state = ST_ISDNERR2;
                        errcnt[channum]++;

                        if (play(channum, invalidfd, 0, 0, 0) == -1) {
                            disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));

                            if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                                disp_msg("Holy shit! isdn_drop() failed! D:");
                                dxinfox[channum].state = ST_GOODBYE;
                                play(channum, errorfd, 0, 0, 1);
                            }

                            return (-1);
                        }

                        return (0);
                    }

                    errcnt[channum] = 0;
                    causecode[channum] = (((dxinfox[channum].digbuf.dg_value[0] - 0x30) * 10) + (dxinfox[channum].digbuf.dg_value[1] - 0x30));
                    dxinfox[channum].state = ST_ISDNTEST_ENDCAUSE2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/thanks.pcm");

                    if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                        disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (0);


                case 1:
                    if (dxinfox[channum].digbuf.dg_value[0] > 0x39) {

                        dx_clrdigbuf(chdev);

                        if (errcnt[channum] >= 3) {
                            dxinfox[ channum ].state = ST_GOODBYE;

                            if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                                disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                            }

                            return (0);
                        }

                        dxinfox[channum].state = ST_ISDNERR2;
                        errcnt[channum]++;

                        if (play(channum, invalidfd, 0, 0, 0) == -1) {
                            disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));

                            if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                                disp_msg("Holy shit! isdn_drop() failed! D:");
                                dxinfox[channum].state = ST_GOODBYE;
                                play(channum, errorfd, 0, 0, 1);
                            }

                            return (-1);
                        }

                        return (0);
                    }

                    errcnt[channum] = 0;
                    causecode[channum] = (dxinfox[channum].digbuf.dg_value[0] - 0x30);
                    dxinfox[channum].state = ST_ISDNTEST_ENDCAUSE2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/thanks.pcm");

                    if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                        disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (0);

                default:
                    dx_clrdigbuf(chdev);

                    if (errcnt[channum] >= 3) {
                        dxinfox[ channum ].state = ST_GOODBYE;

                        if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                            disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                        }

                        return (0);
                    }

                    dxinfox[channum].state = ST_ISDNERR2;
                    errcnt[channum]++;

                    if (play(channum, invalidfd, 0, 0, 0) == -1) {
                        disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));

                        if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                            disp_msg("Holy shit! isdn_drop() failed! D:");
                            dxinfox[channum].state = ST_GOODBYE;
                            play(channum, errorfd, 0, 0, 1);
                        }

                        return (-1);
                    }
            }

            return (0);


        case ST_ISDNTEST:

            // The first two functions are commented out for the moment. They're in the works...


            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                // Read back q.931 fields
                sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/callednumberis.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                    if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                        disp_msg("Holy shit! isdn_drop() failed! D:");
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }

                    return (-1);
                }

                dxinfox[ channum ].state = ST_ISDNTEST_CPNDREAD;
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);

                return (0);

            }



            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                // Call origination test function
                playtone(channum, 350, 0);
                return (0);
            }


            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Access normal IVR functions
                errcnt[channum] = 0;

                if (dx_clrdigbuf(chdev) == 1) {
                    disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                playtone(channum, 400, 0);
                dxinfox[ channum ].state = ST_DIGPROC;
                return (0);
            }

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // End call with a specific cause code
                errcnt[channum] = 0;
                dxinfox[channum].state = ST_ISDNTEST_ENDCAUSE;

                if (dx_clrdigbuf(chdev) == 1) {
                    disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                sprintf(dxinfox[ channum ].msg_name, "sounds/isdntest/isdn_entercause.pcm");

                if ((dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY)) == -1) {
                    disp_msgf("Failure playing %s", dxinfox[ channum ].msg_name);

                    if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                        disp_msg("Holy shit! isdn_drop() failed! D:");
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }

                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);


                return (0);
            }

            if (strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 9; // This is stupid, I know, but we're not using the error counter for anything else right now

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                // Call origination test function
                playtone(channum, 350, 0);
                return (0);
            }

            else {
                // They dun goofed. Let them know they're stupid.
                dx_clrdigbuf(chdev);

                if (errcnt[channum] >= 3) {
                    dxinfox[ channum ].state = ST_GOODBYE;

                    if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                        disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                    }

                    return (0);
                }

                dxinfox[channum].state = ST_ISDNERR;
                errcnt[channum]++;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));

                    if (isdn_drop(channum, NETWORK_OUT_OF_ORDER) == -1) {
                        disp_msg("Holy shit! isdn_drop() failed! D:");
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }

                    return (-1);
                }
            }

            // We shouldn't actually hit this return statement ever unless either we or the caller fucks up
            return (0);

        case ST_CRUDEDIAL:
            disp_msg("Crude wardialer dighdlr active");
            errcnt[ channum ] = 0;

            if (strlen(dxinfox[ channum ].digbuf.dg_value) < 10) {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                playtone(channum, 480, 0);
                return (0);
            }

            else {
                // sprintf( filetmp[channum], "T9,9,%s", dxinfox[ channum ].digbuf.dg_value );
                sprintf(filetmp[channum], "T%s", dxinfox[ channum ].digbuf.dg_value);
                disp_msgf("Dialing %s...", filetmp[channum]);

                if (dx_dial(dxinfox[ channum ].chdev, filetmp[channum], NULL, EV_ASYNC) != 0) {
                    disp_msgf("Oh, shit! Dialout with string %s failed!", filetmp[channum]);
                    return (-1);
                }

            }

            return (0);

        case ST_SASTROLL:

            if (strcmp("E", dxinfox[ channum ].digbuf.dg_value) == 0) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/saso_hello.wav");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (dxinfox[ channum ].msg_fd == -1) {
                    disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                    return (-1);
                }

                playwav(channum, dxinfox[ channum ].msg_fd);
            }

            else {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

                if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                    disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

            }

            return (0);

        case ST_VMAIL1:

            if (strcmp("*", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_VMAILPASS;
                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_enterpass.pcm");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }

            if (strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Move back to the extension selection thing
                if (dx_clrdigbuf(chdev) == 1) {
                    disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(chdev));
                    disp_err(channum, chdev, dxinfox[ channum ].state);
                }

                playtone(channum, 400, 0);
                dxinfox[ channum ].state = ST_DIGPROC;
                return (0);

            }

            // Aaaaand, here's our catch-all

            else {

                dx_clrdigbuf(chdev);

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

                return (0);

            }

        case ST_VMAILPASS:

            if (strlen(passcode[channum]) != 6) {

                dxinfox[ channum ].state = ST_GOODBYE;

                if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

                return (0);
            }

            if (strcmp(passcode[ channum ], dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                ownies[ channum ] = 1;

                // Passcode entered correctly

                if (vmattrib[ channum ] & 0x01) {
                    // This voicemail hasn't been set up. Let's do that.
                    dxinfox[ channum ].state = ST_VMAILSETUP1;
                    sprintf(filetmp2[channum], "%s/old", filetmp[channum]);

                    if (stat(filetmp2[channum], &sts) == -1) {
                        mkdir(filetmp2[channum], 0700);    // Does the old directory exist? Let's create it now if not.
                    }

                    sprintf(dxinfox[channum].msg_name, "sounds/vmail/vmb_intro1.pcm");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                        disp_msg("Uh, is the voicemail setup message working? It's not working here");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }

                    return (0);
                }

                //Voicemail retreival happens here
                sprintf(filetmp2[channum], "%s/new", filetmp[channum]);
                errcnt[ channum ] = 1;
                anncnum[ channum ] = 0;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[channum], anncnum[channum]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                newmsg[channum] = anncnum[channum];
                anncnum[channum] = 0;
                ownies[ channum ] = 110;

                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/vmb_youhave.pcm");
                multiplay[channum][0] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.pcm", newmsg[channum]);
                multiplay[channum][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/newmsg.pcm");
                multiplay[channum][2] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/and.pcm");
                multiplay[channum][3] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                sprintf(filetmp2[channum], "%s/old", filetmp[channum]);
                errcnt[ channum ] = 1;
                anncnum[ channum ] = 0;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp2[channum], anncnum[channum]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                oldmsg[channum] = anncnum[channum];
                anncnum[channum] = 0;
                sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.pcm", oldmsg[channum]);

                multiplay[channum][4] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                sprintf(dxinfox[ channum ].msg_name, "sounds/vmail/savedmsg.pcm");
                multiplay[channum][5] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                dxinfox[channum].state = ST_VMAILMENU;

                if ((errcode[channum] = playmulti(channum, 6, 128, multiplay[channum])) == -1) {
                    disp_msg("VMAILRNEW2 function passed bad data to the playmulti function.");
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (0);
            }

            else {
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                errcnt[channum]++;

                if (errcnt[channum] > 3) {
                    passcode[channum][0] = '\0';
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (0);
                }

                dxinfox[channum].state = ST_VMAILPASS1;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

            }

            return (0);

        case ST_VMAIL3:
        case ST_EMREC2:
        case ST_DCBBSREC2:
        case ST_TC24BBSREC3:
            // TO DO: The recording code really should be overhauled >.<
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;

                // At the very least, this was added to keep a chain of if/else statements
                // from being executed until more serious work can be done.
                switch(dxinfox[channum].state) {
                    case ST_VMAIL3:
                        dxinfox[ channum ].msg_fd = open(filetmp2[channum], O_RDONLY);
                        dxinfox[ channum ].state = ST_VMAIL4;
                        break;
                    case ST_EMREC2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/temp/%d.pcm", msgnum[channum]);
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        dxinfox[ channum ].state = ST_EMREC3;
                        break;
                    case ST_DCBBSREC2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/temp/%d.pcm", msgnum[channum]);
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        dxinfox[ channum ].state = ST_DCBBSREC3;
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/temp/%d.pcm", msgnum[channum]);
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        dxinfox[ channum ].state = ST_TC24BBSREC4;
                }

                if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                    disp_msg("...shit, the BBS review message function isn't working");
                }

                return (errcode[channum]);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;

                if (dxinfox[ channum ].state == ST_EMREC2) {
                    dxinfox[ channum ].state = ST_EMREC1;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/beginmsg.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording is broke");
                    }
                } else if (dxinfox[ channum ].state == ST_VMAIL3) {
                    dxinfox[ channum ].state = ST_VMAIL2;
                    dxinfox[ channum ].msg_fd = open("sounds/vmail/beginmsg.pcm", O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                        file_error(channum, "sounds/vmail/beginmsg.pcm");
                    }
                } else if (dxinfox[ channum ].state == ST_DCBBSREC2) {
                    dxinfox[ channum ].state = ST_DCBBSREC;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/beginmsg.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording is broke");
                    }
                } else {
                    dxinfox[ channum ].state = ST_TC24BBSREC2;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/beginmsg.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS begin message recording recording is broke");
                    }
                }

                return (errcode[channum]);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;

                if (dxinfox[ channum ].state == ST_EMREC2) {
                    sprintf(filetmp[channum], "sounds/emtanon/temp/%d.pcm", msgnum[channum]);

                    if (remove(filetmp[ channum ]) == -1) {
                        disp_msgf("Uhh, something's wrong. Emtanon deletion failed.");
                        return (-1);
                    }

                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/deleted.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    dxinfox[ channum ].state = ST_EMPLAY3; // This *should* take us back to the main menu. If it doesn't, well, fix it or something.

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS deleted recording is broke");
                        file_error(channum, "sounds/emtanon/deleted.vox");
                        return -1;
                    }
                } else if (dxinfox[ channum ].state == ST_DCBBSREC2) {
                    sprintf(filetmp[channum], "sounds/tc24/dcbbs/temp/%d.pcm", msgnum[channum]);

                    if (remove(filetmp[ channum ]) == -1) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 0);
                        disp_msgf("Uhh, something's wrong. DC BBS deletion failed.");
                        return (-1);
                    }

                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/deleted.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    dxinfox[ channum ].state = ST_TC24MENU2; // This *should* take us back to the main menu. If it doesn't, well, fix it or something.

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS deleted recording is broke");
                        file_error(channum, "sounds/emtanon/deleted.vox");
                        return -1;
                    }
                } else if (dxinfox[ channum ].state == ST_TC24BBSREC3) {
                    sprintf(filetmp[channum], "sounds/tc24/bbs/temp/%d.pcm", msgnum[channum]);

                    if (remove(filetmp[ channum ]) == -1) {
                        disp_msgf("Uhh, something's wrong. TC24 BBS deletion failed.");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 0);
                        return (-1);
                    }

                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/deleted.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    dxinfox[ channum ].state = ST_TC24MENU2; // This *should* take us back to the main menu. If it doesn't, well, fix it or something.

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS deleted recording is broke");
                        file_error(channum, "sounds/emtanon/deleted.vox");
                        return -1;
                    }
                }


                else {
                    if (remove(filetmp2[ channum ]) == -1) {
                        disp_msgf("Uhh, something's wrong. Voicemail deletion failed.");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play( channum, errorfd, 0, 0, 0);
                        return (-1);
                    }

                    dxinfox[ channum ].state = ST_GOODBYE;

                    if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                        disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                        dxinfox[channum].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 0);
                    }
                }

                return (errcode[channum]);
            }

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (dxinfox[ channum ].state == ST_EMREC2) {
                    sprintf(filetmp[channum], "sounds/emtanon/temp/%d.pcm", msgnum[channum]);
                } else if (dxinfox[ channum ].state == ST_DCBBSREC2) {
                    sprintf(filetmp[channum], "sounds/tc24/dcbbs/temp/%d.pcm", msgnum[channum]);
                } else if (dxinfox[ channum ].state == ST_TC24BBSREC3) {
                    sprintf(filetmp[channum], "sounds/tc24/bbs/temp/%d.pcm", msgnum[channum]);
                }

                anncnum[ channum ] = 0;
                errcnt[ channum ] = 1;
                ownies[channum] = 0;

                if (dxinfox[ channum ].state == ST_EMREC2)
                    while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);

                            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                                errcnt[ channum ] = 0;
                            } else {
                                anncnum[ channum ]++;
                        }
                    } else if (dxinfox[ channum ].state == ST_DCBBSREC2)
                        while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[ channum ]);

                            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                                errcnt[ channum ] = 0;
                            } else {
                                anncnum[ channum ]++;
                        }
                    } else if (dxinfox[ channum ].state == ST_TC24BBSREC3)
                        while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[ channum ]);

                            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                                errcnt[ channum ] = 0;
                            } else {
                                anncnum[ channum ]++;
                        }
                    } else {
                        // For voicemail
                        while (errcnt[ channum ] == 1) {
                            sprintf(dxinfox[ channum ].msg_name, "%s/new/%d.pcm", filetmp[channum], anncnum[ channum ]);

                            if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                                errcnt[ channum ] = 0;
                            } else {
                                anncnum[ channum ]++;
                            }
                        }

                        sprintf(filetmp3[channum], "%s/new/%d.atr", filetmp[channum], anncnum[channum]);
                        resumefile[channum] = fopen(filetmp3[channum], "w+");
                        sprintf(filetmp[channum], "%s", filetmp2[channum]);  // This is sloppy. Sloppy, sloppy, sloppy. Sorry.
                    }

                if (rename(filetmp[ channum ], dxinfox[ channum ].msg_name) == 0) {
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 0;

                    if (dxinfox[ channum ].state == ST_EMREC2) {
                        dxinfox[ channum ].state = ST_EMPLAY3; // This will take us back to the main menu.
                        dxinfox[ channum ].msg_fd = open("sounds/emtanon/message_sent.vox", O_RDONLY);

                        if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                            disp_msg("...shit, the BBS message sent recording is broke");
                            file_error( channum, "sounds/emtanon/message_sent.vox" );
                            return (errcode[channum]);
                        }
                    } else if ((dxinfox[ channum ].state == ST_DCBBSREC2) || (dxinfox[ channum ].state == ST_TC24BBSREC3)) {
                        disp_msgf("File saved as %s", dxinfox[ channum ].msg_name);
                        dxinfox[ channum ].state = ST_TC24MENU2; // This will take us back to the main menu.
                        dxinfox[ channum ].msg_fd = open("sounds/emtanon/message_sent.vox", O_RDONLY);

                        if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                            disp_msg("...shit, the BBS message sent recording is broke");
                            file_error( channum, "sounds/emtanon/message_sent.vox" );
                            return (errcode[channum]);
                        }
                    } else {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        vmtimeoutput(channum);

                        if (resumefile[channum] != NULL) {
                            if (strlen(isdninfo[channum].cpn) > 0 ) fprintf(resumefile[channum], "%c %c %s", time1[channum], time2[channum], isdninfo[channum].cpn);
                            else fprintf(resumefile[channum], "%c %c e", time1[channum], time2[channum] );
                            fclose(resumefile[channum]);
                        }

                        dxinfox[ channum ].msg_fd = open("sounds/vmail/messagesent.pcm", O_RDONLY);

                        if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                            disp_msg("...shit, the VMS message sent recording is broke");
                            file_error( channum, "sounds/vmail/messagesent.pcm" );
                            return errcode[channum];
                        }
                    }

                }

                return 0;
            }

            else {
                // Some idiot couldn't be bothered to enter DTMF correctly. Some people are so stupid.
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ]++;

                switch(dxinfox[ channum ].state) {
                    case ST_EMREC2:
                        dxinfox[ channum ].state = ST_EMREC3;
                        break;
                    case ST_DCBBSREC2:
                        dxinfox[ channum ].state = ST_DCBBSREC3;
                        break;
                    case ST_TC24BBSREC3:
                        dxinfox[ channum ].state = ST_TC24BBSREC4;
                        break;
                    default:
                        dxinfox[ channum ].state = ST_VMAIL5;
                }

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

                errcode[channum] = 0;
            }

            return (errcode[channum]);

        case ST_EMPLAY1:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Play the BBS content
                errcnt[channum] = 0;
                // Remember to make the playwav function inter-operate correctly.
                // We'll have to do this differently than with raw PCM, since it
                // has to read the header every time playback is invoked.
                sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/0.pcm");

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    // This nifty function makes sure the program doesn't crash if it can't find any content
                    dxinfox[ channum ].state = ST_EMPLAY3;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/nomessages.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS failover recording isn't working");
                        file_error(channum, dxinfox[ channum ].msg_name);
                    }

                    return (errcode[channum]);
                }

                dxinfox[ channum ].state = ST_EMPLAY2;

                // The file limit checking code is sloppily copied and pasted in. We should make sure it's
                // fit for duty.

                anncnum[ channum ] = 1; // Start at 1; we already checked zero.
                errcnt[ channum ] = 1;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                maxannc[ channum ] = (anncnum[ channum ]);
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 1;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]--;
                    }
                }


                minannc[ channum ] = (anncnum[ channum ]);
                disp_msgf("maxannc is %d.", maxannc[ channum ]);
                errcnt[ channum ] = 0; // Mkay, we're all done here. Reset the error counter.

                anncnum[ channum ] = 0;
                sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                playoffset[ channum ] = 0;

                if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                    disp_msg("Emtanon initial player function is broke.");
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (errcode[channum]);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {     // Record a message
                msgnum[channum] = 0;
                errcnt[channum] = 0; // Let's borrow the error counter for this. It needs to be reset anyway.

                // Clear all that crap associated with the easter eggs

                if (termmask[channum] & 1) {
                //if (ownies[channum] == 100) {
                    disp_msg("Removing Chucktone");

                    if (dx_distone(chdev, TID_1, DM_TONEON) == -1) {
                        disp_msg("Couldn't remove Chucktone!");
                    }

                    //dx_deltones(chdev);
                    termmask[ channum ] ^= 1;
                    ownies[channum] = 0; // 200 just existed so the software won't react, but there's still a teardown routine. That's unnecessary with termmask.
                }

                while ((errcnt[channum] == 0) && (msgnum[channum] != 255)) {
                    msgnum[channum]++;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/temp/%d.pcm", msgnum[channum]);
                    errcnt[channum] = stat(dxinfox[ channum ].msg_name, &sts);

                }

                errcnt[channum] = 0;

                if (msgnum[channum] == 255) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 0);
                    return -1;    // Temporary file queue is full. Something isn't working right. Let's stop.
                }

                dxinfox[ channum ].state = ST_EMREC1;
                dxinfox[ channum ].msg_fd = open("sounds/emtanon/beginmsg.vox", O_RDONLY);

                if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                    disp_msg("...shit, the BBS begin message recording recording isn't working");
                    file_error( channum, "sounds/emtanon/beginmsg.vox");
                }

                return (errcode[channum]);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Message counter function
                msgnum[channum] = 0;
                errcnt[channum] = 1;

                // Clear all that crap associated with the easter eggs

                if (termmask[ channum ] & 1) {
                //if (ownies[channum] == 100) {
                    disp_msg("Removing Chucktone");

                    if (dx_distone(chdev, TID_1, DM_TONEON) == -1) {
                        disp_msg("Error removing Chucktone!");
                    }

                    //dx_deltones(chdev);
                    termmask[ channum ] ^= 1;
                    ownies[channum] = 0;
                }

                anncnum[ channum ] = 0; // Remember to reset this variable before counting! Other parts of the software use it.

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                // Done counting? Store the result in a variable, reset the other shit back to normal.

                maxannc[ channum ] = (anncnum[ channum ]);
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 0;

                dxinfox[ channum ].state = ST_EMPLAY3;
                sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/thereare2.vox");
                multiplay[channum][0] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (maxannc[channum] < 100) { // Fix introduced for message numbers higher than 100

                    sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.vox", maxannc[ channum ]);
                    multiplay[channum][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/messages.vox");
                    multiplay[channum][2] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = playmulti(channum, 3, 138, multiplay[channum])) == -1) {
                        disp_msg("The message counter is missing a file.");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }
                }

                else {

                    sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d00.vox", (maxannc[ channum ] / 100));
                    multiplay[channum][1] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/%d.vox", (maxannc[ channum ] % 100));
                    multiplay[channum][2] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    sprintf(dxinfox[ channum ].msg_name, "sounds/msgcount/messages.vox");
                    multiplay[channum][3] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = playmulti(channum, 4, 138, multiplay[channum])) == -1) {
                        disp_msg("The message counter is missing a file.");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, errorfd, 0, 0, 1);
                    }


                }

                return (errcode[channum]);
            }

            /*
            if (( strcmp( "4", dxinfox[ channum ].digbuf.dg_value ) == 0 ) && (ownies[channum] == 8)) {
                ownies[ channum ] = 0; // Make sure we reset this, or else the readback thing won't work; it uses this to count which position of the string to play
                playtone( channum, 350, 430 );
                return(0);

            }
            */

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Play the BBS content
                errcnt[channum] = 0;
                // Remember to make the playwav function inter-operate correctly.
                // We'll have to do this differently than with raw PCM, since it
                // has to read the header every time playback is invoked.
                sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/0.pcm");

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    // This nifty function makes sure the program doesn't crash if it can't find any content
                    dxinfox[ channum ].state = ST_EMPLAY3;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/nomessages.vox");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 0, 0, 0)) == -1) {
                        disp_msg("...shit, the BBS failover recording isn't working");
                        file_error(channum, dxinfox[ channum ].msg_name);
                    }

                    return (errcode[channum]);
                }

                dxinfox[ channum ].state = ST_EMPLAY2;

                // The file limit checking code is sloppily copied and pasted in. We should make sure it's
                // fit for duty.

                anncnum[ channum ] = 1; // Start at 1; we already checked zero.
                errcnt[ channum ] = 1;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                maxannc[ channum ] = (anncnum[ channum ]);
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 1;

                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]--;
                    }
                }


                minannc[ channum ] = (anncnum[ channum ]);
                disp_msgf("maxannc is %d.", maxannc[ channum ]);
                errcnt[ channum ] = 0; // Mkay, we're all done here. Reset the error counter.
                playoffset[ channum ] = 0;

                anncnum[ channum ] = (maxannc[ channum ] - 1);
                sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[ channum ]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if ((errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)) == -1) {
                    disp_msg("Emtanon initial player function is broke.");
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                }

                return (errcode[channum]);
            }
    #ifndef ALTOPTS
            if (strcmp("9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ] = 0;

                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // Since we don't need the digit buffer anymore, let's clear it; other things might need it.
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                // connchan[ channum ] = 1;

                connchan[ channum ] = idle_trunkhunt( channum, 1, 23, false );
                if (connchan[ channum ] == -1) return -1;

                // Trunkhunt replacement

                disp_msgf("Dest. channel is %d", connchan[channum]);

                connchan[connchan[channum]] = channum;

                if (altsig & 4) {
                    if (!fprintf(calllog, "%s;Random Toll-free Outdial;Outgoing CPN %s;In channel %d;Out channel %d\n", timeoutput(channum), dm3board ? config.defaultcpn : config.origtestcpn, channum, connchan[channum])) {
                        disp_msg("Failed to log");
                    }

                    fflush(calllog);
                }

                //set_cpname(connchan[channum], "Voice BBS");
                //disp_msg("Attempting to make call...");
                srandom(time(NULL));
                char dest[14];
                dest[0] = 0x37;
                dest[1] = 0x38;
                dest[2] = 0x31;
                dest[3] = 0x38;
                dest[4] = 0x30;
                dest[5] = 0x30;
                dest[6] = (random_at_most(8) + 0x32);
                dest[7] = (random_at_most(10) + 0x30);
                dest[8] = (random_at_most(10) + 0x30);
                dest[9] = (random_at_most(10) + 0x30);
                dest[10] = (random_at_most(10) + 0x30);
                dest[11] = (random_at_most(10) + 0x30);
                dest[12] = (random_at_most(10) + 0x30);
                dest[13] = 0x00;
                makecall(connchan[channum], dest, "12525350002", FALSE);

                dxinfox[ channum ].state = ST_ROUTED;
                return (0);
            }
        #else
            if (strcmp("9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ] = 0;

                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // Since we don't need the digit buffer anymore, let's clear it; other things might need it.
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                // connchan[ channum ] = 1;

                connchan[ channum ] = idle_trunkhunt( channum, 1, 23, false );
                if (connchan[ channum ] == -1) return -1;

                // Trunkhunt replacement

                connchan[connchan[channum]] = channum;

                if (altsig & 4) {
                    if (!fprintf(calllog, "%s;Voice BBS Fax Outdial;In channel %d;Out channel %d\n", timeoutput(channum), channum, connchan[channum])) {
                        disp_msg("Failed to log");
                    }

                    fflush(calllog);
                }


                makecall(connchan[channum], "9522116", dm3board ? config.defaultcpn : config.origtestcpn, FALSE);

                dxinfox[ channum ].state = ST_ROUTED;
                return (0);
            }

            if (strcmp("0", dxinfox[ channum ].digbuf.dg_value) == 0) {
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ] = 0;

                if (dx_clrdigbuf(dxinfox[channum].chdev) == -1) {     // Since we don't need the digit buffer anymore, let's clear it; other things might need it.
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(dxinfox[channum].chdev));
                }

                // connchan[ channum ] = 1;

                connchan[ channum ] = idle_trunkhunt( channum, 1, 23, false );
                if (connchan[ channum ] == -1) return -1;

                // Trunkhunt replacement

                connchan[connchan[channum]] = channum;

                if (altsig & 4) {
                    if (!fprintf(calllog, "%s;Voice BBS Touchtone Game Outdial;In channel %d;Out channel %d\n", timeoutput(channum), channum, connchan[channum])) {
                        disp_msg("Failed to log");
                    }

                    fflush(calllog);
                }


                makecall(connchan[channum], "9522113", dm3board ? config.defaultcpn : config.origtestcpn, FALSE);

                dxinfox[ channum ].state = ST_ROUTED;
                return (0);
            }

        #endif

        #ifndef ALTOPTS
            if (strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) {
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ] = 0;
                return conf_init(channum, 0, 0);
            }
        #else
        // On some systems, the outgoing call goes to a Mitel PBX. For... science.
            if (strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) {
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ] = 0;
                connchan[ channum ] = idle_trunkhunt( channum, 1, 23, false );
                if (connchan[ channum ] == -1) return -1;
                connchan[connchan[channum]] = channum;
                if (altsig & 4) {
                    if (!fprintf(calllog, "%s;Voice BBS Mitel Outdial;Outgoing CPN %s;In channel %d;Out channel %d\n", timeoutput(channum), dm3board ? config.defaultcpn : config.origtestcpn, channum, connchan[channum])) {
                        disp_msg("Failed to log");
                    }

                    fflush(calllog);
                }
                makecall(connchan[channum], "725555", dm3board ? config.defaultcpn : config.origtestcpn, FALSE);
                dxinfox[ channum ].state = ST_ROUTED;
                return 0;
            }

        #endif


            if ((strcmp("6", dxinfox[ channum ].digbuf.dg_value) == 0) && (ownies[channum] == 100)) {

                dxinfox[ channum ].state = ST_EMPLAY3;

                dxinfox[ channum ].msg_fd = open("sounds/impression.pcm", O_RDONLY);

                if (dxinfox[ channum ].msg_fd == -1) {
                    disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                    return (-1);
                }

                if (errcode[channum] == 0) {
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                return (0);
            }


            if ((strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) && (ownies[channum] == 100)) {

                dxinfox[ channum ].state = ST_EMPLAY3;

                dxinfox[ channum ].msg_fd = open("sounds/impression2.pcm", O_RDONLY);

                if (dxinfox[ channum ].msg_fd == -1) {
                    disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                    return (-1);
                }

                if (errcode[channum] == 0) {
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                return (0);
            }

            if (strcmp("*", dxinfox[ channum ].digbuf.dg_value) == 0) {
                close(dxinfox[ channum ].msg_fd);
                errcnt[ channum ] = 0;
                dx_clrdigbuf(dxinfox[channum].chdev);
                dxinfox[ channum ].state = ST_TC24MENU;
                // Zero this out, just in case.
                memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                errcnt[ channum ] = 0;
                disp_status(channum, "Running Telechallenge IVR");
                dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    file_error( channum, "sounds/tc24/greeting.pcm" );
                    return -1;
                }
                return 0;
            }

            if ((strcmp("E", dxinfox[ channum ].digbuf.dg_value) == 0) && (ownies[channum] == 100)) {

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (dx_distone(chdev, TID_1, DM_TONEON) == -1) {     // Remove custom tone
                    disp_msg("Error removing Chucktone!");
                }

                dx_deltones(chdev);   // Remove all custom tones

                disp_status(channum, "Accessing Sound Player");
                ownies[ channum ] = 0;
                errcnt[ channum ] = 0;
                loggedin[ channum ] = 0;
                passwordfile[channum] = NULL;

                dxinfox[ channum ].state = ST_GETCAT;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            }


            else {

                dxinfox[ channum ].state = ST_EMPLAY3;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                errcnt[ channum ]++;

                if (errcnt[ channum ] >= 3) {
                    errcnt[ channum ] = 0;
                    dxinfox[ channum ].state = ST_GOODBYE;
                    dx_clrdigbuf(chdev);
                }

                return (0);

            }

        case ST_OUTDIAL2:

            if (strcmp("**4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (filetmp[channum][15] == '\0') {
                    // Ignore that shit
                    dxinfox[ channum ].state = ST_OUTDIAL2;

                    if (get_digits(channum, &(dxinfox[ channum ].digbuf), 3) == -1) {
                        disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                    }

                    return (0);
                }

                if (filetmp[channum][15] < 0x39) {

                    filetmp[channum][15]++;
                    sprintf(filetmp2[channum], "&,%s", filetmp[channum]);
                    disp_msgf("Dialing %s...", filetmp[channum]);

                    if (dx_dial(dxinfox[ channum ].chdev, filetmp2[channum], NULL, EV_ASYNC) != 0) {
                    disp_msgf("Oh, shit! Dialout with string %s failed!", filetmp[channum]);
                        return (-1);
                    }

                    return (0);

                }

                if (filetmp[channum][15] >= 0x39) {
                    filetmp[channum][15] = 0x30;

                    if (filetmp[channum][14] < 0x39) {
                        filetmp[channum][14]++;
                    }

                    if (filetmp[channum][14] >= 0x39) {
                        filetmp[channum][14] = 0x30;
                    }

                    sprintf(filetmp2[channum], "&,,%s", filetmp[channum]);
                    disp_msgf("Dialing %s...", filetmp[channum]);

                    if (dx_dial(dxinfox[ channum ].chdev, filetmp2[channum], NULL, EV_ASYNC) != 0) {
                        disp_msgf("Oh, shit! Dialout with string %s failed!", filetmp[channum]);
                        return (-1);
                    }

                    return (0);
                }
            }


            if (strcmp("**#", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[ channum ].state = ST_OUTDIAL3;

                //sprintf( filetmp[channum], "&" ); // make sure this is right!
                if (dx_dial(dxinfox[ channum ].chdev, "&", NULL, EV_ASYNC) == -1) {
                    disp_msg("Oh, shit! Return dial function failed");
                }

                return (0);
            }

            if (strcmp("**0", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[ channum ].state = ST_OUTDIAL3;

                //sprintf( filetmp[channum], "," ); // make sure this is right!
                if (dx_dial(dxinfox[ channum ].chdev, ",", NULL, EV_ASYNC) == -1) {
                    disp_msg("Oh, shit! Return dial function failed");
                }

                return (0);
            }

            if (strcmp("**9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[ channum ].state = ST_OUTDIALSB;
                sprintf(dxinfox[ channum ].msg_name, "sounds/canada.pcm");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msg("Recording playback failure with outdial thingie.");
                    return (-1);
                }

                return (0);

            }

            if (strcmp("**8", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[ channum ].state = ST_OUTDIALSB;
                sprintf(dxinfox[ channum ].msg_name, "sounds/right.pcm");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msg("Recording playback failure with outdial thingie.");
                    return (-1);
                }

                return (0);

            }


            else {
                dxinfox[ channum ].state = ST_OUTDIAL2;

                if (get_digits(channum, &(dxinfox[ channum ].digbuf), 3) == -1) {
                    disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                }

                return (0);
            }


        case ST_OUTDIAL:
            // Outdial code isn't complete. So, er, yeah.
            disp_msg("Outdial state active");

            if (strcmp("911", dxinfox[ channum ].digbuf.dg_value) == 0 || strlen(dxinfox[ channum ].digbuf.dg_value) < 2 || strlen(dxinfox[ channum ].digbuf.dg_value) == 8 || strlen(dxinfox[ channum ].digbuf.dg_value) == 6 || strcmp("9591122", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ]++;

                if (errcnt[channum] < 4) {
                    sprintf(dxinfox[ channum ].msg_name, "sounds/barbe_dialnum_notvalid.pcm");
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                        disp_msg("Recording playback failure with outdial thingie.");
                        return (-1);
                    }

                    return (0);
                    // This shouldn't happen! Put something here to block that shit.
                } else {
                    dxinfox[ channum ].state = ST_GOODBYE;

                    if (dx_clrdigbuf(chdev) == -1) {

                        disp_msgf("DTMF Buffer clear fail on %s", ATDV_NAMEP(chdev));
                        disp_err(channum, chdev, dxinfox[ channum ].state);
                    }

                    if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                        disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                    }

                    return (0);
                }


            }

            errcnt[ channum ] = 0;
            dxinfox[ channum ].state = ST_OUTDIAL2;

            if (strlen(dxinfox[ channum ].digbuf.dg_value) != 10) {
                sprintf(filetmp[channum], "&,T*67,%s&", dxinfox[ channum ].digbuf.dg_value);    // make sure this is right!
            } else {
                sprintf(filetmp[channum], "&,T*67,1%s&", dxinfox[ channum ].digbuf.dg_value);
            }

            disp_msgf("Dialing %s...", filetmp[channum]);

            if (dx_dial(dxinfox[ channum ].chdev, filetmp[channum], NULL, EV_ASYNC) != 0) {
                disp_msgf("Oh, shit! Dialout with string %s failed!", filetmp[channum]);
                return (-1);
            }

            return (0);


        case ST_RESUMEMARK:
            close(dxinfox[ channum ].msg_fd);

            if (strlen(dxinfox[ channum ].digbuf.dg_value) != 4) {
                dxinfox[ channum ].state = ST_RESUMEMARK2;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                return (0);
            }

            sprintf(filetmp2[ channum ], "resume/%s", dxinfox[ channum ].digbuf.dg_value);
            sprintf(bookmark[ channum ], "%s", dxinfox[ channum ].digbuf.dg_value);

            if ((resumefile[ channum ] = fopen(filetmp2[ channum ], "r")) != NULL) {
                fscanf(resumefile[ channum ], "%lu %s %hi", &playoffset[ channum ], filetmp[ channum ], &anncnum[ channum ]);
                fclose(resumefile[ channum ]);
            }

            if ((resumefile[ channum ] == NULL) || (strlen(dxinfox[ channum ].digbuf.dg_value) != 4)) {
                dxinfox[ channum ].state = ST_RESUMEMARK2;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_booknotfound.pcm", O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msg("The bookmark not found recording was not found. How ironic.");
                    file_error(channum, "sounds/ivr_booknotfound.pcm");
                    return (-1);
                }

                return (0);
            }

            dxinfox[ channum ].msg_fd = open("sounds/thanks.pcm", O_RDONLY);
            dxinfox[ channum ].state = ST_RESUMEMARK3;

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                file_error(channum, "sounds/thanks.pcm");
                return (-1);
            }

            return (0);

        case ST_MAKEMARK:
            close(dxinfox[ channum ].msg_fd);

            if (strlen(dxinfox[ channum ].digbuf.dg_value) != 4) {
                dxinfox[ channum ].state = ST_CATMENU;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                return (0);
            }

            sprintf(filetmp2[ channum ], "resume/%s", dxinfox[ channum ].digbuf.dg_value);
            sprintf(bookmark[ channum ], "%s", dxinfox[ channum ].digbuf.dg_value);
            resumefile[ channum ] = fopen(filetmp2[ channum ], "w+");
            // Emergency fix for Toorcamp
            if (resumefile[channum] == NULL) {
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return -1;
            }
            fprintf(resumefile[ channum ], "%lu %s %d", playoffset[ channum ], filetmp[ channum ], anncnum[ channum ]);
            fclose(resumefile[ channum ]);
            dxinfox[ channum ].msg_fd = open("sounds/ivr_bookset.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            dxinfox[ channum ].state = ST_CATMENU;
            return (errcode[channum]);

        case ST_MSGREC2:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Save the message
                sprintf(filetmp2[ channum ], "%s/%d.pcm", filetmp[ channum ], anncnum2[ channum ]);

                if (rename(filetmp3[ channum ], filetmp2[ channum ]) == 0) {
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_msgsaved.pcm", O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    dxinfox[ channum ].state = ST_CATMENU;
                    return (0);
                }

                else {
                    return (-1);
                }
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Listen to the message
                sprintf(dxinfox[ channum ].msg_name, "%s", filetmp3[ channum ]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_MSGREC3;
                return (0);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Delete the message

                if (remove(filetmp3[ channum ]) == -1) {
                    disp_msg("Uhh, something's wrong. Deletion failed.");
                }

                dxinfox[ channum ].msg_fd = open("sounds/ivr_msgdeleted2.pcm", O_RDONLY);
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                return (0);
            }

            else {
                dxinfox[ channum ].state = ST_MSGREC3;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                errcnt[ channum ]++;

                if (errcnt[ channum ] >= 3) {
                    errcnt[ channum ] = 0;
                    break;
                }

                return (0);
            }

        case ST_CATNOEXIST: // This is a workaround for the error function if a category doesn't exist
            dxinfox[ channum ].msg_fd = open("sounds/ivr_catnoexist.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            dxinfox[ channum ].state = ST_CATCREATE;
            return (errcode[channum]);

        case ST_CATREC3:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Save the message
                sprintf(filetmp2[ channum ], "%s/title.pcm", filetmp[ channum ]);

                if (rename(filetmp3[ channum ], filetmp2[ channum ]) == 0) {
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_msgsaved.pcm", O_RDONLY);
                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    dxinfox[ channum ].state = ST_CATMENU;
                    return (0);
                }

                return (-1);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Listen to the message
                sprintf(dxinfox[ channum ].msg_name, "%s", filetmp3[ channum ]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATREC2;
                return (0);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Delete the message

                if (remove(filetmp3[ channum ]) == -1) {
                    disp_msgf("Uhh, something's wrong. Deletion failed.");
                }

                dxinfox[ channum ].msg_fd = open("sounds/ivr_msgdeleted2.pcm", O_RDONLY);
                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                return (0);
            }

            else {
                dxinfox[ channum ].state = ST_CATREC2;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                errcnt[ channum ]++;

                if (errcnt[ channum ] >= 3) {
                    errcnt[ channum ] = 0;
                    break;
                }

                return (0);
            }

        case ST_ENTERPASS:

            if (strcmp(passcode[ channum ], dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Passcode entered correctly
                ownies[ channum ] = 0;
                loggedin[ channum ] = 1;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_loggedin.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                return (0);
            }

            else {
                ownies[ channum ] = 3; // Make sure the IVR doesn't close the invalid recording
                errcnt[ channum ]++;
                dxinfox[ channum ].state = ST_CATMENU;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                if (errcnt[ channum ] >= 3) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                }

                break;
            }

        case ST_CATMENU2:
            close(dxinfox[ channum ].msg_fd);

            if ((strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin [ channum ] == 1)) {
                // Record the category title
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dxinfox[ channum ].state = ST_CATREC;
                sprintf(filetmp3[ channum ], "%s/temp/", filetmp[ channum ]);

                if (stat(filetmp3[ channum ], &sts) == -1) {
                    mkdir(filetmp3[ channum ], 0644);
                }

                dxinfox[ channum ].msg_fd = open("sounds/ivr_startrec.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[channum]);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Playback
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                errcnt[ channum ] = 0;
                ownies[ channum ] = 0; // Initialize variables
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 1;
                playoffset[ channum ] = 0;

                // Determine total number of announcements
                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum[ channum ]++;
                    }
                }

                maxannc[ channum ] = (anncnum[ channum ]);
                minannc[ channum ] = 0; // By default here, all the announcements start at 0
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 0;
                sprintf(dxinfox[ channum ].msg_name, "%s/0.pcm", filetmp[ channum ]);

                if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                    dxinfox[ channum ].state = ST_CATMENU;
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_endofrecs.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);
                }

                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_GETCAT3;
                return (errcode[channum]);
            }

            if (((strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin[ channum ] == 1)) || ((strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) && (userposts[ channum ] == 1))) {
                // Record a message

                errcnt[ channum ] = 1;
                anncnum2[ channum ] = 0;

                // Determine total number of announcements
                while (errcnt[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum2[ channum ]);

                    if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                        errcnt[ channum ] = 0;
                    } else {
                        anncnum2[ channum ]++;
                    }
                }

                disp_msgf("Recording message number %d", anncnum2[ channum ]);
                errcnt[ channum ] = 0;
                sprintf(filetmp3[ channum ], "%s/temp/", filetmp[ channum ]);

                if (stat(filetmp3[ channum ], &sts) == -1) {
                    mkdir(filetmp3[ channum ], 0644);
                }

                // This code is likely to result in collisions with multiple users and should be fixed. Actually, I think the voicemail code implemented a fix for exactly this reason...
                sprintf(filetmp3[ channum ], "%s/temp/%d.pcm", filetmp[ channum ], anncnum2[ channum ]);

                if (loggedin[ channum ] == 1) {
                    sprintf(dxinfox[ channum ].digbuf.dg_value, "%d", anncnum2[ channum ]);
                    sprintf(readback[ channum ], "%d", anncnum2[ channum ]);
                    ownies[channum] = 0; //Bugfix? Remove if not necessary
                    dxinfox[ channum ].state = ST_MSGREAD;
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_recordingmessage.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);
                } else {
                    dxinfox[ channum ].state = ST_MSGREC;
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_startrec.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);
                }
            }

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Pick a new category
                loggedin[ channum ] = 0; // NO HAXX0RiNG!!
                dxinfox[ channum ].state = ST_GETCAT;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[channum]);
            }

            if ((strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin[ channum ] == 1)) {
                // Log out
                errcnt[ channum ] = 0;
                loggedin[ channum ] = 0;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_loggedout.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                return (0);
            }

            if ((strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin[ channum ] == 0)) {
                // Log in
                errcnt[ channum ] = 0;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_enterpass.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_ENTERPASS;
                return (0);
            }

            if ((strcmp("6", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin[ channum ] == 1) && (userposts[ channum ] == 0)) {
                // Let users record new messages for this category
                errcnt[ channum ] = 0;
                sprintf(filetmp2[ channum ], "%s/attrib", filetmp[ channum ]);
                passwordfile[ channum ] = fopen(filetmp2[ channum ], "w+");
                fprintf(passwordfile[ channum ], "%s 1", passcode[ channum ]);
                userposts[ channum ] = 1;
                fclose(passwordfile[ channum ]);
                dxinfox[ channum ].msg_fd = open("sounds/ivr_recenabled.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                return (0);
            }

            if ((strcmp("6", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin[ channum ] == 1) && (userposts[ channum ] == 1)) {
                // Don't let users record new messages for this category
                errcnt[ channum ] = 0;
                sprintf(filetmp2[ channum ], "%s/attrib", filetmp[ channum ]);
                passwordfile[ channum ] = fopen(filetmp2[ channum ], "w+");
                fprintf(passwordfile[ channum ], "%s 0", passcode[ channum ]);
                userposts[ channum ] = 1;
                fclose(passwordfile[ channum ]);
                dxinfox[ channum ].msg_fd = open("sounds/ivr_recdisabled.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                return (0);
            }

            if ((strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) && (loggedin[ channum ] == 1)) {
                // Delete a message. Why was this never implemented?
            }

            if (strcmp("0", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Create a bookmark
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_MAKEMARK;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_plsenterbook.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (0);
            } else {
                dxinfox[ channum ].state = ST_CATMENU;
                errcnt[ channum ]++;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                if (errcnt[ channum ] >= 3) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                }

                return (0);

            }

        case ST_PASSCREATE3:

            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_youreallset.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATMENU;
                loggedin[ channum ] = 1; // Log the user in
                userposts[ channum ] = 0; //By default, nobody can create new recordings unless they're logged in
                // Move caller to the main menu here, and mark them as logged in
                return (0);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                dxinfox[ channum ].state = ST_PASSCREATE;
                close(dxinfox[ channum ].msg_fd);

                if (get_digits(channum, &(dxinfox[ channum ].digbuf), 6) == -1) {
                    disp_msgf("Cannot get digits in passcreate state, channel %s", ATDV_NAMEP(chdev));
                    return (-1);
                }

                return (0);
            }

        case ST_PASSCREATE2:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                dxinfox[ channum ].msg_fd = open("sounds/ivr_setpass.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                errcode[channum] = (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0));
                dxinfox[ channum ].state = ST_PASSCREATE;
                return (0);
            } else {
                mkdir(filetmp[ channum ], 0644);
                sprintf(filetmp2[ channum ], "%s/attrib", filetmp[ channum ]);
                passwordfile[ channum ] = fopen(filetmp2[ channum ], "w+");
                errcode[channum] = (fprintf(passwordfile[ channum ], "%s 0", dxinfox[ channum ].digbuf.dg_value));
                sprintf(passcode[ channum ], "%s", dxinfox[ channum ].digbuf.dg_value);
                // The 0, by default, means users can't post to the category
                fclose(passwordfile[ channum ]);

                if (errcode[channum] != -1) {
                    errcode[channum] = 0; // Just in case there's a collision somewhere
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_passcodeset.pcm", O_RDONLY);
                    errcode[channum] = (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0));
                    sprintf(dxinfox[ channum ].digbuf.dg_value, "%s", readback[ channum ]);
                    ownies[ channum ] = 0;
                    dxinfox[ channum ].state = ST_PASSREADBACK;
                    return (0);
                }

                return (-1);
            }


        case ST_PASSCREATE:

            /* Test for short/shitty passcodes */
            if (strlen(dxinfox[ channum ].digbuf.dg_value) != 6) {
                //Passcode is too short
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                    // Add something to increment the error counter here
                    return (-1);
                }

                return (0);
            }

            // Checking for shitty passcodes. Is there any better way to do this?
            if ((strcmp("123456", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("654321", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("111111", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("222222", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("333333", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("444444", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("555555", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("666666", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("777777", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("888888", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("999999", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("000000", dxinfox[ channum ].digbuf.dg_value) == 0) ||
                    (strcmp("696969", dxinfox[ channum ].digbuf.dg_value) == 0)) {

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dxinfox[ channum ].msg_fd = open("sounds/ivr_shittypass.pcm", O_RDONLY);

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msg("Error playing shittypasscode message");
                    return (-1);
                }

                dxinfox[ channum ].state = ST_PASSCREATE2;
                return (0);
            }

            mkdir(filetmp[ channum ], 0644);
            sprintf(filetmp2[ channum ], "%s/attrib", filetmp[ channum ]);
            passwordfile[ channum ] = fopen(filetmp2[ channum ], "w+");
            disp_msgf("Pass is %s", dxinfox[ channum ].digbuf.dg_value);
            errcode[channum] = (fprintf(passwordfile[ channum ], "%s 0", dxinfox[ channum ].digbuf.dg_value));
            fclose(passwordfile[ channum ]);
            sprintf(passcode[ channum ], "%s", dxinfox[ channum ].digbuf.dg_value);

            if (errcode[channum] != -1) {
                errcode[channum] = 0; //To stop any sort of collisions
                dxinfox[ channum ].msg_fd = open("sounds/ivr_passcodeset.pcm", O_RDONLY);
                errcode[channum] = (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0));
                dxinfox[ channum ].state = ST_PASSREADBACK;
                ownies[ channum ] = 0;
                return (0);
            }

            return (errcode[channum]);

        case ST_CATCREATE:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_setpass.pcm", O_RDONLY);
                errcode[channum] = (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0));
                dxinfox[ channum ].state = ST_PASSCREATE;
                return (errcode[channum]);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_GETCAT;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[channum]);
            }

            else {
                errcnt[ channum ]++;

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                if (errcnt[ channum ] >= 3) {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    break;
                }

                dxinfox[ channum ].state = ST_CATNOEXIST;
                return (0);
            }

        case ST_CATRESUME:
            close(dxinfox[ channum ].msg_fd);

            // dx_adjsv( dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0 ); // Reset volume to normal
            if (strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) {
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                dxinfox[ channum ].state = ST_GETCAT3;
            } else {
                dxinfox[ channum ].state = ST_GETCAT;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            }

            return (errcode[channum]);

        case ST_GETCAT3:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                playoffset[ channum ] = 0;
                errcnt[channum] = 0;
                close(dxinfox[ channum ].msg_fd);
                anncnum[ channum ]--;
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                /* Decrease the announcement number and play it */
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    disp_msg("Accessing category sound player...");
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                else {
                    disp_msg("Category sound player error!");

                    /* Attempt to resolve the error. */
                    if (anncnum[ channum ] < minannc[ channum ]) {
                        anncnum[ channum ] = (minannc[ channum ]);
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }
                }

                return (errcode[channum]);
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                close(dxinfox[ channum ].msg_fd);

                if (ownies[channum] == 5) {
                    ownies[channum] = 6;
                }

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dxinfox[ channum ].state = ST_CATPAUSE;
                sprintf(filetmp2[channum], "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                sprintf(dxinfox[channum].msg_name, "sounds/paused.pcm");
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (dxinfox[ channum ].msg_fd == -1) {
                    // Uhh, this is bad. Let's hang up.
                    dxinfox[ channum ].state = ST_GOODBYE;
                    play(channum, goodbyefd, 0, 0, 0);
                    return (-1);
                } else {
                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                return (0);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                playoffset[ channum ] = 0;
                errcnt[channum] = 0;
                close(dxinfox[ channum ].msg_fd);
                anncnum[ channum ]++;
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                else {
                    disp_msg("Category sound player error!");

                    /* Attempt to resolve the error. */
                    if (anncnum[ channum ] >= maxannc[ channum ]) {
                        anncnum[ channum ] = (maxannc[ channum ] - 1);
                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }
                }

                return (errcode[channum]);
            }

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                // ..why was the file descriptor closed?
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (dxinfox[ channum ].msg_fd != -1) {
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ] - 56000);     // 8000 bytes per sec * 7 = 56,000

                    if (playoffset[ channum ] > (unsigned long) lseek(dxinfox[ channum ].msg_fd, 0, SEEK_END)) {
                        playoffset[ channum ] = 0;    // If the new offset is past the end of the file, reset it back to zero
                    }

                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                else {
                    disp_msgf("Dynamic sound player error! Offset is %lu", playoffset[ channum ]);
                }

                return (errcode[channum]);
            }

            if (strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                close(dxinfox[ channum ].msg_fd);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                playoffset[ channum ] = 0;
                loggedin[ channum ] = 0;
                dxinfox[ channum ].state = ST_GETCAT;
                dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[channum]);
            }

            if (strcmp("6", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (dxinfox[ channum ].msg_fd != -1) {
                    //playoffset[ channum ] = ATDX_TRCOUNT(chdev);   // 8000 bytes per sec * 7 = 56,000
                    playoffset[ channum ] = ( ATDX_TRCOUNT(chdev) + 56000 + playoffset[ channum ]);

                    if (playoffset[ channum ] > (unsigned long) lseek(dxinfox[ channum ].msg_fd, 0, SEEK_END)) {
                        playoffset[ channum ] = 0;
                        anncnum[ channum ]++;
                        dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                        if (anncnum[ channum ] >= maxannc[ channum ]) {
                            anncnum[ channum ] = (maxannc[ channum ] - 1);
                        }

                        sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                    }
                }

                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);

                if (errcode[channum] == -1) {
                    disp_msg("Category sound player error!");
                }

                return (errcode[channum]);
            }

            if (strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                sprintf(filetmp2[ channum ], "%s", dxinfox[ channum ].msg_name);
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    if (strcmp("sounds/ivr_backfwd.pcm", filetmp2[ channum ]) == 0) {
                        playoffset[ channum ] = 0;
                    } else {
                        playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);
                    }

                    errcode[channum] = dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_RELCURPOS, -2);
                    play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Category sound player error in volume adjust function");
                }

                return (errcode[channum]);
            }

            if (strcmp("8", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                sprintf(filetmp2[ channum ], "%s", dxinfox[ channum ].msg_name);
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    if (strcmp("sounds/ivr_backfwd.pcm", filetmp2[ channum ]) == 0) {
                        playoffset[ channum ] = 0;
                    } else {
                        playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);
                    }

                    errcode[channum] = dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                    play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Category sound player error in volume adjust function");
                }

                return (errcode[channum]);
            }

            if (strcmp("9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                errcnt[channum] = 0;
                sprintf(filetmp2[ channum ], "%s", dxinfox[ channum ].msg_name);
                sprintf(dxinfox[ channum ].msg_name, "%s/%d.pcm", filetmp[ channum ], anncnum[channum]);
                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    if (strcmp("sounds/ivr_backfwd.pcm", filetmp2[ channum ]) == 0) {
                        playoffset[ channum ] = 0;
                    } else {
                        playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);
                    }

                    errcode[channum] = dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_RELCURPOS, 2);
                    play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Category sound player error in volume adjust function");
                }

                return (errcode[channum]);
            }


            else {
                if (errcnt[ channum ] == 0) {
                    // Couldn't this just be replaced with playoffset[ channum ] += ATDX_TRCOUNT( chdev ); ? I hate going over my old code.
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);
                }

                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);
                errcnt[ channum ]++;

                if (errcnt[ channum ] == 3) {
                    dxinfox[ channum ].state = ST_INVALID;
                }

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                return (0);
            }

        case ST_GETCAT:

            if (dxinfox[ channum ].digbuf.dg_value[0] == '\0') {
                // Someone pressed nothing. Let's tell them off.
                close(dxinfox[ channum ].msg_fd);
                errcnt[channum]++;

                if (errcnt[channum] >= 3) {
                    dxinfox[ channum ].state = ST_GOODBYE;

                    if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                        disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                    }

                    errcnt[channum] = 0;
                    ownies[channum] = 0;
                    return (0);
                }

                dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Cannot play bookmark message on channel %s", ATDV_NAMEP(chdev));
                }

                ownies[ channum ] = 3;
                return (0);
            }

            // Bookmark mode!
            if ((dxinfox[ channum ].digbuf.dg_value[0] == '0') && (dxinfox[ channum ].digbuf.dg_value[1] == '\0')) {
                dxinfox[ channum ].msg_fd = open("sounds/ivr_plsenterbook.pcm", O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Cannot play bookmark message on channel %s", ATDV_NAMEP(chdev));
                }

                dxinfox[ channum ].state = ST_RESUMEMARK;
                return (0);
            }

            if ((dxinfox[ channum ].digbuf.dg_value[1] == '\0') || (dxinfox[ channum ].digbuf.dg_value[1] == '#')) {
                sprintf(filetmp[ channum ], "sounds/category/%c/", dxinfox[ channum ].digbuf.dg_value[0]);
            }

            if ((dxinfox[ channum ].digbuf.dg_value[2] == '\0') || (dxinfox[ channum ].digbuf.dg_value[2] == '#')) {
                sprintf(filetmp[ channum ], "sounds/category/%c%c/", dxinfox[channum].digbuf.dg_value[0], dxinfox[channum].digbuf.dg_value[1]);
            }

            if ((dxinfox[ channum ].digbuf.dg_value[3] == '\0') || (dxinfox[ channum ].digbuf.dg_value[3] == '#')) {
                sprintf(filetmp[channum], "sounds/category/%c%c%c/", dxinfox[channum].digbuf.dg_value[0], dxinfox[channum].digbuf.dg_value[1], dxinfox[channum].digbuf.dg_value[2]);
            }

            disp_msgf("Filename is %s", filetmp[ channum ]);
            /* If it's a valid number, see if the category already exists. */
            errcnt[ channum ] = 0;

            if (stat(filetmp[channum], &sts) == -1) {
                dxinfox[ channum ].msg_fd = open("sounds/ivr_catnoexist.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                dxinfox[ channum ].state = ST_CATCREATE;
                return (errcode[channum]);
            }

            sprintf(filetmp2[ channum ], "%s/attrib", filetmp[ channum ]);

            if ((passwordfile[ channum ] = fopen(filetmp2[ channum ], "r")) != NULL) {
                fscanf(passwordfile[ channum ], "%6s %d", passcode[ channum ], (char * ) &userposts[ channum ]);
                fclose(passwordfile[ channum ]);
            }
            // If there's no attributes file, lock the recording in question
            else {
                userposts[ channum ] = 0;
                sprintf(passcode[ channum ], "ssssss");
            }

            dxinfox[ channum ].state = ST_GETCAT2;
            dxinfox[ channum ].msg_fd = open("sounds/ivr_categorytitle.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (errcode[channum]);

        case ST_ANAC: /* This just reads back digits the machine receives as DTMF. All the magic is done by the switch. */

            if (strlen(dxinfox[ channum ].digbuf.dg_value) == 0) {
                sprintf(dxinfox[ channum ].msg_name, "sounds/anacerror.pcm");
            } else {
                sprintf(dxinfox[ channum ].msg_name, "sounds/digits1/%c.pcm", dxinfox[ channum ].digbuf.dg_value[ ownies[ channum ] ]);
                ownies[ channum ]++;
            }

            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

            if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                disp_msgf("Could not play back %s in ANAC function", dxinfox[ channum ].msg_name);
            }

            dxinfox[ channum ].state = ST_ANAC;
            break;

        case ST_WINKDIG:
            disp_msgf("Digits received were %s", dxinfox[ channum ].digbuf.dg_value);
            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);
            break;

        case ST_ENIGMAREC2:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                // Add an option for delivery here.
                dxinfox[ channum ].state = ST_GOODBYE;
                dx_stopch(chdev, EV_ASYNC);

                while (ATDX_STATE(chdev) != CS_IDLE);     // For some reason, the channel isn't idle immediately after digit collection, so this is needed

                if (play(channum, goodbyefd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Goodbye Message on channel %s", ATDV_NAMEP(chdev));
                }

                break;
            }

            if (strcmp("2", dxinfox[ channum ].digbuf.dg_value) == 0) {
                if (remove(filetmp[ channum ]) == -1) {
                    disp_msgf("Uhh, something's wrong. Deletion failed.");
                    dxinfox[ channum ].state = ST_ERROR;
                    break;
                }

                dx_stopch(chdev, EV_ASYNC);

                while (ATDX_STATE(chdev) != CS_IDLE);     // For some reason, the channel isn't idle immediately after digit collection, so this is needed

                dxinfox[ channum ].msg_fd = open("sounds/deleted.pcm", O_RDONLY);
                ownies[ channum ] = 0;
                dxinfox[ channum ].state = 8;

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Deletion recording is fuxx0red ( %s ))", ATDV_ERRMSGP(chdev));
                }

                break;
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                sprintf(dxinfox[ channum ].msg_name, "%s", filetmp[ channum ]);
                dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                dx_stopch(chdev, EV_ASYNC);

                while (ATDX_STATE(chdev) != CS_IDLE);     // For some reason, the channel isn't idle immediately after digit collection, so this is needed

                if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0) == -1) {
                    disp_msgf("Couldn't play back the enigmarec again");
                    ownies[ channum ] = 1; //Have the play function repeat the menu for us, since we can't go directly back here.
                }

                dxinfox[ channum ].state = ST_ENIGMAREC2;
                break; //We'll finish this up later.
            }

        case ST_TC24BBS2:
        case ST_DCBBS:
        case ST_TCTUTORIAL:
        case ST_EMPLAY2:
        case ST_DYNPLAY:
            if (strcmp("1", dxinfox[ channum ].digbuf.dg_value) == 0) {
                anncnum[ channum ]--;
                playoffset[ channum ] = 0;
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                /* Decrease the announcement number and play it */
                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    disp_msg("Accessing dynamic sound player...");
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Dynamic sound player error!");

                    /* Attempt to resolve the error. */
                    if (anncnum[ channum ] <= minannc[ channum ]) {
                        anncnum[ channum ] = (minannc[ channum ] + 1);

                    switch(dxinfox[ channum ].state) {
                        case ST_DYNPLAY:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                            break;
                        case ST_TCTUTORIAL:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                            break;
                        case ST_DCBBS:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                            break;
                        case ST_TC24BBS2:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                            break;
                        default:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                        }
                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }
                }

                return (errcode[channum]);
            }

            if (strcmp("3", dxinfox[ channum ].digbuf.dg_value) == 0) {
                anncnum[ channum ]++;
                playoffset[ channum ] = 0;
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                }

                else {
                    disp_msg("Dynamic sound player error!");

                    /* Attempt to resolve the error. */
                    if (anncnum[ channum ] >= maxannc[ channum ]) {
                        anncnum[ channum ] = (maxannc[ channum ] - 1);

                    switch(dxinfox[ channum ].state) {
                        case ST_DYNPLAY:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                            break;
                        case ST_TCTUTORIAL:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                            break;
                        case ST_DCBBS:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                            break;
                        case ST_TC24BBS2:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                            break;
                        default:
                            sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                        }

                        dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }
                }

                return (errcode[channum]);
            }

            if (strcmp("4", dxinfox[ channum ].digbuf.dg_value) == 0) {
                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ] - 56000);     // 8000 bytes per sec * 7 = 56,000

                    if (playoffset[ channum ] > (unsigned long) lseek(dxinfox[ channum ].msg_fd, 0, SEEK_END)) {
                        playoffset[ channum ] = 0;
                    }

                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msgf("Dynamic sound player error! Offset is %lu", playoffset[ channum ]);
                }

                return (errcode[channum]);
            }

            if ((strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) && (dxinfox[ channum ].state == ST_EMPLAY2)) {
                // Repeat the stuff from 2109 processing for a main menu return.
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_EMPLAY1;

                if(!(termmask[channum] & 1)) {
                //if (ownies[channum] != 100) {

                    /*
                    if (dx_blddt(TID_1, 1880, 15, 697, 15, TN_TRAILING) == -1) {
                        disp_msg("Shit we couldn't build the Chucktone!");
                    }
                    */

                    // TO DO: This error happens since dx_addtone() keeps getting invoked inappropriately; we should be adding the tone when we come into the voice BBS, and only enabling it here.

                    if (dx_enbtone(dxinfox[channum].chdev, TID_1, DM_TONEON) == -1) {
                        disp_msgf("Unable to enable Chucktone, error %s", ATDV_ERRMSGP(dxinfox[channum].chdev));
                    }

                    termmask[channum] |= 1;

                }

                // If the Chucktone doesn't work, just keep going.
                ownies[channum] = 100;
                dxinfox[ channum ].msg_fd = open("sounds/emtanon/greeting_new.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[channum]);
            }

            else if ((strcmp("5", dxinfox[ channum ].digbuf.dg_value) == 0) && ((dxinfox[ channum ].state == ST_TCTUTORIAL) || (dxinfox[ channum ].state == ST_DCBBS) || (dxinfox[ channum ].state == ST_TC24BBS2)) ) {
                anncnum[ channum ] = 0;
                errcnt[ channum ] = 0;
                dxinfox[ channum ].state = ST_TC24MENU;
                dxinfox[ channum ].msg_fd = open("sounds/tc24/greeting.pcm", O_RDONLY);
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                return (errcode[channum]);
            }

            // TO DO: Revise this code. Pretty please. It hurts to look at.
            if (strcmp("6", dxinfox[ channum ].digbuf.dg_value) == 0) {
                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + 56000 + playoffset[ channum ]);

                    if (playoffset[ channum ] > (unsigned long) lseek(dxinfox[ channum ].msg_fd, 0, SEEK_END)) {
                        playoffset[ channum ] = 0;
                        anncnum[ channum ]++;
                        dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                        if (anncnum[ channum ] >= maxannc[ channum ]) {
                            anncnum[ channum ] = (maxannc[ channum ] - 1);
                        }

                        if (dxinfox[ channum ].state == ST_DYNPLAY) {
                            sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        } else {
                            close(dxinfox[ channum ].msg_fd);
                            ownies[ channum ] = 1;
                            dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                            if (dxinfox[ channum ].state == ST_DYNPLAY) {
                                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_betabackfwd.pcm");
                            } else {
                                sprintf(dxinfox[ channum ].msg_name, "sounds/ivr_embackfwd.pcm");
                            }

                            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                            playoffset[ channum ] = 0;
                        }
                    }
                }

                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);

                if (errcode[channum] == -1) {
                    disp_msg("Dynamic sound player error!");
                }

                return (errcode[channum]);
            }

            if (strcmp("7", dxinfox[ channum ].digbuf.dg_value) == 0) {
                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);     // Make sure these first three lines are actually needed. I was super tired when I did this.
                    errcode[channum] = dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_RELCURPOS, -2);
                    play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Dynamic sound player error in volume adjust function");
                }

                return (errcode[channum]);
            }

            if (strcmp("8", dxinfox[ channum ].digbuf.dg_value) == 0) {
                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);     // Make sure these first three lines are actually needed. I was super tired when I did this.
                    errcode[channum] = dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal
                    play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Dynamic sound player error in volume adjust function");
                }

                return (errcode[channum]);
            }

            if (strcmp("9", dxinfox[ channum ].digbuf.dg_value) == 0) {
                switch(dxinfox[ channum ].state) {
                    case ST_DYNPLAY:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TCTUTORIAL:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/tutorial/%d.pcm", anncnum[channum]);
                        break;
                    case ST_DCBBS:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/dcbbs/%d.pcm", anncnum[channum]);
                        break;
                    case ST_TC24BBS2:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/tc24/bbs/%d.pcm", anncnum[channum]);
                        break;
                    default:
                        sprintf(dxinfox[ channum ].msg_name, "sounds/emtanon/%d.pcm", anncnum[channum]);
                }

                errcode[channum] = dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                disp_msgf("Announcement number is %d, error code is %d", anncnum[channum], errcode[channum]);

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                if (errcode[channum] != -1) {
                    playoffset[ channum ] = (ATDX_TRCOUNT(chdev) + playoffset[ channum ]);     // Make sure these first three lines are actually needed. I was super tired when I did this.
                    errcode[channum] = dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_RELCURPOS, 2);
                    play(channum, dxinfox[ channum ].msg_fd, 1, playoffset[ channum ], 0);
                }

                if (errcode[channum] == -1) {
                    disp_msg("Dynamic sound player error in volume adjust function");
                }

                return (errcode[channum]);
            }

            if ((strcmp("E", dxinfox[ channum ].digbuf.dg_value) == 0) && (ownies[channum] == 100)  && (anncnum[channum] == 3) && (dxinfox[ channum ].state == ST_EMPLAY2) && (frontend == CT_GCISDN)) {
                // Call origination test function
                ownies[ channum ] = 0;
                errcnt[ channum ] = 0;
                anncnum[ channum ] = 0;
                ownies[ channum ] = 0;
                //connchan[ channum ] = 1;

                if (dx_clrdigbuf(chdev) == -1) {
                    disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                }

                dx_distone(chdev, TID_1, DM_TONEON);   // Remove custom tone
                dx_deltones(chdev);   // Remove all custom tones

                // Trunkhunt replacement

                connchan[ channum ] = idle_trunkhunt( channum, 1, 23, false);
                if (connchan[ channum ] == -1) return -1;
                disp_msgf("Dest. channel is %d", connchan[channum]);

                connchan[connchan[channum]] = channum;
                makecall(connchan[channum], "1174", "2109", FALSE);   // Call 1174
                dxinfox[ channum ].state = ST_ROUTED;
                return (0);
            }


            else {
                disp_msg("No/invalid digit condition received");
                dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);
                errcnt[ channum ]++;

                if (errcnt[ channum ] == 3) {
                    dxinfox[ channum ].state = ST_INVALID;
                }

                if (dxinfox[ channum ].state == ST_ENIGMAREC2) {   //debug code. Remove when done.
                    disp_msgf("Last error state was %s", ATDV_ERRMSGP(chdev));
                    return (0);
                }

                if (play(channum, invalidfd, 0, 0, 0) == -1) {
                    disp_msgf("Cannot Play Invalid Message on channel %s", ATDV_NAMEP(chdev));
                }

                return (0);
            }

            case ST_DIGPROC:

                if (strcmp("2100", dxinfox[ channum ].digbuf.dg_value) == 0)  {

                    // Test program for multi-phrase playback; necessary since DM3 is pokey to send events back

                    dxinfox[ channum ].state = ST_PLAYMULTI;
                    multiplay[channum][0] = open("sounds/time/0.pcm", O_RDONLY);
                    multiplay[channum][1] = open("sounds/time/1.pcm", O_RDONLY);
                    multiplay[channum][2] = open("sounds/time/2.pcm", O_RDONLY);
                    multiplay[channum][3] = open("sounds/time/3.pcm", O_RDONLY);
                    multiplay[channum][4] = open("sounds/time/4.pcm", O_RDONLY);

                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    if ((errcode[channum] = playmulti(channum, 5, 128, multiplay[channum])) == -1) {
                        disp_msg("Playback test passed bad data to the playmulti function.");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        play(channum, goodbyefd, 0, 0, 0);
                    }

                    return (0);
                }

                if (strcmp("2101", dxinfox[ channum ].digbuf.dg_value) == 0)  {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_PLAYMULTI1;
                    ownies[ channum ] = digread(channum, "12184888463");
                    return (0);

                }

                if (strcmp("2102", dxinfox[ channum ].digbuf.dg_value) == 0) {    // This should work on ISDN and analog stuff at the moment. I might CAS it up later.
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    dxinfox[ channum ].state = ST_CALLPTEST;
                    dxinfox[ channum ].msg_fd = open("sounds/enter_startnum.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);
                }

                if (strcmp("2103", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    srandom(time(NULL));
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    disp_status(channum, "Running the Evans Effect DM3 patch...");
                    lig_if_followup[ channum ] = (random_at_most(3) + 1);

                    dxinfox[ channum ].state = ST_EVANSDM3;
                    ownies[ channum ] = 0;

                    if (lig_if_followup[ channum ] >= 3) {
                        ligmain[ channum ] = random_at_most(15);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-init%d.wav", ligmain[ channum ]);
                        disp_msg("Opening init file");
                        multiplay[ channum ][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                        ownies[channum]++;
                    }

                    ligmain[ channum ] = random_at_most(97);
                    dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, SV_ADD8DB);   // Reset volume to normal
                    sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-%d.wav", ligmain[ channum ]);
                    disp_msg("Opening main file");
                    multiplay[channum][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (multiplay[channum][ownies[channum]] == -1) {
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (lig_if_followup[channum] > 1) {
                        ownies[channum]++;
                        ligmain[ channum ] = random_at_most(56);
                        sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-followup%d.wav", ligmain[ channum ]);
                        disp_msg("Opening followup file");
                        multiplay[channum][ownies[channum]] = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    }

                    errcode[channum] = playmulti(channum, lig_if_followup[channum], 1, multiplay[channum]);
                    return (errcode[channum]);
                }

                if (strcmp("2104", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_TONETEST2;
                    playtone_cad(channum, 600, 0, 100);
                    return (0);
                }

                if (strcmp("2105", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Reload config file
                    confparse();

                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[channum].state = ST_GOODBYE;
                    dxinfox[ channum ].msg_fd = open("sounds/start.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);

                }

                if (strcmp("2108", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_RECORD;

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        disp_err(channum, chdev, dxinfox[ channum ].state);
                    }

                    msgnum[channum] = 0;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] != 0) {

                        sprintf(dxinfox[ channum ].msg_name, "message%d.wav", msgnum[channum]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == 0) {
                            msgnum[channum]++;
                        } else {
                            errcnt[ channum ] = 0;
                        }

                    }

                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name,
                                                     O_RDWR | O_TRUNC | O_CREAT, 0666);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot create %s for recording", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = recordwav(channum, dxinfox[ channum ].msg_fd);
                    }

                    return (0);

                }

                if (strcmp("2109", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    if (termmask[ channum ] & 1) {
                        disp_msg("Removing custom tone with incoming call");
                        dx_distone(chdev, TID_1, DM_TONEON);
                        dx_deltones(chdev);
                        termmask[ channum ] ^= 1;
                    }

                    // srandom(time(NULL)); // Seed the random number generator for the thing
                    disp_status(channum, "Accessing Emtanon BBoard");

                    errcnt[ channum ] = 0;
                    dxinfox[ channum ].state = ST_EMPLAY1;
                    ownies[ channum ] = 0;
                    dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, 0);   // Reset volume to normal

                    dxinfox[ channum ].msg_fd = open("sounds/emtanon/greeting_new.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);

                }

                if (strcmp("2110", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                    }

                    disp_status(channum, "Accessing Sound Player");
                    ownies[ channum ] = 0;
                    errcnt[ channum ] = 0;
                    loggedin[ channum ] = 0;
                    passwordfile[channum] = NULL;

                    dxinfox[ channum ].state = ST_GETCAT;
                    dxinfox[ channum ].msg_fd = open("sounds/ivr_entercat.pcm", O_RDONLY);
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (errcode[channum]);

                }

                if ((frontend == CT_GCISDN) && (strcmp("2118", dxinfox[ channum ].digbuf.dg_value) == 0)) {
                    // ISDN call origination test function; not accessible on the analog frontend.

                    if (dm3board) routecall( channum, 1, 23, "1110", "2109", TRUE);
                    else routecall( channum, 1, 23, "711110", "2109", TRUE);
                    return (0);
                }

                if (strcmp("2119", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_GOODBYE;
                    playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                    return (0);
                }

                if (strcmp("2120", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_GOODBYE;
                    playtone_cad(channum, 1025, 0, -1);
                    return (0);
                }

                if (strcmp("2121", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Customer-facing activation IVR
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }
                    memset(filetmp[channum], 0x00, sizeof(filetmp[channum]));
                    disp_statusf(channum, "Running Activation IVR: Ext. %s", isdninfo[channum].cpn);
                    srandom(time(NULL));
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_ACTIVATION;
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activation_intro.pcm", O_RDONLY);
                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                        file_error(channum, "sounds/activation/activation_intro.pcm");
                        return -1;
                    }
                    return 0;

                }

                if (strcmp("2122", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Admin IVR - Edit
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_ADMINACT;
                    disp_statusf(channum, "Running Activation Admin IVR: Ext. %s", isdninfo[channum].cpn);
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                        return -1;
                    }
                    return 0;

                }

                if (strcmp("2123", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Admin IVR - Batch Add
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }
                    errcnt[channum] = 0;
                    dxinfox[ channum ].state = ST_ADMINADD;
                    disp_statusf(channum, "Running Activation Admin Add IVR: Ext. %s", isdninfo[channum].cpn);
                    dxinfox[ channum ].msg_fd = open("sounds/activation/activationivr_admin_enterext.pcm", O_RDONLY);
                    if (play(channum, dxinfox[channum].msg_fd, 1, 0, 0) != 0) {
                        file_error(channum, "sounds/activation/activationivr_admin_enterext.pcm");
                        return -1;
                    }
                    return 0;

                }

               if (strcmp("2125", dxinfox[ channum ].digbuf.dg_value) == 0) {
                   // Loop - low end
                   if (loopchan != 0) {
                       disp_msg("Someone tried to use the low end of the loop when someone was already on it.");
                       dxinfox[ channum ].state = ST_GOODBYE;
                       playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                       return 0;
                   }
                   dxinfox[ channum ].state = ST_LOOP1;
                   loopchan = channum;
                   playtone_rep(channum, 502, 0, -10, 0, 1000, 100);
                   return 0;
               }

               if (strcmp("2126", dxinfox[ channum ].digbuf.dg_value) == 0) {
                   // Loop - high end
                   if (loopchan == 0) {
                       dxinfox[ channum ].state = ST_GOODBYE;
                       playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                       return 0;
                   }
                   if (playtone_cad(channum, 502, 0, 300) == 0) {
                       connchan[loopchan] = channum;
                       connchan[channum] = loopchan;
                       dxinfox[ channum ].state = ST_LOOP2;
                   }
                   else {
                       disp_msg("Loop2 tone generation routine encountered error!");
                       dxinfox[ channum ].state = ST_GOODBYE;
                       playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                   }
                   return 0;
               }

               if (strcmp("2124", dxinfox[ channum ].digbuf.dg_value) == 0) {
                   if (altsig & 1) {
                       set_hkstate(channum, DX_OFFHOOK);
                    }
                    srandom(time(NULL));
                    disp_status(channum, "Playing Phreakspots recording...");
                    anncnum[ channum ] = 0;
                    errcnt[ channum ] = 1;
                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "sounds/phreakspots/%d.pcm", anncnum[ channum ]);
                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }
                    maxannc[ channum ] = anncnum[ channum ];
                    anncnum[channum] = 0;
                    dxinfox[ channum ].state = ST_RDNISREC;
                    // srandom(time(NULL));
                    sprintf(dxinfox[ channum ].msg_name, "sounds/phreakspots/%d.pcm", random_at_most(maxannc[channum]));
                    //sprintf(tmpbuff, "Playing %s", dxinfox[ channum ].msg_name);
                    //disp_msg(tmpbuff);
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
                    if (play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0)  == -1) {
                        file_error(channum, dxinfox[ channum ].msg_name);
                    }
                    return (0);
                }



                if (strcmp("2111", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_DYNPLAY;
                    ownies[ channum ] = 0; // Initialize variables
                    anncnum[ channum ] = 2000;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]++;
                        }
                    }

                    maxannc[ channum ] = (anncnum[ channum ]);
                    anncnum[ channum ] = 2000;
                    errcnt[ channum ] = 1;

                    while (errcnt[ channum ] == 1) {
                        sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[ channum ]);

                        if (stat(dxinfox[ channum ].msg_name, &sts) == -1) {
                            errcnt[ channum ] = 0;
                        } else {
                            anncnum[ channum ]--;
                        }
                    }


                    minannc[ channum ] = (anncnum[ channum ]);
                    disp_msgf("maxannc is %d.", maxannc[ channum ]);

                    anncnum[ channum ] = 2000;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/%d.pcm", anncnum[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }

                    return (errcode[channum]);
                }



                if (strcmp("2112", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    srandom(time(NULL));
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    disp_status(channum, "Running the Evans Effect...");
                    lig_if_followup[ channum ] = random_at_most(3);

                    if (lig_if_followup[ channum ] == 0) {
                        dxinfox[ channum ].state = ST_EVANS1;    // If we draw 0 from the prng, bring it back here
                    }

                    else if (lig_if_followup[ channum ] == 1) {
                        dxinfox[ channum ].state = ST_EVANS2;    // Send it to the follow phrase bank if it's 1
                    }

                    else if (lig_if_followup[ channum ] == 2) {
                        dxinfox[ channum ].state = ST_EVANS3;
                    }

                    ligmain[ channum ] = random_at_most(97);
                    dx_adjsv(dxinfox[ channum ].chdev, SV_VOLUMETBL, SV_ABSPOS, SV_ADD8DB);   // Reset volume to normal
                    sprintf(dxinfox[ channum ].msg_name, "sounds/evans_sound/evans-%d.wav", ligmain[ channum ]);
                    disp_msgf("Clip number is %d", ligmain[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = playwav(channum, dxinfox[ channum ].msg_fd);
                    }

                    return (errcode[channum]);
                }

                if (strcmp("2113", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    // Quick and dirty Saso Bridge Troll Application
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_SASTROLL;
                    termmask[ channum ] |= 1;
                    //ownies[ channum ] = 100; // This is here so the system knows to remove the tone from the digit receiver when we're done

                    disp_msg("Launching bridge troll application...");

                    // Make tone 101 (TID_1) the bridge join tone
                    if (dx_blddtcad(TID_1, 440, 40, 480, 40, 24, 24, 20, 18, 0) == -1)    // The DSP seems to have trouble with cadenced tones
                        // if ( dx_blddt( TID_1, 440, 25, 480, 25, TN_TRAILING) == -1 )
                    {
                        disp_msg("Unable to build bridge join tone.");
                        return (-1);
                    }

                    if (dx_addtone(chdev, 'E', DG_USER1) == -1) {
                        disp_msgf("Unable to add bridge join tone. %s", ATDV_ERRMSGP(chdev));
                        return (-1);
                    }

                    if (dx_enbtone(chdev, TID_1, DM_TONEON) == -1) {
                        disp_msgf("Unable to enable bridge join tone.");
                        return (-1);
                    }

                    if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                        disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                        break;
                    }

                    return (0);
                }

                if (strcmp("2114", dxinfox[ channum ].digbuf.dg_value) == 0) {

                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    if (dx_clrdigbuf(chdev) == -1) {
                        disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                        disp_err(channum, chdev, dxinfox[ channum ].state);
                    }

                    dxinfox[ channum ].state = ST_CRUDEDIAL;
                    playtone(channum, 480, 0);
                    return (0);
                }


                if (strcmp("2115", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_PLAYLOOP;

                    dxinfox[ channum ].msg_fd = open("sounds/fakeconf.wav", O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = playwav(channum, dxinfox[ channum ].msg_fd);
                    }

                    return (0);
                }

                if (strcmp("2116", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_SOUNDTEST;
                    filerrcnt[ channum ] = 0;
                    sprintf(dxinfox[ channum ].msg_name, "sounds/soundtest/%d.pcm", filerrcnt[ channum ]);
                    dxinfox[ channum ].msg_fd = open(dxinfox[channum].msg_name, O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        dxinfox[ channum ].state = ST_GOODBYE;
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                        play(channum, errorfd, 0, 0, 1);
                        return (-1);
                    }

                    play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    return (0);

                }

                if (strcmp("2234", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_PLAY;

                    dxinfox[ channum ].msg_fd = open("sounds/ivr_programvo.pcm", O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }

                    return (0);
                }

                if ((strcmp("2622", dxinfox[ channum ].digbuf.dg_value) == 0) && (altsig & 1))  {   //This is the four digit ANAC function
                    set_hkstate(channum, DX_OFFHOOK);

                    dxinfox[ channum ].state = ST_ANAC;
                    ownies[ channum ] = 0; //Reset ownies to 0. This is important, since it's the variable we keep reusing.
                    disp_status(channum, "Accessing Questionable ANAC");

                    if (get_digits(channum, &(dxinfox[ channum ].digbuf), 4) == -1) {
                        disp_msgf("Cannot get digits from channel %s", ATDV_NAMEP(chdev));
                        return (-1);
                    }

                    return (0);
                }




                if (strcmp("3644", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    ownies[ channum ] = 0; // Initialize variables
                    dxinfox[ channum ].state = ST_ENIGMAREC;
                    dxinfox[ channum ].msg_fd = open("sounds/longrecord.pcm", O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                        errcode[channum] = -1;
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }

                    return (errcode[channum]);
                }

                if (strcmp("4750", dxinfox[ channum ].digbuf.dg_value) == 0) {


                    if (frontend != CT_NTANALOG) {
                        dxinfox[ channum ].state = ST_PLAYNWN;
                        dxinfox[ channum ].msg_fd = open("sounds/toorcamp_nwn.pcm", O_RDONLY);
                        disp_msg("Someone tried using outdial on a non-analog trunk.");

                        if (dxinfox[ channum ].msg_fd == -1) {
                            disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                            errcode[channum] = -1;
                        }

                        if (errcode[channum] == 0) {
                            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                        }

                        return (0);
                    }

                    dxinfox[ channum ].state = ST_OUTDIAL;
                    playtone(channum, 480, 0);
                    return (0);
                }


                if (strcmp("2337", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }

                    dxinfox[ channum ].state = ST_PLAY;

                    dxinfox[ channum ].msg_fd = open("sounds/impression.pcm", O_RDONLY);

                    if (dxinfox[ channum ].msg_fd == -1) {
                        file_error(channum, "sounds/impression.pcm");
                        return (-1);
                    }

                    if (errcode[channum] == 0) {
                        errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }

                    return (0);
                }

                if (dxinfox[ channum ].digbuf.dg_value[0] == 0x31) {
                //if (strncmp(dxinfox[ channum ].digbuf.dg_value, "1", strlen("1")) == 0) { // Compare the first digit, catch anything with a 1

                    if (altsig & 1) {
                        set_hkstate(channum, DX_OFFHOOK);
                    }
                    return(voicemail_xfer( channum, dxinfox[ channum ].digbuf.dg_value ));

                }

                if (dxinfox[ channum ].digbuf.dg_value[0] == 0x33) {
               // if (strncmp(dxinfox[ channum ].digbuf.dg_value, "3", strlen("1")) == 0) { // Same thing here for the NMS tandem op
                    // This was recently changed. Make sure your little logic puzzle still works.
                    if ((frontend != CT_NTT1) || (maxchans <= 24) || !(altsig & 1)) {   // Make sure this is a trunk we can work with first
                        dxinfox[ channum ].state = ST_PLAYNWN;
                        dxinfox[ channum ].msg_fd = open("sounds/toorcamp_nwn.pcm", O_RDONLY);
                        disp_msg("Someone tried using the tandem feature on an incompatible trunk");

                        if (dxinfox[ channum ].msg_fd == -1) {
                            disp_msgf("Cannot open %s for play-back", dxinfox[ channum ].msg_name);
                            errcode[channum] = -1;
                        }

                        if (errcode[channum] == 0) {
                            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                        }

                        return (0);
                    }

                    set_hkstate(channum, DX_OFFHOOK);

                    disp_status(channum, "Preparing for NMS tandem op");

                    for (connchan[channum] = 25; dxinfox[connchan[channum]].state != ST_WTRING; connchan[channum]++) {
                        disp_msgf("Destination channel is %i", connchan[channum]);
                    }

                    dxinfox[connchan[channum]].state = ST_ROUTED2; // Make sure the IVR won't try anything stupid.
                    connchan[connchan[channum]] = channum; // You can actually do this? Wow, that's some serious yodawgage.
                    disp_msgf("Originating channel is %d, destination is %d", connchan[connchan[channum]], connchan[channum]);  // This serves to test the variables

                    if (set_hkstate(connchan[channum], DX_OFFHOOK) == -1) {     // Drivers, start your engines.
                        disp_msg("Weird. The outgoing channel won't go offhook!");
                        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                        return (-1);
                    }

                    return (0); // Now we wait for winkback from the card and handle everything there. To do: make something happen if it fails to wink.
                }
            if (altsig & 1) {
                dxinfox[ channum ].state = ST_PLAYNWN;
                dxinfox[ channum ].msg_fd = open("sounds/toorcamp_nwn.pcm", O_RDONLY);

                if (dxinfox[ channum ].msg_fd == -1) {
                    file_error(channum, "sounds/toorcamp_nwn.pcm");
                    return -1;
                }

                if (errcode[channum] == 0) {
                    errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
                    }
                }

                else {
                    dxinfox[ channum ].state = ST_GOODBYE;
                    playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                    return 0;
                }
                return 0;

        default:

                if (strcmp("*", dxinfox[ channum ].digbuf.dg_value) == 0) {
                    if (dx_clrdigbuf(chdev) == 1) {
                        disp_msgf("DTMF Buffer clear dun goofed on %s", ATDV_NAMEP(chdev));
                        disp_err(channum, chdev, dxinfox[ channum ].state);
                    }

                    playtone(channum, 400, 0);
                    dxinfox[ channum ].state = ST_DIGPROC;
                    return (0);
                }


            disp_msg("Someone dialed a non-working number.");
            dxinfox[ channum ].state = ST_PLAYNWN;
            dxinfox[ channum ].msg_fd = open("sounds/toorcamp_nwn.pcm", O_RDONLY);

            if (dxinfox[ channum ].msg_fd == -1) {
                file_error( channum, "sounds/toorcamp_nwn.pcm");
                return -1;
            }

            if (errcode[channum] == 0) {
                errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            }

    }

    return (0);
}


/***************************************************************************
 *        NAME: int playtone_hdlr()
 * DESCRIPTION: TDX_PLAYTONE event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int playtone_hdlr() {
    int  chdev = sr_getevtdev();
    // int  event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on playtone chdev");
    }

    int  channum = get_channum(chdev);
    int  curstate;
    // int  errcode = 0;

    if (channum == -1) {
        return (0);               /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;          /* Current State */

    // close(dxinfox[ channum ].msg_fd);

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */
    switch (frontend) {
        case CT_NTANALOG:
            if (ATDX_TERMMSK(chdev) & TM_LCOFF) {
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            break;

        case CT_NTT1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) == 0) {
                return (0);
            }

            break;

        case CT_NTE1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) != 0) {
                return (0);
            }

            break;

        case CT_GCISDN:
            if (isdnstatus[channum] == 1) {
                if (ownies[channum] == 9) {
                    // If running Project Upstage, reset the digit receiver back to DTMF, delete 2600 tone definition
                    dx_setdigtyp(dxinfox[channum].chdev, D_DTMF);
                    dx_deltones(dxinfox[ channum ].chdev);
                }

                return (0);
            }

            break;
    }

    switch (curstate) {

        case ST_GOODBYE:
            dxinfox[ channum ].state = ST_ONHOOK;
            set_hkstate(channum, DX_ONHOOK);
            return (0);

        case ST_LOOP1:
            if (ATDX_TERMMSK(chdev) & TM_USRSTOP) {
                // High end will reroute TSI to incoming caller. We just need to make sure the tone player doesn't keep going.
                return 0;
            }

            else {
                // Keep playing
                playtone_rep(channum, 502, 0, -10, 0, 1000, 100);
            }
            return 0;

        case ST_LOOP2:
            dx_stopch(dxinfox[loopchan].chdev, EV_ASYNC);
            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                disp_msg("Holy shit! Looparound scunroute threw an error!");
                disp_err(channum, dxinfox[ channum ].tsdev, dxinfox[ channum ].state);
                return -1;
            }
            if (nr_scunroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                disp_msg("Holy shit! Second looparound scunroute threw an error!");
                disp_err(channum, dxinfox[ connchan[channum] ].tsdev, dxinfox[ connchan[channum] ].state);
                return -1;
            }
            if (nr_scroute(dxinfox[connchan[channum]].tsdev, SC_DTI, dxinfox[channum].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                disp_msg("Holy shit! SCroute_loop2 threw an error!");
                disp_err(channum, dxinfox[channum].tsdev, dxinfox[channum].state);
                return -1;
            }
            return 0;

        case ST_DISALOGIN:
            if (ATDX_TERMMSK(chdev) & TM_MAXDTMF) {
                if (get_digs(channum, &dxinfox[ channum ].digbuf, 15, 90, 0x120F) == -1) {
                    disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                    dxinfox[channum].state = ST_GOODBYE;
                    play(channum, errorfd, 0, 0, 1);
                    return (-1);
                }
            }

            else {
                dxinfox[ channum ].state = ST_GOODBYE;
                playtone_rep(channum, 480, 620, -24, -26, 25, 25);
                disp_status(channum, "Timeout at DISA login");
                return (0);
            }

            return (0);

        case ST_TXDATA:
            sprintf(dxinfox[ channum ].msg_name, "MOD.txt");
            dxinfox[ channum ].msg_fd = open(dxinfox[ channum ].msg_name, O_RDONLY);
            send_bell202(channum, dxinfox[ channum ].msg_fd);
            //send_bell202( channum, filetmp[channum] ); // Replace with string
            return (0);

        case ST_2600STOP:
            dxinfox[ channum ].state = ST_2600_1;
            playtone_cad(channum, 2600, 0, 4);
            return (0);
            
        case ST_2600_3:
            if (dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF) == -1) {
                // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
                // This really, *really* shouldn't fail...
                disp_msg("Unable to set digit type to DTMF!");
                disp_status(channum, "Unable to set digit type to DTMF!");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }
            ownies[channum] = 0;
            dx_deltones(dxinfox[ channum ].chdev);
            set_hkstate(channum, DX_ONHOOK);
            return(0);

        case ST_2600_1:
            // 16 digits, terminate on ST, 10 second inter-digit time
            if (dx_setdigtyp(dxinfox[ channum ].chdev, D_MF) == -1) {
                // DM_MF (for DM3) and D_MF (for JCT) are the same value. Why'd they make two?
                // This really, *really* shouldn't fail...
                disp_msg("Unable to set digit type to MF!");
                disp_status(channum, "Unable to set digit type to MF!");
                dxinfox[ channum ].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
                return (-1);
            }

            get_digs(channum, &dxinfox[ channum ].digbuf, 16, 100, 0x160F);
            return (0);

        case ST_TONETEST2:

            dxinfox[channum].state--;
            playtone_cad(channum, 500, 0, 100);
            return (0);


        case ST_TONETEST:

            dxinfox[channum].state++;
            playtone_cad(channum, 600, 0, 100);
            return (0);

        case ST_ISDNROUTE2:
        case ST_ISDNROUTE1:

            // Beware; this command pisses off analog boards; it'll just hang and block execution. We probably shouldn't be doing it.
            /*
            if (ATDX_STATE(chdev) != CS_IDLE) {
                dx_stopch(chdev, EV_ASYNC);

                while (ATDX_STATE(chdev) != CS_IDLE);
            }
            */

            if (get_digs(channum, &dxinfox[ channum ].digbuf, 20, 90, 0x120F) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                dxinfox[channum].state = ST_GOODBYE;
                play(channum, errorfd, 0, 0, 1);
            }

            return (0);

        case ST_ISDNTEST:

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 1) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
            }

            dxinfox[ channum ].state = ST_ISDNROUTE;
            return (0);


        case ST_CRUDEDIAL:
            disp_msg("Crude wardialer state active");

            errcnt[ channum ] = 0;

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 11) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
                return (-1);
            }

            return (0);

        case ST_EMPLAY1:

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 5) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
            }

            dxinfox[ channum ].state = ST_PLAYMULTI1;
            return (0);

        case ST_DIGPROC:

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 4) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
            }

            break;

        case ST_OUTDIAL:

            if (get_digits(channum, &(dxinfox[ channum ].digbuf), 11) == -1) {
                disp_msgf("DTMF collect error: %s", ATDV_ERRMSGP(chdev));
            }

    }

    return (0);


}

/***************************************************************************
 *        NAME: int txdata_hdlr()
 * DESCRIPTION: TDX_PLAYTONE event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int txdata_hdlr() {
    int  chdev = sr_getevtdev();
    // int  event = sr_getevttype();
    int  channum = get_channum(chdev);
    int  curstate;
    int errcode[MAXCHANS];
    errcode[channum] = 0;

    if (channum == -1) {
        return (0);               /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;          /* Current State */

    close(dxinfox[ channum ].msg_fd);

    /*
     * If drop in loop current, set state to ONHOOK and
     * set channel to ONHOOK in case of ANALOG frontend.
     * In case of digital frontend, the sig_hdlr will take
     * care of it.
     */
    switch (frontend) {
        case CT_NTANALOG:
            if (ATDX_TERMMSK(chdev) & TM_LCOFF) {
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return (0);
            }

            break;

        case CT_NTT1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) == 0) {
                return (0);
            }

            break;

        case CT_NTE1:
            if ((ATDT_TSSGBIT(dxinfox[ channum ].tsdev) & DTSG_RCVA) != 0) {
                return (0);
            }

            break;
    }

    switch (curstate) {

        case ST_TXDATA:
            close(dxinfox[ channum ].msg_fd);
            disp_msg("Data sent successfully. Returning to Emtanon main menu.");
            anncnum[ channum ] = 0;
            errcnt[ channum ] = 0;
            dxinfox[ channum ].state = ST_EMPLAY1;

            ownies[channum] = 0;

            dxinfox[ channum ].msg_fd = open("sounds/emtanon/greeting_new.pcm", O_RDONLY);
            errcode[channum] = play(channum, dxinfox[ channum ].msg_fd, 1, 0, 0);
            return (errcode[channum]);
    }

    return (0);


}


/***************************************************************************
 *        NAME: int sethook_hdlr()
 * DESCRIPTION: TDX_SETHOOK event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int sethook_hdlr() {
    int     chdev = sr_getevtdev();
    // int     event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on sethook chdev");
    }

    int     channum = get_channum(chdev);
    int     curstate;
    DX_CST  *cstp;

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */
    cstp = (DX_CST *) sr_getevtdatap();

    switch (cstp->cst_event) {

        // TO DO: Should this have else ifs instead? Evaluate if that has any negative effect on the code. Y'know, when code isn't being written for Toorcamp.
        // TO DO: Test the looparound implementation on non-ISDN interfaces
        case DX_ONHOOK:
            // This covers the non-ISDN implementations of the looparound code
            if (dxinfox[ channum ].state == ST_LOOP1) {
                // Reroute the TSIs and drop the caller on LOOP2
                loopchan = 0; // Return the looparound channel value to 0 so no new calls can come in
                if (connchan[ channum ] != 0) {
                    // There's someone connected; we have work to do.
                    if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                        disp_msg("Holy shit! Looparound1 scunroute ISDN handler threw an error!");
                        disp_err(channum, dxinfox[ channum ].tsdev, dxinfox[ channum ].state);
                        return -1;
                    }
                    if (nr_scroute(dxinfox[connchan[channum]].tsdev, SC_DTI, dxinfox[connchan[channum]].chdev, SC_VOX, SC_FULLDUP) == -1) {
                        disp_msg("Holy shit! Looparound1 scroute ISDN handler threw an error!");
                        disp_err(channum, dxinfox[connchan[channum]].tsdev, dxinfox[connchan[channum]].state);
                        return -1;
                    }
                    if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
                        disp_msg("Holy hatbowlies! Looparound1 scroute ISDN handler threw an error!");
                        disp_err(channum, dxinfox[channum].tsdev, dxinfox[channum].state);
                        return -1;
                    }
                    dxinfox[connchan[ channum ]].state = ST_ONHOOK;
                    set_hkstate(connchan[channum], DX_ONHOOK);
                    connchan[channum] = 0;
                    connchan[connchan[channum]] = 0;
                }
                dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
                dxinfox[ channum ].state = ST_ONHOOK;
                set_hkstate(channum, DX_ONHOOK);
                return 0;
            }

            if (dxinfox[ channum ].state == ST_LOOP2) {
                // Just reroute the TSI and walk away.
                if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ loopchan ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                    disp_msg("Holy shit! Looparound scunroute ISDN handler threw an error!");
                    disp_err(channum, dxinfox[ channum ].tsdev, dxinfox[ channum ].state);
                    return -1;
                }
                if (nr_scroute(dxinfox[loopchan].tsdev, SC_DTI, dxinfox[loopchan].chdev, SC_VOX, SC_FULLDUP) == -1) {
                    disp_msg("Holy shit! Looparound scroute ISDN handler threw an error!");
                    disp_err(channum, dxinfox[loopchan].tsdev, dxinfox[loopchan].state);
                    return -1;
                }
                if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
                    disp_msg("Holy hatbowlies! Looparound scroute ISDN handler threw an error!");
                    disp_err(channum, dxinfox[channum].tsdev, dxinfox[channum].state);
                    return -1;
                }
                dxinfox[ channum ].state = ST_ONHOOK;
                connchan[loopchan] = 0;
                connchan[channum] = 0;
                set_hkstate(channum, DX_ONHOOK);

                return 0;
            }

            if (dxinfox[ channum ].state == ST_GETCAT3) {
                ownies[ channum ] = 5;
            }

            if (dxinfox[ channum ].state == ST_CALLPTEST4) {

                if (dx_dial(dxinfox[ channum ].chdev, ",", NULL, EV_ASYNC) != 0) {
                    disp_msg("Wardial pause failed!");
                    disp_status(channum, "Error during pause function");
                    dxinfox[channum].state = ST_WTRING;
                    return (-1);
                }

                return (0);

            }

            if (dxinfox[ channum ].state == ST_CALLPTEST5) {
                if (dx_dial(dxinfox[ channum ].chdev, ",", NULL, EV_ASYNC) != 0) {
                    disp_msg("Wardial pause failed!");
                    disp_status(channum, "Error during pause function");
                    dxinfox[channum].state = ST_WTRING;
                    return (-1);
                }

                return (0);
            }

            dxinfox[ channum ].state = ST_WTRING;

            if (dx_clrdigbuf(chdev) == -1) {
                disp_msgf("Cannot clear DTMF Buffer for %s", ATDV_NAMEP(chdev));
                disp_err(channum, chdev, dxinfox[ channum ].state);
            }

            disp_status(channum, "Ready to accept a call");
            break;

        case DX_OFFHOOK:
            if (!(altsig & 1)) {

                if (dxinfox[ channum ].state == ST_CALLPTEST4) {
                    // Call progress dialer test
                    dxinfox[ channum ].state = ST_CALLPTEST5;

                    if (dx_dial(dxinfox[ channum ].chdev, dialerdest[channum], &cap, DX_CALLP | EV_ASYNC) != 0) {
                        disp_msg("Call progress origination test failed!");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        set_hkstate(channum, DX_ONHOOK);
                        return (0);
                    }

                    return (0);
                }

                if (dxinfox[ channum ].state == ST_CALLPTEST5) {
                    disp_msgf("Dialer - dest: %s", dialerdest[ channum ]);

                    if (dx_dial(dxinfox[ channum ].chdev, dialerdest[channum], &cap, DX_CALLP | EV_ASYNC) != 0) {
                        disp_msg("Call progress origination test failed!");
                        dxinfox[ channum ].state = ST_GOODBYE;
                        set_hkstate(channum, DX_ONHOOK);
                    }

                    return (0);
                }

                playtone(channum, 400, 0);
                dxinfox[ channum ].state = ST_DIGPROC;
                break;
            }
    }

    return (0);
}


/***************************************************************************
 *        NAME: int error_hdlr()
 * DESCRIPTION: TDX_ERROR event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int error_hdlr() {
    int chdev = sr_getevtdev();
    // int event = sr_getevttype();

    if (chdev == -1) {
        disp_msg("-1 on error chdev");
    }

    short channum = get_channum(chdev);
    int curstate;

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */

    /*
     * Print a message
     */
    disp_msgf("Received ERROR Event %s for %s in state %d", ATDV_ERRMSGP(chdev), ATDV_NAMEP(chdev), curstate);

    if (ATDV_LASTERR(chdev) == EDX_SYSTEM) {
        disp_msgf("System error code was %d", errno);
    }

    if (errno == 2) {
        disp_msgf("%s", dxinfox[ channum ].msg_name);
    }

    /*
     * Put state into ST_ERROR
     */
    dxinfox[ channum ].state = ST_ERROR;
    set_hkstate(channum, DX_ONHOOK);

    return (0);
}


/***************************************************************************
 *        NAME: int fallback_hdlr()
 * DESCRIPTION: Fall-Back event handler
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ***************************************************************************/
int fallback_hdlr() {
    int chtsdev = sr_getevtdev();
    int event = sr_getevttype();

    if (chtsdev == -1) {
        disp_msg("-1 on fallback chtsdev");
    }

    short channum = get_channum(chtsdev);
    int curstate;

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */

    /*
     * Print a message
     */
    disp_msgf("Unknown event %d for device %s", event, ATDV_NAMEP(chtsdev));

    /*
     * If a fallback handler is called put the state in ST_ERROR
     */
    dxinfox[ channum ].state = ST_ERROR;
    set_hkstate(channum, DX_ONHOOK);

    return (0);
}


/******************************************************************************
 *        NAME: int sig_hdlr()
 * DESCRIPTION: Signal handler to catch DTEV_SIG events generated by the dti
 *              timeslots.
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ******************************************************************************/
int sig_hdlr() {
    int tsdev = sr_getevtdev();
    int event = sr_getevttype();
    unsigned short *ev_datap = (unsigned short *)sr_getevtdatap();
    unsigned short sig = (unsigned short)(*ev_datap);

    if (tsdev == -1) {
        disp_msg("-1 on sig tsdev");
    }

    short channum = get_channum(tsdev);
    int curstate;
    short indx;


    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */

    if (event != DTEV_SIG) {
        disp_msgf("Unknown Event 0x%lx Received on %s.  Data = 0x%hx", (long unsigned int) event, ATDV_NAMEP(tsdev), sig);
        disp_status(channum, "Unknown Event");
        dxinfox[ channum ].state = ST_ERROR;
        set_hkstate(channum, DX_ONHOOK);
        return 0;
    }

    for (indx = 0; indx < 4; indx++) {
        /*
         * Check if bit in change mask (upper nibble - lower byte) is set or
         * if this is a WINK (upper nibble - upper byte) event
         */
        if (!(sig & (SIGEVTCHK << indx))) {
            disp_msgf("Signaling event detected");
            continue;
        }

        switch (sig & (SIGBITCHK << indx)) {
            case DTMM_AON:
                switch (frontend) {
                    case CT_NTT1:       /* Incoming Rings Event */
                        if (curstate == ST_WTRING) {
                            /*
                             * Set Channel to Off-Hook
                             */
                            if (!(altsig & 1)) {   /* Support for T1 wink start signaling */
                                dxinfox[ channum ].state = ST_OFFHOOK;
                                set_hkstate(channum, DX_OFFHOOK);

                                disp_status(channum, "Incoming Call");
                                return 0;
                            }

                            else if (!(altsig & 1)) {

                                if (dt_xmitwink(dxinfox[ channum ].tsdev, 0) == -1) {
                                    disp_msgf("Something went wrong with winkback. Error %s", ATDV_ERRMSGP(tsdev));
                                    break;
                                }

                                dxinfox[ channum ].state = ST_WINK;

                            }
                        }

                        return (0);

                    case CT_NTE1:
                        /*
                         * Caller hangup, set state to ONHOOK and set channel to ONHOOK
                         */
                        dxinfox[ channum ].state = ST_ONHOOK;
                        set_hkstate(channum, DX_ONHOOK);
                        return 0;
                }

                break;

            case DTMM_AOFF:
                switch (frontend) {
                    case CT_NTE1:       /* Incoming Rings Event */
                        if (curstate == ST_WTRING) {
                            /*
                             * Set Channel to Off-Hook
                             */
                            dxinfox[ channum ].state = ST_OFFHOOK;
                            set_hkstate(channum, DX_OFFHOOK);

                            disp_status(channum, "Incoming Call");
                            return 0;
                        }

                        break;

                    case CT_NTT1:

                        /*
                         * Caller hangup, set state to ONHOOK and set channel to ONHOOK
                         */
                        if (dxinfox[ channum ].state == ST_ROUTED2) {
                            return (0);
                        }

                        if (dxinfox[ channum ].state == ST_GETCAT3) {
                            ownies[ channum ] = 5;
                        }

                        if ((dxinfox[ channum ].state == ST_ROUTED) || (dxinfox[ connchan[channum]].state == ST_ROUTED)) {
                            disp_msg("Entering NMS teardown routine");
                            disp_status(channum, "Ready to accept a call");
                            disp_status(connchan[channum], "Ready to accept a call");

                            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                                disp_msg("Couldn't unroute extender!");
                                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                                return (-1);
                            }

                            if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                                disp_msgf("Holy shit! Rerouting channel %c threw an error!", channum);
                                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                                return (-1);
                            }

                            if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                                disp_msgf("Holy shit! Rerouting channel %c threw an error!", connchan[channum]);
                                disp_err(connchan[channum], dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                                return (-1);
                            }

                            dxinfox[connchan[channum]].state = ST_ONHOOK;
                            set_hkstate(connchan[channum], DX_ONHOOK);
                            disp_msgf("De-tandeming call on channel %i", connchan[channum]);
                            connchan[connchan[channum]] = 0; // Bugfix; previously used channels for tandem function will drop a tandem call in progress if variable not reset. You should probably eliminate the symptom of this too.
                            connchan[channum] = 0;
                            disp_msgf("De-tandeming other call on channel %i", channum);
                        }

                        dxinfox[ channum ].state = ST_ONHOOK;
                        set_hkstate(channum, DX_ONHOOK);
                        return 0;
                }

                break;

            case DTMM_BOFF:
            case DTMM_BON:
            case DTMM_COFF:
            case DTMM_CON:
            case DTMM_DOFF:
            case DTMM_DON:
            case DTMM_WINK:
                // Holy bloody buttfuck, these routines are crusty. If we start using non-Globalcall winkstart again, overhaul this and its related functions.
                disp_msg("This is a test of wink detection. We appear to be receiving it correctly.");

                if (dxinfox[ channum ].state == ST_ROUTED2) {
                    disp_status(connchan[channum], "Performing NMS tandem operation, received wink");
                    dx_dial(dxinfox[ channum ].chdev, dxinfox[connchan[channum]].digbuf.dg_value, NULL, EV_ASYNC);
                    return (0);
                }

                dxinfox[ channum ].state = ST_WINKDIG;
                return (0);
                break;

            default:
                disp_msgf("Unknown DTEV_SIG Event 0x%hx Received on %s", sig, ATDV_NAMEP(tsdev));
        }
    }

    return 0;
}


/******************************************************************************
 *        NAME: int dtierr_hdlr()
 * DESCRIPTION: Signal handler to catch DTEV_ERREVT events generated by the
 *              dti timeslots.
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: 0 if event to be eaten by SRL otherwise 1
 *    CAUTIONS: None.
 ******************************************************************************/
int dtierr_hdlr() {
    int tsdev = sr_getevtdev();
    // int event = sr_getevttype();

    if (tsdev == -1) {
        disp_msg("-1 on dtierr tsdev");
    }

    short channum = get_channum(tsdev);
    int curstate;

    if (channum == -1) {
        return (0);      /* Discard Message - Not for a Known Device */
    }

    curstate = dxinfox[ channum ].state;        /* Current State */

    /*
     * Print a message
     */
    disp_msgf("Received an ERROR Event for %s", ATDV_NAMEP(tsdev));

    /*
     * Put state into ST_ERROR
     */
    dxinfox[ channum ].state = ST_ERROR;
    set_hkstate(channum, DX_ONHOOK);

    return (0);
}


/***************************************************************************
 *        NAME: void chkargs( argc, argv )
 * DESCRIPTION: Check Command Line Arguments, Display Help or Set
 *      globals to indicate board number and number of
 *      channels to use.
 *       INPUT: int argc;   - Argument Count.
 *      char *argv[];   - Array of Pointers to Arguments.
 *      OUTPUT: None.
 *     RETURNS: None.
 *    CAUTIONS: None.
 ***************************************************************************/
void chkargs(argc, argv)
int argc;
char *argv[];
{
    int arg;

    /*
     *  Check Command Line for Options
     *  N.B. Arguments to Options MUST be Seperated by White Space
     *
     */
    while ((arg = getopt(argc, argv, "d:t:n:f:prewDCgliT?")) != -1) {
        switch (arg) {
            case 'd':
                /*
                 * First D/4x Board Number
                 */
                d4xbdnum = (int) atol(optarg);
                break;

            case 't':
                /*
                 * First DTI Board Number
                 */
                dtibdnum = (int) atol(optarg);
                break;

            case 'D':

                /*
                 * Q.931 debug (otherwise ignored). This should be part of a main.conf function.
                 */

                q931debug = 1;
                break;

            case 'C':
                /*
                 * This function should only be used with DM3
                 * boards; JCT boards are not conference capable
                 * and will simply return an error.
                 */

                conf = true;

                break;

            case 'l':

                altsig |= 4;
                calllog = fopen("isdncalls.log", "a+");
                disp_msg("ISDN call log opened");
                break;

            case 'g':
                /*
                 * Bitmask; reuse altsig variable to save RAM, engage general debug logging
                 */
                altsig |= 2;
                debugfile = fopen("debug.log", "a+");
                // Error handler should be incorporated into this

                fprintf(debugfile, "General debug log opened anew\n");
                break;

            case 'w':
                /*
                 * altsig
                 */
                altsig |= 1;
                break;

            case 'e':
                /*
                 * Turn echo cancellation for conference call
                 */
                disp_msg("Turning EC on...");
                confattr |= MSPA_ECHOXCLEN;

                break;

            case 'i':
                /*
                 * Turn analog caller ID on
                 */
                disp_msg("Turning caller ID on...");
                altsig |= 8;
                break;

            case 'n':
                /*
                 * Number of Channels to Use
                 */
                maxchans = (int) atol(optarg);
                break;
            case 'T':
                confparse();
                config_dump(&config);
                exit(0);
            case 'f':

                /*
                 * Frontend - T1, E1
                 */
                if (*optarg == 'T') {
                    frontend = CT_NTT1;
                } else if (*optarg == 'E') {
                    frontend = CT_NTE1;
                } else if (*optarg == 'A') {
                    frontend = CT_NTANALOG;
                } else if (*optarg == 'I') {
                    frontend = CT_GCISDN;
                } else {
                    printf("Unknown frontend value: %s\n", optarg);
                    exit(1);
                }

                break;

            case '?':
                /*
                 * Display Help Message
                 */
                printf("\nUsage: %s [-d d4xnum] [-t dtinum] [-n N] [-f frontend]\n", argv[ 0 ]);
                printf("-d d4xnum\tNumber of the first D/4x Board to use\n");
                printf("-t dtinum\tNumber of the first DTI Board to use\n");
                printf("-n N\t\tNumber of D/4x channels to use, max: %d\n", MAXCHANS);
                printf("-f frontend\tAnalog, T1 or E1\n");
                printf("-i Analog Caller ID\t(JCT Analog only)\n");
                printf("-w Winkstart operation (JCT T1 only)\n");
                printf("-e Engage echo cancellers (conference only. Refer to media load properties before using)\n");
                printf("-C Enable conference capabilities (DM3 only)\n");
                printf("-D Q.931 IE debug info written to .dump file\n");
                printf("-g General debug; write channel actions to debug.log\n");
                printf("-l ISDN incoming call log\n");
                exit(1);
        }
    }
}


/***************************************************************************
 *        NAME: void sysinit()
 * DESCRIPTION: Start D/4x System, Enable CST events, put line ON-Hook
 *      and open VOX files.
 *       INPUT: None.
 *      OUTPUT: None.
 *     RETURNS: None.
 *    CAUTIONS: None.
 ***************************************************************************/
void sysinit() {
    int  channum;
    char     d4xname[ 32 ];
    char     dtiname[ 32 ];
    // char *chname;
    // int   chno;
    DX_SVMT volumetbl;
//   TONE_SEG toneseg;

    if (maxchans > MAXCHANS)  {
        disp_msgf("Only %d Channels will be used", MAXCHANS);
        maxchans = MAXCHANS;
    }

    memset(&volumetbl, 0, sizeof(DX_SVMT));

    disp_msg("Initializing ....");
    volumetbl.decrease[0] = -128;
    volumetbl.decrease[1] = -128;
    volumetbl.decrease[2] = -128;
    volumetbl.decrease[3] = -128;
    volumetbl.decrease[4] = -10;
    volumetbl.decrease[5] = -8;
    volumetbl.decrease[6] = -5;
    volumetbl.decrease[7] = -3;
    volumetbl.decrease[8] = 1;
    volumetbl.decrease[9] = 3;
    volumetbl.origin    = 4;
    volumetbl.increase[0] = 6;
    volumetbl.increase[1] = 8;
    volumetbl.increase[2] = 9;
    volumetbl.increase[3] = 10;
    volumetbl.increase[4] = -128;
    volumetbl.increase[5] = -128;
    volumetbl.increase[6] = -128;
    volumetbl.increase[7] = -128;
    volumetbl.increase[8] = -128;
    volumetbl.increase[9] = -128;

    // No more, please. SHIT BE LOOOOOOOUD
    /*
     * Open VOX Files
     */

    if ((invalidfd = open(INVALID_VOX, O_RDONLY)) == -1) {
        disp_msgf("Cannot open %s", INVALID_VOX);
        QUIT(2);
    }

    if ((goodbyefd = open(GOODBYE_VOX, O_RDONLY)) == -1) {
        disp_msgf("Cannot open %s", GOODBYE_VOX);
        QUIT(2);
    }

    if ((errorfd = open(ERROR_VOX, O_RDONLY)) == -1) {
        disp_msgf("Cannot open %s", ERROR_VOX);
        QUIT(2);
    }

    // TO DO: Make an execution flag/config option for these to be optional.

    if (sqlite3_open("activation.db", &activationdb) != SQLITE_OK) {
        disp_msgf("Cannot open database scdp_peers.db");
        QUIT(2);
    }

    if (sqlite3_open("tc.db", &tc_blacklist) != SQLITE_OK) {
        disp_msgf("Cannot open database tc.db");
        QUIT(2);
    }

    /*
     * Clear the dxinfo structure, parse config file.
     * Initialize channel states to start boards/protocols as necessary
     */
    memset(&dxinfox, 0x00, (sizeof(DX_INFO_Y) * (MAXCHANS + 1)));

    confparse();

    if (frontend == CT_GCISDN) {

        if (conf == 1) {
            // Do conference prep here
            if ((confdev = dcb_open("dcbB1D1", 0)) == -1) {
                disp_msg("Could not open DSP device for conferencing capabilities");
                QUIT(2);
            }

            if ((confbrd = dcb_open("dcbB1", 0)) == -1) {
                disp_msg("Could not open board device for conferencing capabilities!");
                QUIT(2);
            }

            if (set_confparm(confbrd, MSG_TONECLAMP, TONECLAMP_OFF) == -1) {
                disp_msgf("Couldn't set_confparm! Error was %s", ATDV_ERRMSGP(confdev));
                QUIT(2);
            }

            if (set_confparm(confbrd, MSG_ACTID, ACTID_OFF) == -1) {
                disp_msgf("Couldn't set_confparm! Error was %s", ATDV_ERRMSGP(confdev));
                QUIT(2);
            }

        }

        isdn_prep(maxchans);

        for (channum = 1; channum <= maxchans; channum++) {
            if ((channum % 24) == 0) {
                continue;
            }
            
            dxinfox[channum].msg_fd = -1;

            if (dx_setsvmt(dxinfox[ channum ].chdev, SV_VOLUMETBL, &volumetbl, 0) == -1) {
                disp_msgf("ERROR: Unable to set default volume table for channel %d!", channum);
                QUIT(2);
            }
        }

        /*if (dm3board) {
            tonedata.structver = 0;
            tonedata.toneseg[0].structver = 0;
            tonedata.toneseg[0].tn_dflag = 0;
            tonedata.toneseg[0].tn1_min = 345;
            tonedata.toneseg[0].tn1_max = 355;
            tonedata.toneseg[0].tn2_min = 435;
            tonedata.toneseg[0].tn2_max = 445;
            tonedata.toneseg[0].tn_twinmin = 0;
            tonedata.toneseg[0].tn_twinmax = 0;
            tonedata.toneseg[0].tnon_min = 10;
            tonedata.toneseg[0].tnon_max = 0;
            tonedata.toneseg[0].tnoff_min = 0;
            tonedata.toneseg[0].tnoff_max = 0;
            tonedata.tn_rep_cnt = 1;
            tonedata.numofseg = 1;

            //tonedata.toneseg = toneseg[0];
            //Temporary change
            //for (bdnum = 1; bdnum <= 2; bdnum++) {
            bdnum = 1;
                // The divisble by 24 thing isn't an issue here.
                sprintf(boardid, "brdB%d", bdnum);
                bddev[bdnum] = dx_open(boardid, 0);

                if (bddev[bdnum] == -1) {
                    disp_msgf("dx_open for board device %d failed! String is %s", bdnum, boardid);
                    QUIT(2);
                }

                // This needs to be done. Every, single, time >.< .
                if (dx_querytone(bddev[bdnum], TID_FAX1, &tonedata2, EV_SYNC) == -1) {
                    disp_msgf("ERROR: Could not perform dx_querytone for board %d! Error %ld", bdnum, ATDV_LASTERR(bddev[bdnum]));
                    QUIT(2);
                }

                if (dx_deletetone(bddev[bdnum], TID_FAX1, EV_SYNC) == -1) {
                    disp_msgf("ERROR: Could not perform dx_deletetone for board %d! Error %ld", bdnum, ATDV_LASTERR(bddev[bdnum]));
                    QUIT(2);
                }

                if (dx_createtone(bddev[bdnum], TID_FAX1, &tonedata, EV_SYNC) == -1) {
                    disp_msgf("ERROR: Could not perform dx_createtone for board %d! Error %ld", bdnum, ATDV_LASTERR(bddev[bdnum]));
                    QUIT(2);
                }

                //bdnum++;
        }
        */
        if (sr_enbhdlr(EV_ANYDEV, TDX_CST, (EVTHDLRTYP)cst_hdlr)
                == -1) {
            disp_msg("Unable to set-up the CST handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_TXDATA, (EVTHDLRTYP)txdata_hdlr)
                == -1) {
            disp_msg("Unable to set-up the TX data handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_PLAY, (EVTHDLRTYP)play_hdlr)
                == -1) {
            disp_msg("Unable to set-up the PLAY handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_RECORD, (EVTHDLRTYP)record_hdlr)
                == -1) {
            disp_msg("Unable to set-up the RECORD handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_PLAYTONE, (EVTHDLRTYP)playtone_hdlr)
                == -1) {
            disp_msg("Unable to set-up the PLAYTONE handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_DIAL, (EVTHDLRTYP)dial_hdlr)
                == -1) {
            disp_msg("Unable to set-up the DIAL handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_GETDIG, (EVTHDLRTYP)getdig_hdlr)
                == -1) {
            disp_msg("Unable to set-up the GETDIG handler");
            QUIT(2);
        }

        if (sr_enbhdlr(EV_ANYDEV, TDX_ERROR, (EVTHDLRTYP)error_hdlr)
                == -1) {
            disp_msg("Unable to set-up the ERROR handler");
            QUIT(2);
        }
    }

    else {
        // There is no channel zero, so position zero on this array will work just fine.
        if (altsig & 8) {
            ownies[0] = DX_CALLIDENABLE;
        }

        for (channum = 1; channum <= maxchans; channum++) {
            /*
             * Open the D/4x Channels
             */
            sprintf(d4xname, "dxxxB%dC%d",
                    (channum % 4) ? (channum / 4) + d4xbdnum :
                    d4xbdnum + (channum / 4) - 1,
                    (channum % 4) ? (channum % 4) : 4);

            if ((dxinfox[ channum ].chdev = dx_open(d4xname, 0)) == -1) {
                disp_msgf("Unable to open channel %s, errno = %d", d4xname, errno);
                QUIT(2);
            }

            disp_msg("Setting channel volume...");

            if (dx_setsvmt(dxinfox[ channum ].chdev, SV_VOLUMETBL, &volumetbl, 0) == -1) {
                disp_msgf("ERROR: Unable to set default volume table for channel %d!", channum);
                QUIT(2);
            }

            if (frontend == CT_NTANALOG) {
                /*
                 * Route analog frontend timeslots to its resource in SCbus mode,
                 * if required.
                 */

                if (altsig & 8) {
                    if (dx_setparm(dxinfox[ channum ].chdev, DXCH_CALLID, (void *)&ownies[0]) == 0) {
                        disp_msg("Caller ID enabled successfully!");
                    } else {
                        disp_msg("Error enabling caller ID!");
                    }
                }

                if (scbus && routeag) {
                    nr_scunroute(dxinfox[ channum ].chdev, SC_LSI,
                                 dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP);

                    if (nr_scroute(dxinfox[ channum ].chdev, SC_LSI,
                                   dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP)
                            == -1) {
                        disp_msgf("nr_scroute() failed for %s", ATDV_NAMEP(dxinfox[ channum ].chdev));
                        QUIT(2);
                    }
                }
            } else {        /* Digital Frontend */
                /*
                 * Form digital timeslots' names based upon bus mode.
                 */
                if (scbus) {
                    //sprintf( dtiname, "dtiB%dT%d", dtibdnum, channum );
                    sprintf(dtiname, "dtiB%dT%d", (channum > 24) ? 2 : 1, (channum > 24) ? (channum - 24) : channum);
                } else {
                    sprintf(dtiname, "/dev/dtiB%dT%d", dtibdnum, channum);
                }

                /*
                 * Open DTI timeslots.
                 */
                if ((dxinfox[ channum ].tsdev = dt_open(dtiname, 0)) == -1) {
                    disp_msgf("Unable to open timeslot %s, errno = %d", dtiname, errno);
                    QUIT(2);
                }

                /*
                 * Route timeslots to channels based upon bus mode.
                 */
                if (!scbus) {
                    disp_msg("Bus type must be scbus only!");
                    QUIT(2);
                }

                nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI,
                             dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP);

                if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI,
                               dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP)
                        == -1) {
                    disp_msgf("nr_scroute() failed for %s - %s", ATDV_NAMEP(dxinfox[ channum ].chdev), ATDV_NAMEP(dxinfox[ channum ].tsdev));
                    QUIT(2);
                }
            }

            /*
             * Enable the callback event handlers
             */
            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_CST, (EVTHDLRTYP)cst_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the CST handler");
                QUIT(2);
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_TXDATA, (EVTHDLRTYP)txdata_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the TX data handler");
                QUIT(2);
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_PLAY, (EVTHDLRTYP)play_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the PLAY handler");
                QUIT(2);
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_RECORD, (EVTHDLRTYP)record_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the RECORD handler");
                QUIT(2);
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_PLAYTONE, (EVTHDLRTYP)playtone_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the PLAYTONE handler");
                QUIT(2);
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_DIAL, (EVTHDLRTYP)dial_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the DIAL handler");
                QUIT(2);
            }

            if (frontend != CT_GCISDN) {

                if (sr_enbhdlr(EV_ANYDEV, TDX_CALLP, (EVTHDLRTYP)callprog_hdlr)
                        == -1) {
                    disp_msg("Unable to set-up the call progress handler");
                    QUIT(2);
                }

            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_GETDIG, (EVTHDLRTYP)getdig_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the GETDIG handler");
                QUIT(2);
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_SETHOOK, (EVTHDLRTYP)sethook_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the SETHOOK handler");
                QUIT(2);
            }

            if (frontend != CT_NTANALOG) {
                if (sr_enbhdlr(dxinfox[channum].tsdev, DTEV_WINKCPLT, (EVTHDLRTYP)wink_hdlr)
                        == -1) {
                    disp_msg("Unable to set-up the WINK handler");
                    QUIT(2);
                }
            }

            if (sr_enbhdlr(dxinfox[channum].chdev, TDX_ERROR, (EVTHDLRTYP)error_hdlr)
                    == -1) {
                disp_msg("Unable to set-up the ERROR handler");
                QUIT(2);
            }

            /*
             * Enable the CST events
             */
            if (dx_setevtmsk(dxinfox[ channum ].chdev, DM_RINGS) == -1) {
                disp_msgf("Cannot set CST events for %s", ATDV_NAMEP(dxinfox[ channum ].chdev));
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                QUIT(2);
            }

            /*
             * Set to answer after MAXRING rings
             */
            if (dx_setrings(dxinfox[ channum ].chdev, MAXRING) == -1) {
                disp_msgf("dx_setrings() failed for %s", ATDV_NAMEP(dxinfox[ channum ].chdev));
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                QUIT(2);
            }

            sprintf(dxinfox[ channum ].msg_name, "message%d.vox", channum);

            /*
             * If it is a digital network environment, disable idle on the timeslot
             * and set it to signalling insertion.  Also setup the signalling event
             * handler.
             */
            if (frontend != CT_NTANALOG) {
                if (dt_setidle(dxinfox[ channum ].tsdev, DTIS_DISABLE) == -1) {
                    disp_msgf("Cannot disable IDLE for %s", ATDV_NAMEP(dxinfox[ channum ].tsdev));
                    disp_err(channum, dxinfox[ channum ].tsdev, dxinfox[ channum ].state);
                    QUIT(2);
                }

                if (dt_setsigmod(dxinfox[ channum ].tsdev, DTM_SIGINS) == -1) {
                    disp_msgf("Cannot set SIGINS for %s", ATDV_NAMEP(dxinfox[ channum ].tsdev));
                    disp_err(channum, dxinfox[ channum ].tsdev, dxinfox[ channum ].state);
                    QUIT(2);
                }

                /*
                 * Unblock E1 timeslot's signalling bits to ready state.
                 */
                if (frontend == CT_NTE1) {
                    if (dt_settssigsim(dxinfox[ channum ].tsdev,
                                       DTB_DON | DTB_COFF | DTB_BOFF | DTB_AON) == -1) {
                        disp_msgf("Cannot set bits to ready state on %s", ATDV_NAMEP(dxinfox[ channum ].tsdev));
                        disp_err(channum, dxinfox[ channum ].tsdev,
                                 dxinfox[ channum ].state);
                        QUIT(2);
                    }
                }

                if (sr_enbhdlr(dxinfox[channum].tsdev, DTEV_SIG, (EVTHDLRTYP)sig_hdlr)
                        == -1) {
                    disp_msg("Unable to set-up the DTI signalling handler");
                    QUIT(2);
                }

                if (sr_enbhdlr(dxinfox[channum].tsdev, DTEV_ERREVT,
                               (EVTHDLRTYP)dtierr_hdlr) == -1) {
                    disp_msg("Unable to set-up the DTI error handler");
                    QUIT(2);
                }

                if (dt_setevtmsk(dxinfox[ channum ].tsdev, DTG_SIGEVT,
                                 DTMM_AOFF | DTMM_AON | DTMM_WINK, DTA_SETMSK) == -1) {
                    disp_msg("Unable to set DTI signalling event mask");
                    QUIT(2);
                }
            }

            /*
             * Start the application by putting the channel to ON-HOOK state.
             */
            dxinfox[ channum ].state = ST_ONHOOK;
            set_hkstate(channum, DX_ONHOOK);
        }
    }

    //else

    /*
     * Display number of channels being used
     */
    disp_msgf("Using %d line%s", maxchans, maxchans > 1 ? "s" : "");
}


/***************************************************************************
 *        NAME: int main( argc, argv )
 * DESCRIPTION: Entry Point to Application.
 *       INPUT: int argc;   - Argument Count.
 *      char *argv[];   - Array of Pointers to Arguments.
 *      OUTPUT: None.
 *     RETURNS: None.
 *    CAUTIONS: None.
 ***************************************************************************/
int main(int argc, char *argv[])
/*
int argc;
char *argv[];
*/
{
    int mode;
    // Initialize variables
    d4xbdnum = 1;
    dtibdnum = 1;
    altsig = 0;

    /* Trust me, I'm a doctor. */
    if (sizeof(long) != sizeof(int)) {
        fprintf(stderr, "sizeof(long) != sizeof(int) (%d != %d) - cannot continue.", sizeof(long), sizeof(int));

        return 255;
    }

    /* Initialize channel info struct to all-zeroes */
    memset(&chaninfo, 0, sizeof(chaninfo));

    printf("Processing args\n");
    /*
     * Process Command Line Arguments
     */
    chkargs(argc, argv);
    printf("Setting up signals\n");


    /*
     * Initialise the Display
     */
    printf("Initializing display\n");
    disp_init();

    /*
     * Set up the Signal Handler
     */
    sigset(SIGHUP, (void (*)()) intr_hdlr);
    sigset(SIGINT, (void (*)()) intr_hdlr);
    sigset(SIGQUIT, (void (*)()) intr_hdlr);
    sigset(SIGTERM, (void (*)()) intr_hdlr);

#ifndef SIGNAL
    /*
     * Set the Device to Polled Mode
     */
    mode = SR_POLLMODE;

    if (sr_setparm(SRL_DEVICE, SR_MODEID, &mode) == -1) {
        disp_msg("Unable to set to Polled Mode");
        QUIT(1);
    }

#endif

    /*
     * Set-up the fall-back handler
     */
    if (sr_enbhdlr(EV_ANYDEV, EV_ANYEVT, (EVTHDLRTYP)fallback_hdlr) == -1) {
        disp_msg("Unable to set-up the fall back handler");
        QUIT(1);
    }

    /*
     * Initialize System
     */
    sysinit();

    /**
     **   Main Loop
     **/
    while (1)  {
#ifndef SIGNAL
        sr_waitevt(1000);
#else
        sleep(1);
#endif

        if (end) {
            end_app();
            break;
        }
    }

    return (0);
}

