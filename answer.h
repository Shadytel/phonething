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
/*********************************************************************
*                                                                   *
*    AA    N    N   SSSS   W    W  EEEEEE  RRRRR           H    H   *
*   A  A   NN   N  S       W    W  E       R    R          H    H   *
*  A    A  N N  N   SSSS   W    W  EEEEE   R    R          HHHHHH   *
*  AAAAAA  N  N N       S  W WW W  E       RRRRR    ...    H    H   *
*  A    A  N   NN  S    S  WW  WW  E       R   R    ...    H    H   *
*  A    A  N    N   SSSS   W    W  EEEEEE  R    R   ...    H    H   *
*                                                                   *
*********************************************************************/

/**
 ** Definitions
 **/
#define MAXDTMF     4   /* Number of Digits Expected        */
#define MAXCHANS    96  /* Maximum Number of Channels       */
#define MAXRING     2   /* Number of Rings Before Picking Up    */
#define MAXMSG      260 /* Maximum Length of Message Filename. So, *so* wasteful... */
#define FALSE       0
#define TRUE        1
#define SIGEVTCHK   0x1010  /* Check for type of signalling event   */
#define SIGBITCHK   0x1111  /* Check for signalling bit change  */
// #pragma once // For global typedef

#define CT_LIST         0x06 // For multiple frontends
#define CT_GCISDN   0x20 // Dirty dirty dirty! Erase eventually.

#define random_at_most(x) (rand() % (x))

/**
 ** External variables for ISDN function
 **/
extern int  scbus; // Use SCBus routing?
extern int  frontend; // Analog? T1? E1?
extern short  maxchans; // Maximum number of channels
extern int  bdnum;    // Board number to start initializing with
int d4xbdnum;       /* Default D/4x Board Number to Start use */
int dtibdnum;       /* Default DTI Board Number to Start use */
//extern char tmpbuff[ 256 ];       /* Temporary Buffer */
extern int  d4xbdnum; // Default D/4X board number to start with
// char dnis[6][MAXCHANS+1]; /* Moved to struct*/
// extern int  chdev;    // Channel device descriptor
// extern int  tsdev;    // Timeslot device descriptor
char dm3board; // This may need to be an array later for multi-board support. Can't we make it a bool?
char isdnstatus[ MAXCHANS + 1 ];
char filetmp[ MAXCHANS + 1 ][ MAXMSG + 1 ];
char filetmp2[ MAXCHANS + 1 ][ MAXMSG + 1 ];
short connchan[MAXCHANS + 1];
int errorfd;
int invalidfd;
int multiplay[MAXCHANS + 1][6];
unsigned char participants[2];
int altsig; /* For winkstart operation or echo cancellation */
short loopchan;
char q931debug; /* Activates IE file dumps for q.931 stack. This should be part of a bitmask. */
FILE *debugfile;
FILE *resumefile[ MAXCHANS + 1 ];
DX_CAP cap;
//typedef struct sqlite3 sqlite3; // This is mostly here so we're not #including the SQL stuff in every source file that has answer.h.
// TO DO: Break the SQL header out into another .h file
//sqlite3 * activationdb;

/*
 * Conference variables
 */

int confdev;
int confbrd;

typedef struct dx_info_x {
    int            chdev;                      /* Channel Device Descriptor    */
    int            tsdev;                      /* Timeslot Device Descriptor   */
    int            state;                      /* State of Channel             */
    int            msg_fd ;                     /* File Desc for Message Files  */
    DV_DIGIT digbuf;                   /* Buffer for DTMF Digits       */
    DX_IOTT  iott[ 1 ];                        /* I/O Transfer Table           */
    char           msg_name[ MAXMSG + 1 ];     /* Message Filename             */
    // long           scts;                       /* Logical timeslot identifier */
    //char           ac_code[ MAXDTMF + 1 ];     /* Access Code for this Channel */ This isn't used - let's get rid of it
} DX_INFO_Y;

DX_INFO_Y                dxinfox[ MAXCHANS + 1 ];

typedef struct isdn {
    char dnis[128]; /* Called number. These first two fields should have the maximum defined with limits from documetation */
    char cpn[128]; // This is wasteful, I know. But this is what the Diva does for MAX_ADDR_LEN, and it seems reasonable enough to prevent overflow on any spec.
    char displayie[83];
    unsigned char prescreen; // Screen bit. For CPN.
    unsigned char bcap[13]; // Maximum length is 12 octets
    unsigned char chanid[7]; // Maximum length is 6 octets? Depends on network, but it's generally three.
    unsigned char progind[5]; // Maximum length is 4 octets
    unsigned char calledtype;
    unsigned char callingtype;
    unsigned char forwardedtype;
    unsigned char forwardedscn;
    unsigned char forwardedrsn;
    unsigned char forwarddata; 
    char forwardednum[22]; // Ensure there isn't an off-by-one here
} Q931SIG;

Q931SIG isdninfo[ MAXCHANS + 1];
/*
 * Vox Files to Open
 */
#define INTRO_VOX   "sounds/INTRO.VOX"
#define INVALID_VOX "sounds/INVALID.VOX"
#define GOODBYE_VOX "sounds/GOODBYE.VOX"
#define ERROR_VOX   "sounds/ERROR.VOX"

/*
 * Definition of states
 */
#define ST_BLOCKED  0   /* For ISDN circuits; channel not available */
#define ST_WTRING   1   /* Waiting for an Incoming Call    */
#define ST_OFFHOOK  2   /* Going Off Hook to Accept Call   */
#define ST_INTRO    3   /* Play the intro.vox File         */
#define ST_GETDIGIT 4   /* Get DTMF Digits (Access Code)   */
#define ST_PLAY     5   /* Play the Caller's Message File  */
#define ST_RECORD   6   /* Recording Message from Caller   */
#define ST_INVALID  7   /* Play invalid.vox (Invalid Code) */
#define ST_GOODBYE  8   /* Play goodbye.vox                */
#define ST_ONHOOK   9   /* Go On Hook                      */
#define ST_ERROR    10  /* An Error has been Encountered   */
#define ST_DYNPLAY_DTMF 11  /* Playing DISA tone           */
#define ST_DIGPROC  12  /* Playtone getdigits stuff        */
#define ST_DYNDIG   13  /* Dynamic sound player digit context */
#define ST_DYNPLAY  14  /* Dynamic sound player play context */
#define ST_ENIGMAREC    15  /* Long record function for Enigma */
#define ST_ENIGMAREC2   16  /* Editing and stuff for Enigmarec */
#define ST_EVANS1   17
#define ST_EVANS2   18
#define ST_EVANS3   19
#define ST_OUTDIALSB    20 // if ( dxinfo[ channum ].state > ST_OUTDIALSB ) tpt[ 0 ].tp_length = DM_P;
#define ST_CATREC   21
#define ST_CATREC2  22
#define ST_CATREC3  23
#define ST_CATCREATE    24
#define ST_GETCAT   25
#define ST_GETCAT2  26
#define ST_GETCAT3  27
#define ST_CATNOEXIST   28
#define ST_ENTERPASS    29
#define ST_PASSCREATE   30
#define ST_PASSCREATE2  31
#define ST_PASSCREATE3  32
#define ST_CATMENU  33
#define ST_CATMENU2 34
#define ST_CATRESUME    35
#define ST_OUTDIAL  36
#define ST_OUTDIAL2 37
#define ST_OUTDIAL3 38
#define ST_ANAC     39
#define ST_PASSREADBACK 40
#define ST_MSGREAD  41
#define ST_PLAYNWN  42
#define ST_WINK     43
#define ST_WINKDIG  44
#define ST_MAKEMARK 45
#define ST_RESUMEMARK   46
#define ST_RESUMEMARK2  47
#define ST_RESUMEMARK3  48
#define ST_MSGREC   49
#define ST_MSGREC2  50
#define ST_MSGREC3  51
#define ST_EMREC1   52
#define ST_EMREC2   53
#define ST_EMREC3   54
#define ST_EMPLAY1  55
#define ST_EMPLAY2  56
#define ST_SASTROLL 57 // Swapped for function that increments state values to eliminate resource waste
#define ST_VMAIL1   58
#define ST_VMAIL2   59
#define ST_VMAIL3   60
#define ST_VMAIL4   61
#define ST_VMAIL5   62
#define ST_VMAILPASS    63
#define ST_CALLPTEST4L  64
#define ST_ROUTEDREC    65
#define ST_ROUTEDREC2   66
#define ST_EMPLAY3  67
#define ST_CRUDEDIAL    68
#define ST_CRUDEDIAL2   69
#define ST_ROUTED   70
#define ST_ROUTED2  71
#define ST_TXDATA   72
#define ST_ISDNTEST 73
#define ST_ISDNERR  74
#define ST_ISDNERR2 75
#define ST_ISDNTEST_ENDCAUSE 76
#define ST_ISDNTEST_ENDCAUSE2 77
#define ST_ISDNTEST_CPNDREAD 78
#define ST_ISDNTEST_CPNDREAD2 79
#define ST_ISDNTEST_CPNREAD  80
#define ST_ISDNTEST_CPNREAD2 81
#define ST_ISDNTEST_CPNREAD3 82
#define ST_ISDNTEST_CPTYPE   83
#define ST_ISDNTEST_CPTYPE2  84
#define ST_ISDNTEST_NUTYPE   85
#define ST_ISDNTEST_NUTYPE2  86
#define ST_ISDNTEST_TEMPMENU 87
#define ST_FAKECONF1         88
#define ST_FAKECONF2         89
#define ST_FAKECONF3         90
#define ST_FAKECONF_ERR      91
#define ST_PLAYLOOP          92
#define ST_VMAILPASS1        93
#define ST_VMAILSETUP1       94
#define ST_VMAILSETUP1E      95
#define ST_VMAILSETUP1C      96
#define ST_VMAILSETUP2       97
#define ST_VMAILSETGREC      98
#define ST_VMAILGRECEDIT1    99
#define ST_VMAILGRECEDIT1E   100
#define ST_VMAILGRECEDIT2   101
#define ST_VMAILGRECEDIT3   102
#define ST_VMAILCHECK1      103
#define ST_TONETEST         104
#define ST_TONETEST2        105
#define ST_VMAILCHECK4      106
#define ST_VMAILMENU        107
#define ST_VMAILMENU2       108
#define ST_VMREADBACK       109
// #define ST_VMAILRNEW2        110
#define ST_VMAILHEADER4     111
#define ST_VMAILHEADER3     112
#define ST_VMAILHEADER2     113
#define ST_VMAILHEADER      114
#define ST_VMAILRNEW        115
#define ST_VMAILRNEW2       116
#define ST_VMAILRNEW3       117
#define ST_VMAILRNEW4       118
#define ST_VMAILRSAVED      119
// These need to be developed further within the application
#define ST_VMAILCOMP        120
#define ST_VMAILSETM        121
#define ST_VMAILSETMP       122
#define ST_VMAILTYP         123
#define ST_VMAILTYP2        124
#define ST_VMAILNPASS1      125
#define ST_VMAILNPASS1C     126
#define ST_VMAILNPASS1E     127
#define ST_VMAILSETGREC2    128
#define ST_COLLCALL         129
#define ST_COLLCALL2        130
#define ST_COLLCALL3        131
#define ST_CONFCONF         132
#define ST_CALLPTEST        133
#define ST_CALLPTESTE       134
#define ST_CALLPTEST2       135
#define ST_CALLPTEST2E      136
#define ST_CALLPTEST3       137
#define ST_CALLPTEST3E      138
#define ST_CALLPTEST4       139
#define ST_CALLPTEST5       140
#define ST_PLAYMULTI        141
#define ST_PLAYMULTI1       142
#define ST_MODEMDETECT      143
#define ST_VMBDETECT        144
#define ST_ISDNROUTE        145
#define ST_ISDNROUTE1       146
#define ST_ISDNROUTE2       147
#define ST_CONFWAIT         148
#define ST_EVANSDM3         149
#define ST_DIALSUPE         150
#define ST_ISDNACB          151
#define ST_CATPAUSE         152
#define ST_ISDNNWN          153
#define ST_2600_1           154
#define ST_2600_2           155
#define ST_2600STOP         156
#define ST_2600ROUTE        157
#define ST_2600ROUTE2       158
#define ST_SOUNDTEST        159
#define ST_CONFWAITSIL      160
#define ST_ECHOTEST         161
#define ST_DISALOGIN        162
#define ST_ROUTEDTEST       163
#define ST_RDNISREC         164
#define ST_2600_3           165
#define ST_RANDOMCAUSE      166
#define ST_DTMFGAME         167
#define ST_DTMFGAME1        168
#define ST_CONFOUT          169
#define ST_ACTIVATION       170
#define ST_ACTIVATION2      171
#define ST_ACTIVATION2E     172
#define ST_ACTIVATION3      173
#define ST_ACTIVATION3E     174
#define ST_ACTIVATION4      175
#define ST_ACTIVATIONF      176
#define ST_ACTIVATIONE      177
#define ST_ACTIVATIONPS     178
#define ST_ACTIVATIONOP     179
#define ST_MUSIC            180
#define ST_ADMINACT         181
#define ST_ADMINACTE        182
#define ST_ADMINACT2        183
#define ST_ADMINACT2A       184
#define ST_ADMINACT2E       185
#define ST_ADMINACT3        186
#define ST_ADMINACT3E       187
#define ST_ADMINADD         188
#define ST_ADMINADDE        189
#define ST_ADMINADDF        190
#define ST_ADMINADD2        191
#define ST_ADMINADD2E       192
#define ST_ADMINADD3        193
#define ST_ADMINADD3E       194
#define ST_LOOP1            195
#define ST_LOOP2            196
/*
 * Macros
 */
#ifndef NO_CURSES
#define QUIT( n )   endwin();   \
    exit( (n) )
#else
#define QUIT( n )   exit( (n) )
#endif

