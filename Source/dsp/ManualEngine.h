#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>
#include "Common.h"
#include "SibPan.h"

namespace onirique::deess
{
/*  MODE MANUEL, côté son : chaque événement réglé baisse le son ENTIER pendant sa durée (comme un gain de clip posé à la
    main), avec un fondu en cosinus avant et après (5 ms pour un son, 20 ms pour une respiration) : pas de clic.

    Le temps : le bloc reçu commence à la position P du DAW ; la sortie a le retard déclaré L (le même qu'en AUTO : passer
    d'un mode à l'autre ne décale rien), donc l'échantillon i de la sortie est celui de la prise en P + i - L.
    Hors lecture (DAW arrêté, position inconnue), le son passe sans changement.

    La table des gains est faite sur le fil des messages et publiée d'un coup (pointeur atomique) : le fil audio ne
    verrouille ni n'alloue jamais ; les anciennes tables sont libérées plus tard, sur le fil des messages.

    L'ÉCOUTE : le son d'un événement (avec un peu d'avant et d'après, son gain appliqué) préparé sur le fil des messages
    dans l'un de deux tampons, puis joué par le fil audio à la place du son qui passe.

    LE PAN DES SIBILANCES : une seconde table, celle des événements de la famille SIBILANCES (même ceux qui ne sont pas
    baissés) ; pendant chacun d'eux, avec ses fondus, SibPan déplace le haut du son dans la stéréo. */

struct Span { juce::int64 start = 0, end = 0; float gain = 1.0f; int fade = 220; };

class GainTable
{
public:
    std::vector<Span> spans;   // rangés par début

    /** Le gain en t (1 hors des événements), et l'indice d'où repartir pour le suivant. */
    float at (juce::int64 t, size_t& cursor) const noexcept
    {
        const size_t n = spans.size();
        if (n == 0) return 1.0f;
        if (cursor >= n || (cursor > 0 && spans[cursor - 1].end + spans[cursor - 1].fade > t) || spans[cursor].start - spans[cursor].fade > t + 4096)
        {
            // saut : on recherche (le premier événement qui finit après t, fondu compris)
            size_t lo = 0, hi = n;
            while (lo < hi) { const size_t m = (lo + hi) / 2; if (spans[m].end + spans[m].fade < t) lo = m + 1; else hi = m; }
            cursor = lo;
        }
        while (cursor < n && spans[cursor].end + spans[cursor].fade < t) ++cursor;
        float g = 1.0f;
        for (size_t k = cursor; k < n && spans[k].start - spans[k].fade <= t; ++k)
        {
            const auto& s = spans[k];
            if (t > s.end + s.fade) continue;
            float w = 1.0f;
            if (t < s.start) w = shape ((float) (t - (s.start - s.fade)) / (float) s.fade);
            else if (t > s.end) w = shape ((float) ((s.end + s.fade) - t) / (float) s.fade);
            g *= 1.0f + (s.gain - 1.0f) * w;
        }
        return g;
    }
    static float shape (float u) noexcept { u = juce::jlimit (0.0f, 1.0f, u); return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * u); }
};

class ManualEngine
{
public:
    ~ManualEngine() { delete table.exchange (nullptr); delete sibTable.exchange (nullptr); }

    void prepare (double sampleRate, int latencySamples)
    {
        fs = sampleRate; lat = latencySamples;
        for (auto& d : delay) d.prepare (std::max (1, lat));
        for (auto& b : preview) b.assign ((size_t) (fs * 6.0), 0.0f);
        previewLen.store (0); previewPos.store (0);
        cursor = sibCursor = 0;
        sibPan.prepare (fs);
        gate = 0.0f;
    }
    int latency() const noexcept { return lat; }

    // ---- fil des messages
    void publish (std::vector<Span> spans, std::vector<Span> sibilances = {})
    {
        auto byStart = [] (const Span& a, const Span& b) { return a.start < b.start; };
        std::sort (spans.begin(), spans.end(), byStart);
        std::sort (sibilances.begin(), sibilances.end(), byStart);
        for (auto& s : sibilances) s.gain = 0.0f;   // lue comme 1 - gain : 1 pendant la sibilance, 0 ailleurs
        auto* t = new GainTable { std::move (spans) };
        auto* st = new GainTable { std::move (sibilances) };
        const auto now = juce::Time::getMillisecondCounter();
        if (auto* old = table.exchange (t, std::memory_order_acq_rel)) retired.push_back ({ old, now });
        if (auto* old = sibTable.exchange (st, std::memory_order_acq_rel)) retired.push_back ({ old, now });
        collect();
    }
    void collect()   // libère les tables que le fil audio ne peut plus lire (plus d'une seconde après leur retrait)
    {
        const auto now = juce::Time::getMillisecondCounter();
        retired.erase (std::remove_if (retired.begin(), retired.end(), [now] (auto& r) { if (now - r.second > 1000) { delete r.first; return true; } return false; }),
                       retired.end());
    }
    /** Fait entendre ces échantillons (mono), à la place du son qui passe. */
    void play (const float* x, int n)
    {
        const int slot = 1 - previewSlot.load();
        n = std::min (n, (int) preview[(size_t) slot].size());
        std::copy (x, x + n, preview[(size_t) slot].begin());
        previewLen.store (0);
        previewSlot.store (slot);
        previewPos.store (0);
        previewLen.store (n);
    }
    void stopPlaying() { previewLen.store (0); }
    bool isPlaying() const { return previewPos.load() < previewLen.load(); }

    // ---- fil audio
    void process (float* const* ch, int numCh, int n, bool rolling, juce::int64 pos, float pan = 0.0f) noexcept
    {
        const GainTable* t = table.load (std::memory_order_acquire);
        const GainTable* st = sibTable.load (std::memory_order_acquire);
        sibPan.setPan (pan);
        for (int i = 0; i < n; ++i)
        {
            const float g = rolling && t != nullptr ? t->at (pos + i - lat, cursor) : 1.0f;
            if (rolling) gNow = g;                      // en lecture : le gain exact de la table (fondus compris)
            else gNow += 0.002f * (1.0f - gNow);        // à l'arrêt : il revient doucement à 1
            if (rolling && st != nullptr) gate = 1.0f - st->at (pos + i - lat, sibCursor);
            else gate += 0.002f * (0.0f - gate);
            float y[2] { 0.0f, 0.0f };
            for (int c = 0; c < numCh; ++c) y[c] = delay[c].process (ch[c][i]) * gNow;
            sibPan.process (y, numCh, gate);
            for (int c = 0; c < numCh; ++c) ch[c][i] = y[c];
        }
        lastGain.store (gNow, std::memory_order_relaxed);
        // l'écoute d'un événement
        const int len = previewLen.load (std::memory_order_acquire);
        int p = previewPos.load (std::memory_order_relaxed);
        if (p < len)
        {
            const auto& b = preview[(size_t) previewSlot.load (std::memory_order_acquire)];
            const int fade = (int) (fs * 0.004);
            for (int i = 0; i < n && p < len; ++i, ++p)
            {
                const float w = std::min (1.0f, std::min ((float) p, (float) (len - 1 - p)) / (float) std::max (1, fade));
                for (int c = 0; c < numCh; ++c) ch[c][i] = b[(size_t) p] * w;
            }
            previewPos.store (p, std::memory_order_relaxed);
        }
    }
    float currentGain() const noexcept { return lastGain.load (std::memory_order_relaxed); }
    int previewPosition() const noexcept { return previewPos.load (std::memory_order_relaxed); }
    int previewLength() const noexcept { return previewLen.load (std::memory_order_relaxed); }

private:
    double fs = 44100.0;
    int lat = 0;
    DelayLine delay[2];
    std::atomic<GainTable*> table { nullptr }, sibTable { nullptr };
    size_t sibCursor = 0;
    SibPan sibPan;
    float gate = 0.0f;
    std::vector<std::pair<GainTable*, juce::uint32>> retired;
    size_t cursor = 0;
    float gNow = 1.0f;
    std::atomic<float> lastGain { 1.0f };
    std::vector<float> preview[2];
    std::atomic<int> previewSlot { 0 }, previewLen { 0 }, previewPos { 0 };
};
} // namespace onirique::deess
