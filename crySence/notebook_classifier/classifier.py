from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np

from feature_extractor import TARGET_SR, decode_wav_bytes, extract_features, prepare_signal

try:
    import joblib
except Exception:  # pragma: no cover
    joblib = None

DEFAULT_LABELS = ("colic", "hunger")


def _normalize_label(label: str) -> str:
    v = (label or "").strip().lower()
    if v in {"colica", "cólica"}:
        return "colic"
    if v in {"fome", "hungry"}:
        return "hunger"
    if v in DEFAULT_LABELS:
        return v
    return ""


def _softmax(values: np.ndarray) -> np.ndarray:
    x = values.astype(np.float32)
    x = x - np.max(x)
    e = np.exp(x)
    s = np.sum(e)
    if s <= 0:
        return np.ones_like(x) / float(x.shape[0])
    return e / s


def _clip01(v: float) -> float:
    return float(max(0.0, min(1.0, v)))


class CryTypeClassifier:
    def __init__(self, model_path: str | None = None, labels_path: str | None = None) -> None:
        self.labels = list(DEFAULT_LABELS)
        self.model = None

        if labels_path:
            p = Path(labels_path)
            if p.exists():
                data = json.loads(p.read_text(encoding="utf-8"))
                if isinstance(data, list) and data:
                    self.labels = [_normalize_label(str(v)) for v in data]
                elif isinstance(data, dict) and "labels" in data and isinstance(data["labels"], list):
                    self.labels = [_normalize_label(str(v)) for v in data["labels"]]

        if model_path and joblib is not None:
            p = Path(model_path)
            if p.exists():
                self.model = joblib.load(p)

    def predict_wav_bytes(self, wav_bytes: bytes, trigger_conf: float | None = None) -> dict[str, Any]:
        samples, sample_rate = decode_wav_bytes(wav_bytes)
        return self.predict_samples(samples=samples, sample_rate=sample_rate, trigger_conf=trigger_conf)

    def predict_samples(
        self,
        samples: np.ndarray,
        sample_rate: int,
        trigger_conf: float | None = None,
    ) -> dict[str, Any]:
        signal = prepare_signal(samples, sample_rate, target_sr=TARGET_SR)
        feats = extract_features(signal, sample_rate=TARGET_SR)

        if self.model is not None:
            try:
                scores = self._predict_with_model(feats.vector)
                if not scores or sum(max(0.0, float(v)) for v in scores.values()) <= 0.0:
                    scores = self._predict_heuristic(feats.details, trigger_conf)
            except Exception:
                scores = self._predict_heuristic(feats.details, trigger_conf)
        else:
            scores = self._predict_heuristic(feats.details, trigger_conf)

        for label in DEFAULT_LABELS:
            scores.setdefault(label, 0.0)

        total = sum(max(0.0, float(v)) for v in scores.values())
        if total <= 0.0:
            uniform = 1.0 / float(len(DEFAULT_LABELS))
            scores = {k: uniform for k in DEFAULT_LABELS}
        else:
            scores = {k: max(0.0, float(v)) / total for k, v in scores.items()}

        label = max(DEFAULT_LABELS, key=lambda k: scores[k])
        conf = float(scores[label])

        return {
            "label": label,
            "confianca": _clip01(conf),
            "scores": {
                "colic": _clip01(scores["colic"]),
                "hunger": _clip01(scores["hunger"]),
            },
            "features": {
                "rms": feats.details["rms"],
                "zcr": feats.details["zcr"],
                "centroid": feats.details["centroid"],
                "flatness": feats.details["flatness"],
                "modulation": feats.details["modulation"],
            },
        }

    def _predict_with_model(self, feature_vector: np.ndarray) -> dict[str, float]:
        x = feature_vector.reshape(1, -1)
        scores: dict[str, float] = {k: 0.0 for k in DEFAULT_LABELS}

        if hasattr(self.model, "predict_proba"):
            proba = np.asarray(self.model.predict_proba(x)[0], dtype=np.float32)
            classes = getattr(self.model, "classes_", np.array(self.labels))
            for c, p in zip(classes, proba):
                lbl = _normalize_label(str(c))
                if lbl:
                    scores[lbl] += float(max(0.0, p))
            return scores

        pred = self.model.predict(x)[0]
        pred_lbl = _normalize_label(str(pred))
        if pred_lbl:
            scores[pred_lbl] = 1.0
        return scores

    def _predict_heuristic(self, details: dict[str, float], trigger_conf: float | None) -> dict[str, float]:
        rms_n = _clip01((details["rms"] - 0.008) / 0.08)
        zcr_n = _clip01((details["zcr"] - 0.02) / 0.22)
        centroid_n = _clip01((details["centroid"] - 300.0) / 2800.0)
        flatness_n = _clip01((details["flatness"] - 0.12) / 0.62)
        flux_n = _clip01(details["flux"] / 0.23)
        mod_n = _clip01(details["modulation"] / 1.2)
        trigger_n = _clip01(trigger_conf if trigger_conf is not None else 0.7)

        logits = np.array(
            [
                # colic: energia alta + tom mais grave + maior modulação.
                1.9 * rms_n + 1.2 * (1.0 - centroid_n) + 1.2 * mod_n + 0.5 * (1.0 - zcr_n),
                # hunger: tom/agressividade mais alta e maior fluxo espectral.
                1.5 * rms_n + 1.2 * centroid_n + 1.0 * zcr_n + 0.9 * flux_n + 0.3 * mod_n,
            ],
            dtype=np.float32,
        )

        # Trigger local vindo do ESP ajuda a reforçar predições de choro.
        logits[0] += max(0.0, trigger_n - 0.60) * 0.35
        logits[1] += max(0.0, trigger_n - 0.60) * 0.25
        logits[0] += max(0.0, 0.55 - trigger_n) * 0.15
        logits[1] += max(0.0, 0.55 - trigger_n) * 0.10

        # Penalização leve para sinais muito "flat" (geralmente menos informativos).
        logits[0] -= flatness_n * 0.60
        logits[1] -= flatness_n * 0.50

        probs = _softmax(logits)
        return {
            "colic": float(probs[0]),
            "hunger": float(probs[1]),
        }

