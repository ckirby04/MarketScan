"""Email when a finished run's scorecard clears the threshold.

docs/specs/SPEC_SCORECARD_ALERTS.md section 5. The rule: the overall score is at
least `alert_min_score`, and the verdict is at least `alert_min_robustness` on
the ladder below (empty means any verdict). Without a recipient or an email key
nothing is sent.

Every decision is written beside the run as alert.json, sent or not, so a run
says whether it alerted and why. Sending can fail -- the email service is down,
the key is wrong -- and that never fails the job: the Results are already
written, and alert.json records the error.

The email describes history. Like the page, it never says "buy", "recommended",
"expected return" or "good idea" (SPEC_RESEARCH_VIEW section 2).
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from .config import Settings

# The verdict, weakest to strongest.
ROBUSTNESS_LADDER = ("insufficient_evidence", "not_supported", "fragile", "supported")

GRADE_WORDS = {
    "supported": "supported",
    "fragile": "fragile",
    "not_supported": "not supported",
    "insufficient_evidence": "insufficient evidence",
}

SUBGRADE_NAMES = {
    "return": "Return",
    "risk": "Risk",
    "volatility": "Volatility",
    "risk_adjusted": "Risk-adjusted",
    "versus_holding": "Versus holding",
    "robustness": "Robustness",
}

RESEND_URL = "https://api.resend.com/emails"


@dataclass(frozen=True)
class Email:
    to: str
    subject: str
    text: str


def decide(results: dict, min_score: float, min_robustness: str) -> tuple[bool, str]:
    """Whether these Results clear the rule, and a reason either way."""
    card = results.get("scorecard")
    if not card:
        return False, "no scorecard in these Results"
    score = card["overall"]["score"]
    if score < min_score:
        return False, f"overall {score:.1f} is below {min_score:g}"
    grade = results["verdict"]["grade"]
    if min_robustness and ROBUSTNESS_LADDER.index(grade) < ROBUSTNESS_LADDER.index(min_robustness):
        return False, (f"overall {score:.1f} clears {min_score:g}, but the verdict is "
                       f"{GRADE_WORDS[grade]}, below {GRADE_WORDS[min_robustness]}")
    return True, f"overall {score:.1f} clears {min_score:g}"


def compose(results: dict, form: dict, to: str, link: str) -> Email:
    card = results["scorecard"]
    verdict = results["verdict"]
    overall = card["overall"]
    window = "the years not used to choose it" if card["window"] == "out_of_sample" else "the whole period"
    unverified = verdict["data_grade"] != "research"
    lines = [
        f"A backtest scored {overall['score']:.0f}/100 ({overall['letter']}) on historical data, "
        f"measured over {window}.",
        "",
    ]
    question = (form.get("prompt") or "").strip()
    if question:
        lines += [f"Your question: {question}", ""]
    lines.append(f"Verdict: historical evidence {GRADE_WORDS[verdict['grade']]}"
                 + (" (unverified data)" if unverified else ""))
    lines.append("")
    for sub in card["subgrades"]:
        name = SUBGRADE_NAMES.get(sub["id"], sub["id"])
        if sub["status"] == "not_applicable":
            lines.append(f"  {name}: doesn't apply")
        else:
            lines.append(f"  {name}: {sub['letter']} ({sub['score']:.0f})")
    lines.append("")
    if link:
        lines += [f"The run: {link}", ""]
    lines.append("This describes how a rule did on past prices. It is not a forecast, and not advice.")
    subject = f"MarketScan: a backtest scored {overall['score']:.0f} ({overall['letter']}) on historical data"
    return Email(to=to, subject=subject, text="\n".join(lines))


def send_with_resend(api_key: str, sender: str, email: Email) -> None:
    body = json.dumps({"from": sender, "to": [email.to], "subject": email.subject,
                       "text": email.text}).encode("utf-8")
    request = urllib.request.Request(
        RESEND_URL, data=body, method="POST",
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status >= 300:
            raise RuntimeError(f"the email service answered {response.status}")


class Alerter:
    """`send` is replaceable so tests never reach the network."""

    def __init__(self, settings: Settings, send=None):
        self.settings = settings
        # Without a key the default sender has nothing to send with.
        self._can_send = send is not None or bool(settings.resend_api_key)
        self._send = send or (lambda email: send_with_resend(
            settings.resend_api_key, settings.alert_from, email))

    def check(self, job_id: str, directory: Path) -> dict:
        """Decides for a finished run, sends if it should, and records it."""
        record: dict = {"job_id": job_id, "checked_at": time.time(), "sent": False}
        try:
            results = json.loads((directory / "results.json").read_text(encoding="utf-8"))
            form_path = directory / "form.json"
            form = json.loads(form_path.read_text(encoding="utf-8")) if form_path.exists() else {}
            s = self.settings
            if not s.alert_email_to:
                record["reason"] = "no alert address is set"
            elif not self._can_send:
                record["reason"] = "no email key is set"
            else:
                clears, reason = decide(results, s.alert_min_score, s.alert_min_robustness)
                record["reason"] = reason
                if clears:
                    link = f"{s.public_url.rstrip('/')}/runs/{job_id}" if s.public_url else ""
                    self._send(compose(results, form, s.alert_email_to, link))
                    record["sent"] = True
        except (OSError, ValueError, KeyError, RuntimeError, urllib.error.URLError) as error:
            record["error"] = f"{type(error).__name__}: {error}"
        (directory / "alert.json").write_text(json.dumps(record), encoding="utf-8")
        # One line in the service's log, like the assistant's usage lines.
        print(json.dumps({"alert": record}), flush=True)
        return record
