#!/usr/bin/env python3
"""Runs the Giant prototype, saves its test screenshots, and fails on any error.

Usage: python3 giant/tests/demo_smoke.py <path to editor binary> <screenshot folder>

Needs a display. On Linux without one it uses xvfb-run, and a software Vulkan
driver (mesa-vulkan-drivers) renders the frames.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.join(HERE, "..", "demo")
EXPECTED = 8

# Messages from machines without sound cards or GPUs, not from Giant.
IGNORED = ("audio", "ALSA", "status < 0", "at: initialize", "2D MSAA is enabled")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    editor = os.path.abspath(sys.argv[1])
    shots = os.path.abspath(sys.argv[2])
    shutil.rmtree(shots, ignore_errors=True)

    # Register the prototype's script classes.
    subprocess.run([editor, "--headless", "--path", DEMO, "--import"], check=True, capture_output=True, timeout=600)

    command = [editor, "--path", DEMO, "--fixed-fps", "30", "--resolution", "1280x720", "--", "--screenshots=" + shots]
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") and shutil.which("xvfb-run"):
        command = ["xvfb-run", "-a", "-s", "-screen 0 1600x900x24"] + command
    run = subprocess.run(command, capture_output=True, text=True, timeout=1800)
    log = run.stdout + run.stderr
    print(log)

    errors = [line for line in log.splitlines() if ("ERROR" in line or "SCRIPT" in line) and not any(i in line for i in IGNORED)]
    pictures = sorted(f for f in os.listdir(shots) if f.endswith(".png")) if os.path.isdir(shots) else []
    print("Screenshots:", ", ".join(pictures) or "none")
    if run.returncode != 0 or errors or len(pictures) != EXPECTED:
        print("FAILED: exit code %d, %d errors, %d of %d screenshots" % (run.returncode, len(errors), len(pictures), EXPECTED))
        for line in errors:
            print("  " + line)
        return 1
    print("Prototype ran cleanly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
