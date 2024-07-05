/**********@@@SOFT@@@WARE@@@COPY@@@RIGHT@@@**********************************
* DIALOGIC CONFIDENTIAL
*
* Copyright (C) 2000-2007 Dialogic Corporation. All Rights Reserved.
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
/*
 * Include files.
 */
#include <stdio.h>
//#include <varargs.h>
#include <stdarg.h>
#include <srllib.h>
#include <scroute.h>
#include <dxxxlib.h>
#ifdef DTISC
    #include <dtilib.h>
#endif
#ifdef MSISC
    #include <msilib.h>
#endif
#ifdef FAXSC
    #include <faxlib.h>
#endif

#include "sctools.h"


/*
 * Function prototypes
 */
static void nr_scerror(char* first, ...);


/***************************************************************************
 *        NAME: nr_scroute(devh1, devtype1, devh2, devtype2, mode)
 * DESCRIPTION: Create a half or full duplex connection between two SCBus
 *              devices.
 *      INPUTS: int devh1;               - First SCBus device handle.
 *              unsigned short devtype1; - Specifies the type for devh1.
 *              int devh2;               - Second SCBus device handle.
 *              unsigned short devtype2; - Specifies the type for devh2.
 *              unsigned char mode;      - SC_FULLDUP or SC_HALFDUP.
 *     OUTPUTS: Nothing.
 *     RETURNS: 0 or -1 on error
 *************************************************************************/
#if ( defined( __STDC__ ) || defined( __cplusplus ) )
int nr_scroute( int devh1, unsigned short devtype1,
                int devh2, unsigned short devtype2, unsigned char mode )
#else
int nr_scroute( devh1, devtype1, devh2, devtype2, mode )
   int            devh1;
   unsigned short devtype1;
   int            devh2;
   unsigned short devtype2;
   unsigned char  mode;
#endif
{
   SC_TSINFO sc_tsinfo;       /* SCBus Timeslots information structure */
   long      scts;            /* SCBus Timeslot */
   /*
    * Setup the SCBus Timeslots information structure.
    */
   sc_tsinfo.sc_numts = 1;
   sc_tsinfo.sc_tsarrayp = &scts;

   /*
    * Get the SCBus timeslot connected to the transmit of the first device.
    */
   switch (devtype1) {
   case SC_VOX:
      if (dx_getxmitslot(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: dx_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;

   case SC_LSI:
      if (ag_getxmitslot(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: ag_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;

#ifdef DTISC
   case SC_DTI:
      if (dt_getxmitslot(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: dt_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;
#endif

#ifdef MSISC
   case SC_MSI:
      if (ms_getxmitslot(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: ms_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;
#endif

#ifdef FAXSC
   case SC_FAX:
      if (fx_getxmitslot(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: fx_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;
#endif

   default:

      nr_scerror("nr_scroute: %s: ERROR: Invalid 1st device type\n",
                                                             ATDV_NAMEP(devh1));
      return -1;
   }

   /*
    * Make the second device type listen to the timeslot that the first
    * device is transmitting on.  If a half duplex connection is desired,
    * then return.  Otherwise, get the SCBus timeslot connected to the
    * transmit of the second device.
    */
   switch (devtype2) {
   case SC_VOX:
      if (dx_listen(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot dx_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh2),scts,ATDV_ERRMSGP(devh2));
         return -1;
      }

      if (mode == SC_HALFDUP) {
         return 0;
      }

      if (dx_getxmitslot(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: dx_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         return -1;
      }
      break;

   case SC_LSI:
      if (ag_listen(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot ag_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh2),scts,ATDV_ERRMSGP(devh2));
         return -1;
      }

      if (mode == SC_HALFDUP) {
         return 0;
      }

      if (ag_getxmitslot(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: ag_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         return -1;
      }
      break;

#ifdef DTISC
   case SC_DTI:
      if (dt_listen(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot dt_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh2),scts,ATDV_ERRMSGP(devh2));
         return -1;
      }

      if (mode == SC_HALFDUP) {
         return 0;
      }

      if (dt_getxmitslot(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: dt_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         return -1;
      }
      break;
#endif

#ifdef MSISC
   case SC_MSI:
      if (ms_listen(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot ms_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh2),scts,ATDV_ERRMSGP(devh2));
         return -1;
      }

      if (mode == SC_HALFDUP) {
         return 0;
      }

      if (ms_getxmitslot(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: ms_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         return -1;
      }
      break;
#endif

#ifdef FAXSC
   case SC_FAX:
      if (fx_listen(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot fx_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh2),scts,ATDV_ERRMSGP(devh2));
         return -1;
      }

      if (mode == SC_HALFDUP) {
         return 0;
      }

      if (fx_getxmitslot(devh2, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: fx_getxmitslot ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         return -1;
      }
      break;
#endif

   default:
      nr_scerror("nr_scroute: %s: ERROR: Invalid 2nd device type\n",
                                                             ATDV_NAMEP(devh2));
      return -1;
   }

   /*
    * Now make the first device listen to the SCBus timeslot that the
    * second device is transmitting on.
    */
   switch (devtype1) {
   case SC_VOX:
      if (dx_listen(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot dx_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh1),scts,ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;

   case SC_LSI:
      if (ag_listen(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot ag_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh1),scts,ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;

#ifdef DTISC
   case SC_DTI:
      if (dt_listen(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot dt_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh1),scts,ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;
#endif

#ifdef MSISC
   case SC_MSI:
      if (ms_listen(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot ms_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh1),scts,ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;
#endif

#ifdef FAXSC
   case SC_FAX:
      if (fx_listen(devh1, &sc_tsinfo) == -1) {
         nr_scerror("nr_scroute: %s: Cannot fx_listen %d ERROR: %s\n",
                                    ATDV_NAMEP(devh1),scts,ATDV_ERRMSGP(devh1));
         return -1;
      }
      break;
#endif
   }

   return 0;
}


/***************************************************************************
 *        NAME: nr_scunroute(devh1, devtype1, devh2, devtype2, mode)
 * DESCRIPTION: Break the half or full duplex connection between two SCBus
 *              devices.
 *      INPUTS: int devh1;               - First SCBus device handle.
 *              unsigned short devtype1; - Specifies the type for devh1.
 *              int devh2;               - Second SCBus device handle.
 *              unsigned short devtype2; - Specifies the type for devh2.
 *              unsigned char mode;      - SC_FULLDUP or SC_HALFDUP.
 *     OUTPUTS: Nothing.
 *     RETURNS: 0 or -1 on error
 *************************************************************************/
#if ( defined( __STDC__ ) || defined( __cplusplus ) )
int nr_scunroute( int devh1, unsigned short devtype1,
                  int devh2, unsigned short devtype2, unsigned char mode )
#else
int nr_scunroute( devh1, devtype1, devh2, devtype2, mode )
   int            devh1;
   unsigned short devtype1;
   int            devh2;
   unsigned short devtype2;
   unsigned char  mode;
#endif
{
   short rc = 0;              /* Return code from the function */


   /*
    * Disconnect the receive of the second device from the SCBus timeslot.
    */
   switch (devtype2) {
   case SC_VOX:
      if (dx_unlisten(devh2) == -1) {
         nr_scerror("nr_scunroute: %s: dx_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         rc = -1;
      }
      break;

   case SC_LSI:
      if (ag_unlisten(devh2) == -1) {
         nr_scerror("nr_scunroute: %s: ag_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         rc = -1;
      }
      break;

#ifdef DTISC
   case SC_DTI:
      if (dt_unlisten(devh2) == -1) {
         nr_scerror("nr_scunroute: %s: dt_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         rc = -1;
      }
      break;
#endif

#ifdef MSISC
   case SC_MSI:
      if (ms_unlisten(devh2) == -1) {
         nr_scerror("nr_scunroute: %s: ms_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         rc = -1;
      }
      break;
#endif

#ifdef FAXSC
   case SC_FAX:
      if (fx_unlisten(devh2) == -1) {
         nr_scerror("nr_scunroute: %s: fx_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh2),ATDV_ERRMSGP(devh2));
         rc = -1;
      }
      break;
#endif

   default:
      nr_scerror("nr_scunroute: %s: ERROR: Invalid 2nd device type\n",
                                                             ATDV_NAMEP(devh2));
      rc = -1;
   }

   /*
    * A half duplex connection has already been broken.  If this is all that
    * is required, then return now.
    */
   if (mode == SC_HALFDUP) {
      return rc;
   }

   /*
    * Disconnect the receive of the first device from the SCBus timeslot.
    */
   switch (devtype1) {
   case SC_VOX:
      if (dx_unlisten(devh1) == -1) {
         nr_scerror("nr_scunroute: %s: dx_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         rc = -1;
      }
      break;

   case SC_LSI:
      if (ag_unlisten(devh1) == -1) {
         nr_scerror("nr_scunroute: %s: ag_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         rc = -1;
      }
      break;

#ifdef DTISC
   case SC_DTI:
      if (dt_unlisten(devh1) == -1) {
         nr_scerror("nr_scunroute: %s: dt_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         rc = -1;
      }
      break;
#endif

#ifdef MSISC
   case SC_MSI:
      if (ms_unlisten(devh1) == -1) {
         nr_scerror("nr_scunroute: %s: ms_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         rc = -1;
      }
      break;
#endif

#ifdef FAXSC
   case SC_FAX:
      if (fx_unlisten(devh1) == -1) {
         nr_scerror("nr_scunroute: %s: fx_unlisten ERROR: %s\n",
                                         ATDV_NAMEP(devh1),ATDV_ERRMSGP(devh1));
         rc = -1;
      }
      break;
#endif

   default:
      nr_scerror("nr_scunroute: %s: ERROR: Invalid 1st device type\n",
                                                             ATDV_NAMEP(devh1));
      rc = -1;
   }

   return rc;
}


/***************************************************************************
 *        NAME: nr_scerror(va_alist)
 * DESCRIPTION: This function takes in variable number of arguments and prints
 *              an error message before exit.
 *       INPUT: int va_alist; - The format string to use for printing the
 *                              error message followed by variable argument
 *                              list.
 *      OUTPUT: None.
 *     RETURNS: Nothing.
 ***************************************************************************/
static void nr_scerror(char* first, ...)
{
    fprintf(stderr, "hello?\n");
    fprintf(stderr, "%s\n", first);
//#ifdef PRINTON
   va_list args;
   char   *fmt;

   /*
    * Make args point to the 1st unnamed argument and then print to stderr.
    */
   va_start(args, first);
   fmt = va_arg(args, char *);
   vfprintf(stderr, fmt, args);
   va_end(args);
   fprintf(stderr, "hello.\n");
//#endif
}
