#include "CaptionSettings.h"

juce::File CaptionSettings::settingsFile() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("MeterBar").getChildFile("Captions")
        .getChildFile("settings.json");
}

CaptionSettingsData CaptionSettings::load() {
    CaptionSettingsData d;
    const auto f = settingsFile();
    if (!f.existsAsFile()) return d;
    const auto v = juce::JSON::parse(f.loadFileAsString());
    if (!v.isObject()) return d;
    const auto modeName = v.getProperty("mode", "off").toString();
    if (modeName == "transcribe") d.mode = meterbar::CaptionMode::Transcribe;
    else if (modeName == "translate") d.mode = meterbar::CaptionMode::Translate;
    d.alwaysOnTop = (bool)v.getProperty("alwaysOnTop", true);
    if (v.getProperty("presentation", "overlay").toString() == "docked")
        d.presentation = 1;
    if (v.hasProperty("x") && v.hasProperty("y") && v.hasProperty("w")) {
        d.hasPosition = true;
        d.x = (int)v.getProperty("x", 0);
        d.y = (int)v.getProperty("y", 0);
        d.w = (int)v.getProperty("w", 900);
    }
    return d;
}

void CaptionSettings::save(const CaptionSettingsData& d) {
    const auto f = settingsFile();
    f.getParentDirectory().createDirectory();
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("schema", 1);
    obj->setProperty("mode", juce::String(meterbar::captionModeName(d.mode)));
    obj->setProperty("alwaysOnTop", d.alwaysOnTop);
    obj->setProperty("presentation", d.presentation == 1 ? "docked" : "overlay");
    if (d.hasPosition) {
        obj->setProperty("x", d.x);
        obj->setProperty("y", d.y);
        obj->setProperty("w", d.w);
    }
    f.replaceWithText(juce::JSON::toString(juce::var(obj), true));
}
