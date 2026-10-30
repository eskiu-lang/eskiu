// OpenSSL stand-in for tls_ctx_hardening.esk: records what tls_server_ctx sets on the
// context, and fails SSL_CTX_check_private_key for the key file named "wrong".
#include <stdlib.h>
#include <string.h>
static long min_version;
static unsigned long long options;
static int key_ok = 1;
void* TLS_server_method(void){ return (void*)1; }
void* SSL_CTX_new(void* m){ min_version = 0; options = 0; return malloc(8); }
void SSL_CTX_free(void* c){ free(c); }
int SSL_CTX_use_certificate_file(void* c, const char* f, int t){ return 1; }
int SSL_CTX_use_PrivateKey_file(void* c, const char* f, int t){ key_ok = strcmp(f, "wrong") != 0; return 1; }
void SSL_CTX_set_alpn_select_cb(void* c, void* cb, void* a){}
int SSL_CTX_check_private_key(void* c){ return key_ok; }
long SSL_CTX_ctrl(void* c, int cmd, long l, void* p){ if (cmd == 123) min_version = l; return 1; }
unsigned long long SSL_CTX_set_options(void* c, unsigned long long o){ options |= o; return options; }
void* SSL_new(void* c){ return 0; }
int SSL_set_fd(void* p, int fd){ return 1; }
int SSL_get_fd(void* p){ return -1; }
int SSL_accept(void* p){ return 1; }
int SSL_read(void* p, void* b, int n){ return 0; }
int SSL_write(void* p, void* b, int n){ return 0; }
int SSL_shutdown(void* p){ return 1; }
void SSL_free(void* p){}
int SSL_get_error(void* p, int ret){ return 5; }
long tls_test_min_version(void){ return min_version; }
unsigned long long tls_test_options(void){ return options; }
