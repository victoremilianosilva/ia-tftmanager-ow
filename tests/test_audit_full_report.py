from scripts.audit_viewer_match import markdown_full_report


def test_full_report_keeps_summary_and_adds_decision_evidence():
    report = {
        "source": "match-fixture",
        "log_files": ["player_1_log.json"],
        "players_seen": ["1"],
        "entries_read": 1,
        "diagnostics_missing": 0,
        "issues": [{
            "severity": "MEDIUM", "code": "EXAMPLE", "player": "Jogador 1",
            "round": "2-1", "step": 7, "message": "review", "evidence": {"gold": 12},
        }],
        "notes": [],
        "decision_timeline": [{
            "player": "Jogador 1", "round": "2-1", "step": 7, "sequence_id": 8,
            "timestamp": "12:00:00", "action": [0, 1, 0, 0, 0, 0],
            "action_name": "BUY", "strategy": "STABILIZE", "strategy_reason": "board",
            "reward": 0.25,
            "decision_context": {"gold": 12, "level": 4},
            "state_before": {"gold": 12, "board": [{"occupied": True, "champion_id": "A", "uuid": "u1", "star": 1}]},
            "state_after": {"gold": 9, "board": [{"occupied": True, "champion_id": "A", "uuid": "u1", "star": 1}],
                            "bench": [], "comp_direction": "A"},
            "legal_action_audit": {"chosen_type_enabled": True, "raw_mask": {"type_mask": [1, 1]}},
            "action_mask_before": {"type_mask": [1, 1]},
            "action_mask_after": {"type_mask": [1, 0]},
            "policy_diagnostics": {"entropy": 0.2},
            "reward_components": {"action": 0.25, "combat": 0.0, "placement": 0.0},
            "economic_diagnostics": {"action_cost": 3, "reason_code": "PURCHASE_COMPATIBLE"},
            "unit_changes": {"added": [], "removed": []},
            "composition_diagnostics": {"unit_fit": {"focus_fit_score": 1.0}},
            "item_audit": {"inventory_after": ["item_a"]},
        }],
    }

    full = markdown_full_report(report)
    assert full.startswith("# Auditoria da partida TFT")
    assert "## Evidências resumidas dos achados" in full
    assert "Legalidade: tipo permitido" in full
    assert '"type_mask": [' not in full
    assert '"focus_fit_score": 1.0' not in full
    assert '"inventory_after": [' not in full
    assert "Evidência adicional: gold=12" in full
    assert "Campos ausentes são exibidos" in full


def test_full_report_handles_empty_decision_timeline():
    full = markdown_full_report({
        "source": "legacy",
        "log_files": [],
        "players_seen": [],
        "entries_read": 0,
        "diagnostics_missing": 0,
        "issues": [],
        "notes": [],
        "decision_timeline": [],
    })
    assert "Decisões únicas estruturadas: 0" in full
    assert "~~~json" not in full
