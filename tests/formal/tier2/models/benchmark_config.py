"""Per-benchmark configuration for Tier 2 generation and testing."""

TIER2_BENCHMARKS = {
    "counter_overflow": {
        "module": "counter_overflow",
        "class_name": "CounterOverflow",
        "bmc_depths": [1, 2, 4, 8, 16, 20],
        "k_induction_ks": [1, 2],
        "expected_bmc_result": "unsat",  # assertion should hold
        "expected_cover_depth": 15,
    },
    "fifo_ptr_valid": {
        "module": "fifo_ptr_valid",
        "class_name": "FifoPtrValid",
        "bmc_depths": [1, 2, 4, 8, 16],
        "k_induction_ks": [1, 2, 4],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": 8,
    },
    "fsm_onehot": {
        "module": "fsm_onehot",
        "class_name": "FsmOnehot",
        "bmc_depths": [1, 2, 4, 8, 16, 32],
        "k_induction_ks": [1, 2],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": 3,
    },
    "alu_pipeline": {
        "module": "alu_pipeline",
        "class_name": "AluPipeline",
        "bmc_depths": [1, 2, 4, 8],
        "k_induction_ks": [1, 2, 4],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": 4,
    },
    "regfile_rdwr": {
        "module": "regfile_rdwr",
        "class_name": "RegfileRdwr",
        "bmc_depths": [1, 2, 4],
        "k_induction_ks": [1, 2],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": 2,
    },
    "arbiter_fairness": {
        "module": "arbiter_fairness",
        "class_name": "ArbiterFairness",
        "bmc_depths": [1, 2, 4, 8, 16],
        "k_induction_ks": [1, 2, 4],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": 4,
    },
    "shift_register": {
        "module": "shift_register",
        "class_name": "ShiftRegister",
        "bmc_depths": [1, 2, 4, 8, 16],
        "k_induction_ks": [1, 2],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": 8,
    },
    "timer_watchdog": {
        "module": "timer_watchdog",
        "class_name": "TimerWatchdog",
        "bmc_depths": [1, 2, 4, 8, 16, 32],
        "k_induction_ks": [1, 2, 4],
        "expected_bmc_result": "unsat",
        "expected_cover_depth": None,
    },
}
