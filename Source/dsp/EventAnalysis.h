#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>
#include "Common.h"

namespace onirique::deess
{
/*  MODE MANUEL : la prise capturée découpée en ÉVÉNEMENTS, chacun son propre son, réglable à la main.

    Trois familles :
      SIBILANCES   S, Z, CH, F : un souffle aigu qui dure (plus de 45 ms) ;
      CONSONNES    P, B (le coup de grave d'une plosive), T, K (une attaque brève, large bande) ;
      RESPIRATIONS un souffle sans hauteur, plus faible que la voix, qui dure (plus de 100 ms).

    Analyse toutes les 2,5 ms (FFT 512 ou 1024 selon la fréquence d'échantillonnage, fenêtre de Hann) :
      - l'énergie par bande : POP 40-180 Hz, CORPS 180-1500, BAS-MÉDIUM 1500-2500, CH 2500-4500, S 4500-9000, AIR 9-16 kHz ;
      - la hauteur (voisement) : autocorrélation normalisée sur 30 ms du son ramené vers 8 kHz, entre 70 et 500 Hz ;
      - le niveau de la voix autour (le plus fort des trames voisées sur ±1,5 s) : chaque événement est mesuré PAR RAPPORT à
        sa phrase (sa « saillie »), le niveau de la prise ne compte pas.
    Un événement porte sa lettre (ce qu'on entend), son niveau et sa saillie ; c'est la saillie qui guide TOUT RÉDUIRE. */

enum Family { sibilance = 0, consonant = 1, breath = 2, numFamilies = 3 };

struct Event
{
    juce::int64 start = 0, end = 0;     // en échantillons, temps de la prise (le temps du DAW)
    int family = sibilance;
    juce::String letter;                // S, Z, CH, F | P, B, T, K | R
    float levelDb = -100.0f;            // crête du son dans l'événement (dBFS)
    float saliencyDb = 0.0f;            // de combien il dépasse ce qu'on attend de sa phrase
    float gainDb = 0.0f;                // le réglage (0 : inchangé)
    bool manual = false;                // réglé à la main : TOUT RÉDUIRE ne le touche plus
    bool muted = false;                 // ignoré (gain non appliqué)
    float rise = 0.0f, fall = 0.0f;     // forme du souffle (bancs de mesure) : montée des aigus, chute au-dessus de 9 kHz
};

class Analyzer
{
public:
    /** Analyse un morceau continu de prise (mono) commençant à 'offset' (échantillons, temps du DAW). */
    static std::vector<Event> run (const float* x, int n, double fs, juce::int64 offset, const std::atomic<bool>* cancel = nullptr)
    {
        std::vector<Event> out;
        if (n < (int) (fs * 0.2)) return out;
        Analyzer a (fs);
        a.measure (x, n, cancel);
        if (cancel != nullptr && cancel->load()) return out;
        a.reference();
        a.findSibilants (out);
        a.findConsonants (out);
        a.findHighBursts (out);
        a.findBreaths (out);
        // l'attaque d'un S ou d'un F n'est pas une consonne : un T, K, P ou B posé sur un souffle aigu est retiré
        {
            std::vector<Event> sib;
            for (const auto& e : out) if (e.family == sibilance) sib.push_back (e);
            const juce::int64 pre = (juce::int64) (fs * 0.012);
            out.erase (std::remove_if (out.begin(), out.end(), [&] (const Event& e)
            {
                if (e.family != consonant) return false;
                for (const auto& q : sib) if (e.end > q.start - pre && e.start < q.end) return true;
                return false;
            }), out.end());
        }
        for (auto& e : out) { e.start += offset; e.end += offset; }
        std::sort (out.begin(), out.end(), [] (const Event& p, const Event& q) { return p.start < q.start; });
        return out;
    }

    // ce qu'il faut pour dessiner un spectre (banc de mesure)
    struct Frame { float total = -120, pop = -120, body = -120, low = -120, ch = -120, s = -120, air = -120, voicing = 0, voiceRef = -120, flux = 0; };
    std::vector<Frame> frames;
    int hop = 110;

    explicit Analyzer (double sampleRate) : fs (sampleRate)
    {
        hop = std::max (32, (int) std::lround (fs * 0.0025));
        order = fs > 64000.0 ? 11 : 10;
        if (fs < 32000.0) order = 9;
        // fenêtre d'environ 21 ms (1024 à 48 kHz)
        size = 1 << order;
    }

    void measure (const float* x, int n, const std::atomic<bool>* cancel)
    {
        juce::dsp::FFT fft (order);
        std::vector<float> win ((size_t) size), buf ((size_t) size * 2);
        for (int i = 0; i < size; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
        const float binHz = (float) fs / (float) size;
        auto bin = [&] (float hz) { return juce::jlimit (1, size / 2 - 1, (int) std::lround (hz / binHz)); };
        const int bPop0 = bin (40), bPop1 = bin (180), bBody1 = bin (1500), bLow1 = bin (2500), bCh1 = bin (4500), bS1 = bin (9000),
                  bAir1 = bin (std::min (16000.0f, (float) fs * 0.45f));

        // le son vers 8 kHz pour la hauteur (passe-bas 3 kHz d'ordre 4 puis décimation)
        const int decim = std::max (1, (int) std::lround (fs / 8000.0));
        std::vector<float> lo ((size_t) (n / decim + 1));
        {
            juce::dsp::IIR::Coefficients<float>::Ptr c1 = juce::dsp::IIR::Coefficients<float>::makeLowPass (fs, 3000.0f, 0.54f),
                                                     c2 = juce::dsp::IIR::Coefficients<float>::makeLowPass (fs, 3000.0f, 1.31f);
            juce::dsp::IIR::Filter<float> f1 (c1), f2 (c2);
            int k = 0;
            for (int i = 0; i < n; ++i) { const float y = f2.processSample (f1.processSample (x[i])); if (i % decim == 0 && k < (int) lo.size()) lo[(size_t) k++] = y; }
            lo.resize ((size_t) k);
        }
        const double fsLo = fs / decim;
        const int acWin = (int) std::lround (0.030 * fsLo), lagMin = (int) std::floor (fsLo / 500.0), lagMax = (int) std::ceil (fsLo / 70.0);

        const int numFrames = std::max (0, (n - size) / hop + 1);
        frames.assign ((size_t) numFrames, {});
        std::vector<float> prevMag ((size_t) size / 2, 0.0f), mag ((size_t) size / 2);
        float lastVoicing = 0.0f;
        for (int f = 0; f < numFrames; ++f)
        {
            if (cancel != nullptr && (f & 1023) == 0 && cancel->load()) return;
            const int p = f * hop;
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < size; ++i) buf[(size_t) i] = x[p + i] * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (buf.data(), true);
            double e[6] {}, tot = 0.0, flux = 0.0;
            for (int b = 1; b < size / 2; ++b)
            {
                const float m = buf[(size_t) b];
                mag[(size_t) b] = m;
                const double pw = (double) m * m;
                tot += pw;
                const float d = m - prevMag[(size_t) b];
                if (d > 0.0f) flux += (double) d * d;
                if (b >= bPop0 && b < bPop1) e[0] += pw;
                else if (b >= bPop1 && b < bBody1) e[1] += pw;
                else if (b >= bBody1 && b < bLow1) e[2] += pw;
                else if (b >= bLow1 && b < bCh1) e[3] += pw;
                else if (b >= bCh1 && b < bS1) e[4] += pw;
                else if (b >= bS1 && b < bAir1) e[5] += pw;
            }
            std::swap (prevMag, mag);
            // la puissance en dBFS (sinus plein = 0 dB) : la fenêtre de Hann rend N/4 d'amplitude pour un sinus
            const double norm = 1.0 / ((double) size * size / 16.0) * 2.0;
            auto db = [norm] (double pw) { return (float) (10.0 * std::log10 (pw * norm + 1.0e-13)); };
            auto& fr = frames[(size_t) f];
            fr.total = db (tot); fr.pop = db (e[0]); fr.body = db (e[1]); fr.low = db (e[2]); fr.ch = db (e[3]); fr.s = db (e[4]); fr.air = db (e[5]);
            fr.flux = (float) std::sqrt (flux / std::max (1.0e-12, tot));

            // la hauteur, une trame sur deux (5 ms)
            if ((f & 1) == 0)
            {
                const int c = (p + size / 2) / decim - acWin / 2;
                float best = 0.0f;
                if (c >= 0 && c + acWin + lagMax < (int) lo.size())
                {
                    const float* s = lo.data() + c;
                    double e0 = 0.0;
                    for (int i = 0; i < acWin; ++i) e0 += (double) s[i] * s[i];
                    if (e0 > 1.0e-9)
                        for (int lag = lagMin; lag <= lagMax; ++lag)
                        {
                            double r = 0.0, e1 = 0.0;
                            for (int i = 0; i < acWin; ++i) { r += (double) s[i] * s[i + lag]; e1 += (double) s[i + lag] * s[i + lag]; }
                            best = std::max (best, (float) (r / std::sqrt (e0 * e1 + 1.0e-12)));
                        }
                }
                lastVoicing = best;
            }
            fr.voicing = lastVoicing;
        }
    }

    // le niveau de la voix autour de chaque trame : le plus fort des trames voisées sur ±1,5 s (moins 3 dB)
    void reference()
    {
        const int nF = (int) frames.size(), w = (int) std::lround (1.5 / (hop / fs));
        std::vector<float> v ((size_t) nF);
        for (int f = 0; f < nF; ++f) v[(size_t) f] = frames[(size_t) f].voicing > 0.55f ? frames[(size_t) f].total : -120.0f;
        std::deque<int> dq;   // maximum glissant
        int right = -1;
        float lastRef = -30.0f;
        for (int f = 0; f < nF; ++f)
        {
            while (right < std::min (nF - 1, f + w)) { ++right; while (! dq.empty() && v[(size_t) dq.back()] <= v[(size_t) right]) dq.pop_back(); dq.push_back (right); }
            while (! dq.empty() && dq.front() < f - w) dq.pop_front();
            const float m = dq.empty() ? -120.0f : v[(size_t) dq.front()];
            if (m > -80.0f) lastRef = m - 3.0f;
            frames[(size_t) f].voiceRef = lastRef;
        }
    }

    juce::int64 toSample (int frame) const { return (juce::int64) frame * hop + size / 2; }

    static float sumDb (float a, float b) { return 10.0f * std::log10 (std::pow (10.0f, a * 0.1f) + std::pow (10.0f, b * 0.1f) + 1.0e-13f); }

    // des trames marquées -> des suites (les trous de moins de 'gap' trames comblés), gardées entre minLen et maxLen
    template <typename Pred>
    std::vector<std::pair<int, int>> runs (Pred pred, int gap, int minLen, int maxLen) const
    {
        std::vector<std::pair<int, int>> r;
        const int nF = (int) frames.size();
        int s = -1, lastOn = -1000;
        for (int f = 0; f <= nF; ++f)
        {
            const bool on = f < nF && pred (f);
            if (on) { if (s < 0) s = f; else if (f - lastOn > gap + 1) { r.push_back ({ s, lastOn + 1 }); s = f; } lastOn = f; }
            else if (s >= 0 && (f - lastOn > gap || f == nF)) { r.push_back ({ s, lastOn + 1 }); s = -1; }
        }
        std::vector<std::pair<int, int>> keep;
        for (auto [a, b] : r) if (b - a >= minLen && b - a <= maxLen) keep.push_back ({ a, b });
        return keep;
    }

    int ms (double m) const { return std::max (1, (int) std::lround (m * 0.001 * fs / hop)); }

    Event make (int a, int b, int family, const juce::String& letter, float bandDb, float padMs) const
    {
        Event e;
        e.family = family; e.letter = letter;
        const auto pad = (juce::int64) std::lround (padMs * 0.001 * fs);
        e.start = std::max<juce::int64> (0, toSample (a) - hop / 2 - pad);
        e.end = toSample (b - 1) + hop / 2 + pad;
        float pk = -120.0f, ref = -120.0f;
        for (int f = a; f < b; ++f) { pk = std::max (pk, frames[(size_t) f].total); ref = std::max (ref, frames[(size_t) f].voiceRef); }
        e.levelDb = pk;
        e.saliencyDb = bandDb - ref;
        return e;
    }

    void findSibilants (std::vector<Event>& out) const
    {
        // un souffle aigu : l'essentiel de l'énergie au-dessus de 2,5 kHz, assez fort par rapport à la phrase
        auto hf = [this] (int f) { const auto& r = frames[(size_t) f]; return sumDb (sumDb (r.ch, r.s), r.air); };
        auto isSib = [&] (int f)
        {
            const auto& r = frames[(size_t) f];
            const float h = hf (f), lowPart = sumDb (r.body, r.low);
            return h > r.voiceRef - 38.0f && h > r.total - 3.5f && h - lowPart > 2.0f;
        };
        for (auto [a, b] : runs (isSib, ms (10), ms (45), ms (450)))
        {
            double ch = 0, s = 0, air = 0, body = 0, vo = 0;
            float bandPk = -120.0f;
            for (int f = a; f < b; ++f)
            {
                const auto& r = frames[(size_t) f];
                ch += std::pow (10.0, r.ch * 0.1); s += std::pow (10.0, r.s * 0.1); air += std::pow (10.0, r.air * 0.1);
                body += std::pow (10.0, r.body * 0.1); vo += r.voicing;
                bandPk = std::max (bandPk, hf (f));
            }
            vo /= (b - a);
            // densités (dB par Hz) : 2,5-4,5 kHz, 4,5-9 kHz, 9-16 kHz
            const double chD = 10.0 * std::log10 (ch / 2000.0 + 1e-20), sD = 10.0 * std::log10 (s / 4500.0 + 1e-20),
                         aD = 10.0 * std::log10 (air / 7000.0 + 1e-20);
            const float rise = (float) (std::max (sD, aD) - chD), fall = (float) (sD - aD);
            juce::String letter = "S";
            const int shape = hissShape (chD, sD, aD);
            if (ch > 1.6 * (s + air) || shape == 1) letter = "CH";
            else if (vo > 0.6 && body > 0.25 * (ch + s + air)) letter = "Z";   // un souffle chanté : Z (avant F, qui est sourd)
            else if (shape == 2 || (air > 1.3 * s && bandPk < frames[(size_t) a].voiceRef - 14.0f)) letter = "F";
            out.push_back (make (a, b, sibilance, letter, bandPk, 6.0f));
            out.back().rise = rise; out.back().fall = fall;
        }
    }

    void findConsonants (std::vector<Event>& out) const
    {
        const int nF = (int) frames.size(), look = ms (15), burstMax = ms (70);
        int f = look;
        while (f < nF - 1)
        {
            const auto& r = frames[(size_t) f];
            float before = 0.0f;   // le plus faible des 15 ms d'avant
            float minPrev = 1.0e9f, minPop = 1.0e9f;
            for (int k = f - look; k < f; ++k) { minPrev = std::min (minPrev, frames[(size_t) k].total); minPop = std::min (minPop, frames[(size_t) k].pop); }
            before = minPrev;
            const bool loud = r.total > r.voiceRef - 30.0f;
            const bool popJump = r.pop - minPop > 14.0f && r.pop > r.total - 6.0f && r.pop > r.voiceRef - 24.0f;
            const bool snap = r.total - before > 16.0f && r.flux > 0.45f;
            if (loud && (popJump || snap))
            {
                // la fin de l'attaque : le niveau retombe de 9 dB sous son pic, ou la voix (voisée) prend le relais
                int e = f + 1;
                float pk = r.total;
                while (e < nF && e - f < burstMax)
                {
                    const auto& q = frames[(size_t) e];
                    pk = std::max (pk, q.total);
                    if (q.total < pk - 9.0f || (e - f > ms (12) && q.voicing > 0.75f)) break;
                    ++e;
                }
                if (e - f >= ms (5))
                {
                    double pop = 0, body = 0, hi = 0, vo = 0;
                    for (int k = f; k < e; ++k)
                    {
                        const auto& q = frames[(size_t) k];
                        pop += std::pow (10.0, q.pop * 0.1); body += std::pow (10.0, q.body * 0.1);
                        hi += std::pow (10.0, q.ch * 0.1) + std::pow (10.0, q.s * 0.1) + std::pow (10.0, q.air * 0.1);
                        vo += q.voicing;
                    }
                    vo /= (e - f);
                    // le début de l'attaque : une consonne sourde n'a pas de hauteur (une voyelle qui démarre, si)
                    float vo0 = 0.0f;
                    const int head = std::min (e, f + ms (8));
                    for (int k = f; k < head; ++k) vo0 += frames[(size_t) k].voicing;
                    vo0 /= (float) std::max (1, head - f);
                    juce::String letter;
                    float band = r.total;
                    // P, B : le coup de grave doit dominer le corps de la voix (8 dB) ; une note chantée qui démarre fait aussi
                    // monter le grave (son fondamental), mais avec ses harmoniques : ce n'est pas une plosive
                    if (popJump && pop > 6.3 * body && pop > hi) { letter = vo0 > 0.6f ? "B" : "P"; band = 10.0f * std::log10 ((float) (pop / (e - f)) + 1.0e-13f) + 6.0f; }
                    else if (vo0 < 0.5f && hi > 0.35 * (pop + body + hi)) letter = "T";
                    else if (vo0 < 0.45f && ! popJump && hi > 0.12 * (pop + body + hi)) letter = "K";
                    if (letter.isNotEmpty())
                    {
                        // un T qui est déjà le début d'une sibilance (« ts ») reste une consonne : les deux se règlent à part
                        out.push_back (make (f, e, consonant, letter, band, 3.0f));
                        f = e + ms (30);
                        continue;
                    }
                }
            }
            ++f;
        }
    }

    // T au milieu d'une phrase (« Ti », « ête ») : un souffle aigu sans hauteur qui surgit d'un coup (12 dB de plus que les 15 ms
    // d'avant), bref (moins de 45 ms : plus long, c'est une sibilance). L'attaque depuis le silence est vue plus haut.
    void findHighBursts (std::vector<Event>& out) const
    {
        const int nF = (int) frames.size(), look = ms (15), longest = ms (45);
        auto hf = [this] (int f) { const auto& r = frames[(size_t) f]; return sumDb (sumDb (r.ch, r.s), r.air); };
        auto taken = [&] (juce::int64 a, juce::int64 b)
        {
            for (const auto& e : out) if (std::min (e.end, b) > std::max (e.start, a)) return true;
            return false;
        };
        int f = look;
        while (f < nF - 1)
        {
            const auto& r = frames[(size_t) f];
            float prev = 1.0e9f;
            for (int k = f - look; k < f; ++k) prev = std::min (prev, hf (k));
            const float h = hf (f);
            if (h - prev > 12.0f && h > r.total - 6.0f && r.voicing < 0.5f && h > r.voiceRef - 35.0f)
            {
                int e = f + 1;
                float pk = h;
                while (e < nF && e - f <= longest)
                {
                    pk = std::max (pk, hf (e));
                    if (hf (e) < pk - 9.0f || frames[(size_t) e].voicing > 0.7f) break;
                    ++e;
                }
                if (e - f >= ms (5) && e - f <= longest && ! taken (toSample (f), toSample (e)))
                {
                    out.push_back (make (f, e, consonant, "T", pk, 3.0f));
                    f = e + ms (30);
                    continue;
                }
            }
            ++f;
        }
    }

    void findBreaths (std::vector<Event>& out) const
    {
        auto isBreath = [this] (int f)
        {
            const auto& r = frames[(size_t) f];
            const float hiShare = sumDb (r.s, r.air) - r.total;
            return r.voicing < 0.45f && r.total < r.voiceRef - 8.0f && r.total > r.voiceRef - 58.0f && r.total > -72.0f && hiShare < -4.0f
                   && sumDb (r.low, r.ch) > r.total - 9.0f;
        };
        for (auto [a, b] : runs (isBreath, ms (25), ms (110), ms (1600)))
        {
            // pas une respiration si une sibilance l'occupe déjà en grande partie
            const auto sa = toSample (a), sb = toSample (b);
            bool taken = false;
            for (const auto& e : out)
                if (e.family == sibilance && std::min (e.end, sb) - std::max (e.start, sa) > (sb - sa) / 2) { taken = true; break; }
            if (taken) continue;
            float pk = -120.0f;
            for (int f = a; f < b; ++f) pk = std::max (pk, frames[(size_t) f].total);
            out.push_back (make (a, b, breath, "R", pk, 12.0f));
        }
    }

private:
    double fs;
    int order = 10, size = 1024;
};
} // namespace onirique::deess
