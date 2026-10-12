"""Offline synthetic Reg diagnostics, never real telemetry."""
from .lab import SCENARIOS, generate, verify, write_fixture

__all__ = ["SCENARIOS", "generate", "verify", "write_fixture"]
