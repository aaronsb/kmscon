# The image helper and the user it runs as

The kitty graphics protocol lets a program name a file, a temporary file or a shared memory object for the terminal to read an image from, and send images compressed with zlib or encoded as PNG. kmscon runs as root, so it does none of that reading or decoding itself. A separate program, `kmscon-image-helper`, installed beside kmscon's modules (`/usr/lib/kmscon` in the fork's package), does it as an ordinary user and hands back the bytes or pixels in a sealed memory file, which kmscon checks before it uses them. [ADR-103](architecture/render/ADR-103-images-are-placements-that-travel-with-lines-sixel-and-the-kitty-graphics-protocol-feed-one-store.md) ("The image helper") is the full design.

kmscon reaches the helper only while the kitty graphics protocol is on (`kitty-graphics`, on by default, in kmscon.conf(5)) and images are allowed (`image-storage-limit` not 0).

## What the helper does

- kmscon starts one helper per terminal session at the session's first image request, and stops it when the session ends, when the session's user changes (a logout and a new login), or when it fails a request.
- The helper reads only what a request bounds, decodes PNG and zlib, and answers each request with an outcome or the image. It knows nothing of the kitty protocol's keys: libtsm, kmscon's terminal library, turns them into plain bounds (where to start, the most to read, the fewest bytes the image needs).
- Bytes a program sends past what the image needs are skipped, as kitty skips them. The helper counts the bytes it read or could have read and did not use (for an uncompressed file from its size, without reading them), and kmscon logs the count under `kmscon --debug`, never the file's name.
- It refuses to open anything but a regular file, refuses files in `/proc`, `/sys`, `/dev` other than `/dev/shm`, and on kernel pseudo-filesystems wherever they are mounted, and deletes a temporary file only when its path lies in `/tmp`, `/dev/shm` or kmscon's `TMPDIR` and contains `tty-graphics-protocol`. It deletes a temporary file or shared memory object only when it owns it, so even a helper running as root leaves other users' files alone.
- It starts stripped down: in a session of its own with no controlling terminal, every user and group id set to the target user's and checked, `no_new_privs` set, ambient capabilities cleared, working directory `/`, no descriptor open but its socket, an environment holding only `TMPDIR`, and a memory limit of three times `image-storage-limit` plus 64 MiB.
- A request it does not answer within 2 seconds, or a reply that fails one of kmscon's checks, costs the program that sent it an error reply and the helper a restart.

## Which user it runs as

The helper runs as the session's user, never as the program in the foreground, so `sudo` inside a session does not let a program have files read as root. Desktop terminals behave the same way: they run as the desktop user whatever runs inside them. kmscon finds the user again before each request, in one of three ways.

| Situation | Files, temporary files, shared memory | Inline PNG and zlib images |
|---|---|---|
| kmscon built with sd-login, and the terminal's login in a logind session of its own, of a login class (`user`, `user-early`, which root's console logins get, or their `-light` forms) | read as the session's user, root included when root logged in through PAM | decoded as the session's user |
| logind running, and the terminal's child in no logind session of its own (the login prompt, or a session it shares with kmscon), or its session's class or user not found | refused, without starting the helper | decoded as `kmscon-image`, else `nobody`, else refused |
| kmscon built without sd-login, or no logind running | read as the owner of the session's pty, as `login` sets it; refused when that owner is root | decoded as the pty's owner; when that owner is root, as `kmscon-image`, else `nobody`, else refused |

sd-login comes from libsystemd or libelogind, and the build finds either on its own (the `sd_login` meson option, `auto` by default). Whether logind is running is read from its seats: none means not running, and an error counts as running, so an unknown state refuses files rather than reading them.

A refused request answers the program the same way a missing file does (`EBADF`), and kmscon logs the reason at debug level.

## Before login

At a login prompt nobody has logged in yet, and the terminal belongs to root. Reading a file for a program at that prompt would mean reading it as root, for whoever is at the keyboard, which is what the helper exists to prevent. So before login, and whenever the identity would fall back to a root-owned pty, kmscon reads no file, temporary file or shared memory object at all. Inline images still work, so a program at the login prompt can still show a picture: their bytes come from the program itself, and decoding them reads nothing from disk.

A temporary file or shared memory object a program names at the login prompt is left where it is, since no helper may delete files before anyone has logged in.

## Why a dedicated user, and what happens without one

Before login there is no session user to read or decode as, and the terminal belongs to root. kmscon answers that with two rules: file media are refused before login whatever users exist, and inline PNG and zlib images decode as a user that can do nothing else.

- **Without any separate user.** Had the helper simply taken the pty's owner, it would run as root at every login prompt, and on a system without logind for as long as the pty stays root's. Any program at that prompt, or an escape sequence printed to the console (a hostile file someone `cat`s, a message a service writes), could name `/etc/shadow` or another user's file and have it read and drawn on the screen. It could have the helper delete root's temporary files and shared memory objects that match the name rules above. And a bug in the PNG or zlib decoder would run as root. Refusing file media before login, and never reading as root by fallback, close the first two; decoding as a user other than root closes the third.
- **With `nobody`.** Inline images decode and file media stay refused, so this works, but the identity is shared. NFS root squashing maps a remote root to `nobody`, and some daemons run as it, so a fault in the decoder running as `nobody` could reach whatever those share: files a client's root wrote over NFS, and those daemons' processes.
- **With `kmscon-image`.** The decoder runs as a user that owns no files, can read nothing private, shares its uid with nothing, and only ever runs the helper. File media stay refused before login whichever user decodes.
- **Logged in.** None of this applies: the helper runs as the session's user and can read only what that user can, as the user's own programs could.

`kmscon-image` has home `/` and no login shell. If neither user exists, or a name service resolves either to uid 0 or gid 0, inline images before login are refused too. The pre-login user runs with its primary group only, with no supplementary groups. kmscon looks it up once, the first time it needs it, so a slow network name service cannot stall the login prompt on every image; a missing user is remembered too, so restart kmscon (each `kmsconvt@` unit) after creating or removing it.

### Creating and checking it

kmscon installs `kmscon-image.conf` into systemd's `sysusers.d` directory (`/usr/lib/sysusers.d` in the fork's package), and `systemd-sysusers` creates the user from it: `systemd-sysusers.service` at boot, and a package's install hook (pacman runs it when the fork's package is installed). `getent passwd kmscon-image` shows whether it exists.

On a system without `systemd-sysusers` nothing creates it, and kmscon uses `nobody` unless a package or the administrator creates `kmscon-image`. The same user by hand, with the system's path to `nologin`:

```sh
useradd --system --user-group --home-dir / --no-create-home \
        --shell /usr/sbin/nologin \
        --comment "kmscon image helper before login" kmscon-image
```

`userdel kmscon-image` removes it safely: it owns no files, and kmscon then decodes as `nobody`.

## What this means for login and sessions

`kmsconvt@.service` starts kmscon on a VT, and kmscon runs `/bin/login -p` in each terminal session unless `--login` names another command. Logging in there opens a logind session through pam_systemd, of class `user` (`user-early` for root, since systemd 256), and the helper runs as its user.

- **Started without login.** On a system running logind, a kmscon session started with a command that does not log in, such as `--login=/bin/bash`, never belongs to a logind session, so file media are refused for the whole session, even when the shell runs as an ordinary user. Inline images still work.
- **Root at the console.** Root logged in through PAM on a system running logind is a logind session, and the helper runs as root for it. On a system without logind, a root login cannot be told apart from the login prompt by the pty's owner, so root gets no file images there, only inline ones.
- **A changed pty owner.** logind's answer comes first because it is the identity PAM authenticated. The pty's owner is file metadata that any root process in the session can change; when the two disagree, kmscon logs the disagreement and uses logind's.
- **Inside someone's session.** A kmscon started from a shell inside a logind session (`kmscon --vt=8 --switchvt` run by a logged-in user) hands that session to its login process, and a login on its VT then creates no session of its own. kmscon does not count a session it shares with its child, so that VT is treated as before login, whoever logs in there: no file images. The same holds for kmscon's own greeter session in a libseat build.
- **Not running as root.** A kmscon started as an ordinary user (with libseat) starts the helper only when the session's user is kmscon's own user, and refuses every image request otherwise. The helper then runs with kmscon's user and group ids, all of them, real and saved included, and keeps kmscon's supplementary groups (`video` or `input`, say), which only root can change; `/dev` is refused by path in any case. A non-root kmscon that holds group 0 in any form, as a setgid-root install would, reads no file images and starts no helper.

## Building and packaging

- The helper needs zlib and libpng at build time and at run time.
- `sd_login` (`auto`, `enabled` or `disabled`) chooses whether kmscon asks logind for the session's user. Without it, every session is identified by its pty's owner, with the root rule above.
- `kmscon-image.conf` is installed into `sysusersdir` from systemd's pkg-config file, or `lib/sysusers.d` under the prefix when there is none.
