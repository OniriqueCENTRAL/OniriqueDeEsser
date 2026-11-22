#include "PluginEditor.h"
#include "OniriqueWindowSize.h"
#include "BinaryData.h"

namespace
{
const juce::String pluginName = SOLO_NAME;
using R = juce::Rectangle<float>;
juce::String fr (const char* s) { return juce::String::fromUTF8 (s); }

namespace layout
{
const R top { 20, 70, 1140, 34 };          // la rangée du haut
const R view { 20, 116, 1140, 344 };       // la vue
const R bottom { 20, 474, 1140, 206 };     // la rangée du bas
const R tameBox { 20, 474, 450, 206 }, inspectorBox { 486, 474, 380, 206 }, optionsBox { 882, 474, 278, 206 };
const R reductionBox { 960, 474, 200, 206 };
}
} // namespace

SoloEditor::SoloEditor (SoloProcessor& proc)
    : AudioProcessorEditor (proc), processor (proc),
      mode (p (params::mode), { "AUTO", "MANUEL" }),
      tS (p (params::targetS), "S"), tT (p (params::targetT), "T"), tCH (p (params::targetCH), "CH"), tF (p (params::targetF), "F"), tK (p (params::targetK), "K"),
      listen (p (params::listen), "LISTEN"), live (p (params::live)),
      threshold (p (params::threshold), "THRESHOLD", knobLook), range (p (params::range), "RANGE", knobLook),
      freq (p (params::freq), "FREQ", knobLook), clarity (p (params::clarity), "CLARITY", knobLook),
      autoView (proc),
      famSib (dui::familyName (0)), famCons (dui::familyName (1)), famBreath (dui::familyName (2)),
      viewCols ("COLONNES"), viewLine ("TIMELINE"), hoverListen (fr ("ÉCOUTE AU SURVOL")), clear (fr ("EFFACER LA CAPTURE")),
      capture (proc), manualView (proc),
      tameSib (p (params::tameSib), 0), tameCons (p (params::tameCons), 1), tameBreath (p (params::tameBreath), 2),
      inspector (proc),
      bar (proc.presets, proc.apvts, "DE-ESSER", {}, baseWidth, baseWidth)
{
    onirique::look::load();
    setLookAndFeel (&menuLook);
    bar.onThemeChanged = [this]
    {
        renderBackground();
        std::function<void (juce::Component&)> all = [&all] (juce::Component& k) { k.repaint(); for (auto* ch : k.getChildren()) all (*ch); };
        all (*this);
    };
    {
        int size = 0;
        if (const char* data = BinaryData::getNamedResource ("logo_png", size))
            logo = juce::ImageFileFormat::loadFrom (data, (size_t) size);
    }

    addAndMakeVisible (mode);
    mode.setTooltip (fr ("AUTO : un de-esser classique, réglé par le THRESHOLD. MANUEL : capture ta prise et règle chaque S, chaque T, chaque respiration à la main."));
    // AUTO
    for (juce::Component* c : std::initializer_list<juce::Component*> { &tS, &tT, &tCH, &tF, &tK, &listen, &live, &threshold, &range, &freq, &clarity, &autoView })
        addChildComponent (c);
    live.setTooltip (fr ("LIVE : pour enregistrer en chantant à travers le plugin. Presque pas de latence (3 ms) ; le tri entre S, T, CH, F et K se fait sur les premières ms du son, un peu moins précis. Décoché (mix) : le tri écoute 30 ms du son pour ne toucher que ce que tu as coché."));
    tS.setTooltip (fr ("S : les S et les Z, à la fréquence FREQ."));
    tT.setTooltip (fr ("T : les attaques de T, vers 4,2 kHz."));
    tCH.setTooltip (fr ("CH : les CH et les J, vers 3,2 kHz."));
    tF.setTooltip (fr ("F : le souffle aigu (F, air), vers 9 kHz."));
    tK.setTooltip (fr ("K : les attaques de K, vers 4 kHz (plus sourdes que les T)."));
    listen.setTooltip (fr ("LISTEN : n'entendre que ce qui est retiré."));
    threshold.setTip (fr ("THRESHOLD : au-dessus de ce niveau, le son est baissé. Plus bas, plus de S retirés. Tu peux aussi glisser la ligne dans la vue."));
    range.setTip (fr ("RANGE : la réduction la plus forte permise (dB)."));
    freq.setTip (fr ("FREQ : où se trouvent tes S (3 à 10 kHz). Plus grave pour une voix grave, plus aigu pour une voix claire."));
    clarity.setTip (fr ("CLARITY : rend de l'éclat et de l'air après coup, sans faire revenir les S."));

    // MANUEL
    for (juce::Component* c : std::initializer_list<juce::Component*> { &famSib, &famCons, &famBreath, &viewCols, &viewLine, &capture, &manualView,
                                                                         &tameSib, &tameCons, &tameBreath, &inspector, &hoverListen, &clear })
        addChildComponent (c);
    famSib.setDot (dui::familyColour (0)); famCons.setDot (dui::familyColour (1)); famBreath.setDot (dui::familyColour (2));
    famSib.setTooltip (fr ("Les S, Z, CH et F."));
    famCons.setTooltip (fr ("Les P, B (le coup de grave), T et K."));
    famBreath.setTooltip (fr ("Les respirations."));
    famSib.onClick = [this] { pickFamily (0); };
    famCons.onClick = [this] { pickFamily (1); };
    famBreath.onClick = [this] { pickFamily (2); };
    tameSib.onPick = [this] { pickFamily (0); };
    tameCons.onPick = [this] { pickFamily (1); };
    tameBreath.onPick = [this] { pickFamily (2); };
    viewCols.onClick = [this] { manualView.timeline = false; viewCols.setLit (true); viewLine.setLit (false); manualView.repaint(); };
    viewLine.onClick = [this] { manualView.timeline = true; viewCols.setLit (false); viewLine.setLit (true); manualView.repaint(); };
    viewCols.setLit (true);
    viewCols.setTooltip (fr ("Une colonne par événement de la famille choisie."));
    viewLine.setTooltip (fr ("Toute la prise, les événements à leur place."));
    capture.setTooltip (fr ("CAPTURE AUTO : à chaque lecture, ce qui est nouveau (une piste ajoutée, une prise changée) est enregistré et découpé en événements. "
                            "Ce qui est déjà gardé n'est pas réenregistré. Clique pour couper ou rallumer."));
    tameSib.setTooltip (fr ("TOUT RÉDUIRE : baisse tous les S d'un coup, les plus forts davantage. Ceux que tu as réglés à la main ne bougent plus."));
    tameCons.setTooltip (fr ("TOUT RÉDUIRE : baisse toutes les plosives et attaques, les plus fortes davantage."));
    tameBreath.setTooltip (fr ("TOUT RÉDUIRE : baisse toutes les respirations."));
    hoverListen.setLit (processor.hoverListen);
    hoverListen.onClick = [this] { processor.hoverListen = ! processor.hoverListen; hoverListen.setLit (processor.hoverListen); };
    hoverListen.setTooltip (fr ("Entendre chaque événement en le survolant."));
    clear.onClick = [this]
    {
        juce::Component::SafePointer<SoloEditor> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, fr ("Effacer la capture ?"),
                                            fr ("La prise capturée et tous les réglages des événements seront effacés."), fr ("Effacer"), fr ("Annuler"), nullptr,
                                            juce::ModalCallbackFunction::create ([safe] (int r) { if (safe != nullptr && r == 1) { safe->processor.clearCapture(); safe->manualView.select (-1); } }));
    };
    manualView.onSelect = [this] { inspector.show (manualView.selected); };
    pickFamily (0);

    addAndMakeVisible (bar);
    bar.setBrand (300, [this] (juce::Graphics& g, juce::Rectangle<float> area) { paintTitle (g, area); });
    addMouseListener (this, true);

#if SOLO_LICENCE
    licenseOverlay = std::make_unique<onirique::license::Overlay> (pluginName, processor.license);
    addAndMakeVisible (*licenseOverlay);
   #if ONIRIQUE_UI_TEST
    if (juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("nolicense"))
        licenseOverlay->setVisible (false);
   #endif
#endif

    auto heightFor = [] (int w) { return juce::roundToInt ((float) w * (float) baseHeight / (float) baseWidth); };
    const int minW = juce::roundToInt (baseWidth * 0.6f), maxW = baseWidth * 2;
    const auto size = onirique::window::openingSize (processor.apvts.state, pluginName, { baseWidth, heightFor (baseWidth) },
                                                     { minW, heightFor (minW) }, { maxW, heightFor (maxW) });
    setResizable (true, true);
    setResizeLimits (minW, heightFor (minW), maxW, heightFor (maxW));
    getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
    setSize (size.x, heightFor (size.x));
    sizeReady = true;
    onirique::window::remember (processor.apvts.state, getWidth(), getHeight());
    showMode();
    startTimerHz (30);

#if ONIRIQUE_UI_TEST
    // captures : ONIRIQUE_UI_VIEW = manuel | timeline (la vue MANUEL, avec la prise ONIRIQUE_UI_WAV analysée)
    {
        const auto v = juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_VIEW", {});
        if (v.isNotEmpty())
        {
            p (params::mode).setValueNotifyingHost (1.0f);
            if (v.contains ("timeline")) viewLine.onClick ? viewLine.onClick() : void();
            if (v.contains ("cons")) pickFamily (1);
            if (v.contains ("breath")) pickFamily (2);
            if (v.contains ("select"))
            {
                juce::Component::SafePointer<SoloEditor> safe (this);
                juce::Timer::callAfterDelay (2600, [safe]
                {
                    if (safe == nullptr) return;
                    const auto list = safe->manualView.visible();
                    if (list.size() > 3) { safe->manualView.select (list[3]); safe->processor.setEventGain (list[3], -9.0f); }
                });
            }
        }
    }
#endif
    if (auto* png = std::getenv ("ONIRIQUE_UI_PNG"))
    {
        const juce::String path (png);
        juce::Component::SafePointer<SoloEditor> safe (this);
        juce::Timer::callAfterDelay (3500, [safe, path]
        {
            if (safe == nullptr) return;
            juce::Image img (juce::Image::ARGB, safe->getWidth() * 2, safe->getHeight() * 2, true, juce::SoftwareImageType());
            { juce::Graphics g (img); g.addTransform (juce::AffineTransform::scale (2.0f)); safe->paintEntireComponent (g, true); }
            juce::File f (path); f.deleteFile();
            if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
        });
    }
}

SoloEditor::~SoloEditor()
{
    stopTimer();
    processor.listenStop();
    juce::PopupMenu::dismissAllActiveMenus();
    removeMouseListener (this);
    setLookAndFeel (nullptr);
    onirique::window::rememberOnThisComputer (pluginName, getWidth(), getHeight());
}

void SoloEditor::pickFamily (int f)
{
    manualView.family = f;
    famSib.setLit (f == 0); famCons.setLit (f == 1); famBreath.setLit (f == 2);
    tameSib.setSelected (f == 0); tameCons.setSelected (f == 1); tameBreath.setSelected (f == 2);
    if (manualView.selected >= 0 && manualView.selected < (int) processor.events().size() && processor.events()[(size_t) manualView.selected].family != f)
        manualView.select (-1);
    manualView.repaint();
}

void SoloEditor::showMode()
{
    const bool man = processor.isManual();
    manualShown = man;
    for (juce::Component* c : std::initializer_list<juce::Component*> { &tS, &tT, &tCH, &tF, &tK, &listen, &live, &threshold, &range, &freq, &clarity, &autoView })
        c->setVisible (! man);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &famSib, &famCons, &famBreath, &viewCols, &viewLine, &capture, &manualView,
                                                                         &tameSib, &tameCons, &tameBreath, &inspector, &hoverListen, &clear })
        c->setVisible (man);
    if (! man) processor.listenStop();
    renderBackground();
    repaint();
}

void SoloEditor::timerCallback()
{
    if (onirique::look::changedSince (themeSeen) && bar.onThemeChanged) bar.onThemeChanged();
    if (processor.isManual() != manualShown) showMode();
    if (manualShown)
    {
        manualView.tick();
        int n[3] {};
        for (const auto& e : processor.events()) n[e.family]++;
        tameSib.setCount (n[0]); tameCons.setCount (n[1]); tameBreath.setCount (n[2]);
        famSib.setText (dui::familyName (0) + "  " + juce::String (n[0]));
        famCons.setText (dui::familyName (1) + "  " + juce::String (n[1]));
        famBreath.setText (dui::familyName (2) + "  " + juce::String (n[2]));
        if (processor.eventsRevision() != inspectorSeen) { inspectorSeen = processor.eventsRevision(); inspector.refresh(); }   // l'inspecteur suit
        repaint (layout::optionsBox.transformedBy (juce::AffineTransform::scale (scale())).toNearestInt());
    }
    else
    {
        autoView.tick();
        const float gr = processor.autoEngine().consumeReduction();
        const float target = gr;
        grShown = target > grShown ? target : grShown + 0.25f * (target - grShown);
        repaint (layout::reductionBox.transformedBy (juce::AffineTransform::scale (scale())).toNearestInt());
    }
}

//==============================================================================
void SoloEditor::resized()
{
    const float s = scale();
    auto place = [s] (juce::Component& c, R r) { c.setBounds (R (r.getX() * s, r.getY() * s, r.getWidth() * s, r.getHeight() * s).toNearestInt()); };
    auto knob = [&] (ui::Knob& k, juce::Point<float> c, juce::Point<float> sz) { place (k, R (sz.x, sz.y).withCentre (c)); k.setScale (s); };
    const auto t = layout::top;

    place (mode, R (t.getX(), t.getY(), 230, t.getHeight())); mode.setScale (s);
    // AUTO
    float x = t.getRight() - 96 - 5 * 64;
    for (auto* sw : { &tS, &tT, &tCH, &tF, &tK }) { place (*sw, R (x, t.getY(), 60, t.getHeight())); sw->setScale (s); x += 64; }
    place (listen, R (t.getRight() - 96, t.getY(), 96, t.getHeight())); listen.setScale (s);
    place (live, R (270, t.getY(), 200, t.getHeight())); live.setScale (s);
    place (autoView, layout::view); autoView.setScale (s);
    const float ky = layout::bottom.getCentreY() + 2;
    knob (threshold, { 150, ky }, { 160, 190 });
    knob (range, { 390, ky }, { 150, 180 });
    knob (freq, { 600, ky }, { 150, 180 });
    knob (clarity, { 810, ky }, { 150, 180 });
    // MANUEL
    x = 266;
    for (auto* f : { &famSib, &famCons, &famBreath }) { place (*f, R (x, t.getY(), 162, t.getHeight())); f->setScale (s); x += 167; }
    place (viewCols, R (776, t.getY(), 104, t.getHeight())); viewCols.setScale (s);
    place (viewLine, R (884, t.getY(), 104, t.getHeight())); viewLine.setScale (s);
    place (capture, R (1000, t.getY(), 160, t.getHeight())); capture.setScale (s);
    place (manualView, layout::view); manualView.setScale (s);
    {
        const auto b = layout::tameBox.reduced (8, 34);
        const float h = b.getHeight() / 3.0f;
        int k = 0;
        for (auto* ts : { &tameSib, &tameCons, &tameBreath }) { place (*ts, R (b.getX(), b.getY() + h * (float) k++, b.getWidth(), h)); ts->setScale (s); }
    }
    place (inspector, layout::inspectorBox); inspector.setScale (s);
    {
        const auto o = layout::optionsBox.reduced (16, 16);
        place (hoverListen, R (o.getX(), o.getY() + 26, o.getWidth(), 32)); hoverListen.setScale (s);
        place (clear, R (o.getX(), o.getY() + 66, o.getWidth(), 32)); clear.setScale (s);
    }

#if SOLO_LICENCE
    const int pillW = licenseOverlay != nullptr ? licenseOverlay->pillWidth() : 0;
#else
    const int pillW = 0;
#endif
    bar.setPillWidth (pillW);
    bar.place (s, getLocalBounds());
#if SOLO_LICENCE
    if (licenseOverlay != nullptr)
        licenseOverlay->fit (getLocalBounds(), s, bar.getPillArea(), juce::Justification::centredRight);
#endif
    renderBackground();
    if (sizeReady) onirique::window::remember (processor.apvts.state, getWidth(), getHeight());
}

void SoloEditor::renderBackground()
{
    const float k = juce::jmax (1.0f, (float) juce::Component::getApproximateScaleFactorForComponent (this));
    background = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) getWidth() * k)),
                              juce::jmax (1, juce::roundToInt ((float) getHeight() * k)), true);
    juce::Graphics g (background);
    g.drawImageAt (onirique::da::backdrop (background.getWidth(), background.getHeight(), scale() * k), 0, 0);
    g.addTransform (juce::AffineTransform::scale (scale() * k));
    using namespace ui;
    const auto cap = font (Face::display, 9.5f, 0.16f);
    if (processor.isManual())
    {
        onirique::da::glass (g, layout::tameBox, 16.0f, 1.0f);
        text (g, fr ("TOUT RÉDUIRE"), R (layout::tameBox.getX() + 18, layout::tameBox.getY() + 10, 200, 18), cap, bone, juce::Justification::centredLeft);
        text (g, fr ("les plus forts davantage"), R (layout::tameBox.getRight() - 218, layout::tameBox.getY() + 10, 200, 18), font (Face::body, 9.5f), haze,
              juce::Justification::centredRight);
        onirique::da::glass (g, layout::optionsBox, 16.0f, 1.0f);
        text (g, "OPTIONS", R (layout::optionsBox.getX() + 18, layout::optionsBox.getY() + 10, 200, 18), cap, bone, juce::Justification::centredLeft);
    }
    else
    {
        onirique::da::glass (g, layout::bottom.withWidth (920), 16.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        for (float xx : { 270.0f, 495.0f, 705.0f }) g.fillRect (xx, layout::bottom.getY() + 30, 1.0f, layout::bottom.getHeight() - 60);
        onirique::da::glass (g, layout::reductionBox, 16.0f, 1.0f);
    }
}

void SoloEditor::paintTitle (juce::Graphics& g, juce::Rectangle<float> area) const
{
    using namespace ui;
    const float cy = 25.0f;
    float x = area.getX();
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    if (logo.isValid()) g.drawImage (logo, R (x, cy - 13, 28, 26), juce::RectanglePlacement::centred);
    x += 38;
    text (g, "ONIRIQUE", R (x, cy - 16, 120, 14), font (Face::display, 11.0f, 0.14f), bone.withAlpha (0.75f), juce::Justification::centredLeft);
    const juce::String name = "DE-ESSER";
    text (g, name, R (x - 1, cy - 4, 180, 22), font (Face::display, 20.0f, 0.1f), accent, juce::Justification::centredLeft);
    const float w = juce::GlyphArrangement::getStringWidth (font (Face::display, 20.0f, 0.1f), name);
    text (g, fr ("S · T · CH · RESPIRATIONS"), R (x + w + 10, cy + 1, 170, 14), font (Face::body, 8.5f, 0.14f), dim, juce::Justification::centredLeft);
}

void SoloEditor::paint (juce::Graphics& g)
{
    if (background.isValid()) g.drawImage (background, getLocalBounds().toFloat());
    else g.fillAll (ui::ink);
    using namespace ui;
    g.addTransform (juce::AffineTransform::scale (scale()));
    if (! processor.isManual())
    {
        // la réduction en cours
        const auto b = layout::reductionBox.reduced (18, 16);
        text (g, fr ("RÉDUCTION"), b.withHeight (18), font (Face::display, 9.5f, 0.16f), bone, juce::Justification::centredLeft);
        text (g, (grShown > 0.05f ? "-" : "") + juce::String (grShown, 1), R (b.getX(), b.getY() + 40, b.getWidth(), 50), font (Face::display, 34.0f, 0.02f), bone);
        text (g, "dB", R (b.getX(), b.getY() + 88, b.getWidth(), 16), font (Face::mono, 10.0f), haze);
        const R track (b.getX(), b.getBottom() - 16, b.getWidth(), 8);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (track, 4.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (track.withWidth (std::max (8.0f, track.getWidth() * juce::jlimit (0.0f, 1.0f, grShown / 18.0f))), 4.0f);
    }
    else
    {
        // l'état de la capture
        const auto o = layout::optionsBox.reduced (18, 16);
        const auto& st = processor.capture();
        juce::String line1, line2;
        if (st.empty()) { line1 = fr ("Aucune prise capturée"); line2 = processor.isCapturing() ? fr ("lance la lecture dans le DAW") : fr ("capture coupée"); }
        else
        {
            double secs = 0; for (auto sp : st.spans()) secs += (double) (sp.second - sp.first) / processor.captureRate();
            line1 = fr ("Prise : ") + dui::timeText (secs);
            line2 = processor.isRecording() ? fr ("enregistre ce qui est nouveau")
                  : processor.isAnalysing() ? fr ("analyse en cours…") : juce::String (processor.events().size()) + fr (" événements");
        }
        text (g, line1, R (o.getX(), o.getBottom() - 44, o.getWidth(), 18), font (Face::mono, 10.5f), bone, juce::Justification::centredLeft);
        text (g, line2, R (o.getX(), o.getBottom() - 24, o.getWidth(), 18), font (Face::body, 10.0f), processor.isRecording() ? accent : haze,
              juce::Justification::centredLeft);
    }
}
