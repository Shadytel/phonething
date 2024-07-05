#ifndef _COMMON_H_INCLUDED
#define _COMMON_H_INCLUDED

/* Checks to assist in source code cross-platform compatibility */
#if defined(__WIN32__) || defined(_WIN32)
        #define PLATFORM_WINDOWS
        #define _CRT_SECURE_NO_WARNINGS
        #include <Windows.h>
#else
        #define PLATFORM_UNIX

        #ifndef _XOPEN_SOURCE
                /* Needed for getopt() */
                #define _XOPEN_SOURCE 500
        #endif
#endif

/* Checks to assist in C version compatibility */
#if !defined(__STDC__) || (__STDC_VERSION__ < 199901L)
        typedef int bool;
        #define false 0
        #define true 1

    #define INLINE __attribute__((always_inline))
#else
        #include <stdbool.h>
    #define INLINE inline
#endif

#endif

