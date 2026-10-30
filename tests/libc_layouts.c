// The C side of libc_layouts.esk: sizes and field offsets of the libc records the stdlib
// declares by hand, as this platform's C compiler lays them out.
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif
#ifdef __linux__
#include <sys/epoll.h>
#endif

int64_t c_layout(int what) {
    switch (what) {
    case 0: return sizeof(struct addrinfo);
    case 1: return offsetof(struct addrinfo, ai_flags);
    case 2: return offsetof(struct addrinfo, ai_family);
    case 3: return offsetof(struct addrinfo, ai_socktype);
    case 4: return offsetof(struct addrinfo, ai_protocol);
    case 5: return offsetof(struct addrinfo, ai_addrlen);
    case 6: return offsetof(struct addrinfo, ai_canonname);
    case 7: return offsetof(struct addrinfo, ai_addr);
    case 8: return offsetof(struct addrinfo, ai_next);
    case 9: return sizeof(((struct addrinfo*)0)->ai_addrlen);
    case 10: return sizeof(struct sockaddr_in);
    case 11: return offsetof(struct sockaddr_in, sin_family);
    case 12: return sizeof(((struct sockaddr_in*)0)->sin_family);
    case 13: return offsetof(struct sockaddr_in, sin_port);
    case 14: return offsetof(struct sockaddr_in, sin_addr);
    case 20: return sizeof(struct pollfd);
    case 21: return offsetof(struct pollfd, events);
    case 22: return offsetof(struct pollfd, revents);
    case 30: return sizeof(struct timeval);
    case 31: return offsetof(struct timeval, tv_usec);
    case 32: return sizeof(struct timespec);
    case 33: return offsetof(struct timespec, tv_nsec);
    case 40: return sizeof(struct tm);
    case 41: return offsetof(struct tm, tm_isdst);
#ifdef __APPLE__
    case 50: return sizeof(struct kevent);
    case 51: return offsetof(struct kevent, filter);
    case 52: return offsetof(struct kevent, flags);
    case 53: return offsetof(struct kevent, fflags);
    case 54: return offsetof(struct kevent, data);
    case 55: return offsetof(struct kevent, udata);
#endif
#ifdef __linux__
    case 50: return sizeof(struct epoll_event);
    case 51: return offsetof(struct epoll_event, data);
#endif
    }
    return -1;
}
