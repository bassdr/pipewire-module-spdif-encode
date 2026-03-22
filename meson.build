project(
    'pipewire-module-spdif-encode',
    'cpp',
    version : '0.1.0',
    default_options : [
        'cpp_std=c++23',
        'cpp_eh=none',
        'cpp_rtti=false',
        'warning_level=2',
    ],
    license : 'MIT',
)

pipewire_dep = dependency('libpipewire-0.3', version : '>= 0.3.0')
libavcodec_dep = dependency('libavcodec')
libavutil_dep = dependency('libavutil')

pipewire_moduledir = pipewire_dep.get_variable(pkgconfig : 'moduledir')

# Encoder library (shared between module and tests)
encoder_lib = static_library(
    'encoder',
    'src/encoder-ac3.cpp',
    dependencies : [libavcodec_dep, libavutil_dep],
)

shared_library(
    'pipewire-module-spdif-encode',
    'src/module-spdif-encode.cpp',
    link_with : encoder_lib,
    dependencies : [pipewire_dep, libavcodec_dep, libavutil_dep],
    install : true,
    install_dir : pipewire_moduledir,
    name_prefix : '',
)

if get_option('tests')
    subdir('tests')
endif
