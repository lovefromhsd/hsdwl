project(
    'hsdwl',
    'c',
    version: '0.1.0',
    license: 'BSD-3-Clause',
    meson_version: '>= 0.60.0',
    default_options: [
        'c_std=c99',
        'warning_level=3',
        'buildtype=debugoptimized',
    ],
)

cc = meson.get_compiler('c')

wlroots = dependency('wlroots-0.20')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
libdrm = dependency('libdrm')
pango = dependency('pango')
pangocairo = dependency('pangocairo')
cairo = dependency('cairo')
pixman = dependency('pixman-1')
libm = cc.find_library('m', required: false)

subdir('src')

hsdwl_inc = include_directories('src')

executable(
    'hsdwl',
    hsdwl_src,
    include_directories: hsdwl_inc,
    dependencies: [
        wlroots,
        wayland_server,
        xkbcommon,
        libdrm,
        pango,
        pangocairo,
        cairo,
        pixman,
        libm,
    ],
    install: true,
)
