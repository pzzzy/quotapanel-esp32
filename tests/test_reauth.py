# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import http.client
import io
import json
import sys
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import MagicMock, call, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import quotapanel as qp


class Reauth(unittest.TestCase):
    def test_selects_only_slots_that_need_sign_in(self):
        self.assertEqual(qp.account_targets({"accounts": [{"ok": True}, {}]}), [])
        self.assertEqual(
            qp.account_targets(
                {
                    "accounts": [
                        {"reauthRequired": False, "error": "SIGN IN AGAIN"},
                        {"error": "SIGN IN AGAIN"},
                    ]
                }
            ),
            [1],
        )
        self.assertEqual(
            qp.account_targets({"accounts": [{"reauthRequired": True}]}), [0]
        )

    def test_recovers_both_accounts_in_the_default_browser(self):
        device = MagicMock()
        device.__enter__.return_value = device
        device.query.side_effect = [
            {"standalone": True, "wifi": True},
            {"accounts": [{"reauthRequired": True}, {"reauthRequired": True}]},
            {"polls": 10},
            {"polls": 11, "accounts": [{"ok": True}, {"ok": True}]},
        ]
        with (
            patch.object(qp, "Device", return_value=device),
            patch.object(qp, "pair") as pair,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            qp.reauth("unused", None, False)
        self.assertEqual(
            pair.call_args_list,
            [
                call("unused", 0, False, expected=None),
                call("unused", 1, False, expected=None),
            ],
        )

    def test_verification_waits_for_a_fresh_successful_poll(self):
        device = MagicMock()
        device.query.side_effect = [
            {"polls": 5, "accounts": [{"ok": True}]},
            {"state": "polling"},
            {"polls": 6, "accounts": [{"ok": False}]},
            {"polls": 7, "accounts": [{"ok": True}]},
        ]
        with patch.object(qp.time, "sleep"):
            self.assertTrue(qp.wait_for_accounts(device, [0], after_poll=5))
        self.assertEqual(device.query.call_count, 4)

    def test_browser_callback_forwards_only_valid_state_without_printing_secrets(self):
        device = MagicMock()
        device.__enter__.return_value = device
        device.native.side_effect = [
            {
                "stateToken": "test-state",
                "authorizationUrl": "https://auth.openai.com/oauth/authorize?state=test-state",
            },
            {"persisted": True},
        ]
        create_server = qp.HTTPServer

        def local_server(address, handler):
            self.assertEqual(address, ("127.0.0.1", 1455))
            server = create_server((address[0], 0), handler)
            self.port = server.server_port
            return server

        def browser_requests():
            def get(path, host="127.0.0.1:1455"):
                connection = http.client.HTTPConnection(
                    "127.0.0.1", self.port, timeout=5
                )
                try:
                    connection.request("GET", path, headers={"Host": host})
                    response = connection.getresponse()
                    return response.status, response.read()
                finally:
                    connection.close()

            callback = "/auth/callback?code=test-code&state="
            self.assertEqual(get(callback + "test-state", "other.local")[0], 400)
            self.assertEqual(get(callback + "%C3%A9")[0], 400)
            self.assertEqual(get(callback + "test-state")[0], 303)
            self.assertIn(b"PRIMARY", get("/done")[1])

        with ThreadPoolExecutor(1) as executor:
            futures = []

            def open_browser(_):
                futures.append(executor.submit(browser_requests))
                return True

            output = io.StringIO()
            with (
                patch.object(qp, "Device", return_value=device),
                patch.object(qp, "HTTPServer", side_effect=local_server),
                patch.object(qp.webbrowser, "open", side_effect=open_browser),
                contextlib.redirect_stdout(output),
            ):
                qp.pair("unused", 0, False, None)
            futures[0].result()

        self.assertEqual(device.native.call_count, 2)
        self.assertEqual(
            json.loads(device.native.call_args.args[0].removeprefix("CALLBACK ")),
            {"code": "test-code", "state": "test-state"},
        )
        self.assertNotIn("test-code", output.getvalue())
        self.assertNotIn("test-state", output.getvalue())
