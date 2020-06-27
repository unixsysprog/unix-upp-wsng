#ifndef WSNG_UTIL_H
#define WSNG_UTIL_H

#include    <sys/types.h>

#define DATE_FMT    "%b %e %H:%M"
#define MAXDATELEN  100

char *fmt_time( time_t timeval , char *fmt );
char *mode_to_letters( int mode, char str[] );
char *uid_to_name( uid_t uid );
char *gid_to_name( gid_t gid );

#endif
