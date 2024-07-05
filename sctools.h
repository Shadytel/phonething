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

/**
 ** Definitions
 **/
#define SC_VOX       0x01                       /* Voice channel device */
#define SC_LSI       0x02                       /* Analog Timeslot device */
#define SC_DTI       0x03                       /* Digital Timeslot device */
#define SC_FAX       0x04                       /* Fax channel device */
#define SC_MSI       0x05                       /* MSI channel device */

#define SC_FULLDUP   0x00                       /* Full duplex connection */
#define SC_HALFDUP   0x01                       /* Half duplex connection */


/**
 ** Function Prototypes
 **/
#ifdef __cplusplus
extern "C" {   // C++ func bindings to enable C funcs to be called from C++
#define extern
#endif

#if ( defined( __STDC__ ) || defined( __cplusplus ) )
int nr_scroute( int, unsigned short, int, unsigned short, unsigned char );
int nr_scunroute( int, unsigned short, int, unsigned short, unsigned char );
#else
int nr_scroute();
int nr_scunroute();
#endif

#ifdef __cplusplus
}
#undef extern
#endif
