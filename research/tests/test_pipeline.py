"""M5: the pipeline runs end to end, and its out-of-sample numbers are honest.

The load-bearing test here is `test_a_signalless_panel_earns_nothing_out_of_sample`.
Every other number in the project is judged against it: a pipeline that reports a
positive R-squared on data containing no signal is not measuring a signal, and M8
is that test run two hundred times.
"""

from __future__ import annotations

import numpy as np
import pytest

from research.features import Feature, exposure_feature
from research.panel import PanelSpec, generate_panel
from research.pipeline import PipelineConfig, run_pipeline
from research.portfolio import PortfolioConstraints


@pytest.fixture(scope="module")
def signal_panel():
    return generate_panel(PanelSpec(n_securities=100, n_periods=600, lam=0.001, seed=81))


@pytest.fixture(scope="module")
def null_panel():
    return generate_panel(PanelSpec(n_securities=100, n_periods=600, lam=0.0, seed=82))


def test_it_runs_end_to_end(signal_panel):
    result = run_pipeline(signal_panel, PipelineConfig())

    assert result.n_rows > 0
    assert result.predictions.notna().all()
    assert result.portfolio is not None
    assert len(result.folds) == 5
    assert result.leaking == ()
    assert "planted" in result.report()


def test_it_recovers_the_planted_signal(signal_panel):
    result = run_pipeline(signal_panel, PipelineConfig())
    assert result.recovers(sigmas=3.0), result.report()
    assert result.ic_mean > 0.0
    assert result.ic_t > 3.0
    assert result.oos_r2 > 0.0


def test_the_model_is_calibrated_not_merely_correlated(signal_panel):
    """A prediction slope of one means the forecasts are the right *size*.

    A model can rank perfectly and still be useless for sizing a position, and a
    portfolio layer that scales by the prediction would then take the wrong risk.
    """
    result = run_pipeline(signal_panel, PipelineConfig())
    assert result.prediction_slope.covers(1.0, sigmas=3.0), str(result.prediction_slope)


def test_a_signalless_panel_earns_nothing_out_of_sample(null_panel):
    """lam = 0. The pipeline must come back empty-handed.

    R-squared out of sample is measured about zero, so a model that has learned
    nothing scores slightly *below* zero rather than at it -- it spends its
    predictions on noise. A positive number here is the smell of a leak.
    """
    result = run_pipeline(null_panel, PipelineConfig())

    assert result.lambda_hat.covers(0.0, sigmas=3.0), str(result.lambda_hat)
    assert abs(result.ic_t) < 3.0
    assert result.oos_r2 < 0.001, f"found {result.oos_r2:+.5f} of R-squared in nothing"


def test_out_of_sample_predictions_are_not_in_sample(signal_panel):
    """Fitting on everything inflates the score; the folds must prevent that.

    Run the pipeline with one fold's worth of leakage -- an embargo of zero and a
    single split is as close as this configuration gets to training on the test
    set -- and confirm the honest configuration scores *lower*. If the two agreed,
    the fold structure would not be doing anything.
    """
    honest = run_pipeline(signal_panel, PipelineConfig(n_splits=5, embargo=10,
                                                      build_portfolio=False))
    leaky = run_pipeline(signal_panel, PipelineConfig(
        features=(Feature(source="ret", transform="leak", window=1, cross_section="zscore"),
                  exposure_feature()),
        n_splits=5, embargo=10, build_portfolio=False))

    assert leaky.oos_r2 > honest.oos_r2 * 5
    assert leaky.leaking, "the leaking feature was not reported as leaking"


def test_the_null_model_is_the_baseline(null_panel):
    """Predicting the training mean scores about zero, and has no ordering at all.

    Not exactly zero, and it can land on either side: R-squared here is measured
    about zero, so a constant forecast is rewarded when the training period's
    drift continued into the test fold and penalised when it reversed. What has to
    be true is that the magnitude is negligible -- a constant cannot rank a
    cross-section, and its information coefficient is undefined rather than small.
    """
    result = run_pipeline(null_panel, PipelineConfig(model="mean", build_portfolio=False))
    assert abs(result.oos_r2) < 0.01
    assert result.ic.empty, "a constant prediction produced a rank correlation"


def test_the_gbm_finds_the_same_signal_the_ridge_does(signal_panel):
    """Not a competition: the panel is linear, so the flexible model should find
    the same thing and no more. It earning materially *more* would mean it had
    found structure that was not planted."""
    ridge = run_pipeline(signal_panel, PipelineConfig(model="ridge", build_portfolio=False))
    gbm = run_pipeline(signal_panel, PipelineConfig(model="gbm", build_portfolio=False))

    assert gbm.ic_mean > 0.0
    assert gbm.oos_r2 < ridge.oos_r2 * 3.0


def test_it_is_deterministic(signal_panel):
    config = PipelineConfig(build_portfolio=False)
    first = run_pipeline(signal_panel, config)
    second = run_pipeline(signal_panel, config)
    assert np.array_equal(first.predictions.to_numpy(), second.predictions.to_numpy())
    assert first.oos_r2 == second.oos_r2


def test_purging_is_reported(signal_panel):
    result = run_pipeline(signal_panel, PipelineConfig(horizon=10, embargo=10,
                                                      build_portfolio=False))
    assert result.purged > 0
    assert result.embargoed > 0
    assert result.purged + result.embargoed < result.n_rows


def test_multi_feature_and_multi_horizon_configurations_run(signal_panel):
    config = PipelineConfig(
        features=(exposure_feature(),
                  Feature(source="exposure", transform="mean", window=10),
                  Feature(source="ret", transform="sd", window=20)),
        horizon=5,
        embargo=5,
        constraints=PortfolioConstraints(max_weight=0.04, turnover_budget=0.15),
        rebalance_every=10,
    )
    result = run_pipeline(signal_panel, config)
    assert result.portfolio is not None
    assert result.recovers(sigmas=3.0), result.report()
    assert result.lambda_hat.nw_lags == 4


def test_a_horizon_longer_than_the_panel_is_an_error():
    tiny = generate_panel(PanelSpec(n_securities=10, n_periods=30, seed=83))
    with pytest.raises(ValueError):
        run_pipeline(tiny, PipelineConfig(horizon=60))
