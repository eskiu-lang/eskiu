"""Resolve a rate-limit identity without trusting client-controlled proxy headers."""

from ipaddress import ip_address, ip_network


def parse_trusted_proxies(value: str):
    """Parse comma-separated proxy IPs or CIDRs, failing closed on bad config."""
    return tuple(
        ip_network(item.strip(), strict=False)
        for item in value.split(",")
        if item.strip()
    )


def client_ip(peer: str, forwarded_for: str | None, trusted_proxies) -> str:
    """Return the first untrusted hop, walking X-Forwarded-For right to left."""
    try:
        peer_ip = ip_address(peer)
    except ValueError:
        return peer

    if not any(peer_ip in network for network in trusted_proxies) or not forwarded_for:
        return peer_ip.compressed

    # Right to left: the entries our proxies appended come first, so the first untrusted hop
    # is the client as our proxies saw it. Whatever the client wrote further left (garbage
    # included) is never read, so it cannot move the request into another bucket. A malformed
    # entry before any untrusted hop falls back to the peer.
    for part in reversed(forwarded_for.split(",")):
        try:
            hop = ip_address(part.strip())
        except ValueError:
            return peer_ip.compressed
        if not any(hop in network for network in trusted_proxies):
            return hop.compressed
    return peer_ip.compressed
