#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace onirique::deess;

SoloProcessor::SoloProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undo, "SOLO", params::layout())
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };
    pMode = raw (params::mode); pBypass = raw (params::bypass); pThr = raw (params::threshold); pRange = raw (params::range);
    pFreq = raw (params::freq); pClar = raw (params::clarity); pListen = raw (params::listen); pLive = raw (params::live);
    pT[0] = raw (params::targetS); pT[1] = raw (params::targetT); pT[2] = raw (params::targetCH); pT[3] = raw (params::targetF); pT[4] = raw (params::targetK);
    pTame[0] = raw (params::tameSib); pTame[1] = raw (params::tameCons); pTame[2] = raw (params::tameBreath);
    apvts.state.setProperty ("preset", "INIT", nullptr);
    captureId = juce::Uuid().toString();
    worker.startThread (juce::Thread::Priority::low);
    startTimerHz (30);
}

SoloProcessor::~SoloProcessor()
{
    stopTimer();
    worker.cancel.store (true);
    worker.signalThreadShouldExit();
    worker.wake.signal();
    worker.stopThread (4000);
    if (storeDirty && ! store.empty()) store.save (captureFile());
}

bool SoloProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void SoloProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
#if SOLO_LICENCE
    license.refresh();
#endif
    ready.store (false);
    rate = sampleRate;
    autoDs.prepare (rate);
    latency = latencyMix = autoDs.latency();
    latencyLive = autoDs.latencyLive();
    manual.prepare (rate, latency);
    for (auto& d : dryDelay) d.prepare (latencyMix);
    for (auto& d : dryDelayLive) d.prepare (latencyLive);
    liveReported = liveNow();
    latency = liveReported ? latencyLive : latencyMix;
    latencyNow.store (latency);
    const int maxN = std::max (4096, samplesPerBlock * 2);
    scratch.setSize (2, maxN);
    other.setSize (2, maxN);
    mono.assign ((size_t) maxN, 0.0f);
    modeS.reset (rate, 0.03); modeS.setCurrentAndTargetValue (pMode->load() > 0.5f ? 1.0f : 0.0f);
    bypassS.reset (rate, 0.02); bypassS.setCurrentAndTargetValue (pBypass->load() > 0.5f ? 1.0f : 0.0f);
    setLatencySamples (latency);
    publish();
    ready.store (true);
}

AutoDeEsser::Settings SoloProcessor::readAuto() const
{
    AutoDeEsser::Settings s;
    s.threshold = pThr->load(); s.range = pRange->load(); s.freq = pFreq->load(); s.clarity = pClar->load();
    for (int t = 0; t < numTargets; ++t) s.on[t] = pT[t]->load() > 0.5f;
    s.listen = pListen->load() > 0.5f;
    s.live = liveNow();
    return s;
}

void SoloProcessor::process (juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), ch = std::min (2, getMainBusNumOutputChannels());
    for (int c = 0; c < ch; ++c)
    {
        auto* s = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i) if (! std::isfinite (s[i])) s[i] = 0.0f;
    }
    for (int c = ch; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (! ready.load() || n == 0) return;
    if (scratch.getNumSamples() < n || other.getNumSamples() < n || (int) mono.size() < n) return;   // un hôte qui dépasse le bloc annoncé (2x)

    // la position du DAW
    bool rolling = false;
    juce::int64 pos = 0;
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
            if (info->getIsPlaying() || info->getIsRecording())
                if (auto t = info->getTimeInSamples()) { rolling = true; pos = *t; }
    playing.store (rolling);
    if (rolling) hostPos.store (pos - latencyNow.load());
#if ONIRIQUE_UI_TEST
    dbgBlocks.fetch_add (1); if (rolling) dbgRolling.fetch_add (1);
#endif

    // CAPTURE (automatique en MANUEL) : la prise, telle qu'elle entre (mono), à sa place dans le temps ; le fil des messages
    // garde seulement ce qui est nouveau
    if (rolling && capturing.load() && pMode->load() > 0.5f)
    {
        for (int i = 0; i < n; ++i) { float m = 0.0f; for (int c = 0; c < ch; ++c) m += buffer.getSample (c, i); mono[(size_t) i] = m / (float) std::max (1, ch); }
        fifo.push (pos, mono.data(), n);
    }

    float* io[2] { buffer.getWritePointer (0), ch > 1 ? buffer.getWritePointer (1) : nullptr };
    // le son direct retardé (bypass en fondu)
    for (int c = 0; c < ch; ++c) scratch.copyFrom (c, 0, buffer, c, 0, n);

    modeS.setTargetValue (pMode->load() > 0.5f ? 1.0f : 0.0f);
    bypassS.setTargetValue (bypassed || pBypass->load() > 0.5f ? 1.0f : 0.0f);
    // les deux moteurs tournent toujours (légers) : passer de l'un à l'autre ne fait entendre aucun reste d'avant.
    // AUTO dans buffer, MANUEL dans une copie, puis le mode choisi (en fondu)
    for (int c = 0; c < ch; ++c) other.copyFrom (c, 0, buffer, c, 0, n);
    float* o2[2] { other.getWritePointer (0), other.getWritePointer (1) };
    autoDs.process (io, ch, n, readAuto());
    manual.process (o2, ch, n, rolling, pos);
    if (modeS.isSmoothing())
        for (int i = 0; i < n; ++i)
        {
            const float m = modeS.getNextValue();
            for (int c = 0; c < ch; ++c) io[c][i] += m * (o2[c][i] - io[c][i]);
        }
    else if (modeS.getTargetValue() > 0.5f)
        for (int c = 0; c < ch; ++c) buffer.copyFrom (c, 0, other, c, 0, n);

    // BYPASS : le son direct, retardé de la même latence
    for (int c = 0; c < ch; ++c)
    {
        auto* d = scratch.getWritePointer (c);
        const bool lv = liveNow();
        for (int i = 0; i < n; ++i) { const float a = dryDelay[c].process (d[i]), b = dryDelayLive[c].process (d[i]); d[i] = lv ? b : a; }
    }
    if (bypassS.isSmoothing() || bypassS.getTargetValue() > 0.5f)
        for (int i = 0; i < n; ++i)
        {
            const float b = bypassS.getNextValue();
            for (int c = 0; c < ch; ++c) io[c][i] += b * (scratch.getSample (c, i) - io[c][i]);
        }

#if SOLO_LICENCE
    juce::AudioBuffer<float> main (buffer.getArrayOfWritePointers(), ch, n);
    license.process (main, getSampleRate(), isNonRealtime());
#endif
}

void SoloProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, false); }
void SoloProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, true); }

//==============================================================================
//  Le mode MANUEL, côté fil des messages
//==============================================================================
void SoloProcessor::setCapturing (bool on)
{
    capturing.store (on);
    if (! on) lastCaptureMs = 0;   // l'analyse finit tout de suite ce qui reste
}

bool SoloProcessor::isRecording() const
{
    return lastCaptureMs != 0 && juce::Time::getMillisecondCounter() - lastCaptureMs < 500;
}

void SoloProcessor::clearCapture()
{
    fifo.drain ([] (juce::int64, const float*, int) {});
    worker.cancel.store (true);
    {
        const juce::ScopedLock sl (evLock);
        evts.clear();
    }
    store.clear();
    for (auto& c : tameRefCount) c = 0;
    juce::File old = captureFile();
    old.deleteFile();
    captureId = juce::Uuid().toString();
    dirtyFrom = dirtyTo = -1;
    storeDirty = false;
    listening = -1;
    manual.stopPlaying();
    ++evRevision;
    publish();
}

float SoloProcessor::tameGain (const Event& e) const
{
    const float a = juce::jlimit (0.0f, 1.0f, pTame[e.family]->load() * 0.01f);
    if (a <= 0.0f) return 0.0f;
    // la saillie médiane de la famille : les plus saillants baissent davantage (ce qu'on ferait à la main)
    const float med = tameRef[e.family];
    const float over = std::max (0.0f, e.saliencyDb - med + 3.0f);
    float g = 0.0f;
    switch (e.family)
    {
        case sibilance: g = -a * (6.0f + 1.1f * over); break;
        case consonant: g = -a * (5.0f + 0.9f * over); break;
        default:        g = -a * (14.0f + 0.5f * over); break;
    }
    return std::max (-30.0f, g);
}

void SoloProcessor::applyTame()
{
    // la référence de chaque famille (sa saillie médiane) : libre tant que TOUT RÉDUIRE est à 0, refaite quand on y touche,
    // puis figée (sauf si la prise change de 30 % et d'au moins 10 événements) : un bout de piste ajouté plus tard ne
    // retouche pas les gains déjà posés
    float* med = tameRef;
    for (int f = 0; f < numFamilies; ++f)
    {
        std::vector<float> s;
        for (const auto& q : evts) if (q.family == f) s.push_back (q.saliencyDb);
        const int n = (int) s.size();
        const bool bigChange = std::abs (n - tameRefCount[f]) >= 10 && (n >= tameRefCount[f] * 13 / 10 || n * 10 <= tameRefCount[f] * 7);
        if (n > 0 && (tameRefCount[f] == 0 || tameFresh[f] || pTame[f]->load() <= 0.0f || bigChange))
        {
            std::nth_element (s.begin(), s.begin() + (long) n / 2, s.end());
            med[f] = s[(size_t) n / 2];
            tameRefCount[f] = n;
        }
        tameFresh[f] = false;
    }
    const juce::ScopedLock sl (evLock);
    for (auto& e : evts)
    {
        if (e.manual) continue;
        const float a = juce::jlimit (0.0f, 1.0f, pTame[e.family]->load() * 0.01f);
        const float over = std::max (0.0f, e.saliencyDb - med[e.family] + 3.0f);
        float g = e.family == sibilance ? -a * (6.0f + 1.1f * over) : e.family == consonant ? -a * (5.0f + 0.9f * over) : -a * (14.0f + 0.5f * over);
        e.gainDb = std::max (-30.0f, g);
    }
}

void SoloProcessor::publish()
{
    std::vector<Span> spans;
    const double fs = captureRate();
    {
        const juce::ScopedLock sl (evLock);
        for (const auto& e : evts)
        {
            if (e.muted || std::abs (e.gainDb) < 0.05f) continue;
            spans.push_back ({ e.start, e.end, juce::Decibels::decibelsToGain (e.gainDb), (int) std::lround (fs * (e.family == breath ? 0.02 : 0.005)) });
        }
    }
    manual.publish (std::move (spans));
}

void SoloProcessor::setEventGain (int i, float db)
{
    if (i < 0 || i >= (int) evts.size()) return;
    {
        const juce::ScopedLock sl (evLock);
        evts[(size_t) i].gainDb = juce::jlimit (-30.0f, 0.0f, db);
        evts[(size_t) i].manual = true;
    }
    ++evRevision;
    publish();
}

void SoloProcessor::resetEvent (int i)
{
    if (i < 0 || i >= (int) evts.size()) return;
    {
        const juce::ScopedLock sl (evLock);
        evts[(size_t) i].manual = false;
        evts[(size_t) i].muted = false;
    }
    applyTame();
    ++evRevision;
    publish();
}

void SoloProcessor::setEventMuted (int i, bool m)
{
    if (i < 0 || i >= (int) evts.size()) return;
    {
        const juce::ScopedLock sl (evLock);
        evts[(size_t) i].muted = m;
    }
    ++evRevision;
    publish();
}

void SoloProcessor::listenTo (int i)
{
    if (i < 0 || i >= (int) evts.size() || store.empty()) return;
    const auto& e = evts[(size_t) i];
    const double fs = captureRate();
    const auto ctx = (juce::int64) (fs * (e.family == breath ? 0.12 : 0.07));
    const juce::int64 a = e.start - ctx, b = e.end + ctx;
    std::vector<float> x ((size_t) (b - a));
    store.read (a, x.data(), (int) x.size());
    // son gain, avec les mêmes fondus qu'à la lecture
    if (! e.muted && std::abs (e.gainDb) > 0.05f)
    {
        GainTable t;
        t.spans.push_back ({ e.start, e.end, juce::Decibels::decibelsToGain (e.gainDb), (int) std::lround (fs * (e.family == breath ? 0.02 : 0.005)) });
        size_t cur = 0;
        for (size_t k = 0; k < x.size(); ++k) x[k] *= t.at (a + (juce::int64) k, cur);
    }
    manual.play (x.data(), (int) x.size());
    listening = i;
}

juce::File SoloProcessor::captureFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Onirique").getChildFile ("ONIRIQUE DE-ESSER").getChildFile ("Captures").getChildFile (captureId + ".ocap");
}

void SoloProcessor::timerCallback()
{
#if ONIRIQUE_UI_TEST
    // captures : ONIRIQUE_UI_WAV = une voix posée comme si elle venait d'être capturée (au temps 0), puis analysée
    if (! uiWavDone)
    {
        uiWavDone = true;
        const auto path = juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_WAV", {});
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        if (path.isNotEmpty())
            if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (juce::File (path)) })
            {
                const int len = (int) std::min<juce::int64> (rd->lengthInSamples, (juce::int64) (rd->sampleRate * 40));
                juce::AudioBuffer<float> b ((int) rd->numChannels, len);
                rd->read (&b, 0, len, 0, true, true);
                std::vector<float> m ((size_t) len);
                for (int c = 0; c < b.getNumChannels(); ++c) for (int i = 0; i < len; ++i) m[(size_t) i] += b.getSample (c, i) / (float) b.getNumChannels();
                if (rate <= 0.0) rate = rd->sampleRate;
                store.write (0, m.data(), len);
                dirtyFrom = 0; dirtyTo = len; lastCaptureMs = 0;
            }
    }
#endif
    // LIVE (en AUTO seulement) : la latence courte ; le DAW est prévenu quand elle change
    if (ready.load() && liveNow() != liveReported)
    {
        liveReported = liveNow();
        latency = liveReported ? latencyLive : latencyMix;
        latencyNow.store (latency);
        setLatencySamples (latency);
    }
    manual.collect();
    if (! manual.isPlaying()) listening = -1;

    // la capture : la Fifo vidée dans la prise
    // seulement ce qui est nouveau : un passage déjà gardé et identique (une réécoute) n'est ni réécrit ni réanalysé
    bool got = false;
    fifo.drain ([this, &got] (juce::int64 pos, const float* x, int n)
    {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = std::max (pk, std::abs (x[i]));
        if (store.covers (pos, n))
        {
            float old[onirique::deess::Fifo::packet];
            store.read (pos, old, n);
            // la même prise, même plus forte ou moins forte (le volume de la piste a bougé) : rien à réenregistrer. On compare la
            // FORME du son (corrélation), pas son niveau ; deux silences sont pareils aussi.
            double xo = 0.0, xx = 0.0, oo = 0.0;
            for (int i = 0; i < n; ++i) { xo += (double) x[i] * old[i]; xx += (double) x[i] * x[i]; oo += (double) old[i] * old[i]; }
            const double quiet = 1.0e-8 * n;   // sous ~ -80 dBFS
            const bool bothQuiet = std::max (xx, oo) < quiet;
            const bool sameShape = xx > 1.0e-14 * n && oo > 1.0e-14 * n && xo / std::sqrt (xx * oo) > 0.97;
            if (bothQuiet || sameShape) { ++dbgSkipped; return; }
        }
        else if (pk < 3.0e-5f) { ++dbgSkipped; return; }   // du silence jamais enregistré : inutile de le garder
        store.write (pos, x, n);
        ++dbgWritten;
        dirtyFrom = dirtyFrom < 0 ? pos : std::min (dirtyFrom, pos);
        dirtyTo = std::max (dirtyTo, pos + n);
        got = true;
    });
    const auto now = juce::Time::getMillisecondCounter();
    if (got) { lastCaptureMs = now; storeDirty = true; }

    // l'analyse : pendant la capture toutes les 1,5 s, et tout de suite quand elle s'arrête
    takeResults();
    if (dirtyFrom >= 0 && ! worker.busy.load())
    {
        const bool idle = lastCaptureMs == 0 || now - lastCaptureMs > 400;
        if (idle || now - lastLaunchMs > 1500) launchAnalysis();
    }

    // la prise enregistrée à part, une fois la capture au repos
    if (storeDirty && (lastCaptureMs == 0 || now - lastCaptureMs > 2000) && ! store.empty())
    {
        store.save (captureFile());
        storeDirty = false;
    }

    // TOUT RÉDUIRE
    bool tameMoved = false;
    for (int f = 0; f < 3; ++f) if (pTame[f]->load() != tameSeen[f]) { tameSeen[f] = pTame[f]->load(); tameFresh[f] = true; tameMoved = true; }
    if (tameMoved) { applyTame(); ++evRevision; publish(); }
}

void SoloProcessor::launchAnalysis()
{
    // le morceau continu qui contient le début de la zone à revoir
    const auto spans = store.spans();
    std::pair<juce::int64, juce::int64> sp { -1, -1 };
    for (auto s : spans) if (s.second > dirtyFrom) { sp = s; break; }
    if (sp.first < 0) { dirtyFrom = dirtyTo = -1; return; }
    const double fs = captureRate();
    const auto ctx = (juce::int64) (fs * 2.0), tail = (juce::int64) (fs * 0.4);
    const bool stillCapturing = capturing.load() && lastCaptureMs != 0 && juce::Time::getMillisecondCounter() - lastCaptureMs < 400;
    const juce::int64 from = std::max (sp.first, dirtyFrom - ctx), to = std::min (sp.second, std::max (dirtyTo, dirtyFrom) + (stillCapturing ? 0 : tail));
    if (to - from < (juce::int64) (fs * 0.3)) { if (! stillCapturing) dirtyFrom = dirtyTo = -1; return; }
    // on garde les résultats sauf près des bords (la référence de niveau y manque), sauf aux vrais bords de la prise
    const juce::int64 keepFrom = from == sp.first ? from : std::max (from, dirtyFrom - (juce::int64) (fs * 0.5));
    const juce::int64 keepTo = (to == sp.second && ! stillCapturing) ? to : to - tail;
    if (keepTo <= keepFrom) return;
    worker.samples.resize ((size_t) (to - from));
    store.read (from, worker.samples.data(), (int) (to - from));
    worker.offset = from; worker.keepFrom = keepFrom; worker.keepTo = keepTo; worker.fs = fs;
    worker.cancel.store (false);
    worker.done.store (false);
    worker.busy.store (true);
    analysing = true;
    lastLaunchMs = juce::Time::getMillisecondCounter();
    // la suite de la zone à revoir
    if (keepTo >= dirtyTo) dirtyFrom = dirtyTo = -1; else dirtyFrom = keepTo;
    worker.wake.signal();
}

void SoloProcessor::Worker::run()
{
    while (! threadShouldExit())
    {
        wake.wait (500);
        if (threadShouldExit()) break;
        if (! busy.load() || done.load()) continue;
        auto ev = Analyzer::run (samples.data(), (int) samples.size(), fs, offset, &cancel);
        {
            const juce::ScopedLock sl (lock);
            result = std::move (ev);
        }
        done.store (true);
    }
}

void SoloProcessor::takeResults()
{
    if (! worker.busy.load() || ! worker.done.load()) return;
    std::vector<Event> fresh;
    {
        const juce::ScopedLock sl (worker.lock);
        fresh = std::move (worker.result);
    }
    const bool cancelled = worker.cancel.load();
    worker.done.store (false);
    worker.busy.store (false);
    analysing = dirtyFrom >= 0;
    if (cancelled) return;
    const auto kf = worker.keepFrom, kt = worker.keepTo;
    auto centre = [] (const Event& e) { return (e.start + e.end) / 2; };
    {
        const juce::ScopedLock sl (evLock);
        // les anciens événements de la zone revue partent ; leurs réglages passent aux nouveaux qui les recouvrent
        std::vector<Event> old, keep;
        for (auto& e : evts) (centre (e) >= kf && centre (e) < kt ? old : keep).push_back (e);
        for (auto& e : fresh)
        {
            if (centre (e) < kf || centre (e) >= kt) continue;
            for (const auto& o : old)
            {
                const auto ov = std::min (o.end, e.end) - std::max (o.start, e.start);
                if (o.family == e.family && ov > (e.end - e.start) / 2) { e.gainDb = o.gainDb; e.manual = o.manual; e.muted = o.muted; break; }
            }
            keep.push_back (e);
        }
        std::sort (keep.begin(), keep.end(), [] (const Event& p, const Event& q) { return p.start < q.start; });
        evts = std::move (keep);
    }
    applyTame();
    ++evRevision;
    publish();
}

//==============================================================================
void SoloProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    presets.writeState (state);
    juce::ValueTree man ("MANUAL");
    man.setProperty ("capture", store.empty() ? juce::String() : captureId, nullptr);
    man.setProperty ("rate", captureRate(), nullptr);
    man.setProperty ("hoverListen", hoverListen, nullptr);
    man.setProperty ("autoCapture", capturing.load(), nullptr);
#if ONIRIQUE_UI_TEST
    man.setProperty ("dbg", juce::String (dbgBlocks.load()) + "/" + juce::String (dbgRolling.load()) + " ecrits=" + juce::String (dbgWritten) + " sautes=" + juce::String (dbgSkipped) + " cap=" + juce::String ((int) capturing.load())
                            + " spans=" + juce::String ((int) store.spans().size())
                            , nullptr);
#endif
    {
        const juce::ScopedLock sl (evLock);
        for (const auto& e : evts)
        {
            juce::ValueTree v ("EVENT");
            v.setProperty ("a", juce::String (e.start), nullptr); v.setProperty ("b", juce::String (e.end), nullptr);
            v.setProperty ("f", e.family, nullptr); v.setProperty ("l", e.letter, nullptr);
            v.setProperty ("lv", e.levelDb, nullptr); v.setProperty ("sal", e.saliencyDb, nullptr);
            v.setProperty ("g", e.gainDb, nullptr);
            if (e.manual) v.setProperty ("m", true, nullptr);
            if (e.muted) v.setProperty ("x", true, nullptr);
            man.appendChild (v, nullptr);
        }
    }
    state.removeChild (state.getChildWithName ("MANUAL"), nullptr);
    state.appendChild (man, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void SoloProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    auto man = state.getChildWithName ("MANUAL");
    state.removeChild (man, nullptr);
    presets.readState (state);
    apvts.replaceState (state);
    undo.clearUndoHistory();

    if (! man.isValid()) return;
    hoverListen = (bool) man.getProperty ("hoverListen", true);
    capturing.store ((bool) man.getProperty ("autoCapture", true));
    std::vector<Event> got;
    const double was = (double) man.getProperty ("rate", 0.0), now = captureRate();
    const double k = was > 0.0 && rate > 0.0 && std::abs (was - now) > 1.0 ? now / was : 1.0;   // le projet rouvert à une autre fréquence
    for (auto v : man)
    {
        Event e;
        e.start = (juce::int64) ((double) v["a"].toString().getLargeIntValue() * k);
        e.end = (juce::int64) ((double) v["b"].toString().getLargeIntValue() * k);
        e.family = juce::jlimit (0, 2, (int) v["f"]); e.letter = v["l"].toString();
        e.levelDb = (float) v["lv"]; e.saliencyDb = (float) v["sal"]; e.gainDb = (float) v["g"];
        e.manual = (bool) v.getProperty ("m", false); e.muted = (bool) v.getProperty ("x", false);
        got.push_back (e);
    }
    {
        const juce::ScopedLock sl (evLock);
        evts = std::move (got);
    }
    const juce::String id = man["capture"].toString();
    store.clear();
    if (id.isNotEmpty())
    {
        captureId = id;
        store.load (captureFile());   // absent (autre ordinateur, fichier effacé) : les réglages s'appliquent quand même
    }
    storeDirty = false;
    for (auto& t : tameSeen) t = -1.0f;   // TOUT RÉDUIRE relu (les événements à la main gardent leur gain)
    ++evRevision;
    publish();
}

juce::AudioProcessorEditor* SoloProcessor::createEditor() { return new SoloEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SoloProcessor(); }
