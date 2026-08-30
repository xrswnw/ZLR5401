"""Result container for a single test case."""
from enum import Enum


class Verdict(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    INCONCLUSIVE = "INCONCLUSIVE"


class Result:
    def __init__(self, name, section, layer=""):
        self.name = name
        self.section = section
        self.layer = layer
        self.verdict = None
        self.message = ""
        self.evidence = []

    def set(self, verdict, message="", evidence=None):
        self.verdict = verdict
        self.message = message
        if evidence:
            self.evidence = list(evidence)
        return self

    @property
    def ok(self):
        return self.verdict == Verdict.PASS

    def to_dict(self):
        return {
            "name": self.name,
            "section": self.section,
            "layer": self.layer,
            "verdict": self.verdict.value if self.verdict else None,
            "message": self.message,
            "evidence": self.evidence,
        }
