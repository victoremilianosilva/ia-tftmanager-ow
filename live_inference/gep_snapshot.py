"""Validation and sequencing for the canonical live-state contract emitted by Overwolf.

This module intentionally contains no GEP feature-path mapping. Electron owns all
GEP interpretation; Python validates the versioned envelope and canonical DTO only.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import json
from typing import Any, Callable, Mapping


class SnapshotValidationError(ValueError):
    """Raised when a canonical TFT snapshot violates its transport contract."""


_REQUIRED_FIELDS = {"schemaVersion", "gameId", "revision", "capturedAt", "complete", "state"}
_FORBIDDEN_KEYS = {
    "token", "credentials", "lcuinfo", "puuid", "accountid", "summonerid",
    "playerpuuid", "riotid", "platformid",
}
_DEFAULT_MAX_BYTES = 16 * 1024 * 1024


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise SnapshotValidationError(f"{name} must be an object")
    return value


def _known(value: Any, name: str, validator: Callable[[Any], bool]) -> None:
    field = _mapping(value, name)
    if type(field.get("known")) is not bool:
        raise SnapshotValidationError(f"{name}.known must be a boolean")
    raw = field.get("value")
    if not field["known"]:
        if raw is not None:
            raise SnapshotValidationError(f"{name} unknown value must be null")
    elif raw is None or not validator(raw):
        raise SnapshotValidationError(f"{name}.value is invalid")


def _number(value: Any) -> bool:
    return type(value) in (int, float) and value == value and abs(value) != float("inf")


def _integer(value: Any) -> bool:
    return type(value) is int


def _text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _validate_unit(value: Any, name: str) -> None:
    unit = _mapping(value, name)
    if not _text(unit.get("apiName")):
        raise SnapshotValidationError(f"{name}.apiName must be a non-empty string")
    _known(unit.get("star"), f"{name}.star", lambda x: type(x) is int and 1 <= x <= 3)
    items = unit.get("items")
    if not isinstance(items, list) or any(not _text(item) for item in items):
        raise SnapshotValidationError(f"{name}.items must be an array of item API names")


def _validate_slots(value: Any, name: str, size: int) -> None:
    container = _mapping(value, name)
    if type(container.get("known")) is not bool:
        raise SnapshotValidationError(f"{name}.known must be a boolean")
    slots = container.get("slots")
    if not container["known"]:
        if slots != []:
            raise SnapshotValidationError(f"{name} unknown slots must be an empty array")
        return
    if not isinstance(slots, list) or len(slots) != size:
        raise SnapshotValidationError(f"{name}.slots must contain exactly {size} slots")
    for index, raw in enumerate(slots):
        slot = _mapping(raw, f"{name}.slots[{index}]")
        if type(slot.get("known")) is not bool:
            raise SnapshotValidationError(f"{name}.slots[{index}].known must be a boolean")
        unit = slot.get("unit")
        if not slot["known"]:
            if unit is not None:
                raise SnapshotValidationError(f"{name}.slots[{index}] unknown unit must be null")
        elif unit is not None:
            _validate_unit(unit, f"{name}.slots[{index}].unit")


def _validate_shop(value: Any, name: str) -> None:
    container = _mapping(value, name)
    if type(container.get("known")) is not bool:
        raise SnapshotValidationError(f"{name}.known must be a boolean")
    slots = container.get("slots")
    if not container["known"]:
        if slots != []:
            raise SnapshotValidationError(f"{name} unknown slots must be an empty array")
        return
    if not isinstance(slots, list) or len(slots) != 5:
        raise SnapshotValidationError(f"{name}.slots must contain exactly 5 slots")
    for index, raw in enumerate(slots):
        slot = _mapping(raw, f"{name}.slots[{index}]")
        if type(slot.get("known")) is not bool:
            raise SnapshotValidationError(f"{name}.slots[{index}].known must be a boolean")
        offer = slot.get("value")
        if not slot["known"] and offer is not None:
            raise SnapshotValidationError(f"{name}.slots[{index}] unknown value must be null")
        if slot["known"] and offer is not None and not _text(offer):
            raise SnapshotValidationError(f"{name}.slots[{index}] offer must be a champion API name or null")


def _validate_state(value: Any) -> None:
    state = _mapping(value, "state")
    if state.get("contract") != "tft.live-state":
        raise SnapshotValidationError("state.contract must be 'tft.live-state'")
    if type(state.get("contractVersion")) is not int or state["contractVersion"] != 1:
        raise SnapshotValidationError("unsupported state.contractVersion; expected integer 1")
    _known(state.get("stage"), "state.stage", _integer)
    _known(state.get("round"), "state.round", _integer)
    _known(state.get("phase"), "state.phase", _text)
    _known(state.get("localPlayerIndex"), "state.localPlayerIndex", lambda x: _integer(x) and 0 <= x < 8)

    players = state.get("players")
    if not isinstance(players, list) or len(players) != 8:
        raise SnapshotValidationError("state.players must contain exactly 8 entries")
    local_index = state["localPlayerIndex"]["value"] if state["localPlayerIndex"]["known"] else None
    for index, raw in enumerate(players):
        if raw is None:
            if index == local_index:
                raise SnapshotValidationError("localPlayerIndex points to a missing player")
            continue
        player = _mapping(raw, f"state.players[{index}]")
        if type(player.get("index")) is not int or player["index"] != index:
            raise SnapshotValidationError(f"state.players[{index}].index must match its array position")
        if type(player.get("localPlayer")) is not bool:
            raise SnapshotValidationError(f"state.players[{index}].localPlayer must be a boolean")
        if player["localPlayer"] != (index == local_index):
            raise SnapshotValidationError("localPlayer flags must agree with localPlayerIndex")
        _known(player.get("hp"), f"players[{index}].hp", lambda x: _number(x) and 0 <= x <= 100)
        _known(player.get("gold"), f"players[{index}].gold", lambda x: _integer(x) and x >= 0)
        _known(player.get("level"), f"players[{index}].level", lambda x: _integer(x) and x >= 1)
        _known(player.get("xp"), f"players[{index}].xp", lambda x: _integer(x) and x >= 0)
        _known(player.get("alive"), f"players[{index}].alive", lambda x: type(x) is bool)
        _validate_slots(player.get("board"), f"players[{index}].board", 28)
        _validate_slots(player.get("bench"), f"players[{index}].bench", 9)
        _validate_shop(player.get("shop"), f"players[{index}].shop")
        components = player.get("itemComponents")
        _known(components, f"players[{index}].itemComponents", lambda x: isinstance(x, list))
        if components["known"]:
            for item in components["value"]:
                entry = _mapping(item, "itemComponents[]")
                if not _text(entry.get("apiName")) or type(entry.get("count")) is not int or entry["count"] <= 0:
                    raise SnapshotValidationError("item component entries require apiName and positive count")

    carousel = state.get("carousel")
    _known(carousel, "state.carousel", lambda x: isinstance(x, list))
    if carousel["known"]:
        for entry in carousel["value"]:
            item = _mapping(entry, "state.carousel[]")
            if not isinstance(item.get("items"), list) or any(not _text(x) for x in item["items"]):
                raise SnapshotValidationError("carousel items must be API-name arrays")


@dataclass(frozen=True)
class GepSnapshot:
    schema_version: int
    game_id: int
    revision: int
    captured_at: str
    complete: bool
    state: Mapping[str, Any]

    @classmethod
    def parse(cls, payload: Any, *, max_bytes: int = _DEFAULT_MAX_BYTES) -> "GepSnapshot":
        if not isinstance(payload, Mapping):
            raise SnapshotValidationError("snapshot must be a JSON object")
        missing = _REQUIRED_FIELDS.difference(payload.keys())
        if missing:
            raise SnapshotValidationError("snapshot is missing required fields: " + ", ".join(sorted(missing)))
        schema_version, game_id = payload["schemaVersion"], payload["gameId"]
        revision, complete, captured_at = payload["revision"], payload["complete"], payload["capturedAt"]
        state = payload["state"]
        if type(schema_version) is not int or schema_version != 2:
            raise SnapshotValidationError("unsupported schemaVersion; expected integer 2")
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
        _validate_state(state)
        _reject_sensitive_keys(state)
        try:
            encoded = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise SnapshotValidationError("snapshot must contain finite JSON values only") from exc
        if len(encoded) > max_bytes:
            raise SnapshotValidationError(f"snapshot exceeds the configured {max_bytes}-byte limit")
        return cls(schema_version, game_id, revision, captured_at, complete, dict(state))


def _reject_sensitive_keys(value: Any, *, depth: int = 0) -> None:
    if depth > 64:
        raise SnapshotValidationError("snapshot nesting exceeds 64 levels")
    if isinstance(value, Mapping):
        for key, nested in value.items():
            if not isinstance(key, str):
                raise SnapshotValidationError("snapshot object keys must be strings")
            normalized = "".join(character for character in key.lower() if character.isalnum())
            if normalized in _FORBIDDEN_KEYS or "credential" in normalized or "puuid" in normalized:
                raise SnapshotValidationError(f"snapshot contains a prohibited sensitive field: {key}")
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
    """Tracks one game session and rejects stale/incomplete canonical snapshots."""
    def __init__(self) -> None:
        self._game_id: int | None = None
        self._revision: int | None = None
        self._ready = False

    def ingest(self, payload: Any) -> SnapshotDecision:
        snapshot = GepSnapshot.parse(payload)
        if self._game_id != snapshot.game_id:
            self._game_id, self._revision, self._ready = snapshot.game_id, None, False
        if self._revision is not None:
            if snapshot.revision < self._revision:
                return SnapshotDecision("stale", snapshot, False)
            if snapshot.revision == self._revision:
                return SnapshotDecision("duplicate", snapshot, self._ready)
        self._revision = snapshot.revision
        if not snapshot.complete:
            self._ready = False
            return SnapshotDecision("awaiting_baseline", snapshot, False)
        local = snapshot.state["localPlayerIndex"]
        if not local["known"] or snapshot.state["players"][local["value"]] is None:
            self._ready = False
            return SnapshotDecision("awaiting_local_player", snapshot, False)
        self._ready = True
        return SnapshotDecision("ready", snapshot, True)

    def reset(self) -> None:
        self._game_id = None
        self._revision = None
        self._ready = False
