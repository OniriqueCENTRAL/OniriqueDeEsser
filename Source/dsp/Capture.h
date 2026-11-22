#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <map>
#include <vector>

namespace onirique::deess
{
/*  LA CAPTURE (mode MANUEL) : la prise enregistrée pendant que le DAW la joue, rangée à sa place dans le temps du DAW.

    Fil audio -> Fifo : des paquets de 512 échantillons (mono) avec leur position, sans verrou ni allocation.
    Fil des messages -> Store : les paquets vidés de la Fifo (minuterie), rangés par blocs de 32768 échantillons alloués à la
    demande ; chaque bloc sait quels morceaux de 512 ont été enregistrés (un bit chacun). Rien n'est gardé là où le DAW n'a
    pas joué : une prise de 3 minutes au milieu d'un morceau ne coûte que ses 3 minutes.
    Sauvegarde : les blocs (en 16 bits) compressés, dans un fichier à côté (le projet ne garde que son nom). */

class Fifo
{
public:
    static constexpr int packet = 512, slots = 1024;   // ~11 s d'avance à 48 kHz
    struct Packet { juce::int64 pos = 0; int n = 0; float s[packet] {}; };

    Fifo() : ring ((size_t) slots) {}

    // fil audio
    void push (juce::int64 pos, const float* x, int n) noexcept
    {
        while (n > 0)
        {
            const int w = wr.load (std::memory_order_relaxed);
            if (w - rd.load (std::memory_order_acquire) >= slots) { dropped.store (true); return; }   // plein : on perd plutôt que bloquer
            auto& p = ring[(size_t) (w % slots)];
            const int k = std::min (n, packet);
            p.pos = pos; p.n = k;
            std::copy (x, x + k, p.s);
            wr.store (w + 1, std::memory_order_release);
            pos += k; x += k; n -= k;
        }
    }
    // fil des messages
    template <typename F> int drain (F&& use)
    {
        int count = 0;
        for (;;)
        {
            const int r = rd.load (std::memory_order_relaxed);
            if (r == wr.load (std::memory_order_acquire)) break;
            const auto& p = ring[(size_t) (r % slots)];
            use (p.pos, p.s, p.n);
            rd.store (r + 1, std::memory_order_release);
            ++count;
        }
        return count;
    }
    std::atomic<bool> dropped { false };

private:
    std::vector<Packet> ring;
    std::atomic<int> wr { 0 }, rd { 0 };
};

class Store
{
public:
    static constexpr int chunk = 32768, sub = 512, subs = chunk / sub;   // 64 morceaux par bloc

    void clear() { chunks.clear(); ++revision; }
    bool empty() const { return chunks.empty(); }
    int getRevision() const { return revision; }

    void write (juce::int64 pos, const float* x, int n)
    {
        if (pos < 0) { const int skip = (int) std::min<juce::int64> (n, -pos); x += skip; n -= skip; pos = 0; }
        while (n > 0)
        {
            const juce::int64 ci = pos / chunk;
            const int off = (int) (pos - ci * chunk), k = std::min (n, chunk - off);
            auto& c = chunks[ci];
            if (c.data.empty()) c.data.assign ((size_t) chunk, 0.0f);
            std::copy (x, x + k, c.data.begin() + off);
            for (int s = off / sub; s <= (off + k - 1) / sub; ++s) c.mask |= (1ull << s);
            pos += k; x += k; n -= k;
        }
        ++revision;
    }

    /** Lit [pos, pos + n) : des zéros là où rien n'a été capturé. */
    void read (juce::int64 pos, float* dest, int n) const
    {
        while (n > 0)
        {
            const juce::int64 ci = pos >= 0 ? pos / chunk : (pos - chunk + 1) / chunk;
            const int off = (int) (pos - ci * chunk), k = std::min (n, chunk - off);
            const auto it = chunks.find (ci);
            if (it == chunks.end()) std::fill (dest, dest + k, 0.0f);
            else std::copy (it->second.data.begin() + off, it->second.data.begin() + off + k, dest);
            pos += k; dest += k; n -= k;
        }
    }

    /** La crête de [pos, pos + n) (pour dessiner) ; -1 si rien n'y a été capturé. */
    float peak (juce::int64 a, juce::int64 b) const
    {
        float m = -1.0f;
        for (juce::int64 p = std::max<juce::int64> (0, a); p < b;)
        {
            const juce::int64 ci = p / chunk;
            const int off = (int) (p - ci * chunk);
            const int k = (int) std::min<juce::int64> (b - p, chunk - off);
            const auto it = chunks.find (ci);
            if (it != chunks.end() && it->second.mask != 0)
            {
                m = std::max (m, 0.0f);
                const float* d = it->second.data.data() + off;
                for (int i = 0; i < k; ++i) m = std::max (m, std::abs (d[i]));
            }
            p += k;
        }
        return m;
    }

    /** Les morceaux continus enregistrés : [début, fin). */
    std::vector<std::pair<juce::int64, juce::int64>> spans() const
    {
        std::vector<std::pair<juce::int64, juce::int64>> out;
        juce::int64 s = -1, e = -1;
        for (const auto& [ci, c] : chunks)
            for (int k = 0; k < subs; ++k)
            {
                const juce::int64 a = ci * chunk + (juce::int64) k * sub;
                if ((c.mask >> k) & 1ull)
                {
                    if (s < 0) { s = a; e = a + sub; }
                    else if (a == e) e = a + sub;
                    else { out.push_back ({ s, e }); s = a; e = a + sub; }
                }
            }
        if (s >= 0) out.push_back ({ s, e });
        return out;
    }

    juce::int64 firstSample() const { const auto sp = spans(); return sp.empty() ? 0 : sp.front().first; }
    juce::int64 lastSample() const { const auto sp = spans(); return sp.empty() ? 0 : sp.back().second; }

    // ---- sauvegarde : 'OCAP', version, puis chaque bloc (index, masque, 32768 échantillons en 16 bits), le tout compressé
    bool save (const juce::File& f) const
    {
        f.getParentDirectory().createDirectory();
        juce::TemporaryFile tmp (f);
        {
            auto raw = tmp.getFile().createOutputStream();
            if (raw == nullptr) return false;
            juce::GZIPCompressorOutputStream z (*raw, 6);
            z.writeInt (0x5041434f); z.writeInt (1); z.writeInt ((int) chunks.size());
            std::vector<juce::int16> q ((size_t) chunk);
            for (const auto& [ci, c] : chunks)
            {
                z.writeInt64 (ci); z.writeInt64 ((juce::int64) c.mask);
                for (int i = 0; i < chunk; ++i) q[(size_t) i] = (juce::int16) juce::jlimit (-32767, 32767, (int) std::lround (c.data[(size_t) i] * 32767.0f));
                z.write (q.data(), q.size() * sizeof (juce::int16));
            }
            z.flush();
        }
        return tmp.overwriteTargetFileWithTemporary();
    }
    bool load (const juce::File& f)
    {
        auto raw = f.createInputStream();
        if (raw == nullptr) return false;
        juce::GZIPDecompressorInputStream z (*raw);
        if (z.readInt() != 0x5041434f || z.readInt() != 1) return false;
        const int count = z.readInt();
        std::map<juce::int64, Chunk> got;
        std::vector<juce::int16> q ((size_t) chunk);
        for (int k = 0; k < count; ++k)
        {
            const juce::int64 ci = z.readInt64();
            Chunk c;
            c.mask = (juce::uint64) z.readInt64();
            if (z.read (q.data(), (int) (q.size() * sizeof (juce::int16))) != (int) (q.size() * sizeof (juce::int16))) return false;
            c.data.resize ((size_t) chunk);
            for (int i = 0; i < chunk; ++i) c.data[(size_t) i] = (float) q[(size_t) i] / 32767.0f;
            got[ci] = std::move (c);
        }
        chunks = std::move (got);
        ++revision;
        return true;
    }

private:
    struct Chunk { std::vector<float> data; juce::uint64 mask = 0; };
    std::map<juce::int64, Chunk> chunks;
    int revision = 0;
};
} // namespace onirique::deess
