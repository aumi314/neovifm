#!/usr/bin/env python3

import atexit
import os
import shlex
import subprocess
import sys
import tempfile
import time


def fail(message: str, output: str = "") -> None:
    sys.stderr.write(message + "\n")
    sys.stderr.write(output[-32768:])
    raise SystemExit(1)


if len(sys.argv) != 3:
    raise SystemExit("usage: pty-smoke.py EXECUTABLE DIRECTORY")

executable = os.path.abspath(sys.argv[1])
directory = os.path.abspath(sys.argv[2])
socket_name = f"neovifm-preview-{os.getpid()}"
marker = tempfile.NamedTemporaryFile(
    dir=directory, prefix="neovifm-pty-ready-", suffix=".txt", delete=False
)
marker.close()
marker_name = os.path.basename(marker.name)
state_directory = tempfile.TemporaryDirectory(prefix="neovifm-preview-state-")
exit_status = os.path.join(state_directory.name, "exit-status")


def tmux(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["tmux", "-L", socket_name, *args],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )


def cleanup() -> None:
    tmux("kill-server")
    if os.path.exists(marker.name):
        os.unlink(marker.name)
    state_directory.cleanup()


atexit.register(cleanup)
command = "\n".join(
    [
        "unset NEOVIFM_CORE_SESSION NEOVIFM_CORE_PROBE",
        f"export NEOVIFM_SESSION_STATE={shlex.quote(os.path.join(state_directory.name, 'session.json'))}",
        f"{shlex.join([executable, directory, directory])}",
        "status=$?",
        f"printf '%s\\n' \"$status\" > {shlex.quote(exit_status)}",
        'exit "$status"',
    ]
)
started = tmux(
    "new-session", "-d", "-s", "preview", "-x", "160", "-y", "80",
    "-c", directory, command,
)
if started.returncode != 0:
    fail("Unable to start portable TUI in tmux", started.stderr)

output = ""
deadline = time.monotonic() + 25
while time.monotonic() < deadline and not (
    marker_name in output and "F10 Quit" in output and "Tasks 0/0" in output
):
    captured = tmux("capture-pane", "-p", "-t", "preview")
    if captured.returncode != 0:
        status = open(exit_status, encoding="utf-8").read().strip() if os.path.exists(exit_status) else "unknown"
        fail(f"Portable TUI exited before rendering its workspace (status {status})", output)
    output = captured.stdout
    time.sleep(0.25)

if marker_name not in output or "F10 Quit" not in output or "Tasks 0/0" not in output:
    fail("Portable TUI did not render its initial workspace and footer", output)

sent = tmux("send-keys", "-t", "preview", "F10")
if sent.returncode != 0:
    fail("Unable to send F10 to portable TUI", sent.stderr)
exit_deadline = time.monotonic() + 15
while time.monotonic() < exit_deadline:
    if os.path.exists(exit_status):
        status = open(exit_status, encoding="utf-8").read().strip()
        if status != "0":
            fail(f"Portable TUI exited with status {status}", output)
        raise SystemExit(0)
    time.sleep(0.1)

captured = tmux("capture-pane", "-p", "-t", "preview")
fail("Portable TUI did not exit after F10", captured.stdout or output)
