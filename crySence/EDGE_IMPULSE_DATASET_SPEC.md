# CrySense AI - Edge Impulse dataset specification

Updated: 2026-03-15

This document defines the dataset spec for the two-model architecture:

1. model A: binary detector (`choro` vs `nao_choro`)
2. model B: cry subtype classifier (`colica`, `fome`, `sono`)

The goal is to reduce false positives in mixed/background audio and keep subtype output stable only when cry is real.

## 1. Shared audio standard

Use the same source format for both projects:

- sample rate: `16000 Hz`
- channels: `mono`
- bit depth: `16-bit PCM`
- file format: `.wav`
- window size used by model training: `1.0 s`

Recommendation:

- keep raw clips as full recordings first
- generate labeled 1-second windows after annotation
- avoid mixing train/test windows from the same original recording

## 2. Edge Impulse project split

Create two separate EI projects:

- `crysense-detector-v1`
- `crysense-subtype-v1`

Do not train both tasks in one multiclass project.

## 3. Model A dataset (`choro` vs `nao_choro`)

### 3.1 Labels

Only two classes:

- `choro`
- `nao_choro`

`nao_choro` must include hard negatives and mixed scenes.

### 3.2 Minimum target size (v1)

Targets in 1-second windows after cleaning:

- `choro`: `>= 6000` windows
- `nao_choro`: `>= 9000` windows
- total: `>= 15000` windows

Reason:

- higher negative diversity is required to reduce false alarms from music, speech, TV, fan, and similar sounds.

### 3.3 Negative class composition (required)

Inside `nao_choro`, keep approximate distribution:

- speech (adult voices, conversations): `20%`
- TV / podcast / radio content: `15%`
- music (multiple genres): `20%`
- home appliance noise (fan, vacuum, blender, AC): `15%`
- street/outdoor noise (traffic, siren, construction): `10%`
- animal sounds (dog, cat, birds): `10%`
- silence / low-energy room tone: `10%`

If one bucket is missing, the detector will overfit and fail in deployment.

### 3.4 Mixed-scene requirement

At least `25%` of `nao_choro` must include layered/mixed sound scenes (not isolated clean sounds):

- music + speech
- TV + fan
- speech + appliance
- dog + TV

At least `30%` of `choro` should include moderate background noise.

### 3.5 Data split policy

Split by recording session and source identity, not by random windows only.

Recommended split:

- train: `70%`
- validation: `15%`
- test: `15%`

Hard rules:

- all windows from the same raw file must stay in one split
- if `baby_id` is known, keep each baby mostly in a single split for stricter generalization test
- if source device/mic changes, ensure each split contains multiple devices

### 3.6 Augmentation policy

Apply on train split only.

Allowed augmentations:

- gain jitter (`-6 dB` to `+6 dB`)
- background noise mix (`SNR 5-20 dB`)
- small time shift (`+-100 ms`)
- mild pitch shift (`+-2 semitones`) for robustness only

Do not augment validation/test sets.

### 3.7 Acceptance criteria for detector

Deploy gate only if all are true on test set:

- recall(`choro`) `>= 0.95`
- precision(`choro`) `>= 0.90`
- false positive rate on `nao_choro` `<= 0.03` per window
- in hard-negative subset (TV/music/speech mix), false positive rate `<= 0.05`

## 4. Model B dataset (`colica`, `fome`, `sono`)

### 4.1 Labels

Three classes only:

- `colica`
- `fome`
- `sono`

No `noise` class in this model.

### 4.2 Minimum target size (v1)

Balanced targets in 1-second windows:

- `colica`: `>= 2500`
- `fome`: `>= 2500`
- `sono`: `>= 2500`
- total: `>= 7500`

If one class is under target, use class-weighting and collect more before final deploy.

### 4.3 Label quality rules

Each window must satisfy:

- cry is audible and dominant
- label confidence by annotator is `>= 0.7` (scale 0-1)
- uncertain windows (`< 0.7`) go to `review` bucket, not train

At least two review passes for subtype labels:

- pass 1: initial label
- pass 2: disagreement resolution

### 4.4 Context balance for subtype model

Each subtype should contain diverse contexts:

- near microphone / far microphone
- quiet room / noisy room
- day / night
- different babies (if available)

Target: no single context should represent more than `35%` of one class.

### 4.5 Data split policy

Use the same leak prevention rules as model A.

Recommended split:

- train: `70%`
- validation: `15%`
- test: `15%`

### 4.6 Augmentation policy

Apply only to train split:

- gain jitter (`-4 dB` to `+4 dB`)
- light background mix (`SNR 10-25 dB`)
- small time shift (`+-80 ms`)

Avoid aggressive pitch shift in subtype model, since subtype cues may depend on prosody and F0 behavior.

### 4.7 Acceptance criteria for subtype model

Deploy only if all are true on test set:

- macro F1 `>= 0.85`
- per-class recall `>= 0.80`
- per-class precision `>= 0.80`
- confusion hot-spot review completed (especially `colica` vs `fome`)

## 5. Annotation schema (mandatory metadata)

Keep one metadata table for both projects.

Required columns:

- `sample_id`
- `source_file`
- `project` (`detector` or `subtype`)
- `label`
- `start_ms`
- `end_ms`
- `baby_id` (`unknown` allowed)
- `environment` (`home_quiet`, `tv_on`, `street`, etc.)
- `distance` (`near`, `mid`, `far`)
- `overlap_sounds` (comma-separated)
- `annotator`
- `label_confidence` (`0.0-1.0`)
- `split` (`train`, `val`, `test`)

Keep this file versioned in the repo for reproducibility.

## 6. Directory convention (suggested)

Suggested local structure for dataset curation:

```text
dataset/
  raw/
    detector/
      choro/
      nao_choro/
    subtype/
      colica/
      fome/
      sono/
  windows_1s/
    detector/
      train/
      val/
      test/
    subtype/
      train/
      val/
      test/
  metadata/
    samples.csv
    review_notes.md
```

## 7. Edge Impulse import checklist

For each project:

1. import only its own labels
2. verify sample rate = `16 kHz`
3. confirm split (train/test) respects file-level isolation
4. run impulse with same DSP family (MFCC/log-Mel)
5. train baseline without augmentation
6. train augmented version and compare
7. export confusion matrix and per-class metrics
8. freeze model only after acceptance criteria in this document

## 8. Validation packs beyond EI test split

Prepare two external packs not used in EI training:

- `pack_fp_hardneg`: TV/music/speech/fan/dog mixed scenes
- `pack_real_world`: long continuous recordings in target deployment rooms

Track:

- false alarms per minute (detector)
- missed cry events (detector)
- subtype stability across consecutive windows

## 9. Release gates

Do not integrate as default firmware model unless all gates pass:

- model A acceptance criteria met
- model B acceptance criteria met
- hard-negative external pack reviewed
- at least one real-device dry-run log reviewed end-to-end

## 10. First collection sprint (practical target)

If dataset is still small, start with this 1-week sprint:

- detector:
  - `120` minutes raw `nao_choro` across all negative buckets
  - `80` minutes raw `choro`
- subtype:
  - `30` minutes per class (`colica`, `fome`, `sono`)

After windowing and cleaning, this should produce enough data for a first robust v1 benchmark.

---

Owner: CrySense AI project
Status: Draft v1 ready for execution