from __future__ import annotations

import io
import wave
from dataclasses import dataclass

import numpy as np

TARGET_SR = 16000
TARGET_SECONDS = 6
TARGET_SAMPLES = TARGET_SR * TARGET_SECONDS
EPS = 1e-9


@dataclass(frozen=True)
class AudioFeatures:
    vector: np.ndarray
    details: dict[str, float]


def decode_wav_bytes(wav_bytes: bytes) -> tuple[np.ndarray, int]:
    if not wav_bytes:
        raise ValueError("empty_audio")

    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            n_channels = wf.getnchannels()
            sample_rate = wf.getframerate()
            sample_width = wf.getsampwidth()
            n_frames = wf.getnframes()
            pcm = wf.readframes(n_frames)
    except wave.Error as exc:
        raise ValueError(f"invalid_wav: {exc}") from exc

    if sample_width == 1:
        samples = np.frombuffer(pcm, dtype=np.uint8).astype(np.float32)
        samples = (samples - 128.0) / 128.0
    elif sample_width == 2:
        samples = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    elif sample_width == 4:
        samples = np.frombuffer(pcm, dtype=np.int32).astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"unsupported_sample_width_{sample_width}")

    if n_channels > 1:
        samples = samples.reshape(-1, n_channels).mean(axis=1)

    return samples.astype(np.float32), sample_rate


def _resample_linear(samples: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    if orig_sr == target_sr:
        return samples
    if samples.size == 0:
        return np.zeros(0, dtype=np.float32)

    target_len = int(round(samples.shape[0] * (target_sr / float(orig_sr))))
    if target_len <= 1:
        return np.zeros(1, dtype=np.float32)

    src_x = np.linspace(0.0, 1.0, num=samples.shape[0], endpoint=True)
    dst_x = np.linspace(0.0, 1.0, num=target_len, endpoint=True)
    return np.interp(dst_x, src_x, samples).astype(np.float32)


def prepare_signal(
    samples: np.ndarray,
    sample_rate: int,
    target_sr: int = TARGET_SR,
    target_samples: int = TARGET_SAMPLES,
) -> np.ndarray:
    if sample_rate != target_sr:
        samples = _resample_linear(samples, sample_rate, target_sr)

    if samples.shape[0] < target_samples:
        samples = np.pad(samples, (0, target_samples - samples.shape[0]), mode="constant")
    elif samples.shape[0] > target_samples:
        samples = samples[:target_samples]

    return samples.astype(np.float32)


def _frame_signal(samples: np.ndarray, frame_size: int, hop_size: int) -> np.ndarray:
    if samples.shape[0] < frame_size:
        samples = np.pad(samples, (0, frame_size - samples.shape[0]), mode="constant")

    n_frames = 1 + (samples.shape[0] - frame_size) // hop_size
    shape = (n_frames, frame_size)
    strides = (samples.strides[0] * hop_size, samples.strides[0])
    return np.lib.stride_tricks.as_strided(samples, shape=shape, strides=strides).copy()


def extract_features(samples: np.ndarray, sample_rate: int = TARGET_SR) -> AudioFeatures:
    x = samples.astype(np.float32)
    frame_size = 512
    hop_size = 256
    window = np.hanning(frame_size).astype(np.float32)
    frames = _frame_signal(x, frame_size=frame_size, hop_size=hop_size)
    windowed = frames * window

    mag = np.abs(np.fft.rfft(windowed, axis=1)).astype(np.float32) + EPS
    power = mag * mag
    freqs = np.fft.rfftfreq(frame_size, d=1.0 / sample_rate).astype(np.float32)

    rms = float(np.sqrt(np.mean(x * x) + EPS))
    peak = float(np.max(np.abs(x)) if x.size else 0.0)
    zcr = float(np.mean(np.abs(np.diff(np.signbit(x).astype(np.int8)))))

    p_sum = np.sum(power, axis=1) + EPS
    centroid_frames = np.sum(power * freqs[None, :], axis=1) / p_sum
    centroid = float(np.mean(centroid_frames))

    cumsum = np.cumsum(power, axis=1)
    roll_target = 0.85 * p_sum[:, None]
    roll_idx = np.argmax(cumsum >= roll_target, axis=1)
    rolloff = float(np.mean(freqs[roll_idx]))

    bandwidth_frames = np.sqrt(
        np.sum(((freqs[None, :] - centroid_frames[:, None]) ** 2) * power, axis=1) / p_sum
    )
    bandwidth = float(np.mean(bandwidth_frames))

    geo = np.exp(np.mean(np.log(mag), axis=1))
    arith = np.mean(mag, axis=1) + EPS
    flatness = float(np.mean(geo / arith))

    if mag.shape[0] > 1:
        flux = float(np.mean(np.sqrt(np.mean((mag[1:] - mag[:-1]) ** 2, axis=1))))
    else:
        flux = 0.0

    frame_rms = np.sqrt(np.mean(windowed * windowed, axis=1) + EPS)
    frame_rms_mean = float(np.mean(frame_rms))
    frame_rms_std = float(np.std(frame_rms))
    modulation = float(frame_rms_std / (frame_rms_mean + EPS))

    details = {
        "rms": rms,
        "peak": peak,
        "zcr": zcr,
        "centroid": centroid,
        "rolloff85": rolloff,
        "bandwidth": bandwidth,
        "flatness": flatness,
        "flux": flux,
        "modulation": modulation,
        "frame_rms_mean": frame_rms_mean,
        "frame_rms_std": frame_rms_std,
    }

    vector = np.array(
        [
            rms,
            peak,
            zcr,
            centroid,
            rolloff,
            bandwidth,
            flatness,
            flux,
            modulation,
            frame_rms_mean,
            frame_rms_std,
        ],
        dtype=np.float32,
    )

    return AudioFeatures(vector=vector, details=details)

