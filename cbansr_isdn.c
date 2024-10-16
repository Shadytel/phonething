/*********************************
* Super Secret ThoughtPhreaker IVR
*    ISDN GlobalCall Handler
*
**********************************/

#include <dtilib.h> // Just for dt_open.  Consider removing that function and putting it in other code.

// Dialogic Libraries

#include <srllib.h>
#include <dxxxlib.h>
#include <gclib.h>
#include <gcisdn.h>
#include <sctools.h>
#include <dm3cc_parm.h> // For call progress analysis on DM3s

#include "answer.h"

// System Libraries

#include <stdio.h>
#include <srllib.h>
#include <malloc.h>
#include <memory.h> // For memset
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h> // For exit function

#define random_at_most(x) (rand() % (x))

// For numbering plan identification in set_cpn():
// These definitions are all pre-LSHed to fit into the most significant bits of an 8-bit integer. Please note, the MSB is an extension bit; MSB low indicates screen/presentation info follows.
#define UNKNOWN 0
#define INTERNATIONAL 16
#define NATIONAL 32
#define NETWORK_SPECIFIC 48
#define SUBSCRIBER 64
#define ABBREVIATED 96
#define RESERVED_TYPE 112

#define ISDN 1
#define DATA 3
#define TELEX 4
#define NATIONAL_PLAN 8
#define PRIVATE 9
#define RESERVED_PLAN 15

#define PRES_ALLOW 128
#define PRES_RESTRICT 160
#define PRES_UNAVAIL 192
#define PRES_RESERVED 224
#define USER_NOSCREEN 0
#define USER_VERPASS 1
#define USER_VERFAIL 2
#define NETSCREEN 3

#define NOREC 0
#define REC 1
#define ONEWAY 2

#define SPEECH 0x80
#define UNRESTRICTED 0x88
#define RESTRICTED 0x89
#define NB_AUDIO 0x90
#define UNRESTRICTED_AUDIO 0x91
#define VIDEO 0x98

/*
 * Cast type for event handler functions to avoid warning from SVR4 compiler.
 */
typedef long int (*EVTHDLRTYP)();

GC_INFO         gc_error_info;  // GC error data


static struct linebag {
    LINEDEV ldev;               /* GlobalCall API line device handle */
    CRN     crn;                /* GlobalCAll API call handle */
    int     blocked;            /* channel blocked/unblocked */
    // See cbansr.c for other resources
} port[MAXCHANS + 1];



char linedti[ 32 ];
bool cutthrough[ MAXCHANS ];

char    tmpbuff[ 256 ];     /* Temporary Buffer */
static GC_IE_BLK raw_info_elements[MAXCHANS + 1];
static IE_BLK info_elements[MAXCHANS + 1];
static long requestinfo[MAXCHANS]; //an array of longs (in theory)

// Declaration of used functions

void disp_msgf(const char *fmt, ...);
bool addtoconf(short channum, unsigned char confnum);
void disp_msg(char *string);
short get_linechan(int linedev);
char causelog(short channum, int cause);
int set_hkstate(short channum, int state);
void disp_status(short chnum, char *stringp);
void disp_err(int chnum, int chfd, int state);
int transaction_rec(short channum1, short channum2, int filedesc);
int isdn_open(int maxchan);
int playtone_cad(short channum, int toneval, int toneval2, int time);
int isdn_trunkhunt(short channum);
bool dropfromconf(short channum, unsigned char confnum);
int isdn_answer(short channum);
char isdn_inroute(short channum);
void tl_reroute(short channum);
char dialer_next(short channum);
int record(short channum, int filedesc, int format, unsigned short termmsk);
int endwin(void);
char set_cpn(short channum, char *number, unsigned char plan_type, unsigned char screen_pres);
char isdn_drop(short channum, int cause);
bool routed_cleanup(short channum);
int get_digs(short channum, DV_DIGIT *digbufp, unsigned short numdigs, unsigned short intermax, short termflags);
char set_bearer (short channum, char bearer);
char progress_writesig(unsigned char offset, short channum);
int playtone_rep(short channum, int toneval, int toneval2, int amp1, int amp2, int ontime, int pausetime);
void outcall_inroute(short channum);

void config_successhdlr() {
    int ldev = sr_getevtdev();
    sprintf(tmpbuff, "Configuration change on device %d made successfully", ldev);
    disp_msg(tmpbuff);
    return;
}
/*******************************************************
           "Handle" a cause code in dialer mode
*******************************************************/
void causehandle( unsigned char value, short channum ) {
    causelog(channum, (int) value | 0x200);
    dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
    set_hkstate(channum, DX_ONHOOK);
    return;
}

int gc_errprint(char *function, int channel, int callstate) {
    disp_msg("Performing gc_errprint");
    FILE  *errfd;
    char errfile[MAXCHANS + 1];
    gc_ErrorInfo(&gc_error_info);
    disp_msg("Opening error file");
    sprintf(errfile, "isdn_error.txt");
    errfd = fopen(errfile, "a+");
    sprintf(tmpbuff, "%s - Printing to error file, fd %i", gc_error_info.gcMsg, (int)errfd);
    disp_msg(tmpbuff);
    fprintf(errfd, "%s() GC ErrorValue: 0x%hx - %s, CC ErrorValue: 0x%lx - %s, additional info: %s, channel %d, call state 0x%hx, program state %d, CRN %ld, ldev %li\n", function, gc_error_info.gcValue, gc_error_info.gcMsg, gc_error_info.ccValue, gc_error_info.ccMsg, gc_error_info.additionalInfo, channel, callstate, dxinfox[channel].state, port[channel].crn, port[channel].ldev);
    disp_msg("Closing error file");
    fclose(errfd);
    return (gc_error_info.gcValue);
}

bool isdn_mediahdlr() {
    disp_msg("ISDN Media event detected");

    int ldev = sr_getevtdev(); // Backup event device deriving
    // Write general failure to wardialer result log
    char conntype;
    short  channum = get_linechan(ldev);
    gc_GetCallInfo(port[channum].crn, CONNECT_TYPE, &conntype);

    causelog(channum, conntype);

    if ((conntype != GCCT_PAMD) && (conntype != GCCT_FAX1) && (conntype != GCCT_FAX2)) {
        // At some point, I'd like to make this more efficient; there shouldn't be so many checks hamfisted in.

        dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
        set_hkstate(channum, DX_ONHOOK);

    }

    return (TRUE);

}

bool check_crn(short channum) {
    // gc_GetNetCRV() is supposed to be deprecated, but they don't offer a replacement for the DM3. WHICH IS IT!? D:
    //GC_PARM_BLKP parm_blkp = NULL, ret_blkp = NULL;
    //GC_PARM_DATAP parm_datap;
    int crn;

    // if ( dm3board == TRUE ) {
    if (gc_GetNetCRV(port[channum].crn, &crn) != GC_SUCCESS) {
        disp_msg("check_crn operation failed!");
        gc_errprint("gc_GetNetCRV", channum, -1);
        return (FALSE);
    }

    sprintf(tmpbuff, "NetCRV for channel %d is %d", channum, crn);
    disp_msg(tmpbuff);
    return (TRUE);
    // }
    /*
    gc_util_insert_parm_ref( &parm_blkp, GCIS_SET_GENERIC, GCIS_PARM_NETCRV, sizeof(int), 0 );

    if ( gc_Extension( GCTGT_GCLIB_CRN, port[channum].crn, GCIS_EXID_GETNETCRV, parm_blkp, &ret_blkp, EV_SYNC ) != GC_SUCCESS ) {
        disp_msg("check_crn operation failed!");
        gc_errprint("gc_Extension_checkcrn", channum, -1 );
        return(FALSE);
    }

    // Get the first parm from the data block
    parm_datap = gc_util_next_parm(parm_blkp, NULL);

    //Get the NetCRV from the parm data
    // *crn = parm_datap->value_buf;
    //memcpy(crn, parm_datap->value_buf, sizeof(int));
    sprintf( tmpbuff, "NetCRV for channel %d is %d", channum, *parm_datap->value_buf );
    disp_msg( tmpbuff );
    gc_util_delete_parm_blk(parm_blkp);
    return(TRUE);
    */
}

bool set_corruptprog(short channum, char msgtype) {
    GC_IE_BLK gc_corrupt_ie;
    IE_BLK corrupt_ie;
    GC_PARM_BLKP progressp = NULL;
    GC_PARM_BLKP returnblkp = NULL;
    int progval = IN_BAND_INFO;

    if (msgtype == 1) {
        corrupt_ie.length = 33;
        memcpy(corrupt_ie.data, "\x28\x1F\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21\x21", corrupt_ie.length); // Set corrupt display IE data as the payload
    } else {
        corrupt_ie.length = 3;
        memset(corrupt_ie.data, 0, 3);  // Change 53 to F4
        memcpy(corrupt_ie.data, "\x28\xF1\x21", corrupt_ie.length); // Set corrupt display IE data as the payload
    }

    gc_corrupt_ie.gclib = NULL;
    gc_corrupt_ie.cclib = &corrupt_ie;

    if (gc_SetInfoElem(port[channum].ldev, &gc_corrupt_ie) != GC_SUCCESS) {
        disp_msg("gc_SetInfoElem returned an error! D:");
        gc_errprint("gc_SetInfoElem", channum, -1);
        return (FALSE);
    }

    gc_util_insert_parm_ref(&progressp, GCIS_SET_CALLPROGRESS, GCIS_PARM_CALLPROGRESS_INDICATOR, sizeof(int), &progval);

    if (progressp == NULL) {
        disp_msg("Memory allocation error in set_corruptprog");
        return (FALSE);
    }

    if (gc_Extension(GCTGT_GCLIB_CRN, port[channum].crn, GCIS_EXID_CALLPROGRESS, progressp, &returnblkp, EV_SYNC) != GC_SUCCESS) {
        disp_msg("set_corruptprog operation failed!");
        gc_errprint("gc_Extension_corruptprog", channum, -1);
        return (FALSE);
    }

    disp_msg("Corrupt progress message sent!");
    return (TRUE);
}

bool set_corruptie(short channum, char type) {
    GC_IE_BLK gc_corruptcpn_ie;
    IE_BLK corruptcpn_ie;
    unsigned char payload[67];
    memset(payload, 0, 67);

    switch (type) {

        case 1:
            memcpy(&payload, "\x6C\x14\x80", 3);   // Calling party number
            // unsigned char payload[] = { 0x6c, 0x14, 0xA1 }; // Calling party number
            // 0x6C indicates calling party number IE, 0xDB specifies a length of 219 octets (anything larger, and called party numbers over four digits overflow on the NEC),  0xA1 specifies national number, ISDN/telephony numbering plan
            corruptcpn_ie.length = 3;
            break;

        case 2:
            memcpy(&payload, "\x28\x22\x00", 3);
            //unsigned char payload[] = { 0x28, 0x22, 0x00 }; //Display IE test
            corruptcpn_ie.length = 3;

            if (set_cpn(channum, "3115552368", (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
                disp_msg("set_cpn returned an error!");
                return (FALSE);
            }

            break;

        case 3:
            // Pay no attention to the base-16 11 value; that's irrelevant to the buffer length specifier.
            memcpy(&payload, "\x70\x11\x80\x31\x37\x2A\x32\x32\x31\x31\x33", 11);
            // unsigned char payload[] = { 0x70, 0x0F, 0x80, 0x37, 0x31, 0x31, 0x31, 0x38, 0x39 }; // Called party number IE test
            corruptcpn_ie.length = 11;

            if (set_cpn(channum, "3115552369", (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
                disp_msg("set_cpn returned an error!");
                return (FALSE);
            }

            break;

        case 4:
            memcpy(&payload, "\x70\x41\x80\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36\x36", 67);
            // unsigned char payload[] = { 0x70, 0x41, 0x80, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36 }; // Called party number overflow test
            corruptcpn_ie.length = 67;

            if (set_cpn(channum, "3115552370", (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
                disp_msg("set_cpn returned an error!");
                return (FALSE);
            }

            break;

        case 5:
            memcpy(&payload, "\x7E\x0C\x42\x4F\x57\x4C\x4F\x46\x48\x41\x54\x53\x53\x53", 14);
            //unsigned char payload[] = { 0x7E, 0x80, 0x00 }; // User-to-user IE test
            corruptcpn_ie.length = 14;

            if (set_cpn(channum, "3115552371", (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
                disp_msg("set_cpn returned an error!");
                return (FALSE);
            }

            break;

        default:
            disp_msg("Incorrect value specified to set_corruptie. Continuing without corrupt element...");

            if (set_cpn(channum, "3115552372", (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
                disp_msg("set_cpn returned an error!");
                return (FALSE);
            }

            return (FALSE);
    }

    gc_corruptcpn_ie.gclib = NULL;
    gc_corruptcpn_ie.cclib = &corruptcpn_ie;
    memcpy(corruptcpn_ie.data, payload, corruptcpn_ie.length);

    if (gc_SetInfoElem(port[channum].ldev, &gc_corruptcpn_ie) != GC_SUCCESS) {
        gc_errprint("gc_SetInfoElem", channum, -1);
        return (FALSE);
    }

    disp_msg("gc_setcorruptIE OK!");
    return (TRUE);
}



char glare_hdlr() {
    int ldev = sr_getevtdev();
    short channum = get_linechan(ldev);
    sprintf(tmpbuff, "WARNING: Glare detected on channel %d", channum);
    disp_msg(tmpbuff);
    return (0);
}

/***********************************************************
*   Functionized call cleanup task for ST_ROUTED and ROUTED2
*   ***********************************************************/
bool routed_cleanup( short channum ) {
    dx_stopch(dxinfox[ connchan[channum] ].chdev, EV_ASYNC);
    dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
    disp_msg("Unrouting channels from voice devices...");

    if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
        disp_msg("Uhh, I hate to break it to you man, but SCUnroute1 is shitting itself");
        disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
        return (-1);
    }

    if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
        disp_msg("Holy shit! SCUnroute2 threw an error!");
        disp_err(connchan[channum], dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
        return (-1);
    }

    if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
        disp_msg("Holy shit! SCroute threw an error!");
        disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
        return (-1);
    }

    if (dxinfox[ connchan[channum] ].state != ST_ROUTEDREC) {
        dxinfox[ connchan[channum] ].state = ST_ONHOOK;
    }

    cutthrough[ channum ] = 0;
    dxinfox[ channum ].state = ST_ONHOOK;
    dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
    set_hkstate(channum, DX_ONHOOK);
    return (0);

}


/***************************************************************************
 *        NAME: char randomcpn()
 * DESCRIPTION: Function that generates a random calling party number. To
 *              simplify, this only generates phone numbers in old (pre-95)
 *              area codes, and in non-0xx/1xx exchanges.
 *       INPUT: short channum (channel to set with random CPN)
 *      OUTPUT: None.
 *     RETURNS: 0 on success, -1 on failure.
 *    CAUTIONS: None.
 **************************************************************************/

char randomcpn(short channum) {
    char cpn[11]; // Does this start with zero or one?
    cpn[0] = (random_at_most(8) + 0x32);
    cpn[1] = (random_at_most(1) + 0x30);
    cpn[2] = (random_at_most(8) + 0x32);
    cpn[3] = (random_at_most(8) + 0x32);
    cpn[4] = (random_at_most(10) + 0x30);
    cpn[5] = (random_at_most(10) + 0x30);
    cpn[6] = (random_at_most(10) + 0x30);
    cpn[7] = (random_at_most(10) + 0x30);
    cpn[8] = (random_at_most(10) + 0x30);
    cpn[9] = (random_at_most(10) + 0x30);
    cpn[10] = '\0';

    if (set_cpn(channum, cpn, (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
        disp_msg("set_cpn returned an error!");
        return (-1);
    }

    return (0);
}

/***********************************************************
            Turn DM3 call progress analysis on
***********************************************************/
bool callprog(short channum, bool setting) {
    GC_PARM_BLK       *progblkp = NULL;

    // Setting this shit on DM3s (as is pretty much everything else on it) is sort of a pain in the ass.

    if (setting) {
        // Turn on CPA, set PAMD detection to accurate

        if (gc_util_insert_parm_val(&progblkp, CCSET_CALLANALYSIS, CCPARM_CA_MODE,
                                    sizeof(int), GC_CA_ENABLE_ALL) != GC_SUCCESS) {
            sprintf(tmpbuff, "Insertion failed - CCPARM_CA_MODE, channel %d", channum);
            disp_msg(tmpbuff);
            gc_errprint("gc_util_insert_parm_val", 0, -1);
            return (-1);
        }

        if (gc_util_insert_parm_val(&progblkp, CCSET_CALLANALYSIS, CCPARM_CA_PAMDSPDVAL,
                                    sizeof(int), PAMD_ACCU) != GC_SUCCESS) {
            sprintf(tmpbuff, "Insertion failed - CCPARM_CA_PAMDSPVAL, channel %d", channum);
            disp_msg(tmpbuff);
            gc_errprint("gc_util_insert_parm_val", 0, -1);
            return (-1);
        }

    } else {
        // Turn off CPA
        // disp_msg("Turning CPA *off*");

        if (gc_util_insert_parm_val(&progblkp, CCSET_CALLANALYSIS, CCPARM_CA_MODE,
                                    sizeof(int), GC_CA_DISABLE) != GC_SUCCESS) {
            sprintf(tmpbuff, "Insertion failed - CCPARM_CA_MODE, channel %d", channum);
            disp_msg(tmpbuff);
            gc_errprint("gc_util_insert_parm_val", 0, -1);
            return (-1);
        }

    }

    if (gc_SetConfigData(GCTGT_CCLIB_CHAN, port[ channum ].ldev, progblkp, 0, GCUPDATE_IMMEDIATE, &requestinfo[channum], EV_ASYNC) != GC_SUCCESS) {
        disp_msg("gc_SetConfigData failed!");
        gc_errprint("gc_SetConfigData", channum, -1);
        return (-1);
    }

    disp_msg("callprog() terminated successfully!");
    gc_util_delete_parm_blk(progblkp);
    progblkp = NULL;
    return (0);

}
bool callprogAndrew(short channum, bool setting) {
    GC_PARM gc_parm;
    gc_parm.shortvalue = GCPV_ENABLE;

    if (gc_SetParm(port[channum].ldev, GCPR_MEDIADETECT, gc_parm) == -1) {
        /* process error return as shown */
        disp_msg("gc_SetParm MEDIADETECT function returned an error!");
        gc_errprint("gc_SetParm", channum, -1);
        return (-1);
    } else {
        sprintf(tmpbuff, "Enabled MEDIADETECT on %d", channum);
        disp_msg(tmpbuff);
    }

    if (gc_SetParm(port[channum].ldev, GCPR_CALLPROGRESS, gc_parm) == -1) {
        /* process error return as shown */
        disp_msg("gc_SetParm CALLPROGRESS function returned an error!");
        gc_errprint("gc_SetParm", channum, -1);
        return (-1);
    } else {
        sprintf(tmpbuff, "Enabled CALLPROGRESS on %d", channum);
        disp_msg(tmpbuff);
    }


    return (0);


    GC_PARM_BLKP       progblkp = NULL;

    // Setting this shit on DM3s (as is pretty much everything else on it) is sort of a pain in the ass.

    if (setting == true) {
        // Turn on CPA, set PAMD detection to accurate

        if (gc_util_insert_parm_val(&progblkp, CCSET_CALLANALYSIS, CCPARM_CA_MODE,
                                    sizeof(unsigned char), GC_CA_ENABLE_ALL) != GC_SUCCESS) {
            sprintf(tmpbuff, "Insertion failed - CCPARM_CA_MODE, channel %d", channum);
            disp_msg(tmpbuff);
            gc_errprint("gc_util_insert_parm_val", 0, -1);
            return (-1);
        }

        if (gc_util_insert_parm_val(&progblkp, CCSET_CALLANALYSIS, CCPARM_CA_PAMDSPDVAL,
                                    sizeof(unsigned char), PAMD_ACCU) != GC_SUCCESS) {
            sprintf(tmpbuff, "Insertion failed - CCPARM_CA_PAMDSPVAL, channel %d", channum);
            disp_msg(tmpbuff);
            gc_errprint("gc_util_insert_parm_val", 0, -1);
            return (-1);
        }

    }

    else {
        // Turn off CPA
        disp_msg("Turning CPA *off*");

        if (gc_util_insert_parm_val(&progblkp, CCSET_CALLANALYSIS, CCPARM_CA_MODE,
                                    sizeof(unsigned char), GC_CA_DISABLE) != GC_SUCCESS) {
            sprintf(tmpbuff, "Insertion failed - CCPARM_CA_MODE, channel %d", channum);
            disp_msg(tmpbuff);
            gc_errprint("gc_util_insert_parm_val", 0, -1);
            return (-1);
        }

    }

    if (gc_SetConfigData(GCTGT_CCLIB_CHAN, port[ channum ].ldev, progblkp, 0, GCUPDATE_IMMEDIATE, &requestinfo[channum], EV_SYNC) != GC_SUCCESS) {
        disp_msg("gc_SetConfigData failed!");
        gc_errprint("gc_SetConfigData", channum, -1);
        return (-1);
    }

    disp_msg("callprog() terminated successfully!");
    return (0);

}

/***********************************************************
       Set RDNIS information for the requested channel
***********************************************************/
char set_rdnis(short channum, char *number, char reason) {
    GC_IE_BLK gc_rdnis_ie;
    IE_BLK rdnis_ie;

    gc_rdnis_ie.gclib = NULL;
    gc_rdnis_ie.cclib = &rdnis_ie;

    rdnis_ie.length = (strlen(number) + 5);   // Three fields, the number, the length indicator, and the IE identifier

    if ((rdnis_ie.length < 1) || (rdnis_ie.length > MAXLEN_IEDATA)) {
        return (-1);    // No overflows plz
    }

    sprintf(rdnis_ie.data, "%c%c%c%c%c%s", 0x74, (rdnis_ie.length - 2), 33, 3, reason, number);
    // 0x74, 0x0D (for 10-digit numbers), 0x21, 0x03, 0x01, number

    if (gc_SetInfoElem(port[channum].ldev, &gc_rdnis_ie) != GC_SUCCESS) {
        gc_errprint("gc_SetInfoElem", channum, -1);
        return (-1);
    }

    sprintf(tmpbuff, "gc_SetInfoElem OK! %s", rdnis_ie.data);
    disp_msg(tmpbuff);
    return (0);
}

char set_bearer (short channum, char bearer) {

GC_IE_BLK gc_bearer_ie;
IE_BLK bearer_ie;

gc_bearer_ie.gclib = NULL;
gc_bearer_ie.cclib = &bearer_ie;

bearer_ie.length = 4;

bearer_ie.data[0] = 0x04;
bearer_ie.data[1] = 2; // This should be something different. Change plz.
bearer_ie.data[2] = bearer;
bearer_ie.data[3] = 0x90; // For the moment, let's keep this hardcoded to 64 kbps/circuit mode.
//bearer_ie.data[4] = 0xA2; // Honestly? Not sure.

    if (gc_SetInfoElem(port[channum].ldev, &gc_bearer_ie) != GC_SUCCESS) {
        gc_errprint("gc_SetInfoElem_set_bearer", channum, -1);
        return (-1);
    }

    sprintf(tmpbuff, "gc_SetInfoElem OK! %s", bearer_ie.data);
    disp_msg(tmpbuff);
    return (0);
}

/***********************************************************
     Set calling party number with non-standard fields
***********************************************************/
char set_cpn(short channum, char *number, unsigned char plan_type, unsigned char screen_pres) {
    // At this point, we've replaced all the standard Dialogic API calls for CPN with this; they won't inter-operate between DM3/JCT, so fuck those losers.
    // LSH is pre-performed to get plan_type - just OR the two values. For example, plantype = (ISDN | NATIONAL)
    // Same for screen_pres - do, for example, ( PRES_RESTRICT | NETSCREEN )
    GC_IE_BLK gc_cpn_ie;
    IE_BLK cpn_ie;

    gc_cpn_ie.gclib = NULL;
    gc_cpn_ie.cclib = &cpn_ie;

    // First is 0x6C, then length, then plan_type, then presentation/screening string, then CPN string. See spec page 84.
    cpn_ie.length = strlen(number); // Don't compensate for extra fields... yet.

    // cpn_ie.length = ( strlen(number) + 4 ); // Three fields, the number, the length indicator, and the IE identifier
    if ((cpn_ie.length < 1) || (cpn_ie.length > 19)) {
        disp_msg("Malformed CPN inputted: over 23 characters. No CPN sent!");
        return (-1); // No overflows plz. CPN IE max is 23 octets.
    }

    cpn_ie.data[0] = 0x6c;
    cpn_ie.data[1] = (cpn_ie.length + 2);
    cpn_ie.data[2] = plan_type;
    cpn_ie.data[3] = screen_pres;
    memcpy(&cpn_ie.data[4], number, cpn_ie.length);
    cpn_ie.length = (cpn_ie.length + 4); // Set proper length here. This avoids the issue of doing excess arithmetic

    //sprintf( cpn_ie.data, "%c%c%c%c%s", 0x6C, ( cpn_ie.length - 2 ), plan_type, screen_pres, number ); // Can we make this a memcpy() op instead?
    //memcpy( &cpn_ie.data, "\x6C\x0C\x21\xA3\x32\x30\x32\x34\x38\x34\x30\x30\x30\x30", 14); // For testing...
    if (gc_SetInfoElem(port[channum].ldev, &gc_cpn_ie) != GC_SUCCESS) {
        gc_errprint("gc_SetInfoElem_set_cpn", channum, -1);
        return (-1);
    }

    sprintf(tmpbuff, "gc_SetInfoElem OK! %s", cpn_ie.data);
    disp_msg(tmpbuff);
    return (0);
}

/***********************************************************
       Set calling party name for a requested channel
***********************************************************/
char set_cpname(short channum, char *name, char *callingnum) {
    GC_IE_BLK gc_cpname_ie;
    IE_BLK cpname_ie;

    gc_cpname_ie.gclib = NULL;
    gc_cpname_ie.cclib = &cpname_ie;
    cpname_ie.length = strlen(name);

    if (cpname_ie.length > 27) {
        // Check length
        //name[15] = '\0'; // This can segfault, so it was removed in favor of snprintf
        //cpname_ie.length = 16; // Fifteen characters plus two for the IE identifier and length indicator
        cpname_ie.length = 28; // Fifteen characters plus two for the IE identifier and length indicator
    }

    else {
        (cpname_ie.length = (cpname_ie.length + 2));
    }
    // The ISDN stack wants CPN *before* display IE, so let's give the switches what they want.
    if (set_cpn(channum, callingnum, (PRIVATE | SUBSCRIBER), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
        disp_msg("set_cpn returned an error!");
        return -1;
    }
    snprintf(cpname_ie.data, 29, "%c%c%s", 0x28, (cpname_ie.length - 2), name);
    //sprintf(cpname_ie.data, "%c%c%s", 0x28, (cpname_ie.length - 2), name);

    if (gc_SetInfoElem(port[channum].ldev, &gc_cpname_ie) != GC_SUCCESS) {
        gc_errprint("gc_SetInfoElem", channum, -1);
        return (-1);
    }

    sprintf(tmpbuff, "gc_SetInfoElem OK! %s", cpname_ie.data);
    disp_msg(tmpbuff);
    return (0);
}




/***********************************************************
             Identify a line device for an event
***********************************************************/
short get_linechan(int linedev) {
    short linenum = 1;

    // This is going to bug me; it's crude and really should
    // be a simple translation table instead of this bruteforce
    // garbage.

    while (linenum <= maxchans) {
        if (port[linenum].ldev == linedev)  {
            return (linenum);
        }

        linenum++;
    }

    /*
     * Not Found in the Array, print error and return -1
     */
    sprintf(tmpbuff, "Unknown Event for Line %d - Ignored", linedev);
    disp_msg(tmpbuff);

    return (-1);
}

/***********************************************************
         Outgoing Call Connection - Answer message
***********************************************************/
char isdn_connecthdlr() {
    int ldev = sr_getevtdev();
    char conntype;
    int callstate; // Can we allocate this elsewhere? We need it for like, 1% of the calls hitting this function
    short channum = get_linechan(ldev);
    isdnstatus[ channum ] = 2; // Indicate outgoing call has suped to application

    sprintf(tmpbuff, "Outgoing call answered on channel %d", channum);
    disp_msg(tmpbuff);
    disp_status(channum, "Outgoing call - Answered");

    if (dxinfox[ channum ].state == ST_CALLPTEST5) {
        if (gc_GetCallInfo(port[channum].crn, CONNECT_TYPE, &conntype) != GC_SUCCESS) {
            gc_errprint("gc_GetCallInfo", channum, -1);
        }

        if (conntype == GCCT_INPROGRESS) {
            return (0);
        }

        if ((conntype != GCCT_PAMD) && (conntype != GCCT_FAX1) && (conntype != GCCT_FAX2)) {      // 0x6 means media detection in progress. 0x4 means extended PAMD detection/ 0x9|0x0A means modem detection, will handle hangup. Either way, don't touch it.
            causelog(channum, conntype);
            dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
            set_hkstate(channum, DX_ONHOOK);
        }

        return (0);
    }

    if (dxinfox[ channum ].state == ST_OUTCALLTEST) {
        outcall_inroute(channum);
        return 0;
    }

    if (dxinfox[ channum ].state == ST_CONFOUT) {
        addtoconf(channum, 0); // For now, let's just assume the conference we're adding it to is the general one
        return 0;
    }

    if (cutthrough[ channum ] == 0) {
        // Don't run this code on wardial calls.

        if ((dxinfox[ channum ].state != ST_EVANS1) && (dxinfox[ channum ].state != ST_2600ROUTE2) && (dxinfox[connchan[ channum ]].state != ST_ROUTEDTEST)) {
            // I forget what this Gregory Evans stuff is (early application test?), but we'll need the DSP to listen for 2600 hertz if it's something that's supposed to do so.

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

        }
        if (dxinfox[connchan[ channum ]].state == ST_ROUTEDTEST) get_digs(connchan[channum], &dxinfox[ connchan[ channum ]].digbuf, 0, 0, 0x100F);

        if (dxinfox[ channum ].state == ST_ROUTEDREC2) {

            // Make a listen only connection between the two timeslots, and have the DSPs clamped in in a listen-only configuration.
            /*
            if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_HALFDUP) == -1) {
                disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }
            */
            //For now, this is full duplex
            if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }


            if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_HALFDUP) == -1) {
                disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_HALFDUP) == -1) {
                disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                disp_err(channum, dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                return (-1);
            }

            record(channum, dxinfox[ channum ].msg_fd, 1, DM_C);

        }

        else {
            if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }
        }

        if (dxinfox[ channum ].state == ST_ROUTEDREC) {
            if ((transaction_rec(channum, connchan[ channum ], dxinfox[channum].msg_fd)) == -1) {
                disp_status(channum, "Transaction recording failed!");
                // Should we have any other error handling? I think transaction_rec handles most of that.
            }
        }

        cutthrough[channum] = 1;

    }

    if ((dxinfox[ connchan[ channum ] ].state == ST_ROUTED) || (dxinfox[ connchan[ channum ] ].state == ST_ROUTEDTEST)) {
        gc_GetCallState(port[connchan[ channum ]].crn, &callstate);

        if (callstate != GCST_CONNECTED) {
            set_hkstate(connchan[ channum ], DX_OFFHOOK);    // Pass supervision to the originating caller
        }
    }

    if (dxinfox[ connchan[ channum ] ].state == ST_2600ROUTE) {
        gc_GetCallState(port[connchan[ channum ]].crn, &callstate);

        if (callstate != GCST_CONNECTED) {
            isdn_answer(connchan[channum]);    // Pass supervision to the originating caller
        }
    }

    return (0);
}

/***********************************************************
                    Trunk hunt operation
***********************************************************/
int isdn_trunkhunt(short channum) {
    int callstate = 1; // GCST_NULL is zero, so let's define this as 1 for the while loop.

    if (channum < 0) {
        return (-1);    // Channum is less than zero? We can't have this.
    }

    while (callstate != GCST_NULL) {

        channum++;

        if (gc_GetCallState(port[channum].crn, &callstate) != SUCCESS) {
            if (port[channum].crn == '\0') {
                return (channum);
            }

            sprintf(tmpbuff, "Invalid CRN is %li", port[channum].crn);
            disp_msg(tmpbuff);
            gc_errprint("gc_GetCallState_th", channum, callstate);
            return (-1);
        }

    }

    return (channum);

}

/***********************************************************
         Outgoing Call Connection - Progress message
***********************************************************/
char isdn_progressing() {
    // At the moment, Alerting events are coming here too. If both come back, is it going to cause problems with the timeslot routing functions?
    // gc_MakeCall() was successful. Let's bridge it to the originating channel.

    disp_msg("Entering call progress handler");
    char retcode, offset = 0;
    METAEVENT            metaevent;
    
    if (gc_GetMetaEvent(&metaevent) != GC_SUCCESS) {
        disp_msg("gc_GetMetaEvent failed. It really shouldn't.");
        return (-1);
    }

    int ldev = metaevent.evtdev; // This'll be more efficient; no need to call the same function twice.
    short  channum = get_linechan(ldev);
    port[channum].crn = metaevent.crn;

    if (gc_GetSigInfo(ldev, (char *)&info_elements[channum], U_IES, &metaevent) != GC_SUCCESS) {
        disp_msg("gc_GetSigInfo_prog error!");
        gc_errprint("gc_GetSigInfo_prog", channum, -1);
        return (-1);
    }
        
    if (q931debug == 1) {
        FILE  *iefd;
        char iefile[180];
        sprintf(iefile, "%s-%i-%ld_progress.dump", isdninfo[channum].dnis, channum, port[channum].crn);
        iefd = fopen(iefile, "a");
        // fprintf( iefd, "%s", info_elements[channum].data );
        // fwrite(info_elements[channum].data, sizeof(char), info_elements[channum].length, iefd);
        fwrite(&info_elements[channum], 1, sizeof(info_elements[channum]), iefd); // This should return size of IE dump in first two bytes, little endian format
        fclose(iefd);
    }


    disp_msg("Connecting outgoing call");
    sprintf(tmpbuff, "Outgoing call - Progress, state %d", dxinfox[ channum ].state);
    disp_status(channum, tmpbuff);

    // Perform cut-through
    if (dxinfox[channum].state == ST_OUTCALLTEST) return 0; // Stop the software from trying to bridge calls to non-existent channels

    if (!((dxinfox[ channum ].state == ST_CALLPTEST4) || (dxinfox[ channum ].state == ST_CALLPTEST5))) {
        // Instead of performing timeslot routing operations, activate call progress detection for dialer mode

            if (cutthrough[ channum ] == 0) {

                if ((dxinfox[ channum ].state != ST_EVANS1) && (dxinfox[ channum ].state != ST_2600ROUTE2) && (dxinfox[ connchan[channum] ].state != ST_ROUTEDTEST)) {
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

                }
                else if (dxinfox[connchan[ channum ]].state == ST_ROUTEDTEST) get_digs(connchan[channum], &dxinfox[ connchan[ channum ]].digbuf, 0, 0, 0x100F);

            if (dxinfox[ channum ].state == ST_ROUTEDREC2) {

                // Make a listen only connection between the two timeslots, and have the DSPs clamped in in a listen-only configuration.
                // this is full duplex for now
                if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                    disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                    disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                    return (-1);
                }

                if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ channum ].chdev, SC_VOX, SC_HALFDUP) == -1) {
                    disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                    disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                    return (-1);
                }

                if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_HALFDUP) == -1) {
                    disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                    disp_err(channum, dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                    return (-1);
                }

                record(channum, dxinfox[ channum ].msg_fd, 1, DM_C);

            } else {
                if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                    disp_msg("Uhh, I hate to break it to you man, but SCRoute is shitting itself");
                    disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                    return (-1);
                }
            }

            if (dxinfox[ channum ].state == ST_ROUTEDREC) {
                // Should we be using [connchan[ chnannum ] ] and ST_ROUTEDTREC2 instead? Also, do we need two ST_ROUTEDREC states?
                if ((transaction_rec(channum, connchan[ channum ], dxinfox[channum].msg_fd)) == -1) {
                    disp_status(channum, "Transaction recording failed!");
                    // Should we have any other error handling? I think transaction_rec handles most of that.
                }
            }

            cutthrough[channum] = 1;

        }
    
    }
    
    retcode = progress_writesig(offset, channum);
    offset = retcode;

    while (offset > 0) {
        if (offset >= info_elements[channum].length) break; // Make sure we don't extend past the (supposed) length of the buffer
        retcode = progress_writesig(offset, channum);
        offset = retcode;
        sprintf(tmpbuff, "Offset is %u", offset);
        disp_msg(tmpbuff);
    }

    return (0);

}

/***********************************************************
      Outgoing Call Status (timeout/no answer) Handler)
***********************************************************/

char isdn_callstatushdlr() {
    int  ldev = sr_getevtdev();
    short channum = get_linechan(ldev);
    sprintf(tmpbuff, "Outgoing call timed out or didn't answer on chnanel %d", channum);
    disp_msg(tmpbuff);

    return (0);
}

/***********************************************************
                        Make call
***********************************************************/
char makecall(short channum, char *destination, char *callingnum, char rec) {

    // To do: Make calling number passing possible by sending 0 or something
    //GC_MAKECALL_BLK block;
    //block.gclib = &gclib_makecallp;

    int callstate;

    if (dxinfox[ channum ].state == ST_OUTCALLTEST) {
        cutthrough[channum] = 1; // Never try to cut an unattached DSP object through
    }


    else if ((dxinfox[ channum ].state != ST_CALLPTEST4) &&
            (dxinfox[ channum ].state != ST_CALLPTEST5) &&  // && <cr> and add more states here if needed
            (dxinfox[ channum ].state != ST_2600ROUTE2)
       ) {
        if (rec == NOREC) {
            dxinfox[ channum ].state = ST_ROUTED2;    // Mark this call as being used for bridging to an existing channel
        } else if (rec == REC) {
            dxinfox[ channum ].state = ST_ROUTEDREC;
        } else if (rec == ONEWAY) {
            dxinfox[ channum ].state = ST_ROUTEDREC2;
        }
    }

    //else set_bearer( channum, UNRESTRICTED ); // For testing

    switch (callingnum[0]) {

        case 0x00:
            // Nothing there? Don't bother then.
            break;

        case 0x72: // r
            randomcpn(channum);
            break;

        case 0x70: // p
            set_cpn(channum, (callingnum + 1), (ISDN | NATIONAL), (PRES_RESTRICT | NETSCREEN));
            break;

        case 0x21:
            set_corruptie(channum, 1);   // !
            break;

        case 0x40:
            set_corruptie(channum, 2);   // @
            break;

        case 0x23:
            set_corruptie(channum, 3);   // #
            destination[0] = '\0'; // A call to a function was made to write an invalid destination. We shouldn't have two.
            break;
        case 0x24:
            set_corruptie(channum, 4);   // $
            destination[0] = '\0'; // A call to a function was made to write an invalid destination. We shouldn't have two.
            break;
        case 0x25:
            set_corruptie(channum, 5);   // %
            break;
        case 0x26:
            set_corruptie(channum, 6);   // &
            break;
        default:
            // This is a normal calling party number. Let's just set it, well, normally.

            if (set_cpn(channum, callingnum, (ISDN | NATIONAL), (PRES_ALLOW | NETSCREEN)) != GC_SUCCESS) {
                disp_msg("set_cpn returned an error!");
                return (-1);
            }
    }

    sprintf(tmpbuff, "Finished setting CPN, originating %s on channel %d with CPN %s", destination, channum, callingnum);
    disp_msg(tmpbuff);

    // Only DM3 cards allow a call timeout in asynchronous mode, so keep call timeout to 0 for interoperability

    sprintf(tmpbuff, "destination in makecall is %s", destination);
    disp_msg(tmpbuff);
    // set_rdnis( channum, "2127365000", 0x81);
    gc_GetCallState(port[channum].crn, &callstate);

    if (gc_MakeCall(port[channum].ldev, &port[channum].crn, destination, NULL, 0, EV_ASYNC) != GC_SUCCESS) {
        disp_msg("gc_MakeCall function returned an error!");
        gc_errprint("gc_MakeCall", channum, callstate);
        return (-1);
    }

    isdnstatus[channum] = 0; // Set ISDN status to in-progress/outgoing call if gc_MakeCall succeeds
    disp_status(channum, "Originating ISDN call...");

    return (0);

}

/***********************************************************
               Call Acknowledgement/More Info
***********************************************************/

//char isdn_infohdlr()

//{
//   int  ldev = sr_getevtdev();
// int  event = sr_getevttype();
//   int  channum = get_linechan(ldev);
// Acknowledge an ISDN call, answer it

//   if (gc_GetCallInfo( port[channum].crn, DESTINATION_ADDRESS, dnis[channum]) != GC_SUCCESS) {
//      disp_msg("gc_GetCallInfo failed");
//      gc_errprint("gc_GetCallInfo");
//      return(-1);
//   }

//   sprintf( tmpbuff, "Incoming dest. is %s", dnis[channum] );
//   disp_msg(tmpbuff);

//   dxinfox[ channum ].state = ST_DIGPROC;
//   set_hkstate( channum, DX_OFFHOOK);
//   return(0);

// }

/***********************************************************
                   Task Failure Handler
***********************************************************/
char isdn_failhdlr() {
    METAEVENT failevent;
    GC_INFO failinfo;
    int errnum;
    errnum = gc_ResultInfo( &failevent, &failinfo );
    if (errnum != 0) {
        int  ldev = sr_getevtdev();
        short  channum = get_linechan(ldev);
        disp_msgf("CRITICAL ERROR %d: Cannot get info for ISDN task failure! Channel will remain out of service", errnum);
        disp_msgf( "DEBUG: ISDN task failure occurred for channel %d, connected to channel %d", channum, connchan[channum]);

        // This was code from the SSS-2000 project. Is it appropriate here?
        if( ( connchan[channum] != 0) && (connchan[channum] < MAXCHANS) ) {
            dxinfox[ connchan[ channum ] ].state = ST_GOODBYE;
            playtone_rep( connchan[channum], 480, 620, -25, -27, 25, 25);
        }
        if (gc_ResetLineDev(port[channum].ldev, EV_ASYNC) == GC_SUCCESS) {
            disp_msg("Taskfail recovery: resetting line device...");
        }
        return(-1);
    }
    else {
        FILE  *errfd;
        int ldev = failevent.evtdev;
        short  channum = get_linechan(ldev);

        disp_msg("ISDN task failed!");
        if (failinfo.gcValue == GCRV_NONRECOVERABLE_FATALERROR) {
            disp_msg("CRITICAL ERROR: Non-recoverable error encountered! Application must be restarted!");
        }

        else {
            disp_msg("Opening error file");
            errfd = fopen("isdn_error.txt", "a+");
            sprintf(tmpbuff, "%s - Printing to error file, fd %i", failinfo.gcMsg, (int)errfd);
            disp_msg(tmpbuff);
            fprintf(errfd, "Task fail handler: GC ErrorValue: 0x%hx - %s, CC ErrorValue: 0x%lx - %s, additional info: %s, channel %d, program state %d, CRN %ld, ldev %li\n", failinfo.gcValue, failinfo.gcMsg, failinfo.ccValue, failinfo.ccMsg, failinfo.additionalInfo, channum, dxinfox[channum].state, port[channum].crn, port[channum].ldev);
            disp_msg("Closing error file");
            fclose(errfd);

            if (gc_ResetLineDev(port[channum].ldev, EV_ASYNC) == GC_SUCCESS) {
                disp_msg("Taskfail recovery: resetting line device...");
            }
            else {
                disp_msg("WARNING: Couldn't reset line device from fatal error state! Will be pulled out of service.");
            }
        }
        if ( ( connchan[channum] != 0) && (connchan[channum] < MAXCHANS) ) {
            dxinfox[ connchan[ channum ] ].state = ST_GOODBYE;
            playtone_rep( connchan[channum], 480, 620, -25, -27, 25, 25);
        }

    }

    return 0;
}

/***********************************************************
              Call Acknowledgement/Unexpected
***********************************************************/

//char isdn_ackhdlr()

//{
// This event shouldn't be called ever. I don't think.
// For the moment, it's going to quit if it actually gets
// executed - just to make sure we know it never gets called.

//    disp_msg("Unexpected GCEV_ACKCALL. Quitting!");
//    QUIT(2);
//}

/***********************************************************
               Handle unblocked ISDN events
***********************************************************/
int isdn_unblock() {
    int  ldev = sr_getevtdev();
    // int  event = sr_getevttype();
    short  lnum = get_linechan(ldev);

    disp_msgf("ldev %i unblocked", ldev);
    /*

    // We can comment these out for now; they're basically debug events
    sprintf( tmpbuff, "event %i in unblock handler", event);
    disp_msg(tmpbuff);
    sprintf( tmpbuff, "lnum %i ready, event %i", lnum, event);
    disp_msg(tmpbuff);
    */
    port[lnum].blocked = 0; //Indicate a lack of blocking
    disp_status(lnum, "ISDN channel ready!");

    if (gc_WaitCall(port[lnum].ldev, NULL, 0, 0, EV_ASYNC) == -1) {
        // Try to recover from the error
        if (gc_ResetLineDev(port[lnum].ldev, EV_SYNC) == GC_SUCCESS) {
            disp_msg("gc_waitcall fail recovery: resetting line device...");
            if (gc_WaitCall(port[lnum].ldev, NULL, 0, 0, EV_ASYNC) == -1) {
                printf("We tried gc_WaitCall again and it failed anyway >.<");
                return -1;
            }
            else {
                printf("Hey, gc_WaitCall's working again!");
            }
            return 0;
        }
        gc_errprint("gc_WaitCall", lnum, -1);
        QUIT(2);
    }

    dxinfox[lnum].state = ST_WTRING;
    return (0);

}

/***********************************************************
               ISDN extension event handler
***********************************************************/
bool isdn_extension() {
    // Note: This event should only be invoked by JCT cards. At least in theory.
    METAEVENT           metaevt;

    if (gc_GetMetaEvent(&metaevt) != GC_SUCCESS) {
        disp_msg("gc_GetMetaEvent failed on GCEV_EXTENSION event. That's... that's bad.");
        return (-1);
    }

    int ldev = metaevt.evtdev; // This'll be more efficient; no need to call the same function twice.
    short channum = get_linechan(ldev);

    // unsigned char ext_id = ((EXTENSIONEVTBLK*)(metaevt.extevtdatap))->ext_id;
    // This is sort of a textbook implementation, but whatever. It (hopefully) works, we'll change it as needed.
    switch (((EXTENSIONEVTBLK *)(metaevt.extevtdatap))->ext_id) {

        case GCIS_EXEV_STATUS:
            // Some switches seem to send this for whatever reason
            sprintf(tmpbuff, "STATUS message received from the network on channel %i! Keep debugging on to get it.", channum);
            disp_msg(tmpbuff);

            if (q931debug == 1) {
                port[channum].crn = metaevt.crn;

                if (gc_GetSigInfo(ldev, (char *)&info_elements[channum], U_IES, &metaevt) != GC_SUCCESS) {
                    disp_msg("gc_GetSigInfo_facilty error!");
                    gc_errprint("gc_GetSigInfo_facility", channum, -1);
                    return (-1);
                }

                FILE  *iefd;
                char iefile[180];
                sprintf(iefile, "%s-%i-%ld_status.dump", isdninfo[channum].dnis, channum, port[channum].crn);
                iefd = fopen(iefile, "a");
                // fprintf( iefd, "%s", info_elements[channum].data );
                //fwrite(info_elements[channum].data, sizeof(char), info_elements[channum].length, iefd);
                fwrite(&info_elements[channum], 1, sizeof(info_elements[channum]), iefd); // This should return size of IE dump in first two bytes, little endian format
                fclose(iefd);
            }

            break;

        case GCIS_EXEV_CONGESTION:
            // Use gc_GetCallInfo() in the future
            sprintf(tmpbuff, "CONGESTION message received via extension event on channel %i!", channum);
            disp_msg(tmpbuff);
            break;

        case GCIS_EXEV_CONFDROP:
            // A drop request has been received
            sprintf(tmpbuff, "DROP request received via extension event for channel %i! Event currently ignored.", channum);
            disp_msg(tmpbuff);
            break;

        case GCIS_EXEV_DIVERTED:
            sprintf(tmpbuff, "INFO: Call successfully diverted on channel %i", channum);
            disp_msg(tmpbuff);
            break;

        case GCIS_EXEV_DROPACK:
            sprintf(tmpbuff, "INFO: Network honored our DROP request on channel %i. Yay.", channum);
            disp_msg(tmpbuff);
            break;

        case GCIS_EXEV_DROPREJ:
            sprintf(tmpbuff, "INFO: Network rejected our DROP request on channel %i! RUDE! >:O", channum);
            disp_msg(tmpbuff);
            break;

        case GCIS_EXEV_FACILITY:
            sprintf(tmpbuff, "INFO: FACILITY message received on channel %i! No action taken; ensure debugging is enabled to receive.", channum);
            disp_msg(tmpbuff);
            port[channum].crn = metaevt.crn;

            if (q931debug == 1) {
                if (gc_GetSigInfo(ldev, (char *)&info_elements[channum], U_IES, &metaevt) != GC_SUCCESS) {
                    disp_msg("gc_GetSigInfo_facilty error!");
                    gc_errprint("gc_GetSigInfo_facility", channum, -1);
                    return (-1);
                }

                FILE  *iefd;
                char iefile[180];
                sprintf(iefile, "%s-%i-%ld_facility.dump", isdninfo[channum].dnis, channum, port[channum].crn);
                iefd = fopen(iefile, "a");
                // fprintf( iefd, "%s", info_elements[channum].data );
                //fwrite(info_elements[channum].data, sizeof(char), info_elements[channum].length, iefd);
                fwrite(&info_elements[channum], 1, sizeof(info_elements[channum]), iefd); // This should return size of IE dump in first two bytes, little endian format
                fclose(iefd);
            }

            break;


        default:
            sprintf(tmpbuff, "GCEV_EXTENSION event received with unknown ext_id %u, channel %i", ((EXTENSIONEVTBLK *)(metaevt.extevtdatap))->ext_id, channum);
            disp_msg(tmpbuff);
            break;
    }

    return (0);
}

/***********************************************************
               Handle blocked ISDN events
***********************************************************/

int isdn_block() {
    int  ldev = sr_getevtdev();
    // int  event = sr_getevttype();
    short  lnum = get_linechan(ldev);

    disp_msgf("ldev %i blocked", ldev);
    /*

    // We can comment these out for now; they're basically debug events
    sprintf( tmpbuff, "event %i in block handler", event);
    disp_msg(tmpbuff);
    sprintf( tmpbuff, "lnum %i not ready, event %i", lnum, event);
    disp_msg(tmpbuff);
    */
    port[lnum].blocked = 1; //Indicate blocking
    disp_status(lnum, "ISDN channel not ready - blocked state");

    dxinfox[lnum].state = ST_BLOCKED;
    return (0);

}

/***********************************************************
                 Detect call progress event
***********************************************************/
/* Placeholder event handler */

char isdn_progresshdlr() {
    int  ldev = sr_getevtdev();
    short  lnum = get_linechan(ldev);

    disp_msgf("Call progress indicator received on %i", lnum);

    return (0);
}

/***********************************************************
                Detect call proceeding event
***********************************************************/
/* Placeholder event handler */
char isdn_proceedinghdlr() {
    int  ldev = sr_getevtdev();
    short  lnum = get_linechan(ldev);

    disp_status(lnum, "Outgoing call - Proceeding");
    return (0);
}

/***********************************************************
                   Handle a facility event
***********************************************************/
char isdn_facilityhdlr() {
    int  ldev = sr_getevtdev();
    short  channum = get_linechan(ldev);
    disp_msgf("Holy shit! ISDN Facility message returned on channel %d!", channum);

    if (q931debug == 1) {
        METAEVENT                    metaevent;

        if (gc_GetMetaEvent(&metaevent) != GC_SUCCESS) {
            disp_msg("gc_GetMetaEvent failed. It really shouldn't.");
            return (-1);
        }

        int ldev = metaevent.evtdev; // This'll be more efficient; no need to call the same function twice.
        port[channum].crn = metaevent.crn;

        if (gc_GetSigInfo(ldev, (char *)&info_elements[channum], U_IES, &metaevent) != GC_SUCCESS) {
            disp_msg("gc_GetSigInfo_facilty error!");
            gc_errprint("gc_GetSigInfo_facility", channum, -1);
            return (-1);
        }

        FILE  *iefd;
        char iefile[180];
        sprintf(iefile, "%s-%i-%ld_facility.dump", isdninfo[channum].dnis, channum, port[channum].crn);
        iefd = fopen(iefile, "a");
        // fprintf( iefd, "%s", info_elements[channum].data );
        // fwrite(info_elements[channum].data, sizeof(char), info_elements[channum].length, iefd);
        fwrite(&info_elements[channum], 1, sizeof(info_elements[channum]), iefd); // This should return size of IE dump in first two bytes, little endian format
        fclose(iefd);
    }


    return (0);
}

/***********************************************************
                       Answer a call
***********************************************************/

char isdn_answerhdlr() {
    int  ldev = sr_getevtdev();
    short  channum = get_linechan(ldev);
    disp_msgf("Call answered! Routing to places. Current state is %d", dxinfox[channum].state);

    return 0;
}

/***********************************************************
                       Accept a call
***********************************************************/

char isdn_accept(short channum) {
    if (gc_AcceptCall(port[channum].crn, 0, EV_ASYNC) != GC_SUCCESS) {
        gc_errprint("gc_AcceptCall", channum, -1);
        return (-1);
    }

    return 0;
}

/***********************************************************
                    Drop an ISDN call
***********************************************************/
char isdn_drop(short channum, int cause) {
    int callstate;
    isdnstatus[channum] = 1;

    if (port[channum].crn == '\0') {
        disp_msgf("channel %d got isdn_drop on idle call. Responsible state is %d", channum, dxinfox[channum].state);
        return 0;
        // Is the software trying to hang up on a call that doesn't exist?
        // Just smile and nod. And tell the user they're being stupid.
    }

    if (gc_GetCallState(port[channum].crn, &callstate) != GC_SUCCESS) {
        // if (callstate == 0x40) {
        // This is a debug message. You can probably erase it. That being said, this function is part of diagnosing
        // a rather nasty bug where calls fail to release.
        // If this fails, it indicates a serious problem. Should we reset the line device?
        gc_errprint("gc_DropCall1", channum, callstate);
        disp_msg("Disconnected call passed to isdn_drop function. Ignoring...");
        return (-1);
    }

    if (callstate == 0x20) {
        // Call is already idle. If it was dropped/idle, the drop_hdlr will release it for us.
        return 0;
    }

    disp_msgf("Channel dropped via isdn_drop, cause value %d", cause);

    if (gc_DropCall(port[channum].crn, cause, EV_ASYNC) != GC_SUCCESS) {
        disp_msgf("Call won't drop in callstate %d. Attempting to release...", callstate);

        if (gc_ReleaseCallEx(port[channum].crn, EV_ASYNC) != GC_SUCCESS) {
            disp_msg("Call won't release. This thing is clingy...");
            gc_GetCallState(port[channum].crn, &callstate);
            gc_errprint("gc_ReleaseCallEx2", channum, callstate);
        }

        return (-1);
    }

    if ((dxinfox[channum].state != ST_CALLPTEST4) && (dxinfox[channum].state != ST_CALLPTEST5)) {
        dxinfox[channum].state = ST_WTRING;
    }

    return (0);
}

/***********************************************************
          Handle an accepted call - report progress
***********************************************************/

int isdn_accepthdlr() {
    int  ldev = sr_getevtdev();
    short  channum = get_linechan(ldev);

    // Corrupt progress testing
    check_crn(channum);
    // set_corruptprog(channum, 1);
    // Send the call to the ISDN digit processor in the voice routine

    if (isdn_inroute(channum) == -1) {
        disp_msgf("Inbound call routing failed on channel %d!", channum);
        return (-1);
    }

    disp_msg("ISDN call accepted successfully");
    return (0);

}

/***********************************************************
          Answer an incoming call - send offhook
***********************************************************/

int isdn_answer(short channum) {
    disp_msg("Answering ISDN call");

    if (gc_AnswerCall(port[channum].crn, 0, EV_ASYNC) != GC_SUCCESS) {
        gc_errprint("gc_AnswerCall", channum, -1);
        return (-1);
    }

    return (0);

}

/***********************************************************
     Handle a dropped call (release, send to releasehdlr)
***********************************************************/

int isdn_drophdlr() {
    int  ldev = sr_getevtdev();
    short  lnum = get_linechan(ldev);

    if (gc_ReleaseCallEx(port[lnum].crn, EV_ASYNC) != GC_SUCCESS) {
        disp_msg("Call won't release. This thing is clingy...");
        gc_errprint("gc_ReleaseCallEx", lnum, -1);
        return (-1);
    }

    disp_msg("Call dropped successfully");
    return (0);
}

/***********************************************************
             Release a call, re-prep channel
***********************************************************/

int isdn_releasehdlr() {
    int  ldev = sr_getevtdev();
    short  lnum = get_linechan(ldev);

    port[lnum].crn = '\0';

    if ((dxinfox[lnum].state == ST_CALLPTEST4) || (dxinfox[lnum].state == ST_CALLPTEST5)) {
        dialer_next(lnum);
        return (0);
    }

    disp_msgf("Call released in state %d! Returning to waitcall state.", dxinfox[ lnum ].state);
    // memset(info_elements[lnum].data, 0x00, 260); // There appear to be memory errors sometimes. This didn't fix, but maybe it'll fix some future problem
    if (gc_WaitCall(port[lnum].ldev, NULL, 0, 0, EV_ASYNC) == -1) {
        gc_errprint("gc_WaitCall", lnum, -1);
        QUIT(2);
    }

    cutthrough[ lnum ] = 0;

    return 0;
}

/***********************************************************
      Mark a released channel as idle, await new calls
***********************************************************/
int isdn_waitcall(short channum) {
    disp_msgf("Calling isdn_waitcall in state %d! Attempting ", dxinfox[ channum ].state);

    if (gc_WaitCall(port[channum].ldev, NULL, 0, 0, EV_ASYNC) == -1) {
        gc_errprint("gc_WaitCall", channum, -1);
        return (-1);
    }

    cutthrough[ channum ] = 0;

    return 0;
}

/***********************************************************
                   Disconnect a call
***********************************************************/

int isdn_discohdlr() {
    GC_INFO callresults;
    METAEVENT           metaevent;
    int ldev;

    if (gc_GetMetaEvent(&metaevent) != GC_SUCCESS) {
        disp_msg("gc_GetMetaEvent failed. It really shouldn't.");
        return (-1);
    }

    // This event structure is required to derive cause codes ^

    if (gc_ResultInfo(&metaevent, &callresults) != GC_SUCCESS) {
        disp_msg("Failed to get ISDN disconnect results!");
        ldev = sr_getevtdev(); // Backup event device deriving
        // Write general failure to wardialer result log
    } else {
        ldev = metaevent.evtdev;
    }

    short  channum = get_linechan(ldev);
    // Can we allocate this last thing in ST_CALLPTEST5? We don't need it otherwise.
    isdnstatus[channum] = 1; // This is here for debugging purposes. Erase if not needed.
    // Clear outstanding data
    switch (dxinfox[ channum ].state) {

        case ST_LOOP1:
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
        case ST_LOOP2:
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
        case ST_CONFWAITSIL:
        case ST_CONFWAIT:
        case ST_CONFCONF:

            // get_confchan figures out the conference participant ID before dropping them.
            // Should we do anything else before exiting the event handler? We may not be
            // able to do much else than complain to the user.

            if (dropfromconf(channum, (unsigned char) connchan[channum]) == FALSE) {
                disp_status(channum, "Conference call | Drop error!");
                return (-1);
            }
            connchan[channum] = 0;

            break;

        case ST_MODEMDETECT:
        case ST_VMBDETECT:
            // These states are handled differently since a dx_stopch must be invoked to properly terminate
            // the dx_getdig function. Though it's likely that this would be a good idea to have anyway on
            // a disconnect handler since DX_LCOFF doesn't do anything on a DM3. Maybe we should do away with
            // the cases and just put this at the top of the function?

            dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
            // isdn_drop(channum, GC_NORMAL_CLEARING); // This is normally uncommented. Let's see what this does...
            return (0);

        case ST_CALLPTEST5:

            // This is an ISDN cause code event coming back. Let's log it if it's abnormal. Should there be a switch case?
            if (callresults.gcValue == 0x500) {
                causelog(channum, (int) callresults.ccValue);
            } else if (callresults.gcValue == 0x503) {
                causelog(channum, (int) callresults.ccValue);
            } else if (callresults.gcValue == 0x547) {
                causelog(channum, (int) callresults.ccValue);    // Congestion. For the moment, let's send the actual q.931 cause code instead.
            } else if (callresults.gcValue == 0x506) {
                causelog(channum, (int) callresults.ccValue);    // cclib-specific failure
            } else {
                causelog(channum, callresults.gcValue);
            }

            dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
            set_hkstate(channum, DX_ONHOOK);
            return (0);

        case ST_ROUTEDREC2:
        case ST_ROUTEDREC:
            // This is a bugfix to stop the isdn_drop() function from being called twice when we're recording. ST_ROUTEDREC may need to go here too.
            if (dxinfox[ connchan[channum] ].state == ST_ROUTEDTEST) {
            if(cutthrough[channum] == 0) {
                routed_cleanup(channum);
                tl_reroute( connchan[channum] ); // Bugfix. Sometimes calls failed to come back to the ISDN test line, and left the outgoing trunk in a hanging state, unable to be used
                return (0);
                }
            }
            if (ATDX_STATE(dxinfox[ connchan[ channum ] ].chdev) != CS_IDLE) {
                dx_stopch(dxinfox[ connchan[channum] ].chdev, EV_ASYNC);
            }
            else {
                // Did the recording routine stop? We can probably just clear normally.
                dxinfox[ connchan[channum] ].state = ST_ONHOOK;
                if (callresults.ccValue & 0x200) {
                    isdn_drop(connchan[channum], (int)(callresults.ccValue & 0x7F));
                }
            }
            return (0);
        case ST_ROUTED:
        case ST_ROUTED2:
        case ST_ROUTEDTEST:
            dx_stopch(dxinfox[ connchan[channum] ].chdev, EV_ASYNC);
            dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
            disp_msg("Unrouting channels from voice devices...");

            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Uhh, I hate to break it to you man, but SCUnroute1 is shitting itself");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCUnroute2 threw an error!");
                disp_msg(tmpbuff);
                disp_err(connchan[channum], dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCroute threw an error!");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
                return (-1);
            }



            // dxinfox[ connchan[channum] ].state = ST_ONHOOK;
            if (dxinfox[ connchan[channum] ].state != ST_ROUTEDREC) {
                if (dxinfox[ connchan[channum] ].state == ST_ROUTEDTEST) {
                    if(cutthrough[channum] == 0) tl_reroute( connchan[channum] ); // This could be a bugfix. Let's find out! Sometimes calls fail to come back to the ISDN test line, and leave the outgoing trunk in a hanging state, unable to be used
                    break;
                }

                dxinfox[ connchan[channum] ].state = ST_ONHOOK;    // This is probably causing issues with ST_ROUTEDREC halting
            }

            if (callresults.ccValue & 0x200) {
                isdn_drop(connchan[channum], (int)(callresults.ccValue & 0x7F));
            }
            // If we're getting a valid cause code (and we more than likely will), pass it onto the connected channel during teardown.
            // If not, just send a normal clearing code.
            else {
                set_hkstate(connchan[channum], DX_ONHOOK);
            }

            break;

        case ST_ECHOTEST:

            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[channum].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Uhh, I hate to break it to you man, but SCUnrouteE is shitting itself");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCroute threw an error!");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
                return (-1);
            }

            break;

        case ST_2600ROUTE:
            dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
            dxinfox[ connchan[channum] ].state = ST_GOODBYE;
            dx_stopch(dxinfox[ connchan[channum] ].chdev, EV_ASYNC);
            // set_hkstate( connchan[channum], DX_ONHOOK);
            disp_msg("Unrouting channels from voice devices...");

            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Uhh, I hate to break it to you man, but SCUnroute1 is shitting itself");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCUnroute2 threw an error!");
                disp_msg(tmpbuff);
                disp_err(connchan[channum], dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCroute threw an error!");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
                return (-1);
            }

            // dx_stopch( dxinfox[ channum ].chdev, EV_ASYNC );
            // dx_stopch( dxinfox[connchan[ channum ]].chdev, EV_ASYNC );
            cutthrough[ connchan[ channum ] ] = 0;
            cutthrough[ channum ] = 0;
            // dxinfox[connchan[ channum ]].state = ST_ONHOOK;
            dxinfox[ channum ].state = ST_ONHOOK;
            set_hkstate( channum, DX_ONHOOK); // Is this going to make problems for us?
            dx_setdigtyp(dxinfox[ channum ].chdev, D_DTMF);
            playtone_cad(connchan[ channum ], 2600, 0, 4);
            dx_deltones(dxinfox[ channum ].chdev);
            return (0);

        case ST_2600ROUTE2:

            disp_msg("Unrouting channels from voice devices...");

            if (nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].tsdev, SC_DTI, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Uhh, I hate to break it to you man, but SCUnroute1 is shitting itself");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[ channum ].chdev, dxinfox[ channum ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[ connchan[channum] ].tsdev, SC_DTI, dxinfox[ connchan[channum] ].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCUnroute2 threw an error!");
                disp_msg(tmpbuff);
                disp_err(connchan[channum], dxinfox[ connchan[channum] ].chdev, dxinfox[ connchan[channum] ].state);
                return (-1);
            }

            if (nr_scroute(dxinfox[channum].tsdev, SC_DTI, dxinfox[channum].chdev, SC_VOX, SC_FULLDUP) == -1) {
                sprintf(tmpbuff, "Holy shit! SCroute threw an error!");
                disp_msg(tmpbuff);
                disp_err(channum, dxinfox[channum].chdev, dxinfox[channum].state);
                return (-1);
            }

            /*
            dxinfox[connchan[ channum ] ].state = ST_2600_2;
            dxinfox[connchan[ channum ] ].digbuf.dg_value[0] = 'V'; // I'd like to conserve states. This seems like the most efficient way to do so.
            dxinfox[connchan[ channum ] ].digbuf.dg_value[1] = '\0';
            */
            dxinfox[connchan[ channum ] ].state = ST_2600_3;
            dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
            dx_stopch(dxinfox[connchan[ channum ]].chdev, EV_ASYNC);
            cutthrough[ channum ] = 0;
            cutthrough[ connchan[ channum ] ] = 0;
            dxinfox[ channum ].state = ST_ONHOOK;
            // dxinfox[ connchan[channum] ].state = ST_2600_1;
            playtone_cad(connchan[channum], 2600, 0, 4);
            set_hkstate(channum, DX_ONHOOK);
            return (0);

    }


    cutthrough[ channum ] = 0;
    dxinfox[ channum ].state = ST_ONHOOK;
    dx_stopch(dxinfox[ channum ].chdev, EV_ASYNC);
    set_hkstate(channum, DX_ONHOOK);
    return (0);
}

/***********************************************************
   Write Q.931 signaling data for progress messages to RAM
***********************************************************/

char progress_writesig(unsigned char offset, short channum) {
    unsigned char type;
    unsigned char length;
    char returncode = 0; // Verify we actually need returncode; I did this with a migraine.
    unsigned char offset2 = 0;
    unsigned char extra = 0;
    type = info_elements[channum].data[offset];
    sprintf(tmpbuff, "Writesig type %u", type);
    disp_msg(tmpbuff);

    switch (type) {
        // REMINDER: MAXIMUM LENGTH OF AN IE IN THE SPEC SHOULD BE CHECKED TO PREVENT OVERFLOWS.
        // THIS IS ALL CAPS LEVELS OF IMPORTANT.

        // Following data is custom to network. We don't need
        // to handle anything else. Oh, or null data.
        case 0x96:
            disp_msg("Codeset 6 locking IE received!");
            offset++;
            returncode = offset;
            break;
            
        case 0x1C:
            disp_msg("Facility IE received! What the fucking hatbowls!?");
            offset++;
            length = info_elements[channum].data[offset];
            offset++; // Data starts here.
            returncode = offset + length;
            break;
            
        case 0x7D:
            disp_msg("Ugh, just got a high layer compatibility IE");
            offset++;
            length = info_elements[channum].data[offset];
            disp_msg("Warning: this IE isn't currently parsed!");
            returncode = (offset + length);
            break;
            
        case 0x08:
            disp_msg("Writing q.850 cause IE");
            offset++;
            length = info_elements[channum].data[offset];
            if (length != 2) {
                disp_msg("Warning: unsupported cause code format! We ghosting this bitch.");
                returncode = (offset + length + 1);
                break;
            }
            
            offset++;
            // *sigh* Why the fucking hatbowls does it think there's a signedness bit?
            if ( (unsigned char) info_elements[channum].data[offset] & 0x70) {
                sprintf(tmpbuff, "Warning: unsupported coding standard in cause IE: %02hhX", (unsigned char) info_elements[channum].data[offset] );
                disp_msg(tmpbuff);
                returncode = (offset + 2);
                break;
            }
            
            offset++;
            if (dxinfox[channum].state == ST_CALLPTEST5) {
                if ( (unsigned char) info_elements[channum].data[offset] != 0xFF ) {
                    // If you get an interworking cause code, don't kill the call.
                    // Are we going to need more of these?
                    causehandle( (unsigned char) info_elements[channum].data[offset] & 0x7F, channum );
                    disp_msg("Cause code received, exiting parser...");
                    returncode = 0;
                }
                else {
                    offset++;
                    returncode = offset;
                }
            }
            else {
                offset++;
                returncode = offset;
                sprintf(tmpbuff, "DEBUG: returned cause code is %02hhX", info_elements[channum].data[offset]);
                disp_msg(tmpbuff);
            }
            break;

        case 0x04:
            // Bearer cap IE
            disp_msg("Writing bearer cap IE");
            offset++;
            length = info_elements[channum].data[offset];

            // Have a length limiter here if it's out of spec. We don't need buffer overflows.
            if (length > 12) {
                disp_msg("Warning: invalid BCAP length. Truncating to 12 bytes.");
                extra = (length - 12);
                length = 12;
            }

            sprintf(tmpbuff, "Length is %u", length);
            disp_msg(tmpbuff);
            offset++;

            while (length > 0) {
                isdninfo[channum].bcap[offset2] = info_elements[channum].data[offset]; // Is there any way to do this with less variables?
                length--;
                offset++;
                offset2++;
            }

            sprintf(tmpbuff, "BCAP is %02hhX %02hhX %02hhX", isdninfo[channum].bcap[0], isdninfo[channum].bcap[1], isdninfo[channum].bcap[2]);
            disp_msg(tmpbuff);
            returncode = (offset + extra); // We shouldn't have to do addition all the time. This should keep the handler on track in case of an (unlikely) overflow, though.
            break;

        case 0x1E:
            // Progress indicator IE
            disp_msg("Writing progress indicator IE");
            offset++;
            length = info_elements[channum].data[offset];

            if (length > 4) {
                disp_msg("Warning: invalid channel ID length. Truncating to 4 bytes.");
                extra = (length - 4);
                length = 4;
            }

            sprintf(tmpbuff, "Length is %u", length);
            disp_msg(tmpbuff);
            offset++;

            while (length > 0) {
                isdninfo[channum].progind[offset2] = info_elements[channum].data[offset];
                length--;
                offset++;
                offset2++;
            }

            sprintf(tmpbuff, "PROG is %02hhX %02hhX", isdninfo[channum].progind[0], isdninfo[channum].progind[1]);
            disp_msg(tmpbuff);
            returncode = (offset + extra);
            break;

        case 0x28:
            disp_msg("Display IE received!");
            // Calling party name. Just skip through this for now; we don't actually need it.
            offset++;
            length = info_elements[channum].data[offset];

            if ((length > 82) || (length > (260 - offset))) {         // Lots of (basic) arithmetic here. Can we fix that?
                disp_msg("ERROR: Display IE exceeds frame and/or standard length! Skipping...");
                returncode = 0;
                break;
            }

            offset++;
            sprintf(tmpbuff, "Display length is %d", length);
            disp_msg(tmpbuff);
            strncpy(isdninfo[channum].displayie, (info_elements[channum].data + offset), length);
            memcpy((isdninfo[channum].displayie + length + 1), "\x00", 1);   // Stick null terminator at the end of the string. Is this necessary?
            returncode = (offset + length);
            sprintf(tmpbuff, "Return code is %d", returncode);
            disp_msg(tmpbuff);
            sprintf(tmpbuff, "Display IE is %s", isdninfo[channum].displayie);
            disp_msg(tmpbuff);
            break;

        case 0xA1:
            disp_msg("Sending complete IE received - exiting IE parser");
            returncode = 0;
            break;

        // Should we implement the rest?

        case 0x00:
            disp_msg("Null terminator received - exiting IE parser");
            returncode = 0;
            break;

        default:
            sprintf(tmpbuff, "Unkn ISDN IE: %u, off %u", type, offset);
            disp_msg(tmpbuff);
            returncode = 0;
            break;

    }

    return (returncode);
}

/***********************************************************
             Write Q.931 signaling data to RAM
***********************************************************/
char writesig(unsigned char offset, short channum) {
    unsigned char type;
    unsigned char length;
    char returncode = 0; // Verify we actually need returncode; I did this with a migraine.
    unsigned char offset2 = 0;
    unsigned char extra = 0;
    type = info_elements[channum].data[offset];
    sprintf(tmpbuff, "Writesig type %u", type);
    disp_msg(tmpbuff);

    switch (type) {
        // REMINDER: MAXIMUM LENGTH OF AN IE IN THE SPEC SHOULD BE CHECKED TO PREVENT OVERFLOWS.
        // THIS IS ALL CAPS LEVELS OF IMPORTANT.

        // Following data is custom to network. We don't need
        // to handle anything else. Oh, or null data.
        case 0x96:
            disp_msg("Codeset 6 locking IE received!");
            offset++;
            returncode = offset;
            break;
            
        case 0x1C:
            disp_msg("Facility IE received! What the fucking hatbowls!?");
            offset++;
            length = info_elements[channum].data[offset];
            offset++; // Data starts here.
            returncode = offset + length;
            break;

        case 0x04:
            // Bearer cap IE
            disp_msg("Writing bearer cap IE");
            offset++;
            length = info_elements[channum].data[offset];

            // Have a length limiter here if it's out of spec. We don't need buffer overflows.
            if (length > 12) {
                disp_msg("Warning: invalid BCAP length. Truncating to 12 bytes.");
                extra = (length - 12);
                length = 12;
            }

            sprintf(tmpbuff, "Length is %u", length);
            disp_msg(tmpbuff);
            offset++;

            while (length > 0) {
                isdninfo[channum].bcap[offset2] = info_elements[channum].data[offset]; // Is there any way to do this with less variables?
                length--;
                offset++;
                offset2++;
            }

            sprintf(tmpbuff, "BCAP is %02hhX %02hhX %02hhX", isdninfo[channum].bcap[0], isdninfo[channum].bcap[1], isdninfo[channum].bcap[2]);
            disp_msg(tmpbuff);
            returncode = (offset + extra); // We shouldn't have to do addition all the time. This should keep the handler on track in case of an (unlikely) overflow, though.
            break;

        case 0x18:
            // Channel ID IE
            disp_msg("Writing channel ID IE");
            offset++;
            length = info_elements[channum].data[offset];

            if (length > 6) {
                disp_msg("Warning: invalid channel ID length. Truncating to 6 bytes.");
                extra = (length - 6);
                length = 6;
            }

            sprintf(tmpbuff, "Length is %u", length);
            disp_msg(tmpbuff);
            offset++;

            while (length > 0) {
                isdninfo[channum].chanid[offset2] = info_elements[channum].data[offset];
                length--;
                offset++;
                offset2++;
            }

            sprintf(tmpbuff, "CHANID is %02hhX %02hhX %02hhX", isdninfo[channum].chanid[0], isdninfo[channum].chanid[1], isdninfo[channum].chanid[2]);
            disp_msg(tmpbuff);
            returncode = (offset + extra);
            break;

        case 0x1E:
            // Progress indicator IE
            disp_msg("Writing progress indicator IE");
            offset++;
            length = info_elements[channum].data[offset];

            if (length > 4) {
                disp_msg("Warning: invalid channel ID length. Truncating to 4 bytes.");
                extra = (length - 4);
                length = 4;
            }

            sprintf(tmpbuff, "Length is %u", length);
            disp_msg(tmpbuff);
            offset++;

            while (length > 0) {
                isdninfo[channum].progind[offset2] = info_elements[channum].data[offset];
                length--;
                offset++;
                offset2++;
            }

            sprintf(tmpbuff, "PROG is %02hhX %02hhX", isdninfo[channum].progind[0], isdninfo[channum].progind[1]);
            disp_msg(tmpbuff);
            returncode = (offset + extra);
            break;


        case 0x6C:
            disp_msg("Writing CPN screen bit.");
            offset++;
            length = info_elements[channum].data[offset];
            offset++;
            isdninfo[channum].callingtype = info_elements[channum].data[offset];
            sprintf(tmpbuff, "CITYP is %02hhX", isdninfo[channum].callingtype);
            disp_msg(tmpbuff);

            if (isdninfo[channum].callingtype & 0x80) {   // Screening/presentation bits sent
                offset++;
                length--;
                isdninfo[channum].prescreen = info_elements[channum].data[offset];
                sprintf(tmpbuff, "CSCRN is %02hhX", isdninfo[channum].prescreen);
                disp_msg(tmpbuff);
            } else {
                isdninfo[channum].prescreen = 0x00;    // This'll never be all zeroes in real life. We'll use this to internally notate a lack of a presentation/screen bit.
            }

            returncode = (offset + length);
            break;

        case 0x70:
            disp_msg("Called number field received. Grabbing number type...");
            // Called party number
            offset++;
            length = info_elements[channum].data[offset];
            offset++;
            isdninfo[channum].calledtype = info_elements[channum].data[offset];
            sprintf(tmpbuff, "CTYP is %02hhX", isdninfo[channum].calledtype);
            disp_msg(tmpbuff);
            returncode = (offset + length);
            break;

        case 0x28:
            disp_msg("Display IE received!");
            // Calling party name. Just skip through this for now; we don't actually need it.
            offset++;
            length = info_elements[channum].data[offset];

            if ((length > 82) || (length > (260 - offset))) {         // Lots of (basic) arithmetic here. Can we fix that?
                disp_msg("ERROR: Display IE exceeds frame and/or standard length! Skipping...");
                returncode = 0;
                break;
            }

            offset++;
            sprintf(tmpbuff, "Display length is %d", length);
            disp_msg(tmpbuff);
            strncpy(isdninfo[channum].displayie, (info_elements[channum].data + offset), length);
            memcpy((isdninfo[channum].displayie + length + 1), "\x00", 1);   // Stick null terminator at the end of the string. Is this necessary?
            returncode = (offset + length);
            sprintf(tmpbuff, "Return code is %d", returncode);
            disp_msg(tmpbuff);
            sprintf(tmpbuff, "Display IE is %s", isdninfo[channum].displayie);
            disp_msg(tmpbuff);
            break;

        case 0x73:
            // Original Called Number. Basically, RDNIS but not.
            disp_msg("Original Called Number received. Thanks, Nortel -_-");
            offset++;
            length = info_elements[channum].data[offset];
            if (( length > 18) || (length > (260 - offset))) {
                disp_msg("ERROR: Original Called Number IE exceeds frame and/or standard length! Skipping...");
                returncode = 0;
                break;
            }
            offset++;
            isdninfo[channum].forwardedtype = info_elements[channum].data[offset];
            switch ( isdninfo[channum].forwardedtype & 0x70 ) {
                case 0x40:
                    disp_msg("DEBUG: Original called number is subscriber type");
                    break;
                case 0x20:
                    disp_msg("DEBUG: Original called number is national type");
                    break;
                case 0x10:
                    disp_msg("DEBUG: Original called number is international type");
                    break;
                case 0x00:
                    disp_msg("DEBUG: Original called number is unknown type");
                    break;
                default:
                    disp_msg("DEBUG: Original called number has malformed number type field");
                    
            }
            switch ( isdninfo[channum].forwardedtype & 0x0F ) {
                case 0x00:
                    disp_msg("DEBUG: Original called number has unknown numbering plan");
                    break;
                case 0x01:
                    disp_msg("DEBUG: Original called number has ISDN numbering plan");
                    break;
                case 0x09:
                    disp_msg("DEBUG: Original called number has private numbering plan");
                    break;
                default:
                    disp_msg("DEBUG: Original called number has malformed numbering plan");
            }
            offset++;
            if (!(isdninfo[channum].forwardedtype & 0x80)) { //Extension bit low? Move to Octet 3A
                isdninfo[channum].forwardedscn = info_elements[channum].data[offset];
                switch ( isdninfo[channum].forwardedscn & 0x60 ) {
                    case 0x00:
                        disp_msg("DEBUG: Original called number presentation is allowed");
                        break;
                    case 0x20:
                        disp_msg("DEBUG: Original called number presentation isn't allowed");
                        break;
                    case 0x40:
                        disp_msg("DEBUG: Original called number isn't available");
                        break;
                    default:
                        disp_msg("DEBUG: Original called number presentation bits are malformed");
                }
                
                switch ( isdninfo[channum].forwardedscn & 0x03 ) {
                    case 0x00:
                        disp_msg("DEBUG: Original called number is user provided, not screened");
                        break;
                    case 0x01:
                        disp_msg("DEBUG: Original called number is user provided, verified and passed");
                        break;
                    case 0x02:
                        disp_msg("DEBUG: Original called number is user provided, verified and failed");
                        break;
                    case 0x03:
                        disp_msg("DEBUG: Original called number is network provided");
                }
                offset++;
                if (!(isdninfo[channum].forwardedscn & 0x80)) { // Process Octet 3B
                    isdninfo[channum].forwardedrsn = info_elements[channum].data[offset];
                    switch ( isdninfo[channum].forwardedrsn & 0x0F ) {
                    case 0x00:
                        disp_msg("DEBUG: Original called number forward reason is unknown");
                        break;
                    case 0x01:
                        disp_msg("DEBUG: Original called number forward reason is call forward busy");
                        break;
                    case 0x02:
                        disp_msg("DEBUG: Original called number forward reason is call forward no reply");
                        break;
                    case 0x0D:
                        disp_msg("DEBUG: Original called number forward reason is call transfer");
                        break;
                    case 0x0E:
                        disp_msg("DEBUG: Original called number forward reason is call pickup");
                        break;
                    case 0x0F:
                        disp_msg("DEBUG: Original called number forward reason is call forwarding unconditional");
                        break;
                    default:
                        sprintf( tmpbuff, "DEBUG: Original called number forward reason is unknown: 0x%x", isdninfo[channum].forwardedtype );
                        disp_msg(tmpbuff);
                }
                    offset++;
                    if (info_elements[channum].data[offset] & 0x80) { // Process Octet 3C
                        isdninfo[channum].forwarddata = info_elements[channum].data[offset];
                        offset++;
                        strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 4) );
                        memcpy(isdninfo[channum].forwardednum + (length - 3), "\x00", 1);
                    }
                    else {
                        isdninfo[channum].forwarddata = 0x81;
                        strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 3) );
                        memcpy(isdninfo[channum].forwardednum + (length - 2), "\x00", 1);
                    }
                }
                else {
                    isdninfo[channum].forwarddata = 0x81;
                    isdninfo[channum].forwardedrsn = 0x00;
                    strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 2));
                    memcpy(isdninfo[channum].forwardednum + (length - 1), "\x00", 1);
                }
            }
            else {
                isdninfo[channum].forwarddata = 0x81;
                isdninfo[channum].forwardedscn = 0x00;
                isdninfo[channum].forwardedrsn = 0x00;
                strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 1));
                memcpy(isdninfo[channum].forwardednum + (length), "\x00", 1);
            }
            sprintf(tmpbuff, "DEBUG: ISDN original called number number is: %s", isdninfo[channum].forwardednum);
            disp_msg(tmpbuff);
            returncode = (offset + length);
            break;

        case 0x74:
            // RDNIS. Wait, we're actually getting this?
            disp_msg("RDNIS received. IT'S A MIRACLE!!!!ONE");
            offset++;
            length = info_elements[channum].data[offset];
            if (( length > 25) || (length > (260 - offset))) {
                disp_msg("ERROR: RDNIS IE exceeds frame and/or standard length! Skipping...");
                returncode = 0;
                break;
            }

            offset++;
            isdninfo[channum].forwardedtype = info_elements[channum].data[offset];
            switch ( isdninfo[channum].forwardedtype & 0x70 ) {
                case 0x40:
                    disp_msg("DEBUG: RDNIS has subscriber number");
                    break;
                case 0x20:
                    disp_msg("DEBUG: RDNIS has national number");
                    break;
                case 0x10:
                    disp_msg("DEBUG: RDNIS has international number");
                    break;
                default:
                    disp_msg("DEBUG: RDNIS has malformed number type field");
                    
            }

            switch (isdninfo[channum].forwardedtype & 0x0F) {
                case 0x00:
                    disp_msg("DEBUG: RDNIS has unknown numbering plan");
                    break;
                case 0x01:
                    disp_msg("DEBUG: RDNIS has ISDN numbering plan");
                    break;
                case 0x09:
                    disp_msg("DEBUG: RDNIS has private numbering plan");
                    break;
                default:
                    disp_msg("DEBUG: RDNIS has malformed numbering plan");
            }

            offset++;
            offset2++;
            if (!(isdninfo[channum].forwardedtype & 0x80)) { //Extension bit low? Move to Octet 3A
                isdninfo[channum].forwardedscn = info_elements[channum].data[offset];

                switch (isdninfo[channum].forwardedscn & 0x60) {
                    case 0x00:
                        disp_msg("DEBUG: RDNIS presentation is allowed");
                        break;
                    case 0x20:
                        disp_msg("DEBUG: RDNIS presentation isn't allowed");
                        break;
                    default:
                        disp_msg("DEBUG: RDNIS presentation bits are malformed");
                }

                switch (isdninfo[channum].forwardedscn & 0x03) {
                    case 0x00:
                        disp_msg("DEBUG: RDNIS number is user provided, not screened");
                        break;
                    case 0x01:
                        disp_msg("DEBUG: RDNIS number is user provided, verified and passed");
                        break;
                    case 0x02:
                        disp_msg("DEBUG: RDNIS number is user provided, verified and failed");
                        break;
                    case 0x03:
                        disp_msg("DEBUG: RDNIS number is network provided");
                }
                offset++;
                offset2++;
                if ((info_elements[channum].data[offset] & 0x80)) { // Process Octet 3B
                    isdninfo[channum].forwardedrsn = info_elements[channum].data[offset];
                    switch ( isdninfo[channum].forwardedrsn & 0x0F ) {
                    case 0x00:
                        disp_msg("DEBUG: RDNIS forward reason is unknown");
                        break;
                    case 0x01:
                        disp_msg("DEBUG: RDNIS forward reason is call forward busy");
                        break;
                    case 0x02:
                        disp_msg("DEBUG: RDNIS forward reason is call forward no reply");
                        break;
                    case 0x03:
                        disp_msg("DEBUG: RDNIS forward reason is call forward network busy");
                        break;
                    case 0x04:
                        disp_msg("DEBUG: RDNIS forward reason is call deflection");
                        break;
                    case 0x09:
                        disp_msg("DEBUG: RDNIS forward reason is called DTE out of order");
                        break;
                    case 0x0A:
                        disp_msg("DEBUG: RDNIS forward reason is call forwarding ordered by DTE");
                        break;
                    case 0x0F:
                        disp_msg("DEBUG: RDNIS forward reason is call forwarding unconditional or systemic redirection");
                        break;
                    default:
                        sprintf( tmpbuff, "DEBUG: RDNIS forward reason is unknown: 0x%x", isdninfo[channum].forwardedtype );
                        disp_msg(tmpbuff);
                }
                    offset++;
                    offset2++;
                    strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 3) );
                    memcpy(isdninfo[channum].forwardednum + (length - 2), "\x00", 1);
                }
                else {
                    isdninfo[channum].forwardedrsn = 0x00;
                    strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 2));
                    memcpy(isdninfo[channum].forwardednum + (length - 1), "\x00", 1);
                }
            }
            else {
                isdninfo[channum].forwardedscn = 0x00;
                isdninfo[channum].forwardedrsn = 0x00;
                strncpy(isdninfo[channum].forwardednum, (info_elements[channum].data + offset), (length - 1));
                memcpy(isdninfo[channum].forwardednum + (length), "\x00", 1);
            }
            sprintf(tmpbuff, "DEBUG: ISDN forwarded number is: %s", isdninfo[channum].forwardednum);
            disp_msg(tmpbuff);
            returncode = (offset + length - offset2);
            break;

        case 0xA1:
            disp_msg("Sending complete IE received - exiting IE parser");
            returncode = 0;
            break;

        // Should we implement the rest?

        case 0x00:
            disp_msg("Null terminator received - exiting IE parser");
            returncode = 0;
            break;

        default:
            sprintf(tmpbuff, "Unkn ISDN IE: %u, off %u", type, offset);
            disp_msg(tmpbuff);
            returncode = 0;
            break;

    }

    return (returncode);
}

/***********************************************************
              Handle an offered incoming call
***********************************************************/

int isdn_offerhdlr()

{
    METAEVENT           metaevent;

    if (gc_GetMetaEvent(&metaevent) != GC_SUCCESS) {
        disp_msg("gc_GetMetaEvent failed. It really shouldn't.");
        return (-1);
    }

    // int  ldev = sr_getevtdev();
    int ldev = metaevent.evtdev; // This'll be more efficient; no need to call the same function twice.
    short  channum = get_linechan(ldev);
    char retcode = 1;
    isdninfo[channum].forwardedtype = 0x00;
    isdninfo[channum].forwardedscn = 0x00;
    isdninfo[channum].forwardedrsn = 0x00;
    isdninfo[channum].forwarddata = 0x81;
    // isdninfo[channum].displayie[0] = 0x00; // Instead of initializing the CPName array, put this here, so the logger doesn't accidentally get old data if there's no display IE
    memset(isdninfo[channum].displayie, 0x00, 83);  // Change 53 to F4
    memset(isdninfo[channum].forwardednum, 0x00, 22);
    port[channum].crn = metaevent.crn;
    sprintf(tmpbuff, "CRN %ld incoming", port[channum].crn);
    disp_msg(tmpbuff);
    disp_msg("Call offered! Take it! TAKE IT QUICK!");
    isdnstatus[channum] = 3; // 3 indicates incoming call to application
    unsigned char offset = 0;
    // char length;

    // gc_CallAck is unnecessary (from what I can tell) and will fail. Keep that shit off.

//    if ( gc_CallAck( port[channum].crn, &callack[channum], EV_ASYNC) != GC_SUCCESS ) {
//    disp_msg("gc_CallAck failed");
//       gc_errprint("gc_CallAck");
    // (insert a call rejection function here)
//       return(-1);
//    }

    if (gc_GetCallInfo(port[channum].crn, DESTINATION_ADDRESS, &(isdninfo[channum].dnis[0])) != GC_SUCCESS) {
        disp_msg("gc_GetCallInfo() error!");
        sprintf(isdninfo[channum].dnis, "00000");
        // If for some reason we can't get the destination (a null destination will not fail), let's write a filler destination and log the error.
        gc_errprint("gc_GetCallInfo_dnis", channum, -1);
    }

    // If there's no CPN, this function will return an error on Springware boards
    // DM3 boards, however, will just silently keep on going.
    if (gc_GetCallInfo(port[channum].crn, ORIGINATION_ADDRESS, &(isdninfo[channum].cpn[0])) != GC_SUCCESS) {
        disp_msg("Error getting CPN!");
        gc_errprint("gc_GetCallInfo_cpn", channum, -1);
        sprintf(isdninfo[channum].cpn, "702");
    }

    else {
        // Is there a + as the first character?
        unsigned short length = strlen(isdninfo[channum].cpn);
        if (length == 1) isdninfo[channum].cpn[0] = 0x30;
        if (length > 1) {
            if (isdninfo[channum].cpn[0] == '+') {
                unsigned short counter;
                for (counter = 0; counter < length; counter++) {
                    isdninfo[channum].cpn[counter] = isdninfo[channum].cpn[counter + 1];
                }
            }

            // Stupid media gateways call for stupid solutions!
            else if (strcmp("Anonymous", isdninfo[channum].cpn) == 0) {
                sprintf(isdninfo[channum].cpn, "702");
            }
        }

        // To do: use snprintf to truncate this if it's too long. Read some specs and see what flavor allows the longest destination.

    }

    // U_IES should be used to get the information elements. They will however, be unformatted. We should fix that here.


    if (gc_GetSigInfo(ldev, (char *)&info_elements[channum], U_IES, &metaevent) != GC_SUCCESS) {
        disp_msg("gc_GetSigInfo() error!");
        gc_errprint("gc_GetSigInfo", channum, -1);
    } else {
        // Dump the IE to a log. For development purposes. Make this something a flag in the
        // program will do at some point; it's completely unnecessary under normal circumstances.
        if (q931debug == 1) {
            FILE  *iefd;
            char iefile[180]; // This is a bit short; the maximum space for CPN alone is 128 bytes. The actual spec has shorter destinations, but still, this leaves you at risk of overflow
            sprintf(iefile, "%s-%i-%ld.dump", isdninfo[channum].dnis, channum, port[channum].crn);
            iefd = fopen(iefile, "a");
            //fprintf( iefd, "%s", info_elements[channum].data );
            //fwrite(info_elements[channum].data, sizeof(char), info_elements[channum].length, iefd);
            fwrite(&info_elements[channum], 1, sizeof(info_elements[channum]), iefd); // This should return size of IE dump in first two bytes, little endian format
            fclose(iefd);
        }

        // Write signaling data to RAM
        // while ( retcode == 1 ) {
        // length = info_elements[channum].data[(offset + 1)];
        // offset = (offset + 2); // This is probably invalid. Take another look when your head isn't in your ass.
        retcode = (writesig(offset, channum));
        offset = retcode;

        while (offset > 0) {
            if (offset >= info_elements[channum].length) break; // Make sure we don't extend past the (supposed) length of the buffer
            retcode = (writesig(offset, channum));
            offset = retcode;
            sprintf(tmpbuff, "Offset is %u", offset);
            disp_msg(tmpbuff);
        }

        // offset = (offset + length);
        // }
    }

    if (isdn_accept(channum) == -1) {
        disp_msg("Holy shit! isdn_accept() failed! D:");
        return (-1);
    }

    return (0);

}

/***********************************************************
              Prepare to open all channels
***********************************************************/

char isdn_prep(int maxchans) {

    GC_START_STRUCT gclib_start;    // Struct for gc_start(). The Dialogic people like their weird typedefs. This is in gclib.h .
    char dtiname[10]; // For board deriving function
    int tsdev; // For board deriving function
    CT_DEVINFO ctdevinfo;

    // The next part of this routine determines what sort of board we're using.
    sprintf(dtiname, "dtiB%dT1", dtibdnum);

    if ((tsdev = dt_open(dtiname, 0)) == -1) {
        disp_msg("Couldn't open board for type probing! Exiting.");
        QUIT(2);
    }

    if (dt_getctinfo(tsdev, &ctdevinfo) == -1) {
        sprintf(tmpbuff, "Error message = %s", ATDV_ERRMSGP(tsdev));
        disp_msg(tmpbuff);
        QUIT(2);
    }

    if (dt_close(tsdev) == -1) {
        disp_msg("Weird. Couldn't close timeslot. This is bad.");
        QUIT(2);
    }

    if (ctdevinfo.ct_devfamily == CT_DFDM3) {
        dm3board = TRUE;
    }

    // boardtyp = ctdevinfo.ct_devfamily;

    // Can we do this differently? The compiler expects a non-empty struct by the time
    // gclib_start.cclib_list = cclib_Start is applied, and a catch-all else statement
    // won't satisfy it. \/

    CCLIB_START_STRUCT cclib_Start[] = {
        // Ayup. Just ISDN. Fuck you and your weird protocols.
        {"GC_ISDN_LIB", NULL},
    };

    if (dm3board == TRUE) {
        disp_msg("Man dang, this here be a DM3!");
        cclib_Start->cclib_name = "GC_DM3CC_LIB";
        // Apparently there's one library for all DM3 protocols. Heh.
    } else {
        disp_msg("This whoozit is a JCT. Fancy.");
    }

    gclib_start.num_cclibs = 1; // Only one library needed; the ISDN one
    gclib_start.cclib_list = cclib_Start; // List of needed call control libraries; namely, just the ISDN one.

    disp_msg("isdn_init start");

    // We're going to assume scbus is set to true. Is this a problem?

    if ((!scbus) || (frontend == CT_NTANALOG)) {
        // The non-ISDN initialization code is located elsewhere - we should never reach this state.
        sprintf(tmpbuff, "Error! Please set scbus, frontend to TRUE, digital interface for ISDN. \n");
        disp_msg(tmpbuff);
        QUIT(2);
    }

    disp_msg("Starting gc_Start");

    if (gc_Start(&gclib_start) != GC_SUCCESS) {
        gc_errprint("gc_Start", 0, -1);
        gc_Stop();
//    return(gc_error_info.gcValue);
        QUIT(2);
    }

    disp_msg("gc_Start initialized");

    if (sr_enbhdlr(EV_ANYDEV, GCEV_UNBLOCKED, (EVTHDLRTYP)isdn_unblock) == -1) {
        disp_msg("Unable to set-up the GlobalCall unblock handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_BLOCKED, (EVTHDLRTYP)isdn_block) == -1) {
        disp_msg("Unable to set-up the GlobalCall block handler");
        QUIT(2);
    }

    // Prepare an event handler for the unblocking of ISDN channels
    disp_msg("Starting gc_Start");


    if (sr_enbhdlr(EV_ANYDEV, GCEV_ACCEPT, (EVTHDLRTYP)isdn_accepthdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Accept handler");
        QUIT(2);
    }

    // To do: make this work for DM3 devices too:
    if (sr_enbhdlr(EV_ANYDEV, GCEV_FACILITY, (EVTHDLRTYP)isdn_facilityhdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Accept handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_OFFERED, (EVTHDLRTYP)isdn_offerhdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Offer handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_PROCEEDING, (EVTHDLRTYP)isdn_proceedinghdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Offer handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_DROPCALL, (EVTHDLRTYP)isdn_drophdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Drop handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_DISCONNECTED, (EVTHDLRTYP)isdn_discohdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall DISCO handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_CALLPROGRESS, (EVTHDLRTYP)isdn_progresshdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Progress handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_ANSWERED, (EVTHDLRTYP)isdn_answerhdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Answer handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_RELEASECALL, (EVTHDLRTYP)isdn_releasehdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Release handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_CONNECTED, (EVTHDLRTYP)isdn_connecthdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Connect handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_CALLSTATUS, (EVTHDLRTYP)isdn_callstatushdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Release handler");
        QUIT(2);
    }

    /*
    if (sr_enbhdlr(EV_ANYDEV, GCEV_ACKCALL, (EVTHDLRTYP)isdn_ackhdlr) == -1 ) {
     disp_msg( "Unable to set-up the GlobalCall Acknowledgement/Error handler" );
     QUIT( 2 );
      }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_MOREINFO, (EVTHDLRTYP)isdn_infohdlr) == -1 ) {
     disp_msg( "Unable to set-up the GlobalCall Answer handler" );
     QUIT( 2 );
      }

      */

    if (sr_enbhdlr(EV_ANYDEV, GCEV_PROGRESSING, (EVTHDLRTYP)isdn_progressing) == -1) {
        disp_msg("Unable to set-up the GlobalCall Progress handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_ALERTING, (EVTHDLRTYP)isdn_progressing) == -1) {
        disp_msg("Unable to set-up the GlobalCall Alerting handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_TASKFAIL, (EVTHDLRTYP)isdn_failhdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Answer handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_EXTENSION, (EVTHDLRTYP)isdn_extension) == -1) {
        disp_msg("Unable to set-up the GlobalCall Extension handler");
        QUIT(2);
    }

    // This last one is a test handler

    if (sr_enbhdlr(EV_ANYDEV, GCEV_MEDIADETECTED, (EVTHDLRTYP)isdn_mediahdlr) == -1) {
        disp_msg("Unable to set-up the GlobalCall Media handler");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, GCEV_SETCONFIGDATA, (EVTHDLRTYP)config_successhdlr) == -1) {
        disp_msg("Unable to set up the Globalcall configuration success handler. HOW WILL WE EVER HAVE SUCCESS AGAIN!? D:");
        QUIT(2);
    }

    if (sr_enbhdlr(EV_ANYDEV, EGC_GLARE, (EVTHDLRTYP)glare_hdlr) == -1) {
        disp_msg("Unable to set-up the Glare handler");
        QUIT(2);
    }

    if (maxchans > 23) {
        // To do: add support for multiple different boards. The real logic puzzle
        // is to make it efficiently count upwards without touching the 24th (signaling)
        // channel.
        disp_msg("More than 23 channels detected. Board uniformity assumed.");
    }

    isdn_open(maxchans);
    return (0);

}

/***********************************************************
                Actually open all channels
***********************************************************/

int isdn_open(int maxchan) {
    char dtiname[ 32 ];
    char d4xname[ 32 ];
    char linedti[ 32 ];
    short channum;
    // For IE receive buffer size setting
    GC_PARM parm = {0};
    parm.shortvalue = 10;


    for (channum = 1; channum <= maxchan; channum++) {

        if ((channum % 24) == 0) {
            // Is there a better way to do this? I'd rather not do so many division operations...
            // channum++;
            continue;
        }

        sprintf(tmpbuff, "Init %i", channum);
        disp_msg(tmpbuff);

        // Initialize settings for information elements
        raw_info_elements[channum].gclib = NULL; // That's what they say to do...
        raw_info_elements[channum].cclib = &info_elements[channum];

        /* Initial channel state is blocked */
        port[channum].blocked = 1; // 1 = Yes, 0 = No

        // I don't see a better way of doing it, but the division bothers me...

        sprintf(dtiname, "dtiB%dT%d", (channum > 24) ? (dtibdnum + (channum / 24)) : dtibdnum, (channum > 24) ? (channum - (24 * (channum / 24))) : channum);
        sprintf(d4xname, "dxxxB%dC%d", (channum % 4) ? (channum / 4) + d4xbdnum : d4xbdnum + (channum / 4) - 1, (channum % 4) ? (channum % 4) : 4);

        sprintf(linedti, ":N_%s:P_ISDN", dtiname);

        disp_msg("gc_OpenEx init");
        disp_msg(dtiname);
        disp_msg(d4xname);
        disp_msg(linedti);

        // Open the GlobalCall devices. We should probably modify this for asynchronous execution later.
        if (gc_OpenEx(&port[channum].ldev, linedti, EV_SYNC, (void *)&port[channum]) != GC_SUCCESS) {
            sprintf(tmpbuff, "Uh, we have a problem here. gc_OpenEx dun goofed on channel %d", dxinfox[channum].tsdev);
            disp_msg(tmpbuff);
            gc_errprint("gc_OpenEX", channum, -1);
            sr_release(); // To do: figure out why this is needed/a good idea
            QUIT(2);
        }

        sprintf(tmpbuff, "%s gc_OpenEx success", linedti);
        disp_msg(tmpbuff);

        // Now for the tricky part...
        // Should we insert voice parameters into the code at a later point?
        // Note all these functions are synchronous. This isn't a problem for you people, is it? It's just a setup function...

        disp_status(channum, "ISDN channel not ready - blocked state");


        // Make sure this works on JCT architecture cards, plz&thx2u
        // (It seems to)

        if (gc_SetParm(port[channum].ldev, RECEIVE_INFO_BUF, parm) != GC_SUCCESS) {
            disp_msg("Fuck, we're hosed. RECEIVE_INFO_BUF setting failed.");
            QUIT(2);
        }

        // Finish voice routing semi-normally

        if ((dxinfox[ channum ].chdev = dx_open(d4xname, 0)) == -1) {
            sprintf(tmpbuff, "Unable to open channel %s, errno = %d",
                    d4xname, errno);
            disp_msg(tmpbuff);
            QUIT(2);
        }

        sprintf(tmpbuff, "chdev %i opened", dxinfox[channum].chdev);
        disp_msg(tmpbuff);

        if (dm3board == TRUE) {
            // JCT boards don't support this for ISDN operations; nr_scroute is sufficient.
            if (gc_AttachResource(port[ channum ].ldev, dxinfox[ channum ].chdev, NULL, NULL, GC_VOICEDEVICE, EV_SYNC) != GC_SUCCESS) {
                gc_errprint("gc_AttachResource", channum, -1);
                sprintf(tmpbuff, "gc_AttachResource failed on channel %d!", channum);
                disp_msg(tmpbuff);
                QUIT(2);
            }
            callprog( channum, FALSE ); // In case call progress detection from the dialer is still on, turn that off.
        }

        if (gc_GetResourceH(port[channum].ldev, &dxinfox[ channum ].tsdev, GC_NETWORKDEVICE) != GC_SUCCESS) {
            gc_errprint("gc_GetResourceH(GC_NETWORKDEVICE)", channum, -1);
            QUIT(2);
        }

        sprintf(tmpbuff, "tsdev %i assigned", dxinfox[channum].tsdev);
        disp_msg(tmpbuff);

        sprintf(tmpbuff, "ldev %li assigned", port[channum].ldev);
        disp_msg(tmpbuff);

        nr_scunroute(dxinfox[ channum ].tsdev, SC_DTI,
                     dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP);

        // This code reserved for echo cancellation. Uncomment/modify when that's finished.
        /*
                 if ((altsig && 128 ) && (boardtyp == CT_DFDM3) ) {
                     disp_msg("Preparing for EC channel set");
                     if (nr_scroute( dxinfox[ channum ].tsdev, SC_DTI,
                                        dxinfox[ channum ].chdev, SC_EC, SC_FULLDUP )
                                                                                == -1 ) {
                    sprintf( tmpbuff, "nr_scroute() failed for %s - %s",
                                         ATDV_NAMEP( dxinfox[ channum ].chdev ),
                                         ATDV_NAMEP( dxinfox[ channum ].tsdev ) );
                    disp_msg( tmpbuff );
                    QUIT( 2 );
                    }

                 ec_taps = 128; // 128 taps is 16 milliseconds of echo cancellation
                     if (ec_setparm( dxinfox[ channum ].chdev, DXCH_EC_TAP_LENGTH, (void *)&ec_taps) == -1 ) {
                         sprintf( tmpbuff, "ec_setparm error %i", ATDV_LASTERR(dxinfox[ channum ].chdev));
                         disp_msg(tmpbuff);
                         QUIT(2);
                     }
                  }

                  else {
        */
        if (nr_scroute(dxinfox[ channum ].tsdev, SC_DTI,
                       dxinfox[ channum ].chdev, SC_VOX, SC_FULLDUP)
                == -1) {
            sprintf(tmpbuff, "nr_scroute() failed for %s - %s",
                    ATDV_NAMEP(dxinfox[ channum ].chdev),
                    ATDV_NAMEP(dxinfox[ channum ].tsdev));
            disp_msg(tmpbuff);
            QUIT(2);
        }

//          }

        sprintf(tmpbuff, "Channel %i is ready for calls!", channum);
        disp_msg(tmpbuff);

    }

    return (0);
}

void isdn_close(int channum) {

    //for(int counter = 0; counter < maxchan; counter++) {
        if (channum % 24 == 0) return;
        int callstate;
        if (gc_GetCallState(port[ channum ].crn, &callstate) == GC_SUCCESS)
            disp_msgf("DEBUG: callstate for channel %d is %d", channum, callstate);
        else disp_msgf("DEBUG: Couldn't get call state for channel %d", channum);
        /*
        if (gc_SetChanState(port[ channum ].ldev, GCLS_OUT_OF_SERVICE, EV_SYNC) != GC_SUCCESS) {
            disp_msg("ERROR: gc_SetChanState failed!");
            gc_errprint("gc_SetChanState", port[channum].ldev, 0);
        }
        */        
        if (gc_ResetLineDev(port[channum].ldev, EV_SYNC) != GC_SUCCESS) {
            disp_msg("ERROR: Couldn't reset line device!");
        }

        if (gc_Close(port[ channum ].ldev) != GC_SUCCESS) {
            disp_msg("ERROR: gc_Close failed!");
            gc_errprint("gc_Close", port[channum].ldev, 0);
        }
    //}
    return;
}
