#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/Controls.h"
#include "ui/OniriqueBar.h"
#include "ui/DeessViews.h"
#if SOLO_LICENCE
 #include "License/LicenseOverlay.h"
#endif

//==============================================================================
//  L'interface : la barre ONIRIQUE (presets, IN, A/B, licence), puis
//    en haut     AUTO | MANUEL ; en AUTO les sons traités (S, T, CH, F) et LISTEN ; en MANUEL les familles
//                (SIBILANCES, CONSONNES, RESPIRATIONS), COLONNES | TIMELINE et CAPTURE ;
//    au centre   la vue (ce qui est retiré en direct, ou les événements de la prise) ;
//    en bas      AUTO : THRESHOLD, RANGE, FREQ, CLARITY et la réduction ; MANUEL : TOUT RÉDUIRE (une dose par famille),
//                l'événement choisi, les options (écoute au survol, effacer la capture).
//==============================================================================
class SoloEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SoloEditor (SoloProcessor&);
    ~SoloEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override { processor.undo.beginNewTransaction(); }

    static constexpr int baseWidth = 1180, baseHeight = 700;

private:
    juce::RangedAudioParameter& p (const juce::String& id) { return *processor.apvts.getParameter (id); }
    void renderBackground();
    void paintTitle (juce::Graphics&, juce::Rectangle<float> area) const;
    void timerCallback() override;
    void showMode();
    void pickFamily (int f);
    float scale() const { return (float) getWidth() / (float) baseWidth; }

    SoloProcessor& processor;
    ui::FontScope fonts;
    ui::KnobLook knobLook;
    ui::MenuLook menuLook;
    juce::Image background, logo;
    juce::TooltipWindow tooltips { this, 600 };

    ui::Segments mode;
    // AUTO
    ui::Switch tS, tT, tCH, tF, tK, listen;
    dui::LiveCheck live;
    ui::Knob threshold, range, freq, clarity, sibPan;
    dui::PanSlider sibPanManual;
    dui::AutoView autoView;
    float grShown = 0.0f;
    // MANUEL
    dui::Pill famSib, famCons, famBreath, viewCols, viewLine, hoverListen, clear;
    dui::CaptureButton capture;
    dui::ManualView manualView;
    dui::TameSlider tameSib, tameCons, tameBreath;
    dui::Inspector inspector;
    bool manualShown = false;
    int inspectorSeen = -1;

    onirique::nuit::Bar bar;
    bool sizeReady = false;
    int themeSeen = -1;
#if SOLO_LICENCE
    std::unique_ptr<onirique::license::Overlay> licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoloEditor)
};
