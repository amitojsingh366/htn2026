"""Only fixed condition codes cross the private-output boundary."""
from dataclasses import dataclass


class RecoveryError(Exception):
    def __init__(self, condition, **evidence):
        super().__init__(condition)
        self.condition = condition
        self.evidence = evidence


def require(value, condition, **evidence):
    if not value:
        raise RecoveryError(condition, **evidence)


@dataclass(frozen=True)
class GateResult:
    passed: bool
    condition: str
    evidence: dict

    def enforce(self):
        require(self.passed, self.condition, **self.evidence)
        return self


def check(condition, passed, **evidence):
    return GateResult(bool(passed), condition, evidence)
