##########@@@SOFT@@@WARE@@@COPY@@@RIGHT@@@###################################
# DIALOGIC CONFIDENTIAL
#
# Copyright (C) 1990-2007 Dialogic Corporation. All Rights Reserved.
# The source code contained or described herein and all documents related
# to the source code ("Material") are owned by Dialogic Corporation or its
# suppliers or licensors. Title to the Material remains with Dialogic Corporation
# or its suppliers and licensors. The Material contains trade secrets and
# proprietary and confidential information of Dialogic or its suppliers and
# licensors. The Material is protected by worldwide copyright and trade secret
# laws and treaty provisions. No part of the Material may be used, copied,
# reproduced, modified, published, uploaded, posted, transmitted, distributed,
# or disclosed in any way without Dialogic's prior express written permission.
#
# No license under any patent, copyright, trade secret or other intellectual
# property right is granted to or conferred upon you by disclosure or delivery
# of the Materials, either expressly, by implication, inducement, estoppel or
# otherwise. Any license under such intellectual property rights must be
# express and approved by Dialogic in writing.
#
###################################@@@SOFT@@@WARE@@@COPY@@@RIGHT@@@##########
#******************************************************************
#             R E V I S I O N    H I S T O R Y
#
# date______  initials  comments__________________________________
#
# 06-16-1999  PH        Modified to fix PTR14297, unable to make
#                       and make clean goes into infinite loop.
#
#******************************************************************

#
# Flags
#

CFLAGS= -g -O2 -I${INTEL_DIALOGIC_INC} -DLINUX -Wall -Wextra -std=c99
SCFLAGS= -I${INTEL_DIALOGIC_DIR}/sctools
LFLAGS=-cu

#
# Programming Tools
#

CC=cc
LINT=lint

#
# Dialogic Application Tool Kit
#


#
# Updated for PTR# 14297 - SD
#

SCTOOLS_DIR= ./
SCTOOLS= $(SCTOOLS_DIR)sctools

SCTOOLS_SRC= ${INTEL_DIALOGIC_DIR}/sctools/sctools.c

#
# End of PTR# 14297 - SD
#
# Lint Files
#

#
# Demonstration Programs
#



ANSR_DIR= ./
SQLCALLBACK= $(ANSR_DIR)sqlcallback
CBANSR= $(ANSR_DIR)cbansr
CBANSR_ISDN= $(ANSR_DIR)cbansr_isdn
CONFIGURATION= $(ANSR_DIR)configuration
DISPLAY= $(ANSR_DIR)display
DIALER_LIST= $(ANSR_DIR)dialer_list
VER_DIR=./

DEMOS= $(CBANSR)

#
# Includes
#

VTINCLUDES= $(VTINCLUDE)digits.h $(VTINCLUDE)menu.h $(VTINCLUDE)date.h 


################################################################################
# Dependency Lines
################################################################################
all:	$(DEMOS)

#
# dialer_list
#
$(DIALER_LIST).o: $(DIALER_LIST).c $(DIALER_LIST).h
	cd $(@D) ; $(CC) $(CFLAGS) $(SCFLAGS) -c $(<F) -m32
#
#configuration
#
$(CONFIGURATION).o: $(CONFIGURATION).c $(CONFIGURATION).h
	cd $(@D) ; $(CC) $(CFLAGS) $(SCFLAGS) -c $(<F) -m32
#
# cbansr 
#
$(CBANSR): $(DISPLAY).o $(CBANSR_ISDN).o $(SCTOOLS).o $(CBANSR).o $(CONFIGURATION).o $(DIALER_LIST).o $(SQLCALLBACK).o
	$(CC) -o$@ $@.o $(SCTOOLS).o $(DISPLAY).o $(CBANSR_ISDN).o $(CONFIGURATION).o $(DIALER_LIST).o $(SQLCALLBACK).o -ldxxx -ldti -lsrl -lcurses -lgc -lsqlite3 -m32

$(CBANSR).o: $(CBANSR).c $(ANSR_DIR)answer.h $(ANSR_DIR)sql.h /usr/dialogic/inc/gcisdn.h /usr/dialogic/inc/gclib.h
	cd $(@D) ; $(CC) $(CFLAGS) $(SCFLAGS) -c $(<F) -m32

$(CBANSR_ISDN).o: $(CBANSR_ISDN).c $(ANSR_DIR)answer.h /usr/dialogic/inc/gcisdn.h /usr/dialogic/inc/gclib.h
	cd $(@D) ; $(CC) $(CFLAGS) $(SCFLAGS) -c $(<F) -m32 

$(CBANSR).ln: $(CBANSR).c $(ANSR_DIR)answer.h
	cd  $(@D) ; $(LINT) $(LFLAGS) `basename $(CBANSR)`.c 
	$(LINT) -u $(CBANSR).ln -ld4xt

#$(CBANSR): $(DISPLAY).o  $(SCTOOLS).o $(CBANSR).o
#	$(CC) -o$@ $@.o $(SCTOOLS).o $(DISPLAY).o -ldxxx -ldti -lsrl -lcurses


#$(CBANSR).o: $(CBANSR).c $(ANSR_DIR)answer.h
#	cd $(@D) ; $(CC) $(CFLAGS) $(SCFLAGS) -c $(<F)

#$(CBANSR).ln: $(CBANSR).c $(ANSR_DIR)answer.h
#	cd  $(@D) ; $(LINT) $(LFLAGS) `basename $(CBANSR)`.c 
#	$(LINT) -u $(CBANSR).ln -ld4xt

#
# sctools - Updated for PTR# 14297 - SD
#

$(SCTOOLS).o: $(SCTOOLS_SRC)
	cd $(SCTOOLS_DIR); $(CC) $(SCFLAGS) -DDTISC -c $(SCTOOLS_SRC) -m32

#
#sqlcallback
#
$(SQLCALLBACK).o: $(SQLCALLBACK).c $(ANSR_DIR)sql.h
	cd $(@D) ; $(CC) -I ../include $(CFLAGS) -c $(<F) -lsqlite3 -m32
#
#display
#

$(DISPLAY).o: $(DISPLAY).c $(CBANSR).c
	cd $(@D) ; $(CC) -I ../include $(CFLAGS) -c $(<F) -m32 

#
#cbansr_isdn
#

$(CBANSR_ISDN).o: $(CBANSR_ISDN).c
	cd $(@D) ; $(CC) -I ../include $(CFLAGS) $(SCFLAGS) -c $(<F) -m32

#
# Lint
#
lint:	$(LINTFILES) 

clean:
	-rm -f $(CBANSR)
	-rm -f $(CBANSR).o
	-rm -f $(CBANSR_ISDN).o
	-rm -f $(DISPLAY).o
	-rm -f $(SCTOOLS).o
	-rm -f $(SQLCALLBACK).o
	-rm -f $(CONFIGURATION).o
