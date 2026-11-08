"""Contract tests for the Overwolf GEP snapshot boundary."""
import unittest

from live_inference.gep_snapshot import (
    GepSnapshot,
    SnapshotSequence,
    SnapshotValidationError,
)


def snapshot(**overrides):
    value = {
        "schemaVersion": 1,
        "gameId": 109,
        "revision": 1,
        "capturedAt": "2026-11-08T16:00:00.000Z",
        "complete": True,
        "info": {"shop": [], "units": [], "economy": {"gold": 10}},
    }
    value.update(overrides)
    return value


class GepSnapshotTests(unittest.TestCase):
    def test_parses_versioned_envelope_without_assuming_info_schema(self):
        parsed = GepSnapshot.parse(snapshot())
        self.assertEqual(parsed.schema_version, 1)
        self.assertEqual(parsed.game_id, 109)
        self.assertEqual(parsed.info["economy"]["gold"], 10)

    def test_rejects_missing_required_field(self):
        value = snapshot()
        del value["info"]
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(value)

    def test_rejects_unsupported_schema(self):
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(schemaVersion=2))

    def test_rejects_bool_as_integer_game_id(self):
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(gameId=True))

    def test_rejects_sensitive_nested_fields(self):
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(info={"player": {"puuid": "private"}}))

    def test_rejects_non_finite_json_values(self):
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(info={"gold": float("nan")}))

    def test_rejects_oversized_snapshot(self):
        with self.assertRaises(SnapshotValidationError):
            GepSnapshot.parse(snapshot(info={"payload": "x" * 100}), max_bytes=64)

    def test_sequence_waits_for_complete_baseline(self):
        sequence = SnapshotSequence()
        waiting = sequence.ingest(snapshot(revision=0, complete=False, info={}))
        self.assertEqual(waiting.status, "awaiting_baseline")
        self.assertFalse(waiting.ready_for_inference)

        ready = sequence.ingest(snapshot(revision=1, complete=True))
        self.assertEqual(ready.status, "ready")
        self.assertTrue(ready.ready_for_inference)

    def test_sequence_ignores_duplicates_and_stale_revisions(self):
        sequence = SnapshotSequence()
        sequence.ingest(snapshot(revision=5))
        duplicate = sequence.ingest(snapshot(revision=5))
        stale = sequence.ingest(snapshot(revision=4))
        self.assertEqual(duplicate.status, "duplicate")
        self.assertEqual(stale.status, "stale")

    def test_new_game_starts_a_new_revision_sequence(self):
        sequence = SnapshotSequence()
        sequence.ingest(snapshot(revision=12))
        next_game = sequence.ingest(snapshot(gameId=110, revision=0, complete=False, info={}))
        self.assertEqual(next_game.status, "awaiting_baseline")
        self.assertFalse(next_game.ready_for_inference)


if __name__ == "__main__":
    unittest.main()
