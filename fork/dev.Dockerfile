# check=skip=InvalidDefaultArgInFrom

# Build environment for this fork: everything kmscon and the libtsm fork need
# to build and run their tests, so the host never needs the toolchain or
# -devel packages. The Makefile at the repository root runs every build step
# in this image as the invoking user, with the sources bind-mounted.
#
# The base image and the package set are both pinned (see the pin variables at
# the top of the Makefile): BASE is a digest, ARCHIVE_DATE a snapshot of the
# Arch Linux Archive that pacman reads instead of the live mirrors.
ARG BASE
FROM ${BASE}
ARG BASE
ARG ARCHIVE_DATE
ENV DEV_IMAGE_BASE=${BASE} DEV_ARCHIVE_DATE=${ARCHIVE_DATE}

RUN echo "Server = https://archive.archlinux.org/repos/${ARCHIVE_DATE}/\$repo/os/\$arch" \
        > /etc/pacman.d/mirrorlist
RUN pacman -Syu --noconfirm --needed \
        meson ninja git python check \
        libdrm libxkbcommon systemd-libs dbus glib2 zlib \
        freetype2 fontconfig pango libglvnd mesa seatd \
        docbook-xsl libxslt linux-api-headers \
    && pacman -Scc --noconfirm
