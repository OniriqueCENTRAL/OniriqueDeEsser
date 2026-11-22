#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "ui/OniriquePresets.h"

//==============================================================================
//  Les presets d'usine (mode AUTO), rangés par usage. Un réglage non cité reprend sa valeur d'usine.
//  Valeurs en unités du réglage (dB, Hz, %), les cases à cocher à 0 ou 1.
//==============================================================================
namespace solopresets
{
enum Tags { rap = 1, sung = 2, speech = 4, fix = 8 };

inline juce::StringArray tagNames() { return { "Rap", "Sung vocals", "Voice & podcast", "Fix a take" }; }

inline std::vector<onirique::PresetBank::Stock> stockPresets()
{
    using namespace params;
    return {
        { "INIT", 0, {} },
        { "Rap Lead", rap, { { threshold, -32 }, { range, 10 }, { freq, 6000 }, { targetT, 1 } } },
        { "Rap Aggressive", rap, { { threshold, -38 }, { range, 14 }, { freq, 6200 }, { targetT, 1 }, { targetCH, 1 } } },
        { "Soft Singer", sung, { { threshold, -28 }, { range, 7 }, { freq, 5200 }, { clarity, 25 } } },
        { "Bright Pop", sung, { { threshold, -34 }, { range, 9 }, { freq, 7000 }, { clarity, 40 } } },
        { "Airy Female", sung, { { threshold, -33 }, { range, 8 }, { freq, 7800 }, { targetF, 1 }, { clarity, 30 } } },
        { "Deep Male", sung | rap, { { threshold, -30 }, { range, 9 }, { freq, 4600 }, { targetCH, 1 } } },
        { "Podcast Voice", speech, { { threshold, -30 }, { range, 8 }, { freq, 5500 }, { targetCH, 1 } } },
        { "Harsh S Rescue", fix, { { threshold, -42 }, { range, 18 }, { freq, 6500 }, { targetT, 1 }, { targetCH, 1 }, { clarity, 20 } } },
        { "Whistling S", fix, { { threshold, -40 }, { range, 16 }, { freq, 8500 }, { targetF, 1 } } },
    };
}
} // namespace solopresets
