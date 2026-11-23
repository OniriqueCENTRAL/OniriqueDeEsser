#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Params.h"
#include "Presets.h"
#include "dsp/AutoDeEsser.h"
#include "dsp/ManualEngine.h"
#include "dsp/Capture.h"
#include "dsp/EventAnalysis.h"
#if SOLO_LICENCE
 #include "License/LicenseGuard.h"
#endif

#define SOLO_NAME "ONIRIQUE DE-ESSER"
#define SOLO_SLUG "onirique-de-esser"

//==============================================================================
//  ONIRIQUE DE-ESSER : le processeur. Mono ou stéréo, latence fixe (l'anticipation du de-esser, ~3 ms), la même dans les
//  deux modes. Passer de AUTO à MANUEL se fait en fondu (30 ms).
//
//  MANUEL : la prise s'enregistre toute seule pendant que le DAW la joue (CAPTURE AUTO), seulement ce qui est nouveau ou a
//  changé ; l'analyse (fil à part) la découpe en événements au fur et à mesure ; chaque événement se règle à la main ou par TOUT RÉDUIRE. Les événements et le nom du fichier de
//  capture vont dans le projet ; l'audio capturé, dans un fichier à part (dossier ONIRIQUE DE-ESSER/Captures).
//==============================================================================
class SoloProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    SoloProcessor();
    ~SoloProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter (params::bypass); }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return SOLO_NAME; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::UndoManager undo { 30000, 60 };
    juce::AudioProcessorValueTreeState apvts;
    onirique::PresetBank presets { apvts, "DE-ESSER", solopresets::stockPresets(), solopresets::tagNames(), params::notStored() };
#if SOLO_LICENCE
    onirique::license::Guard license { SOLO_SLUG };
#endif

    // ---- pour l'interface (fil des messages)
    using Event = onirique::deess::Event;
    bool isManual() const { return apvts.getRawParameterValue (params::mode)->load() > 0.5f; }
    onirique::deess::AutoDeEsser& autoEngine() { return autoDs; }
    const onirique::deess::Store& capture() const { return store; }
    const std::vector<Event>& events() const { return evts; }
    int eventsRevision() const { return evRevision; }
    double captureRate() const { return rate > 0.0 ? rate : 44100.0; }

    bool isCapturing() const { return capturing.load(); }   // CAPTURE AUTO (allumée par défaut)
    bool isRecording() const;                                // du nouveau vient d'être enregistré
    void setCapturing (bool on);
    void clearCapture();
    bool isAnalysing() const { return analysing; }
    bool hostPlaying() const { return playing.load(); }
    juce::int64 hostPosition() const { return hostPos.load(); }   // en échantillons, ce qu'on entend (latence retirée)
    bool captureDropped() { return fifo.dropped.exchange (false); }

    void setEventGain (int index, float db);          // à la main : TOUT RÉDUIRE ne le touche plus
    void resetEvent (int index);                       // rendu à TOUT RÉDUIRE
    void setEventMuted (int index, bool muted);        // ignoré : son gain n'est plus appliqué
    void listenTo (int index);                         // l'entendre (son gain appliqué)
    void listenStop() { manual.stopPlaying(); }
    bool isListening() const { return manual.isPlaying(); }
    int listeningIndex() const { return listening; }
    bool hoverListen = true;                           // ÉCOUTE AU SURVOL

    /** Le gain proposé par TOUT RÉDUIRE pour un événement (dB). */
    float tameGain (const Event& e) const;

private:
    void process (juce::AudioBuffer<float>&, bool bypassed);
    void timerCallback() override;
    onirique::deess::AutoDeEsser::Settings readAuto() const;
    bool liveNow() const noexcept { return pLive != nullptr && pLive->load() > 0.5f && pMode->load() < 0.5f; }   // LIVE ne vaut qu'en AUTO
    void applyTame();
    void publish();
    void launchAnalysis();
    void takeResults();
    juce::File captureFile() const;

    // le travail d'analyse, sur un fil à part
    struct Worker : juce::Thread
    {
        Worker() : juce::Thread ("ONIRIQUE DE-ESSER analyse") {}
        void run() override;
        juce::WaitableEvent wake;
        juce::CriticalSection lock;
        std::vector<float> samples; juce::int64 offset = 0, keepFrom = 0, keepTo = 0; double fs = 44100.0;
        std::vector<onirique::deess::Event> result;
        std::atomic<bool> busy { false }, done { false }, cancel { false };
    } worker;

    onirique::deess::AutoDeEsser autoDs;
    onirique::deess::ManualEngine manual;
    onirique::deess::Fifo fifo;
    onirique::deess::Store store;
    std::vector<Event> evts;
    juce::CriticalSection evLock;            // l'état du projet peut être demandé depuis un autre fil
    int evRevision = 0, listening = -1;
    juce::int64 dirtyFrom = -1, dirtyTo = -1;
    juce::uint32 lastCaptureMs = 0, lastLaunchMs = 0;
    bool storeDirty = false, analysing = false;
    float tameSeen[3] { -1, -1, -1 }, tameRef[3] {};
    int tameRefCount[3] {};
    bool tameFresh[3] {};
    juce::String captureId;
    bool uiWavDone = false;
    std::atomic<int> dbgBlocks { 0 }, dbgRolling { 0 };
    int dbgWritten = 0, dbgSkipped = 0;

    double rate = 0.0;
    int latency = 0, latencyMix = 0, latencyLive = 0;
    std::atomic<int> latencyNow { 0 };   // celle du moment (LIVE en AUTO : courte), lue par le fil audio
    bool liveReported = false;
    std::atomic<bool> capturing { true }, playing { false }, ready { false };
    std::atomic<juce::int64> hostPos { 0 };
    juce::AudioBuffer<float> scratch, other;
    juce::SmoothedValue<float> modeS, bypassS;
    onirique::DelayLine dryDelay[2], dryDelayLive[2];
    std::vector<float> mono;
    std::atomic<float> *pMode = nullptr, *pBypass = nullptr, *pThr = nullptr, *pRange = nullptr, *pFreq = nullptr, *pClar = nullptr, *pListen = nullptr, *pLive = nullptr, *pPan = nullptr,
                       *pT[5] {}, *pTame[3] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoloProcessor)
};
