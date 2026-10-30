// OpenSSL stand-in for http2_tls_engine.esk: an "SSL" is just the socket, read
// and written in the clear, with SSL_get_error reporting WANT_READ/WANT_WRITE
// on EAGAIN so the async TLS pump is exercised too.
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
typedef struct { int fd; int err; } S;
void* TLS_server_method(void){ return (void*)1; }
void* SSL_CTX_new(void* m){ return malloc(8); }
void SSL_CTX_free(void* c){ free(c); }
int SSL_CTX_use_certificate_file(void* c, const char* f, int t){ return 1; }
int SSL_CTX_use_PrivateKey_file(void* c, const char* f, int t){ return 1; }
void SSL_CTX_set_alpn_select_cb(void* c, void* cb, void* a){}
int SSL_CTX_check_private_key(void* c){ return 1; }
long SSL_CTX_ctrl(void* c, int cmd, long l, void* p){ return 1; }
unsigned long long SSL_CTX_set_options(void* c, unsigned long long o){ return o; }
void* SSL_new(void* c){ S* s = calloc(1, sizeof(S)); return s; }
int SSL_set_fd(void* p, int fd){ ((S*)p)->fd = fd; return 1; }
int SSL_get_fd(void* p){ return ((S*)p)->fd; }
int SSL_accept(void* p){ return 1; }
int SSL_read(void* p, void* b, int n){ S* s=p; ssize_t r = read(s->fd, b, n); if (r < 0) { s->err = (errno==EAGAIN)?2:5; return -1; } if (r==0){ s->err=6; return 0;} return (int)r; }
int SSL_write(void* p, void* b, int n){ S* s=p; ssize_t r = write(s->fd, b, n); if (r < 0) { s->err = (errno==EAGAIN)?3:5; return -1; } return (int)r; }
int SSL_shutdown(void* p){ return 1; }
void SSL_free(void* p){ free(p); }
int SSL_get_error(void* p, int ret){ return ((S*)p)->err; }
