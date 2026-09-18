"""M4: the exported files are the formats the engine already reads.

These tests parse the output the way the C++ side does rather than the way the
writer wrote it. A round trip that only checks its own conventions proves the
writer is self-consistent, which is not the property anybody needs.
"""

from __future__ import annotations

import csv
import json
import datetime as dt

import pytest

from research.export import bars_frame, export_panel, membership_snapshots
from research.panel import PanelSpec, generate_panel


@pytest.fixture(scope="module")
def panel():
    return generate_panel(PanelSpec(n_securities=25, n_periods=90, seed=31,
                                    delist_hazard=0.004, delist_exposure_tilt=0.5))


def test_bars_cover_exactly_the_membership_days(panel):
    bars = bars_frame(panel)
    assert len(bars) == len(panel.frame)

    exported = set(zip(bars["date"], bars["symbol"]))
    expected = set(panel.frame.index)
    assert exported == expected


def test_bar_prices_are_internally_consistent(panel):
    bars = bars_frame(panel)
    assert (bars["high"] >= bars[["open", "close"]].max(axis=1) - 1e-9).all()
    assert (bars["low"] <= bars[["open", "close"]].min(axis=1) + 1e-9).all()
    assert (bars["low"] > 0).all()
    assert (bars["volume"] > 0).all()


def test_the_close_is_the_panel_price(panel):
    """The pipeline reads returns and the engine reads closes. If those two
    disagree, a parity test at M9 would be comparing two different datasets.
    """
    bars = bars_frame(panel).set_index(["date", "symbol"])
    bars.index.names = ["date", "security"]
    difference = (bars["close"] - panel.frame["price"]).abs()
    assert difference.max() < 1e-9


def test_membership_snapshots_are_one_per_date_and_sorted(panel):
    snapshots = membership_snapshots(panel)
    assert len(snapshots) == len(panel.dates)
    for date, members in snapshots:
        assert members == sorted(members)
        assert len(members) == len(set(members))


def test_written_files_parse_the_way_the_cpp_loader_parses_them(panel, tmp_path):
    """`SnapshotMembershipSource` reads `date,TICKER,TICKER,...`, skipping blank
    lines and lines beginning with '#', and derives intervals by diffing
    consecutive snapshots. This reimplements that diff and checks the intervals
    it produces are the ones the generator recorded -- which is the property M9
    will depend on when the engine loads this file for real.
    """
    written = export_panel(panel, tmp_path)
    assert written["bars"] == len(panel.frame)

    snapshots: list[tuple[dt.date, set[str]]] = []
    with (tmp_path / "membership.csv").open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field.strip() for field in line.split(",")]
            snapshots.append((dt.date.fromisoformat(fields[0]), {f for f in fields[1:] if f}))

    assert len(snapshots) == len(panel.dates)

    # Snapshots to intervals, as the loader does it.
    intervals: dict[str, list[list[dt.date]]] = {}
    open_names: dict[str, dt.date] = {}
    for date, members in snapshots:
        for name in members - open_names.keys():
            open_names[name] = date
        for name in list(open_names.keys() - members):
            intervals.setdefault(name, []).append([open_names.pop(name), date])
    for name, start in open_names.items():
        intervals.setdefault(name, []).append([start, snapshots[-1][0]])

    recorded = {
        row.security: (row.start.date(), row.end.date())
        for row in panel.membership.itertuples()
    }
    assert set(intervals) == set(recorded)
    for name, periods in intervals.items():
        assert len(periods) == 1, f"{name} was split into {len(periods)} intervals"
        start, end = periods[0]
        expected_start, expected_end = recorded[name]
        assert start == expected_start
        # A name that left is last seen on its final membership date, and the
        # loader closes the interval at the next snapshot it is absent from --
        # so the end it derives is the first date *after* membership, except for
        # names still present at the end of the file.
        assert end >= expected_end


def test_bars_csv_has_the_header_csv_to_msb_expects(panel, tmp_path):
    export_panel(panel, tmp_path)
    with (tmp_path / "bars.csv").open(encoding="utf-8") as handle:
        header = next(csv.reader(handle))
    assert header == ["date", "symbol", "open", "high", "low", "close", "volume"]


def test_provenance_travels_with_the_data(panel, tmp_path):
    export_panel(panel, tmp_path)
    provenance = json.loads((tmp_path / "panel.json").read_text(encoding="utf-8"))
    assert provenance["spec"]["seed"] == panel.spec.seed
    assert provenance["spec"]["lam"] == panel.spec.lam
    assert "Synthetic" in provenance["warning"]
