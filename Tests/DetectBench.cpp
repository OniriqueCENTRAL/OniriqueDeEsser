// Banc de la détection : DetectBench <voix.wav> [sortie.png] [début s] [durée s]
// Liste les événements trouvés et dessine le spectrogramme avec les événements par-dessus (S, CH, Z, F en rouge, P B T K en
// ambre, respirations en gris bleu), pour vérifier à l'œil que chaque lettre tombe au bon endroit.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include "dsp/EventAnalysis.h"
#include <iostream>

int main (int argc, char** argv)
{
    using namespace onirique::deess;
    if (argc < 2) { std::cout << "DetectBench voix.wav [sortie.png] [debut] [duree]\n"; return 1; }
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File { juce::String (argv[1]) }));
    if (rd == nullptr) { std::cout << "illisible\n"; return 1; }
    const int n = (int) rd->lengthInSamples;
    juce::AudioBuffer<float> buf ((int) rd->numChannels, n);
    rd->read (&buf, 0, n, 0, true, true);
    std::vector<float> mono ((size_t) n);
    for (int c = 0; c < buf.getNumChannels(); ++c) for (int i = 0; i < n; ++i) mono[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();
    const double fs = rd->sampleRate;

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto ev = Analyzer::run (mono.data(), n, fs, 0);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    int count[3] {};
    for (auto& e : ev) count[e.family]++;
    std::cout << "duree " << n / fs << " s, analyse " << (t1 - t0) << " ms ; sibilances " << count[0] << ", consonnes " << count[1]
              << ", respirations " << count[2] << "\n";
    std::map<juce::String, int> letters;
    for (auto& e : ev) letters[e.letter]++;
    for (auto& [l, c] : letters) std::cout << "  " << l << " " << c;
    std::cout << "\n";
    for (auto& e : ev)
        std::cout << juce::String (e.start / fs, 3) << "-" << juce::String (e.end / fs, 3) << "  " << e.letter << "  niv " << juce::String (e.levelDb, 1)
                  << "  saillie " << juce::String (e.saliencyDb, 1)
                  << (e.family == 0 ? "  montee " + juce::String (e.rise, 1) + "  chute " + juce::String (e.fall, 1) : juce::String()) << "\n";

    if (auto* dbg = std::getenv ("DEBUG_T"))
    {
        Analyzer a (fs);
        a.measure (mono.data(), n, nullptr);
        a.reference();
        const double t = atof (dbg);
        for (int f = 0; f < (int) a.frames.size(); ++f)
        {
            const double tf = (double) a.toSample (f) / fs;
            if (std::abs (tf - t) > (std::getenv ("DEBUG_W") ? atof (std::getenv ("DEBUG_W")) : 0.08) || (f % 2)) continue;
            const auto& r = a.frames[(size_t) f];
            std::cout << juce::String (tf, 3) << " tot " << juce::String (r.total, 1) << " pop " << juce::String (r.pop, 1) << " body " << juce::String (r.body, 1)
                      << " low " << juce::String (r.low, 1) << " ch " << juce::String (r.ch, 1) << " s " << juce::String (r.s, 1) << " air " << juce::String (r.air, 1)
                      << " vo " << juce::String (r.voicing, 2) << " ref " << juce::String (r.voiceRef, 1) << " flux " << juce::String (r.flux, 2) << "\n";
        }
        return 0;
    }
    if (argc >= 3)
    {
        const double from = argc >= 4 ? atof (argv[3]) : 0.0;
        const double len = std::min (argc >= 5 ? atof (argv[4]) : 10.0, (n - 1025) / fs - from);
        Analyzer a (fs);
        a.measure (mono.data(), n, nullptr);
        a.reference();
        const int W = 2000, H = 700, top = 40, specH = 480;
        juce::Image img (juce::Image::RGB, W, H, true, juce::SoftwareImageType());
        juce::Graphics g (img);
        g.fillAll (juce::Colour (0xff0b0909));
        // spectrogramme (FFT 1024, 0..16 kHz, échelle log douce)
        juce::dsp::FFT fft (10);
        std::vector<float> w (2048);
        const juce::int64 s0 = (juce::int64) (from * fs), s1 = std::min<juce::int64> (n - 1024, (juce::int64) ((from + len) * fs));
        for (int x = 0; x < W; ++x)
        {
            const juce::int64 p = s0 + (juce::int64) ((double) x / W * (double) (s1 - s0));
            std::fill (w.begin(), w.end(), 0.0f);
            for (int i = 0; i < 1024; ++i) w[(size_t) i] = mono[(size_t) (p + i)] * (0.5f - 0.5f * std::cos (6.2831853f * i / 1024.0f));
            fft.performFrequencyOnlyForwardTransform (w.data(), true);
            for (int y = 0; y < specH; ++y)
            {
                const double hz = 60.0 * std::pow (16000.0 / 60.0, 1.0 - (double) y / specH);
                const int b = std::min (511, (int) (hz / fs * 1024.0));
                const float db = 20.0f * std::log10 (w[(size_t) b] / 256.0f + 1.0e-7f);
                const float v = juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 80.0f);
                img.setPixelAt (x, top + y, juce::Colour::fromHSV (0.02f, 0.6f * (1 - v), v, 1.0f));
            }
        }
        g.setFont (14.0f);
        for (double hz : { 200.0, 1000.0, 2500.0, 4500.0, 9000.0 })
        {
            const int y = top + (int) ((1.0 - std::log (hz / 60.0) / std::log (16000.0 / 60.0)) * specH);
            g.setColour (juce::Colours::white.withAlpha (0.25f)); g.drawHorizontalLine (y, 0.0f, (float) W);
            g.drawText (juce::String ((int) hz) + " Hz", 4, y - 14, 80, 14, juce::Justification::left);
        }
        const juce::Colour famCol[3] { juce::Colour (0xffff3b4e), juce::Colour (0xffffb35c), juce::Colour (0xff8fb6d8) };
        for (auto& e : ev)
        {
            const double xa = (e.start / fs - from) / len * W, xb = (e.end / fs - from) / len * W;
            if (xb < 0 || xa > W) continue;
            const int lane = top + specH + 20 + e.family * 50;
            g.setColour (famCol[e.family].withAlpha (0.18f));
            g.fillRect ((float) xa, (float) top, (float) (xb - xa), (float) specH);
            g.setColour (famCol[e.family]);
            g.fillRect ((float) xa, (float) lane, (float) std::max (2.0, xb - xa), 30.0f);
            g.setColour (juce::Colours::black);
            g.drawText (e.letter, (int) xa, lane, (int) std::max (18.0, xb - xa), 30, juce::Justification::centred);
        }
        // forme d'onde en haut
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        for (int x = 0; x < W; ++x)
        {
            const juce::int64 p0 = s0 + (juce::int64) ((double) x / W * (s1 - s0)), p1 = s0 + (juce::int64) ((double) (x + 1) / W * (s1 - s0));
            float mx = 0; for (auto p = p0; p < p1; ++p) mx = std::max (mx, std::abs (mono[(size_t) p]));
            g.drawVerticalLine (x, 20.0f - 18.0f * mx, 20.0f + 18.0f * mx);
        }
        for (int sec = (int) std::ceil (from); sec < from + len; ++sec)
        {
            const int x = (int) ((sec - from) / len * W);
            g.setColour (juce::Colours::white.withAlpha (0.5f)); g.drawVerticalLine (x, (float) (H - 20), (float) H);
            g.drawText (juce::String (sec) + " s", x + 2, H - 18, 50, 16, juce::Justification::left);
        }
        juce::File f { juce::String (argv[2]) }; f.deleteFile();
        if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
    }
    return 0;
}
