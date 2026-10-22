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

    extra: dict = field(default_factory=dict)


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
    if not csv and not env.get("MS_DAILY_DIR") and fixture.exists():
        # The committed synthetic series, so a fresh deployment can run the
        # starter form before any real data is attached.
        csv = (("FIXTURE", fixture),)
    return Settings(
        api_key=env.get("MS_API_KEY", ""),
        ms_research=(env.get("MS_RESEARCH_BIN", "ms_research"),),
        contracts=Path(env.get("MS_CONTRACTS", str(REPO / "contracts" / "research"))),
        daily_dir=Path(env["MS_DAILY_DIR"]) if env.get("MS_DAILY_DIR") else None,
        csv_sources=csv,
        runs_dir=Path(env.get("MS_RUNS_DIR", "/tmp/runs")),
        runs_bucket=env.get("MS_RUNS_BUCKET", ""),
        null_draws=int(env.get("MS_NULL_DRAWS", "1000")),
        bootstrap_paths=int(env.get("MS_BOOTSTRAP_PATHS", "1000")),
    )
