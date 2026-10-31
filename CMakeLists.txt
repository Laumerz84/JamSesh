cmake_minimum_required(VERSION 3.22)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT so the .vst3 and the .exe have no runtime DLL dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# macOS: the oldest macOS the app and the plugins run on (read by project(), so set before it). The build is
# for the build machine's processor unless CMAKE_OSX_ARCHITECTURES says otherwise; the release is universal:
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
# (.github/workflows/build-mac.yml builds it that way; packaging/mac/make-dist.sh packages it)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Oldest macOS the app and the plugins run on")

project(ClodScope VERSION 1.3.0)

add_subdirectory(JUCE)

# The editor paints in software so screen sharing works (Source/UI/StreamSafe.h). JUCE 8.0.4's software
# renderer makes each transparency layer (every component with setAlpha < 1) a GPU image on Windows,
# which made dimmed panels take up to a second to paint. JUCE is patched to use software layers there;
# re-apply the patch after updating JUCE if it is still needed.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/modules/juce_graphics/native/juce_RenderingHelpers.h" CLOD_JUCE_RENDERING)
string(FIND "${CLOD_JUCE_RENDERING}" "Clod Scope patch" CLOD_JUCE_PATCHED)
if(CLOD_JUCE_PATCHED EQUAL -1)
    message(WARNING "JUCE is not patched: SoftwareRendererSavedState::beginTransparencyLayer in "
                    "juce_RenderingHelpers.h should create its layer with SoftwareImageType() (see CMakeLists.txt)")
endif()

# Opus (third_party/opus, BSD licence): Jam Sesh's optional audio compression
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
if(APPLE)
    # A universal build compiles each file once per processor, but Opus picks its SIMD code (NEON or SSE) for the
    # build machine's processor when it is configured: plain C in a universal build, so both halves compile.
    list(LENGTH CMAKE_OSX_ARCHITECTURES CLOD_ARCH_COUNT)
    if(CLOD_ARCH_COUNT GREATER 1)
        set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
    else()
        set(OPUS_DISABLE_INTRINSICS OFF CACHE BOOL "" FORCE)
    endif()
endif()
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)

# The formats: the plugin (VST3) and the standalone app everywhere, and on macOS the Audio Unit as well. The Mac
# app's Info.plist gets its icon and the texts of the two permission prompts: the microphone (audio input) and
# the local network (friends on the same network and the router's port mapping, macOS 15 and later).
# The Audio Unit is a plain effect (aufx), so Logic Pro and GarageBand offer it in an audio track's Audio FX
# slots. (A "MIDI-controlled effect" would get MIDI there, but Logic only offers those in an instrument slot,
# fed by a side chain, and GarageBand not at all. The synth follows the audio input without MIDI.)
set(CLOD_FORMATS VST3 Standalone)
set(CLOD_MAC_OPTIONS)
if(APPLE)
    set(CLOD_FORMATS VST3 AU Standalone)
    set(CLOD_MAC_OPTIONS
        AU_MAIN_TYPE kAudioUnitType_Effect
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Source/Resources/JamSesh-1024.png"
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Jam Sesh plays your instrument or voice from the audio input through its effects, and to your friends when you jam."
        PLIST_TO_MERGE "<plist><dict><key>NSLocalNetworkUsageDescription</key><string>Jam Sesh connects to friends on your local network, and asks your router to open a port so friends on the internet can join.</string></dict></plist>")
endif()

# The plugin (VST3, AU) and the standalone app are built from the same code, and all are called Jam Sesh. Until
# 1.4.0 the plugin was "Clod Scope": the bundle ID and the plugin codes stay, so hosts still find it.
juce_add_plugin(ClodScope
    COMPANY_NAME "Clod Codes"
    BUNDLE_ID com.clodcodes.clodscope
    PLUGIN_MANUFACTURER_CODE Cldc
    PLUGIN_CODE Csc1
    FORMATS ${CLOD_FORMATS}
    PRODUCT_NAME "Jam Sesh"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer
    ${CLOD_MAC_OPTIONS})

set(CLOD_APP_NAME "Jam Sesh")
set_target_properties(ClodScope_Standalone PROPERTIES OUTPUT_NAME "${CLOD_APP_NAME}")

if(WIN32)
    # JUCE writes the plugin's Windows version resource (the version in the file properties of the .vst3) once
    # and never rewrites it, so after a version bump the file would still show the old version. A stale
    # resource file is removed here, and the build writes it again.
    get_target_property(CLOD_GENERATED ClodScope JUCE_GENERATED_SOURCES_DIRECTORY)
    get_target_property(CLOD_VERSION ClodScope JUCE_VERSION)
    set(CLOD_RC "${CLOD_GENERATED}/ClodScope_resources.rc")
    if(EXISTS "${CLOD_RC}")
        file(STRINGS "${CLOD_RC}" CLOD_RC_FILEVERSION REGEX "\"FileVersion\"")
        string(FIND "${CLOD_RC_FILEVERSION}" "\"${CLOD_VERSION}\\0\"" CLOD_RC_CURRENT)
        if(CLOD_RC_FILEVERSION AND CLOD_RC_CURRENT EQUAL -1)
            message(STATUS "Version resource is older than ${CLOD_VERSION}: rebuilding it")
            file(REMOVE "${CLOD_RC}")
        endif()
    endif()

    # The standalone app (Jam Sesh.exe, CLOD_APP_NAME in the code) gets its own resources in place of JUCE's: its
    # icon (CLOD_APP_ICON, drawn by
    # tools/icon/make-icon.ps1) and a version resource, so Windows (Task Manager, the Firewall prompt, the
    # volume mixer) shows the app's name too. Like JUCE's, they compile in an object library of their own,
    # without the plugin's compile definitions.
    set(CLOD_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Source/Resources/JamSesh.ico")
    get_target_property(CLOD_APP_LINKS ClodScope_Standalone LINK_LIBRARIES)
    list(REMOVE_ITEM CLOD_APP_LINKS ClodScope_rc_lib)
    set_target_properties(ClodScope_Standalone PROPERTIES LINK_LIBRARIES "${CLOD_APP_LINKS}")
    configure_file(Source/App/AppResources.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ClodScopeApp_resources.rc" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/ClodScopeApp_resources.rc" PROPERTIES OBJECT_DEPENDS "${CLOD_APP_ICON}")
    add_library(ClodScopeApp_rc_lib OBJECT "${CMAKE_CURRENT_BINARY_DIR}/ClodScopeApp_resources.rc")
    target_link_libraries(ClodScope_Standalone PRIVATE ClodScopeApp_rc_lib)
endif()

# Files compiled into the binary: BinaryData::NewRockerRegular_ttf (the JAM SESH title font, SIL OFL)
juce_add_binary_data(ClodScopeData SOURCES Source/Resources/Fonts/NewRocker-Regular.ttf)

set(CLOD_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DoubleSlit.cpp
    Source/SoundEquation.cpp
    Source/Expr/Expression.cpp
    Source/Render3D/Scene3D.cpp
    Source/Render3D/OffscreenGL.cpp
    Source/Render3D/Views3D.cpp
    Source/Orbital/OrbitalModel.cpp
    Source/Orbital/OrbitalView.cpp
    Source/Sculpt/SculptModel.cpp
    Source/Sculpt/SculptView.cpp
    Source/Sculpt/SculptPanel.cpp
    Source/Synth/SynthEngine.cpp
    Source/Synth/Tracker.cpp
    Source/Synth/ChordTracker.cpp
    Source/Synth/SynthVoice.cpp
    Source/Synth/SynthPanel.cpp
    Source/Fx/FxRack.cpp
    Source/Fx/FxAmpCab.cpp
    Source/Fx/FxDynamicsEq.cpp
    Source/Fx/FxModDelay.cpp
    Source/Fx/FxReverb.cpp
    Source/Fx/FxPanels.cpp
    Source/Presets/PresetManager.cpp
    Source/Presets/PresetUI.cpp
    Source/Presets/FactoryPresets.cpp
    Source/UI/ScopeView.cpp
    Source/UI/Themes.cpp
    Source/UI/Surface.cpp
    Source/UI/Editor/EditorParts.cpp
    Source/UI/Editor/ThemePicker.cpp
    Source/Jam/JamSession.cpp
    Source/Jam/JamNet.cpp
    Source/Jam/JamReach.cpp
    Source/Jam/JamClock.cpp
    Source/Jam/JamReplica.cpp
    Source/Jam/JamEngine.cpp
    Source/Jam/JamPort.cpp
    Source/Jam/JamCodec.cpp
    Source/Jam/JamStream.cpp
    Source/Jam/JamView.cpp
    Source/Jam/JamPanel.cpp
    Source/Jam/ViewState.cpp
    Source/Jam/ViewTile.cpp
    Source/App/StandaloneApp.cpp
    Source/App/StandaloneBar.cpp
    Source/App/TrackPlayer.cpp
    Source/App/Recorder.cpp)

target_sources(ClodScope PRIVATE ${CLOD_SOURCES})

# ASIO (Windows): Steinberg's ASIO SDK (third_party/asiosdk), used under its GPLv3 option: Clod Scope and Jam Sesh
# are GPLv3 (LICENSE.txt), and every shared copy comes with its source code (packaging/make-dist.ps1).
if(WIN32)
    target_include_directories(ClodScope PRIVATE third_party/asiosdk/common)
    target_compile_definitions(ClodScope PUBLIC JUCE_ASIO=1)
endif()

target_compile_definitions(ClodScope PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    JUCE_USE_MP3AUDIOFORMAT=1
    "CLOD_APP_NAME=\"${CLOD_APP_NAME}\"")

target_link_libraries(ClodScope
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_opengl
        opus
        ClodScopeData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Headless tools: the processor and editor in a console app, never a window or an audio device.
# Not part of the default build: cmake --build <dir> --target <tool>
#   ClodScopeSnap  paints the editor into PNGs and measures drawing costs (tools/snap)
#   ClodJamTest    Jam Sesh between instances over a simulated network (tools/jam-test)
function(clod_add_tool target product)
    juce_add_console_app(${target} PRODUCT_NAME "${product}")
    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(${target} PRIVATE ${CLOD_SOURCES} ${ARGN})
    target_compile_definitions(${target} PRIVATE
        JucePlugin_Name="Jam Sesh"
        JucePlugin_VersionString="1.0.0"
        "CLOD_APP_NAME=\"${CLOD_APP_NAME}\""
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_opengl
            juce::juce_recommended_config_flags
            opus
            ClodScopeData)
endfunction()

clod_add_tool(ClodScopeSnap "Clod Scope Snap" tools/snap/SnapMain.cpp tools/snap/CpuTime.cpp)
clod_add_tool(ClodJamTest "Clod Jam Test" tools/jam-test/JamTest.cpp tools/jam-test/ReachTest.cpp)

#   ClodLatencyProbe  opens the audio interface with every driver type and prints the latencies it reports
#                     (tools/latency-probe). Takes over the interface for a moment per test.
juce_add_console_app(ClodLatencyProbe PRODUCT_NAME "Clod Latency Probe")
set_target_properties(ClodLatencyProbe PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_sources(ClodLatencyProbe PRIVATE tools/latency-probe/LatencyProbe.cpp)
if(WIN32)
    target_include_directories(ClodLatencyProbe PRIVATE third_party/asiosdk/common)
    target_compile_definitions(ClodLatencyProbe PRIVATE JUCE_ASIO=1)
endif()
target_compile_definitions(ClodLatencyProbe PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(ClodLatencyProbe PRIVATE
    juce::juce_audio_devices
    juce::juce_recommended_config_flags)
