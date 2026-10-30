// OpenSSL stand-in for tls_trickle_deadline.esk with a record layer like the
// real one: a record on the wire is one length byte and that many bytes, and
// SSL_accept / SSL_read keep calling read() until a whole record is in. On a
// blocking socket that loop waits for every byte (so SO_RCVTIMEO restarts per
// byte, as with OpenSSL); on a non-blocking one read() failing with EAGAIN is
// WANT_READ, and the partial record is kept for the next call.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
typedef struct {
    int fd; int err;
    unsigned char rec[256]; int have;
    int pos, len;
} S;
void* TLS_server_method(void){ return (void*)1; }
void* SSL_CTX_new(void* m){ return malloc(8); }
void SSL_CTX_free(void* c){ free(c); }
int SSL_CTX_use_certificate_file(void* c, const char* f, int t){ return 1; }
int SSL_CTX_use_PrivateKey_file(void* c, const char* f, int t){ return 1; }
void SSL_CTX_set_alpn_select_cb(void* c, void* cb, void* a){}
int SSL_CTX_check_private_key(void* c){ return 1; }
long SSL_CTX_ctrl(void* c, int cmd, long l, void* p){ return 1; }
unsigned long long SSL_CTX_set_options(void* c, unsigned long long o){ return o; }
void* SSL_new(void* c){ return calloc(1, sizeof(S)); }
int SSL_set_fd(void* p, int fd){ ((S*)p)->fd = fd; return 1; }
int SSL_get_fd(void* p){ return ((S*)p)->fd; }
static int record(S* s) {
    for (;;) {
        int need = s->have == 0 ? 1 : 1 + s->rec[0];
        if (s->have >= need) { s->pos = 1; s->len = need; s->have = 0; return 1; }
        ssize_t r = read(s->fd, s->rec + s->have, need - s->have);
        if (r < 0) { s->err = errno == EAGAIN ? 2 : 5; return -1; }
        if (r == 0) { s->err = 6; return 0; }
        s->have += (int)r;
    }
}
int SSL_accept(void* p){ S* s = p; int r = record(s); if (r > 0) { s->pos = s->len = 0; } return r; }
int SSL_read(void* p, void* b, int n){
    S* s = p;
    if (s->pos >= s->len) { int r = record(s); if (r <= 0) { return r; } }
    int k = s->len - s->pos;
    if (k > n) { k = n; }
    memcpy(b, s->rec + s->pos, k);
    s->pos += k;
    return k;
}
int SSL_write(void* p, void* b, int n){ S* s=p; ssize_t r = write(s->fd, b, n); if (r < 0) { s->err = (errno==EAGAIN)?3:5; return -1; } return (int)r; }
int SSL_shutdown(void* p){ return 1; }
void SSL_free(void* p){ free(p); }
int SSL_get_error(void* p, int ret){ return ((S*)p)->err; }
