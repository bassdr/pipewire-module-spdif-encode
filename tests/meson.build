test_iec61937 = executable(
    'test-iec61937',
    'test-iec61937.cpp',
    dependencies : [catch2_dep],
)
test('iec61937', test_iec61937)

test_encoder_ac3 = executable(
    'test-encoder-ac3',
    'test-encoder-ac3.cpp',
    link_with : encoder_lib,
    dependencies : [catch2_dep, libavcodec_dep, libavutil_dep],
)
test('encoder-ac3', test_encoder_ac3)

test_encoder_dts = executable(
    'test-encoder-dts',
    'test-encoder-dts.cpp',
    link_with : encoder_lib,
    dependencies : [catch2_dep, libavcodec_dep, libavutil_dep],
)
test('encoder-dts', test_encoder_dts)

executable(
    'generate-dts-spdif',
    'generate-dts-spdif.cpp',
    link_with : encoder_lib,
    dependencies : [libavcodec_dep, libavutil_dep],
)
