import unittest

from client_ip import client_ip, parse_trusted_proxies


class ClientIpTests(unittest.TestCase):
    def setUp(self):
        self.trusted = parse_trusted_proxies("127.0.0.1,10.0.0.0/8,::1")

    def test_untrusted_peer_cannot_spoof_forwarded_header(self):
        self.assertEqual(
            client_ip("203.0.113.9", "198.51.100.7", self.trusted),
            "203.0.113.9",
        )

    def test_trusted_proxy_uses_forwarded_client(self):
        self.assertEqual(
            client_ip("127.0.0.1", "203.0.113.9", self.trusted),
            "203.0.113.9",
        )

    def test_client_supplied_leftmost_value_cannot_bypass_limit(self):
        self.assertEqual(
            client_ip("10.0.0.3", "198.51.100.77, 203.0.113.9", self.trusted),
            "203.0.113.9",
        )

    def test_multiple_trusted_proxies_are_skipped(self):
        self.assertEqual(
            client_ip("10.0.0.3", "203.0.113.9, 10.0.0.2", self.trusted),
            "203.0.113.9",
        )

    def test_malformed_chain_fails_closed(self):
        self.assertEqual(
            client_ip("127.0.0.1", "not-an-ip, 203.0.113.9", self.trusted),
            "127.0.0.1",
        )

    def test_invalid_proxy_configuration_is_rejected(self):
        with self.assertRaises(ValueError):
            parse_trusted_proxies("127.0.0.1,not-a-network")


if __name__ == "__main__":
    unittest.main()
