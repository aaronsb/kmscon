# check=skip=InvalidDefaultArgInFrom

# The format checkers the repository's GitHub workflow runs (.github/workflows/
# meson.yml), at the versions it runs, so `make lint` agrees with CI. clang-format
# output differs between LLVM releases, so a host's clang-format will not do.
# The pins live at the top of fork/lint.mk; the build refuses a clang-format
# other than the pinned version.
ARG BASE
FROM ${BASE}
ARG CLANG_FORMAT_VERSION
ARG MESON_FORMAT_VERSION

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-format-20 python3-pip git \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --break-system-packages --no-cache-dir meson==${MESON_FORMAT_VERSION}
RUN clang-format-20 --version | grep -q "version ${CLANG_FORMAT_VERSION}" \
    || { echo "clang-format-20 is not ${CLANG_FORMAT_VERSION}:"; clang-format-20 --version; exit 1; }
