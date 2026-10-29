juce_add_gui_app(MeterBar PRODUCT_NAME "MeterBar" VERSION "1.0.0"
    COMPANY_NAME "MeterBar" BUNDLE_ID "com.meterbar.app"
    ICON_BIG "icon/icon_big.png" ICON_SMALL "icon/icon_small.png")
# v0.16 Anime Mode assets, embedded into the binary: the mascot cutouts
# (transparent-background versions, per Jackson's amendment) and the three
# synthesized UI click WAVs (assets/synth_clicks.py — no external samples).
juce_add_binary_data(MeterBarAssets
    SOURCES
        ../assets/anime-mascot-calm-clear.png
        ../assets/anime-mascot-excited-clear.png
        ../assets/click-pop.wav
        ../assets/click-bloop.wav
        ../assets/click-sparkle.wav)
target_sources(MeterBar PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/ScreenSpace.cpp
    Source/WindowsLoopback.cpp)
target_include_directories(MeterBar PRIVATE Source ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/include)
target_compile_definitions(MeterBar PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_APPLICATION_NAME_STRING="MeterBar"
    JUCE_APPLICATION_VERSION_STRING="1.0.0"
    # NOMINMAX (v0.12.1): windows.h (pulled in by JUCE) defines min/max as
    # macros that mangle every std::min/std::max call site — MSVC C2589.
    # GCC/Linux never sees this; the fix must live here, not per-call-site.
    NOMINMAX)
target_link_libraries(MeterBar PRIVATE
    juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats juce::juce_audio_utils
    meterbar_dsp MeterBarAssets juce::juce_recommended_config_flags)
if(WIN32)
  # shell32 = SHAppBarMessage, comctl32 = SetWindowSubclass,
  # dwmapi = DWMWA_CLOAKED — all for ScreenSpace (v0.18 AppBar/arrange).
  target_link_libraries(MeterBar PRIVATE ole32 mmdevapi shell32 comctl32 dwmapi)
endif()
