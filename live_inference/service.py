"""Loopback HTTP receiver for canonical Overwolf TFT snapshots.

This service validates and sequences snapshots only. It intentionally does not
construct native observations, masks, or policy recommendations.
"""
from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import threading
from typing import Any

from live_inference.gep_snapshot import SnapshotSequence, SnapshotValidationError

MAX_BODY_BYTES = 16 * 1024 * 1024


class SnapshotReceiver:
    def __init__(self) -> None:
        self._sequence = SnapshotSequence()
        self._lock = threading.Lock()
        self._latest: dict[str, Any] | None = None
        self._latest_status = "waiting_for_snapshot"
        self._accepted = 0
        self._rejected = 0

    def ingest(self, payload: Any) -> dict[str, Any]:
        with self._lock:
            try:
                decision = self._sequence.ingest(payload)
            except SnapshotValidationError:
                self._rejected += 1
                raise
            if decision.status not in {"duplicate", "stale"}:
                self._latest = dict(payload)
                self._latest_status = decision.status
                self._accepted += 1
            return {
                "status": decision.status,
                "snapshotReady": decision.ready_for_inference,
                "executed": False,
                "inferenceAvailable": False,
                "revision": decision.snapshot.revision,
                "sessionId": decision.snapshot.session_id,
                "matchId": decision.snapshot.state["matchId"]["value"]
                if decision.snapshot.state["matchId"]["known"] else None,
            }

    def health(self) -> dict[str, Any]:
        with self._lock:
            latest = self._latest or {}
            state = latest.get("state", {})
            match = state.get("matchId", {})
            return {
                "status": "ok",
                "service": "tft-live-snapshot-receiver",
                "latestStatus": self._latest_status,
                "latestRevision": latest.get("revision"),
                "sessionId": latest.get("sessionId"),
                "matchId": match.get("value") if match.get("known") else None,
                "snapshotReady": self._latest_status == "ready",
                "inferenceAvailable": False,
                "acceptedSnapshots": self._accepted,
                "rejectedSnapshots": self._rejected,
            }


def make_handler(receiver: SnapshotReceiver):
    class Handler(BaseHTTPRequestHandler):
        server_version = "TFTLiveSnapshot/1"

        def _write_json(self, status: int, value: dict[str, Any]) -> None:
            body = json.dumps(value, separators=(",", ":"), allow_nan=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            if self.path != "/health":
                self._write_json(404, {"error": "not_found"})
                return
            self._write_json(200, receiver.health())

        def do_POST(self) -> None:
            if self.path != "/v1/snapshot":
                self._write_json(404, {"error": "not_found"})
                return
            content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
            if content_type != "application/json":
                self._write_json(415, {"error": "content_type_must_be_application_json"})
                return
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                self._write_json(411, {"error": "content_length_required"})
                return
            if length <= 0 or length > MAX_BODY_BYTES:
                self._write_json(413, {"error": "invalid_or_oversized_body"})
                return
            try:
                raw = self.rfile.read(length)
                if len(raw) != length:
                    self._write_json(400, {"error": "incomplete_body"})
                    return
                payload = json.loads(raw.decode("utf-8"))
                result = receiver.ingest(payload)
            except (UnicodeDecodeError, json.JSONDecodeError):
                self._write_json(400, {"error": "invalid_json"})
                return
            except SnapshotValidationError as exc:
                self._write_json(422, {"error": "invalid_snapshot", "message": str(exc)})
                return
            self._write_json(200, result)

        def log_message(self, _format: str, *_args: Any) -> None:
            return

    return Handler


def serve(host: str = "127.0.0.1", port: int = 8765) -> None:
    try:
        address = ipaddress.ip_address(host)
    except ValueError as exc:
        raise ValueError("host must be a loopback IP address") from exc
    if not address.is_loopback:
        raise ValueError("refusing to expose the live snapshot receiver outside loopback")
    server = ThreadingHTTPServer((host, port), make_handler(SnapshotReceiver()))
    server.daemon_threads = True
    print(f"TFT live snapshot receiver listening on http://{host}:{server.server_port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Receive canonical TFT snapshots from Overwolf")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    serve(args.host, args.port)


if __name__ == "__main__":
    main()
