# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

#
# Binary File Compiler
#
embed_gen = generator(
    python,
    arguments: [
        meson.project_source_root() / 'tools' / 'embedded_file.py',
        '@EXTRA_ARGS@',
        '@INPUT@',
        '@BUILD_DIR@',
    ],
    output: ['@PLAINNAME@.bin.h', '@PLAINNAME@.bin.c'],
)
shader_regex = [
    '--regex',
    '^/\\*.*$',
    '',  # Start of multi-line comment
    '--regex',
    '^ \\*.*$',
    '',  # Multi-line comment body
    '--regex',
    '^[ \\t]*',
    '',  # Indentation whitespace
    '--regex',
    '[\\r\\n]',
    '',  # Newlines
]

src_inc = [include_directories('.')]

subdir('shl')

subdir('font')
subdir('input')
subdir('video')
subdir('render')
subdir('misc')
subdir('uterm')

#
# Binaries
# These are the sources for the main binaries and test programs. They mostly
# consists of a single source file only and include all the libraries that are
# built as part of kmscon.
#

kmscon_srcs = [
    'seat.c',
    'config.c',
    'image_host.c',
    'image_identity.c',
    'main.c',
    'render_timer.c',
    'terminal.c',
    'terminal_image.c',
    'terminal_sync.c',
    'terminal_vte.c',
]
kmscon_dep = [
    xkbcommon_deps,
    libtsm_deps,
    threads_deps,
    dl_deps,
    shl_deps,
    eloop_deps,
    uterm_deps,
    render_deps,
    font_deps,
    misc_deps,
]
if enable_sd_login
    kmscon_dep += sd_login_deps
endif

# Reads kitty graphics files and decodes PNG and zlib as the session's user,
# started by kmscon per terminal (ADR-103); it links nothing of kmscon's.
kmscon_image_helper = executable(
    'kmscon-image-helper',
    'image_helper.c',
    dependencies: [libpng_deps, zlib_deps],
    install: true,
    install_dir: moduledir,
)

kmscon = executable(
    'kmscon',
    kmscon_srcs,
    dependencies: kmscon_dep,
    export_dynamic: true,
    install: true,
    install_dir: bindir,
)
