from __future__ import annotations

import argparse
import json
from pathlib import Path

import joblib
import numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report
from sklearn.model_selection import train_test_split

from feature_extractor import decode_wav_bytes, extract_features, prepare_signal

DEFAULT_LABELS = ("colic", "hunger")


def normalize_label(raw: str) -> str:
    v = raw.strip().lower()
    if v in {"colica", "cólica"}:
        return "colic"
    if v in {"fome", "hungry"}:
        return "hunger"
    return v


def collect_dataset(dataset_dir: Path) -> tuple[np.ndarray, np.ndarray]:
    xs: list[np.ndarray] = []
    ys: list[str] = []
    wav_exts = {".wav", ".wave"}

    for label_dir in sorted(dataset_dir.iterdir()):
        if not label_dir.is_dir():
            continue
        label = normalize_label(label_dir.name)
        if label not in DEFAULT_LABELS:
            continue

        for file in sorted(label_dir.rglob("*")):
            if file.suffix.lower() not in wav_exts:
                continue
            wav_bytes = file.read_bytes()
            samples, sr = decode_wav_bytes(wav_bytes)
            signal = prepare_signal(samples, sr)
            feats = extract_features(signal).vector
            xs.append(feats)
            ys.append(label)

    if not xs:
        raise RuntimeError("dataset_vazio")

    return np.vstack(xs).astype(np.float32), np.array(ys)


def main() -> None:
    parser = argparse.ArgumentParser(description="Treina classificador remoto CrySense (sklearn)")
    parser.add_argument("--dataset", required=True, help="Pasta com subpastas colic/hunger")
    parser.add_argument(
        "--out-model",
        default="models/cry_type_model.joblib",
        help="Caminho de saída do modelo .joblib",
    )
    parser.add_argument(
        "--out-labels",
        default="models/labels.json",
        help="Caminho de saída do arquivo labels.json",
    )
    args = parser.parse_args()

    dataset_dir = Path(args.dataset)
    out_model = Path(args.out_model)
    out_labels = Path(args.out_labels)
    out_model.parent.mkdir(parents=True, exist_ok=True)
    out_labels.parent.mkdir(parents=True, exist_ok=True)

    x, y = collect_dataset(dataset_dir)
    x_train, x_test, y_train, y_test = train_test_split(
        x,
        y,
        test_size=0.2,
        random_state=42,
        stratify=y if len(np.unique(y)) > 1 else None,
    )

    clf = RandomForestClassifier(
        n_estimators=400,
        max_depth=20,
        min_samples_leaf=2,
        class_weight="balanced_subsample",
        random_state=42,
        n_jobs=-1,
    )
    clf.fit(x_train, y_train)

    y_pred = clf.predict(x_test)
    print(classification_report(y_test, y_pred, digits=4))

    joblib.dump(clf, out_model)
    out_labels.write_text(json.dumps({"labels": list(DEFAULT_LABELS)}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Modelo salvo em: {out_model}")
    print(f"Labels salvos em: {out_labels}")


if __name__ == "__main__":
    main()

