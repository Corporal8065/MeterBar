#!/usr/bin/env python3
"""MeterBar Live Captions helper (v0.23.0).

Design: ~/workspace/bar-apps/captions-spec/SPEC.md. MeterBar is the
only audio capturer; this helper receives 100 ms mono PCM frames
from MeterBar over a loopback TCP seam (MBC1 framing, identical to
dsp/include/meterbar/Captions.hpp), segments speech, and runs
faster-whisper in a CHILD worker process so a game can force a
VRAM release by terminating the worker.

Privacy rules (spec section 5): audio/text never leave this PC;
logs record timings, states and counters ONLY — never caption text.
This module must never print or log transcript text.

Usage (launched by MeterBar, not by hand):
    meterbar_captions_helper.py --session <id> --rundir <dir>
        [--mode transcribe|translate] [--models-dir <dir>]
        [--log-dir <dir>] [--agent-urls <csv>]
        [--allow-model-names]   dev only: bare model names vs local dirs
        [--fake-asr]            dev/test only: stub ASR, no model
    meterbar_captions_helper.py --worker   (internal child mode)
"""
import argparse
import ctypes
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

import numpy as np

PROTOCOL_VERSION = 1
MAGIC = b"MBC1"
PKT_HELLO, PKT_CONFIG, PKT_AUDIO, PKT_CONTROL, PKT_HEARTBEAT = 1, 2, 3, 4, 5
PKT_READY, PKT_STATUS, PKT_CAPTION, PKT_ERROR = 6, 7, 8, 9
AUDIO_PREFIX = struct.Struct(">QIH")  # first_native_sample, sample_rate, frame_samples
SR16 = 16000
BUFFER_SECONDS = 45
VAD_TICK_SAMPLES = SR16 // 2          # one scheduling tick per 500 ms of stream
PROVISIONAL_STEP = SR16               # 1.0 s of new speech per provisional pass
CONTEXT_SAMPLES = 4 * SR16            # up to 4 s of preceding context per job
SAFETY_TAIL = int(0.8 * SR16)         # 800 ms commit safety tail
MAX_UTTERANCE = 12 * SR16

# ------------------------------------------------------------------
# Logging. NEVER log caption text — states, timings, counts only.
# ------------------------------------------------------------------
import logging
from logging.handlers import RotatingFileHandler

log = logging.getLogger("mbcaptions")


def setup_logging(log_dir):
    os.makedirs(log_dir, exist_ok=True)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
    fh = RotatingFileHandler(os.path.join(log_dir, "helper.log"),
                             maxBytes=1024 * 1024, backupCount=3, encoding="utf-8")
    fh.setFormatter(fmt)
    log.addHandler(fh)
    log.setLevel(logging.INFO)
    if os.environ.get("MBCAPTIONS_STDERR"):
        sh = logging.StreamHandler(sys.stderr)
        sh.setFormatter(fmt)
        log.addHandler(sh)


# ------------------------------------------------------------------
# MBC1 framing — mirrors meterbar::Captions.hpp exactly.
# ------------------------------------------------------------------
def pack_packet(ptype, epoch, seq, payload):
    hdr = (MAGIC + bytes([PROTOCOL_VERSION, ptype]) + struct.pack(">H", 0)
           + struct.pack(">I", len(payload)) + struct.pack(">I", epoch)
           + struct.pack(">Q", seq))
    assert len(hdr) == 24
    return hdr + payload


class PacketReader:
    def __init__(self):
        self.buf = bytearray()
        self.error = False

    def feed(self, data):
        self.buf += data

    def next(self):
        if self.error or len(self.buf) < 24:
            return None
        if bytes(self.buf[0:4]) != MAGIC or self.buf[4] != PROTOCOL_VERSION:
            self.error = True
            return None
        length = struct.unpack(">I", self.buf[8:12])[0]
        if length > 4 * 1024 * 1024:
            self.error = True
            return None
        if len(self.buf) < 24 + length:
            return None
        ptype = self.buf[5]
        epoch = struct.unpack(">I", self.buf[12:16])[0]
        seq = struct.unpack(">Q", self.buf[16:24])[0]
        payload = bytes(self.buf[24:24 + length])
        del self.buf[:24 + length]
        return ptype, epoch, seq, payload


def json_payload(obj):
    return json.dumps(obj, ensure_ascii=False).encode("utf-8")


# ------------------------------------------------------------------
# CUDA discipline — adapted from the Room Voice Link v2.2 stack
# (~/workspace/your_files/room-voice-link-v2.2/room_voice_link.py):
# PATH prepend + add_dll_directory + explicit cuBLAS preload, and a
# CUDA runtime failure is recoverable by rebuilding on CPU.
# ------------------------------------------------------------------
_DLL_DIR_HANDLES = []


def _cuda_dll_dirs():
    dirs = []
    try:
        import site
        roots = site.getsitepackages()
    except Exception:
        roots = []
    roots.append(os.path.join(os.path.dirname(sys.executable), "Lib", "site-packages"))
    for root in roots:
        for pkg in ("cublas", "cudnn"):
            d = os.path.join(root, "nvidia", pkg, "bin")
            if os.path.isdir(d):
                dirs.append(d)
    return dirs


def register_cuda_dll_dirs():
    if os.name != "nt" and not hasattr(os, "add_dll_directory"):
        return
    dirs = _cuda_dll_dirs()
    if not dirs:
        return
    # 1) PATH prepend (some native loads search PATH, not DLL dirs)
    os.environ["PATH"] = os.pathsep.join(dirs) + os.pathsep + os.environ.get("PATH", "")
    # 2) add_dll_directory
    if hasattr(os, "add_dll_directory"):
        for d in dirs:
            try:
                _DLL_DIR_HANDLES.append(os.add_dll_directory(d))
            except OSError:
                pass
    # 3) explicit cuBLAS preload so the DLLs are resident
    cublas_dirs = [d for d in dirs if os.path.basename(os.path.dirname(d)) == "cublas"]
    for d in cublas_dirs or dirs:
        for name in ("cublas64_12.dll", "cublasLt64_12.dll"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                try:
                    ctypes.WinDLL(p)
                except Exception:
                    pass


def _is_cuda_runtime_error(exc):
    msg = str(exc).lower()
    return any(k in msg for k in ("cublas", "cudnn", "cuda", ".dll"))


def cuda_device_count():
    try:
        import ctranslate2
        return ctranslate2.get_cuda_device_count()
    except Exception:
        return 0


def nvidia_smi_gpu():
    """(free_mib, util_pct) or None when nvidia-smi is unavailable."""
    exe = shutil.which("nvidia-smi")
    if not exe:
        return None
    try:
        out = subprocess.run(
            [exe, "--query-gpu=memory.free,utilization.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5)
        parts = out.stdout.strip().splitlines()[0].split(",")
        return int(parts[0].strip()), int(parts[1].strip())
    except Exception:
        return None


# ------------------------------------------------------------------
# ASR worker child process. Owns exactly one faster-whisper model.
# Stdio protocol: 4-byte BE length, then 2-byte BE header length,
# header JSON, then raw float32 LE audio (transcribe jobs only).
# stdout is reserved for packets; logs go to stderr.
# ------------------------------------------------------------------
def _w_send(conn, obj, audio=None):
    hdr = json.dumps(obj).encode("utf-8")
    body = struct.pack(">H", len(hdr)) + hdr + (audio.tobytes() if audio is not None else b"")
    conn.write(struct.pack(">I", len(body)) + body)
    conn.flush()


def _w_recv(conn):
    raw = conn.read(4)
    if not raw or len(raw) < 4:
        return None, None
    (n,) = struct.unpack(">I", raw)
    body = b""
    while len(body) < n:
        chunk = conn.read(n - len(body))
        if not chunk:
            return None, None
        body += chunk
    (hlen,) = struct.unpack(">H", body[:2])
    obj = json.loads(body[2:2 + hlen].decode("utf-8"))
    return obj, body[2 + hlen:]


def worker_main():
    register_cuda_dll_dirs()
    from faster_whisper import WhisperModel  # after DLL registration

    model = None
    inp, outp = sys.stdin.buffer, sys.stdout.buffer
    while True:
        cmd, audio = _w_recv(inp)
        if cmd is None:
            return
        op = cmd.get("cmd")
        if op == "quit":
            return
        if op == "load":
            try:
                model = WhisperModel(cmd["model"], device=cmd["device"],
                                     compute_type=cmd["compute_type"])
                _w_send(outp, {"ok": True})
            except Exception as exc:  # noqa: BLE001 — reported, not raised
                _w_send(outp, {"ok": False, "error": str(exc),
                               "cuda_runtime": _is_cuda_runtime_error(exc)})
            continue
        if op == "transcribe":
            if model is None:
                _w_send(outp, {"ok": False, "error": "no model loaded",
                               "cuda_runtime": False})
                continue
            try:
                samples = np.frombuffer(audio, dtype=np.float32).copy()
                segments, info = model.transcribe(
                    samples,
                    language=cmd.get("language") or None,
                    task=cmd.get("task", "transcribe"),
                    beam_size=1,
                    temperature=0.0,
                    condition_on_previous_text=False,
                    word_timestamps=True,  # supervisor reconciles by word times
                    vad_filter=False,
                    compression_ratio_threshold=2.4,
                    log_prob_threshold=-1.0,
                    no_speech_threshold=0.6,
                )
                segs = [{"start": s.start, "end": s.end, "text": s.text.strip(),
                         "no_speech_prob": s.no_speech_prob,
                         "avg_logprob": s.avg_logprob,
                         "words": [{"word": w.word, "start": w.start,
                                    "end": w.end} for w in (s.words or [])]}
                        for s in segments]
                _w_send(outp, {"ok": True, "segments": segs,
                               "language": info.language,
                               "language_probability": info.language_probability})
            except Exception as exc:  # noqa: BLE001
                _w_send(outp, {"ok": False, "error": str(exc),
                               "cuda_runtime": _is_cuda_runtime_error(exc)})
            continue
        _w_send(outp, {"ok": False, "error": f"unknown cmd {op}",
                       "cuda_runtime": False})


class WorkerClient:
    """Supervisor-side handle on the worker child (or the stub)."""

    def __init__(self, helper_path, log_dir, fake_asr=False):
        self.proc = None
        self.fake_asr = fake_asr
        self.helper_path = helper_path
        self.log_dir = log_dir
        self.model = None
        self.device = None
        self.compute_type = None
        self._stub_counter = 0

    def start(self, model, device, compute_type):
        self.stop()
        self.model, self.device, self.compute_type = model, device, compute_type
        if self.fake_asr:
            log.warning("FAKE ASR stub worker in use (dev/test only)")
            return True, None
        errlog = open(os.path.join(self.log_dir, "worker-stderr.log"), "ab")
        self.proc = subprocess.Popen(
            [sys.executable, self.helper_path, "--worker"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errlog,
            env={**os.environ, "PYTHONUNBUFFERED": "1"})
        ok, err = self._call({"cmd": "load", "model": model, "device": device,
                              "compute_type": compute_type})
        if not ok:
            self.stop()
            return False, err
        return True, None

    def alive(self):
        return self.fake_asr or (self.proc is not None and self.proc.poll() is None)

    def stop(self):
        if self.proc is not None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=3)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
            self.proc = None

    def _call(self, obj, audio=None):
        _w_send(self.proc.stdin, obj, audio)
        resp, _ = _w_recv(self.proc.stdout)
        if resp is None:
            return False, {"error": "worker closed the pipe", "cuda_runtime": False}
        if resp.get("ok"):
            return True, resp
        return False, resp

    def transcribe(self, audio_f32, task, language):
        """Returns (ok, result_or_error). Result: segments/language/..."""
        if self.fake_asr:
            self._stub_counter += 1
            dur = len(audio_f32) / SR16
            return True, {
                "segments": [{"start": 0.0, "end": max(0.1, dur - 0.05),
                              "text": f"stub caption segment {self._stub_counter}",
                              "no_speech_prob": 0.0, "avg_logprob": -0.1}],
                "language": "en", "language_probability": 0.99}
        return self._call({"cmd": "transcribe", "task": task,
                           "language": language}, audio_f32.astype(np.float32))


# ------------------------------------------------------------------
# PC agent presence (spec 3.4). The agent binds the PC's tailnet
# IPv4 when one exists, else 127.0.0.1 — so the candidate list is:
# explicit override, loopback, this machine's CGNAT (100.64/10)
# interface addresses, and the tailscale CLI's answer.
# ------------------------------------------------------------------
def agent_candidate_urls(extra):
    urls = []
    if extra:
        urls.extend(u.strip() for u in extra.split(",") if u.strip())
    env = os.environ.get("METERBAR_PC_AGENT_URL")
    if env:
        urls.insert(0, env)
    urls.append("http://127.0.0.1:8912")
    try:
        import socket as _s
        for info in _s.getaddrinfo(_s.gethostname(), None, _s.AF_INET):
            ip = info[4][0]
            parts = ip.split(".")
            if len(parts) == 4 and parts[0] == "100" and 64 <= int(parts[1]) <= 127:
                urls.append(f"http://{ip}:8912")
    except Exception:
        pass
    exe = shutil.which("tailscale")
    if exe:
        try:
            out = subprocess.run([exe, "ip", "-4"], capture_output=True,
                                 text=True, timeout=4)
            for line in out.stdout.splitlines():
                ip = line.strip()
                if ip:
                    urls.append(f"http://{ip}:8912")
                    break
        except Exception:
            pass
    seen, out = set(), []
    for u in urls:
        if u not in seen:
            seen.add(u)
            out.append(u.rstrip("/"))
    return out


class AgentPoller(threading.Thread):
    """Polls the PC agent /status every 2 s; shares one snapshot."""

    def __init__(self, urls):
        super().__init__(daemon=True, name="agent-poll")
        self.urls = urls
        self.good_url = None
        self.snap = {"game": False, "game_name": None, "agent_ok": False,
                     "free_mib": None, "util": None, "low_vram_streak": 0}
        self.lock = threading.Lock()
        self._last_smi = 0.0

    def _fetch(self, url):
        with urllib.request.urlopen(url + "/status", timeout=0.7) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def run(self):
        while True:
            snap = None
            candidates = ([self.good_url] if self.good_url else []) + \
                         [u for u in self.urls if u != self.good_url]
            for url in candidates:
                try:
                    data = self._fetch(url)
                    if time.time() - float(data.get("ts", 0)) > 30:
                        continue  # stale snapshot — not evidence either way
                    self.good_url = url
                    game = data.get("game", {})
                    gpu = data.get("gpu", {})
                    free = None
                    if gpu.get("vram_total_mb") and gpu.get("vram_used_mb") is not None:
                        free = int(gpu["vram_total_mb"]) - int(gpu["vram_used_mb"])
                    snap = {"game": bool(game.get("detected")),
                            "game_name": game.get("name"),
                            "agent_ok": True, "free_mib": free,
                            "util": gpu.get("util_pct")}
                    break
                except Exception:
                    continue
            if snap is None:
                # Agent unreachable/stale: fall back to nvidia-smi
                # headroom every ~10 s (spec 3.4).
                snap = {"game": False, "game_name": None, "agent_ok": False,
                        "free_mib": None, "util": None}
                if time.monotonic() - self._last_smi >= 10:
                    self._last_smi = time.monotonic()
                    got = nvidia_smi_gpu()
                    if got:
                        snap["free_mib"], snap["util"] = got
                    with self.lock:
                        self.snap.update({k: snap[k] for k in ("free_mib", "util")})
                        self.snap["agent_ok"] = False
                        self.snap["game"] = False
                else:
                    with self.lock:
                        self.snap["agent_ok"] = False
                        self.snap["game"] = False
                    time.sleep(2)
                    continue
            with self.lock:
                prev_low = self.snap.get("low_vram_streak", 0)
                self.snap.update(snap)
                free = snap["free_mib"]
                self.snap["low_vram_streak"] = \
                    prev_low + 1 if (free is not None and free < 1229) else 0
            time.sleep(2)

    def snapshot(self):
        with self.lock:
            return dict(self.snap)


# ------------------------------------------------------------------
# Supervisor: owns the socket, the 16 kHz rolling buffer, VAD
# scheduling, caption reconciliation, and the game/GPU policy.
# ------------------------------------------------------------------
class Supervisor:
    def __init__(self, args):
        self.args = args
        self.session = args.session
        self.mode = args.mode               # pending until CONFIG lands
        self.source_class = "system"
        self.worker = WorkerClient(os.path.abspath(__file__), args.log_dir,
                                   fake_asr=args.fake_asr)
        self.state = "stopped"
        self.send_lock = threading.Lock()
        self.job_lock = threading.Lock()     # serialises ASR jobs (scheduler + recv thread)
        self.stream_lock = threading.RLock()  # guards buf / epoch state
        self.conn = None
        self.out_seq = 0
        self.shutdown = False
        self.want_worker = False
        self.worker_dead_since = None
        self.worker_crashes = []
        self.backend = None                 # (model_ref, size, device, compute)
        self.backend_override = None
        self.last_backend_change = 0.0
        self.job_ratio_streak = 0
        # stream state (16 kHz domain)
        self.epoch = 0
        self.epoch_base_abs = 0
        self.abs_end = 0                    # absolute 16k index of next sample
        self.buf = np.zeros(0, dtype=np.float32)
        self.buf_start_abs = 0
        self.resampler = None
        self.native_rate = 0
        self.native_expected = None
        self.dropped_frames = 0
        self.next_speech_from = 0
        self.utt = None
        self.emitted = {}
        self.paused = False
        self.pending_mode = None
        self.last_committed_end_abs = 0
        self.last_vad_tick_abs = 0
        self.last_outbound = 0.0
        self.last_status_sent = 0.0
        # policy / holds
        self.poller = AgentPoller(agent_candidate_urls(args.agent_urls))
        self.hold_game = False
        self.hold_since = 0.0
        self.clear_since = None
        self.last_language = None

    # ---------------- socket plumbing ----------------
    def send(self, ptype, payload_obj=None, raw=None):
        if self.conn is None:
            return
        payload = raw if raw is not None else json_payload(payload_obj or {})
        with self.send_lock:
            self.out_seq += 1
            try:
                self.conn.sendall(pack_packet(ptype, self.epoch, self.out_seq, payload))
                self.last_outbound = time.monotonic()
            except OSError:
                pass

    def set_state(self, state):
        if state != self.state:
            self.state = state
            log.info("state -> %s", state)
            self.send_status()

    def send_status(self):
        self.last_status_sent = time.monotonic()
        model = self.backend[1] if self.backend else None
        device = self.backend[2] if self.backend else None
        compute = self.backend[3] if self.backend else None
        lag = 0
        if self.utt is not None:
            lag = int((self.abs_end - self.last_committed_end_abs) * 1000 / SR16)
        self.send(PKT_STATUS, {
            "state": self.state, "mode": self.mode, "model": model,
            "device": device, "compute_type": compute,
            "source_class": self.source_class,
            "language": self.last_language,
            "asr_lag_ms": max(0, lag), "dropped_frames": self.dropped_frames})

    def send_ready(self):
        self.send(PKT_READY, {
            "state": "ready", "protocol": PROTOCOL_VERSION,
            "model": self.backend[1], "device": self.backend[2],
            "compute_type": self.backend[3]})

    # ---------------- backend / worker lifecycle ----------------
    def model_ref(self, size):
        if self.args.allow_model_names:
            return size
        return os.path.join(self.args.models_dir, size)

    def model_available(self, size):
        if self.args.allow_model_names or self.args.fake_asr:
            return True
        return os.path.isfile(os.path.join(self.model_ref(size), "model.bin"))

    def choose_backend(self):
        if self.backend_override:
            return self.backend_override
        if cuda_device_count() == 0:
            return ("base", "cpu", "int8")
        snap = self.poller.snapshot()
        free = snap["free_mib"]
        if free is None:
            got = nvidia_smi_gpu()
            free = got[0] if got else None
        if free is None or free >= 3072:
            return ("small", "cuda", "float16")
        if free >= 1843:
            return ("base", "cuda", "int8_float16")
        return ("base", "cpu", "int8")

    def ensure_worker(self):
        """Start the worker if it should be running. Returns alive?"""
        if self.worker.alive():
            return True
        if self.state in ("setup_needed", "error"):
            return False
        size, device, compute = self.choose_backend()
        if not self.model_available(size):
            log.warning("model cache missing for size=%s -> setup_needed", size)
            self.set_state("setup_needed")
            return False
        self.set_state("loading_model")
        ref = self.model_ref(size)
        ok, err = self.worker.start(ref, device, compute)
        if not ok and err and err.get("cuda_runtime") and device == "cuda":
            # Spec discipline (Room Voice Link): a CUDA runtime failure
            # is recoverable by rebuilding on CPU — once.
            log.warning("CUDA worker load failed (%s); rebuilding on CPU/base",
                        err.get("error"))
            size, device, compute = "base", "cpu", "int8"
            if not self.model_available(size):
                self.set_state("setup_needed")
                return False
            ok, err = self.worker.start(self.model_ref(size), device, compute)
        if not ok:
            log.error("worker load failed: %s", (err or {}).get("error"))
            if err and "model" in str(err.get("error", "")).lower() and \
                    ("not" in str(err.get("error", "")).lower() or
                     "exist" in str(err.get("error", "")).lower()):
                self.set_state("setup_needed")
            else:
                self.set_state("error")
                self.send(PKT_ERROR, {"code": "worker_load_failed",
                                      "message": "ASR worker failed to load",
                                      "recoverable": False})
            return False
        self.backend = (ref, size, device, compute)
        self.last_backend_change = time.monotonic()
        self.send_ready()
        self.set_state("cpu_degraded" if device == "cpu" else "listening")
        log.info("worker up: size=%s device=%s compute=%s", size, device, compute)
        return True

    def note_worker_crash(self):
        now = time.monotonic()
        self.worker_crashes = [t for t in self.worker_crashes if now - t < 60]
        self.worker_crashes.append(now)
        if len(self.worker_crashes) >= 2:
            log.error("worker crashed twice within 60 s -> error state")
            self.set_state("error")
            self.send(PKT_ERROR, {"code": "worker_crashed",
                                  "message": "ASR worker crashed repeatedly",
                                  "recoverable": False})
            return False
        log.warning("worker died; one automatic restart")
        self.backend = None
        return True

    # ---------------- stream intake ----------------
    def reset_epoch(self, epoch):
        with self.stream_lock:
            self._reset_epoch_locked(epoch)

    def _reset_epoch_locked(self, epoch):
        self.epoch = epoch
        self.epoch_base_abs = self.abs_end
        self.buf = np.zeros(0, dtype=np.float32)
        self.buf_start_abs = self.abs_end
        self.resampler = None
        self.native_expected = None
        self.utt = None
        self.emitted.clear()
        self.next_speech_from = self.abs_end
        self.last_committed_end_abs = self.abs_end
        self.last_vad_tick_abs = self.abs_end
        log.info("epoch reset -> %d", epoch)

    def on_audio(self, epoch, payload):
        with self.stream_lock:
            self._on_audio_locked(epoch, payload)

    def _on_audio_locked(self, epoch, payload):
        if epoch != self.epoch:
            self.reset_epoch(epoch)
        if len(payload) < AUDIO_PREFIX.size:
            return
        first_native, rate, count = AUDIO_PREFIX.unpack_from(payload, 0)
        pcm = np.frombuffer(payload, dtype=np.int16,
                            count=count, offset=AUDIO_PREFIX.size)
        if self.native_expected is not None and first_native != self.native_expected:
            # Timeline discontinuity (tap dropped frames / source jump):
            # flush the resampler and drop uncommitted speech.
            gap = max(0, first_native - self.native_expected)
            frame = max(1, count)
            self.dropped_frames += int(gap / frame)
            self.resampler = None
            self.utt = None
            self.next_speech_from = self.abs_end
            log.info("audio discontinuity: native gap %d samples", gap)
        self.native_expected = first_native + count
        self.native_rate = rate
        import av
        if self.resampler is None:
            self.resampler = av.AudioResampler(format="flt", layout="mono",
                                               rate=SR16)
        frame = av.AudioFrame(format="s16", layout="mono", samples=count)
        frame.planes[0].update(pcm.astype(np.int16).tobytes())
        frame.sample_rate = rate
        out = []
        for rf in self.resampler.resample(frame):
            arr = rf.to_ndarray().reshape(-1).astype(np.float32)
            out.append(arr)
        if not out:
            return
        new = np.concatenate(out)
        self.buf = np.concatenate([self.buf, new]) if len(self.buf) else new
        self.abs_end += len(new)
        cap = BUFFER_SECONDS * SR16
        if len(self.buf) > cap:
            excess = len(self.buf) - cap
            self.buf = self.buf[excess:]
            self.buf_start_abs += excess
            if self.utt is not None and self.utt["start"] < self.buf_start_abs:
                self.utt = None  # slid out of the window unfinalised
            if self.next_speech_from < self.buf_start_abs:
                self.next_speech_from = self.buf_start_abs

    def buf_slice(self, start_abs, end_abs):
        with self.stream_lock:
            a = max(0, start_abs - self.buf_start_abs)
            b = max(0, end_abs - self.buf_start_abs)
            return self.buf[a:b].copy()

    def epoch_ms(self, abs_sample):
        return int((abs_sample - self.epoch_base_abs) * 1000 / SR16)

    # ---------------- control/config ----------------
    def on_control(self, obj, epoch=None):
        event = obj.get("event")
        log.info("control: %s", event)
        if event == "source_paused":
            self.paused = True
            if self.utt is not None:
                self.run_job(final=True)
            self.utt = None
        elif event == "source_resumed":
            self.paused = False
            self.utt = None
            self.next_speech_from = self.abs_end
        elif event == "source_changed":
            # Reset to the epoch the sender is now on (the AUDIO
            # packets carry it too and would force the same reset).
            self.reset_epoch(self.epoch if epoch is None else epoch)
        elif event == "stop":
            self.shutdown = True

    def on_config(self, obj):
        mode = obj.get("mode")
        if mode in ("transcribe", "translate"):
            if mode != self.mode:
                self.pending_mode = mode  # applies at the next utterance boundary
        sc = obj.get("source_class")
        if sc in ("system", "microphone"):
            self.source_class = sc
        self.want_worker = True

    # ---------------- ASR jobs ----------------
    def run_job(self, final):
        """Run one ASR job for the open utterance. Blocking (scheduler
        thread). Returns True if a job ran."""
        with self.job_lock:
            return self._run_job_locked(final)

    def _run_job_locked(self, final):
        utt = self.utt
        if utt is None or self.paused and not final:
            return False
        if not self.worker.alive():
            return False
        job_start = max(self.epoch_base_abs, utt["start"] - CONTEXT_SAMPLES,
                        self.buf_start_abs)
        job_end = utt["end"]
        audio = self.buf_slice(job_start, job_end)
        if len(audio) < SR16 // 4:
            if final:
                self.next_speech_from = utt["end"]
                self.utt = None
            return False
        # Conservative per-utterance gain: lift quiet speech only.
        peak = float(np.max(np.abs(audio))) if len(audio) else 0.0
        if 0.02 < peak < 0.85:
            gain = min(4.0, 0.85 / peak)
            if gain > 1.0:
                audio = (audio * gain).astype(np.float32)
        task = "translate" if self.mode == "translate" else "transcribe"
        t0 = time.monotonic()
        ok, res = self.worker.transcribe(audio, task, utt.get("language"))
        wall = time.monotonic() - t0
        dur = len(audio) / SR16
        log.info("job final=%s ok=%s audio=%.2fs wall=%.2fs", final, ok, dur, wall)
        if not ok:
            if res and res.get("cuda_runtime") and self.backend and \
                    self.backend[2] == "cuda":
                log.warning("CUDA inference failed; rebuilding worker on CPU/base")
                self.worker.stop()
                self.backend_override = ("base", "cpu", "int8")
                self.backend = None
                self.ensure_worker()
            return False
        # latency step-down: small/CUDA persistently missing budget
        if self.backend and self.backend[1] == "small" and self.backend[2] == "cuda" \
                and dur > 0:
            if wall / dur > 0.8:
                self.job_ratio_streak += 1
                if self.job_ratio_streak >= 5 and \
                        time.monotonic() - self.last_backend_change > 60:
                    log.warning("small/CUDA missing latency budget; stepping to base")
                    self.job_ratio_streak = 0
                    self.backend_override = ("base", "cuda", "int8_float16")
                    self.worker.stop()
                    self.backend = None
                    self.ensure_worker()
            else:
                self.job_ratio_streak = 0
        lang = res.get("language")
        lprob = float(res.get("language_probability") or 0.0)
        if lang:
            self.last_language = lang
            if utt.get("language") is None:
                utt["language"] = lang
                utt["language_prob"] = lprob
        # Translate low-confidence language hold (spec 4.2): keep the
        # text provisional (unpublished) until a later pass stabilises.
        hold_publish = (self.mode == "translate" and not final
                        and utt.get("language_prob", 1.0) < 0.35)
        cutoff = job_end if final else job_end - SAFETY_TAIL
        for seg in res.get("segments", []):
            if float(seg.get("no_speech_prob", 0.0)) > 0.6:
                continue
            words = seg.get("words") or []
            if words:
                # Word-level reconciliation: the job audio carries up
                # to 4 s of preceding context, and Whisper happily
                # returns one segment spanning context + utterance.
                # Attribute by WORD times: a word belongs to this
                # utterance when its midpoint falls inside it; the
                # block's text/times come from the accepted words
                # alone, so context words never leak into a caption.
                acc = []
                for w in words:
                    ws = job_start + int(float(w["start"]) * SR16)
                    we = job_start + int(float(w["end"]) * SR16)
                    if utt["start"] - SR16 // 10 <= (ws + we) // 2 < utt["end"]:
                        acc.append((ws, we, w["word"]))
                if not final:
                    acc = [a for a in acc if a[1] <= cutoff]
                if not acc:
                    continue
                text = "".join(a[2] for a in acc).strip()
                if not text:
                    continue
                s_clip, e_clip = acc[0][0], acc[-1][1]
            else:
                text = (seg.get("text") or "").strip()
                if not text:
                    continue
                s_abs = job_start + int(float(seg["start"]) * SR16)
                e_abs = job_start + int(float(seg["end"]) * SR16)
                if e_abs <= utt["start"] or s_abs >= utt["end"]:
                    continue  # context-only segment
                s_clip = max(s_abs, utt["start"])
                e_clip = min(e_abs, utt["end"])
                if not final and e_clip > cutoff:
                    continue  # inside the safety tail — a later pass owns it
            if hold_publish:
                continue
            start_ms = self.epoch_ms(s_clip)
            cid = f"cap-{self.epoch}-{start_ms}"
            prev = self.emitted.get(cid)
            if prev and prev[1] and prev[0] != text:
                continue  # already final with other text — finals are locked
            if prev and prev[0] == text and prev[1] == final:
                continue
            self.emitted[cid] = (text, final)
            self.send(PKT_CAPTION, {
                "id": cid, "epoch": self.epoch,
                "start_ms": start_ms, "end_ms": self.epoch_ms(e_clip),
                "final": final, "text": text, "mode": self.mode,
                "language": utt.get("language") or lang,
                "language_probability": utt.get("language_prob", lprob)})
            if e_clip > self.last_committed_end_abs:
                self.last_committed_end_abs = e_clip
        if final:
            self.next_speech_from = utt["end"]
            self.utt = None
        else:
            utt["prov_upto"] = job_end
        return True

    # ---------------- scheduler ----------------
    def vad_regions(self):
        from faster_whisper.vad import VadOptions, get_speech_timestamps
        win_start = max(self.buf_start_abs,
                        min(self.next_speech_from,
                            self.utt["start"] if self.utt else self.next_speech_from)
                        - 2 * SR16)
        audio = self.buf_slice(win_start, self.abs_end)
        if len(audio) < SR16 // 4:
            return []
        regions = get_speech_timestamps(
            audio, vad_options=VadOptions(
                threshold=0.5, min_speech_duration_ms=250,
                min_silence_duration_ms=700, speech_pad_ms=200),
            sampling_rate=SR16)
        out = []
        for r in regions:
            s, e = r["start"], r["end"]
            # faster-whisper returns SAMPLE offsets at the given rate;
            # guard against a seconds-returning version.
            if e <= len(audio) / SR16 + 1 and len(audio) > SR16:
                s, e = int(s * SR16), int(e * SR16)
            out.append((win_start + s, win_start + e))
        return out

    def scheduler_tick(self):
        """One 500 ms-of-stream scheduling step."""
        if self.pending_mode and self.utt is None:
            self.mode = self.pending_mode
            self.pending_mode = None
            log.info("mode -> %s", self.mode)
        regions = self.vad_regions()
        now_end = self.abs_end
        if self.utt is None:
            for (s, e) in regions:
                if e > self.next_speech_from and s >= self.buf_start_abs:
                    self.utt = {"start": max(s, self.next_speech_from),
                                "end": e, "prov_upto": 0,
                                "language": None, "language_prob": 1.0}
                    break
        if self.utt is not None:
            utt = self.utt
            for (s, e) in regions:
                if abs(s - utt["start"]) <= SR16 // 2 or \
                        (s <= utt["start"] + SR16 // 2 and e > utt["start"]):
                    utt["end"] = max(utt["end"], e)
            # force-split a continuous region at 12 s, preferring the
            # latest low-energy 100 ms point in the final 2 s.
            if utt["end"] - utt["start"] >= MAX_UTTERANCE:
                cap = utt["start"] + MAX_UTTERANCE
                lo = cap - 2 * SR16
                seg = self.buf_slice(lo, cap)
                if len(seg) >= SR16 // 10:
                    frame = SR16 // 10
                    energies = [float(np.mean(np.abs(
                        seg[i:i + frame]))) for i in range(0, len(seg) - frame, frame)]
                    if energies:
                        cap = lo + (int(np.argmin(energies)) + 1) * frame
                utt["end"] = cap
                self.run_job(final=True)
                return
            closed = now_end - utt["end"] >= int(0.7 * SR16)
            if closed:
                self.run_job(final=True)
                return
            if utt["end"] - max(utt["prov_upto"], utt["start"]) >= PROVISIONAL_STEP \
                    and not self.paused:
                self.run_job(final=False)

    def scheduler_loop(self):
        while not self.shutdown:
            time.sleep(0.1)
            now = time.monotonic()
            snap = self.poller.snapshot()
            # ---- game hold (spec 3.4) ----
            if snap["game"]:
                if not self.hold_game:
                    self.hold_game = True
                    self.hold_since = now
                    self.clear_since = None
                    self.utt = None
                    self.next_speech_from = self.abs_end
                    self.set_state("game_hold")
                    log.info("game hold: %s", snap["game_name"])
                elif self.worker.alive() and now - self.hold_since >= 10:
                    log.info("game hold 10 s: terminating worker (VRAM release)")
                    self.worker.stop()
                    self.backend = None
                continue
            if self.hold_game:
                if self.clear_since is None:
                    self.clear_since = now
                if now - self.clear_since >= 5:
                    self.hold_game = False
                    self.clear_since = None
                    self.buf = np.zeros(0, dtype=np.float32)
                    self.buf_start_abs = self.abs_end
                    self.utt = None
                    self.next_speech_from = self.abs_end
                    if self.want_worker:
                        self.ensure_worker()
                continue
            # ---- GPU busy (agent down + no headroom) ----
            gpu_busy = (not snap["agent_ok"] and snap["free_mib"] is not None
                        and (snap["free_mib"] < 3072 or (snap["util"] or 0) >= 95))
            if gpu_busy:
                if self.state != "gpu_busy" and self.state not in ("setup_needed", "error"):
                    self.set_state("gpu_busy")
                continue
            if self.state == "gpu_busy":
                self.set_state("listening" if self.backend and self.backend[2] == "cuda"
                               else "cpu_degraded")
            # ---- VRAM collapse under a live CUDA worker -> CPU once ----
            if snap["low_vram_streak"] >= 2 and self.backend and \
                    self.backend[2] == "cuda" and not self.hold_game and \
                    now - self.last_backend_change > 60:
                log.warning("free VRAM collapsed; respawning worker on CPU/base")
                self.backend_override = ("base", "cpu", "int8")
                self.worker.stop()
                self.backend = None
            # ---- worker lifecycle ----
            if self.want_worker and not self.paused:
                if not self.worker.alive():
                    if self.state not in ("setup_needed", "error"):
                        if self.backend is not None or self.worker.proc is not None \
                                or self.args.fake_asr:
                            if not self.note_worker_crash():
                                pass
                            else:
                                self.ensure_worker()
                        else:
                            self.ensure_worker()
                elif self.state in ("starting", "loading_model"):
                    self.set_state("cpu_degraded" if self.backend and
                                   self.backend[2] == "cpu" else "listening")
            # ---- stream scheduling ----
            # Advance in fixed 500 ms stream steps and DRAIN: when the
            # socket delivers a burst (or a job blocked the loop), one
            # step per loop pass would strand complete utterances
            # behind last_vad_tick_abs forever once the stream slows.
            if self.worker.alive() and not self.paused:
                steps = 0
                while self.abs_end - self.last_vad_tick_abs >= VAD_TICK_SAMPLES \
                        and steps < 8 and not self.shutdown:
                    self.last_vad_tick_abs += VAD_TICK_SAMPLES
                    try:
                        self.scheduler_tick()
                    except Exception:  # noqa: BLE001 — keep the seam alive
                        log.exception("scheduler tick failed")
                    steps += 1
            # ---- heartbeat + periodic status ----
            if now - self.last_outbound > 1.0 and self.conn is not None:
                self.send(PKT_HEARTBEAT, raw=b"")
            if now - self.last_status_sent > 2.0 and self.conn is not None:
                self.send_status()

    # ---------------- connection ----------------
    def run(self):
        self.poller.start()
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        os.makedirs(self.args.rundir, exist_ok=True)
        ready_path = os.path.join(self.args.rundir,
                                  f"helper-ready-{self.session}.json")
        with open(ready_path, "w", encoding="utf-8") as f:
            json.dump({"pid": os.getpid(), "port": port,
                       "protocol": PROTOCOL_VERSION, "session": self.session,
                       "started_at": time.time()}, f)
        log.info("listening on 127.0.0.1:%d session=%s", port, self.session)
        srv.settimeout(0.5)
        conn = None
        try:
            while not self.shutdown:
                try:
                    conn, _ = srv.accept()
                    break
                except socket.timeout:
                    continue
            if conn is not None:
                self.conn = conn
                conn.settimeout(0.5)
                self.set_state("starting")
                sched = threading.Thread(target=self.scheduler_loop, daemon=True,
                                         name="scheduler")
                sched.start()
                reader = PacketReader()
                hello_ok = False
                while not self.shutdown:
                    try:
                        data = conn.recv(65536)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not data:
                        break
                    reader.feed(data)
                    while True:
                        pkt = reader.next()
                        if pkt is None:
                            break
                        ptype, epoch, _seq, payload = pkt
                        if ptype == PKT_HELLO:
                            try:
                                obj = json.loads(payload.decode("utf-8"))
                            except Exception:
                                obj = {}
                            if obj.get("session") != self.session or \
                                    obj.get("protocol") != PROTOCOL_VERSION:
                                self.send(PKT_ERROR, {
                                    "code": "protocol_mismatch",
                                    "message": "Caption helper protocol mismatch",
                                    "recoverable": False})
                                self.shutdown = True
                                break
                            hello_ok = True
                            self.epoch = int(obj.get("epoch", 0))
                            self.epoch_base_abs = self.abs_end
                        elif ptype == PKT_CONFIG and hello_ok:
                            self.on_config(json.loads(payload.decode("utf-8")))
                        elif ptype == PKT_AUDIO and hello_ok:
                            self.on_audio(epoch, payload)
                        elif ptype == PKT_CONTROL and hello_ok:
                            self.on_control(json.loads(payload.decode("utf-8")),
                                            epoch)
                        elif ptype == PKT_HEARTBEAT:
                            pass
                    if reader.error:
                        log.warning("packet stream desync; closing")
                        break
                # client gone (or stop): short grace, then exit — no
                # orphan transcription process by design.
                time.sleep(3)
        finally:
            self.shutdown = True
            try:
                self.worker.stop()
            except Exception:
                pass
            try:
                if conn is not None:
                    conn.close()
                srv.close()
            except Exception:
                pass
            try:
                os.remove(ready_path)
            except OSError:
                pass
            log.info("helper exiting")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worker", action="store_true", help="internal ASR worker mode")
    ap.add_argument("--session")
    ap.add_argument("--rundir")
    ap.add_argument("--mode", default="transcribe",
                    choices=["transcribe", "translate"])
    ap.add_argument("--models-dir")
    ap.add_argument("--log-dir")
    ap.add_argument("--agent-urls", default="")
    ap.add_argument("--allow-model-names", action="store_true")
    ap.add_argument("--fake-asr", action="store_true")
    args = ap.parse_args()
    if args.worker:
        worker_main()
        return
    if not args.session or not args.rundir:
        ap.error("--session and --rundir are required")
    if not args.log_dir:
        args.log_dir = os.path.join(os.path.dirname(os.path.abspath(args.rundir)),
                                    "logs")
    if not args.models_dir:
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        args.models_dir = os.path.join(base, "MeterBar", "Captions", "models")
    setup_logging(args.log_dir)
    log.info("helper starting (mode=%s fake_asr=%s)", args.mode, args.fake_asr)
    Supervisor(args).run()


if __name__ == "__main__":
    main()
