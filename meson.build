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

wlroots = dependency('wlroots-0.20')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

executable(
    'hsdwl',
    'main.c',
    dependencies: [
        wlroots,
        wayland_server,
        xkbcommon,
    ],
    install: true,
)
