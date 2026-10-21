# Build environment for this fork: everything kmscon and the libtsm fork need
# to build and run their tests, so the host never needs the toolchain or
# -devel packages. The Makefile at the repository root runs every build step
# in this image as the invoking user, with the sources bind-mounted.
FROM archlinux:base-devel

RUN pacman -Syu --noconfirm --needed \
        meson ninja git python check \
        libdrm libxkbcommon systemd-libs dbus glib2 zlib \
        freetype2 fontconfig pango libglvnd mesa seatd \
        docbook-xsl libxslt linux-api-headers \
    && pacman -Scc --noconfirm
