"""Run the CodeMyCar host regression suites. Requires only Python's standard library."""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STM32 = ("control", "power", "lighting", "display", "runtime")
BRIDGE = ("stop_barrier", "motion_json", "network")
VOICE = ("voice_frontend", "voice_session", "voice_playback", "voice_wake")
FRONTEND = ("vitest", "frontend-build")

# Each leaf has one owner. Delegate flags, stubs and build variants to that project.
SUITES = {
    "protocol": ("serial_pkt_protocol", ["bash", "tests/run.sh"]),
    **{s: ("stm32_project", ["bash", "tests/host/run.sh", s]) for s in STM32},
    **{s: ("esp32_project", ["bash", "tests/host/run.sh", s]) for s in BRIDGE},
    **{s: ("esp32_project", ["bash", f"tests/{s}_host/run.sh"]) for s in VOICE},
    "pyserver": ("pyserver", [".venv/bin/python", "-m", "pytest", "-q", "tests"]),
    "vitest": ("frontend", ["npm", "run", "test:unit", "--", "--run"]),
    "frontend-build": ("frontend", ["npm", "run", "build"]),
}
GROUPS = {
    "all": tuple(SUITES),
    "stm32": STM32,
    "esp32": BRIDGE + VOICE,
    "bridge": BRIDGE,
    "voice": VOICE,
    "frontend": FRONTEND,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("groups", nargs="*", help="Suites or groups (default: all)")
    parser.add_argument("--list", action="store_true", help="Show groups and individual suites")
    args = parser.parse_args()
    if args.list:
        for group, suites in GROUPS.items():
            print(f"{group:10} {' '.join(suites)}")
        return 0
    selected = {}
    for group in args.groups or ["all"]:
        if group not in GROUPS and group not in SUITES:
            parser.error(f"unknown suite '{group}' (see --list)")
        selected.update(dict.fromkeys(GROUPS.get(group, (group,))))

    started = time.monotonic()
    for suite in selected:
        folder, command = SUITES[suite]
        print(f"\n[{suite}] ({folder}) {shlex.join(command)}", flush=True)
        try:
            result = subprocess.run(command, cwd=ROOT / folder, check=False)
        except FileNotFoundError as exc:
            print(f"[{suite}] {exc}. See Docs/testing/README.md for setup.", file=sys.stderr)
            return 1
        if result.returncode:
            print(f"[{suite}] FAILED (exit {result.returncode}). Repeat: "
                  f"python3 tools/regress.py {suite}", file=sys.stderr)
            return result.returncode if result.returncode > 0 else 128 - result.returncode
    print(f"\nPASS: {len(selected)} suites in {time.monotonic() - started:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
