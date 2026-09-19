"""The research pipeline, end to end.

Spec 2 M5. Features to labels to purged folds to a model to a portfolio, with
every number it reports carrying the qualification it needs.

WHAT IS MEASURED, AND AGAINST WHAT

`lambda_hat` is the pipeline's estimate of the planted signal strength, in the
panel's own units, measured **on out-of-sample rows only**. It is deliberately
computed against the raw exposure rather than against the standardized feature
built from it, so that the recovery plot in M6 has the same quantity on both
axes: a per-date cross-sectional z-score rescales by a number that fluctuates
with the cross-section, and a recovery test whose axes differed by a few percent
for that reason would be measuring the normalization, not the pipeline.

What the *model* contributed is reported separately and honestly:

| `oos_r2` | Out-of-sample R-squared about zero. Expect something small. |
| `ic` | Per-date rank correlation of prediction and realized return. |
| `prediction_slope` | Realized return regressed on predicted return. One means calibrated: the model's forecasts are the right size, not merely the right order. |
| `portfolio` | What a constrained book following those predictions would have earned. |

`leaking` is non-empty when any feature can see the future. It exists so that an
M7 result cannot be mistaken for an M6 one.

ONE SUBTLETY, MEASURED RATHER THAN ASSUMED

On a panel whose departure hazard is tilted by the exposure, `lambda_hat` comes
out **above** the planted `lam`, and it is right to. Low exposure there predicts
two things: a slightly lower continuous return, which is `lam`, and a raised
chance of departing at -30%, which is not. The total relationship between exposure
and forward return is the sum of both channels, and `lambda_hat` measures the
total.

So the recovery milestone runs on panels with no churn, where the two coincide,
and the size of the second channel is M7's business. A test that "recovered" lam
on a churning panel would only do so by cancelling one bias against another.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from research import delisting, metrics, thresholds
from research.features import Feature, build_features, exposure_feature, leaking
from research.delisting import DEFAULT_MODEL, DelistingReturnModel
from research.labels import Terminal, forward_return
from research.models import make_model
from research.panel import Panel
from research.portfolio import PortfolioConstraints, PortfolioResult, build_portfolio
from research.splits import FoldReport, PurgedKFold


@dataclass(frozen=True)
class PipelineConfig:
    """One pipeline variant. The unit the M8 sweep enumerates."""

    features: tuple[Feature, ...] = (exposure_feature(),)
    horizon: int = 1
    model: str = "ridge"
    model_kwargs: dict[str, float | int] = field(default_factory=dict)
    n_splits: int = 5
    embargo: int = 5
    terminal: Terminal = "compound"

    delisting_model: DelistingReturnModel = DEFAULT_MODEL
    """What a position is worth after the last quoted price. Defaults to the
    optimistic `last_price`, which is what the engine does and what every
    committed result here was measured under -- see research/delisting.py."""
    constraints: PortfolioConstraints = field(default_factory=PortfolioConstraints)
    rebalance_every: int = 21
    build_portfolio: bool = True
    """Off for the sweep's cheaper passes. The portfolio is the slowest stage and
    a variant being screened on its information coefficient does not need one."""

    label: str = ""

    def named(self) -> str:
        if self.label:
            return self.label
        features = "+".join(feature.label for feature in self.features)
        return f"{self.model}|h={self.horizon}|{features}"


@dataclass
class PipelineResult:
    """Everything one run produced, with its caveats attached."""

    config: PipelineConfig
    panel_lam: float
    """The planted lam. Known, because the panel was generated."""

    lambda_hat: metrics.LambdaEstimate
    prediction_slope: metrics.LambdaEstimate
    oos_r2: float
    ic: pd.Series
    predictions: pd.Series
    folds: tuple[FoldReport, ...]
    portfolio: PortfolioResult | None
    leaking: tuple[str, ...]
    n_rows: int

    @property
    def ic_mean(self) -> float:
        return metrics.mean(self.ic)

    @property
    def ic_t(self) -> float:
        """t-statistic of the mean information coefficient.

        Corrected for the overlap the horizon implies, for the same reason
        `fama_macbeth_for_horizon` is: consecutive ICs from a horizon of h share
        h-1 periods of return, and the uncorrected statistic is too large.
        """
        if self.ic.size < 2:
            return metrics.UNDEFINED
        se = metrics.newey_west_se(self.ic, max(self.config.horizon - 1, 0))
        if not np.isfinite(se) or se <= 0.0:
            return metrics.UNDEFINED
        return float(self.ic.mean() / se)

    @property
    def ic_t_false_positive(self) -> float:
        """How often this t-statistic fires on nothing, at this horizon.

        M8b measured it: between 8% and 26% depending on the horizon, against a
        nominal 5%. The number travels with the statistic rather than living only
        in the documentation, because a t-statistic that looks like every other
        t-statistic is exactly the thing somebody reads at face value.

        NaN when the horizon is outside the calibrated range, which is a truthful
        answer rather than an extrapolated one.
        """
        try:
            return thresholds.ic_t_false_positive_rate(self.config.horizon)
        except ValueError:
            return metrics.UNDEFINED

    def ic_bar(self, variants_tried: int = 1) -> float:
        """The measured |IC| this result has to clear. See M8b."""
        try:
            return thresholds.ic_threshold_for(self.config.horizon, variants_tried)
        except ValueError:
            return metrics.UNDEFINED

    def ic_is_distinguishable(self, variants_tried: int = 1) -> bool:
        """Whether the IC clears the measured bar rather than the nominal one."""
        try:
            return thresholds.ic_distinguishable(self.ic_mean, self.config.horizon,
                                                 variants_tried)
        except ValueError:
            return False

    @property
    def sharpe(self) -> float:
        if self.portfolio is None:
            return metrics.UNDEFINED
        return metrics.sharpe(self.portfolio.active_returns)

    @property
    def purged(self) -> int:
        return sum(fold.purged for fold in self.folds)

    @property
    def embargoed(self) -> int:
        return sum(fold.embargoed for fold in self.folds)

    def recovers(self, sigmas: float = 2.0) -> bool:
        """Whether the estimate's error bars cover the planted lam."""
        return self.lambda_hat.covers(self.panel_lam, sigmas=sigmas)

    def report(self) -> str:
        lines = [
            f"{self.config.named()}",
            f"  rows          {self.n_rows} ({self.purged} purged, {self.embargoed} embargoed)",
            f"  lam           planted {self.panel_lam:+.6f}, {self.lambda_hat}",
            f"  oos R2        {self.oos_r2:+.5f}",
            f"  IC            {self.ic_mean:+.4f} over {self.ic.size} dates",
            f"                t={self.ic_t:+.2f}, and that t fires on nothing "
            f"{self.ic_t_false_positive:.1%} of the time at h={self.config.horizon}",
            f"                measured bar |IC| > {self.ic_bar():.4f} single-test; "
            f"this result {'clears' if self.ic_is_distinguishable() else 'does not clear'} it",
            f"  calibration   {self.prediction_slope}",
        ]
        if self.portfolio is not None:
            summary = self.portfolio.summary()
            lines.append(f"  portfolio     sharpe {summary['sharpe']:+.2f}, "
                         f"turnover {summary['turnover']:.3f}, "
                         f"{self.portfolio.rebalances} rebalances, "
                         f"{self.portfolio.forced_exits} forced exits")
            if self.portfolio.delisting_loss:
                lines.append(f"  delistings    {self.portfolio.delisting_loss:+.5f} "
                             f"earned in departure periods")
        if self.leaking:
            lines.append(f"  LEAKING       {', '.join(self.leaking)} -- this result is not honest")
        return "\n".join(lines)


def _accumulation(rho: float, horizon: int) -> float:
    """How much of a single-period lam an h-period forward return accumulates.

    The exposure decays at `rho`, so an h-period return earns lam against
    x_t, then rho*x_t, and so on: the geometric sum. Using the generator's own
    rho is legitimate here and nowhere else -- this is a calibration rig and the
    persistence is a number that was planted, not estimated.
    """
    if horizon <= 1:
        return 1.0
    if rho >= 1.0:
        return float(horizon)
    return float((1.0 - rho ** horizon) / (1.0 - rho))


def run_pipeline(panel: Panel, config: PipelineConfig | None = None) -> PipelineResult:
    """Runs one variant against one panel."""
    config = config or PipelineConfig()

    features = build_features(panel.frame, list(config.features))
    applied_delistings = delisting.applied_returns(panel.delistings, config.delisting_model)
    labels = forward_return(panel.frame, horizon=config.horizon, terminal=config.terminal,
                            delisting_returns=applied_delistings)

    aligned = features.join(labels.values.rename("_label"), how="inner").dropna()
    if aligned.empty:
        raise ValueError("no rows survived feature and label alignment; the windows are longer "
                         "than the panel")
    aligned = aligned.join(labels.end.rename("_label_end"), how="left")

    row_dates = aligned.index.get_level_values(0)
    x = aligned[list(features.columns)].to_numpy(dtype=float)
    y = aligned["_label"].to_numpy(dtype=float)

    cv = PurgedKFold(n_splits=config.n_splits, embargo=config.embargo)
    predictions = np.full(len(aligned), np.nan)
    folds: list[FoldReport] = []
    for train, test, report in cv.split_with_reports(row_dates, aligned["_label_end"]):
        folds.append(report)
        if train.size == 0:
            # Every training row overlapped the fold. Not an error -- it is what
            # a horizon longer than a fold produces -- but the fold contributes
            # nothing and saying so beats an array of quiet zeros.
            continue
        model = make_model(config.model, **config.model_kwargs)
        model.fit(x[train], y[train])
        predictions[test] = model.predict(x[test])

    prediction_series = pd.Series(predictions, index=aligned.index, name="prediction").dropna()
    if prediction_series.empty:
        raise ValueError("no out-of-sample predictions were produced; every fold purged its "
                         "entire training set")

    out_of_sample = prediction_series.index
    label_series = aligned["_label"].reindex(out_of_sample)

    # lam, measured out of sample, in the panel's own exposure units.
    exposure = panel.frame["exposure"].reindex(out_of_sample)
    raw = metrics.fama_macbeth_for_horizon(exposure, label_series, horizon=config.horizon)
    divisor = _accumulation(panel.spec.exposure_rho, config.horizon)
    lambda_hat = metrics.LambdaEstimate(raw.estimate / divisor, raw.stderr / divisor,
                                        raw.n_periods, raw.nw_lags)

    portfolio = None
    if config.build_portfolio:
        portfolio = build_portfolio(prediction_series, panel.frame, panel.betas, panel.sectors,
                                    constraints=config.constraints,
                                    rebalance_every=config.rebalance_every,
                                    delisting_returns=applied_delistings)

    return PipelineResult(
        config=config,
        panel_lam=panel.spec.lam,
        lambda_hat=lambda_hat,
        prediction_slope=metrics.fama_macbeth_for_horizon(prediction_series, label_series,
                                                          horizon=config.horizon),
        oos_r2=metrics.out_of_sample_r2(label_series, prediction_series),
        ic=metrics.information_coefficient(prediction_series, label_series),
        predictions=prediction_series,
        folds=tuple(folds),
        portfolio=portfolio,
        leaking=tuple(leaking(list(config.features))),
        n_rows=int(len(aligned)),
    )
