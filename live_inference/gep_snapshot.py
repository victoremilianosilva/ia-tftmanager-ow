"""Validation and sequencing for strategic snapshots emitted by the Overwolf GEP collector.

This module deliberately does not interpret game-specific fields. The collector owns
partial-update reconciliation; this boundary validates the versioned envelope and
prevents stale/incomplete snapshots from reaching live inference.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import json
from typing import Any, Mapping


class SnapshotValidationError(ValueError):
    """Raised when a GEP snapshot violates the documented transport contract."""


_REQUIRED_FIELDS = {
    "schemaVersion",
    "gameId",
    "revision",
    "capturedAt",
    "complete",
    "info",
}
_FORBIDDEN_KEYS = {
    "token",
    "credentials",
    "lcuinfo",
    "puuid",
    "accountid",
    "summonerid",
    "playerpuuid",
    "riotid",
    "platformid",
}
_DEFAULT_MAX_BYTES = 16 * 1024 * 1024


@dataclass(frozen=True)
class GepSnapshot:
    schema_version: int
    game_id: int
    revision: int
    captured_at: str
    complete: bool
    info: Mapping[str, Any]

    @classmethod
    def parse(
        cls,
        payload: Any,
        *,
        max_bytes: int = _DEFAULT_MAX_BYTES,
    ) -> "GepSnapshot":
        if not isinstance(payload, Mapping):
            raise SnapshotValidationError("snapshot must be a JSON object")
        missing = _REQUIRED_FIELDS.difference(payload.keys())
        if missing:
            raise SnapshotValidationError(
                "snapshot is missing required fields: " + ", ".join(sorted(missing))
            )

        schema_version = payload["schemaVersion"]
        game_id = payload["gameId"]
        revision = payload["revision"]
        complete = payload["complete"]
        captured_at = payload["capturedAt"]
        info = payload["info"]

        if type(schema_version) is not int or schema_version != 1:
            raise SnapshotValidationError("unsupported schemaVersion; expected integer 1")
        if type(game_id) is not int or game_id <= 0:
            raise SnapshotValidationError("gameId must be a positive integer")
        if type(revision) is not int or revision < 0:
            raise SnapshotValidationError("revision must be a non-negative integer")
        if type(complete) is not bool:
            raise SnapshotValidationError("complete must be a boolean")
        if not isinstance(captured_at, str) or not captured_at.strip():
            raise SnapshotValidationError("capturedAt must be a non-empty ISO-8601 string")
        try:
            datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
        except ValueError as exc:
            raise SnapshotValidationError("capturedAt must be a valid ISO-8601 timestamp") from exc
        if not isinstance(info, Mapping):
            raise SnapshotValidationError("info must be a JSON object")

        _reject_sensitive_keys(info)
        try:
            encoded = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise SnapshotValidationError("snapshot must contain finite JSON values only") from exc
        if len(encoded) > max_bytes:
            raise SnapshotValidationError(
                f"snapshot exceeds the configured {max_bytes}-byte limit"
            )

        return cls(
            schema_version=schema_version,
            game_id=game_id,
            revision=revision,
            captured_at=captured_at,
            complete=complete,
            info=dict(info),
        )


def _reject_sensitive_keys(value: Any, *, depth: int = 0) -> None:
    if depth > 64:
        raise SnapshotValidationError("snapshot nesting exceeds 64 levels")
    if isinstance(value, Mapping):
        for key, nested in value.items():
            if not isinstance(key, str):
                raise SnapshotValidationError("snapshot object keys must be strings")
            normalized = "".join(character for character in key.lower() if character.isalnum())
            if normalized in _FORBIDDEN_KEYS or "credential" in normalized or "puuid" in normalized:
                raise SnapshotValidationError(
                    f"snapshot contains a prohibited sensitive field: {key}"
                )
            _reject_sensitive_keys(nested, depth=depth + 1)
    elif isinstance(value, list):
        for nested in value:
            _reject_sensitive_keys(nested, depth=depth + 1)


@dataclass(frozen=True)
class SnapshotDecision:
    status: str
    snapshot: GepSnapshot
    ready_for_inference: bool


class SnapshotSequence:
    """Tracks one game session and rejects stale revisions.

    A new gameId starts a fresh sequence. A snapshot is inference-ready only after
    the collector has reconciled its baseline and marks complete=true.
    """

    def __init__(self) -> None:
        self._game_id: int | None = None
        self._revision: int | None = None
        self._ready = False

    def ingest(self, payload: Any) -> SnapshotDecision:
        snapshot = GepSnapshot.parse(payload)

        if self._game_id != snapshot.game_id:
            self._game_id = snapshot.game_id
            self._revision = None
            self._ready = False

        if self._revision is not None:
            if snapshot.revision < self._revision:
                return SnapshotDecision("stale", snapshot, False)
            if snapshot.revision == self._revision:
                return SnapshotDecision("duplicate", snapshot, self._ready)

        self._revision = snapshot.revision
        if not snapshot.complete:
            self._ready = False
            return SnapshotDecision("awaiting_baseline", snapshot, False)

        self._ready = True
        return SnapshotDecision("ready", snapshot, True)

    def reset(self) -> None:
        self._game_id = None
        self._revision = None
        self._ready = False
