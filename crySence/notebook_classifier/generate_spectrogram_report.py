from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from classifier import CryTypeClassifier
from feature_extractor import TARGET_SECONDS, TARGET_SR, decode_wav_bytes, prepare_signal

DEFAULT_LABELS = ("colic", "hunger")
WAV_EXTENSIONS = {".wav", ".wave"}
EPS = 1e-9


@dataclass(frozen=True)
class DatasetSample:
    label: str
    path: Path


@dataclass(frozen=True)
class PredictionRow:
    true_label: str
    pred_label: str
    confidence: float
    score_colic: float
    score_hunger: float


@dataclass(frozen=True)
class AmplitudeExample:
    path: Path
    amplitude: np.ndarray


def normalize_label(raw: str) -> str:
    value = raw.strip().lower()
    if value in {"colica", "cólica"}:
        return "colic"
    if value in {"fome", "hungry"}:
        return "hunger"
    return value


def resolve_path(base_dir: Path, candidate: str) -> Path:
    path = Path(candidate)
    if path.is_absolute():
        return path
    return (base_dir / path).resolve()


def collect_dataset_samples(dataset_dir: Path, max_per_label: int) -> list[DatasetSample]:
    if not dataset_dir.exists():
        raise RuntimeError(f"dataset_nao_encontrado: {dataset_dir}")

    samples: list[DatasetSample] = []
    for label_dir in sorted(dataset_dir.iterdir()):
        if not label_dir.is_dir():
            continue
        label = normalize_label(label_dir.name)
        if label not in DEFAULT_LABELS:
            continue

        files = [p for p in sorted(label_dir.rglob("*")) if p.suffix.lower() in WAV_EXTENSIONS]
        if max_per_label > 0:
            files = files[:max_per_label]
        for path in files:
            samples.append(DatasetSample(label=label, path=path))
    return samples


def frame_signal(samples: np.ndarray, frame_size: int, hop_size: int) -> np.ndarray:
    if samples.shape[0] < frame_size:
        samples = np.pad(samples, (0, frame_size - samples.shape[0]), mode="constant")
    n_frames = 1 + (samples.shape[0] - frame_size) // hop_size
    shape = (n_frames, frame_size)
    strides = (samples.strides[0] * hop_size, samples.strides[0])
    return np.lib.stride_tricks.as_strided(samples, shape=shape, strides=strides).copy()


def spectrogram_db(
    samples: np.ndarray,
    sample_rate: int = TARGET_SR,
    frame_size: int = 512,
    hop_size: int = 256,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    window = np.hanning(frame_size).astype(np.float32)
    frames = frame_signal(samples.astype(np.float32), frame_size=frame_size, hop_size=hop_size)
    magnitude = np.abs(np.fft.rfft(frames * window, axis=1)).astype(np.float32) + EPS
    power = magnitude * magnitude
    spec = 10.0 * np.log10(power + EPS)
    spec = spec.T
    freqs = np.fft.rfftfreq(frame_size, d=1.0 / sample_rate).astype(np.float32)
    times = ((np.arange(spec.shape[1], dtype=np.float32) * hop_size) + frame_size / 2.0) / float(sample_rate)
    return spec, freqs, times


def plot_spectrogram_2d(
    spec_db: np.ndarray,
    freqs: np.ndarray,
    times: np.ndarray,
    title: str,
    out_path: Path,
    *,
    cmap: str = "magma",
    vmin: float | None = None,
    vmax: float | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=(11, 4.2))
    image = ax.imshow(
        spec_db,
        origin="lower",
        aspect="auto",
        extent=[float(times[0]), float(times[-1]), float(freqs[0]), float(freqs[-1])],
        cmap=cmap,
        vmin=vmin,
        vmax=vmax,
    )
    ax.set_title(title)
    ax.set_xlabel("Tempo (s)")
    ax.set_ylabel("Frequencia (Hz)")
    plt.colorbar(image, ax=ax, label="dB")
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def extract_frequency_rails(
    spec_db: np.ndarray,
    freqs: np.ndarray,
    *,
    n_rails: int = 6,
    min_freq_hz: float = 120.0,
    max_freq_hz: float = 4200.0,
    min_separation_hz: float = 150.0,
) -> np.ndarray:
    mask = (freqs >= min_freq_hz) & (freqs <= max_freq_hz)
    if not np.any(mask):
        return np.zeros(0, dtype=np.float32)

    profile = np.mean(spec_db[mask, :], axis=1)
    freqs_masked = freqs[mask]
    order = np.argsort(profile)[::-1]

    selected: list[float] = []
    for idx in order:
        hz = float(freqs_masked[idx])
        if all(abs(hz - current) >= min_separation_hz for current in selected):
            selected.append(hz)
        if len(selected) >= n_rails:
            break

    if not selected:
        return np.zeros(0, dtype=np.float32)
    return np.array(sorted(selected), dtype=np.float32)


def plot_spectrogram_inverted_with_rails(
    spec_db: np.ndarray,
    freqs: np.ndarray,
    times: np.ndarray,
    rails_hz: np.ndarray,
    title: str,
    out_path: Path,
    *,
    cmap: str = "magma",
    vmin: float | None = None,
    vmax: float | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=(11.3, 5.2))
    image = ax.imshow(
        spec_db.T,
        origin="lower",
        aspect="auto",
        extent=[float(freqs[0]), float(freqs[-1]), float(times[0]), float(times[-1])],
        cmap=cmap,
        vmin=vmin,
        vmax=vmax,
    )
    ax.set_title(title)
    ax.set_xlabel("Frequencia (Hz)")
    ax.set_ylabel("Tempo (s)")
    ax.grid(color="#ffffff", alpha=0.18, linewidth=0.6)

    if rails_hz.size > 0:
        for hz in rails_hz:
            ax.axvline(float(hz), color="#00ffc4", linewidth=1.2, linestyle="--", alpha=0.95)
            ax.text(
                float(hz),
                1.01,
                f"{int(round(float(hz)))} Hz",
                transform=ax.get_xaxis_transform(),
                rotation=90,
                va="bottom",
                ha="center",
                fontsize=7,
                color="#00ffd0",
            )

    plt.colorbar(image, ax=ax, label="dB")
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_frequency_rails_comparison(
    colic_spec: np.ndarray,
    hunger_spec: np.ndarray,
    freqs: np.ndarray,
    colic_rails_hz: np.ndarray,
    hunger_rails_hz: np.ndarray,
    out_path: Path,
) -> None:
    colic_profile = np.mean(colic_spec, axis=1)
    hunger_profile = np.mean(hunger_spec, axis=1)

    fig, ax = plt.subplots(figsize=(11.3, 4.5))
    ax.plot(freqs, colic_profile, color="#df4b4b", linewidth=2.0, label="colic")
    ax.plot(freqs, hunger_profile, color="#4589ff", linewidth=2.0, label="hunger")

    for hz in colic_rails_hz:
        ax.axvline(float(hz), color="#df4b4b", alpha=0.18, linewidth=1.0)
    for hz in hunger_rails_hz:
        ax.axvline(float(hz), color="#4589ff", alpha=0.18, linewidth=1.0)

    ax.set_title("Raias de frequencia dominantes (media do dataset)")
    ax.set_xlabel("Frequencia (Hz)")
    ax.set_ylabel("Energia media (dB)")
    ax.grid(color="#d0d0d0", linewidth=0.6, alpha=0.75)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def amplitude_spectrum(signal: np.ndarray, sample_rate: int = TARGET_SR) -> tuple[np.ndarray, np.ndarray]:
    x = signal.astype(np.float32)
    window = np.hanning(x.shape[0]).astype(np.float32)
    fft = np.fft.rfft(x * window)
    amplitude = np.abs(fft).astype(np.float32)
    scale = float(np.sum(window) / 2.0)
    if scale > 0.0:
        amplitude = amplitude / scale
    freqs = np.fft.rfftfreq(x.shape[0], d=1.0 / sample_rate).astype(np.float32)
    return amplitude, freqs


def smooth_curve(values: np.ndarray, window_size: int = 31) -> np.ndarray:
    if window_size <= 1 or values.shape[0] < window_size:
        return values
    kernel = np.ones(window_size, dtype=np.float32) / float(window_size)
    return np.convolve(values, kernel, mode="same").astype(np.float32)


def plot_amplitude_frequency_comparison(
    freqs: np.ndarray,
    colic_amplitude: np.ndarray,
    hunger_amplitude: np.ndarray,
    out_path: Path,
    *,
    max_freq_hz: float = 4200.0,
) -> None:
    mask = (freqs >= 0.0) & (freqs <= max_freq_hz)
    f = freqs[mask]
    colic = smooth_curve(colic_amplitude[mask], window_size=33)
    hunger = smooth_curve(hunger_amplitude[mask], window_size=33)

    max_amp = float(max(np.max(colic), np.max(hunger), EPS))
    colic_norm = colic / max_amp
    hunger_norm = hunger / max_amp

    fig, ax = plt.subplots(figsize=(11.3, 4.5))
    ax.plot(f, colic_norm, color="#df4b4b", linewidth=2.0, label="colic")
    ax.plot(f, hunger_norm, color="#4589ff", linewidth=2.0, label="hunger")
    ax.fill_between(f, colic_norm, 0.0, color="#df4b4b", alpha=0.10)
    ax.fill_between(f, hunger_norm, 0.0, color="#4589ff", alpha=0.10)
    ax.set_title("Amplitude x Frequencia (media do dataset)")
    ax.set_xlabel("Frequencia (Hz)")
    ax.set_ylabel("Amplitude normalizada")
    ax.set_ylim(0.0, 1.05)
    ax.grid(color="#d0d0d0", linewidth=0.6, alpha=0.75)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_amplitude_frequency_single(
    freqs: np.ndarray,
    amplitude: np.ndarray,
    out_path: Path,
    title: str,
    *,
    color: str,
    max_freq_hz: float = 4200.0,
) -> None:
    mask = (freqs >= 0.0) & (freqs <= max_freq_hz)
    f = freqs[mask]
    amp = smooth_curve(amplitude[mask], window_size=33)
    amp_norm = amp / float(max(np.max(amp), EPS))

    fig, ax = plt.subplots(figsize=(11.3, 4.3))
    ax.plot(f, amp_norm, color=color, linewidth=2.0)
    ax.fill_between(f, amp_norm, 0.0, color=color, alpha=0.12)
    ax.set_title(title)
    ax.set_xlabel("Frequencia (Hz)")
    ax.set_ylabel("Amplitude normalizada")
    ax.set_ylim(0.0, 1.05)
    ax.grid(color="#d0d0d0", linewidth=0.6, alpha=0.75)
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_amplitude_frequency_random_examples(
    freqs: np.ndarray,
    examples: list[AmplitudeExample],
    out_path: Path,
    title: str,
    *,
    cmap_name: str,
    max_freq_hz: float = 4200.0,
) -> None:
    mask = (freqs >= 0.0) & (freqs <= max_freq_hz)
    f = freqs[mask]
    curves = [smooth_curve(item.amplitude[mask], window_size=33) for item in examples]
    max_amp = float(max(max(np.max(curve), EPS) for curve in curves))

    fig, ax = plt.subplots(figsize=(11.3, 4.6))
    cmap = plt.get_cmap(cmap_name)
    total = max(1, len(examples) - 1)

    for idx, (item, curve) in enumerate(zip(examples, curves)):
        color = cmap(0.35 + 0.55 * (idx / total))
        curve_norm = curve / max_amp
        ax.plot(f, curve_norm, color=color, linewidth=1.8, label=item.path.name)

    ax.set_title(title)
    ax.set_xlabel("Frequencia (Hz)")
    ax.set_ylabel("Amplitude normalizada")
    ax.set_ylim(0.0, 1.05)
    ax.grid(color="#d0d0d0", linewidth=0.6, alpha=0.75)
    ax.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def signal_envelope_rms(signal: np.ndarray, frame_size: int = 512, hop_size: int = 256) -> np.ndarray:
    frames = frame_signal(signal.astype(np.float32), frame_size=frame_size, hop_size=hop_size)
    envelope = np.sqrt(np.mean(frames * frames, axis=1) + EPS)
    return envelope.astype(np.float32)


def plot_waveform_like_comparison(
    times: np.ndarray,
    colic_env: np.ndarray,
    hunger_env: np.ndarray,
    out_path: Path,
) -> None:
    max_value = float(max(np.max(colic_env), np.max(hunger_env), EPS))
    colic = colic_env / max_value
    hunger = hunger_env / max_value

    fig, axes = plt.subplots(2, 1, figsize=(11.8, 6.0), sharex=True)
    plots = [("colic", colic, axes[0]), ("hunger", hunger, axes[1])]

    for label, env, axis in plots:
        axis.set_facecolor("#ececec")
        axis.fill_between(times, env, -env, color="#00a800", alpha=0.94, linewidth=0.0)
        axis.plot(times, env, color="#007200", linewidth=0.5, alpha=0.95)
        axis.plot(times, -env, color="#007200", linewidth=0.5, alpha=0.95)
        axis.set_ylim(-1.05, 1.05)
        axis.set_ylabel("Amp. norm")
        axis.set_title(f"Forma de onda media (estilo leigo) - {label}")
        axis.grid(color="#c9c9c9", linewidth=0.7, alpha=0.9)

    axes[-1].set_xlabel("Tempo (s)")
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def downsample_surface(
    spec_db: np.ndarray,
    freqs: np.ndarray,
    times: np.ndarray,
    *,
    max_points: int = 90,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    freq_step = max(1, int(np.ceil(spec_db.shape[0] / float(max_points))))
    time_step = max(1, int(np.ceil(spec_db.shape[1] / float(max_points))))
    z = spec_db[::freq_step, ::time_step]
    f = freqs[::freq_step]
    t = times[::time_step]
    t_grid, f_grid = np.meshgrid(t, f)
    return t_grid, f_grid, z


def plot_spectrogram_3d_pair(
    colic_spec: np.ndarray,
    hunger_spec: np.ndarray,
    freqs: np.ndarray,
    times: np.ndarray,
    out_path: Path,
) -> None:
    fig = plt.figure(figsize=(14, 5.5))
    classes = [("colic", colic_spec, "viridis"), ("hunger", hunger_spec, "plasma")]

    for index, (label, spec_db, cmap) in enumerate(classes, start=1):
        axis = fig.add_subplot(1, 2, index, projection="3d")
        t_grid, f_grid, z = downsample_surface(spec_db, freqs, times, max_points=85)
        surface = axis.plot_surface(
            t_grid,
            f_grid,
            z,
            cmap=cmap,
            linewidth=0,
            antialiased=False,
            rcount=z.shape[0],
            ccount=z.shape[1],
        )
        axis.set_title(f"Media espectral 3D - {label}")
        axis.set_xlabel("Tempo (s)")
        axis.set_ylabel("Frequencia (Hz)")
        axis.set_zlabel("dB")
        fig.colorbar(surface, ax=axis, shrink=0.65, pad=0.08)

    fig.tight_layout()
    fig.savefig(out_path, dpi=170)
    plt.close(fig)


def summarize_predictions(rows: list[PredictionRow]) -> tuple[np.ndarray, dict[str, dict[str, float]], dict[str, int]]:
    label_to_idx = {label: idx for idx, label in enumerate(DEFAULT_LABELS)}
    confusion = np.zeros((2, 2), dtype=np.int32)
    by_true = {
        label: {
            "count": 0.0,
            "avg_confidence": 0.0,
            "avg_score_colic": 0.0,
            "avg_score_hunger": 0.0,
        }
        for label in DEFAULT_LABELS
    }
    predicted_counts = {label: 0 for label in DEFAULT_LABELS}

    for row in rows:
        confusion[label_to_idx[row.true_label], label_to_idx[row.pred_label]] += 1
        predicted_counts[row.pred_label] += 1
        item = by_true[row.true_label]
        item["count"] += 1.0
        item["avg_confidence"] += row.confidence
        item["avg_score_colic"] += row.score_colic
        item["avg_score_hunger"] += row.score_hunger

    for label, data in by_true.items():
        count = data["count"]
        if count > 0.0:
            data["avg_confidence"] /= count
            data["avg_score_colic"] /= count
            data["avg_score_hunger"] /= count

    return confusion, by_true, predicted_counts


def plot_triage_summary(
    confusion: np.ndarray,
    by_true: dict[str, dict[str, float]],
    out_path: Path,
) -> None:
    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(12.5, 4.5))

    heat = ax0.imshow(confusion, cmap="Blues")
    ax0.set_title("Matriz de triagem (real x previsto)")
    ax0.set_xticks(np.arange(len(DEFAULT_LABELS)), labels=list(DEFAULT_LABELS))
    ax0.set_yticks(np.arange(len(DEFAULT_LABELS)), labels=list(DEFAULT_LABELS))
    ax0.set_xlabel("Previsto")
    ax0.set_ylabel("Real")
    for i in range(confusion.shape[0]):
        for j in range(confusion.shape[1]):
            ax0.text(j, i, int(confusion[i, j]), ha="center", va="center", color="black")
    fig.colorbar(heat, ax=ax0, fraction=0.046, pad=0.04)

    labels = list(DEFAULT_LABELS)
    x = np.arange(len(labels), dtype=np.float32)
    score_colic = np.array([by_true[label]["avg_score_colic"] for label in labels], dtype=np.float32)
    score_hunger = np.array([by_true[label]["avg_score_hunger"] for label in labels], dtype=np.float32)
    width = 0.33
    ax1.bar(x - width / 2.0, score_colic, width, label="score colic")
    ax1.bar(x + width / 2.0, score_hunger, width, label="score hunger")
    ax1.set_ylim(0.0, 1.0)
    ax1.set_xticks(x, labels)
    ax1.set_title("Separacao media de scores por classe real")
    ax1.set_ylabel("Score medio")
    ax1.legend()

    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Gera visualizacao 2D/3D de espectrograma e resumo final de triagem colic vs hunger."
    )
    parser.add_argument("--dataset", default="dataset", help="Pasta com subpastas colic/hunger.")
    parser.add_argument("--output", default="visual_report", help="Pasta de saida para PNG/JSON.")
    parser.add_argument("--model", default="models/cry_type_model.joblib", help="Modelo .joblib.")
    parser.add_argument("--labels", default="models/labels.json", help="Arquivo labels.json.")
    parser.add_argument(
        "--max-per-label",
        type=int,
        default=0,
        help="Limite de arquivos por classe (0 = sem limite).",
    )
    parser.add_argument(
        "--skip-3d",
        action="store_true",
        help="Nao gera grafico 3D.",
    )
    parser.add_argument(
        "--random-samples-per-class",
        type=int,
        default=3,
        help="Quantidade de audios aleatorios por classe para graficos amplitude x frequencia.",
    )
    parser.add_argument(
        "--random-seed",
        type=int,
        default=42,
        help="Seed para selecao aleatoria reproduzivel.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    base_dir = Path(__file__).resolve().parent
    dataset_dir = resolve_path(base_dir, args.dataset)
    output_dir = resolve_path(base_dir, args.output)
    model_path = resolve_path(base_dir, args.model)
    labels_path = resolve_path(base_dir, args.labels)
    output_dir.mkdir(parents=True, exist_ok=True)

    samples = collect_dataset_samples(dataset_dir, max_per_label=max(0, int(args.max_per_label)))
    if not samples:
        raise RuntimeError(f"dataset_vazio_em: {dataset_dir}")

    classifier = CryTypeClassifier(
        model_path=str(model_path) if model_path.exists() else None,
        labels_path=str(labels_path) if labels_path.exists() else None,
    )

    per_label_specs: dict[str, list[np.ndarray]] = {label: [] for label in DEFAULT_LABELS}
    per_label_envelopes: dict[str, list[np.ndarray]] = {label: [] for label in DEFAULT_LABELS}
    per_label_amplitudes: dict[str, list[np.ndarray]] = {label: [] for label in DEFAULT_LABELS}
    per_label_amplitude_examples: dict[str, list[AmplitudeExample]] = {label: [] for label in DEFAULT_LABELS}
    prediction_rows: list[PredictionRow] = []
    skipped_files: list[str] = []
    reference_freqs: np.ndarray | None = None
    reference_times: np.ndarray | None = None
    reference_amplitude_freqs: np.ndarray | None = None

    for sample in samples:
        wav_bytes = sample.path.read_bytes()
        try:
            decoded, sample_rate = decode_wav_bytes(wav_bytes)
        except ValueError as exc:
            skipped_files.append(f"{sample.path}: {exc}")
            continue

        signal = prepare_signal(
            decoded,
            sample_rate,
            target_sr=TARGET_SR,
            target_samples=TARGET_SR * TARGET_SECONDS,
        )
        spec_db, freqs, times = spectrogram_db(signal, sample_rate=TARGET_SR)
        amplitude, amplitude_freqs = amplitude_spectrum(signal, sample_rate=TARGET_SR)
        per_label_specs[sample.label].append(spec_db)
        per_label_envelopes[sample.label].append(signal_envelope_rms(signal))
        per_label_amplitudes[sample.label].append(amplitude)
        per_label_amplitude_examples[sample.label].append(AmplitudeExample(path=sample.path, amplitude=amplitude))

        if reference_freqs is None or reference_times is None:
            reference_freqs = freqs
            reference_times = times
        if reference_amplitude_freqs is None:
            reference_amplitude_freqs = amplitude_freqs

        prediction = classifier.predict_wav_bytes(wav_bytes)
        prediction_rows.append(
            PredictionRow(
                true_label=sample.label,
                pred_label=prediction["label"],
                confidence=float(prediction["confianca"]),
                score_colic=float(prediction["scores"]["colic"]),
                score_hunger=float(prediction["scores"]["hunger"]),
            )
        )

    for label in DEFAULT_LABELS:
        if not per_label_specs[label]:
            raise RuntimeError(f"sem_amostras_validas_para: {label}")
    if reference_freqs is None or reference_times is None or reference_amplitude_freqs is None:
        raise RuntimeError("sem_espectrogramas_validos")

    mean_specs = {
        label: np.mean(np.stack(per_label_specs[label], axis=0), axis=0).astype(np.float32)
        for label in DEFAULT_LABELS
    }
    mean_envelopes = {
        label: np.mean(np.stack(per_label_envelopes[label], axis=0), axis=0).astype(np.float32)
        for label in DEFAULT_LABELS
    }
    mean_amplitudes = {
        label: np.mean(np.stack(per_label_amplitudes[label], axis=0), axis=0).astype(np.float32)
        for label in DEFAULT_LABELS
    }
    diff_spec = mean_specs["colic"] - mean_specs["hunger"]
    rails_colic = extract_frequency_rails(mean_specs["colic"], reference_freqs)
    rails_hunger = extract_frequency_rails(mean_specs["hunger"], reference_freqs)
    rails_diff = np.unique(np.concatenate([rails_colic, rails_hunger])).astype(np.float32)

    global_min = float(min(np.min(mean_specs["colic"]), np.min(mean_specs["hunger"])))
    global_max = float(max(np.max(mean_specs["colic"]), np.max(mean_specs["hunger"])))
    diff_abs = float(np.max(np.abs(diff_spec)))
    if diff_abs <= 0.0:
        diff_abs = 1.0

    out_colic_2d = output_dir / "spectrogram_colic_mean_2d.png"
    out_hunger_2d = output_dir / "spectrogram_hunger_mean_2d.png"
    out_diff_2d = output_dir / "spectrogram_diff_colic_minus_hunger_2d.png"
    out_colic_inv = output_dir / "spectrogram_colic_invertido_raias.png"
    out_hunger_inv = output_dir / "spectrogram_hunger_invertido_raias.png"
    out_diff_inv = output_dir / "spectrogram_diff_invertido_raias.png"
    out_rails_cmp = output_dir / "raias_frequencia_comparativo.png"
    out_amp_freq = output_dir / "amplitude_x_frequencia.png"
    out_amp_freq_colic = output_dir / "amplitude_x_frequencia_media_colic.png"
    out_amp_freq_hunger = output_dir / "amplitude_x_frequencia_media_hunger.png"
    out_amp_freq_random_colic = output_dir / "amplitude_x_frequencia_random_colic.png"
    out_amp_freq_random_hunger = output_dir / "amplitude_x_frequencia_random_hunger.png"
    out_amp_freq_random_dir = output_dir / "amplitude_x_frequencia_random_audios"
    out_waveform_like = output_dir / "forma_onda_media_leigo.png"
    out_triage = output_dir / "triagem_resumo.png"
    out_summary = output_dir / "triagem_resumo.json"
    out_3d = output_dir / "spectrogram_means_3d.png"
    out_amp_freq_random_dir.mkdir(parents=True, exist_ok=True)

    plot_spectrogram_2d(
        mean_specs["colic"],
        reference_freqs,
        reference_times,
        title="Espectrograma medio - colic",
        out_path=out_colic_2d,
        cmap="magma",
        vmin=global_min,
        vmax=global_max,
    )
    plot_spectrogram_2d(
        mean_specs["hunger"],
        reference_freqs,
        reference_times,
        title="Espectrograma medio - hunger",
        out_path=out_hunger_2d,
        cmap="magma",
        vmin=global_min,
        vmax=global_max,
    )
    plot_spectrogram_2d(
        diff_spec,
        reference_freqs,
        reference_times,
        title="Diferenca espectral media (colic - hunger)",
        out_path=out_diff_2d,
        cmap="seismic",
        vmin=-diff_abs,
        vmax=diff_abs,
    )
    plot_spectrogram_inverted_with_rails(
        mean_specs["colic"],
        reference_freqs,
        reference_times,
        rails_colic,
        title="Espectrograma invertido com raias - colic",
        out_path=out_colic_inv,
        cmap="magma",
        vmin=global_min,
        vmax=global_max,
    )
    plot_spectrogram_inverted_with_rails(
        mean_specs["hunger"],
        reference_freqs,
        reference_times,
        rails_hunger,
        title="Espectrograma invertido com raias - hunger",
        out_path=out_hunger_inv,
        cmap="magma",
        vmin=global_min,
        vmax=global_max,
    )
    plot_spectrogram_inverted_with_rails(
        diff_spec,
        reference_freqs,
        reference_times,
        rails_diff,
        title="Diferenca espectral invertida com raias (colic - hunger)",
        out_path=out_diff_inv,
        cmap="seismic",
        vmin=-diff_abs,
        vmax=diff_abs,
    )
    plot_frequency_rails_comparison(
        mean_specs["colic"],
        mean_specs["hunger"],
        reference_freqs,
        rails_colic,
        rails_hunger,
        out_rails_cmp,
    )
    plot_amplitude_frequency_comparison(
        reference_amplitude_freqs,
        mean_amplitudes["colic"],
        mean_amplitudes["hunger"],
        out_amp_freq,
    )
    plot_amplitude_frequency_single(
        reference_amplitude_freqs,
        mean_amplitudes["colic"],
        out_amp_freq_colic,
        title="Amplitude x Frequencia - media geral colic",
        color="#df4b4b",
    )
    plot_amplitude_frequency_single(
        reference_amplitude_freqs,
        mean_amplitudes["hunger"],
        out_amp_freq_hunger,
        title="Amplitude x Frequencia - media geral hunger",
        color="#4589ff",
    )

    random_samples_per_class = max(1, int(args.random_samples_per_class))
    rng = np.random.default_rng(int(args.random_seed))
    random_selected_files: dict[str, list[str]] = {label: [] for label in DEFAULT_LABELS}
    random_generated_files: list[str] = []

    for label, color, cmap_name, out_overlay in [
        ("colic", "#df4b4b", "Reds", out_amp_freq_random_colic),
        ("hunger", "#4589ff", "Blues", out_amp_freq_random_hunger),
    ]:
        pool = per_label_amplitude_examples[label]
        pick_count = min(len(pool), random_samples_per_class)
        if pick_count <= 0:
            continue

        selected_idx = rng.choice(len(pool), size=pick_count, replace=False)
        selected_examples = [pool[int(idx)] for idx in selected_idx]
        plot_amplitude_frequency_random_examples(
            reference_amplitude_freqs,
            selected_examples,
            out_overlay,
            title=f"Amplitude x Frequencia - audios aleatorios ({label})",
            cmap_name=cmap_name,
        )

        for sample_idx, example in enumerate(selected_examples, start=1):
            out_single = out_amp_freq_random_dir / f"amplitude_x_frequencia_random_{label}_{sample_idx:02d}.png"
            plot_amplitude_frequency_single(
                reference_amplitude_freqs,
                example.amplitude,
                out_single,
                title=f"Amplitude x Frequencia - {label} - {example.path.name}",
                color=color,
            )
            random_generated_files.append(str(out_single))
            random_selected_files[label].append(str(example.path))

    plot_waveform_like_comparison(
        reference_times,
        mean_envelopes["colic"],
        mean_envelopes["hunger"],
        out_waveform_like,
    )
    if not args.skip_3d:
        plot_spectrogram_3d_pair(
            mean_specs["colic"],
            mean_specs["hunger"],
            reference_freqs,
            reference_times,
            out_path=out_3d,
        )

    confusion, by_true, predicted_counts = summarize_predictions(prediction_rows)
    plot_triage_summary(confusion, by_true, out_triage)

    total = int(sum(int(v["count"]) for v in by_true.values()))
    correct = int(np.trace(confusion))
    accuracy = float(correct / total) if total > 0 else 0.0
    files_per_label = {label: int(sum(1 for sample in samples if sample.label == label)) for label in DEFAULT_LABELS}
    generated_files = [
        str(out_colic_2d),
        str(out_hunger_2d),
        str(out_diff_2d),
        str(out_colic_inv),
        str(out_hunger_inv),
        str(out_diff_inv),
        str(out_rails_cmp),
        str(out_amp_freq),
        str(out_amp_freq_colic),
        str(out_amp_freq_hunger),
        str(out_amp_freq_random_colic),
        str(out_amp_freq_random_hunger),
        str(out_waveform_like),
        str(out_triage),
        str(out_summary),
    ]
    generated_files.extend(random_generated_files)
    if not args.skip_3d:
        generated_files.append(str(out_3d))

    summary = {
        "dataset_dir": str(dataset_dir),
        "model_path": str(model_path) if model_path.exists() else None,
        "labels_path": str(labels_path) if labels_path.exists() else None,
        "files_per_label": files_per_label,
        "samples_processed": int(total),
        "samples_skipped": int(len(skipped_files)),
        "accuracy": accuracy,
        "predicted_counts": predicted_counts,
        "by_true_label": by_true,
        "confusion_matrix": confusion.tolist(),
        "frequency_rails_hz": {
            "colic": [float(v) for v in rails_colic.tolist()],
            "hunger": [float(v) for v in rails_hunger.tolist()],
        },
        "amplitude_peak_hz": {
            "colic": float(reference_amplitude_freqs[int(np.argmax(mean_amplitudes["colic"]))]),
            "hunger": float(reference_amplitude_freqs[int(np.argmax(mean_amplitudes["hunger"]))]),
        },
        "random_selection": {
            "samples_per_class": random_samples_per_class,
            "seed": int(args.random_seed),
            "selected_files": random_selected_files,
        },
        "generated_files": generated_files,
        "skipped_files": skipped_files[:20],
    }
    out_summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

    print("Relatorio de espectrograma e triagem gerado com sucesso.")
    print(f"Saida: {output_dir}")
    for generated in generated_files:
        print(f" - {generated}")


if __name__ == "__main__":
    main()
