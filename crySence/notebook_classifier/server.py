from __future__ import annotations

import os
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from classifier import CryTypeClassifier

BASE_DIR = Path(__file__).resolve().parent
MODEL_PATH = os.getenv("CRYSENSE_MODEL_PATH", str(BASE_DIR / "models" / "cry_type_model.joblib"))
LABELS_PATH = os.getenv("CRYSENSE_LABELS_PATH", str(BASE_DIR / "models" / "labels.json"))
INCLUDE_FEATURES = os.getenv("CRYSENSE_INCLUDE_FEATURES", "0") == "1"

RECEIVED_DIR = Path(os.getenv("CRYSENSE_RECEIVED_DIR", str(BASE_DIR / "received_audio")))
RECEIVED_KEEP = max(20, int(os.getenv("CRYSENSE_RECEIVED_KEEP", "300")))

classifier = CryTypeClassifier(
    model_path=MODEL_PATH if Path(MODEL_PATH).exists() else None,
    labels_path=LABELS_PATH if Path(LABELS_PATH).exists() else None,
)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _now_iso(ms: int | None = None) -> str:
    value = _now_ms() if ms is None else ms
    return datetime.fromtimestamp(value / 1000.0).isoformat(timespec="seconds")


def _parse_trigger_conf(raw: str | None) -> float | None:
    if raw is None:
        return None
    try:
        value = float(raw)
    except ValueError:
        return None
    if value < 0:
        return 0.0
    if value > 1:
        return 1.0
    return value


def _ensure_received_dir() -> None:
    RECEIVED_DIR.mkdir(parents=True, exist_ok=True)


def _prune_received_dir() -> None:
    files = sorted(
        RECEIVED_DIR.glob("*.wav"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    for old in files[RECEIVED_KEEP:]:
        try:
            old.unlink(missing_ok=True)
        except Exception:
            pass


def _new_audio_filename(request_id: str) -> str:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
    return f"{ts}_{request_id}.wav"


def _save_received_audio(request_id: str, wav_bytes: bytes) -> Path:
    _ensure_received_dir()
    name = _new_audio_filename(request_id)
    path = RECEIVED_DIR / name
    path.write_bytes(wav_bytes)
    _prune_received_dir()
    return path


@dataclass
class RuntimeStats:
    started_ms: int = field(default_factory=_now_ms)
    total: int = 0
    ok: int = 0
    errors: int = 0
    last_latency_ms: int = 0
    avg_latency_ms: float = 0.0
    by_label: dict[str, int] = field(default_factory=lambda: {"colic": 0, "hunger": 0})

    recent: deque[dict[str, Any]] = field(default_factory=lambda: deque(maxlen=50))
    events: deque[dict[str, Any]] = field(default_factory=lambda: deque(maxlen=120))
    received_files: deque[dict[str, Any]] = field(default_factory=lambda: deque(maxlen=200))

    request_seq: int = 0
    pipeline_phase: str = "idle"  # idle|receiving_audio|analyzing|classified|error
    pipeline_request_id: str = ""
    pipeline_file_name: str = ""
    pipeline_file_size_bytes: int = 0
    pipeline_trigger_conf: float | None = None
    pipeline_last_label: str = ""
    pipeline_last_conf: float = 0.0
    pipeline_last_error: str = ""
    pipeline_updated_ms: int = field(default_factory=_now_ms)

    lock: threading.Lock = field(default_factory=threading.Lock)

    def next_request_id(self) -> str:
        with self.lock:
            self.request_seq += 1
            return f"req-{_now_ms()}-{self.request_seq:06d}"

    def add_metric(self, ok: bool, latency_ms: int, label: str, conf: float) -> None:
        with self.lock:
            self.total += 1
            self.last_latency_ms = latency_ms
            self.avg_latency_ms += (latency_ms - self.avg_latency_ms) / float(self.total)
            if ok:
                self.ok += 1
                if label in self.by_label:
                    self.by_label[label] += 1
            else:
                self.errors += 1
            self.recent.append(
                {
                    "ts_ms": _now_ms(),
                    "ok": ok,
                    "latency_ms": latency_ms,
                    "label": label,
                    "confianca": conf,
                }
            )

    def set_pipeline(
        self,
        phase: str,
        *,
        request_id: str | None = None,
        file_name: str | None = None,
        file_size_bytes: int | None = None,
        trigger_conf: float | None = None,
        last_label: str | None = None,
        last_conf: float | None = None,
        last_error: str | None = None,
    ) -> None:
        with self.lock:
            self.pipeline_phase = phase
            if request_id is not None:
                self.pipeline_request_id = request_id
            if file_name is not None:
                self.pipeline_file_name = file_name
            if file_size_bytes is not None:
                self.pipeline_file_size_bytes = file_size_bytes
            if trigger_conf is not None or phase == "receiving_audio":
                self.pipeline_trigger_conf = trigger_conf
            if last_label is not None:
                self.pipeline_last_label = last_label
            if last_conf is not None:
                self.pipeline_last_conf = float(last_conf)
            if last_error is not None:
                self.pipeline_last_error = last_error
            self.pipeline_updated_ms = _now_ms()

    def log_event(
        self,
        *,
        stage: str,
        message: str,
        level: str = "info",
        request_id: str = "",
        extra: dict[str, Any] | None = None,
    ) -> None:
        ts = _now_ms()
        event: dict[str, Any] = {
            "ts_ms": ts,
            "ts": _now_iso(ts),
            "level": level,
            "stage": stage,
            "message": message,
            "request_id": request_id,
        }
        if extra:
            event.update(extra)
        with self.lock:
            self.events.append(event)
            self.pipeline_updated_ms = ts
        print(f"[EVENT] {event['ts']} [{stage}] {message}")

    def add_received_file(
        self,
        *,
        path: Path,
        request_id: str,
        size_bytes: int,
        trigger_conf: float | None,
    ) -> None:
        ts = _now_ms()
        with self.lock:
            self.received_files.appendleft(
                {
                    "name": path.name,
                    "path": str(path),
                    "size_bytes": int(size_bytes),
                    "ts_ms": ts,
                    "ts": _now_iso(ts),
                    "request_id": request_id,
                    "trigger_conf": trigger_conf,
                    "label": "",
                    "confianca": 0.0,
                    "url": f"/received/{path.name}",
                }
            )

    def mark_received_file_result(self, request_id: str, label: str, conf: float) -> None:
        with self.lock:
            for item in self.received_files:
                if item.get("request_id") == request_id:
                    item["label"] = label
                    item["confianca"] = float(conf)
                    break

    def status_snapshot(self) -> dict[str, Any]:
        with self.lock:
            return {
                "phase": self.pipeline_phase,
                "request_id": self.pipeline_request_id,
                "file_name": self.pipeline_file_name,
                "file_size_bytes": self.pipeline_file_size_bytes,
                "trigger_conf": self.pipeline_trigger_conf,
                "last_label": self.pipeline_last_label,
                "last_conf": self.pipeline_last_conf,
                "last_error": self.pipeline_last_error,
                "updated_ms": self.pipeline_updated_ms,
                "updated_at": _now_iso(self.pipeline_updated_ms),
            }

    def recent_events(self, limit: int = 40) -> list[dict[str, Any]]:
        with self.lock:
            items = list(self.events)[-max(1, limit) :]
        return items

    def recent_files(self, limit: int = 30) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        with self.lock:
            for item in self.received_files:
                if Path(item["path"]).exists():
                    out.append(dict(item))
                if len(out) >= max(1, limit):
                    break
        return out


stats = RuntimeStats()
app = FastAPI(title="CrySense Notebook Classifier", version="1.1.0")

_ensure_received_dir()
app.mount("/received", StaticFiles(directory=str(RECEIVED_DIR)), name="received")


def _bootstrap_received_cache() -> None:
    files = sorted(
        RECEIVED_DIR.glob("*.wav"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )[:80]
    with stats.lock:
        stats.received_files.clear()
        for p in files:
            st = p.stat()
            ts = int(st.st_mtime * 1000)
            stats.received_files.append(
                {
                    "name": p.name,
                    "path": str(p),
                    "size_bytes": int(st.st_size),
                    "ts_ms": ts,
                    "ts": _now_iso(ts),
                    "request_id": "bootstrap",
                    "trigger_conf": None,
                    "label": "",
                    "confianca": 0.0,
                    "url": f"/received/{p.name}",
                }
            )


_bootstrap_received_cache()


def _service_info() -> dict[str, Any]:
    return {
        "ok": True,
        "service": "crysense-notebook-classifier",
        "endpoints": [
            "/ui",
            "/health",
            "/status",
            "/metrics",
            "/events",
            "/received-files",
            "/classify",
            "/docs",
        ],
    }


def _dashboard_html() -> str:
    return """<!doctype html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CrySense Notebook Classifier</title>
  <style>
    :root {
      --bg: #0b1020;
      --panel: #121a33;
      --muted: #8ea0bf;
      --text: #e6ecff;
      --ok: #27c07d;
      --warn: #ffbd2e;
      --bad: #ff5d5d;
      --accent: #59a8ff;
      --border: #263255;
    }
    * { box-sizing: border-box; }
    body { margin: 0; background: #0b1020; color: var(--text); font-family: ui-sans-serif, system-ui, Arial, sans-serif; }
    .wrap { max-width: 1120px; margin: 0 auto; padding: 20px; }
    .top { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 16px; }
    .title { font-size: 20px; font-weight: 700; }
    .sub { color: var(--muted); font-size: 13px; }
    .badge { border: 1px solid var(--border); border-radius: 999px; padding: 6px 10px; font-size: 12px; background: #0e1630; }
    .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; margin-bottom: 12px; }
    .card { border: 1px solid var(--border); border-radius: 12px; background: var(--panel); padding: 12px; }
    .k { color: var(--muted); font-size: 12px; margin-bottom: 6px; }
    .v { font-size: 21px; font-weight: 700; line-height: 1.1; }
    .mono { font-family: ui-monospace, Menlo, Consolas, monospace; }
    .row2 { display: grid; grid-template-columns: 1.15fr 1fr; gap: 12px; margin-bottom: 12px; }
    .panel-title { margin: 0 0 10px 0; font-size: 14px; font-weight: 700; color: #d6e1ff; }
    .kv { display: grid; grid-template-columns: 170px 1fr; row-gap: 8px; column-gap: 8px; font-size: 13px; }
    .label { color: var(--muted); }
    .ok { color: var(--ok); }
    .warn { color: var(--warn); }
    .bad { color: var(--bad); }
    .form { display: grid; gap: 10px; font-size: 13px; }
    input[type="file"], input[type="number"] { width: 100%; background: #0f1730; color: var(--text); border: 1px solid var(--border); border-radius: 9px; padding: 8px 10px; }
    button { border: 1px solid #2f4f85; background: #173569; color: #e6ecff; padding: 9px 12px; border-radius: 9px; cursor: pointer; font-weight: 600; }
    button:hover { filter: brightness(1.07); }
    pre { margin: 0; max-height: 300px; overflow: auto; background: #0b142d; border: 1px solid var(--border); border-radius: 9px; padding: 10px; font-size: 12px; line-height: 1.35; white-space: pre-wrap; }
    .file-list { display: grid; gap: 6px; max-height: 260px; overflow: auto; }
    .file-item { border: 1px solid var(--border); border-radius: 8px; padding: 8px; background: #101a35; font-size: 12px; }
    .file-item a { color: #9ec8ff; text-decoration: none; }
    .endpoints a { color: #9ec8ff; text-decoration: none; margin-right: 10px; font-size: 13px; }
    @media (max-width: 980px) {
      .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .row2 { grid-template-columns: 1fr; }
      .kv { grid-template-columns: 140px 1fr; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="top">
      <div>
        <div class="title">CrySense Notebook Classifier</div>
        <div class="sub">Pipeline visivel: recebendo audio -> analisando -> classificado, com log e armazenamento WAV.</div>
      </div>
      <div class="badge" id="status-badge">Conectando...</div>
    </div>

    <div class="grid">
      <div class="card"><div class="k">Modelo carregado</div><div class="v" id="model-loaded">-</div></div>
      <div class="card"><div class="k">Requests totais</div><div class="v" id="req-total">0</div></div>
      <div class="card"><div class="k">Latencia media</div><div class="v" id="lat-avg">0 ms</div></div>
      <div class="card"><div class="k">Uptime</div><div class="v" id="uptime">0 s</div></div>
    </div>

    <div class="row2">
      <div class="card">
        <h3 class="panel-title">Pipeline remoto (ESP -> Notebook)</h3>
        <div class="kv">
          <div class="label">Fase</div><div id="p-phase">idle</div>
          <div class="label">Request ID</div><div id="p-req" class="mono">-</div>
          <div class="label">Arquivo</div><div id="p-file" class="mono">-</div>
          <div class="label">Tamanho</div><div id="p-size">-</div>
          <div class="label">Trigger conf</div><div id="p-trigger">-</div>
          <div class="label">Resultado</div><div id="p-result">-</div>
          <div class="label">Erro</div><div id="p-error">-</div>
          <div class="label">Atualizado</div><div id="p-updated">-</div>
        </div>
        <div class="endpoints" style="margin-top: 10px;">
          <a href="/health" target="_blank" rel="noopener noreferrer">/health</a>
          <a href="/status" target="_blank" rel="noopener noreferrer">/status</a>
          <a href="/metrics" target="_blank" rel="noopener noreferrer">/metrics</a>
          <a href="/events" target="_blank" rel="noopener noreferrer">/events</a>
          <a href="/received-files" target="_blank" rel="noopener noreferrer">/received-files</a>
          <a href="/docs" target="_blank" rel="noopener noreferrer">/docs</a>
        </div>
      </div>

      <div class="card">
        <h3 class="panel-title">Testar classificacao WAV</h3>
        <form id="classify-form" class="form">
          <div>
            <div class="label" style="margin-bottom: 4px;">Arquivo WAV (6s recomendado)</div>
            <input id="wav-file" type="file" accept=".wav,audio/wav" required />
          </div>
          <div>
            <div class="label" style="margin-bottom: 4px;">X-Trigger-Confidence (0.0 - 1.0)</div>
            <input id="trigger-conf" type="number" min="0" max="1" step="0.01" value="0.85" />
          </div>
          <button type="submit">Classificar</button>
        </form>
        <div id="classify-msg" style="margin-top: 10px; color: var(--muted);">Aguardando envio.</div>
        <pre id="classify-result">{}</pre>
      </div>
    </div>

    <div class="row2">
      <div class="card">
        <h3 class="panel-title">Log dos ultimos eventos</h3>
        <pre id="event-log">[]</pre>
      </div>
      <div class="card">
        <h3 class="panel-title">Ultimos arquivos recebidos</h3>
        <div id="file-list" class="file-list"></div>
      </div>
    </div>
  </div>

  <script>
    const el = {
      badge: document.getElementById("status-badge"),
      modelLoaded: document.getElementById("model-loaded"),
      reqTotal: document.getElementById("req-total"),
      latAvg: document.getElementById("lat-avg"),
      uptime: document.getElementById("uptime"),
      pPhase: document.getElementById("p-phase"),
      pReq: document.getElementById("p-req"),
      pFile: document.getElementById("p-file"),
      pSize: document.getElementById("p-size"),
      pTrigger: document.getElementById("p-trigger"),
      pResult: document.getElementById("p-result"),
      pError: document.getElementById("p-error"),
      pUpdated: document.getElementById("p-updated"),
      eventLog: document.getElementById("event-log"),
      fileList: document.getElementById("file-list"),
      form: document.getElementById("classify-form"),
      wav: document.getElementById("wav-file"),
      trig: document.getElementById("trigger-conf"),
      msg: document.getElementById("classify-msg"),
      out: document.getElementById("classify-result"),
    };

    function toMs(v) { return `${Math.round(Number(v || 0))} ms`; }
    function toSize(v) {
      const n = Number(v || 0);
      if (n < 1024) return `${n} B`;
      if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
      return `${(n / (1024 * 1024)).toFixed(2)} MB`;
    }
    function toUptime(ms) {
      const s = Math.floor(Number(ms || 0) / 1000);
      if (s < 60) return `${s} s`;
      const m = Math.floor(s / 60);
      const rs = s % 60;
      if (m < 60) return `${m} min ${rs}s`;
      const h = Math.floor(m / 60);
      const rm = m % 60;
      return `${h}h ${rm}m`;
    }
    function phaseLabel(p) {
      const map = {
        idle: "idle",
        receiving_audio: "recebendo audio",
        analyzing: "analisando",
        classified: "classificado",
        error: "erro",
      };
      return map[p] || p || "-";
    }
    function badge(ok, modelLoaded) {
      if (!ok) return `<span class="bad">offline</span>`;
      if (!modelLoaded) return `<span class="warn">online (sem modelo)</span>`;
      return `<span class="ok">online</span>`;
    }
    function renderEvents(events) {
      if (!events || !events.length) return "Sem eventos ainda.";
      return events.slice(-40).map((e) => {
        const rid = e.request_id ? ` | ${e.request_id}` : "";
        return `[${e.ts}] [${e.stage}] ${e.message}${rid}`;
      }).join("\\n");
    }
    function renderFiles(files) {
      if (!files || !files.length) return '<div class="file-item">Nenhum arquivo recebido.</div>';
      return files.map((f) => {
        const lbl = f.label ? `${f.label} ${(Number(f.confianca || 0) * 100).toFixed(0)}%` : "-";
        return `
          <div class="file-item">
            <div><a href="${f.url}" target="_blank" rel="noopener noreferrer">${f.name}</a></div>
            <div class="mono">${toSize(f.size_bytes)} | ${f.ts}</div>
            <div>resultado: <b>${lbl}</b></div>
            <div class="mono">${f.request_id || "-"}</div>
          </div>
        `;
      }).join("");
    }

    async function refresh() {
      try {
        const [hRes, mRes, sRes, eRes, fRes] = await Promise.all([
          fetch("/health"),
          fetch("/metrics"),
          fetch("/status"),
          fetch("/events?limit=60"),
          fetch("/received-files?limit=40"),
        ]);
        const health = await hRes.json();
        const metrics = await mRes.json();
        const status = await sRes.json();
        const events = await eRes.json();
        const files = await fRes.json();

        el.badge.innerHTML = badge(Boolean(health.ok), Boolean(health.model_loaded));
        el.modelLoaded.textContent = health.model_loaded ? "SIM" : "NAO";
        el.modelLoaded.className = `v ${health.model_loaded ? "ok" : "warn"}`;
        el.reqTotal.textContent = String(metrics.requests_total || 0);
        el.latAvg.textContent = toMs(metrics.avg_latency_ms || 0);
        el.uptime.textContent = toUptime(health.uptime_ms || 0);

        const p = status.pipeline || {};
        el.pPhase.textContent = phaseLabel(p.phase);
        el.pReq.textContent = p.request_id || "-";
        el.pFile.textContent = p.file_name || "-";
        el.pSize.textContent = p.file_size_bytes ? toSize(p.file_size_bytes) : "-";
        el.pTrigger.textContent = (p.trigger_conf === null || p.trigger_conf === undefined) ? "-" : Number(p.trigger_conf).toFixed(3);
        el.pResult.textContent = p.last_label ? `${p.last_label} ${(Number(p.last_conf || 0) * 100).toFixed(0)}%` : "-";
        el.pError.textContent = p.last_error || "-";
        el.pUpdated.textContent = p.updated_at || "-";

        el.eventLog.textContent = renderEvents(events.events || []);
        el.fileList.innerHTML = renderFiles(files.files || []);
      } catch (err) {
        el.badge.innerHTML = '<span class="bad">offline</span>';
        el.msg.textContent = `Falha de conexao: ${String(err)}`;
      }
    }

    el.form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      const file = el.wav.files && el.wav.files[0];
      if (!file) {
        el.msg.textContent = "Selecione um arquivo .wav.";
        return;
      }
      const triggerRaw = Number(el.trig.value);
      const triggerConf = Number.isFinite(triggerRaw) ? Math.max(0, Math.min(1, triggerRaw)) : 0.85;
      el.msg.textContent = "Enviando e classificando...";
      try {
        const buffer = await file.arrayBuffer();
        const resp = await fetch("/classify", {
          method: "POST",
          headers: {
            "Content-Type": "audio/wav",
            "X-Trigger-Confidence": String(triggerConf),
          },
          body: buffer,
        });
        const data = await resp.json();
        el.out.textContent = JSON.stringify(data, null, 2);
        if (resp.ok) {
          el.msg.innerHTML = `<span class="ok">Classificacao OK (${data.label}, ${(Number(data.confianca || 0) * 100).toFixed(0)}%)</span>`;
        } else {
          el.msg.innerHTML = `<span class="bad">Erro: ${data.error || "request_failed"}</span>`;
        }
        await refresh();
      } catch (err) {
        el.msg.innerHTML = `<span class="bad">Falha ao enviar audio: ${String(err)}</span>`;
      }
    });

    refresh();
    setInterval(refresh, 1500);
  </script>
</body>
</html>"""


@app.get("/")
def root() -> HTMLResponse:
    return HTMLResponse(content=_dashboard_html())


@app.get("/ui")
def ui() -> HTMLResponse:
    return HTMLResponse(content=_dashboard_html())


@app.get("/api")
def api() -> dict[str, Any]:
    return _service_info()


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "ok": True,
        "model_loaded": classifier.model is not None,
        "model_path": MODEL_PATH if classifier.model is not None else None,
        "uptime_ms": _now_ms() - stats.started_ms,
    }


@app.get("/status")
def status() -> dict[str, Any]:
    return {
        "ok": True,
        "uptime_ms": _now_ms() - stats.started_ms,
        "pipeline": stats.status_snapshot(),
    }


@app.get("/events")
def events(limit: int = 50) -> dict[str, Any]:
    limit = max(1, min(200, int(limit)))
    return {"ok": True, "events": stats.recent_events(limit)}


@app.get("/received-files")
def received_files(limit: int = 40) -> dict[str, Any]:
    limit = max(1, min(200, int(limit)))
    return {
        "ok": True,
        "dir": str(RECEIVED_DIR),
        "files": stats.recent_files(limit),
    }


@app.get("/metrics")
def metrics() -> dict[str, Any]:
    with stats.lock:
        return {
            "ok": True,
            "uptime_ms": _now_ms() - stats.started_ms,
            "requests_total": stats.total,
            "requests_ok": stats.ok,
            "requests_error": stats.errors,
            "last_latency_ms": stats.last_latency_ms,
            "avg_latency_ms": round(stats.avg_latency_ms, 2),
            "by_label": dict(stats.by_label),
            "recent": list(stats.recent),
            "pipeline_phase": stats.pipeline_phase,
        }


@app.post("/classify")
async def classify(request: Request) -> Any:
    content_type = request.headers.get("content-type", "")
    if content_type and ("audio/wav" not in content_type and "application/octet-stream" not in content_type):
        return JSONResponse(
            status_code=415,
            content={"ok": False, "error": "unsupported_content_type", "expected": "audio/wav"},
        )

    wav_bytes = await request.body()
    if not wav_bytes:
        return JSONResponse(status_code=400, content={"ok": False, "error": "empty_body"})

    trigger_conf = _parse_trigger_conf(request.headers.get("X-Trigger-Confidence"))
    request_id = stats.next_request_id()
    size_bytes = len(wav_bytes)
    t0 = time.perf_counter()

    stats.set_pipeline(
        "receiving_audio",
        request_id=request_id,
        file_name="",
        file_size_bytes=size_bytes,
        trigger_conf=trigger_conf,
        last_error="",
    )
    stats.log_event(
        stage="receiving_audio",
        message=f"Audio recebido ({size_bytes} bytes)",
        request_id=request_id,
        extra={"size_bytes": size_bytes, "trigger_conf": trigger_conf},
    )

    try:
        saved_path = _save_received_audio(request_id, wav_bytes)
        stats.add_received_file(
            path=saved_path,
            request_id=request_id,
            size_bytes=size_bytes,
            trigger_conf=trigger_conf,
        )
    except Exception as exc:
        latency_ms = int((time.perf_counter() - t0) * 1000)
        msg = f"save_failed: {exc}"
        stats.add_metric(ok=False, latency_ms=latency_ms, label="error", conf=0.0)
        stats.set_pipeline(
            "error",
            request_id=request_id,
            file_name="",
            file_size_bytes=size_bytes,
            trigger_conf=trigger_conf,
            last_error=msg,
        )
        stats.log_event(stage="error", level="error", message=msg, request_id=request_id)
        return JSONResponse(status_code=500, content={"ok": False, "error": "save_failed"})

    stats.set_pipeline(
        "analyzing",
        request_id=request_id,
        file_name=saved_path.name,
        file_size_bytes=size_bytes,
        trigger_conf=trigger_conf,
        last_error="",
    )
    stats.log_event(
        stage="analyzing",
        message=f"Classificando {saved_path.name}",
        request_id=request_id,
    )

    try:
        result = classifier.predict_wav_bytes(wav_bytes=wav_bytes, trigger_conf=trigger_conf)
    except ValueError as exc:
        latency_ms = int((time.perf_counter() - t0) * 1000)
        msg = str(exc)
        stats.add_metric(ok=False, latency_ms=latency_ms, label="error", conf=0.0)
        stats.set_pipeline(
            "error",
            request_id=request_id,
            file_name=saved_path.name,
            file_size_bytes=size_bytes,
            trigger_conf=trigger_conf,
            last_error=msg,
        )
        stats.log_event(stage="error", level="error", message=f"classify_error: {msg}", request_id=request_id)
        return JSONResponse(status_code=400, content={"ok": False, "error": msg, "request_id": request_id})
    except Exception as exc:  # pragma: no cover
        latency_ms = int((time.perf_counter() - t0) * 1000)
        msg = f"internal_error: {exc}"
        stats.add_metric(ok=False, latency_ms=latency_ms, label="error", conf=0.0)
        stats.set_pipeline(
            "error",
            request_id=request_id,
            file_name=saved_path.name,
            file_size_bytes=size_bytes,
            trigger_conf=trigger_conf,
            last_error=msg,
        )
        stats.log_event(stage="error", level="error", message=msg, request_id=request_id)
        return JSONResponse(status_code=500, content={"ok": False, "error": msg, "request_id": request_id})

    latency_ms = int((time.perf_counter() - t0) * 1000)
    label = result["label"]
    conf = float(result["confianca"])
    stats.add_metric(ok=True, latency_ms=latency_ms, label=label, conf=conf)
    stats.mark_received_file_result(request_id, label, conf)
    stats.set_pipeline(
        "classified",
        request_id=request_id,
        file_name=saved_path.name,
        file_size_bytes=size_bytes,
        trigger_conf=trigger_conf,
        last_label=label,
        last_conf=conf,
        last_error="",
    )
    stats.log_event(
        stage="classified",
        message=f"Classificado como {label} ({int(conf * 100)}%), {latency_ms}ms",
        request_id=request_id,
        extra={"label": label, "confianca": conf, "latency_ms": latency_ms, "file_name": saved_path.name},
    )

    payload: dict[str, Any] = {
        "ok": True,
        "label": label,
        "confianca": conf,
        "scores": result["scores"],
        "latency_ms": latency_ms,
        "request_id": request_id,
        "saved_file": saved_path.name,
    }
    if INCLUDE_FEATURES:
        payload["features"] = result["features"]
    return payload


if __name__ == "__main__":
    import uvicorn

    host = os.getenv("CRYSENSE_HOST", "0.0.0.0")
    port = int(os.getenv("CRYSENSE_PORT", "8000"))
    uvicorn.run("server:app", host=host, port=port, reload=False)

