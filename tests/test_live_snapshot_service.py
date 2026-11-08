"""Tests for the loopback canonical snapshot receiver."""
from __future__ import annotations

from http.server import ThreadingHTTPServer
import http.client
import json
import threading
import unittest

from live_inference.service import SnapshotReceiver, make_handler, serve


def snapshot(revision: int = 1, complete: bool = True) -> dict:
    unknown = {"known": False, "value": None}
    board = {"known": True, "slots": [{"known": True, "unit": None} for _ in range(28)]}
    bench = {"known": True, "slots": [{"known": True, "unit": None} for _ in range(9)]}
    shop = {"known": True, "slots": [{"known": True, "value": None} for _ in range(5)]}
    player = {
        "index": 0, "localPlayer": True,
        "hp": {"known": True, "value": 84}, "gold": {"known": True, "value": 35},
        "level": {"known": True, "value": 4}, "xp": {"known": True, "value": 4},
        "alive": {"known": True, "value": True},
        "board": board, "bench": bench, "shop": shop,
        "itemComponents": {"known": True, "value": []},
    }
    state = {
        "contract": "tft.live-state", "contractVersion": 1,
        "matchId": {"known": True, "value": "match-123"},
        "stage": {"known": True, "value": 2}, "round": {"known": True, "value": 6},
        "phase": unknown, "localPlayerIndex": {"known": True, "value": 0},
        "players": [player] + [None] * 7,
        "carousel": {"known": True, "value": []},
    }
    return {
        "schemaVersion": 2, "gameId": 21570, "sessionId": "session-a",
        "revision": revision, "capturedAt": "2026-11-08T16:00:00Z",
        "complete": complete, "state": state,
    }


class SnapshotReceiverTests(unittest.TestCase):
    def test_accepts_complete_snapshot_but_does_not_claim_inference(self):
        receiver = SnapshotReceiver()
        result = receiver.ingest(snapshot())
        self.assertEqual(result["status"], "ready")
        self.assertTrue(result["snapshotReady"])
        self.assertFalse(result["inferenceAvailable"])
        self.assertFalse(result["executed"])
        self.assertEqual(receiver.health()["latestRevision"], 1)

    def test_duplicate_and_stale_revisions_do_not_replace_latest(self):
        receiver = SnapshotReceiver()
        receiver.ingest(snapshot(revision=5))
        duplicate = receiver.ingest(snapshot(revision=5))
        stale = receiver.ingest(snapshot(revision=4))
        self.assertEqual(duplicate["status"], "duplicate")
        self.assertEqual(stale["status"], "stale")
        self.assertEqual(receiver.health()["latestRevision"], 5)

    def test_http_health_and_snapshot_endpoints(self):
        receiver = SnapshotReceiver()
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(receiver))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            body = json.dumps(snapshot()).encode("utf-8")
            connection.request("POST", "/v1/snapshot", body=body,
                               headers={"Content-Type": "application/json"})
            response = connection.getresponse()
            payload = json.loads(response.read())
            self.assertEqual(response.status, 200)
            self.assertEqual(payload["status"], "ready")
            self.assertFalse(payload["inferenceAvailable"])

            connection.request("GET", "/health")
            health_response = connection.getresponse()
            health = json.loads(health_response.read())
            self.assertEqual(health_response.status, 200)
            self.assertEqual(health["latestRevision"], 1)
            self.assertFalse(health["inferenceAvailable"])
            connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    def test_receiver_refuses_non_loopback_bind(self):
        with self.assertRaises(ValueError):
            serve("0.0.0.0", 8765)


if __name__ == "__main__":
    unittest.main()
