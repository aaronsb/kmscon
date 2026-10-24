# VT demo: what to run

- **Visual checks:** `make vt-demo KIT=kitty` from the checkout (`KIT=text`, `sixel` or `all` for the other kits). In this session kmscon reads no file for a program, so the kitty-files page shows EBADF replies with a note explaining why.
- **Real-session file media:** install the built package over the system's kmscon ([docs/fork/demo.md, "File media: the login run"](../../docs/fork/demo.md#file-media-the-login-run)), log in on a kmscon VT, then `cd <checkout> && fork/demo/demo.sh kitty-files`.
- **Feedback:** every rating is appended to `/tmp/kmscon-demo/feedback.tsv` (or `$DEMO_FEEDBACK` when demo.sh is run directly); `fork/demo/feedback.sh` prints it. `/tmp` is cleared on reboot.
- **Undo the install:** `sudo pacman -S kmscon kmscon-terminfo` puts Arch's packages back; restart or stop any kmscon VT you started.

[docs/fork/demo.md](../../docs/fork/demo.md) has the rest.
