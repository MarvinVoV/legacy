#!/usr/bin/env python3
"""Build isolated board profiles without replacing the C6 sdkconfig/build tree."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=("s3-n32r16v", "c6"), default="s3-n32r16v")
    parser.add_argument("--port", help="Required for flash/monitor; verify chip before flashing")
    parser.add_argument("action", choices=("build", "reconfigure", "menuconfig", "flash", "monitor"), default="build", nargs="?")
    args = parser.parse_args()
    if args.action in ("flash", "monitor") and not args.port:
        parser.error("--port is required for flash/monitor")
    target = "esp32s3" if args.board.startswith("s3") else "esp32c6"
    defaults = [ROOT / "sdkconfig.defaults"]
    if target == "esp32s3":
        defaults.append(ROOT / "profiles/s3-n32r16v.defaults")
    config = ROOT / ("sdkconfig.s3" if target == "esp32s3" else "sdkconfig")
    command = ["idf.py", "-B", str(ROOT / ("build-s3" if target == "esp32s3" else "build")),
               "-D", f"IDF_TARGET={target}", "-D", f"SDKCONFIG={config}",
               "-D", "SDKCONFIG_DEFAULTS=" + ";".join(map(str, defaults))]
    if args.port:
        command += ["-p", args.port]
    command.append(args.action)
    env = dict(os.environ, IDF_TARGET=target)
    return subprocess.call(command, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
