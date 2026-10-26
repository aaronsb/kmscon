"""The terminal side of the kitty graphics player (kitty.py): output, keys,
the window size and its changes.

Each frame is one os.write(), so a frame never interleaves with another
process writing to the same terminal: the kernel holds the tty's write lock
for the whole call.

Kept from the Bad Apple!! showcase's screen.py when the show moved to
aaronsb/kmscon-showcase (#239), cut to what the player uses; the show keeps
its own copy, and the two may drift.
"""

import fcntl
import os
import select
import shutil
import signal
import struct
import termios
import time

ESC = "\x1b"


class Quit(Exception):
    """q was pressed: the player ends."""


class AnyKey(Exception):
    """A key was pressed where any key ends the run."""


# ---------------------------------------------------------------- terminal

def out(s):
    """Write s in one call: one frame, one write(). SIGWINCH is held off
    while it writes, so a resize cannot cut the write short and let another
    process's frame land inside this one; the loop is for what remains."""
    b = s.encode()
    held = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGWINCH})
    try:
        while b:
            n = os.write(1, b)
            b = b[n:]
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, held)


def geom():
    """(cols, rows, xpixel, ypixel) of the terminal; pixels are 0 when unknown."""
    for fd in (1, 0, 2):
        try:
            rows, cols, xp, yp = struct.unpack("HHHH", fcntl.ioctl(fd, termios.TIOCGWINSZ, b"\0" * 8))
            if cols and rows:
                return cols, rows, xp, yp
        except OSError:
            pass
    c, r = shutil.get_terminal_size((80, 25))
    return c, r, 0, 0


_resized = False


def _on_winch(_sig, _frame):
    global _resized
    _resized = True


def watch_resize():
    signal.signal(signal.SIGWINCH, _on_winch)


def resized():
    """True once after each SIGWINCH: the caller reads geom() again."""
    global _resized
    r, _resized = _resized, False
    return r


_any_key = False


def any_key_ends(on=True):
    """From now on any key raises AnyKey (q still raises Quit)."""
    global _any_key
    _any_key = on


def poll_keys():
    """Reads pending keys without waiting; raises Quit on q."""
    try:
        if not os.isatty(0):
            return
        while select.select([0], [], [], 0)[0]:
            b = os.read(0, 256)
            if not b:
                return
            if b"q" in b or b"Q" in b:
                raise Quit
            if _any_key:
                raise AnyKey
    except (OSError, InterruptedError):
        return


def wait(sec):
    """Sleeps sec seconds, reading keys as they come."""
    end = time.monotonic() + max(0.0, sec)
    tty_in = os.isatty(0)
    while True:
        left = end - time.monotonic()
        if left <= 0:
            poll_keys()
            return
        if tty_in:
            try:
                if select.select([0], [], [], left)[0]:
                    poll_keys()
            except InterruptedError:
                pass
        else:
            time.sleep(left)


def cup(row, col):
    return f"{ESC}[{row};{col}H"
