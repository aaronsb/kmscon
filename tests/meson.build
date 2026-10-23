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

test_bbulk = executable(
    'test_bbulk',
    'test_bbulk.c',
    include_directories: src_inc,
    dependencies: [libtsm_deps, htable_deps],
)
test('test_bbulk', test_bbulk)

# ADR-103's image budget against real libtsm screens, made through its test calls
test_image_budget = executable(
    'test_image_budget',
    'test_image_budget.c',
    include_directories: src_inc,
    dependencies: [subproject('libtsm').get_variable('libtsm_test_dep')],
)
test('test_image_budget', test_image_budget)

if enable_renderer_gltex
    # GL is stubbed in the test; only its headers are used.
    test_gltex = executable(
        'test_gltex',
        [
            'test_gltex.c',
            embed_gen.process(
                '../src/render/gltex_atlas.vert',
                extra_args: shader_regex,
            ),
            embed_gen.process(
                '../src/render/gltex_atlas.frag',
                extra_args: shader_regex,
            ),
        ],
        include_directories: src_inc,
        dependencies: [
            libtsm_deps,
            shl_deps,
            htable_deps,
            glesv2_deps.partial_dependency(compile_args: true, includes: true),
        ],
    )
    test('test_gltex', test_gltex)
endif

if enable_video_drm2d
    test_blend = executable(
        'test_blend',
        'test_blend.c',
        include_directories: src_inc,
        dependencies: [libdrm_deps, shl_deps, htable_deps],
    )
    test('test_blend', test_blend)
endif

test_asciinema = executable(
    'test_asciinema',
    'test_asciinema.c',
    '../src/misc/asciinema.c',
    include_directories: misc_inc,
    dependencies: [libtsm_deps, shl_deps, eloop_deps],
)
test('test_asciinema', test_asciinema)

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

# Braille, sextant and octant sprites against tests/box_model.h, at every cell
# size to 32x64 (#76)
test_font_box = executable(
    'test_font_box',
    'test_font_box.c',
    include_directories: src_inc,
    dependencies: [libtsm_deps, shl_deps, math_deps],
)
test('test_font_box', test_font_box, timeout: 300)

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
caps_args = [
    files('capabilities/run.py'),
    '--list',
    files('capabilities/capabilities.list'),
    '--probe',
    test_capabilities,
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

# make vt-demo's script (fork/demo/vt-demo.sh): the VT it picks, its refusals,
# and the summary of the run it started, against a stand-in run-vt.sh.
test(
    'demo_vt_select',
    python,
    args: [files('demo/vt_demo.py'), '--root', meson.project_source_root()],
)

# The Bad Apple!! showcase (fork/showcase/bad-apple.sh, #75): its act table,
# its plan and refusals without a video or a terminal, and fx.py's helpers.
test(
    'showcase',
    python,
    args: [files('showcase/showcase.py'), '--root', meson.project_source_root()],
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
