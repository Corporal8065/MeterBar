#pragma once
#include "State.hpp"
// v0.28.8: close-save comparison — the dirty check behind the exit
// save prompt (Jackson's ruling, 2026-11-03, shipped first in
// SessionBar v0.4.20: prompt on close ONLY when the state changed
// since the app opened; the baseline is the state as opened).
// A preset is the COMPLETE state — every parameter, nothing
// partial — so equality here is exact field-by-field equality over
// the whole PresetState, window geometry included. Floats compare
// exactly on purpose: the baseline and the live state pass through
// the same writers, so an unchanged setting compares equal
// bit-for-bit, and any epsilon would only hide real changes.
// Deliberately NOT compared: the Live Captions settings — they are
// not PresetState fields at all. They live in their own file
// (Documents\MeterBar\Captions\settings.json) and never join
// presets (Jackson's Fork C ruling, 2026-11-03); a captions-only
// change therefore never triggers this prompt, exactly as ruled.
namespace meterbar {
inline bool sameState(const PresetState& a, const PresetState& b) {
    return a.name == b.name
        && a.spectrogramEnabled == b.spectrogramEnabled
        && a.waveformEnabled == b.waveformEnabled
        && a.loudnessEnabled == b.loudnessEnabled
        && a.stereometerEnabled == b.stereometerEnabled
        && a.oscilloscopeEnabled == b.oscilloscopeEnabled
        && a.spectrumEnabled == b.spectrumEnabled
        && a.spectrogramOrientation == b.spectrogramOrientation
        && a.fade == b.fade
        && a.loopEnabled == b.loopEnabled
        && a.linkedSpeed == b.linkedSpeed
        && a.waveCh1 == b.waveCh1
        && a.waveCh2 == b.waveCh2
        && a.waveHistory == b.waveHistory
        && a.loudnessMode == b.loudnessMode
        && a.loudnessShowTargets == b.loudnessShowTargets
        && a.stereoMode == b.stereoMode
        && a.stereoColor == b.stereoColor
        && a.stereoPointAuto == b.stereoPointAuto
        && a.stereoPointSize == b.stereoPointSize
        && a.scopeTrigger == b.scopeTrigger
        && a.scopeDisplay == b.scopeDisplay
        && a.scopeWindowMs == b.scopeWindowMs
        && a.spectrumStyle == b.spectrumStyle
        && a.spectrumSmoothingPct == b.spectrumSmoothingPct
        && a.spectrumTiltDb == b.spectrumTiltDb
        && a.fftSize == b.fftSize
        && a.fftScale == b.fftScale
        && a.spectrumShowReference == b.spectrumShowReference
        && a.spectrumShowTarget == b.spectrumShowTarget
        && a.spectrumColorMode == b.spectrumColorMode
        && a.spectrumDisplayTiltDb == b.spectrumDisplayTiltDb
        && a.spectrumSixthOctave == b.spectrumSixthOctave
        && a.themeName == b.themeName
        && a.colorMap == b.colorMap
        && a.backgroundOpacity == b.backgroundOpacity
        && a.backgroundStyle == b.backgroundStyle
        && a.beatPulseEnabled == b.beatPulseEnabled
        && a.beatTimeSignature == b.beatTimeSignature
        && a.linkEnabled == b.linkEnabled
        && a.linkBackground == b.linkBackground
        && a.visualizerEnabled == b.visualizerEnabled
        && a.visualizerStyle == b.visualizerStyle
        && a.undertowBloomQuarter == b.undertowBloomQuarter
        && a.undertowBloomEighth == b.undertowBloomEighth
        && a.undertowPreArm == b.undertowPreArm
        && a.undertowKickTauMs == b.undertowKickTauMs
        && a.visualizerBaseHue == b.visualizerBaseHue
        && a.visualizerSaturation == b.visualizerSaturation
        && a.visualizerSpread == b.visualizerSpread
        && a.visualizerCycleSec == b.visualizerCycleSec
        && a.animeClickSounds == b.animeClickSounds
        && a.mascotFlex == b.mascotFlex
        && a.audioSource == b.audioSource
        && a.appSource == b.appSource
        && a.pinnedExe == b.pinnedExe
        && a.audioDeviceName == b.audioDeviceName
        && a.outputDeviceName == b.outputDeviceName
        && a.outputDeviceId == b.outputDeviceId
        && a.trimDb == b.trimDb
        && a.routingPair == b.routingPair
        && a.routingCh1 == b.routingCh1
        && a.routingCh2 == b.routingCh2
        && a.vsync == b.vsync
        && a.fpsLimit == b.fpsLimit
        && a.performanceMode == b.performanceMode
        && a.moduleFlex == b.moduleFlex
        && a.pinned == b.pinned
        && a.stick == b.stick
        && a.showToolbar == b.showToolbar
        && a.showRepositionHint == b.showRepositionHint
        && a.reserveScreenSpace == b.reserveScreenSpace
        && a.windowX == b.windowX
        && a.windowY == b.windowY
        && a.windowW == b.windowW
        && a.windowH == b.windowH;
}
inline bool changedSinceOpen(const PresetState& openBaseline, const PresetState& current) {
    return !sameState(openBaseline, current);
}
} // namespace meterbar
