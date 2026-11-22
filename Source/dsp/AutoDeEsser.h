#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <array>
#include "Biquad.h"
#include "Common.h"

namespace onirique::deess
{
/*  MODE AUTO : le DE-ESSER de VOCALBOX (bande divisée, anticipation 0,7 ms, action en parallèle sur la seule zone sifflante :
    y = x + (g - 1) * bande), réglé ici par un THRESHOLD comme un de-esser classique.

    Un détecteur par son, en série : S (à la fréquence FREQ), T (vers 4,2 kHz), CH (vers 3,2 kHz), F (le souffle aigu vers
    9 kHz). Chacun mesure le niveau de SA bande (passe-bande étroit, crête suivie) ; au-dessus du THRESHOLD, il baisse sa
    zone (passe-bande large centré un peu au-dessus) : pente ≈ 3:1, genou doux de 6 dB, au plus RANGE dB.
    Une garde relative évite de toucher aux voyelles fortes : la bande doit peser nettement plus que d'habitude dans cette
    voix (le principe de VOCALBOX : un S, c'est une bande qui domine, pas seulement une bande forte).

    Ce que l'interface lit, toutes les 10 ms (colonnes d'un anneau) : le niveau d'entrée, de sortie, le niveau détecté
    (le plus haut des sons cochés), la réduction et QUEL SON a été baissé (S, T, CH ou F) : la vue AUTO écrit la lettre
    au-dessus de chaque retrait. */

enum Target { tS = 0, tT, tCH, tF, numTargets };

class Band
{
public:
    void prepare (double sampleRate, int lookahead)
    {
        fs = sampleRate;
        for (int c = 0; c < 2; ++c) { dx[c].prepare (lookahead); d1[c].prepare (lookahead); }
        bandAtk = coef (0.2); bandRel = coef (12.0);
        fullAtk = coef (3.0); fullRel = coef (40.0);
        redAtk = coef (0.5); redRel = coef (35.0);
        avgK = coef (2000.0);
        reset();
        setFreq (5500.0f);
    }
    void reset()
    {
        env = red = 0.0f; envFull = 1.0f; shareAvg = 0.1f;
        bpDet.reset(); bpAct.reset();
        for (auto& d : dx) d.reset();
        for (auto& d : d1) d.reset();
    }
    void setFreq (float f)
    {
        if (std::abs (f - freq) < 0.5f) return;
        freq = f;
        bpDet.setCoeffs (BiquadCoeffs::bandpass (fs, f, 1.4));
        bpAct.setCoeffs (BiquadCoeffs::bandpass (fs, std::min (f * 1.25, 0.45 * fs), 0.65));
    }

    // un échantillon de chaque canal ; renvoie la réduction (dB) appliquée
    float process (float* x, int numCh, float thrDb, float rangeDb, bool on) noexcept
    {
        float band = 0.0f, full = 0.0f, b[2] { 0.0f, 0.0f };
        for (int c = 0; c < numCh; ++c)
        {
            band = std::max (band, std::abs (bpDet.process (x[c], c)));
            b[c] = bpAct.process (x[c], c);
            full = std::max (full, std::abs (x[c]));
        }
        env += (band > env ? bandAtk : bandRel) * (band - env);
        envFull += (full > envFull ? fullAtk : fullRel) * (full - envFull);
        levelDb = 20.0f * std::log10 (env + 1.0e-7f) + 6.0f;   // un passe-bande étroit perd ~6 dB d'un souffle large
        // la part habituelle de la bande (moyenne lente, seulement quand il y a de la voix)
        const float share = env / (envFull + 1.0e-6f);
        if (envFull > 0.003f) shareAvg += avgK * (std::min (share, 1.0f) - shareAvg);

        float target = 0.0f;
        if (on && rangeDb > 0.01f)
        {
            constexpr float knee = 6.0f, slope = 0.67f;
            const float over = levelDb - thrDb;
            if (over > -0.5f * knee)
            {
                target = over < 0.5f * knee ? slope * (over + 0.5f * knee) * (over + 0.5f * knee) / (2.0f * knee) : slope * over;
                // garde relative : la bande doit peser nettement plus que d'habitude dans CETTE voix (une voix brillante
                // a beaucoup d'aigu partout, ses voyelles ne doivent pas être prises pour des S) : 12 points au-dessus
                // de sa part habituelle, 19 % au moins ; plein effet 15 points plus haut
                const float floor = std::max (0.19f, shareAvg + 0.12f);
                target *= juce::jlimit (0.0f, 1.0f, (share - floor) / 0.15f);
                target = std::min (target, rangeDb);
            }
        }
        red += (target > red ? redAtk : redRel) * (target - red);
        const float gm = red > 1.0e-4f ? std::exp (-red * 0.11512925f) - 1.0f : 0.0f;
        for (int c = 0; c < numCh; ++c)
        {
            const float xd = dx[c].process (x[c]), bd = d1[c].process (b[c]);
            x[c] = xd + gm * bd;
        }
        return red;
    }
    float levelDb = -140.0f;

private:
    float coef (double ms) const { return (float) (1.0 - std::exp (-1.0 / (ms * 0.001 * fs))); }
    double fs = 44100.0;
    float freq = -1.0f, env = 0, envFull = 1, red = 0, shareAvg = 0.1f, avgK = 0.0f;
    float bandAtk = 0, bandRel = 0, fullAtk = 0, fullRel = 0, redAtk = 0, redRel = 0;
    Biquad bpDet, bpAct;
    DelayLine dx[2], d1[2];
};

/** Une colonne de l'historique (10 ms). */
struct Column
{
    float inDb = -120.0f, outDb = -120.0f, detDb = -120.0f, redDb = 0.0f;
    int who = -1;    // le son le plus baissé dans la colonne (Target), -1 : rien
};

class AutoDeEsser
{
public:
    static constexpr int historySize = 2048;   // ~20 s

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        look = std::max (1, (int) std::lround (0.0007 * fs));
        for (auto& b : bands) b.prepare (fs, look);
        dry[0].prepare (latency()); dry[1].prepare (latency());
        clar.reset (fs, 0.05);
        colLen = std::max (16, (int) std::lround (fs * 0.01));
        shelfDb = 0.0f; duckS = 1.0f; shelf.reset();
        listenS.reset (fs, 0.03);
        colCount = 0; colIn = colOut = colDet = colRed = 0.0f; for (auto& w : colWho) w = 0.0f;
    }
    int latency() const noexcept { return look * numTargets; }

    struct Settings { float threshold = -30, range = 10, freq = 5500, clarity = 0; bool on[numTargets] { true, false, false, false }; bool listen = false; };

    void process (float* const* ch, int numCh, int n, const Settings& s) noexcept
    {
        bands[tS].setFreq (s.freq);
        bands[tT].setFreq (4200.0f);
        bands[tCH].setFreq (3200.0f);
        bands[tF].setFreq (9000.0f);
        clar.setTargetValue (s.clarity * 0.01f);
        listenS.setTargetValue (s.listen ? 1.0f : 0.0f);
        float x[2], red[numTargets];
        for (int i = 0; i < n; ++i)
        {
            float in = 0.0f;
            for (int c = 0; c < numCh; ++c) { x[c] = ch[c][i]; in = std::max (in, std::abs (x[c])); }
            float det = -140.0f, worst = 0.0f;
            int who = -1;
            for (int t = 0; t < numTargets; ++t)
            {
                red[t] = bands[t].process (x, numCh, s.threshold, s.range, s.on[t]);
                if (s.on[t]) det = std::max (det, bands[t].levelDb);
                if (red[t] > worst) { worst = red[t]; who = t; }
            }
            // CLARITY : une étagère douce dès 3 kHz, qui s'efface pendant qu'un son est baissé (l'éclat revient, pas les S)
            const float c = clar.getNextValue();
            const float duck = 1.0f - juce::jlimit (0.0f, 1.0f, worst / 6.0f);
            duckS += 0.002f * (duck - duckS);
            const float want = 4.0f * c * duckS;
            if (std::abs (want - shelfDb) > 0.03f || (want < 0.01f && shelfDb != 0.0f))
            {
                shelfDb = want < 0.01f ? 0.0f : want;
                if (shelfDb != 0.0f) shelf.setCoeffs (BiquadCoeffs::highShelf (fs, std::min (3000.0, 0.45 * fs), 0.6, shelfDb));
            }
            const float lm = listenS.getNextValue();
            float out = 0.0f;
            for (int cc = 0; cc < numCh; ++cc)
            {
                const float d = dry[cc].process (ch[cc][i]);
                float y = shelfDb != 0.0f ? shelf.process (x[cc], cc) : x[cc];
                if (lm > 0.0f) y += lm * ((d - x[cc]) - y);   // LISTEN : seulement ce qui est retiré
                ch[cc][i] = y;
                out = std::max (out, std::abs (x[cc]));
            }
            // l'historique
            colIn = std::max (colIn, in); colOut = std::max (colOut, out); colDet = std::max (colDet, det); colRed = std::max (colRed, worst);
            if (who >= 0) colWho[who] += red[who];
            if (++colCount >= colLen) pushColumn();
        }
    }

    // l'interface : les colonnes écrites depuis la dernière lecture
    int writeIndex() const noexcept { return written.load (std::memory_order_acquire); }
    Column column (int index) const noexcept { return ring[(size_t) (index & (historySize - 1))]; }
    float consumeReduction() noexcept { return meter.exchange (0.0f); }

private:
    void pushColumn() noexcept
    {
        Column c;
        auto db = [] (float a) { return 20.0f * std::log10 (a + 1.0e-7f); };
        c.inDb = db (colIn); c.outDb = db (colOut); c.detDb = colDet; c.redDb = colRed;
        float best = 0.3f;   // moins de 0,3 dB : rien de retiré
        for (int t = 0; t < numTargets; ++t) if (colWho[t] / (float) colLen > best) { best = colWho[t] / (float) colLen; c.who = t; }
        const int w = written.load (std::memory_order_relaxed);
        ring[(size_t) (w & (historySize - 1))] = c;
        written.store (w + 1, std::memory_order_release);
        meter.store (std::max (meter.load (std::memory_order_relaxed), colRed), std::memory_order_relaxed);
        colCount = 0; colIn = colOut = colRed = 0.0f; colDet = -140.0f;
        for (auto& v : colWho) v = 0.0f;
    }

    double fs = 44100.0;
    int look = 31, colLen = 441, colCount = 0;
    Band bands[numTargets];
    DelayLine dry[2];
    Biquad shelf;
    float shelfDb = 0.0f, duckS = 1.0f;
    juce::SmoothedValue<float> clar, listenS;
    float colIn = 0, colOut = 0, colDet = -140, colRed = 0, colWho[numTargets] {};
    std::array<Column, historySize> ring {};
    std::atomic<int> written { 0 };
    std::atomic<float> meter { 0.0f };
};
} // namespace onirique::deess
