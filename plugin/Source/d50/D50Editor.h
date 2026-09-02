#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <utility>
#include <vector>

#include "D50AudioProcessor.h"
#include "../D110Keyboard.h"

// D-50 editor: patch picker, the panel's own three balance knobs (volume/
// reverb/chorus), a first slice of real sound-parameter editing (structure
// and each partial's waveform/PCM source/filter - see PartialPanel below),
// and the shared on-screen test keyboard so this is playable without a DAW
// or external controller. Not a front-panel recreation like D110Panel -
// there is no real firmware/panel behind this engine to photograph or
// measure (see CLAUDE.md's D-50 section), so this is plain JUCE widgets.
//
// Uses the OS's own native window chrome (see parentHierarchyChanged()) and
// is genuinely resizable - Alan's explicit request, 2026-09-01: JUCE's own
// custom-drawn title bar wouldn't let the Standalone window drag past the
// screen edge (see project memory project_window_offscreen_drag_
// investigation - a native title bar is one of the two mechanisms it
// flagged as "not yet checked", now resolved by just using one).
class D50Editor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit D50Editor(D50AudioProcessor &p);
    ~D50Editor() override;

    void paint(juce::Graphics &g) override;
    void resized() override;
    // Only reachable once this editor is actually attached to a top-level
    // window (see this file's own header comment) - a Standalone run gets
    // JUCE's own DocumentWindow, so this applies; a future VST3 build's host
    // window may or may not be a juce::TopLevelWindow at all, in which case
    // the dynamic_cast below is simply null and this is a no-op there.
    void parentHierarchyChanged() override;

    // {display name, byte offset, maximum value} - same idiom as D110EditorPane's
    // own ToneParam (PluginEditor.h), one generic row per entry rather than a
    // hand-written widget per parameter. Offsets/ranges are relative to
    // whatever base a ParamColumn instance is given (a partial's own 64-byte
    // block, the tone-common block, or the patch block) - see the .cpp's own
    // parameter tables for where each one comes from in d5_patch_map.h. Public
    // only so the .cpp's file-scope tables can name the type; never used
    // outside this class.
    struct ToneParam {
        const char *name;
        int offset;
        int hi;
    };

private:
    void timerCallback() override;
    void refreshPatchList();
    void layoutToneColumns();
    void importSyxBank();
    void exportCurrentPatch();
    void exportBank();
    // Re-targets partial1/partial2 and every tone-scoped ParamColumn (WG/TVF/
    // TVA per partial, P-ENV, the three LFOs, EQ/Chorus) at the lower tone's
    // blocks instead of the upper tone's, or back - the D-50 has two of these
    // (Alan, 2026-09-02: confirming it plays both at once in Dual/Split key
    // mode), and this toggles which one the Tone section is currently
    // editing rather than showing both at once and doubling the section's
    // already-substantial size. Patch Common (key mode, split point, etc.)
    // is genuinely patch-level, not tone-level, so it never moves.
    void setToneScope(bool lower);

    // A cosmetic lookalike of a D-50-era alphanumeric LCD - patch number/name
    // and structure, in the shared dot-matrix font (see ../DotMatrixFont.h).
    // Not a real display: there is no real firmware behind this engine to
    // render an authentic one (see this class's own CLAUDE.md reference) -
    // this is only for the same reason D110SequencerRetroPanel's screen is,
    // to look the part.
    class LcdReadout : public juce::Component {
    public:
        void setLines(const juce::String &line1, const juce::String &line2);
        void paint(juce::Graphics &g) override;

    private:
        juce::String line1, line2;
    };

    // One partial's sound-shaping controls: WG Waveform, PCM wave number,
    // TVF cutoff/resonance, pulse width. `blockBase` is that partial's own
    // 64-byte block's absolute offset into the 448-byte patch (see
    // d50/d5_engine/d5_patch_map.h's PatchBlock enum - kBlkUpperP1/P2 times
    // 64) - the byte offsets used here are relative to it, straight from
    // that file's own map_partial() comments.
    class PartialPanel : public juce::Component {
    public:
        PartialPanel(D50AudioProcessor &proc, int blockBase, const juce::String &title);
        void resized() override;
        void refresh();
        // Re-targets this panel at a different 64-byte block - the Upper/Lower
        // toggle (see D50Editor::setToneScope()) moves the SAME two
        // PartialPanel instances between the upper and lower tone's blocks
        // rather than building a second pair, so the compact view doesn't
        // double in size for a patch that hardly ever needs both at once.
        void setBase(int newBase) {
            base = newBase;
            refresh();
        }

    private:
        D50AudioProcessor &processor;
        int base;
        juce::Label titleLabel;
        juce::Label waveformLabel, pcmLabel, cutoffLabel, resonanceLabel, pulseWidthLabel;
        juce::ComboBox waveformBox;
        juce::Slider pcmSlider, cutoffSlider, resonanceSlider, pulseWidthSlider;
    };

    // One titled group of raw-byte sliders, built from a ToneParam table -
    // the generic building block every group below (WG/TVF/TVA per partial,
    // P-ENV, the three LFOs, EQ/Chorus, patch-common) is made of, so adding
    // another group later is a new table, not new layout code.
    class ParamColumn : public juce::Component {
    public:
        ParamColumn(D50AudioProcessor &proc, int base, const juce::String &title, const ToneParam *params,
                    int count);
        void resized() override;
        void refresh();
        int preferredHeight() const;
        // See PartialPanel::setBase()'s own comment - same idea, for the
        // scrolling per-partial/tone-common groups below.
        void setBase(int newBase) {
            base = newBase;
            refresh();
        }

    private:
        D50AudioProcessor &processor;
        int base;
        juce::Label titleLabel;
        juce::OwnedArray<juce::Label> paramLabels;
        juce::OwnedArray<juce::Slider> sliders;
        std::vector<int> offsets;
    };

    D50AudioProcessor &processor;

    juce::Label titleLabel, statusLabel;
    // Standalone only (see the .cpp): the native title bar means JUCE's own
    // StandaloneFilterWindow "Options" button - which lived IN that
    // custom-drawn title bar - no longer has anywhere to sit
    // (getTitleBarHeight() goes to 0), so this replaces it for Audio/MIDI
    // Settings access. Always constructed (a plain TextButton costs nothing
    // unused) but only wired up and shown for JucePlugin_Build_Standalone.
    juce::TextButton audioSettingsButton{"Audio/MIDI Settings..."};
    // Import a whole bank (any real D-50 SysEx bulk dump) or export just the
    // currently-sounding patch, with whatever edits the Tone section above
    // made to it, as a single-patch SysEx file someone else's D-50 - or this
    // app, later - can load back in. See importSyxBank()/exportCurrentPatch().
    juce::TextButton patchMenuButton{"Patch..."};
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::ComboBox patchBox;
    LcdReadout lcd;
    juce::Label volumeLabel, reverbLabel, chorusLabel;
    juce::Slider volumeSlider, reverbSlider, chorusSlider;

    juce::Label toneSectionLabel, structureLabel, balanceLabel;
    juce::TextButton toneScopeToggle{"Switch to Lower"};
    // Solo/mute for auditioning, one button per PARTIAL rather than one per
    // tone - see D50AudioProcessor::setUpperPartial1Muted()'s own comment
    // (Alan, 2026-09-02: split further to localize a per-note artifact one
    // tone-wide mute couldn't pin down). Independent of toneScopeToggle,
    // which only moves which tone the Tone section below is EDITING, not
    // which partials are audible. Lit (toggle state on) means the partial
    // SOUNDS - so each button's own on/off state is the inverse of
    // D5_Bridge's mute flag, and all four start lit (nothing muted).
    juce::TextButton lowerP1OnButton{"Lo P1"}, lowerP2OnButton{"Lo P2"};
    juce::TextButton upperP1OnButton{"Up P1"}, upperP2OnButton{"Up P2"};
    // One-click A/B of the TVF ENV DEPTH keyfollow direction (Alan,
    // 2026-09-02) - see D5_Bridge::setTvfKeyfollowFixed(). Sits with the mute
    // buttons because it belongs to the same listening-test toolkit, but
    // unlike them it really changes the instrument, so its label says which
    // of the two firmware revisions is being heard rather than on/off.
    juce::TextButton keyfollowFixButton;
    bool showingLowerTone = false;
    // Structure/Balance live in the tone-common block, which moves with the
    // Upper/Lower toggle - kept as a member rather than the upper-tone
    // constant every other reference in the .cpp uses, so their onChange
    // handlers write to whichever tone is currently shown.
    int currentCommonBase = 0;
    juce::ComboBox structureBox;
    juce::Slider balanceSlider;
    PartialPanel partial1, partial2;

    // The rest of the 448-byte patch - full TVF/TVA envelopes, P-ENV, the
    // three LFOs, EQ/chorus detail, patch-common - scrolls, rather than
    // trying to cram ~150 more sliders into a window that still has to fit
    // on a screen. See buildToneColumns()/layoutToneColumns() in the .cpp.
    juce::Viewport toneViewport;
    juce::Component toneViewportContent;
    juce::OwnedArray<ParamColumn> toneColumns;
    // Which of toneColumns move with the Upper/Lower toggle, and how - see
    // setToneScope(). Patch Common columns are absent from both: they never
    // move at all.
    std::vector<ParamColumn *> partial1ScopedColumns, partial2ScopedColumns;
    std::vector<std::pair<ParamColumn *, int>> commonScopedColumns;

    D110Keyboard keyboard;

    bool wasReady = false;
    bool nativeTitleBarApplied = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(D50Editor)
};
