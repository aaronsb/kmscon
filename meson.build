# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

project(
    'kmscon',
    'c',
    version: '10.1.0',
    license: 'MIT',
    # meson 0.58: f-string
    # meson 0.62: dependency libdl
    meson_version: '>=1.1',
    default_options: [
        'warning_level=1',
        'werror=true',
        'buildtype=debugoptimized',
        'c_std=gnu99',
    ],
)

add_project_arguments(
    '-ffast-math',
    '-fno-strict-aliasing',
    '-ffunction-sections',
    '-fdata-sections',
    '-fstack-protector',
    '-fvisibility=hidden',
    '-D_GNU_SOURCE',
    '-D_POSIX_C_SOURCE=200809L',
    language: 'c',
)

#
# Directory structure
#
prefix = get_option('prefix')
bindir = get_option('bindir')
datadir = get_option('datadir')
sysconfdir = get_option('sysconfdir') / meson.project_name()
libexecdir = get_option('libexecdir') / meson.project_name()
mandir = get_option('mandir')
moduledir = get_option('libdir') / meson.project_name()

systemd_deps = dependency('systemd', required: false)
systemdsystemunitdir = systemd_deps.get_variable(
    'systemdsystemunitdir',
    default_value: 'lib/systemd/system',
)
# kmscon-image, the image helper's user before login (ADR-103)
sysusersdir = systemd_deps.get_variable(
    'sysusersdir',
    default_value: 'lib/sysusers.d',
)

#
# Required dependencies
#
xkbcommon_deps = dependency('xkbcommon', version: '>=0.5.0')
libudev_deps = dependency('libudev', version: '>=172')
dl_deps = dependency('dl')
threads_deps = dependency('threads')
math_deps = meson.get_compiler('c').find_library('m', required: false)
# Upstream requires zlib in every build; the fork makes it a feature, so a
# build can leave it out (ADR-103, The zlib-free variant; ADR-303)
zlib_deps = dependency('zlib', disabler: true, required: get_option('zlib'))
# mod-psf reads gzip fonts when zlib is found (ADR-103)
psf_gz_deps = dependency('', required: false)
if zlib_deps.found()
    psf_gz_deps = declare_dependency(
        dependencies: zlib_deps,
        compile_args: '-DKMSCON_PSF_GZ=1',
    )
endif
# genunifont runs on the build machine and needs zlib there, so unifont also
# needs the build machine's zlib: auto leaves unifont out when a cross build's
# build machine lacks it, and nothing is looked up without unifont (ADR-103)
zlib_deps_native = dependency('', required: false)
if zlib_deps.found() and not get_option('font_unifont').disabled()
    zlib_deps_native = dependency(
        'zlib',
        native: true,
        required: get_option('font_unifont'),
    )
endif

python = find_program('python3')

#
# Dependencies
# These are controlled by feature options.
# When the feature is disabled, the corresponding dependency is not searched,
# instad, a disabler object is produced to disable anything uses it.
# This also allows early reporting when a feature is enabled but its dependencis
# are not found.
#
require_glesv2 = get_option('video_drm3d').enabled() or get_option(
    'renderer_gltex',
).enabled()
require_libdrm = get_option('video_drm2d').enabled() or get_option('video_drm3d').enabled()

libseat_deps = dependency(
    'libseat',
    disabler: true,
    required: get_option('libseat'),
)
libdrm_deps = dependency('libdrm', disabler: true, required: require_libdrm)
gbm_deps = dependency('gbm', disabler: true, required: get_option('video_drm3d'))
egl_deps = dependency('egl', disabler: true, required: get_option('video_drm3d'))
glesv2_deps = dependency('glesv2', disabler: true, required: require_glesv2)
pango_deps = dependency(
    'pangoft2',
    disabler: true,
    required: get_option('font_pango'),
)
# Both font backends draw colour glyphs through FreeType, fontconfig and
# HarfBuzz (src/font/font_colour.c, ADR-102), so either one requires them.
require_ft = get_option('font_freetype').enabled() or get_option('font_pango').enabled()
freetype_deps = dependency(
    'freetype2',
    disabler: true,
    required: require_ft,
)
fontconfig_deps = dependency(
    'fontconfig',
    version: '>=2.16.0',  # FcConfigSetFontSetFilter()
    disabler: true,
    required: require_ft,
)
# The freetype backend shapes multi-codepoint clusters with HarfBuzz (ADR-102)
harfbuzz_deps = dependency(
    'harfbuzz',
    disabler: true,
    required: require_ft,
)
# HarfBuzz's rasteriser paints COLRv1 glyphs; its API is stable from 14.2.0.
# Without it, COLRv1 glyphs draw as masks.
harfbuzz_raster_deps = dependency(
    'harfbuzz-raster',
    version: '>=14.2.0',
    required: false,
)
if harfbuzz_raster_deps.found()
    harfbuzz_raster_deps = declare_dependency(
        dependencies: harfbuzz_raster_deps,
        compile_args: '-DKMSCON_HB_RASTER=1',
    )
endif
dbus_deps = dependency('dbus-1', disabler: true, required: get_option('dbus'))
# Sixel and the kitty graphics protocol (ADR-103): kmscon-image-helper decodes
# PNG and zlib as the session's user
libpng_deps = dependency(
    'libpng',
    disabler: true,
    required: get_option('images'),
)
# The same value as the feature loop's enable_images below, which comes too
# late for the sd_login lookup; the loop fails an enabled images that lacks
# libpng or zlib, naming the one it lacks
images_allowed = (
    not get_option('images').disabled()
    and libpng_deps.found()
    and zlib_deps.found()
)
# The image helper's user through logind (ADR-103): libsystemd's sd-login, or
# libelogind's, which provides the same <systemd/sd-login.h>. Not looked for
# in a build without images, where -Dsd_login=enabled fails the configure.
sd_login_deps = dependency('', required: false)
if get_option('sd_login').enabled() and not images_allowed
    error(
        'sd_login finds the image helper\'s user, and this build has no images',
    )
endif
if images_allowed and not get_option('sd_login').disabled()
    sd_login_deps = dependency('libsystemd', required: false)
    if not sd_login_deps.found()
        sd_login_deps = dependency('libelogind', required: false)
    endif
endif
xsltproc = find_program(
    'xsltproc',
    native: true,
    disabler: true,
    required: get_option('docs'),
)
check_deps = dependency('check', disabler: true, required: get_option('tests'))
tic = find_program('tic', required: false)

#
# Handle feature options
# This combines feature option inputs with dependency requirements to derive the
# final set of values that will determine what gets built.
# This will also store the information in a configuration header.
#
sections = {
    'video': 'Video Backends',
    'font': 'Font Backends',
    'renderer': 'Renderers',
}
config = configuration_data()
# Note: keep this in sync with the dependencies above. Each feature names the
# dependencies it needs, so a feature turned on without one says which.
foreach name, reqs : {
    'libseat': {'libseat': libseat_deps},
    'video_fbdev': {},
    'video_drm2d': {'libdrm': libdrm_deps},
    'video_drm3d': {
        'libdrm': libdrm_deps,
        'gbm': gbm_deps,
        'egl': egl_deps,
        'glesv2': glesv2_deps,
    },
    'renderer_gltex': {'glesv2': glesv2_deps},
    'font_psf': {},
    'font_unifont': {
        'zlib': zlib_deps,
        'zlib for the build machine': zlib_deps_native,
    },
    'font_freetype': {
        'freetype2': freetype_deps,
        'fontconfig': fontconfig_deps,
        'harfbuzz': harfbuzz_deps,
    },
    'font_pango': {
        'pangoft2': pango_deps,
        'freetype2': freetype_deps,
        'fontconfig': fontconfig_deps,
        'harfbuzz': harfbuzz_deps,
    },
    'dbus': {'dbus-1': dbus_deps},
    'zlib': {'zlib': zlib_deps},
    'images': {'libpng': libpng_deps, 'zlib': zlib_deps},
    'sd_login': {'libsystemd or libelogind': sd_login_deps},
}
    # found() is false on a disabler, the lookup that was not found
    missing = []
    foreach dep_name, req : reqs
        if not req.found()
            missing += dep_name
        endif
    endforeach
    enabled = get_option(name).require(
        missing.length() == 0,
        error_message: f'@name@ needs ' + ', '.join(missing),
    ).allowed()
    upper_name = name.to_upper()
    # set a variable for later use
    set_variable(f'enable_@name@', enabled)
    # set in config header for code use
    config.set(f'BUILD_ENABLE_@upper_name@', enabled)
    # summary output
    summary(
        name,
        enabled,
        section: sections.get(name.split('_')[0], 'Miscellaneous'),
    )
endforeach

# libtsm builds its sixel decoder and kitty parser only for a kmscon with
# images (ADR-103, libtsm's images option). default_options reach only a
# libtsm built here, and only at its first configure: a system libtsm ignores
# them (a build that needs libtsm without images sets
# force_fallback_for=libtsm), and `meson configure -Dimages=...` on an
# existing build leaves libtsm's setting as it was, so it is read back here.
libtsm_deps = dependency(
    'libtsm',
    version: '>=4.8.0',
    allow_fallback: true,
    default_options: ['images=' + (enable_images ? 'true' : 'false')],
)
if libtsm_deps.type_name() == 'internal'
    libtsm_images = subproject('libtsm').get_variable('images')
    if libtsm_images != enable_images
        error(
            f'images is @enable_images@ and libtsm\'s images is @libtsm_images@:',
            f'pass -Dlibtsm:images=@enable_images@ or set up a new build directory',
        )
    endif
endif

config.set('BUILD_ENABLE_DEBUG', get_option('extra_debug'))
config.set_quoted('BUILD_MODULE_DIR', prefix / moduledir)
config.set_quoted('BUILD_CONFIG_DIR', prefix / sysconfdir)

# Default $TERM value
config.set_quoted('BUILD_DEFAULT_TERM', get_option('term'))

# The version XTVERSION reports (terminal_vte.c)
config.set_quoted('BUILD_VERSION', meson.project_version())

# Make all files include "config.h" by default. This shouldn't cause any
# problems and we cannot forget to include it anymore.
config_h = configure_file(configuration: config, output: 'config.h')
abs_config_h = meson.current_build_dir() / '@0@'.format('config.h')
add_project_arguments('-include', abs_config_h, language: 'c')

#
# Miscellaneous checks
# Not directly related to dependencies but
#
manpages_stylesheet = 'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl'
have_manpages_stylesheet = false
if xsltproc.found()
    have_manpages_stylesheet = run_command(
        xsltproc,
        '--nonet',
        manpages_stylesheet,
        check: false,
    ).returncode() == 0
    if not have_manpages_stylesheet
        docs_error_string = 'docbook.xsl stylesheet was not found.'
    else
        docs_error_string = ''
    endif
else
    docs_error_string = 'executable xsltproc was not found.'
endif
enable_docs = get_option('docs').require(
    xsltproc.found() and have_manpages_stylesheet,
    error_message: docs_error_string,
).allowed()

# Additionally check if glesv2 is needed
enable_glesv2 = enable_video_drm3d or enable_renderer_gltex

#
# Other configuration output
# Show configuration to the user so they can check whether everything was
# configured as expected.
#
summary(
    {
        'prefix': prefix,
        'libexecdir': libexecdir,
        'bindir': bindir,
        'sysconfdir': sysconfdir,
        'moduledir': moduledir,
        'mandir': mandir,
        'systemdsystemunitdir': systemdsystemunitdir,
        'sysusersdir': sysusersdir,
    },
    section: 'Directories',
)
summary(
    {
        'extra_debug': get_option('extra_debug'),
        'tests': get_option('tests'),
        'docs': enable_docs,
    },
    section: 'Miscellaneous',
)

#
# Putting code together
#
subdir('src')

if get_option('tests')
    subdir('tests')
endif
if enable_docs
    subdir('docs')
endif

#
# Process other files
#
dirs_info = configuration_data()
dirs_info.set('libexecdir', prefix / libexecdir)
dirs_info.set('bindir', prefix / bindir)
if enable_libseat
    dirs_info.set('PAM_CFG', 'User=root\nPAMName=kmscon')
else
    dirs_info.set('PAM_CFG', '')
endif
foreach filename, kwargs : {
    'scripts/kmscon-launch-gui.sh': {
        'install_dir': bindir,
        'install_mode': 'rwxr-xr-x',
    },
    'scripts/systemd/kmscon.service.in': {
        'install_dir': systemdsystemunitdir,
        'install_mode': 'rw-r--r--',
    },
    'scripts/systemd/kmsconvt@.service.in': {
        'install_dir': systemdsystemunitdir,
        'install_mode': 'rw-r--r--',
    },
}
    install_data(
        configure_file(
            input: filename,
            output: '@BASENAME@',
            configuration: dirs_info,
        ),
        kwargs: kwargs,
    )
endforeach

# The image helper's user before login: a system user with home / and no
# login shell, created by systemd-sysusers (ADR-103, The image helper)
if enable_images
    install_data(
        'scripts/sysusers.d/kmscon-image.conf',
        install_dir: sysusersdir,
        install_mode: 'rw-r--r--',
    )
endif

# kmscon.conf
install_data(
    'scripts/etc/kmscon.conf.example',
    install_dir: join_paths(get_option('sysconfdir'), 'kmscon'),
)
if enable_libseat
    install_data(
        'scripts/pam/kmscon',
        install_dir: prefix / get_option('libdir') / 'pam.d',
        rename: 'kmscon',
    )
endif

# Terminfo
terminfo_src = files('scripts/terminfo/kmscon.ti')

if tic.found()
    # Schedule the script to run during 'meson install'
    meson.add_install_script(
        'scripts/terminfo/build_terminfo.py',
        prefix / datadir,
        terminfo_src,
    )
else
    warning('The "tic" compiler was not found. Terminfo will not be installed.')
endif
