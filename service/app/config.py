"""Settings, from the environment. Nothing here has a secret default."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Settings:
    # The bearer token every request must carry. Empty refuses every request:
    # a service that runs strategies on someone's account should not be open by
    # accident because a variable was not set.
    api_key: str = ""

    # The engine, as a command (a test substitutes a fake), and what it reads.
    ms_research: tuple[str, ...] = ("ms_research",)
    contracts: Path = REPO / "contracts" / "research"
    daily_dir: Path | None = None
    # More directories of .msb files after the first (rates beside prices);
    # the first holding a symbol answers.
    extra_daily_dirs: tuple[Path, ...] = ()
    # The catalog's synthetic recipes (scripts/build_recipes.py writes them).
    recipes: Path | None = None
    csv_sources: tuple[tuple[str, Path], ...] = ()

    # Where runs are written, and the bucket they are copied to so they survive
    # an instance being shut down. No bucket means local only.
    runs_dir: Path = Path("/tmp/runs")
    runs_bucket: str = ""

    null_draws: int = 1000
    bootstrap_paths: int = 1000

    # One job at a time, per instance. Personal use; the engine is single
    # threaded and a second job would only slow the first.
    max_running: int = 1

    # The assistant. No key means no assistant: its routes answer 503 and the
    # job routes work as before.
    anthropic_api_key: str = ""
    assistant_model: str = "claude-opus-5-5"
    assistant_effort: str = "medium"

    # Alerts (docs/specs/SPEC_SCORECARD_ALERTS.md section 5). No address or no
    # key means no email; every run still records its decision in alert.json.
    alert_email_to: str = ""
    alert_from: str = "MarketScan <onboarding@resend.dev>"
    alert_min_score: float = 80.0
    # The weakest verdict that may alert; "" lets any verdict through.
    alert_min_robustness: str = "supported"
    resend_api_key: str = ""
    # Where the web app is, for the link in an alert.
    public_url: str = ""

    extra: dict = field(default_factory=dict)

    def daily_dirs(self) -> list[Path]:
        return ([self.daily_dir] if self.daily_dir is not None else []) + list(self.extra_daily_dirs)


def _csv_sources(spec: str) -> tuple[tuple[str, Path], ...]:
    out = []
    for item in filter(None, (s.strip() for s in spec.split(","))):
        symbol, _, path = item.partition("=")
        if symbol and path:
            out.append((symbol, Path(path)))
    return tuple(out)


def from_env() -> Settings:
    env = os.environ
    fixture = REPO / "data" / "samples" / "research" / "drawdown_prices.csv"
    csv = _csv_sources(env.get("MS_CSV", ""))
    # MS_DAILY_DIR may name several directories, comma-separated.
    daily = [Path(d.strip()) for d in env.get("MS_DAILY_DIR", "").split(",") if d.strip()]
    if not csv and not env.get("MS_DAILY_DIR") and fixture.exists():
        # The committed synthetic series, so a fresh deployment can run the
        # starter form before any real data is attached.
        csv = (("FIXTURE", fixture),)
    return Settings(
        api_key=env.get("MS_API_KEY", ""),
        ms_research=(env.get("MS_RESEARCH_BIN", "ms_research"),),
        contracts=Path(env.get("MS_CONTRACTS", str(REPO / "contracts" / "research"))),
        daily_dir=daily[0] if daily else None,
        extra_daily_dirs=tuple(daily[1:]),
        recipes=Path(env["MS_RECIPES"]) if env.get("MS_RECIPES") else None,
        csv_sources=csv,
        runs_dir=Path(env.get("MS_RUNS_DIR", "/tmp/runs")),
        runs_bucket=env.get("MS_RUNS_BUCKET", ""),
        null_draws=int(env.get("MS_NULL_DRAWS", "1000")),
        bootstrap_paths=int(env.get("MS_BOOTSTRAP_PATHS", "1000")),
        # Stripped: a key read from a file or a secret often ends in a newline.
        anthropic_api_key=env.get("ANTHROPIC_API_KEY", "").strip(),
        assistant_model=env.get("MS_ASSISTANT_MODEL", "claude-opus-5-5"),
        assistant_effort=env.get("MS_ASSISTANT_EFFORT", "medium"),
        alert_email_to=env.get("MS_ALERT_EMAIL_TO", "").strip(),
        alert_from=env.get("MS_ALERT_FROM", "MarketScan <onboarding@resend.dev>"),
        alert_min_score=float(env.get("MS_ALERT_MIN_SCORE", "80")),
        alert_min_robustness=env.get("MS_ALERT_MIN_ROBUSTNESS", "supported").strip(),
        resend_api_key=env.get("RESEND_API_KEY", "").strip(),
        public_url=env.get("MS_PUBLIC_URL", "").strip(),
    )
