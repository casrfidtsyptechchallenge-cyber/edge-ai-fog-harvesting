"""Run folders: data/runs/<run_id>/ holds run.json (what was done) and raw.jsonl (every message received).

A run is one experiment session. Its settings are free key=value pairs (gap_mm=15, hv_mode=fixed, ...),
so a new experiment parameter needs no code change. Keep notes anonymous: no names.
"""

import json
import re
from datetime import datetime
from pathlib import Path

import aeroion_contract as ac

DEFAULT_DATA_DIR = Path("data") / "runs"
RAW_FILE = "raw.jsonl"
META_FILE = "run.json"
DATASET_DIR = "dataset"
CONFIG_FILE = "label_config.json"  # optional, per run: overrides the default label settings


def parse_settings(pairs):
    """["gap_mm=15", "hv_mode=fixed"] -> {"gap_mm": 15, "hv_mode": "fixed"}."""
    settings = {}
    for pair in pairs or []:
        key, sep, value = pair.partition("=")
        key = key.strip()
        if not sep or not key:
            raise ValueError(f"setting {pair!r} must look like key=value")
        settings[key] = _parse_value(value.strip())
    return settings


def _parse_value(text):
    for convert in (int, float):
        try:
            return convert(text)
        except ValueError:
            pass
    if text.lower() in ("true", "false"):
        return text.lower() == "true"
    return text


def new_run(data_dir, tag="", settings=None, notes="", source="mqtt", now=None):
    """Create data_dir/<date_time>_<tag>/ with its run.json. Returns (run_dir, meta)."""
    now = now or datetime.now()
    slug = re.sub(r"[^a-z0-9]+", "-", tag.lower()).strip("-")
    run_id = now.strftime("%Y-%m-%d_%H%M%S") + (f"_{slug}" if slug else "")
    run_dir = Path(data_dir) / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    meta = {
        "run_id": run_id,
        "created": now.isoformat(timespec="seconds"),
        "contract_version": ac.SCHEMA_VERSION,
        "source": source,
        "tag": slug,
        "settings": settings or {},
        "notes": notes,
    }
    write_meta(run_dir, meta)
    return run_dir, meta


def read_meta(run_dir):
    return json.loads((Path(run_dir) / META_FILE).read_text(encoding="utf-8"))


def write_meta(run_dir, meta):
    text = json.dumps(meta, indent=2, ensure_ascii=False) + "\n"
    (Path(run_dir) / META_FILE).write_text(text, encoding="utf-8")


def list_runs(data_dir):
    data_dir = Path(data_dir)
    if not data_dir.exists():
        return []
    return sorted(p for p in data_dir.iterdir() if (p / META_FILE).exists())


def resolve_run(name, data_dir):
    """A run folder path, a run id, or "latest" -> the run folder."""
    if name == "latest":
        found = list_runs(data_dir)
        if not found:
            raise FileNotFoundError(f"no runs in {data_dir}")
        return found[-1]
    for candidate in (Path(name), Path(data_dir) / name):
        if (candidate / META_FILE).exists():
            return candidate
    raise FileNotFoundError(f"no run called {name!r} (looked in {data_dir})")