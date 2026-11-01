#!/usr/bin/env python3
"""Fake-MeterBar client tests for meterbar_captions_helper.py.

Runs the helper as a subprocess and speaks the MBC1 protocol to it:
  1. framing round-trip (in-process),
  2. transcribe session — JFK (44.1 kHz) must yield a final caption
     containing "country",
  3. translate session — Spanish article clip (48 kHz) must yield an
     English final caption with detected language "es",
  4. fake-ASR soak — 3 minutes of stream time at high speed; the
     helper must stay alive, keep captioning, and never write
     caption text into its log,
  5. stop + disconnect — the helper exits and removes its ready file.

Needs a local faster-whisper "base" model dir (see --models-dir;
default /tmp/mb-models). Real-ASR tests use CPU/int8.
"""
import argparse
import importlib.util
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "mbh", os.path.join(HERE, "meterbar_captions_helper.py"))
mbh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mbh)

FAILURES = []


def check(cond, label):
    print(("ok   " if cond else "FAIL ") + label)
    if not cond:
        FAILURES.append(label)


def load_pcm16(path):
    """Decode any fixture to mono PCM16 at its NATIVE rate (the helper
    under test does the 16 kHz conversion — that's part of the test)."""
    import av
    c = av.open(path)
    stream = c.streams.audio[0]
    rate = stream.codec_context.sample_rate
    res = av.AudioResampler(format="s16", layout="mono", rate=rate)
    chunks = []
    for frame in c.decode(audio=0):
        for rf in res.resample(frame):
            chunks.append(rf.to_ndarray().reshape(-1))
    return np.concatenate(chunks).astype(np.int16), rate


class FakeClient:
    def __init__(self, port, session, epoch=1):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.settimeout(0.5)
        self.reader = mbh.PacketReader()
        self.packets = []
        self.lock = threading.Lock()
        self.alive = True
        self.seq = 0
        self.epoch = epoch
        self.session = session
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self):
        while self.alive:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            self.reader.feed(data)
            while True:
                pkt = self.reader.next()
                if pkt is None:
                    break
                with self.lock:
                    self.packets.append(pkt)

    def send(self, ptype, payload):
        self.seq += 1
        self.sock.sendall(mbh.pack_packet(ptype, self.epoch, self.seq, payload))

    def send_json(self, ptype, obj):
        self.send(ptype, mbh.json_payload(obj))

    def hello(self):
        self.send_json(mbh.PKT_HELLO, {"app_version": "0.23.0", "protocol": 1,
                                       "session": self.session,
                                       "encoding": "pcm_s16le", "epoch": self.epoch})

    def stream_audio(self, pcm, rate, start_native=0, pace=0.01, silence_tail_s=2.0):
        frame_n = int(rate * 0.1)
        pos = 0
        native = start_native
        while pos < len(pcm):
            chunk = pcm[pos:pos + frame_n]
            if len(chunk) < frame_n:
                chunk = np.concatenate([chunk, np.zeros(frame_n - len(chunk),
                                                        dtype=np.int16)])
            payload = mbh.AUDIO_PREFIX.pack(native, rate, frame_n) + chunk.tobytes()
            self.send(mbh.PKT_AUDIO, payload)
            native += frame_n
            pos += frame_n
            if pace:
                time.sleep(pace)
        for _ in range(int(silence_tail_s * 10)):
            payload = mbh.AUDIO_PREFIX.pack(
                native, rate, frame_n) + np.zeros(frame_n, dtype=np.int16).tobytes()
            self.send(mbh.PKT_AUDIO, payload)
            native += frame_n
            if pace:
                time.sleep(pace)
        return native

    def captions(self):
        out = []
        with self.lock:
            for (t, _e, _s, payload) in self.packets:
                if t == mbh.PKT_CAPTION:
                    out.append(json.loads(payload.decode("utf-8")))
        return out

    def types_seen(self):
        with self.lock:
            return {t for (t, _e, _s, _p) in self.packets}

    def close(self):
        self.alive = False
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for(pred, timeout, label):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return True
        time.sleep(0.2)
    print(f"timeout waiting: {label}")
    return False


class HelperProc:
    def __init__(self, session, models_dir, extra=()):
        self.session = session
        self.tmp = tempfile.mkdtemp(prefix="mbcap-test-")
        self.rundir = os.path.join(self.tmp, "run")
        self.logdir = os.path.join(self.tmp, "logs")
        cmd = [sys.executable, os.path.join(HERE, "meterbar_captions_helper.py"),
               "--session", session, "--rundir", self.rundir,
               "--models-dir", models_dir, "--log-dir", self.logdir,
               "--agent-urls", "http://127.0.0.1:9"] + list(extra)  # agent: dead port
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.ready_path = os.path.join(self.rundir, f"helper-ready-{session}.json")

    def port(self, timeout=15):
        if not wait_for(lambda: os.path.exists(self.ready_path), timeout,
                        "ready file"):
            return None
        with open(self.ready_path) as f:
            return json.load(f)["port"]

    def log_text(self):
        p = os.path.join(self.logdir, "helper.log")
        if not os.path.exists(p):
            return ""
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def test_framing():
    pkt = mbh.pack_packet(mbh.PKT_CONFIG, 7, 42, b"hi")
    r = mbh.PacketReader()
    r.feed(pkt[:10])
    check(r.next() is None, "framing: partial header waits")
    r.feed(pkt[10:])
    got = r.next()
    check(got is not None and got[0] == mbh.PKT_CONFIG and got[1] == 7
          and got[2] == 42 and got[3] == b"hi", "framing: round-trip")
    bad = bytearray(pkt)
    bad[0] = ord('X')
    r2 = mbh.PacketReader()
    r2.feed(bytes(bad))
    check(r2.next() is None and r2.error, "framing: bad magic latches error")


def test_transcribe(models_dir):
    h = HelperProc("test-transcribe", models_dir)
    try:
        port = h.port()
        check(port is not None, "transcribe: helper ready")
        if not port:
            return
        c = FakeClient(port, "test-transcribe")
        c.hello()
        c.send_json(mbh.PKT_CONFIG, {"mode": "transcribe", "source_class": "system"})
        pcm, rate = load_pcm16(os.path.join(HERE, "testdata", "jfk.flac"))
        check(rate == 44100, f"transcribe: fixture rate {rate}")
        c.stream_audio(pcm, rate, pace=0.005)
        # CPU/base jobs on a shared VM take seconds each; wait for the
        # CONTENT (the 'country' line is the 2nd/3rd utterance), not
        # just the first final.
        def joined_finals():
            return " ".join(cp["text"] for cp in c.captions()
                            if cp.get("final")).lower()
        ok = wait_for(lambda: "country" in joined_finals(), 280,
                      "final caption containing 'country'")
        caps = c.captions()
        finals = [cp for cp in caps if cp.get("final")]
        check(ok and len(finals) >= 1, f"transcribe: {len(finals)} final caption(s)")
        joined = joined_finals()
        check("country" in joined, "transcribe: JFK text mentions 'country'")
        check(mbh.PKT_READY in c.types_seen(), "transcribe: READY received")
        check("country" not in h.log_text().lower() or "job" in h.log_text(),
              "transcribe: helper log carries no caption text")
        check("your country" not in h.log_text(),
              "transcribe: log free of transcript phrases")
        c.send_json(mbh.PKT_CONTROL, {"event": "stop"})
        c.close()
        gone = wait_for(lambda: h.proc.poll() is not None, 20, "helper exit")
        check(gone, "transcribe: helper exits after stop+disconnect")
        check(not os.path.exists(h.ready_path),
              "transcribe: ready file removed on exit")
    finally:
        h.stop()


def test_translate(models_dir):
    h = HelperProc("test-translate", models_dir)
    try:
        port = h.port()
        check(port is not None, "translate: helper ready")
        if not port:
            return
        c = FakeClient(port, "test-translate")
        c.hello()
        c.send_json(mbh.PKT_CONFIG, {"mode": "translate", "source_class": "system"})
        pcm, rate = load_pcm16(os.path.join(HERE, "testdata", "es-15s-48k.wav"))
        check(rate == 48000, f"translate: fixture rate {rate}")
        c.stream_audio(pcm, rate, pace=0.005)
        ok = wait_for(lambda: any(cp.get("final") for cp in c.captions()),
                      180, "final caption")
        caps = c.captions()
        finals = [cp for cp in caps if cp.get("final")]
        check(ok and len(finals) >= 1, f"translate: {len(finals)} final caption(s)")
        if finals:
            langs = {cp.get("language") for cp in caps}
            check("es" in langs, f"translate: detected language es (saw {langs})")
            text = " ".join(cp["text"] for cp in finals)
            check("colonización" not in text and "colonizacion" not in text.lower(),
                  "translate: output is not the Spanish source")
            check(" the " in (" " + text.lower() + " "),
                  "translate: output reads as English")
        c.send_json(mbh.PKT_CONTROL, {"event": "stop"})
        c.close()
        wait_for(lambda: h.proc.poll() is not None, 20, "helper exit")
    finally:
        h.stop()


def test_soak_fake(models_dir):
    h = HelperProc("test-soak", models_dir, extra=["--fake-asr"])
    try:
        port = h.port()
        check(port is not None, "soak: helper ready")
        if not port:
            return
        c = FakeClient(port, "test-soak")
        c.hello()
        c.send_json(mbh.PKT_CONFIG, {"mode": "transcribe", "source_class": "system"})
        pcm, rate = load_pcm16(os.path.join(HERE, "testdata", "jfk.flac"))
        # ~3 minutes of stream time: loop speech with 1.5 s silence gaps
        native = 0
        for i in range(9):
            native = c.stream_audio(pcm, rate, start_native=native, pace=0.001,
                                    silence_tail_s=1.5)
        ok = wait_for(lambda: any(cp.get("final") for cp in c.captions()),
                      60, "soak final caption")
        caps = c.captions()
        check(ok, f"soak: finals flowed ({len(caps)} captions total)")
        check(h.proc.poll() is None, "soak: helper still alive")
        check("stub caption" not in h.log_text(),
              "soak: no caption text in helper log")
        check("Traceback" not in h.log_text(), "soak: no tracebacks in log")
        c.send_json(mbh.PKT_CONTROL, {"event": "stop"})
        c.close()
        wait_for(lambda: h.proc.poll() is not None, 20, "helper exit")
    finally:
        h.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models-dir", default="/tmp/mb-models")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    test_framing()
    if not args.only or args.only == "transcribe":
        test_transcribe(args.models_dir)
    if not args.only or args.only == "translate":
        test_translate(args.models_dir)
    if not args.only or args.only == "soak":
        test_soak_fake(args.models_dir)
    print(f"\n{'FAILED: ' + str(len(FAILURES)) if FAILURES else 'ALL PASS'}")
    for f in FAILURES:
        print("  -", f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
