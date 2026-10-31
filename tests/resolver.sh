# Sourced by the test runners. Every test runs on the machine alone (its servers listen on
# 127.0.0.1) except these, which also need the system resolver to map localhost to
# loopback and to fail for a .invalid name. A proxy that answers every name with one
# address (fake-ip setups hand out 198.18.0.0/15) breaks them: ESKIU_RESOLVER_TESTS=0
# skips them in every runner. CI keeps them on.
RESOLVER_TESTS=" dns_connect fetch_dns "

# Whether the test NAME is skipped here.
resolver_skip() {
    [ "${ESKIU_RESOLVER_TESTS:-1}" = 0 ] && [[ "$RESOLVER_TESTS" == *" $1 "* ]]
}
