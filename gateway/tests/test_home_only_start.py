"""Home-only setups have no VPS relay and therefore no pinned CA file.

The gateway must still start; only a CA path that is configured but missing is
a startup error. (Found by a clean-room build on 2026-10-29: with the old
behaviour the gateway exited with "startup failed type=FileNotFoundError".)
"""

import os
import ssl
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import stackchan_gateway as gw  # noqa: E402

TOKEN_HASH = "a" * 64
STT_HASH = "b" * 64


def env(**extra):
    base = {
        "STACKCHAN_DEVICE_TOKEN_SHA256": TOKEN_HASH,
        "STACKCHAN_STT_TOKEN_SHA256": STT_HASH,
        "STACKCHAN_UPSTREAM_URL": "https://example.invalid/v1/chat/completions",
    }
    base.update(extra)
    return base


class HomeOnlyStartTest(unittest.TestCase):
    def test_unset_ca_and_missing_default_starts_with_system_trust(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(gw, "DEFAULT_UPSTREAM_CA", Path(d) / "absent.pem"), \
                mock.patch.dict(os.environ, env(), clear=True):
            cfg = gw.GatewayConfig.from_environment()
            self.assertIsNone(cfg.upstream_ca)
            app = gw.GatewayApplication(cfg)
            self.assertIsInstance(app._upstream_ssl_context, ssl.SSLContext)

    def test_empty_ca_starts_with_system_trust(self):
        with mock.patch.dict(os.environ, env(STACKCHAN_UPSTREAM_CA=""), clear=True):
            cfg = gw.GatewayConfig.from_environment()
            self.assertIsNone(cfg.upstream_ca)
            gw.GatewayApplication(cfg)

    def test_configured_but_missing_ca_still_refuses(self):
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.dict(os.environ, env(STACKCHAN_UPSTREAM_CA=str(Path(d) / "nope.pem")), clear=True):
            cfg = gw.GatewayConfig.from_environment()
            with self.assertRaises(FileNotFoundError):
                gw.GatewayApplication(cfg)


if __name__ == "__main__":
    unittest.main()
