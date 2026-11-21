"""Record real phase times in WORK, without asserting firmware acceptance."""
from __future__ import annotations

import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path
import tempfile


def timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def seconds_between(start: str, end: str) -> float:
    elapsed = (datetime.fromisoformat(end) - datetime.fromisoformat(start)).total_seconds()
    if elapsed < 0:
        raise ValueError("Clock moved backwards; preserve evidence and correct timestamps explicitly")
    return round(elapsed, 3)


def outside_package(path: Path) -> Path:
    path = path.resolve()
    package = Path(__file__).resolve().parents[1]
    if path.is_relative_to(package):
        raise ValueError("Write task records to WORK, not the installed Skill")
    return path


def update(state: dict, action: str, phase: str | None, status: str | None,
           evidence: str | None, now: str) -> None:
    measurements = state.setdefault("measurements", {})
    progress = state.setdefault("progress", {})
    measurements.setdefault("started_at", now)
    if measurements["started_at"] is None:
        measurements["started_at"] = now
    phases = measurements.setdefault("phases", [])
    if action == "init":
        return
    if not phase:
        raise ValueError("--phase is required")
    active = [p for p in phases if p["phase"] == phase and p.get("ended_at") is None]
    if action == "start":
        if active:
            raise ValueError("This phase already has an active attempt; end it before retrying")
        phases.append({"phase": phase, "attempt": 1 + sum(p["phase"] == phase for p in phases),
                       "started_at": now, "ended_at": None, "elapsed_seconds": None,
                       "status": "running", "evidence": []})
        progress["phase"] = phase
    elif action == "end":
        if len(active) != 1:
            raise ValueError("No unique active phase to end")
        if status not in {"passed", "failed", "blocked", "cancelled", "pending"}:
            raise ValueError("--status is required for end")
        if status in {"passed", "failed"} and not evidence:
            raise ValueError("Passed/failed phases need an --evidence path or record ID")
        item = active[0]
        elapsed = seconds_between(item["started_at"], now)
        item.update(ended_at=now, elapsed_seconds=elapsed, status=status)
        if evidence:
            item["evidence"].append(evidence)
    else:
        raise ValueError("Unknown action")
    measurements["observed_at"] = now
    measurements["elapsed_seconds"] = seconds_between(measurements["started_at"], now)
    # Parallel phase durations are deliberately not summed. Neither this recorder
    # nor a phase named 'delivery' can declare device acceptance or invent usage.


def save(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=path.parent, suffix=".tmp", delete=False) as stream:
            temp = Path(stream.name)
            json.dump(state, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        if temp and temp.exists():
            temp.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("action", choices=["init", "start", "end"])
    parser.add_argument("--phase")
    parser.add_argument("--status", choices=["passed", "failed", "blocked", "cancelled", "pending"])
    parser.add_argument("--evidence")
    args = parser.parse_args()
    try:
        path = outside_package(args.state)
        lock = path.with_name(path.name + ".lock")
        path.parent.mkdir(parents=True, exist_ok=True)
        # One writer at a time; never silently merge racing subagent updates.
        with lock.open("x", encoding="utf-8") as stream:
            stream.write(str(os.getpid()))
        try:
            if path.exists():
                state = json.loads(path.read_text(encoding="utf-8-sig"))
            elif args.action == "init":
                template = Path(__file__).resolve().parents[1] / "assets/task-state.example.json"
                state = json.loads(template.read_text(encoding="utf-8-sig"))
            else:
                raise ValueError("Initialize the task record first")
            update(state, args.action, args.phase, args.status, args.evidence, timestamp())
            save(path, state)
        finally:
            lock.unlink()
        print(json.dumps({"status": "RECORDED", "state": str(path), "action": args.action,
                          "scope": "timing_only_not_acceptance"}, ensure_ascii=True))
        return 0
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(json.dumps({"status": "ERROR", "error": str(exc)}, ensure_ascii=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
