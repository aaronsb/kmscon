# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

foreach name, deps : {
    'output': [uterm_deps],
    'vt': [uterm_deps],
    'input': [uterm_deps],
    'key': [],
}
    deps += [
        shl_deps,
        misc_deps,
        xkbcommon_deps,
    ]
    test_name = f'test_@name@'
    exe = executable(
        test_name,
        f'@test_name@.c',
        dependencies: deps,
    )
endforeach

test_shl = executable(
    'test_shl',
    'test_shl.c',
    dependencies: [shl_deps, check_deps],
)
test(
    'test_shl',
    test_shl,
    protocol: 'tap',
    env: {'CK_TAP_LOG_FILE_NAME': '-', 'CK_VERBOSITY': 'silent'},
)

test_text = executable(
    'test_text',
    'test_text.c',
    dependencies: [libtsm_deps, shl_deps],
)
test('test_text', test_text)

# libtsm's test calls make the images a scaled picture evicts
test_bbulk = executable(
    'test_bbulk',
    'test_bbulk.c',
    include_directories: src_inc,
    dependencies: [
        subproject('libtsm').get_variable('libtsm_test_dep'),
        htable_deps,
    ],
)
test('test_bbulk', test_bbulk)

# ADR-103's image budget against real libtsm screens, made through its test
# calls, and terminal_new's vte made into it
test_image_budget = executable(
    'test_image_budget',
    'test_image_budget.c',
    include_directories: [src_inc, misc_inc],
    dependencies: [
        subproject('libtsm').get_variable('libtsm_test_dep'),
        shl_deps,
    ],
)
test('test_image_budget', test_image_budget)

if enable_renderer_gltex
    # GL is stubbed in the test; only its headers are used. The decoration
    # sprites are the real ones.
    test_gltex = executable(
        'test_gltex',
        [
            'test_gltex.c',
            '../src/render/deco.c',
            '../src/render/image_budget.c',
            '../src/render/picture.c',
            embed_gen.process(
                '../src/render/gltex_atlas.vert',
                extra_args: shader_regex,
            ),
            embed_gen.process(
                '../src/render/gltex_atlas.frag',
                extra_args: shader_regex,
            ),
            embed_gen.process(
                '../src/render/gltex_image.vert',
                extra_args: shader_regex,
            ),
            embed_gen.process(
                '../src/render/gltex_image.frag',
                extra_args: shader_regex,
            ),
        ],
        include_directories: src_inc,
        dependencies: [
            subproject('libtsm').get_variable('libtsm_test_dep'),
            shl_deps,
            htable_deps,
            math_deps,
            glesv2_deps.partial_dependency(compile_args: true, includes: true),
        ],
    )
    test('test_gltex', test_gltex)

    # mod-gltex links the render library, so it holds its own copy of
    # image_budget.c; each budget call gltex makes must bind to kmscon's
    test(
        'gltex_budget_symbols',
        python,
        args: [
            files('check_module_symbols.py'),
            kmscon,
            mod_gltex,
            files('../src/render/gltex.c'),
            'image_budget_',
        ],
    )
endif

if enable_video_drm2d
    test_blend = executable(
        'test_blend',
        'test_blend.c',
        include_directories: src_inc,
        dependencies: [libdrm_deps, shl_deps, htable_deps],
    )
    test('test_blend', test_blend)

    # a DRM mode's refresh rate, which bounds coalescing (ADR-201 step 6)
    test_drm_refresh = executable(
        'test_drm_refresh',
        'test_drm_refresh.c',
        include_directories: src_inc,
        dependencies: [libdrm_deps],
    )
    test('test_drm_refresh', test_drm_refresh)
endif

if enable_video_fbdev
    # fbdev's per-depth blend against the single loop it replaced (fbdev_ref.h)
    test_fbdev_blend = executable(
        'test_fbdev_blend',
        'test_fbdev_blend.c',
        include_directories: [src_inc, include_directories('../src/video')],
        dependencies: [shl_deps, htable_deps],
    )
    test('test_fbdev_blend', test_fbdev_blend)

    # the same, timed; run by make bench-fbdev, not a test
    executable(
        'bench_fbdev',
        'bench_fbdev.c',
        include_directories: [src_inc, include_directories('../src/video')],
        dependencies: [shl_deps, htable_deps, math_deps],
    )
endif

test_asciinema = executable(
    'test_asciinema',
    'test_asciinema.c',
    '../src/misc/asciinema.c',
    include_directories: misc_inc,
    dependencies: [libtsm_deps, shl_deps, eloop_deps],
)
test('test_asciinema', test_asciinema)

# the pty holds the output its input callback leaves (ADR-103 step 3d)
test_pty = executable(
    'test_pty',
    'test_pty.c',
    include_directories: misc_inc,
    dependencies: [libtsm_deps, shl_deps, eloop_deps],
)
test('test_pty', test_pty)

# The image host, its helper and the helper's user: a build with images
# only (ADR-103, The capability probes)
if enable_images
    # kmscon-image-helper over its socket, with no libtsm (ADR-103): the real one,
    # and a build with stops between each check and the step it guards
    test_image_helper_bin = executable(
        'test_image_helper_bin',
        '../src/image_helper.c',
        c_args: '-DIMAGE_HELPER_TEST',
        include_directories: src_inc,
        dependencies: [libpng_deps, zlib_deps],
    )
    test_image_helper = executable(
        'test_image_helper',
        'test_image_helper.c',
        include_directories: src_inc,
        dependencies: [libpng_deps, zlib_deps],
    )
    test(
        'test_image_helper',
        test_image_helper,
        args: [
            kmscon_image_helper,
            test_image_helper_bin,
            meson.current_build_dir(),
        ],
    )

    # kmscon's side of the helper: the real one, and a stand-in whose replies
    # each break one of kmscon's checks
    fake_image_helper = executable(
        'fake_image_helper',
        'fake_image_helper.c',
        include_directories: src_inc,
    )
    test_image_host = executable(
        'test_image_host',
        'test_image_host.c',
        include_directories: src_inc,
        dependencies: [shl_deps, eloop_deps],
    )
    test(
        'test_image_host',
        test_image_host,
        args: [
            kmscon_image_helper,
            fake_image_helper,
            meson.current_build_dir(),
        ],
        timeout: 60,
    )

    # a reply memfd shrunk between kmscon's checks: fcntl is wrapped to hold the
    # window open while the stand-in shrinks and seals it
    test_image_host_race = executable(
        'test_image_host_race',
        'test_image_host_race.c',
        '../src/image_host.c',
        include_directories: src_inc,
        link_args: ['-Wl,--wrap=fcntl', '-Wl,--wrap=fcntl64'],
        dependencies: [shl_deps, eloop_deps],
    )
    test(
        'test_image_host_race',
        test_image_host_race,
        args: [fake_image_helper, meson.current_build_dir()],
    )

    # a terminal's kitty graphics host end to end: a pseudo-terminal into kmscon's
    # pty and a real vte, served by the real helper or the stand-in; the
    # session's user stood in for
    test_terminal_image = executable(
        'test_terminal_image',
        'test_terminal_image.c',
        '../src/terminal_image.c',
        '../src/terminal_vte.c',
        '../src/image_host.c',
        '../src/render/image_budget.c',
        '../src/misc/asciinema.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            subproject('libtsm').get_variable('libtsm_test_dep'),
            shl_deps,
            eloop_deps,
            zlib_deps,
        ],
    )
    test(
        'test_terminal_image',
        test_terminal_image,
        args: [kmscon_image_helper, fake_image_helper],
        timeout: 60,
    )

    # the helper's user, built without sd-login and with it, its calls stood in for
    test_image_identity = executable(
        'test_image_identity',
        'test_image_identity.c',
        '../src/image_identity.c',
        c_args: '-DIMAGE_IDENTITY_SD_LOGIN=0',
        include_directories: src_inc,
        dependencies: [shl_deps],
    )
    test('test_image_identity', test_image_identity)
    if sd_login_deps.found()
        test_image_identity_sd_login = executable(
            'test_image_identity_sd_login',
            'test_image_identity.c',
            '../src/image_identity.c',
            c_args: '-DIMAGE_IDENTITY_SD_LOGIN=1',
            include_directories: src_inc,
            dependencies: [
                shl_deps,
                sd_login_deps.partial_dependency(
                    compile_args: true,
                    includes: true,
                ),
            ],
        )
        test('test_image_identity_sd_login', test_image_identity_sd_login)
    endif
else
    # a terminal with no image host, its asciicast playback among it
    test_terminal_image_none = executable(
        'test_terminal_image_none',
        'test_terminal_image_none.c',
        '../src/terminal_image_none.c',
        '../src/terminal_vte.c',
        '../src/render/image_budget.c',
        '../src/misc/asciinema.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            subproject('libtsm').get_variable('libtsm_test_dep'),
            shl_deps,
            eloop_deps,
        ],
    )
    test('test_terminal_image_none', test_terminal_image_none, timeout: 60)
endif

# the vte kmscon makes from its settings, and the input modes terminal.c
# asks it about (DECARM, focus, the pointer), in every build
test_terminal_vte = executable(
    'test_terminal_vte',
    'test_terminal_vte.c',
    include_directories: [src_inc, misc_inc],
    dependencies: [
        subproject('libtsm').get_variable('libtsm_test_dep'),
        shl_deps,
    ],
)
test('test_terminal_vte', test_terminal_vte)

test_issue = executable(
    'test_issue',
    'test_issue.c',
    '../src/misc/issue.c',
    '../src/misc/issue_network.c',
    include_directories: misc_inc,
    dependencies: [libtsm_deps, shl_deps],
)
test('test_issue', test_issue)

test_issue_parameter = executable(
    'test_issue_parameter',
    'test_issue_parameter.c',
    '../src/misc/issue.c',
    '../src/misc/issue_network.c',
    include_directories: misc_inc,
    dependencies: [libtsm_deps, shl_deps],
)
test('test_issue_parameter', test_issue_parameter)

if enable_font_freetype
    test_font_freetype = executable(
        'test_font_freetype',
        'test_font_freetype.c',
        include_directories: src_inc,
        c_args: '-DTEST_FONT_DIR="@0@"'.format(
            meson.current_source_dir() / 'fonts',
        ),
        dependencies: [
            libtsm_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            threads_deps,
            shl_deps,
        ],
    )
    test('test_font_freetype', test_font_freetype)
endif

test_render_timer = executable(
    'test_render_timer',
    'test_render_timer.c',
    '../src/render_timer.c',
    include_directories: src_inc,
)
test('test_render_timer', test_render_timer)

# synchronized output's present rules (ADR-201 step 5) on a real vte and a fake clock
test_terminal_sync = executable(
    'test_terminal_sync',
    'test_terminal_sync.c',
    '../src/terminal_sync.c',
    include_directories: src_inc,
    dependencies: [libtsm_deps],
)
test('test_terminal_sync', test_terminal_sync)

# Braille, sextant and octant sprites against tests/box_model.h, at every cell
# size to 32x64 (#76)
test_font_box = executable(
    'test_font_box',
    'test_font_box.c',
    include_directories: src_inc,
    dependencies: [libtsm_deps, shl_deps, math_deps],
)
test('test_font_box', test_font_box, timeout: 300)

# mod-psf against crafted fonts, plain and gzipped, and against every console
# font kbd installs on the build machine (#229); and on a plain PSF2 fixture
# and on it gzipped, which a build without zlib refuses (ADR-103, The
# zlib-free variant)
if enable_font_psf
    test_font_psf = executable(
        'test_font_psf',
        'test_font_psf.c',
        include_directories: [src_inc, include_directories('../src/font')],
        dependencies: [shl_deps, psf_gz_deps],
    )
    test(
        'test_font_psf',
        test_font_psf,
        args: [
            meson.current_source_dir() / 'fonts',
            enable_zlib ? 'gz' : 'no-gz',
        ],
        timeout: 120,
    )
endif

test_conf = executable(
    'test_conf',
    'test_conf.c',
    include_directories: [src_inc, misc_inc],
    dependencies: [libtsm_deps, shl_deps],
)
test('test_conf', test_conf)


python = find_program('python3')

# Capability probes (tests/capabilities/README.md): one small check per
# terminal feature, judged against the works/missing list beside the runner.
test_capabilities = executable(
    'test_capabilities',
    'test_capabilities.c',
    dependencies: [libtsm_deps],
)
# sixel, kitty graphics and sixel's modes through kmscon's own configuration
# and vte, judged against the build's images option (-Dimages in the list)
test_capabilities_images = executable(
    'test_capabilities_images',
    'test_capabilities_images.c',
    include_directories: [src_inc, misc_inc],
    dependencies: [libtsm_deps, shl_deps],
)
caps_args = [
    files('capabilities/run.py'),
    '--list',
    files('capabilities/capabilities.list'),
    '--probe',
    test_capabilities,
    '--image-probe',
    test_capabilities_images,
    '--build',
    'images=' + (enable_images ? 'true' : 'false'),
]
if enable_font_freetype
    test_capabilities_font = executable(
        'test_capabilities_font',
        'test_capabilities_font.c',
        include_directories: src_inc,
        dependencies: [
            libtsm_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            threads_deps,
            shl_deps,
        ],
    )
    caps_args += [
        '--font-probe',
        test_capabilities_font,
        '--fonts',
        meson.current_source_dir() / 'fonts',
    ]
endif
infocmp = find_program('infocmp', required: false)
if tic.found() and infocmp.found()
    caps_args += [
        '--terminfo',
        meson.project_source_root() / 'scripts/terminfo/kmscon.ti',
        '--tic',
        tic.full_path(),
        '--infocmp',
        infocmp.full_path(),
    ]
    tput = find_program('tput', required: false)
    if tput.found()
        caps_args += ['--tput', tput.full_path()]
    endif
    # the XTGETTCAP table (src/terminal_termcap.h) against the entry it is
    # generated from: a change to kmscon.ti is followed by make termcap
    test(
        'termcap_drift',
        python,
        args: [
            files('../scripts/terminfo/gen_termcap.py'),
            '--tic',
            tic.full_path(),
            '--infocmp',
            infocmp.full_path(),
            '--check',
            meson.project_source_root() / 'scripts/terminfo/kmscon.ti',
            meson.project_source_root() / 'src/terminal_termcap.h',
        ],
    )
endif
test('capabilities', python, args: caps_args)

if enable_font_freetype and enable_video_drm2d
    # ADR-103's test calls for images build only into what links this
    libtsm_test_deps = subproject('libtsm').get_variable('libtsm_test_dep')
    test_pixels = executable(
        'test_pixels',
        'test_pixels.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            libtsm_test_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            libdrm_deps,
            threads_deps,
            math_deps,
            shl_deps,
            htable_deps,
        ],
    )
    # tier 2 judges the default build, whose sixel and kitty screens need
    # images; a build without them changes no drawing path (ADR-103, Tests)
    if enable_images
        test(
            'tier2_pixels',
            python,
            args: [
                files('tier2/run.py'),
                '--render',
                test_pixels,
                '--fonts',
                meson.current_source_dir() / 'fonts',
                '--goldens',
                meson.current_source_dir() / 'goldens/tier2',
                '--out',
                meson.project_build_root() / 'tier2',
                '--report',
                meson.project_build_root() / 'tier2-report.json',
                '--root',
                meson.project_source_root(),
            ],
        )
    else
        test(
            'tier2_pixels',
            python,
            args: [
                files('tier2/run.py'),
                '--skip',
                'tier 2 runs on a build with images (ADR-103)',
            ],
        )
    endif

    test_blink = executable(
        'test_blink',
        'test_blink.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            libtsm_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            libdrm_deps,
            threads_deps,
            math_deps,
            shl_deps,
            htable_deps,
        ],
    )
    test('test_blink', test_blink, args: [meson.current_source_dir() / 'fonts'])

    test_present = executable(
        'test_present',
        'test_present.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            libtsm_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            libdrm_deps,
            threads_deps,
            math_deps,
            shl_deps,
            htable_deps,
        ],
    )
    test(
        'test_present',
        test_present,
        args: [meson.current_source_dir() / 'fonts'],
    )

    # coalescing without mode 2026 (ADR-201 step 6), on a fake clock and a real pty
    test_coalesce = executable(
        'test_coalesce',
        'test_coalesce.c',
        '../src/terminal_sync.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            libtsm_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            libdrm_deps,
            threads_deps,
            math_deps,
            shl_deps,
            htable_deps,
        ],
    )
    test(
        'test_coalesce',
        test_coalesce,
        args: [meson.current_source_dir() / 'fonts'],
        timeout: 120,
    )

    # ADR-103's S1 to S6 drawn into memory; run by make bench-compose, not a test
    executable(
        'bench_compose',
        'bench_compose.c',
        include_directories: [src_inc, misc_inc],
        dependencies: [
            libtsm_test_deps,
            freetype_deps,
            fontconfig_deps,
            harfbuzz_deps,
            harfbuzz_raster_deps,
            libdrm_deps,
            threads_deps,
            math_deps,
            shl_deps,
            htable_deps,
        ],
    )
else
    # Visible as SKIP, not a silent absence, when the renderer pieces are off.
    test(
        'tier2_pixels',
        python,
        args: [
            files('tier2/run.py'),
            '--skip',
            'tier 2 needs the freetype font backend and the drm2d video backend',
        ],
    )
endif

# The local VT demo's feedback (#47): demo.sh driven through scripted keys on a
# pty, then the file it records and what fork/demo/feedback.sh prints.
test(
    'demo_feedback',
    python,
    args: [files('demo/feedback.py'), '--root', meson.project_source_root()],
)

# The VT demo's sixel pages drawn below demo.sh's header and on tier 3's
# clear screen, with stand-ins for chafa and mpv: the same placement in both,
# and no picture over text where the page keeps them apart.
test(
    'demo_sixel_placement',
    python,
    args: [
        files('demo/sixel_placement.py'),
        '--root',
        meson.project_source_root(),
    ],
)

# make vt-demo's script (fork/demo/vt-demo.sh): the VT it picks, its refusals,
# and the summary of the run it started, against a stand-in run-vt.sh.
# run-vt.sh's login watch polls once a second, and its checks wait through
# about 25 polls (10 failing in a row for the warning alone).
test(
    'demo_vt_select',
    python,
    args: [files('demo/vt_demo.py'), '--root', meson.project_source_root()],
    timeout: 120,
)

# The kitty graphics player of the acceptance runs
# (fork/tools/kitty-player/kitty.py, #177): its layout and moves, and its
# output on a model of the spec's image store, alone and on a pty with
# stand-ins for ffmpeg and ffprobe.
test(
    'kitty_player',
    python,
    args: [
        files('tools/kitty_player.py'),
        '--root',
        meson.project_source_root(),
    ],
    timeout: 120,
)

# The harness's own Python: the run and comparison page generators
# (make sheet-selftest) and tier 3's image compare and screen modules
# (make tier3-selftest), so meson test and CI run them too.
foreach name, script : {
    'sheet_runpage': 'fork/conformance/test_runpage.py',
    'sheet_compare': 'fork/conformance/test_compare.py',
    'tier3_t3img': 'fork/tier3/test_t3img.py',
    'tier3_t3': 'fork/tier3/test_t3.py',
}
    test(
        name,
        python,
        args: ['-I', '-B', meson.project_source_root() / script],
        workdir: meson.project_source_root(),
    )
endforeach
