#define PROXY_SERVER_PORT     9748
#define ANARCAST_SERVER_PORT  9209
#define INFORM_SERVER_PORT    7342
#define HASH_LEN              20
#define DEFAULT_INFORM_SERVER "localhost"
#define _GNU_SOURCE

#include <err.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

inline void
ioerror ()
{
    extern int errno;
    printf("I/O Error: %s.\n", strerror(errno));
}

inline int
readall (int c, const void *b, int len)
{
    int j = 0, i = 0;
    while ((i += j) < len)
	if ((j = read(c, &((char *)b)[i], len-i)) <= 0)
	    return i;
    return i;
}

inline int
writeall (int c, const void *b, int len)
{
    int j = 0, i = 0;
    while ((i += j) < len)
	if ((j = write(c, &((char *)b)[i], len-i)) <= 0)
	    return i;
    return i;
}

inline char *
mbuf (size_t len)
{
    int c;
    char *p, b[1024];
    
    strcpy(b, "/tmp/anarcast-XXXXXX");
    if ((c = mkstemp(b)) == -1)
	err(1, "mkstemp(3) failed");
    
    if (ftruncate(c, len) == -1)
	err(1, "ftruncate(2) failed");
    
    p = mmap(0, len, PROT_READ|PROT_WRITE, MAP_SHARED, c, 0);
    if (p == MAP_FAILED)
	err(1, "mmap(2) failed");
    
    close(c);
    return p;
}

inline void
bytestohex (char *hex, const void *bytes, int blen)
{
    static char hextable[] = "0123456789ABCDEF";
    for ( ; blen-- ; bytes++) {
        *hex++ = hextable[*(char *)bytes >> 4 & 0x0f];
        *hex++ = hextable[*(char *)bytes & 0x0f];
    }
    *hex = 0;
}

inline int
hextobytes (const char *hex, void *bytes, uint hlen)
{
    int i, j;
    char d;

    if (hlen & 1)
        return 0;
    j = 0;
    for (i = 0; i < hlen; i += 2) {
        d = 0;

        if (hex[i] >= 'a' && hex[i] <= 'f')
            d |= (hex[i] - 'a') + 10;
        else if (hex[i] >= 'A' && hex[i] <= 'F')
            d |= (hex[i] - 'A') + 10;
        else if (hex[i] >= '0' && hex[i] <= '9')
            d |= (hex[i] - '0');
        else
            return 0;
        d <<= 4;

        if (hex[i + 1] >= 'a' && hex[i + 1] <= 'f')
            d |= (hex[i + 1] - 'a') + 10;
        else if (hex[i + 1] >= 'A' && hex[i + 1] <= 'F')
            d |= (hex[i + 1] - 'A') + 10;
        else if (hex[i + 1] >= '0' && hex[i + 1] <= '9')
            d |= (hex[i + 1] - '0');
        else
            return 0;
        ((char *)bytes)[j++] = d;
    }

    return j;
}

inline char *
timestr ()
{
    long t = time(NULL);
    struct tm *tm = localtime(&t);
    static char ts[128];
    if (!strftime(ts, 128, "%T", tm))
	err(1, "strftime() failed");
    return ts;
}

int
listening_socket (int port)
{
    struct sockaddr_in a;
    int r = 1, s;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &r, sizeof(r));

    if (bind(s, &a, sizeof(a)) < 0)
        return -1;

    if (listen(s, SOMAXCONN) < 0)
        return -1;

    return s;
}

inline void
xor (void *a, const void *b, int len)
{
    int i;
    
    for (i = 0 ; i < len / sizeof(int) ; i++)
	((int *)a)[i] ^= ((int *)b)[i];
    
    i = len % sizeof(int);
    do ((char *)a)[len-i] ^= ((char *)b)[len-i];
    while (--i);
}
