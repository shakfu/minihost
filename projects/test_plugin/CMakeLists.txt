# Deterministic VST3 fixtures for the test suite. Opt-in
# (-DMINIHOST_BUILD_TEST_PLUGIN=ON): nothing that ships depends on them, and
# they cost a JUCE plugin-client compile.
#
# Two targets from one source, because an effect and an instrument exercise
# different host paths (audio in vs. no audio in, latency/sidechain vs. MIDI
# note rendering). See TestPluginProcessor.h.

set(_mh_test_plugin_common
    COMPANY_NAME "minihost"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS VST3)

juce_add_plugin(MinihostTestFx
    ${_mh_test_plugin_common}
    PLUGIN_MANUFACTURER_CODE Mnhs
    PLUGIN_CODE Mtfx
    PRODUCT_NAME "MinihostTestFx")

juce_add_plugin(MinihostTestSynth
    COMPANY_NAME "minihost"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS VST3
    PLUGIN_MANUFACTURER_CODE Mnhs
    PLUGIN_CODE Mtsy
    PRODUCT_NAME "MinihostTestSynth")

foreach(_target MinihostTestFx MinihostTestSynth)
    target_sources(${_target} PRIVATE TestPluginProcessor.cpp)
    # The host links its own copy of JUCE into the same process. Hidden
    # visibility keeps the plugin's copy from being interposed by it.
    set_target_properties(${_target} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_compile_definitions(${_target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        # Otherwise a MIDI-input VST3 exposes 2048 synthetic CC parameters,
        # which buries the three the fixture is about.
        JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0)
    # No juce_recommended_lto_flags: the fixture is built for determinism,
    # not speed, and LTO across two JUCE copies (host and plugin) in one
    # process is not worth the ODR risk.
    target_link_libraries(${_target} PRIVATE
        juce::juce_audio_utils
        PUBLIC
        juce::juce_recommended_config_flags)
endforeach()

target_compile_definitions(MinihostTestSynth PUBLIC MINIHOST_TEST_SYNTH=1)
