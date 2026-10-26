# fork/demo

The operator's review pages. The full guide is [docs/fork/demo.md](../../docs/fork/demo.md).

**Visual checks, from the desktop:** `make vt-demo KIT=kitty` (or `KIT=sixel`, `KIT=text`, `KIT=glyphs`, `KIT=all`). It switches to VT 5 and pages through the kit. There, kitty-files shows EBADF replies by design, because the demo runs outside a login session.

**File media in a real session (kitty-files):**

1. `make pkg`, then `sudo pacman -U fork/build/pkg/kmscon-aaronsb-<version>-1-x86_64.pkg.tar.zst fork/build/pkg/kmscon-aaronsb-terminfo-<version>-1-x86_64.pkg.tar.zst` over Arch's kmscon.
2. `sudo systemctl restart kmsconvt@tty5.service` (or start it, after stopping `getty@tty5.service`).
3. Ctrl-Alt-F5, log in, then `cd <checkout> && fork/demo/demo.sh kitty-files`.

**Guided checks, started by the agent:** `make vt-check GUIDE=NAME` runs `fork/demo/checks/NAME.guide` on a free VT. The operator switches to it, follows each step and answers; the agent reads the answers when it ends.

**Answers** land in `/tmp/kmscon-demo/feedback.tsv`; `fork/demo/feedback.sh` prints them.

**Undo the install:** `sudo pacman -S kmscon kmscon-terminfo`, then restart or stop the kmscon VT.
