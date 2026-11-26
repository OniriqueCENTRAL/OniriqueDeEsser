#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "OniriqueDA.h"
#include "../PluginProcessor.h"

//==============================================================================
//  Les vues d'ONIRIQUE DE-ESSER, dans la DA ONIRIQUE (verre nuit, braise) :
//    AutoView      ce qui est retiré, en direct : le niveau de la voix, la part retirée en braise, la LETTRE de chaque
//                  retrait (S, T, CH, F), le niveau détecté et la ligne THRESHOLD qu'on glisse.
//    ManualView    les événements capturés : en COLONNES (une bande par événement, la ligne = son gain) ou en TIMELINE
//                  (la prise entière) ; survoler fait entendre, glisser baisse, double-clic rend à TOUT RÉDUIRE.
//    Inspector     l'événement choisi ; TameSlider : TOUT RÉDUIRE d'une famille ; Pill : un bouton gélule.
//==============================================================================
namespace dui
{
using R = juce::Rectangle<float>;
inline juce::String fr (const char* s) { return juce::String::fromUTF8 (s); }

// les couleurs des familles (MANUEL) et des sons (AUTO)
inline juce::Colour familyColour (int f)
{
    switch (f)
    {
        case 0:  return ui::accent;                      // sibilances : la braise
        case 1:  return juce::Colour (0xffffb35c);       // consonnes : ambre
        default: return juce::Colour (0xff8fb6d8);       // respirations : bleu nuit clair
    }
}
inline juce::Colour targetColour (int t)
{
    switch (t)
    {
        case 0:  return ui::accent;                      // S
        case 1:  return juce::Colour (0xffffb35c);       // T
        case 2:  return juce::Colour (0xffff8fbf);       // CH
        case 4:  return juce::Colour (0xffe7d27a);       // K
        default: return juce::Colour (0xff9fc3e6);       // F
    }
}
inline const char* targetName (int t) { static const char* n[] { "S", "T", "CH", "F", "K" }; return n[juce::jlimit (0, 4, t)]; }
inline juce::String familyName (int f) { return f == 0 ? fr ("SIBILANCES") : f == 1 ? fr ("CONSONNES") : fr ("RESPIRATIONS"); }

inline juce::String timeText (double seconds)
{
    const int m = (int) (seconds / 60.0);
    const double s = seconds - 60.0 * m;
    return juce::String (m) + ":" + (s < 10.0 ? "0" : "") + juce::String (s, 2);
}

/** Une pastille de lettre (S, CH, P, R…) dans la couleur de sa famille. */
inline void chip (juce::Graphics& g, R r, const juce::String& t, juce::Colour c, float u, float alpha = 1.0f)
{
    g.setColour (c.withAlpha (0.92f * alpha));
    g.fillRoundedRectangle (r, r.getHeight() * 0.35f);
    g.setColour (juce::Colours::white.withAlpha (0.25f * alpha));
    g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.35f, 1.0f);
    ui::text (g, t, r, ui::font (ui::Face::display, std::min (r.getHeight() * 0.66f, 15.0f * u), 0.02f), juce::Colour (0xff120c0e).withAlpha (alpha));
}

//==============================================================================
/** Un bouton gélule (pas un paramètre) : allumé ou non, couleur au choix. */
class Pill : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Pill (juce::String t) : label (std::move (t)) { setMouseCursor (juce::MouseCursor::PointingHandCursor); setRepaintsOnMouseActivity (true); }
    std::function<void()> onClick;
    void setLit (bool l) { if (lit != l) { lit = l; repaint(); } }
    void setText (const juce::String& t) { if (label != t) { label = t; repaint(); } }
    void setScale (float s) { u = s; repaint(); }
    void setDot (juce::Colour c) { dot = c; hasDot = true; repaint(); }
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        ui::fullPill (g, r, lit ? 1.0f : 0.0f, isMouseOver() ? 1.0f : 0.0f, u);
        auto tr = r;
        if (hasDot)
        {
            const float d = 7.0f * u;
            g.setColour (dot.withAlpha (lit ? 1.0f : 0.7f));
            g.fillEllipse (R (d, d).withCentre ({ r.getX() + r.getHeight() * 0.55f, r.getCentreY() }));
            tr = tr.withTrimmedLeft (r.getHeight() * 0.5f);
        }
        ui::text (g, label, tr, ui::font (ui::Face::body, 10.5f * u, 0.14f), ui::bone.withAlpha (lit ? 1.0f : 0.7f));
    }
    void mouseUp (const juce::MouseEvent& e) override { if (getLocalBounds().contains (e.getPosition()) && onClick) onClick(); }
private:
    juce::String label;
    bool lit = false, hasDot = false;
    juce::Colour dot;
    float u = 1.0f;
};

//==============================================================================
/** LIVE (mode AUTO) : une case à cocher, liée à son paramètre. Cochée : latence courte, pour enregistrer à travers. */
class LiveCheck : public juce::Button
{
public:
    explicit LiveCheck (juce::RangedAudioParameter& p) : juce::Button ("LIVE"), attachment (p, *this)
    {
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    void setScale (float s) { u = s; repaint(); }
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        using namespace ui;
        const auto r = getLocalBounds().toFloat();
        const bool on = getToggleState();
        const float b = 16.0f * u;
        const auto box = R (b, b).withCentre ({ r.getX() + 2.0f * u + b * 0.5f, r.getCentreY() });
        g.setColour (on ? accent : juce::Colour (0xff120c0e).withAlpha (0.85f));
        g.fillRoundedRectangle (box, 4.0f * u);
        g.setColour (on ? accent.brighter (0.3f) : bone.withAlpha (over ? 0.55f : 0.3f));
        g.drawRoundedRectangle (box.reduced (0.5f), 4.0f * u, 1.2f * u);
        if (on)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + b * 0.25f, box.getCentreY());
            tick.lineTo (box.getX() + b * 0.43f, box.getBottom() - b * 0.28f);
            tick.lineTo (box.getRight() - b * 0.22f, box.getY() + b * 0.27f);
            g.setColour (bone);
            g.strokePath (tick, juce::PathStrokeType (2.0f * u, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        const auto tr = r.withTrimmedLeft (b + 10.0f * u);
        ui::text (g, "LIVE", tr.withWidth (40.0f * u), font (Face::body, 10.5f * u, 0.14f), bone.withAlpha (on || over ? 1.0f : 0.7f), juce::Justification::centredLeft);
        ui::text (g, on ? fr ("latence 3 ms") : fr ("mix : tri précis"), tr.withTrimmedLeft (44.0f * u), font (Face::mono, 8.5f * u), haze, juce::Justification::centredLeft);
    }
private:
    juce::ButtonParameterAttachment attachment;
    float u = 1.0f;
};

//==============================================================================
/** CAPTURE AUTO (allumée par défaut) : la prise s'enregistre toute seule à la lecture. Le rond pulse quand du nouveau est
    gardé ; À JOUR quand la lecture ne lui apprend rien ; un clic la coupe ou la rallume. */
class CaptureButton : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit CaptureButton (SoloProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); setRepaintsOnMouseActivity (true); startTimerHz (30); }
    void setScale (float s) { u = s; repaint(); }
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = proc.isCapturing(), rec = on && proc.isRecording();
        ui::fullPill (g, r, on ? (rec ? 0.45f + 0.3f * pulse : 0.3f) : 0.0f, isMouseOver() ? 1.0f : 0.0f, u);
        const float d = 10.0f * u;
        const auto dotR = R (d, d).withCentre ({ r.getX() + r.getHeight() * 0.6f, r.getCentreY() });
        if (rec) onirique::da::softGlowRect (g, dotR, d * 0.5f, juce::Colour (0xffff2a3d).withAlpha (0.5f + 0.4f * pulse), 8.0f * u);
        g.setColour (on ? juce::Colour (0xffff2a3d).withAlpha (rec ? 1.0f : 0.8f) : ui::haze.withAlpha (0.5f));
        g.fillEllipse (dotR);
        const juce::String t = ! on ? fr ("CAPTURE COUPÉE") : rec ? fr ("ENREGISTRE…") : proc.hostPlaying() ? fr ("À JOUR") : fr ("CAPTURE AUTO");
        ui::text (g, t, r.withTrimmedLeft (r.getHeight() * 0.9f), ui::font (ui::Face::body, 10.5f * u, 0.14f), ui::bone.withAlpha (on ? 1.0f : 0.6f));
    }
    void mouseUp (const juce::MouseEvent& e) override { if (getLocalBounds().contains (e.getPosition())) proc.setCapturing (! proc.isCapturing()); repaint(); }
private:
    void timerCallback() override
    {
        const bool rec = proc.isCapturing() && proc.isRecording();
        if (rec) { phase += 0.12f; pulse = 0.5f + 0.5f * std::sin (phase); }
        else pulse = 0.0f;
        const int st = (proc.isCapturing() ? 1 : 0) + (rec ? 2 : 0) + (proc.hostPlaying() ? 4 : 0);
        if (rec || st != seen) { seen = st; repaint(); }
    }
    SoloProcessor& proc;
    float u = 1.0f, phase = 0.0f, pulse = 0.0f;
    int seen = -1;
};

//==============================================================================
/** PAN DES SIBILANCES (MANUEL) : une glissière de gauche à droite, le centre marqué ; double-clic : au centre. */
class PanSlider : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PanSlider (juce::RangedAudioParameter& p) : param (p), attachment (p, [this] (float v) { value = v; repaint(); })
    {
        attachment.sendInitialUpdate();
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setRepaintsOnMouseActivity (true);
    }
    void setScale (float s) { u = s; repaint(); }
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const auto c = familyColour (0);
        ui::text (g, fr ("PAN DES SIBILANCES"), R (r.getX(), r.getY(), r.getWidth(), 14 * u), ui::font (ui::Face::display, 8.5f * u, 0.14f), ui::bone,
                  juce::Justification::centredLeft);
        ui::text (g, param.getText (param.getValue(), 8), R (r.getX(), r.getY(), r.getWidth(), 14 * u), ui::font (ui::Face::mono, 10.0f * u),
                  isMouseOver() || dragging ? ui::bone : ui::haze, juce::Justification::centredRight);
        const auto tr = track();
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (tr, tr.getHeight() * 0.5f);
        const float cx = tr.getCentreX(), k = value / 100.0f, kx = cx + tr.getWidth() * 0.5f * k;
        g.setColour (ui::haze.withAlpha (0.6f));
        g.fillRect (R (1.5f * u, tr.getHeight() + 6 * u).withCentre ({ cx, tr.getCentreY() }));
        if (std::abs (k) > 0.001f)
        {
            g.setColour (c);
            g.fillRoundedRectangle (R (std::min (cx, kx), tr.getY(), std::abs (kx - cx), tr.getHeight()), tr.getHeight() * 0.5f);
        }
        const auto knob = R (14 * u, 14 * u).withCentre ({ kx, tr.getCentreY() });
        g.setColour (ui::bone);
        g.fillEllipse (knob);
        g.setColour (c);
        g.drawEllipse (knob.reduced (1.0f), 1.5f);
        ui::text (g, "L", R (tr.getX() - 2 * u, tr.getBottom() + 3 * u, 12 * u, 10 * u), ui::font (ui::Face::mono, 8.0f * u), ui::haze, juce::Justification::centredLeft);
        ui::text (g, "R", R (tr.getRight() - 10 * u, tr.getBottom() + 3 * u, 12 * u, 10 * u), ui::font (ui::Face::mono, 8.0f * u), ui::haze, juce::Justification::centredRight);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        dragging = true;
        attachment.beginGesture();
        set (e.position.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (dragging) set (e.position.x); }
    void mouseUp (const juce::MouseEvent&) override { if (dragging) { attachment.endGesture(); dragging = false; repaint(); } }
    void mouseDoubleClick (const juce::MouseEvent&) override { attachment.setValueAsCompleteGesture (0.0f); }

private:
    R track() const { const auto r = getLocalBounds().toFloat(); return R (r.getX() + 7 * u, r.getY() + 20 * u, r.getWidth() - 14 * u, 6 * u); }
    void set (float x)
    {
        const auto tr = track();
        float v = juce::jlimit (-100.0f, 100.0f, ((x - tr.getX()) / tr.getWidth() * 2.0f - 1.0f) * 100.0f);
        if (std::abs (v) < 3.0f) v = 0.0f;   // le centre accroche
        attachment.setValueAsPartOfGesture (std::round (v));
    }
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    float value = 0.0f, u = 1.0f;
    bool dragging = false;
};

//==============================================================================
/** TOUT RÉDUIRE pour une famille : une glissière horizontale dans sa couleur, le nombre d'événements à gauche. */
class TameSlider : public juce::Component, public juce::SettableTooltipClient
{
public:
    TameSlider (juce::RangedAudioParameter& p, int family) : fam (family), param (p),
        attachment (p, [this] (float v) { value = v; repaint(); })
    {
        attachment.sendInitialUpdate();
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setRepaintsOnMouseActivity (true);
    }
    void setScale (float s) { u = s; repaint(); }
    void setCount (int n) { if (count != n) { count = n; repaint(); } }
    void setSelected (bool s) { if (selected != s) { selected = s; repaint(); } }
    std::function<void()> onPick;

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const auto c = familyColour (fam);
        if (selected)
        {
            g.setColour (c.withAlpha (0.07f));
            g.fillRoundedRectangle (r, 10.0f * u);
        }
        const auto head = r.withHeight (r.getHeight() * 0.5f).reduced (10.0f * u, 0);
        g.setColour (c);
        g.fillEllipse (R (7 * u, 7 * u).withCentre ({ head.getX() + 4 * u, head.getCentreY() }));
        ui::text (g, familyName (fam), head.withTrimmedLeft (14 * u), ui::font (ui::Face::display, 9.5f * u, 0.14f), ui::bone, juce::Justification::centredLeft);
        ui::text (g, juce::String (count), head, ui::font (ui::Face::mono, 10.5f * u), ui::haze, juce::Justification::centredRight);
        const auto tr = track();
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (tr, tr.getHeight() * 0.5f);
        const float k = value / 100.0f;
        if (k > 0.0f)
        {
            g.setGradientFill (juce::ColourGradient (c.withAlpha (0.5f), tr.getX(), 0, c, tr.getRight(), 0, false));
            g.fillRoundedRectangle (tr.withWidth (std::max (tr.getHeight(), tr.getWidth() * k)), tr.getHeight() * 0.5f);
        }
        const auto knob = R (14 * u, 14 * u).withCentre ({ tr.getX() + tr.getWidth() * k, tr.getCentreY() });
        g.setColour (ui::bone);
        g.fillEllipse (knob);
        g.setColour (c);
        g.drawEllipse (knob.reduced (1.0f), 1.5f);
        ui::text (g, juce::String (juce::roundToInt (value)) + " %", R (tr.getRight() + 6 * u, tr.getCentreY() - 8 * u, 44 * u, 16 * u),
                  ui::font (ui::Face::mono, 10.0f * u), isMouseOver() || dragging ? ui::bone : ui::haze, juce::Justification::centredRight);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onPick) onPick();
        if (e.mods.isPopupMenu()) return;
        dragging = true;
        attachment.beginGesture();
        set (e.position.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (dragging) set (e.position.x); }
    void mouseUp (const juce::MouseEvent&) override { if (dragging) { attachment.endGesture(); dragging = false; repaint(); } }
    void mouseDoubleClick (const juce::MouseEvent&) override { attachment.setValueAsCompleteGesture (0.0f); }

private:
    R track() const { const auto r = getLocalBounds().toFloat(); return R (r.getX() + 10 * u, r.getCentreY() + r.getHeight() * 0.14f, r.getWidth() - 70 * u, 6 * u); }
    void set (float x) { const auto tr = track(); attachment.setValueAsPartOfGesture (juce::jlimit (0.0f, 100.0f, (x - tr.getX()) / tr.getWidth() * 100.0f)); }
    int fam = 0, count = 0;
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    float value = 0.0f, u = 1.0f;
    bool dragging = false, selected = false;
};

//==============================================================================
/** MODE AUTO : ce qui est retiré, en direct. Échelle en dB (0 en haut, -72 en bas), 6 s d'historique. */
class AutoView : public juce::Component, public juce::SettableTooltipClient
{
public:
    AutoView (SoloProcessor& p) : proc (p), thr (*p.apvts.getParameter (params::threshold)),
        attachment (thr, [this] (float v) { thrDb = v; repaint(); })
    {
        attachment.sendInitialUpdate();
        setTooltip (fr ("Glisse la ligne THRESHOLD : le de-esser agit sur ce qui la dépasse. En braise : ce qui est retiré, avec la lettre du son baissé."));
    }
    void setScale (float s) { u = s; repaint(); }

    bool enabled (int t) const
    {
        static const juce::String ids[] { params::targetS, params::targetT, params::targetCH, params::targetF, params::targetK };
        auto* v = proc.apvts.getRawParameterValue (ids[juce::jlimit (0, 4, t)]);
        return v != nullptr && v->load() > 0.5f;
    }

    void tick()
    {
        const int w = proc.autoEngine().writeIndex();
        if (w != lastWrite) { lastWrite = w; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace ui;
        const auto r = getLocalBounds().toFloat();
        onirique::da::glass (g, r, 16.0f * u, u, false);
        const auto plot = r.reduced (16.0f * u, 0).withTrimmedTop (44.0f * u).withTrimmedBottom (14.0f * u);
        auto y = [&] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 72.0f); };

        // la grille
        for (float db : { -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.drawHorizontalLine (juce::roundToInt (y (db)), plot.getX(), plot.getRight());
            text (g, juce::String ((int) db), R (plot.getRight() - 34 * u, y (db) - 12 * u, 32 * u, 11 * u), font (Face::mono, 8.5f * u), haze.withAlpha (0.5f),
                  juce::Justification::centredRight);
        }

        // les colonnes, de la plus ancienne (à gauche) à la plus récente
        auto& eng = proc.autoEngine();
        const int N = shown, w = eng.writeIndex();
        const float dx = plot.getWidth() / (float) N;
        juce::Path inP, outP, detP;
        inP.startNewSubPath (plot.getX(), plot.getBottom());
        outP.startNewSubPath (plot.getX(), plot.getBottom());
        struct Group { int a, b, who; float red; };
        std::vector<Group> groups;
        int counts[onirique::deess::numTargets] {};
        bool det = false;
        for (int i = 0; i < N; ++i)
        {
            const int idx = w - N + i;
            const auto c = idx >= 0 ? eng.column (idx) : onirique::deess::Column {};
            const float x = plot.getX() + dx * (float) i;
            inP.lineTo (x, y (c.inDb)); inP.lineTo (x + dx, y (c.inDb));
            outP.lineTo (x, y (c.outDb)); outP.lineTo (x + dx, y (c.outDb));
            if (c.detDb > -100.0f) { if (! det) { detP.startNewSubPath (x, y (c.detDb)); det = true; } else detP.lineTo (x + dx * 0.5f, y (c.detDb)); }
            if (c.who >= 0)
            {
                if (! groups.empty() && groups.back().b == i) { groups.back().b = i + 1; groups.back().red = std::max (groups.back().red, c.redDb); }
                else groups.push_back ({ i, i + 1, c.who, c.redDb });
            }
        }
        // la lettre de chaque retrait : le son qui a été baissé (sa forme), pas seulement le détecteur qui a agi
        std::vector<int> colWho ((size_t) N, -1);
        {
            std::vector<onirique::deess::Column> cols;
            for (auto& gr : groups)
            {
                cols.clear();
                for (int i = gr.a; i < gr.b; ++i) cols.push_back (eng.column (w - N + i));
                const int shapeWho = onirique::deess::soundOf (cols.data(), (int) cols.size());
                // seuls les sons cochés sont traités : la lettre est celle d'un son coché (sinon, le détecteur qui a agi)
                if (shapeWho >= 0 && shapeWho < onirique::deess::numTargets && enabled (shapeWho)) gr.who = shapeWho;
                for (int i = gr.a; i < gr.b; ++i) colWho[(size_t) i] = gr.who;
            }
        }
        inP.lineTo (plot.getRight(), plot.getBottom()); inP.closeSubPath();
        outP.lineTo (plot.getRight(), plot.getBottom()); outP.closeSubPath();
        g.setColour (bone.withAlpha (0.07f));
        g.fillPath (inP);
        g.setColour (bone.withAlpha (0.20f));
        g.fillPath (outP);

        // ce qui est retiré : en braise (la couleur du son), par-dessus le niveau d'entrée, épais de la réduction
        for (int i = 0; i < N; ++i)
        {
            const int idx = w - N + i;
            if (idx < 0) continue;
            const auto c = eng.column (idx);
            if (c.who < 0 || c.redDb < 0.3f) continue;
            const float x = plot.getX() + dx * (float) i, top = y (c.inDb), h = std::max (2.0f * u, plot.getHeight() * c.redDb / 72.0f * 2.2f);
            g.setColour (targetColour (colWho[(size_t) i] >= 0 ? colWho[(size_t) i] : c.who).withAlpha (0.85f));
            g.fillRect (R (x, top, dx + 0.6f, h));
        }
        // le niveau détecté (ce que regarde le THRESHOLD)
        if (det)
        {
            g.setColour (onirique::da::mist.withAlpha (0.55f));
            g.strokePath (detP, juce::PathStrokeType (1.2f * u));
        }

        // la ligne THRESHOLD
        const float ty = y (thrDb);
        {
            juce::Path line; line.startNewSubPath (plot.getX(), ty); line.lineTo (plot.getRight(), ty);
            juce::Path dashed; const float dashes[] { 6.0f * u, 4.0f * u };
            juce::PathStrokeType (1.4f * u).createDashedStroke (dashed, line, dashes, 2);
            g.setColour (accent.withAlpha (hoverThr || dragging ? 1.0f : 0.8f));
            g.fillPath (dashed);
            const R tag (plot.getRight() - 150 * u, ty - 22 * u, 150 * u, 18 * u);
            g.setColour (juce::Colour (0xff120c0e).withAlpha (0.85f));
            g.fillRoundedRectangle (tag, 9 * u);
            g.setColour (accent.withAlpha (0.8f));
            g.drawRoundedRectangle (tag.reduced (0.5f), 9 * u, 1.0f);
            text (g, "THRESHOLD " + juce::String (thrDb, 1) + " dB", tag, font (Face::mono, 9.5f * u), bone);
        }

        // les lettres de ce qui est retiré, au-dessus
        for (const auto& gr : groups)
        {
            counts[gr.who]++;
            const float cx = plot.getX() + dx * (float) (gr.a + gr.b) * 0.5f;
            const float cw = std::max (22.0f * u, std::min (40.0f * u, dx * (float) (gr.b - gr.a)));
            chip (g, R (cw, 18 * u).withCentre ({ cx, r.getY() + 22 * u }), targetName (gr.who), targetColour (gr.who), u);
            if (gr.red >= 1.0f && dx * (float) (gr.b - gr.a) > 30.0f * u)
                text (g, "-" + juce::String (gr.red, 1), R (60 * u, 12 * u).withCentre ({ cx, r.getY() + 38 * u }), font (Face::mono, 8.5f * u), haze);
        }
        // le compte des 6 dernières secondes
        juce::String sum;
        for (int t = 0; t < onirique::deess::numTargets; ++t) sum << targetName (t) << " " << counts[t] << (t < onirique::deess::numTargets - 1 ? "   " : "");
        text (g, fr ("CE QUI EST RETIRÉ, SON PAR SON  ·  ") + sum, R (r.getX() + 16 * u, r.getBottom() - 22 * u, 520 * u, 16 * u), font (Face::mono, 9.0f * u), haze,
              juce::Justification::centredLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool h = std::abs (e.position.y - thrY()) < 8.0f * u;
        if (h != hoverThr) { hoverThr = h; setMouseCursor (h ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor); repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override { hoverThr = false; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (std::abs (e.position.y - thrY()) > 14.0f * u) return;
        dragging = true; attachment.beginGesture(); grab = e.position.y - thrY();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        const auto plot = plotArea();
        const float db = -72.0f * (e.position.y - grab - plot.getY()) / plot.getHeight();
        attachment.setValueAsPartOfGesture (juce::jlimit (-60.0f, 0.0f, std::round (db * 2.0f) / 2.0f));
    }
    void mouseUp (const juce::MouseEvent&) override { if (dragging) { dragging = false; attachment.endGesture(); repaint(); } }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (std::abs (e.position.y - thrY()) < 14.0f * u) attachment.setValueAsCompleteGesture (thr.convertFrom0to1 (thr.getDefaultValue()));
    }

private:
    R plotArea() const { return getLocalBounds().toFloat().reduced (16.0f * u, 0).withTrimmedTop (44.0f * u).withTrimmedBottom (14.0f * u); }
    float thrY() const { const auto p = plotArea(); return p.getY() + p.getHeight() * juce::jlimit (0.0f, 1.0f, -thrDb / 72.0f); }
    SoloProcessor& proc;
    juce::RangedAudioParameter& thr;
    juce::ParameterAttachment attachment;
    float thrDb = -30.0f, u = 1.0f, grab = 0.0f;
    bool hoverThr = false, dragging = false;
    int lastWrite = -1;
    static constexpr int shown = 600;   // 6 s
};

//==============================================================================
/** MODE MANUEL : les événements capturés. COLONNES (la famille choisie) ou TIMELINE (la prise entière). */
class ManualView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ManualView (SoloProcessor& p) : proc (p) { setWantsKeyboardFocus (true); setRepaintsOnMouseActivity (true); }
    void setScale (float s) { u = s; repaint(); }
    int family = 0;              // la famille réglée
    bool timeline = false;       // la vue
    int selected = -1;           // l'événement choisi (indice dans proc.events())
    std::function<void()> onSelect;

    void tick()
    {
        const int rev = proc.eventsRevision() * 7919 + proc.capture().getRevision();
        const bool play = proc.hostPlaying(), listen = proc.isListening();
        if (rev != seenRev) { seenRev = rev; waves.clear(); overview.clear(); repaint(); }
        else if (play || listen || play != wasPlaying || listen != wasListening) repaint();
        wasPlaying = play; wasListening = listen;
        // la timeline suit la lecture pendant qu'elle enregistre du nouveau
        if (timeline && play && proc.isRecording()) follow();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace ui;
        const auto r = getLocalBounds().toFloat();
        onirique::da::glass (g, r, 16.0f * u, u, false);
        const auto& ev = proc.events();
        if (proc.capture().empty() && ev.empty())
        {
            text (g, proc.isCapturing() ? fr ("Lance la lecture de ta prise dans le DAW : elle s'enregistre toute seule.")
                                        : fr ("La capture est coupée : rallume CAPTURE AUTO, puis lance la lecture."),
                  r.withTrimmedBottom (r.getHeight() * 0.1f), font (Face::body, 15.0f * u), bone.withAlpha (0.85f));
            text (g, fr ("Chaque S, T, CH, P et chaque respiration apparaît ici : tu le survoles, tu l'entends, tu le baisses à la main."),
                  r.withTrimmedTop (r.getHeight() * 0.12f), font (Face::body, 11.5f * u), haze);
            return;
        }
        if (timeline) paintTimeline (g, r); else paintColumns (g, r);
        if (proc.isAnalysing())
            text (g, fr ("ANALYSE…"), R (r.getRight() - 110 * u, r.getY() + 8 * u, 96 * u, 14 * u), font (Face::mono, 9.0f * u), accent, juce::Justification::centredRight);
    }

    // ---- souris
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h != hover)
        {
            hover = h;
            if (h >= 0 && proc.hoverListen) proc.listenTo (h);
            repaint();
        }
    }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const int h = hit (e.position);
        dragIndex = -1; panning = false;
        if (h < 0) { if (timeline) { panning = true; panFrom = viewStart; panX = e.position.x; } return; }
        select (h);
        if (e.mods.isPopupMenu()) { menu (h); return; }
        dragIndex = h;
        proc.undo.beginNewTransaction();
        setGainFromY (h, e.position.y);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIndex >= 0) setGainFromY (dragIndex, e.position.y);
        else if (panning)
        {
            const double spp = (double) (viewEnd - viewStart) / std::max (1.0f, plot().getWidth());
            const auto len = viewEnd - viewStart;
            viewStart = panFrom - (juce::int64) ((e.position.x - panX) * spp);
            viewEnd = viewStart + len;
            overview.clear(); repaint();
        }
    }
    void mouseUp (const juce::MouseEvent&) override { if (dragIndex >= 0) proc.listenTo (dragIndex); dragIndex = -1; panning = false; }
    void mouseDoubleClick (const juce::MouseEvent& e) override { const int h = hit (e.position); if (h >= 0) proc.resetEvent (h); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (timeline)
        {
            ensureView();
            const auto p = plot();
            if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
            {
                const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
                const auto len = viewEnd - viewStart;
                viewStart -= (juce::int64) (d * 0.25f * (float) len); viewEnd = viewStart + len;
            }
            else
            {
                const double at = (double) viewStart + (e.position.x - p.getX()) / p.getWidth() * (double) (viewEnd - viewStart);
                const double k = w.deltaY > 0 ? 0.8 : 1.25;
                const double len = juce::jlimit (proc.captureRate() * 0.5, proc.captureRate() * 1200.0, (double) (viewEnd - viewStart) * k);
                const double frac = (at - (double) viewStart) / (double) (viewEnd - viewStart);
                viewStart = (juce::int64) (at - frac * len); viewEnd = viewStart + (juce::int64) len;
            }
            overview.clear(); repaint();
        }
        else
        {
            scroll = juce::jlimit (0.0f, maxScroll(), scroll - (w.deltaY + w.deltaX) * 300.0f * u);
            repaint();
        }
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto list = visible();
        int pos = -1;
        for (size_t i = 0; i < list.size(); ++i) if (list[i] == selected) pos = (int) i;
        if (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey)
        {
            if (list.empty()) return true;
            pos = pos < 0 ? 0 : juce::jlimit (0, (int) list.size() - 1, pos + (k == juce::KeyPress::rightKey ? 1 : -1));
            select (list[(size_t) pos]); reveal (list[(size_t) pos]); proc.listenTo (selected);
            return true;
        }
        if (selected >= 0 && selected < (int) proc.events().size())
        {
            const auto& e = proc.events()[(size_t) selected];
            if (k == juce::KeyPress::upKey) { proc.setEventGain (selected, e.gainDb + 1.0f); return true; }
            if (k == juce::KeyPress::downKey) { proc.setEventGain (selected, e.gainDb - 1.0f); return true; }
            if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { proc.setEventMuted (selected, ! e.muted); return true; }
            if (k == juce::KeyPress::spaceKey) { proc.listenTo (selected); return true; }
        }
        return false;
    }

    void select (int i) { if (selected != i) { selected = i; if (onSelect) onSelect(); repaint(); } }

    /** Les événements de la famille réglée, dans l'ordre. */
    std::vector<int> visible() const
    {
        std::vector<int> v;
        const auto& ev = proc.events();
        for (int i = 0; i < (int) ev.size(); ++i) if (ev[(size_t) i].family == family) v.push_back (i);
        return v;
    }

private:
    // ---- vue en colonnes
    R plot() const { return getLocalBounds().toFloat().reduced (14.0f * u, 0).withTrimmedTop (30.0f * u).withTrimmedBottom (14.0f * u); }
    float colWidth() const { const auto n = (float) std::max<size_t> (1, visible().size()); return juce::jlimit (30.0f * u, 92.0f * u, plot().getWidth() / n); }
    float maxScroll() const { return std::max (0.0f, colWidth() * (float) visible().size() - plot().getWidth()); }
    R columnRect (int k) const { const auto p = plot(); const float w = colWidth(); return R (p.getX() + w * (float) k - scroll, p.getY(), w, p.getHeight()); }
    float gainToY (float db, R c) const { const auto a = c.reduced (0, 12.0f * u).withTrimmedBottom (26.0f * u); return a.getY() + a.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 30.0f); }
    float yToGain (float y, R c) const { const auto a = c.reduced (0, 12.0f * u).withTrimmedBottom (26.0f * u); return -30.0f * juce::jlimit (0.0f, 1.0f, (y - a.getY()) / a.getHeight()); }

    const std::vector<float>& wave (int i)
    {
        auto it = waves.find (i);
        if (it != waves.end()) return it->second;
        const auto& e = proc.events()[(size_t) i];
        std::vector<float> pk (64, 0.0f);
        const auto len = e.end - e.start;
        for (int k = 0; k < 64; ++k)
        {
            const auto a = e.start + len * k / 64, b = e.start + len * (k + 1) / 64;
            pk[(size_t) k] = std::max (0.0f, proc.capture().peak (a, std::max (a + 1, b)));
        }
        return waves[i] = std::move (pk);
    }

    void paintColumns (juce::Graphics& g, R r)
    {
        using namespace ui;
        const auto list = visible();
        const auto c = familyColour (family);
        text (g, familyName (family) + "  ·  " + juce::String ((int) list.size()) + fr (" TROUVÉES  ·  GLISSE POUR BAISSER, SURVOLE POUR ÉCOUTER"),
              R (r.getX() + 16 * u, r.getY() + 8 * u, r.getWidth() - 140 * u, 14 * u), font (Face::display, 8.5f * u, 0.14f), haze.withAlpha (0.8f),
              juce::Justification::centredLeft);
        if (list.empty())
        {
            text (g, fr ("Rien de cette famille dans la prise (pour l'instant)."), plot(), font (Face::body, 12.0f * u), haze);
            return;
        }
        // l'échelle des formes d'onde : la plus forte de la famille remplit presque la colonne (les niveaux restent comparables)
        float top = 1.0e-4f;
        for (int i : list) for (float v : wave (i)) top = std::max (top, v);
        const float vis = 0.92f / top;
        const auto p = plot();
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (p.toNearestInt());
        const int playing = proc.isListening() ? proc.listeningIndex() : -1;
        for (size_t k = 0; k < list.size(); ++k)
        {
            const int i = list[k];
            auto col = columnRect ((int) k);
            if (col.getRight() < p.getX() || col.getX() > p.getRight()) continue;
            const auto& e = proc.events()[(size_t) i];
            const auto cr = col.reduced (2.0f * u, 0);
            const bool sel = i == selected, hov = i == hover;
            // la bande, teintée de sa famille
            g.setGradientFill (juce::ColourGradient (c.withAlpha (e.muted ? 0.05f : (sel ? 0.32f : hov ? 0.24f : 0.16f)), 0, cr.getY(),
                                                     c.withAlpha (0.02f), 0, cr.getBottom(), false));
            g.fillRoundedRectangle (cr, 6.0f * u);
            g.setColour (sel ? c : juce::Colours::white.withAlpha (hov ? 0.18f : 0.07f));
            g.drawRoundedRectangle (cr.reduced (0.5f), 6.0f * u, sel ? 1.6f : 1.0f);
            // la forme d'onde, à son gain
            const auto wa = cr.reduced (3.0f * u, 12.0f * u).withTrimmedBottom (26.0f * u);
            const float gl = e.muted ? 1.0f : juce::Decibels::decibelsToGain (e.gainDb);
            const auto& pk = wave (i);
            juce::Path wp;
            const float cy = wa.getCentreY(), hh = wa.getHeight() * 0.5f;
            for (int s = 0; s < 64; ++s) { const float x = wa.getX() + wa.getWidth() * (float) s / 63.0f, yv = cy - hh * std::min (1.0f, pk[(size_t) s] * vis * gl); if (s == 0) wp.startNewSubPath (x, yv); else wp.lineTo (x, yv); }
            for (int s = 63; s >= 0; --s) { const float x = wa.getX() + wa.getWidth() * (float) s / 63.0f; wp.lineTo (x, cy + hh * std::min (1.0f, pk[(size_t) s] * vis * gl)); }
            wp.closeSubPath();
            g.setColour ((e.muted ? haze : bone).withAlpha (i == playing ? 0.95f : 0.7f));
            g.fillPath (wp);
            // l'ancienne taille, en filigrane, quand il est baissé
            if (gl < 0.99f)
            {
                juce::Path op;
                for (int s = 0; s < 64; ++s) { const float x = wa.getX() + wa.getWidth() * (float) s / 63.0f; const float yv = cy - hh * std::min (1.0f, pk[(size_t) s] * vis); if (s == 0) op.startNewSubPath (x, yv); else op.lineTo (x, yv); }
                g.setColour (c.withAlpha (0.45f));
                g.strokePath (op, juce::PathStrokeType (1.0f * u));
            }
            // la ligne du gain
            if (! e.muted)
            {
                const float gy = gainToY (e.gainDb, col);
                g.setColour (c.withAlpha (0.95f));
                g.fillRect (R (cr.getX() + 3 * u, gy - 1.2f * u, cr.getWidth() - 6 * u, 2.4f * u));
                if (e.manual) { g.setColour (bone); g.fillEllipse (R (5 * u, 5 * u).withCentre ({ cr.getCentreX(), gy })); }
            }
            // la lettre, et le gain
            const auto foot = R (cr.getX(), cr.getBottom() - 24 * u, cr.getWidth(), 20 * u);
            chip (g, R (std::min (cr.getWidth() - 6 * u, 32 * u), 19 * u).withCentre ({ foot.getCentreX(), foot.getY() + 9 * u }), e.letter, c, u, e.muted ? 0.35f : 1.0f);
            // la réduction, accrochée à sa ligne (au-dessus, ou en dessous si la ligne touche le haut), lisible sur fond sombre
            if ((hov || sel || std::abs (e.gainDb) >= 0.05f) && ! e.muted && cr.getWidth() > 22 * u)
            {
                const juce::String v = std::abs (e.gainDb) < 0.05f ? juce::String ("0") : juce::String (e.gainDb, 1);
                const float fs = cr.getWidth() > 40 * u ? 10.5f * u : 8.5f * u, h = fs + 5 * u;
                const auto f = font (Face::mono, fs);
                const float w = std::min (cr.getWidth() - 4 * u, juce::GlyphArrangement::getStringWidth (f, v) + 10 * u);
                const float gy = gainToY (e.gainDb, col);
                float ty = gy - h - 3 * u;
                if (ty < cr.getY() + 2 * u) ty = gy + 3 * u;
                const auto tag = R (w, h).withCentre ({ cr.getCentreX(), ty + h * 0.5f });
                g.setColour (juce::Colour (0xff120c0e).withAlpha (0.8f));
                g.fillRoundedRectangle (tag, h * 0.5f);
                text (g, v, tag, f, hov || sel ? bone : bone.withAlpha (0.85f));
            }
            if (e.muted) text (g, fr ("IGNORÉ"), R (cr.getX(), cr.getCentreY() - 6 * u, cr.getWidth(), 12 * u), font (Face::mono, 7.5f * u), haze);
        }
    }

    // ---- vue timeline
    void ensureView()
    {
        if (viewEnd > viewStart) return;
        const auto& st = proc.capture();
        juce::int64 a = st.firstSample(), b = st.lastSample();
        for (const auto& e : proc.events()) { a = std::min (a, e.start); b = std::max (b, e.end); }
        if (b <= a) b = a + (juce::int64) (proc.captureRate() * 10.0);
        const auto pad = (b - a) / 40;
        viewStart = a - pad; viewEnd = b + pad;
    }
    void follow()
    {
        ensureView();
        const auto t = proc.hostPosition(), len = viewEnd - viewStart;
        if (t > viewEnd - len / 10 || t < viewStart) { viewStart = t - len / 2; viewEnd = viewStart + len; overview.clear(); }
    }
    void reveal (int i)
    {
        if (i < 0 || i >= (int) proc.events().size()) return;
        if (timeline)
        {
            ensureView();
            const auto& e = proc.events()[(size_t) i];
            const auto len = viewEnd - viewStart;
            if (e.start < viewStart || e.end > viewEnd) { viewStart = (e.start + e.end) / 2 - len / 2; viewEnd = viewStart + len; overview.clear(); }
        }
        else
        {
            const auto list = visible();
            for (size_t k = 0; k < list.size(); ++k)
                if (list[k] == i)
                {
                    const auto cr = columnRect ((int) k), p = plot();
                    if (cr.getX() < p.getX()) scroll += cr.getX() - p.getX();
                    else if (cr.getRight() > p.getRight()) scroll += cr.getRight() - p.getRight();
                    scroll = juce::jlimit (0.0f, maxScroll(), scroll);
                }
        }
        repaint();
    }
    float xOf (juce::int64 t) const { const auto p = plot(); return p.getX() + p.getWidth() * (float) ((double) (t - viewStart) / (double) std::max<juce::int64> (1, viewEnd - viewStart)); }
    R eventRect (const onirique::deess::Event& e) const
    {
        const auto p = plot();
        const float a = xOf (e.start), b = std::max (a + 4.0f * u, xOf (e.end));
        return R (a, p.getY() + 22 * u, b - a, p.getHeight() - 22 * u);
    }

    void paintTimeline (juce::Graphics& g, R r)
    {
        using namespace ui;
        ensureView();
        const auto p = plot();
        const auto& ev = proc.events();
        text (g, fr ("LA PRISE  ·  MOLETTE POUR ZOOMER, GLISSE LE FOND POUR TE DÉPLACER"), R (r.getX() + 16 * u, r.getY() + 8 * u, r.getWidth() - 140 * u, 14 * u),
              font (Face::display, 8.5f * u, 0.14f), haze.withAlpha (0.8f), juce::Justification::centredLeft);
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (p.toNearestInt());
        // la forme d'onde de la prise (une crête par pixel, gardée tant que la vue ne bouge pas)
        const int W = std::max (1, (int) p.getWidth());
        if ((int) overview.size() != W)
        {
            overview.assign ((size_t) W, -1.0f);
            for (int x = 0; x < W; ++x)
            {
                const auto a = viewStart + (viewEnd - viewStart) * x / W, b = viewStart + (viewEnd - viewStart) * (x + 1) / W;
                overview[(size_t) x] = proc.capture().peak (a, std::max (a + 1, b));
            }
        }
        float top = 1.0e-4f; for (float v : overview) top = std::max (top, v);
        const float vis = 0.9f / top, cy = p.getY() + 22 * u + (p.getHeight() - 22 * u) * 0.5f, hh = (p.getHeight() - 22 * u) * 0.5f;
        // les événements derrière la forme d'onde
        for (int i = 0; i < (int) ev.size(); ++i)
        {
            const auto& e = ev[(size_t) i];
            const auto er = eventRect (e);
            if (er.getRight() < p.getX() || er.getX() > p.getRight()) continue;
            const auto c = familyColour (e.family);
            const bool mine = e.family == family, sel = i == selected, hov = i == hover;
            g.setColour (c.withAlpha (e.muted ? 0.04f : mine ? (sel ? 0.34f : hov ? 0.26f : 0.18f) : 0.07f));
            g.fillRect (er);
            if (sel) { g.setColour (c); g.drawRect (er, 1.5f); }
        }
        g.setColour (bone.withAlpha (0.55f));
        for (int x = 0; x < W; ++x)
        {
            const float v = overview[(size_t) x];
            if (v < 0.0f) continue;
            // le son tel qu'il sortira : la crête baissée là où un événement est réglé
            const float h = std::max (0.5f, hh * std::min (1.0f, v * vis * gainAtPixel (x)));
            g.fillRect (R (p.getX() + (float) x, cy - h, 1.0f, 2.0f * h));
        }
        // les lettres et les lignes de gain
        for (int i = 0; i < (int) ev.size(); ++i)
        {
            const auto& e = ev[(size_t) i];
            const auto er = eventRect (e);
            if (er.getRight() < p.getX() || er.getX() > p.getRight()) continue;
            const auto c = familyColour (e.family);
            const bool mine = e.family == family;
            if (er.getWidth() >= 10.0f * u || mine)
                chip (g, R (std::max (16.0f * u, std::min (28.0f * u, er.getWidth())), 15 * u).withCentre ({ er.getCentreX(), p.getY() + 10 * u }), e.letter, c, u,
                      e.muted ? 0.3f : mine ? 1.0f : 0.45f);
            if (! e.muted && std::abs (e.gainDb) >= 0.05f)
            {
                const float gy = er.getY() + er.getHeight() * (-e.gainDb / 30.0f);
                g.setColour (c.withAlpha (mine ? 0.95f : 0.4f));
                g.fillRect (R (er.getX(), gy - 1.0f * u, er.getWidth(), 2.0f * u));
            }
        }
        // la tête de lecture
        if (proc.hostPlaying())
        {
            const float x = xOf (proc.hostPosition());
            g.setColour (bone.withAlpha (0.9f));
            g.fillRect (R (x - 0.75f * u, p.getY(), 1.5f * u, p.getHeight()));
        }
        // le temps, en bas
        const double fs = proc.captureRate();
        const double span = (double) (viewEnd - viewStart) / fs;
        const double step = span > 120 ? 30 : span > 40 ? 10 : span > 12 ? 2 : span > 4 ? 1 : 0.25;
        for (double t = std::max (0.0, std::ceil ((double) viewStart / fs / step) * step); t * fs < (double) viewEnd; t += step)
        {
            const float x = xOf ((juce::int64) (t * fs));
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (R (x, p.getBottom() - 6 * u, 1.0f, 6 * u));
            text (g, timeText (t), R (x + 3 * u, p.getBottom() - 14 * u, 60 * u, 12 * u), font (Face::mono, 8.0f * u), haze.withAlpha (0.6f), juce::Justification::centredLeft);
        }
    }
    float gainAtPixel (int x) const
    {
        const auto t = viewStart + (viewEnd - viewStart) * x / std::max (1, (int) plot().getWidth());
        float g = 1.0f;
        for (const auto& e : proc.events()) if (! e.muted && t >= e.start && t < e.end) g *= juce::Decibels::decibelsToGain (e.gainDb);
        return g;
    }

    // ---- ce qui est sous la souris
    int hit (juce::Point<float> pt) const
    {
        const auto p = plot();
        if (! p.contains (pt)) return -1;
        if (timeline)
        {
            const auto& ev = proc.events();
            int best = -1;
            for (int i = 0; i < (int) ev.size(); ++i)
            {
                const auto er = eventRect (ev[(size_t) i]).expanded (2.0f * u, 0);
                if (er.contains (pt) && (best < 0 || ev[(size_t) i].family == family)) best = i;
            }
            return best;
        }
        const auto list = visible();
        for (size_t k = 0; k < list.size(); ++k) if (columnRect ((int) k).contains (pt)) return list[k];
        return -1;
    }
    void setGainFromY (int i, float y)
    {
        R c;
        if (timeline) c = eventRect (proc.events()[(size_t) i]).withTrimmedBottom (-26.0f * u).expanded (0, 12.0f * u);
        else { const auto list = visible(); for (size_t k = 0; k < list.size(); ++k) if (list[k] == i) c = columnRect ((int) k); }
        if (c.isEmpty()) return;
        float db = timeline ? -30.0f * juce::jlimit (0.0f, 1.0f, (y - eventRect (proc.events()[(size_t) i]).getY()) / eventRect (proc.events()[(size_t) i]).getHeight())
                            : yToGain (y, c);
        db = std::round (db * 2.0f) / 2.0f;
        if (proc.events()[(size_t) i].muted) proc.setEventMuted (i, false);
        proc.setEventGain (i, db);
    }
    void menu (int i)
    {
        const auto& e = proc.events()[(size_t) i];
        juce::PopupMenu m;
        m.setLookAndFeel (&getLookAndFeel());   // le menu dans la DA du plugin (jamais le style par défaut)
        m.addItem (1, fr ("Écouter"));
        m.addItem (2, fr ("Rendre à TOUT RÉDUIRE"), e.manual || e.muted);
        m.addItem (3, e.muted ? fr ("Ne plus ignorer") : fr ("Ignorer (laisser tel quel)"));
        m.addItem (4, fr ("Mettre à 0 dB"));
        juce::Component::SafePointer<ManualView> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, i] (int r)
        {
            if (safe == nullptr || i >= (int) safe->proc.events().size()) return;
            if (r == 1) safe->proc.listenTo (i);
            if (r == 2) safe->proc.resetEvent (i);
            if (r == 3) safe->proc.setEventMuted (i, ! safe->proc.events()[(size_t) i].muted);
            if (r == 4) safe->proc.setEventGain (i, 0.0f);
        });
    }

    SoloProcessor& proc;
    float u = 1.0f, scroll = 0.0f, panX = 0.0f;
    int hover = -1, dragIndex = -1, seenRev = -1;
    bool panning = false, wasPlaying = false, wasListening = false;
    juce::int64 viewStart = 0, viewEnd = 0, panFrom = 0;
    std::map<int, std::vector<float>> waves;
    std::vector<float> overview;
};

//==============================================================================
/** L'événement choisi : sa lettre, son gain, où il est, et quoi en faire. */
class Inspector : public juce::Component
{
public:
    explicit Inspector (SoloProcessor& p) : proc (p)
    {
        for (auto* b : { &listen, &reset, &mute }) { addAndMakeVisible (b); }
        listen.onClick = [this] { if (index >= 0) proc.listenTo (index); };
        reset.onClick = [this] { if (index >= 0) proc.resetEvent (index); };
        mute.onClick = [this] { if (index >= 0 && index < (int) proc.events().size()) proc.setEventMuted (index, ! proc.events()[(size_t) index].muted); };
        listen.setTooltip (fr ("Écouter l'événement (avec son gain). Espace au clavier."));
        reset.setTooltip (fr ("Le rendre à TOUT RÉDUIRE (double-clic sur l'événement)."));
        mute.setTooltip (fr ("Ignorer : il reste tel quel, quoi qu'on règle. Suppr au clavier."));
    }
    void setScale (float s) { u = s; for (auto* b : { &listen, &reset, &mute }) b->setScale (s); resized(); repaint(); }
    void show (int i) { index = i; refresh(); }
    void refresh()
    {
        const bool ok = index >= 0 && index < (int) proc.events().size();
        for (auto* b : { &listen, &reset, &mute }) b->setVisible (ok);
        if (ok) mute.setText (proc.events()[(size_t) index].muted ? fr ("RÉTABLIR") : fr ("IGNORER"));
        repaint();
    }
    void resized() override
    {
        const auto r = getLocalBounds().toFloat().reduced (16 * u, 12 * u);
        const float w = (r.getWidth() - 16 * u) / 3.0f, y = r.getBottom() - 30 * u;
        listen.setBounds (R (r.getX(), y, w, 30 * u).toNearestInt());
        reset.setBounds (R (r.getX() + w + 8 * u, y, w, 30 * u).toNearestInt());
        mute.setBounds (R (r.getX() + 2 * (w + 8 * u), y, w, 30 * u).toNearestInt());
    }
    void paint (juce::Graphics& g) override
    {
        using namespace ui;
        const auto r = getLocalBounds().toFloat();
        onirique::da::glass (g, r, 16.0f * u, u, false);
        const auto in = r.reduced (16 * u, 12 * u);
        if (index < 0 || index >= (int) proc.events().size())
        {
            text (g, fr ("Choisis un événement"), in.withTrimmedBottom (20 * u), font (Face::body, 12.5f * u), haze);
            text (g, fr ("Flèches gauche et droite : le suivant, haut et bas : le régler"), in.withTrimmedTop (40 * u), font (Face::body, 10.0f * u), haze.withAlpha (0.7f));
            return;
        }
        const auto& e = proc.events()[(size_t) index];
        const auto c = familyColour (e.family);
        const double fs = proc.captureRate();
        chip (g, R (in.getX(), in.getY() + 4 * u, 46 * u, 46 * u), e.letter, c, u * 1.6f, e.muted ? 0.4f : 1.0f);
        text (g, familyName (e.family), R (in.getX() + 58 * u, in.getY() + 4 * u, 200 * u, 14 * u), font (Face::display, 9.0f * u, 0.14f), c, juce::Justification::centredLeft);
        text (g, timeText ((double) e.start / fs) + "  ·  " + juce::String (juce::roundToInt ((double) (e.end - e.start) / fs * 1000.0)) + " ms",
              R (in.getX() + 58 * u, in.getY() + 22 * u, 220 * u, 14 * u), font (Face::mono, 10.0f * u), haze, juce::Justification::centredLeft);
        text (g, e.muted ? fr ("ignoré") : e.manual ? fr ("réglé à la main") : fr ("suit TOUT RÉDUIRE"),
              R (in.getX() + 58 * u, in.getY() + 38 * u, 220 * u, 14 * u), font (Face::body, 10.0f * u), haze.withAlpha (0.7f), juce::Justification::centredLeft);
        const juce::String gain = e.muted ? juce::String ("--") : std::abs (e.gainDb) < 0.05f ? juce::String ("0.0") : juce::String (e.gainDb, 1);
        text (g, gain, R (in.getRight() - 150 * u, in.getY(), 150 * u, 40 * u), font (Face::display, 30.0f * u, 0.02f), e.muted ? haze : bone, juce::Justification::centredRight);
        text (g, "dB", R (in.getRight() - 150 * u, in.getY() + 38 * u, 150 * u, 14 * u), font (Face::mono, 9.5f * u), haze, juce::Justification::centredRight);
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (index < 0 || index >= (int) proc.events().size()) return;
        proc.setEventGain (index, proc.events()[(size_t) index].gainDb + (w.deltaY > 0 ? 0.5f : -0.5f));
    }

private:
    SoloProcessor& proc;
    Pill listen { fr ("ÉCOUTER") }, reset { fr ("RÉINITIALISER") }, mute { fr ("IGNORER") };
    int index = -1;
    float u = 1.0f;
};
} // namespace dui
