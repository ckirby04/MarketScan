"""The two models, behind one interface.

Spec 2 M5 asks for a ridge and a gradient-boosted model. They are here for
contrast rather than competition: the panel's true relationship is linear by
construction, so ridge is the correctly-specified model and the GBM is the
over-flexible one. What the pair is for is the sweep in M8, where the question is
not which model is better but how much more often a flexible model claims to have
found something in data containing nothing.

Both are deterministic. A model whose result changes between runs cannot be part
of a calibration, and `random_state` is set on everything that has one.

Both standardize their inputs. Ridge's penalty is not scale-invariant, so an
unstandardized feature set makes the regularization strength mean something
different for each feature -- and since the features arrive cross-sectionally
normalized already, this is mostly a guard against a caller who turns that off.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Protocol

import numpy as np
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.linear_model import Ridge
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler

SEED = 20260918


class Model(Protocol):
    """What the pipeline needs from a model, and nothing else."""

    name: str

    def fit(self, x: np.ndarray, y: np.ndarray) -> "Model": ...

    def predict(self, x: np.ndarray) -> np.ndarray: ...


@dataclass
class RidgeModel:
    """Linear, penalized. The correctly-specified model for these panels."""

    alpha: float = 10.0
    name: str = field(default="", init=False)
    _fitted: Pipeline | None = field(default=None, init=False, repr=False)

    def __post_init__(self) -> None:
        self.name = f"ridge(alpha={self.alpha:g})"

    def fit(self, x: np.ndarray, y: np.ndarray) -> "RidgeModel":
        self._fitted = Pipeline([
            ("scale", StandardScaler()),
            ("model", Ridge(alpha=self.alpha, fit_intercept=True, random_state=None)),
        ]).fit(x, y)
        return self

    def predict(self, x: np.ndarray) -> np.ndarray:
        if self._fitted is None:
            raise RuntimeError("predict before fit")
        return np.asarray(self._fitted.predict(x), dtype=float)


@dataclass
class GBMModel:
    """Gradient-boosted trees. The over-flexible model, deliberately.

    Shallow and heavily early-stopped by default. A deep GBM on a panel whose
    true relationship is a straight line does not find a better answer, it finds
    the noise -- which is the point of including it in the false-discovery sweep,
    but would be a silly default for the recovery test.
    """

    max_depth: int = 3
    learning_rate: float = 0.05
    max_iter: int = 200
    min_samples_leaf: int = 200
    l2_regularization: float = 1.0
    name: str = field(default="", init=False)
    _fitted: Pipeline | None = field(default=None, init=False, repr=False)

    def __post_init__(self) -> None:
        self.name = (f"gbm(depth={self.max_depth},lr={self.learning_rate:g},"
                     f"iter={self.max_iter})")

    def fit(self, x: np.ndarray, y: np.ndarray) -> "GBMModel":
        self._fitted = Pipeline([
            ("scale", StandardScaler()),
            ("model", HistGradientBoostingRegressor(
                max_depth=self.max_depth,
                learning_rate=self.learning_rate,
                max_iter=self.max_iter,
                min_samples_leaf=self.min_samples_leaf,
                l2_regularization=self.l2_regularization,
                early_stopping=True,
                validation_fraction=0.1,
                n_iter_no_change=20,
                random_state=SEED,
            )),
        ]).fit(x, y)
        return self

    def predict(self, x: np.ndarray) -> np.ndarray:
        if self._fitted is None:
            raise RuntimeError("predict before fit")
        return np.asarray(self._fitted.predict(x), dtype=float)


@dataclass
class MeanModel:
    """Predicts the training mean for everything. The null model.

    Not a strawman: it is the thing every reported R-squared and Sharpe has to
    beat to mean anything, and having it here as an object the pipeline can run
    means the comparison is a measurement rather than an argument.
    """

    name: str = field(default="mean", init=False)
    _mean: float = field(default=0.0, init=False)

    def fit(self, x: np.ndarray, y: np.ndarray) -> "MeanModel":
        self._mean = float(np.mean(y)) if y.size else 0.0
        return self

    def predict(self, x: np.ndarray) -> np.ndarray:
        return np.full(len(x), self._mean, dtype=float)


def make_model(kind: str, **kwargs: float | int) -> Model:
    """Builds a model by name. What the M8 sweep configures itself through."""
    if kind == "ridge":
        return RidgeModel(**kwargs)  # type: ignore[arg-type]
    if kind == "gbm":
        return GBMModel(**kwargs)  # type: ignore[arg-type]
    if kind == "mean":
        return MeanModel()
    raise ValueError(f"unknown model {kind!r}; have ridge, gbm, mean")
