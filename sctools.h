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
