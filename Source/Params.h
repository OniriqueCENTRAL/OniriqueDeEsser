#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
//  Les réglages d'ONIRIQUE DE-ESSER.
//    AUTO    le DE-ESSER de VOCALBOX, réglé par un THRESHOLD : S (à la fréquence FREQ), T, CH, F cochés ; RANGE limite la
//            réduction, CLARITY rend de l'éclat après coup, LISTEN fait entendre seulement ce qui est retiré.
//    MANUEL  la prise capturée, découpée en événements réglés un par un (les réglages de chaque événement sont dans l'état
//            du plugin, pas en paramètres) ; TOUT RÉDUIRE (une dose par famille) les règle d'un coup.
//==============================================================================
namespace params
{
inline const juce::String mode = "mode", threshold = "threshold", range = "range", freq = "freq",
                          targetS = "targetS", targetT = "targetT", targetCH = "targetCH", targetF = "targetF", targetK = "targetK", live = "autoLive",
                          clarity = "clarity", listen = "listen",
                          tameSib = "tameSibilance", tameCons = "tameConsonant", tameBreath = "tameBreath",
                          bypass = "bypass";

inline juce::AudioProcessorValueTreeState::ParameterLayout layout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto dbText = [] (float v, int) { v = std::round (v * 10.0f) / 10.0f + 0.0f; return String (v, 1) + " dB"; };
    auto pct = [] (float v, int) { return String (roundToInt (v)) + " %"; };
    auto hz = [] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 1) + " kHz" : String (roundToInt (v)) + " Hz"; };

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { mode, 1 }, "Mode", StringArray { "Auto", "Manual" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { threshold, 1 }, "Threshold", NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -32.0f,
                                                        Attr().withLabel ("dB").withStringFromValueFunction (dbText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { range, 1 }, "Range", NormalisableRange<float> (0.0f, 24.0f, 0.1f), 10.0f,
                                                        Attr().withLabel ("dB").withStringFromValueFunction (dbText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { freq, 1 }, "Freq", NormalisableRange<float> (3000.0f, 10000.0f, 1.0f, 0.5f), 5500.0f,
                                                        Attr().withLabel ("Hz").withStringFromValueFunction (hz)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { targetS, 1 }, "S", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { targetT, 1 }, "T", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { targetCH, 1 }, "CH", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { targetF, 1 }, "F", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { targetK, 2 }, "K", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { live, 2 }, "Live", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { clarity, 1 }, "Clarity", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                        Attr().withLabel ("%").withStringFromValueFunction (pct)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { listen, 1 }, "Listen", false));
    for (auto [id, name] : { std::pair { tameSib, "Reduce All Sibilance" }, { tameCons, "Reduce All Consonants" }, { tameBreath, "Reduce All Breaths" } })
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                            Attr().withLabel ("%").withStringFromValueFunction (pct).withAutomatable (false)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { bypass, 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

/** Les réglages de session : un preset les laisse (ce qu'on écoute, le mode, les doses du mode manuel, propres à la prise). */
inline juce::StringArray notStored() { return { bypass, listen, mode, tameSib, tameCons, tameBreath }; }
} // namespace params
