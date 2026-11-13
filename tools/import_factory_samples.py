#!/usr/bin/env python3
"""Rebuild the included CC0 factory subset. Development only: numpy/scipy required.

The plugin ships with the converted files and never downloads during playback.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import pathlib
import statistics
import urllib.request

import numpy as np
from scipy.io import wavfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "SculptBass-factory-import/0.1"})
    with urllib.request.urlopen(request, timeout=45) as response:
        return response.read()


def git_blob_sha(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", type=pathlib.Path, default=ROOT.parent / "reference" / "bass-factory-cache")
    parser.add_argument("--source-directory", type=pathlib.Path, help="Optional checked-out upstream repository; prevents all network access.")
    args = parser.parse_args()
    source = json.loads((ROOT / "tools" / "factory_sources.json").read_text())
    base = "https://raw.githubusercontent.com/sfzinstruments/karoryfer.black-and-blue-basses/" + source["upstreamCommit"] + "/"
    args.cache.mkdir(parents=True, exist_ok=True)
    out = ROOT / "Samples"
    out.mkdir(exist_ok=True)
    records = source["sampleFiles"]

    def load(record: dict):
        cached = args.cache / pathlib.Path(record["sourcePath"]).name
        if args.source_directory:
            data = (args.source_directory / record["sourcePath"]).read_bytes()
        else:
            data = cached.read_bytes() if cached.exists() else fetch(base + record["sourcePath"])
        if git_blob_sha(data) != record["sourceBlobSha"]:
            raise ValueError("Upstream sample integrity mismatch: " + record["sourcePath"])
        if not cached.exists():
            cached.write_bytes(data)
        sr, raw = wavfile.read(io.BytesIO(data))
        if sr != 44100 or raw.ndim != 1:
            raise ValueError("Unexpected factory sample format: " + record["sourcePath"])
        if np.issubdtype(raw.dtype, np.integer):
            audio = raw.astype(np.float64) / float(2 ** (raw.dtype.itemsize * 8 - 1))
        else:
            audio = raw.astype(np.float64)
        audio -= np.mean(audio)
        threshold = max(0.0001, float(np.max(np.abs(audio))) * 0.001)
        active = np.flatnonzero(np.abs(audio) > threshold)
        start = max(0, int(active[0]) - int(sr * 0.001)) if len(active) else 0
        audio = audio[start:]
        # Preserve the full recorded tail; only trim silence before the attack.
        attack = min(len(audio), int(sr * 0.001))
        release = min(len(audio), int(sr * 0.025))
        audio[:attack] *= np.linspace(0.0, 1.0, attack)
        audio[-release:] *= np.linspace(1.0, 0.0, release)
        rms = float(np.sqrt(np.mean(audio[:int(sr * 0.3)] ** 2)))
        return record, sr, audio, rms

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        prepared = list(pool.map(load, records))
    print(f"Verified {len(prepared)} original sample hashes.", flush=True)

    gains = {}
    for root in sorted({r["rootNote"] for r in records}):
        group = [item for item in prepared if item[0]["rootNote"] == root]
        reference = statistics.median(item[3] for item in group if item[0]["velocityLayer"] == 1)
        # A shared gain per note preserves dynamics and RR loudness differences.
        gain = 0.16 / max(reference, 1.0e-6)
        maximum = max(float(np.max(np.abs(item[2]))) for item in group)
        gains[root] = min(gain, 0.85 / max(maximum, 1.0e-6))

    manifest = {key: value for key, value in source.items() if key != "sampleFiles"}
    manifest.update({
        "version": 1,
        "sampleRate": 44100,
        "encoding": "mono PCM16",
        "description": "15 sampled pitches, 2 dynamics, 4 recorded round robins; original acoustic pitch mapped to MIDI (B0 = 23).",
        "processing": "Leading silence trimmed; DC offset removed; 1 ms attack / 25 ms tail fade; shared level calibration per root note; PCM16 conversion.",
        "sampleFiles": [],
    })
    for record, sr, audio, _ in prepared:
        pcm = np.rint(np.clip(audio * gains[record["rootNote"]], -1.0, 1.0) * 32767.0).astype("<i2")
        path = out / record["file"]
        wavfile.write(path, sr, pcm)
        manifest["sampleFiles"].append({
            **record,
            "frames": len(pcm),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    thirdparty = ROOT / "ThirdParty"
    thirdparty.mkdir(exist_ok=True)
    license_text = ((args.source_directory / "license").read_bytes()
                    if args.source_directory else fetch(base + "license"))
    (thirdparty / "Karoryfer-CC0.txt").write_bytes(license_text)
    size = sum((out / record["file"]).stat().st_size for record in records)
    print(f"Prepared {len(records)} WAV files, {size / 1024 / 1024:.1f} MiB; manifest and original CC0 license included.", flush=True)


if __name__ == "__main__":
    main()
