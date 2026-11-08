"""Contract tests for the canonical state snapshots emitted by Overwolf."""
import unittest

from live_inference.gep_snapshot import GepSnapshot, SnapshotSequence, SnapshotValidationError


def canonical_state():
    unknown = {"known": False, "value": None}
    empty_slots = {"known": True, "slots": [{"known": True, "unit": None} for _ in range(28)]}
    bench_slots = {"known": True, "slots": [{"known": True, "unit": None} for _ in range(9)]}
    shop_slots = {"known": True, "slots": [{"known": True, "value": None} for _ in range(5)]}
    player = {
        "index": 0, "localPlayer": True,
        "hp": {"known": True, "value": 84}, "gold": {"known": True, "value": 35},
        "level": {"known": True, "value": 4}, "xp": {"known": True, "value": 4},
        "alive": {"known": True, "value": True},
        "board": empty_slots, "bench": bench_slots, "shop": shop_slots,
        "itemComponents": {"known": True, "value": [{"apiName": "DA_Component_BFSword", "count": 2}]},
    }
    return {
        "contract": "tft.live-state", "contractVersion": 1,
        "stage": {"known": True, "value": 2}, "round": {"known": True, "value": 6},
        "phase": unknown, "localPlayerIndex": {"known": True, "value": 0},
        "players": [player] + [None] * 7,
        "carousel": {"known": True, "value": []},
    }


def snapshot(**overrides):
    value = {
        "schemaVersion": 2, "gameId": 109, "revision": 1,
        "capturedAt": "2026-11-08T16:00:00.000Z", "complete": True,
        "state": canonical_state(),
    }
    value.update(overrides)
    return value


class CanonicalSnapshotTests(unittest.TestCase):
    def test_parses_canonical_contract_without_gep_paths(self):
        parsed = GepSnapshot.parse(snapshot())
        self.assertEqual(parsed.schema_version, 2)
        self.assertEqual(parsed.state["stage"]["value"], 2)
        self.assertEqual(parsed.state["players"][0]["gold"]["value"], 35)

    def test_rejects_raw_info_schema_and_legacy_version(self):
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse({"schemaVersion": 1, "gameId": 1, "revision": 1,
                               "capturedAt": "2026-11-08T16:00:00Z", "complete": True, "info": {}})

    def test_rejects_missing_required_field(self):
        value = snapshot()
        del value["state"]
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(value)

    def test_rejects_invalid_contract_version(self):
        state = canonical_state()
        state["contractVersion"] = 2
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(state=state))

    def test_rejects_wrong_board_slot_count(self):
        state = canonical_state()
        state["players"][0]["board"]["slots"].pop()
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(state=state))

    def test_rejects_inconsistent_local_player_index(self):
        state = canonical_state()
        state["localPlayerIndex"]["value"] = 2
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(state=state))

    def test_rejects_sensitive_nested_fields(self):
        state = canonical_state()
        state["players"][0]["puuid"] = "private"
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(state=state))

    def test_rejects_non_finite_json_values(self):
        state = canonical_state()
        state["players"][0]["gold"]["value"] = float("nan")
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(state=state))

    def test_rejects_oversized_snapshot(self):
        state = canonical_state()
        state["players"][0]["board"]["slots"][0]["unit"] = {"apiName": "x" * 200, "star": 1, "items": []}
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(state=state), max_bytes=100)

    def test_sequence_waits_for_complete_baseline(self):
        sequence = SnapshotSequence()
        waiting = sequence.ingest(snapshot(revision=0, complete=False))
        self.assertEqual(waiting.status, "awaiting_baseline")
        self.assertFalse(waiting.ready_for_inference)
        ready = sequence.ingest(snapshot(revision=1, complete=True))
        self.assertEqual(ready.status, "ready")
        self.assertTrue(ready.ready_for_inference)

    def test_sequence_waits_for_local_player_mapping(self):
        state = canonical_state()
        state["localPlayerIndex"] = {"known": False, "value": None}
        state["players"] = [None] * 8
        decision = SnapshotSequence().ingest(snapshot(state=state))
        self.assertEqual(decision.status, "awaiting_local_player")
        self.assertFalse(decision.ready_for_inference)

    def test_sequence_ignores_duplicates_and_stale_revisions(self):
        sequence = SnapshotSequence()
        sequence.ingest(snapshot(revision=5))
        self.assertEqual(sequence.ingest(snapshot(revision=5)).status, "duplicate")
        self.assertEqual(sequence.ingest(snapshot(revision=4)).status, "stale")

    def test_new_game_starts_a_new_revision_sequence(self):
        sequence = SnapshotSequence()
        sequence.ingest(snapshot(revision=12))
        next_game = sequence.ingest(snapshot(gameId=110, revision=0, complete=False))
        self.assertEqual(next_game.status, "awaiting_baseline")
        self.assertFalse(next_game.ready_for_inference)


if __name__ == "__main__":
    unittest.main()
