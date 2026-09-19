"""M4: the generator is deterministic, and it plants the lam it was asked to."""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from research import metrics
from research.labels import forward_return
from research.panel import PanelSpec, generate_panel


def test_deterministic_given_a_seed():
    a = generate_panel(PanelSpec(n_securities=40, n_periods=120, seed=99))
    b = generate_panel(PanelSpec(n_securities=40, n_periods=120, seed=99))
    pd.testing.assert_frame_equal(a.frame, b.frame)
    pd.testing.assert_series_equal(a.market, b.market)
    pd.testing.assert_frame_equal(a.membership, b.membership)


def test_a_different_seed_is_a_different_panel():
    a = generate_panel(PanelSpec(n_securities=40, n_periods=120, seed=1))
    b = generate_panel(PanelSpec(n_securities=40, n_periods=120, seed=2))
    assert not np.allclose(a.frame["ret"].to_numpy(), b.frame["ret"].to_numpy())


def test_streams_are_independent():
    """Changing the noise scale must not redraw the betas or the exposures.

    The same property the C++ engine gives each theory's RNG: a knob that
    silently moves an unrelated quantity makes every comparison between two runs
    a comparison of two different experiments.
    """
    quiet = generate_panel(PanelSpec(n_securities=40, n_periods=120, seed=5, idio_vol=0.001))
    loud = generate_panel(PanelSpec(n_securities=40, n_periods=120, seed=5, idio_vol=0.100))

    pd.testing.assert_series_equal(quiet.betas, loud.betas)
    pd.testing.assert_series_equal(quiet.market, loud.market)
    pd.testing.assert_series_equal(quiet.frame["exposure"], loud.frame["exposure"])
    assert not np.allclose(quiet.frame["ret"].to_numpy(), loud.frame["ret"].to_numpy())


def test_exposure_has_unit_variance_and_the_configured_persistence():
    """lam is only interpretable if the exposure's scale is what it claims."""
    panel = generate_panel(PanelSpec(n_securities=120, n_periods=800, seed=3, exposure_rho=0.9))
    wide = panel.exposures()

    assert wide.stack().std() == pytest.approx(1.0, abs=0.05)

    lagged = wide.shift(1)
    both = pd.concat([wide.stack(), lagged.stack()], axis=1).dropna()
    assert both.corr().iloc[0, 1] == pytest.approx(0.9, abs=0.03)


def test_the_market_factor_is_shared_and_betas_load_on_it():
    panel = generate_panel(PanelSpec(n_securities=60, n_periods=500, seed=8, lam=0.0))
    returns = panel.returns()

    # Regressing each name's returns on the market recovers its beta. Not a
    # statement about the model, a check that the panel implements it.
    market = panel.market.loc[returns.index]
    recovered = {}
    for name in returns.columns:
        series = pd.concat([returns[name], market], axis=1).dropna()
        x = series.iloc[:, 1].to_numpy()
        y = series.iloc[:, 0].to_numpy()
        centred = x - x.mean()
        recovered[name] = float(np.dot(centred, y - y.mean()) / np.dot(centred, centred))

    estimated = pd.Series(recovered)
    error = (estimated - panel.betas.reindex(estimated.index)).abs()
    assert error.max() < 0.25
    assert error.mean() < 0.06


@pytest.mark.parametrize("lam", [0.0, 0.0002, 0.0005, 0.001, 0.002])
def test_realized_relationship_matches_the_configured_lam(lam):
    """M4's definition of done.

    The relationship between the exposure and the *forward* return, measured on
    the generated panel, is the lam the spec asked for, within sampling error.
    If this fails, nothing downstream means anything: every later milestone is
    measured against this number being true.
    """
    spec = PanelSpec(n_securities=200, n_periods=1200, lam=lam, seed=4321)
    panel = generate_panel(spec)
    labels = forward_return(panel.frame, horizon=1)

    estimate = metrics.fama_macbeth_for_horizon(panel.frame["exposure"], labels.values, horizon=1)
    assert estimate.covers(lam, sigmas=3.0), f"{estimate} does not cover lam={lam}"


def test_a_longer_horizon_accumulates_a_decaying_exposure():
    """The h-period lam is the geometric sum, not h times lam.

    The exposure is persistent but mean-reverting, so a five-period forward
    return earns lam against x_t, then against rho*x_t, and so on. Anybody
    comparing a multi-period result against h*lam will conclude the pipeline
    under-recovers, and be wrong.
    """
    spec = PanelSpec(n_securities=200, n_periods=1500, lam=0.0005, seed=77, exposure_rho=0.95)
    panel = generate_panel(spec)
    horizon = 5
    labels = forward_return(panel.frame, horizon=horizon)

    rho = spec.exposure_rho
    expected = spec.lam * (1.0 - rho ** horizon) / (1.0 - rho)

    estimate = metrics.fama_macbeth_for_horizon(panel.frame["exposure"], labels.values,
                                                horizon=horizon)
    assert estimate.covers(expected, sigmas=3.0), f"{estimate} vs expected {expected:.6f}"


def test_membership_is_point_in_time_by_construction():
    panel = generate_panel(PanelSpec(n_securities=50, n_periods=400, seed=6,
                                     delist_hazard=0.001, delist_exposure_tilt=0.5))
    assert len(panel.membership) > 50, "the hazard produced no churn; the test is not testing"

    frame = panel.frame.index.to_frame(index=False)
    for row in panel.membership.itertuples():
        appearances = frame.loc[frame["security"] == row.security, "date"]
        assert appearances.min() == row.start
        assert appearances.max() == row.end

    # And the contrapositive: no name appears outside its own interval.
    intervals = panel.membership.set_index("security")
    for security, group in frame.groupby("security"):
        start = intervals.loc[security, "start"]
        end = intervals.loc[security, "end"]
        assert group["date"].between(start, end).all()


def test_a_departure_leaves_the_price_series_ordinary():
    """The delisting return is **not** in the final bar, and that is Session D.

    It used to be: a departing name's last bar carried -30%, which meant the
    engine's "liquidate at the last price the book has" captured the loss by
    construction and the whole question of delisting returns could not be asked.
    On real data the delisting return is a separate event after the last quoted
    price. So the final bar is now an ordinary bar and the return that follows it
    is recorded beside the panel.
    """
    panel = generate_panel(PanelSpec(n_securities=50, n_periods=400, seed=6,
                                     delist_hazard=0.002, delist_exposure_tilt=0.0))
    departures = panel.frame[panel.frame["delisted"]]
    assert not departures.empty

    # An ordinary bar: nothing near a delisting-sized move.
    assert departures["ret"].abs().max() < 0.25

    # And the truth is recorded, once per departing name.
    assert len(panel.delistings) == len(departures)
    assert set(panel.delistings.index) == {s for _, s in departures.index}

    # A departure is the name's last row, and never the panel's last date.
    last_date = panel.dates[-1]
    for (date, security) in departures.index:
        assert date != last_date
        assert panel.frame.xs(security, level=1).index.max() == date


def test_the_delisting_model_is_bimodal_by_cause():
    """Distress with mass near total loss, voluntary exits that pay a premium.

    A single mean would describe neither arm. The parameters are a modelling
    choice and `panel.py` says so; what is asserted here is that the two arms are
    actually distinguishable and that neither can lose more than everything.
    """
    panel = generate_panel(PanelSpec(n_securities=120, n_periods=800, seed=31,
                                     delist_hazard=0.003))
    d = panel.delistings
    assert len(d) > 50
    assert set(d["cause"]) == {"distress", "voluntary"}

    distress = d.loc[d["cause"] == "distress", "delisting_return"]
    voluntary = d.loc[d["cause"] == "voluntary", "delisting_return"]

    assert distress.mean() < -0.3
    assert voluntary.mean() > -0.05
    assert distress.mean() < voluntary.mean() - 0.4

    assert (d["delisting_return"] >= -1.0).all(), "a position cannot lose more than itself"
    assert (distress <= 0.0).all(), "a distressed delisting does not pay a premium"
    assert (d["delisting_return"] <= -0.99).any(), "no total loss in the distress arm"


def test_the_delisting_draw_is_seeded_like_everything_else():
    a = generate_panel(PanelSpec(n_securities=60, n_periods=400, seed=5, delist_hazard=0.003))
    b = generate_panel(PanelSpec(n_securities=60, n_periods=400, seed=5, delist_hazard=0.003))
    pd.testing.assert_frame_equal(a.delistings, b.delistings)


def test_the_tilt_decides_which_names_die():
    """What makes survivorship bias *directional* in M7.

    A positive tilt kills low-exposure names, a negative one kills high-exposure
    names, and which leg the failures land in is what decides whether removing
    them inflates or deflates a measured premium. M7 runs both.
    """
    panel = generate_panel(PanelSpec(n_securities=150, n_periods=600, seed=21,
                                     delist_hazard=0.002, delist_exposure_tilt=0.8))
    departures = panel.frame[panel.frame["delisted"]]
    assert len(departures) > 30

    exposure_at_death = departures["exposure"].mean()
    assert exposure_at_death < -0.1, ("departures are not tilted towards low exposure; "
                                      f"mean exposure at exit was {exposure_at_death:.3f}")


def test_the_pool_never_silently_shrinks():
    """A generator that ran out of replacements would be survivorship bias
    introduced by the instrument, which is the one place it must not come from.
    """
    panel = generate_panel(PanelSpec(n_securities=80, n_periods=500, seed=12,
                                     delist_hazard=0.004, delist_exposure_tilt=0.5))
    per_date = panel.frame.groupby(level=0).size()
    assert (per_date == 80).all(), "membership count moved; replacements did not keep up"


def test_an_impossible_spec_is_rejected_rather_than_worked_around():
    with pytest.raises(ValueError):
        PanelSpec(n_securities=1)
    with pytest.raises(ValueError):
        PanelSpec(exposure_rho=1.0)
    with pytest.raises(ValueError):
        PanelSpec(delist_hazard=1.5)
