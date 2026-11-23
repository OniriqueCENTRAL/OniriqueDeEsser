#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <array>
#include "Biquad.h"
#include "Common.h"
#include "SibPan.h"

namespace onirique::deess
{
/*  MODE AUTO : le DE-ESSER de VOCALBOX (bande divisée, anticipation 0,7 ms, action en parallèle sur la seule zone sifflante :
    y = x + (g - 1) * bande), réglé ici par un THRESHOLD comme un de-esser classique.

    Un détecteur par son, en série : S (à la fréquence FREQ), T (vers 4,2 kHz), CH (vers 3,2 kHz), F (le souffle aigu vers
    9 kHz), K (vers 4 kHz). Chaque détecteur ne touche QUE son son : un tri (hissWeights) écoute 30 ms d'avance, dit s'il
    s'agit d'un S, d'un CH, d'un F, d'un T ou d'un K (une attaque qui retombe, ou un souffle qui tient, et sa forme), et un
    détecteur coché n'agit que sur le sien (F coché seul ne baisse plus un S qui siffle vers 9 kHz). LIVE (pour enregistrer
    à travers) : sans ces 30 ms, le tri juge sur les premières ms du son, un peu moins précis. Chacun mesure le niveau de SA bande (passe-bande étroit, crête suivie) ; au-dessus du THRESHOLD, il baisse sa
    zone (passe-bande large centré un peu au-dessus) : pente ≈ 3:1, genou doux de 6 dB, au plus RANGE dB.
    Une garde relative évite de toucher aux voyelles fortes : la bande doit peser nettement plus que d'habitude dans cette
    voix (le principe de VOCALBOX : un S, c'est une bande qui domine, pas seulement une bande forte).

    Ce que l'interface lit, toutes les 10 ms (colonnes d'un anneau) : le niveau d'entrée, de sortie, le niveau détecté
    (le plus haut des sons cochés), la réduction et QUEL SON a été baissé (S, T, CH ou F) : la vue AUTO écrit la lettre
    au-dessus de chaque retrait. */

enum Target { tS = 0, tT, tCH, tF, tK, numTargets };

class Band
{
public:
    void prepare (double sampleRate, int lookahead)
    {
        fs = sampleRate;
        for (int c = 0; c < 2; ++c) { dx[c].prepare (lookahead); d1[c].prepare (lookahead); }
        bandAtk = coef (0.2); bandRel = coef (12.0);
        fullAtk = coef (3.0); fullRel = coef (40.0);
        redAtk = coef (0.5); redRel = coef (35.0); redQuick = coef (3.0);
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
    float process (float* x, int numCh, float thrDb, float rangeDb, bool on, float weight = 1.0f) noexcept
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

        // la détection seule (0 à 1), quel que soit RANGE : sert au PAN des sibilances
        detect = 0.0f;
        if (on)
        {
            const float floorD = std::max (0.19f, shareAvg + 0.12f);
            detect = juce::jlimit (0.0f, 1.0f, (levelDb - thrDb + 3.0f) / 6.0f) * juce::jlimit (0.0f, 1.0f, (share - floorD) / 0.15f);
        }
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
                target = std::min (target * weight, rangeDb);
            }
        }
        // ce n'est plus son son (le tri est passé à un autre) : il lâche tout de suite, sans traîner sur le son d'à côté
        red += (target > red ? redAtk : (weight < 0.5f ? redQuick : redRel)) * (target - red);
        const float gm = red > 1.0e-4f ? std::exp (-red * 0.11512925f) - 1.0f : 0.0f;
        for (int c = 0; c < numCh; ++c)
        {
            const float xd = dx[c].process (x[c]), bd = d1[c].process (b[c]);
            x[c] = xd + gm * bd;
        }
        return red;
    }
    float levelDb = -140.0f, detect = 0.0f;

private:
    float coef (double ms) const { return (float) (1.0 - std::exp (-1.0 / (ms * 0.001 * fs))); }
    double fs = 44100.0;
    float freq = -1.0f, env = 0, envFull = 1, red = 0, shareAvg = 0.1f, avgK = 0.0f;
    float bandAtk = 0, bandRel = 0, fullAtk = 0, fullRel = 0, redAtk = 0, redRel = 0, redQuick = 0;
    Biquad bpDet, bpAct;
    DelayLine dx[2], d1[2];
};

/** Une colonne de l'historique (10 ms). */
struct Column
{
    float inDb = -120.0f, outDb = -120.0f, detDb = -120.0f, redDb = 0.0f;
    int who = -1;    // le son le plus baissé dans la colonne (Target), -1 : rien
    float eCh = 0.0f, eS = 0.0f, eAir = 0.0f, eAll = 0.0f;   // densités (puissance par Hz) 2,5-4,5 / 4,5-9 / 9-16 kHz : la forme du son (affichage)
};

/** La lettre d'un retrait (des colonnes à la suite) : T ou K si c'est surtout un détecteur d'attaque qui a baissé, sinon la forme du
    souffle (hissShape) dit S, CH ou F, quel que soit le détecteur qui a agi. Seul compte le cœur du retrait (les colonnes
    les plus baissées et les plus riches en aigu), pas la queue du relâchement ni la voyelle d'après. Affichage seulement. */
inline int soundOf (const Column* c, int n, float* riseOut = nullptr, float* fallOut = nullptr, int* coreOut = nullptr) noexcept
{
    if (n <= 0) return tS;
    int tCols = 0;
    float maxRed = 0.0f; double maxHf = 0.0;
    auto hf = [] (const Column& k) { return (double) k.eCh * 2000.0 + (double) k.eS * 4500.0 + (double) k.eAir * 7000.0; };
    double bS = 0.0, bAir = 0.0;
    for (int i = 0; i < n; ++i)
    {
        tCols += (c[i].who == tT || c[i].who == tK) ? 1 : 0; maxRed = std::max (maxRed, c[i].redDb); maxHf = std::max (maxHf, hf (c[i]));
        bS += c[i].eS; bAir += c[i].eAir;
    }
    // une attaque (T ou K) : le K retombe au-dessus de 9 kHz, le T monte jusque dans l'extrême aigu (exemples de Nov)
    if (tCols * 2 > n) return 10.0 * std::log10 (bS + 1.0e-30) - 10.0 * std::log10 (bAir + 1.0e-30) > 7.0 ? tK : tT;
    double e[3] {}, all = 0.0, hot = 0.0;
    int core = 0, whoCount[numTargets] {};
    for (int i = 0; i < n; ++i)
    {
        if (c[i].who >= 0) whoCount[c[i].who]++;
        if (c[i].redDb >= 0.5f * maxRed && hf (c[i]) >= 0.25 * maxHf)
        { e[0] += c[i].eCh; e[1] += c[i].eS; e[2] += c[i].eAir; all += c[i].eAll; hot += hf (c[i]); ++core; }
    }
    auto d = [] (double v) { return 10.0 * std::log10 (v + 1.0e-30); };
    if (riseOut != nullptr) *riseOut = (float) (std::max (d (e[1]), d (e[2])) - d (e[0]));
    if (fallOut != nullptr) *fallOut = (float) (d (e[1]) - d (e[2]));
    if (coreOut != nullptr) *coreOut = core;
    // pas un souffle (une voyelle brillante baissée) : la lettre du détecteur qui a agi, comme avant
    if (hot < 0.35 * all)
    {
        int best = tS;
        for (int t = 0; t < numTargets; ++t) if (whoCount[t] > whoCount[best]) best = t;
        return best;
    }
    const int k = hissShape (d (e[0]), d (e[1]), d (e[2]));
    return k == 1 ? tCH : (k == 2 ? tF : tS);
}

class AutoDeEsser
{
public:
    static constexpr int historySize = 2048;   // ~20 s

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        look = std::max (1, (int) std::lround (0.0007 * fs));
        ahead = std::max (1, (int) std::lround (aheadSec * fs));
        pre[0].prepare (ahead); pre[1].prepare (ahead);
        dryLive[0].prepare (latencyLive()); dryLive[1].prepare (latencyLive());
        // l'anticipation totale reste celle des quatre détecteurs d'origine (4 x 0,7 ms) : la latence ne change pas
        for (int t = 0; t < numTargets; ++t) bands[t].prepare (fs, std::max (1, (4 * look) / numTargets + (t < (4 * look) % numTargets ? 1 : 0)));
        dry[0].prepare (latency()); dry[1].prepare (latency());
        clar.reset (fs, 0.05);
        colLen = std::max (16, (int) std::lround (fs * 0.01));
        shelfDb = 0.0f; duckS = 1.0f; shelf.reset();
        listenS.reset (fs, 0.03);
        colCount = 0; colIn = colOut = colDet = colRed = 0.0f; for (auto& w : colWho) w = 0.0f;
        // la forme du son, pour la lettre affichée : trois bandes raides (passe-haut et passe-bas doublés, 24 dB/oct)
        const double top = std::min (16000.0, 0.45 * fs);
        const double edges[3][2] { { 2500.0, 4500.0 }, { 4500.0, 9000.0 }, { 9000.0, top } };
        for (int k = 0; k < 3; ++k)
        {
            shape[k][0].setCoeffs (BiquadCoeffs::highpass (fs, edges[k][0], 0.7071)); shape[k][1].setCoeffs (BiquadCoeffs::highpass (fs, edges[k][0], 0.7071));
            shape[k][2].setCoeffs (BiquadCoeffs::lowpass (fs, edges[k][1], 0.7071));  shape[k][3].setCoeffs (BiquadCoeffs::lowpass (fs, edges[k][1], 0.7071));
            for (auto& b : shape[k]) b.reset();
        }
        shapeW[0] = 2000.0; shapeW[1] = 4500.0; shapeW[2] = std::max (1000.0, top - 9000.0);
        for (auto& e : colE) e = 0.0;
        for (auto& r : colEAgo) for (auto& e : r) e = 0.0;
        colRing = 0;
        for (auto& d : dryLive) d.reset();
        sibPan.prepare (fs);
        sibGate = 0.0f;
        gateAtk = (float) (1.0 - std::exp (-1.0 / (0.001 * fs))); gateRel = (float) (1.0 - std::exp (-1.0 / (0.040 * fs)));
        for (auto& d : pre) d.reset();
        for (auto& e : shapeEnv) e = 0.0;
        allEnv = 0.0;
        hfFast = hfSlow = 0.0; age = 1 << 30; for (auto& wt : weight) wt = 1.0f; wCount = 0;
        held[0] = 1.0f; held[1] = held[2] = held[3] = held[4] = 0.0f; other = 0.0; armed = true; cur = 0; onset = judging = false;
        envAtk = envRel = 1.0 - std::exp (-1.0 / (0.006 * fs));   // même lissage partout : les rapports entre bandes restent justes dès l'attaque
        fastAtk = 1.0 - std::exp (-1.0 / (0.001 * fs)); fastRel = 1.0 - std::exp (-1.0 / (0.003 * fs)); slowK = 1.0 - std::exp (-1.0 / (0.040 * fs));
        onsetHp[0].setCoeffs (BiquadCoeffs::highpass (fs, 2500.0, 0.7071)); onsetHp[1].setCoeffs (BiquadCoeffs::highpass (fs, 2500.0, 0.7071));
        for (auto& b : onsetHp) b.reset();
    }
    // l'anticipation des détecteurs (4 x 0,7 ms, partagée entre tous) et les 30 ms d'écoute du tri (un T, un K ou un CH ?)
    int latency() const noexcept { return look * 4 + ahead; }
    // LIVE : sans l'écoute du tri, pour enregistrer à travers le plugin (le tri décide sur les premières ms du son)
    int latencyLive() const noexcept { return look * 4; }

    struct Settings { float threshold = -30, range = 10, freq = 5500, clarity = 0; bool on[numTargets] { true, false, false, false, false }; bool listen = false; bool live = false; float pan = 0.0f; };

    void process (float* const* ch, int numCh, int n, const Settings& s) noexcept
    {
        bands[tS].setFreq (s.freq);
        bands[tT].setFreq (4200.0f);
        bands[tCH].setFreq (3200.0f);
        bands[tF].setFreq (9000.0f);
        bands[tK].setFreq (4000.0f);
        live = s.live;
        sibPan.setPan (s.pan);
        clar.setTargetValue (s.clarity * 0.01f);
        listenS.setTargetValue (s.listen ? 1.0f : 0.0f);
        float x[2], red[numTargets];
        for (int i = 0; i < n; ++i)
        {
            float in = 0.0f, mid = 0.0f;
            // le tri écoute le son tel qu'il arrive ; les détecteurs le traitent 10 ms plus tard
            for (int c = 0; c < numCh; ++c) { const float r = ch[c][i]; mid += r; const float dl = pre[c].process (r); x[c] = live ? r : dl; in = std::max (in, std::abs (x[c])); }
            mid /= (float) numCh;
            colE[3] += (double) mid * mid;
            for (int k = 0; k < 3; ++k)
            {
                float v = mid;
                for (auto& b : shape[k]) v = b.process (v, 0);
                colE[k] += (double) v * v;
                follow (shapeEnv[k], (double) v * v);
            }
            follow (allEnv, (double) mid * mid);
            {
                // le début d'un souffle aigu : une attaque (T, K) ne dure que quelques dizaines de ms
                // l'aigu (au-dessus de 2,5 kHz) à 1 ms près, et son plancher récent (descend vite, remonte lentement) :
                // une attaque de T ou de K sort d'une fermeture (presque rien dans l'aigu) et bondit d'un coup
                float hv = mid;
                for (auto& b : onsetHp) hv = b.process (hv, 0);
                const double hfNow = (double) hv * hv;
                hfFast += fastAtk * (hfNow - hfFast);
                hfSlow += (hfFast < hfSlow ? fastRel : slowK) * (hfFast - hfSlow);
                if (hfFast < 3.0 * hfSlow) armed = true;
                if (armed && hfFast > 12.0 * hfSlow && hfFast > 1.0e-8) { age = 0; armed = false; onset = true; }
                else if (age < (1 << 30)) ++age;
            }
            if (++wCount >= 16) { wCount = 0; hissWeights(); }
            float det = -140.0f, worst = 0.0f;
            int who = -1;
            for (int t = 0; t < numTargets; ++t)
            {
                red[t] = bands[t].process (x, numCh, s.threshold, s.range, s.on[t], weight[t]);
                if (s.on[t]) det = std::max (det, bands[t].levelDb);
                if (red[t] > worst) { worst = red[t]; who = t; }
            }
            // PAN : une sibilance (S, CH ou F coché, et reconnu comme tel par le tri) détectée en ce moment
            {
                float g = 0.0f;
                for (int t : { (int) tS, (int) tCH, (int) tF }) g = std::max (g, bands[t].detect * weight[t]);
                sibGate += (g > sibGate ? gateAtk : gateRel) * (g - sibGate);
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
            float out = 0.0f, y[2] { 0.0f, 0.0f };
            for (int cc = 0; cc < numCh; ++cc) y[cc] = shelfDb != 0.0f ? shelf.process (x[cc], cc) : x[cc];
            sibPan.process (y, numCh, sibGate);   // après tout le traitement : la détection n'en voit rien
            for (int cc = 0; cc < numCh; ++cc)
            {
                const float dm = dry[cc].process (ch[cc][i]), dlv = dryLive[cc].process (ch[cc][i]);
                const float d = live ? dlv : dm;
                if (lm > 0.0f) y[cc] += lm * ((d - x[cc]) - y[cc]);   // LISTEN : seulement ce qui est retiré
                ch[cc][i] = y[cc];
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
    float shapeWeight (int t) const noexcept { return weight[t]; }   // le poids de forme du détecteur t (bancs de mesure)

private:
    void follow (double& e, double p) noexcept { e += (p > e ? envAtk : envRel) * (p - e); }
    static float ramp (double v, double width) noexcept { return (float) juce::jlimit (0.0, 1.0, v / width + 0.5); }

    /** Le son tenu (S, CH ou F) d'après sa forme : 0 S, 1 CH, 2 F. Mêmes repères que l'affichage (hissShape). */
    static int heldShape (double rise, double fall) noexcept
    {
        if (rise < 2.0 && fall > 2.0) return 1;
        if (rise >= 5.0 && rise < 9.0 && fall >= 2.0 && fall <= 5.5) return 2;
        return 0;
    }

    /** Quel son passe, et donc quel détecteur a le droit d'agir (les autres n'y touchent pas). Le tri entend 10 ms
        d'avance : à chaque attaque, il écoute ces 10 ms (forme moyenne, et l'aigu qui retombe ou qui tient) puis décide,
        juste au moment où le son arrive aux détecteurs. Une attaque qui retombe : T ou K (le K retombe au-dessus de
        9 kHz) ; un souffle qui tient : S, CH ou F selon sa forme. En cas de doute, c'est un S. */
    void hissWeights() noexcept
    {
        auto d = [] (double v) { return 10.0 * std::log10 (v + 1.0e-30); };
        const double step = 16.0 / fs, a = (double) age / fs, win = live ? 0.004 : aheadSec - 0.006;   // décidé 6 ms avant que le son n'arrive (l'attaque est repérée avec un peu de retard)
        const double bandAge = a - (live ? 0.0 : aheadSec);   // l'âge du son là où il arrive aux détecteurs
        if (onset) { onset = false; judging = true; for (auto& v : acc) v = 0.0; e1 = e2 = 0.0; }
        if (judging && a >= win - 0.003 && e2 == 0.0) e2 = 1.0e-30;
        if (judging)
        {
            for (int k = 0; k < 3; ++k) acc[k] += shapeEnv[k];
            e1 = std::max (e1, hfFast);                                                  // le plus fort de l'écoute
            if (a >= win - 0.003) e2 = std::max (e2, hfFast);                             // ce qu'il en reste à la fin
            if (a >= win)
            {
                judging = false; other = 0.0;
                const double ch = d (acc[0] / shapeW[0]), sD = d (acc[1] / shapeW[1]), aD = d (acc[2] / shapeW[2]);
                const double rise = std::max (sD, aD) - ch, fall = sD - aD, decay = d (e2) - d (e1);
                if (live)
                {
                    // LIVE : rien entendu de la suite ; une attaque qui retombe au-dessus de 9 kHz est un K, un souffle plat un T,
                    // le reste se juge à sa forme (après 50 ms, une attaque qui dure redevient S, CH ou F)
                    cur = fall > kFall ? 3 : (rise < 5.0 && fall < 2.0) ? 4 : heldShape (rise, fall);
                }
                else
                {
                    // 30 ms d'écoute : un T ou un K retombe déjà (un T dure ~35 ms), un S, un CH ou un F tient
                    const bool burst = decay < -3.0 || (decay < -1.9 && rise < 6.0);
                    cur = burst ? (fall > kFall ? 3 : 4) : heldShape (rise, fall);
                }
            }
        }
        else
        {
            // la forme du moment ; pas un souffle aigu (une voyelle) : S, le réglage d'origine (sa garde protège les voyelles)
            const double ch = d (shapeEnv[0] / shapeW[0]), sD = d (shapeEnv[1] / shapeW[1]), aD = d (shapeEnv[2] / shapeW[2]);
            const bool hiss = shapeEnv[0] + shapeEnv[1] + shapeEnv[2] >= 0.35 * allEnv;
            const int cand = hiss ? heldShape (std::max (sD, aD) - ch, sD - aD) : 0;
            if (cur >= 3) { if (bandAge > 0.045) { cur = cand; other = 0.0; } }   // une attaque finie
            // un son tenu garde sa classe ; il n'en change que si une autre forme tient plus de 25 ms
            else if (cand == cur) other = 0.0;
            else if ((other += step) >= (live ? 0.025 : aheadSec)) { cur = cand; other = 0.0; }   // en mix : pile quand le changement arrive aux détecteurs
        }
        // une seule classe à la fois, en fondu de 2 ms ; une attaque ne dure pas (T et K s'effacent après ~40 ms)
        const float g = (float) (1.0 - std::exp (-step / 0.002));
        for (int k = 0; k < 5; ++k) held[k] += g * ((k == cur ? 1.0f : 0.0f) - held[k]);
        const float burstWin = 1.0f - ramp (bandAge - 0.050, 0.010);   // une attaque s'efface ~45 ms après son arrivée aux détecteurs
        weight[tS] = held[0];
        weight[tCH] = held[1];
        weight[tF] = held[2];
        weight[tK] = held[3] * burstWin;
        weight[tT] = held[4] * burstWin;
    }

    void pushColumn() noexcept
    {
        Column c;
        auto db = [] (float a) { return 20.0f * std::log10 (a + 1.0e-7f); };
        c.inDb = db (colIn); c.outDb = db (colOut); c.detDb = colDet; c.redDb = colRed;
        // la forme du son est mesurée en avance (30 ms : trois colonnes) ; en LIVE, au même instant
        const double* e = live ? colE : colEAgo[(colRing + 1) % 3];
        c.eCh = (float) (e[0] / shapeW[0]); c.eS = (float) (e[1] / shapeW[1]); c.eAir = (float) (e[2] / shapeW[2]); c.eAll = (float) e[3];
        colRing = (colRing + 1) % 3;
        for (int k = 0; k < 4; ++k) { colEAgo[colRing][k] = colE[k]; colE[k] = 0.0; }
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
    Biquad shape[3][4], onsetHp[2];
    double shapeEnv[3] {}, allEnv = 0.0, hfFast = 0.0, hfSlow = 0.0, envAtk = 0.1, envRel = 0.001, fastAtk = 0.1, fastRel = 0.01, slowK = 0.001;
    int age = 1 << 30, wCount = 0;
    float held[5] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    double other = 0.0, acc[3] {}, e1 = 0.0, e2 = 0.0, colEAgo[3][4] {};
    int cur = 0, ahead = 1440, colRing = 0;
    bool live = false;
    DelayLine dryLive[2];
    SibPan sibPan;
    float sibGate = 0.0f, gateAtk = 0.1f, gateRel = 0.001f;
    bool onset = false, judging = false;
    DelayLine pre[2];
    bool armed = true;
    double kFall = 4.5, aheadSec = 0.030;
    float weight[numTargets] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    double colE[4] {}, shapeW[3] { 2000.0, 4500.0, 7000.0 };
    std::array<Column, historySize> ring {};
    std::atomic<int> written { 0 };
    std::atomic<float> meter { 0.0f };
};
} // namespace onirique::deess
