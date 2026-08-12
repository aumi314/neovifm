#!/usr/bin/env python3

import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time


def fail(message: str, output: bytes) -> None:
    sys.stderr.write(message + "\n")
    sys.stderr.buffer.write(output[:16384])
    if len(output) > 32768:
        sys.stderr.write("\n... output truncated ...\n")
    sys.stderr.buffer.write(output[-16384:])
    raise SystemExit(1)


if len(sys.argv) != 3:
    raise SystemExit("usage: pty-smoke.py EXECUTABLE DIRECTORY")

executable = os.path.abspath(sys.argv[1])
directory = os.path.abspath(sys.argv[2])
state_directory = tempfile.TemporaryDirectory(prefix="neovifm-preview-state-")
pid, fd = pty.fork()
if pid == 0:
    os.chdir(directory)
    os.environ.pop("NEOVIFM_CORE_SESSION", None)
    os.environ.pop("NEOVIFM_CORE_PROBE", None)
    os.environ["NEOVIFM_SESSION_STATE"] = os.path.join(state_directory.name, "session.json")
    os.execv(executable, [executable, directory, directory])

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 80, 160, 0, 0))
output = bytearray()
deadline = time.monotonic() + 25
while time.monotonic() < deadline and not (b"F10" in output and b"Quit" in output):
    readable, _, _ = select.select([fd], [], [], 0.25)
    if readable:
        try:
            output.extend(os.read(fd, 65536))
        except OSError:
            break
        if len(output) > 1024 * 1024:
            del output[:-1024 * 1024]
else:
    pass

if b"F10" not in output or b"Quit" not in output:
    os.kill(pid, signal.SIGTERM)
    os.waitpid(pid, 0)
    fail("Portable TUI did not render its initial footer", bytes(output))

os.write(fd, b"\x1b[21~")
exit_deadline = time.monotonic() + 15
while time.monotonic() < exit_deadline:
    finished, status = os.waitpid(pid, os.WNOHANG)
    if finished == pid:
        if os.waitstatus_to_exitcode(status) != 0:
            fail("Portable TUI exited with a failure", bytes(output))
        state_directory.cleanup()
        raise SystemExit(0)
    time.sleep(0.1)

os.kill(pid, signal.SIGTERM)
os.waitpid(pid, 0)
fail("Portable TUI did not exit after F10", bytes(output))
