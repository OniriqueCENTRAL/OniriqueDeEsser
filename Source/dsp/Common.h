#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace onirique
{
// Détection NaN/Inf par les bits de l'exposant : fonctionne même avec -ffast-math.
inline bool isBadFloat (float x) noexcept
{
    std::uint32_t u;
    std::memcpy (&u, &x, sizeof (u));
    return (u & 0x7f800000u) == 0x7f800000u;
}

// Remplace NaN/Inf par 0 et borne à +/-lim (~ +30 dB au-dessus de 0 dBFS par défaut).
inline float sanitize (float x, float lim = 32.0f) noexcept
{
    if (isBadFloat (x)) return 0.0f;
    return x > lim ? lim : (x < -lim ? -lim : x);
}

inline float dbToGain (float db) noexcept { return std::exp (db * 0.11512925465f); }
inline float gainToDb (float g) noexcept  { return 8.68588963807f * std::log (g); }

// fondu en S quintique : 0 -> 1 sans angle (dérivées première et seconde nulles aux deux bouts). Appliqué à un fondu linéaire
// 0..1, il ne laisse aucune trace dans l'aigu (un fondu linéaire, avec ses deux angles, laissait un résidu vers -75 dB).
inline float sCurve (float t) noexcept
{
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * t * (t * (6.0f * t - 15.0f) + 10.0f);
}

// Ligne à retard simple (délai entier), allocation uniquement dans prepare().
class DelayLine
{
public:
    void prepare (int delaySamples)
    {
        len = delaySamples;
        buf.assign ((size_t) std::max (1, delaySamples), 0.0f);
        idx = 0;
    }

    void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); idx = 0; }

    // longueur réglable dans la capacité allouée par prepare() (remet la ligne à zéro)
    void setLength (int delaySamples) noexcept
    {
        len = std::clamp (delaySamples, 0, (int) buf.size());
        reset();
    }

    inline float process (float in) noexcept
    {
        if (len <= 0) return in;
        const float out = buf[(size_t) idx];
        buf[(size_t) idx] = in;
        if (++idx >= len) idx = 0;
        return out;
    }

private:
    std::vector<float> buf;
    int len = 0, idx = 0;
};
// La forme d'un souffle aigu : S, CH ou F. Réglé sur des exemples nommés (S, CH, F, T d'une vraie voix) :
//   CH : plat de 2,5 à 9 kHz puis il retombe (rien ne monte vers l'aigu) ;
//   S  : monte fort vers l'aigu (+10 dB et plus entre 2,5-4,5 kHz et le haut) ;
//   F  : monte moins que le S, diffus, et retombe un peu au-dessus de 9 kHz.
// Les entrées sont des densités (dB par Hz) : 2,5-4,5 kHz, 4,5-9 kHz, 9-16 kHz. Renvoie 0 S, 1 CH, 2 F.
inline int hissShape (double chD, double sD, double aD) noexcept
{
    const double rise = std::max (sD, aD) - chD, fall = sD - aD;
    if (rise < 2.0 && fall > 3.0) return 1;
    if (rise >= 4.0 && rise < 10.0 && fall >= 1.5 && fall <= 6.0) return 2;
    return 0;
}
} // namespace onirique
