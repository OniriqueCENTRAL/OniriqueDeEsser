// Banc des moteurs d'ONIRIQUE DE-ESSER : EngineBench [voix.wav]
//  1. AUTO, RANGE 0 : la sortie est le son d'entrée exactement retardé de la latence.
//  2. AUTO sur une voix : de combien chaque son est baissé, combien de retraits marqués S / T / CH / F.
//  3. MANUEL : le gain d'un événement tombe exactement à sa place (temps du DAW moins la latence), fondus compris ; hors
//     événement et à l'arrêt, rien ne change.
//  4. La capture : écrite, relue (16 bits) à moins de -90 dB près ; les morceaux continus retrouvés.
#include <juce_audio_formats/juce_audio_formats.h>
#include "dsp/AutoDeEsser.h"
#include "dsp/ManualEngine.h"
#include "dsp/Capture.h"
#include "dsp/EventAnalysis.h"
#include <iostream>

using namespace onirique::deess;
static int failures = 0;
static void check (bool ok, const juce::String& what) { std::cout << (ok ? "  ok   " : "  ÉCHEC ") << what << "\n"; if (! ok) ++failures; }

int main (int argc, char** argv)
{
    const double fs = 48000.0;
    juce::Random rnd (7);
    // un signal de test : bruit rose-ish + sinus, avec des bouffées aiguës (des « S »)
    const int n = (int) fs * 4;
    std::vector<float> x ((size_t) n);
    {
        juce::dsp::IIR::Filter<float> hp (juce::dsp::IIR::Coefficients<float>::makeHighPass (fs, 5000.0f));
        for (int i = 0; i < n; ++i)
        {
            const float t = (float) i / (float) fs;
            float v = 0.25f * std::sin (6.2831853f * 220.0f * t);
            const bool burst = std::fmod (t, 1.0f) > 0.6f && std::fmod (t, 1.0f) < 0.75f;
            v = burst ? 0.05f * v + 0.6f * hp.processSample (rnd.nextFloat() * 2.0f - 1.0f) : v;
            x[(size_t) i] = v;
        }
    }

    std::cout << "1. AUTO, RANGE 0 : le son direct retardé\n";
    {
        AutoDeEsser a; a.prepare (fs);
        AutoDeEsser::Settings s; s.range = 0.0f; s.threshold = -60.0f;
        std::vector<float> y = x;
        float* ch[1] { y.data() };
        for (int p = 0; p < n; p += 512) { float* c[1] { y.data() + p }; a.process (c, 1, std::min (512, n - p), s); }
        const int L = a.latency();
        double err = 0.0;
        for (int i = L; i < n; ++i) err = std::max (err, (double) std::abs (y[(size_t) i] - x[(size_t) (i - L)]));
        check (err < 1.0e-6, "écart max " + juce::String (err, 9) + " (latence " + juce::String (L) + " échantillons, " + juce::String (L / fs * 1000.0, 2) + " ms)");
        juce::ignoreUnused (ch);
    }
    {
        // LIVE : la latence courte, le son direct tout aussi exact
        AutoDeEsser a; a.prepare (fs);
        AutoDeEsser::Settings s; s.range = 0.0f; s.threshold = -60.0f; s.live = true;
        std::vector<float> y = x;
        for (int p = 0; p < n; p += 512) { float* c[1] { y.data() + p }; a.process (c, 1, std::min (512, n - p), s); }
        const int L = a.latencyLive();
        double err = 0.0;
        for (int i = L; i < n; ++i) err = std::max (err, (double) std::abs (y[(size_t) i] - x[(size_t) (i - L)]));
        check (err < 1.0e-6 && L < a.latency(), "LIVE : écart max " + juce::String (err, 9) + " (latence " + juce::String (L / fs * 1000.0, 2) + " ms)");
    }

    std::cout << "2. AUTO : les bouffées aiguës baissées et marquées S, la voix intacte\n";
    {
        AutoDeEsser a; a.prepare (fs);
        AutoDeEsser::Settings s; s.range = 12.0f; s.threshold = -40.0f;
        std::vector<float> y = x;
        for (int p = 0; p < n; p += 512) { float* c[1] { y.data() + p }; a.process (c, 1, std::min (512, n - p), s); }
        const int L = a.latency();
        auto rms = [&] (const std::vector<float>& v, double t0, double t1, int shift)
        { double e = 0; int k = 0; for (int i = (int) (t0 * fs); i < (int) (t1 * fs); ++i, ++k) { const float q = v[(size_t) (i + shift)]; e += q * q; } return 10.0 * std::log10 (e / k + 1e-20); };
        // la zone des S (4 à 10 kHz), avant et après
        auto band = [&] (const std::vector<float>& v)
        {
            juce::dsp::IIR::Filter<float> h (juce::dsp::IIR::Coefficients<float>::makeHighPass (fs, 4000.0f)), l (juce::dsp::IIR::Coefficients<float>::makeLowPass (fs, 10000.0f));
            std::vector<float> o (v.size()); for (size_t i = 0; i < v.size(); ++i) o[i] = l.processSample (h.processSample (v[i])); return o;
        };
        const auto xb = band (x), yb = band (y);
        const double redBurst = rms (xb, 1.62, 1.73, 0) - rms (yb, 1.62, 1.73, L), redVoice = rms (x, 1.1, 1.5, 0) - rms (y, 1.1, 1.5, L);
        check (redBurst > 4.0, "zone des S (4-10 kHz) de la bouffée baissée de " + juce::String (redBurst, 1) + " dB");
        check (std::abs (redVoice) < 0.3, "voix (220 Hz) changée de " + juce::String (redVoice, 2) + " dB");
        int marked[5] {}, cols = 0;
        for (int i = 0; i < a.writeIndex(); ++i) { const auto c = a.column (i); if (c.who >= 0) marked[c.who]++; ++cols; }
        check (marked[0] > 20 && marked[1] + marked[2] + marked[3] + marked[4] == 0, "colonnes marquées S : " + juce::String (marked[0]) + " (T/CH/F/K : " + juce::String (marked[1] + marked[2] + marked[3] + marked[4]) + ")");
    }

    std::cout << "3. MANUEL : le gain à sa place dans le temps du DAW\n";
    {
        ManualEngine m;
        const int L = 124;
        m.prepare (fs, L);
        std::vector<Span> sp { { 48000, 52800, juce::Decibels::decibelsToGain (-12.0f), 240 } };
        m.publish (sp);
        std::vector<float> ones ((size_t) n, 1.0f), y = ones;
        const juce::int64 hostStart = 10000;   // le DAW démarre à 10000
        for (int p = 0; p < n; p += 333) { float* c[1] { y.data() + p }; m.process (c, 1, std::min (333, n - p), true, hostStart + p); }
        // la sortie i correspond à la prise en hostStart + i - L
        auto outAt = [&] (juce::int64 t) { return y[(size_t) (t - hostStart + L)]; };
        check (std::abs (outAt (50000) - juce::Decibels::decibelsToGain (-12.0f)) < 1e-6, "au milieu : " + juce::String (juce::Decibels::gainToDecibels (outAt (50000)), 2) + " dB");
        check (std::abs (outAt (47000) - 1.0f) < 1e-6 && std::abs (outAt (54000) - 1.0f) < 1e-6, "avant et après : 0 dB");
        const float mid = outAt (48000 - 120);
        check (mid < 0.99f && mid > juce::Decibels::decibelsToGain (-12.0f), "fondu d'entrée : " + juce::String (juce::Decibels::gainToDecibels (mid), 2) + " dB");
        // à l'arrêt : rien ne change (le gain revient à 1)
        std::vector<float> z ((size_t) 48000, 1.0f);
        for (int p = 0; p < 48000; p += 512) { float* c[1] { z.data() + p }; m.process (c, 1, std::min (512, 48000 - p), false, 0); }
        check (std::abs (z.back() - 1.0f) < 1e-3, "à l'arrêt : " + juce::String (z.back(), 4));
    }

    std::cout << "4. La capture : écrite, relue\n";
    {
        Store st;
        st.write (100000, x.data(), 48000);
        st.write (400000, x.data() + 48000, 9600);
        const auto spans = st.spans();
        check (spans.size() == 2 && spans[0].first <= 100000 && spans[0].second >= 148000 && spans[1].first <= 400000, "deux morceaux : " + juce::String ((int) spans.size()));
        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("deess-bench.ocap");
        check (st.save (f), "écrite (" + juce::String (f.getSize() / 1024) + " ko)");
        Store back;
        check (back.load (f), "relue");
        std::vector<float> a (48000), b (48000);
        st.read (100000, a.data(), 48000); back.read (100000, b.data(), 48000);
        double err = 0; for (int i = 0; i < 48000; ++i) err = std::max (err, (double) std::abs (a[(size_t) i] - b[(size_t) i]));
        check (err < 1.0e-4, "écart après relecture : " + juce::String (20.0 * std::log10 (err + 1e-12), 1) + " dB");
        f.deleteFile();
    }

    if (argc > 1)
    {
        std::cout << "5. Une vraie voix : " << argv[1] << "\n";
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File { juce::String (argv[1]) }));
        if (rd != nullptr)
        {
            const int len = (int) rd->lengthInSamples;
            juce::AudioBuffer<float> buf ((int) rd->numChannels, len);
            rd->read (&buf, 0, len, 0, true, true);
            const double sr = rd->sampleRate;
            AutoDeEsser a; a.prepare (sr);
            AutoDeEsser::Settings s;
            for (auto& o : s.on) o = true;
            const int nc = std::min (2, buf.getNumChannels());
            int groups[5] {}, last = -1;
            float maxRed = 0;
            for (int p = 0; p < len; p += 512)
            {
                float* c[2] { buf.getWritePointer (0) + p, buf.getWritePointer (nc - 1) + p };
                a.process (c, nc, std::min (512, len - p), s);
                // les colonnes de ce bloc
                static int seen = 0;
                for (; seen < a.writeIndex(); ++seen)
                {
                    const auto col = a.column (seen);
                    if (col.who >= 0 && col.who != last) groups[col.who]++;
                    last = col.who;
                    maxRed = std::max (maxRed, col.redDb);
                }
            }
            bool finite = true;
            for (int c = 0; c < nc; ++c) for (int i = 0; i < len; ++i) if (! std::isfinite (buf.getSample (c, i))) finite = false;
            check (finite, "sortie finie");
            std::cout << "       THRESHOLD -30 : retraits S " << groups[0] << ", T " << groups[1] << ", CH " << groups[2] << ", F " << groups[3] << ", K " << groups[4]
                      << " ; réduction max " << maxRed << " dB\n";
        }
    }
    std::cout << (failures == 0 ? "TOUT EST BON\n" : "DES ÉCHECS\n");
    return failures == 0 ? 0 : 1;
}
