"""Explicit Windows serial enumeration/capture; never flash or auto-pick a port.

Requires the fixed toolchain Python's pyserial for list/capture. Capturing logs
does not prove that an intended BIN was actually flashed or passed acceptance.
"""
from __future__ import annotations

import argparse
import codecs
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import time

SKILL_ROOT = Path(__file__).resolve().parents[3]


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def port_description(port) -> dict:
    return {key: getattr(port, key, None) for key in (
        "device", "description", "hwid", "vid", "pid", "serial_number", "location")}


def serial_modules():
    try:
        import serial
        from serial.tools import list_ports
        return serial, list_ports
    except ImportError as error:
        raise ValueError("pyserial unavailable; use the activated fixed ESP-IDF Python environment") from error


def enumerate_ports(list_ports) -> list[dict]:
    return [port_description(port) for port in list_ports.comports()]


def capture(port: str, baud: int, seconds: float, out: Path, binary: Path,
            mode: str, confirmed: bool, serial_module, list_ports) -> dict:
    if not confirmed:
        raise ValueError("Identify the target device/port with the user before --confirmed-device")
    if baud <= 0 or not 1 <= seconds <= 300:
        raise ValueError("Use a positive project baud rate and a bounded 1..300 second capture")
    if mode not in ("standalone", "pocket"):
        raise ValueError("Mode must be standalone or pocket")
    ports = enumerate_ports(list_ports)
    selected = [item for item in ports if str(item["device"]).casefold() == port.casefold()]
    if len(selected) != 1:
        raise ValueError("Selected port absent/ambiguous; enumerate again, do not substitute another port")
    binary = binary.resolve(strict=True)
    image = binary.read_bytes()
    if not 0 < len(image) <= 16 * 1024 * 1024:
        raise ValueError("Expected the intended full BIN, within 16 MiB")
    out = out.resolve()
    if out.exists() or out == SKILL_ROOT or SKILL_ROOT in out.parents:
        raise ValueError("Use a new capture directory in WORK, outside the installed Skill")
    out.mkdir(parents=True, exist_ok=False)
    started = utc()
    log_hash, total = hashlib.sha256(), 0
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    status, error_text = "CAPTURED", None
    handle = None
    try:
        # Set control line intent before opening; OS/driver opening glitches are
        # still possible. Never pulse reset, send bytes or clear the RX buffer.
        handle = serial_module.Serial(port=None, baudrate=baud, timeout=0.25,
                                      xonxoff=False, rtscts=False, dsrdtr=False)
        handle.dtr = False
        handle.rts = False
        handle.port = selected[0]["device"]
        handle.open()
        deadline = time.monotonic() + seconds
        with (out / "serial.raw").open("xb") as raw, (out / "serial.txt").open("x", encoding="utf-8", newline="\n") as text:
            while time.monotonic() < deadline:
                chunk = handle.read(4096)
                if not chunk:
                    continue
                raw.write(chunk)
                raw.flush()
                log_hash.update(chunk)
                total += len(chunk)
                text.write(decoder.decode(chunk))
                text.flush()
            text.write(decoder.decode(b"", final=True))
    except KeyboardInterrupt:
        status = "INTERRUPTED"
        error_text = "Capture interrupted; partial raw bytes remain available"
    except (OSError, ValueError) as error:
        status = "CAPTURE_FAILED"
        error_text = str(error)
    finally:
        if handle is not None:
            try:
                handle.close()
            except OSError as error:
                status = "CAPTURE_FAILED"
                error_text = (error_text + "; " if error_text else "") + "Closing port: " + str(error)
    result = {
        "status": status, "started_utc": started, "ended_utc": utc(),
        "device": selected[0], "baud": baud, "mode": mode,
        "intended_binary": {"path": str(binary), "bytes": len(image), "sha256": hashlib.sha256(image).hexdigest()},
        "observed_image_identity": None,
        "serial_raw": "serial.raw", "serial_raw_bytes": total,
        "serial_raw_sha256": log_hash.hexdigest(), "serial_text": "serial.txt",
        "control_lines": {"dtr_requested": False, "rts_requested": False, "reset_command_sent": False},
        "hardware_verified": False,
        "scope": "Serial observation only; correlate actual boot identity, UI and physical interaction separately",
    }
    if error_text:
        result["error"] = error_text
    with (out / "capture.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return result


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    cap = commands.add_parser("capture")
    cap.add_argument("--port", required=True)
    cap.add_argument("--baud", type=int, required=True)
    cap.add_argument("--seconds", type=float, default=60)
    cap.add_argument("--out", type=Path, required=True)
    cap.add_argument("--bin", dest="binary", type=Path, required=True)
    cap.add_argument("--mode", choices=("standalone", "pocket"), required=True)
    cap.add_argument("--confirmed-device", action="store_true")
    args = parser.parse_args()
    try:
        serial, list_ports = serial_modules()
        if args.command == "list":
            result = {"ports": enumerate_ports(list_ports), "selected": None}
        else:
            result = capture(args.port, args.baud, args.seconds, args.out, args.binary,
                             args.mode, args.confirmed_device, serial, list_ports)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if args.command == "list" or result["status"] == "CAPTURED" else 1
    except (ValueError, OSError) as error:
        print(json.dumps({"status": "ERROR", "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
