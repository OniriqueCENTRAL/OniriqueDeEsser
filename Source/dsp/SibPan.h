#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include "Biquad.h"

namespace onirique::deess
{
/*  PAN DES SIBILANCES : déplace seulement les sibilances (S, CH, F) dans la stéréo, de gauche à droite, sans rien
    changer à la détection ni au reste de la voix.

    La voix est coupée en deux à 3 kHz par un filtre Linkwitz-Riley (24 dB/oct) : le bas et le haut se recombinent
    sans creux ni bosse. Pendant une sibilance (gate de 0 à 1, donné par le moteur : la détection en AUTO, les
    événements en MANUEL), le haut de chaque côté reçoit son gain de panoramique à puissance constante (au centre :
    1 et 1 ; tout à droite : 0 à gauche et 1,41 à droite). Hors sibilance, ou au centre, rien ne bouge.
    Le panoramique s'enclenche en fondu dès qu'on le quitte du centre (et se retire au centre) : au centre, le son
    sort exactement comme avant. Une voix mono (une seule voie) ne peut pas être panoramiquée : elle passe telle quelle. */
class SibPan
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const double f = std::min (3000.0, 0.45 * fs);
        for (auto* b : { &lpA, &lpB }) b->setCoeffs (BiquadCoeffs::lowpass (fs, f, 0.7071));
        for (auto* b : { &hpA, &hpB }) b->setCoeffs (BiquadCoeffs::highpass (fs, f, 0.7071));
        engageK = (float) (1.0 - std::exp (-1.0 / (0.030 * fs)));
        panS.reset (fs, 0.02);
        reset();
    }
    void reset()
    {
        for (auto* b : { &lpA, &lpB, &hpA, &hpB }) b->reset();
        engage = std::abs (target) > 0.001f ? 1.0f : 0.0f;
        panS.setCurrentAndTargetValue (target);
    }
    /** -1 (tout à gauche) … 0 (centre) … 1 (tout à droite). */
    void setPan (float p) noexcept { target = juce::jlimit (-1.0f, 1.0f, p); panS.setTargetValue (target); }

    /** Un échantillon de chaque voie ; gate : 1 pendant une sibilance, 0 ailleurs (fondus compris). */
    void process (float* x, int numCh, float gate) noexcept
    {
        const float p = panS.getNextValue();
        engage += engageK * ((std::abs (target) > 0.001f ? 1.0f : 0.0f) - engage);
        if (numCh < 2) return;
        float lo[2], hi[2];
        for (int c = 0; c < 2; ++c)
        {
            lo[c] = lpB.process (lpA.process (x[c], c), c);
            hi[c] = hpB.process (hpA.process (x[c], c), c);
        }
        if (engage < 1.0e-4f) return;   // au centre : le son direct (les filtres tournent pour un retour sans clic)
        const float th = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float g[2] { juce::MathConstants<float>::sqrt2 * std::cos (th), juce::MathConstants<float>::sqrt2 * std::sin (th) };
        for (int c = 0; c < 2; ++c)
        {
            const float y = lo[c] + (1.0f + gate * (g[c] - 1.0f)) * hi[c];
            x[c] += engage * (y - x[c]);
        }
    }

private:
    double fs = 44100.0;
    Biquad lpA, lpB, hpA, hpB;
    float target = 0.0f, engage = 0.0f, engageK = 0.001f;
    juce::SmoothedValue<float> panS;
};
} // namespace onirique::deess
