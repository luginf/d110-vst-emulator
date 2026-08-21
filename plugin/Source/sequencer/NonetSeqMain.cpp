// Nonet Sequencer: a bare window around D110SequencerPanel and a small toolbar to pick
// the direct system MIDI In/Out ports, backed by NonetSeqHost - no firmware, no ROMs, no
// plugin wrapper. See docs/sequencer.md's "MIDI Out" section for why this exists (driving
// a real D-110, or any other synth, without the emulation/DAW in the loop at all) and
// NonetSeqHost.h for what it does and doesn't do.

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "../D110Keyboard.h"
#include "../UiTheme.h"
#include "D110SequencerPanel.h"
#include "D110SequencerRetroPanel.h"
#include "NonetSeqHost.h"

namespace {

constexpr int kToolbarHeight = 28;

// Options screen (AlertWindow::addCustomComponent(), same pattern as
// D110SequencerPanel::promptForEventList()'s note-list dialog): the THEME toggle this
// standalone app needs, that the D-110 plugin instead tucks into its Utility tab, plus a
// correction for the Program Change/Bank Select bytes actually sent. Different synths'
// manuals number these from 0 or from 1 inconsistently, and there's no way to know which
// from here, so rather than guessing, +/- buttons let Alan nudge the outgoing byte until
// playback matches what the manual says it should - see
// NonetSeqHost::setProgramChangeOffset()/setBankOffset().
class OptionsDialogContent : public juce::Component {
public:
	std::function<void()> onThemeToggled;
	std::function<void(int)> onProgramOffsetChanged;
	std::function<void(int)> onBankOffsetChanged;
	std::function<void()> onExtraTracksToggled;
	std::function<void()> onRetroModeToggled;
	std::function<void()> onAudioSettingsRequested;
	std::function<void()> onQuantizeModeToggled;

	void setValues(bool themeLightIn, int programOffsetIn, int bankOffsetIn, bool extraTracksOnIn,
	               bool retroModeOnIn, bool quantizeSoftIn) {
		themeLight = themeLightIn;
		programOffset = programOffsetIn;
		bankOffset = bankOffsetIn;
		extraTracksOn = extraTracksOnIn;
		retroModeOn = retroModeOnIn;
		quantizeSoft = quantizeSoftIn;
		repaint();
	}

	void paint(juce::Graphics &g) override {
		const auto &pal = d110ui::palette();
		g.fillAll(pal.box);

		auto b = getLocalBounds().toFloat().reduced(6.0f);
		themeRowBounds = b.removeFromTop(kRowH);
		b.removeFromTop(kRowGap);
		auto progRow = b.removeFromTop(kRowH);
		b.removeFromTop(kRowGap);
		auto bankRow = b.removeFromTop(kRowH);
		b.removeFromTop(kRowGap);
		extraTracksRowBounds = b.removeFromTop(kRowH);
		b.removeFromTop(kRowGap);
		retroModeRowBounds = b.removeFromTop(kRowH);
		b.removeFromTop(kRowGap);
		quantizeModeRowBounds = b.removeFromTop(kRowH);
		b.removeFromTop(kRowGap);
		audioRowBounds = b.removeFromTop(kRowH);

		paintToggleRow(g, pal, themeRowBounds, "Theme", themeLight ? "LIGHT" : "DARK");
		paintOffsetRow(g, pal, progRow, "Program Change offset", programOffset, progMinusBounds, progPlusBounds);
		paintOffsetRow(g, pal, bankRow, "Bank offset", bankOffset, bankMinusBounds, bankPlusBounds);
		paintToggleRow(g, pal, extraTracksRowBounds, "Extra tracks (16 total)", extraTracksOn ? "ON" : "OFF");
		paintToggleRow(g, pal, retroModeRowBounds, "Retro Sequencer (D-20 style)", retroModeOn ? "ON" : "OFF");
		paintToggleRow(g, pal, quantizeModeRowBounds, "Quantize mode", quantizeSoft ? "SOFT" : "HARD");
		paintToggleRow(g, pal, audioRowBounds, "Audio Device", "Configure...");
	}

	void mouseDown(const juce::MouseEvent &e) override {
		if (themeRowBounds.contains(e.position)) { if (onThemeToggled) onThemeToggled(); return; }
		if (progMinusBounds.contains(e.position)) { if (onProgramOffsetChanged) onProgramOffsetChanged(programOffset - 1); return; }
		if (progPlusBounds.contains(e.position)) { if (onProgramOffsetChanged) onProgramOffsetChanged(programOffset + 1); return; }
		if (bankMinusBounds.contains(e.position)) { if (onBankOffsetChanged) onBankOffsetChanged(bankOffset - 1); return; }
		if (bankPlusBounds.contains(e.position)) { if (onBankOffsetChanged) onBankOffsetChanged(bankOffset + 1); return; }
		if (extraTracksRowBounds.contains(e.position)) { if (onExtraTracksToggled) onExtraTracksToggled(); return; }
		if (retroModeRowBounds.contains(e.position)) { if (onRetroModeToggled) onRetroModeToggled(); return; }
		if (quantizeModeRowBounds.contains(e.position)) { if (onQuantizeModeToggled) onQuantizeModeToggled(); return; }
		if (audioRowBounds.contains(e.position)) { if (onAudioSettingsRequested) onAudioSettingsRequested(); return; }
	}

private:
	static constexpr float kRowH = 30.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kButtonW = 32.0f;

	// Shared by the Theme, Extra-tracks and Audio Device rows - a label on the left, a
	// single clickable state/action box on the right (the whole row is the hit target,
	// see mouseDown()).
	void paintToggleRow(juce::Graphics &g, const d110ui::Palette &pal, juce::Rectangle<float> row,
	                     const juce::String &label, const juce::String &stateText) {
		g.setColour(pal.value);
		g.setFont(juce::FontOptions(13.0f));
		g.drawText(label, row.removeFromLeft(row.getWidth() * 0.5f), juce::Justification::centredLeft);
		g.setColour(pal.boxBorder);
		g.drawRoundedRectangle(row, 3.0f, 1.0f);
		g.setColour(pal.value);
		g.drawText(stateText, row, juce::Justification::centred);
	}

	void paintOffsetRow(juce::Graphics &g, const d110ui::Palette &pal, juce::Rectangle<float> row,
	                     const juce::String &label, int value, juce::Rectangle<float> &minusBounds,
	                     juce::Rectangle<float> &plusBounds) {
		g.setColour(pal.value);
		g.setFont(juce::FontOptions(13.0f));
		g.drawText(label, row.removeFromLeft(row.getWidth() - 3.0f * kButtonW - 8.0f), juce::Justification::centredLeft);

		plusBounds = row.removeFromRight(kButtonW);
		auto valueBounds = row.removeFromRight(kButtonW);
		minusBounds = row.removeFromRight(kButtonW);

		auto paintButton = [&](juce::Rectangle<float> bounds, const juce::String &text) {
			g.setColour(pal.boxBorder);
			g.drawRoundedRectangle(bounds.reduced(2.0f), 3.0f, 1.0f);
			g.setColour(pal.value);
			g.drawText(text, bounds, juce::Justification::centred);
		};
		paintButton(minusBounds, "-");
		paintButton(plusBounds, "+");
		g.setColour(pal.value);
		g.drawText((value > 0 ? "+" : "") + juce::String(value), valueBounds, juce::Justification::centred);
	}

	bool themeLight = false;
	int programOffset = 0, bankOffset = 0;
	bool extraTracksOn = false;
	bool retroModeOn = false;
	bool quantizeSoft = false;
	juce::Rectangle<float> themeRowBounds, progMinusBounds, progPlusBounds, bankMinusBounds, bankPlusBounds,
		extraTracksRowBounds, retroModeRowBounds, quantizeModeRowBounds, audioRowBounds;
};

// Three clickable fields: MIDI In and MIDI Out (the same idea as
// D110Panel::showOptionsMenu()'s own "MIDI In"/"MIDI Out" submenus, just promoted to
// always-visible since this app has no other menu to tuck them into) and OPTIONS, which
// opens OptionsDialogContent above.
class Toolbar : public juce::Component, private juce::Timer {
public:
	Toolbar(NonetSeqHost &h, std::function<void()> onThemeChangedIn, std::function<void()> onExtraTracksToggledIn,
	        std::function<void()> onSequencerModeChangedIn)
		: host(h), onThemeChanged(std::move(onThemeChangedIn)),
		  onExtraTracksToggled(std::move(onExtraTracksToggledIn)),
		  onSequencerModeChanged(std::move(onSequencerModeChangedIn)) {
		// Just fast enough that a single incoming note visibly flashes the LED (see
		// isMidiInActive()'s own 120ms window) without repainting the whole toolbar needlessly
		// often - MIDI activity is asynchronous (OS MIDI thread) so nothing else would
		// otherwise trigger a repaint when it starts or stops.
		startTimerHz(30);
	}

	void paint(juce::Graphics &g) override {
		const auto &pal = d110ui::palette();
		g.fillAll(pal.panelBg);

		auto b = getLocalBounds().toFloat().reduced(4.0f, 3.0f);
		optionsRect = b.removeFromRight(90.0f).reduced(3.0f, 0.0f);
		auto ledStrip = b.removeFromLeft(10.0f);
		midiInLedBounds = ledStrip.withSizeKeepingCentre(8.0f, 8.0f);
		b.removeFromLeft(4.0f);
		inRect = b.removeFromLeft(b.getWidth() * 0.5f).reduced(3.0f, 0.0f);
		outRect = b.reduced(3.0f, 0.0f);

		paintField(g, inRect, "MIDI In: " + labelFor(NonetSeqHost::midiInputs(), host.getMidiInputId()));
		paintField(g, outRect, "MIDI Out: " + labelFor(NonetSeqHost::midiOutputs(), host.getMidiOutputId()));
		paintField(g, optionsRect, "OPTIONS");

		// Lit for real incoming MIDI on the system port only (see NonetSeqHost::isMidiInActive())
		// - not the on-screen/PC keyboard, so it's a straight answer to "is anything actually
		// reaching this app from my controller", the question that's hard to tell just from
		// whether notes come out the other end.
		g.setColour(host.getMidiInputId().isEmpty() ? pal.seqMetroBeat.withAlpha(0.12f)
		                                             : (host.isMidiInActive() ? pal.seqMetroBeat
		                                                                      : pal.seqMetroBeat.withAlpha(0.25f)));
		g.fillEllipse(midiInLedBounds);
	}

	void mouseDown(const juce::MouseEvent &e) override {
		if (inRect.contains(e.position)) showInMenu();
		else if (outRect.contains(e.position)) showOutMenu();
		else if (optionsRect.contains(e.position)) showOptionsDialog();
	}

private:
	void timerCallback() override { repaint(); }

	void showOptionsDialog() {
		auto *content = new OptionsDialogContent();
		content->setSize(320, 7 * 30 + 6 * 10 + 12);
		auto refresh = [this, content] {
			content->setValues(d110ui::getTheme() == d110ui::Theme::Light, host.getProgramChangeOffset(),
			                    host.getBankOffset(), host.getSequencer().getExtraTracksEnabled(),
			                    host.getSequencerRetroMode(),
			                    host.getSequencer().getQuantizeMode() == d110seq::QuantizeMode::soft);
		};
		refresh();
		content->onThemeToggled = [this, content, refresh] {
			const bool light = d110ui::getTheme() != d110ui::Theme::Light;
			d110ui::setTheme(light ? d110ui::Theme::Light : d110ui::Theme::Dark);
			host.setUiThemeLight(light);
			if (onThemeChanged) onThemeChanged();
			refresh();
		};
		content->onProgramOffsetChanged = [this, content, refresh](int offset) {
			host.setProgramChangeOffset(offset);
			refresh();
		};
		content->onBankOffsetChanged = [this, content, refresh](int offset) {
			host.setBankOffset(offset);
			refresh();
		};
		content->onExtraTracksToggled = [this, refresh] {
			if (onExtraTracksToggled) onExtraTracksToggled();
			refresh();
		};
		content->onRetroModeToggled = [this, refresh] {
			host.setSequencerRetroMode(!host.getSequencerRetroMode());
			if (onSequencerModeChanged) onSequencerModeChanged();
			refresh();
		};
		content->onQuantizeModeToggled = [this, refresh] {
			auto &eng = host.getSequencer();
			eng.setQuantizeMode(eng.getQuantizeMode() == d110seq::QuantizeMode::soft ? d110seq::QuantizeMode::hard
			                                                                        : d110seq::QuantizeMode::soft);
			refresh();
		};
		content->onAudioSettingsRequested = [this] { showAudioSettingsDialog(); };

		auto *aw = new juce::AlertWindow(
			"Options",
			"The offsets below correct the raw Program Change/Bank Select bytes actually sent, in "
			"case an external synth's own manual numbers them differently (from 0 or from 1) than "
			"this app's Program/Bank fields do. Extra tracks: same switch as right-clicking above "
			"the track rows, see docs/sequencer.md. Quantize mode: HARD moves a track's own "
			"recorded notes onto the grid for good; SOFT leaves them exactly as played and only "
			"snaps them live during playback - picking OFF on a track's own quantize control then "
			"plays the original recording again, unchanged. Audio Device opens the usual JUCE "
			"device picker - this app doesn't output sound, but the device chosen there is what "
			"the transport's low-jitter clock is tied to (see docs/sequencer.md's Timing section).",
			juce::AlertWindow::NoIcon);
		aw->addCustomComponent(content);
		aw->addButton("Close", 0, juce::KeyPress(juce::KeyPress::returnKey), juce::KeyPress(juce::KeyPress::escapeKey));
		aw->enterModalState(true, juce::ModalCallbackFunction::create([aw, content](int) {
			delete content;
			delete aw;
		}));
	}

	// Standard JUCE Audio/MIDI Settings picker (device type/output device/sample rate/
	// buffer size), same component juce_StandaloneFilterWindow.h uses for the plugin's own
	// Standalone build's "Audio/MIDI Settings..." menu entry - this app never had an
	// equivalent since it opens its audio device silently (see NonetSeqHost.h's own
	// comment on why one is opened at all). No MIDI input/output pickers here - this app's
	// own MIDI In/Out fields (left of OPTIONS) already cover that, a second control
	// surface for the same thing would just be confusing. Input channels are 0/0 since
	// nothing is ever recorded; output 0/2 matches what NonetSeqHost::deviceManager itself
	// opens.
	void showAudioSettingsDialog() {
		auto *selector = new juce::AudioDeviceSelectorComponent(host.getAudioDeviceManager(), 0, 0, 0, 2, false,
		                                                         false, true, true);
		selector->setSize(500, 400);

		juce::DialogWindow::LaunchOptions o;
		o.content.setOwned(selector);
		o.dialogTitle = "Audio Settings";
		o.dialogBackgroundColour = d110ui::palette().panelBg;
		o.escapeKeyTriggersCloseButton = true;
		o.useNativeTitleBar = true;
		o.resizable = false;
		o.launchAsync();
	}

	static juce::String labelFor(const juce::Array<juce::MidiDeviceInfo> &devs, const juce::String &id) {
		if (id.isEmpty()) return "(none)";
		for (const auto &d : devs)
			if (d.identifier == id) return d.name;
		return id;
	}

	void paintField(juce::Graphics &g, juce::Rectangle<float> r, const juce::String &text) {
		const auto &pal = d110ui::palette();
		g.setColour(pal.box);
		g.fillRoundedRectangle(r, 3.0f);
		g.setColour(pal.boxBorder);
		g.drawRoundedRectangle(r, 3.0f, 1.0f);
		g.setColour(pal.value);
		g.setFont(juce::Font(juce::FontOptions(13.0f)));
		g.drawText(text, r.reduced(6.0f, 0.0f), juce::Justification::centredLeft);
	}

	void showInMenu() {
		const auto devs = NonetSeqHost::midiInputs();
		juce::PopupMenu m;
		m.addItem(1, "(none)", true, host.getMidiInputId().isEmpty());
		for (int i = 0; i < devs.size(); ++i) m.addItem(100 + i, devs[i].name, true, host.getMidiInputId() == devs[i].identifier);
		m.showMenuAsync(juce::PopupMenu::Options(), [this, devs](int result) {
			if (result == 0) return;
			host.setMidiInputDevice(result == 1 ? juce::String() : devs[result - 100].identifier);
			repaint();
		});
	}

	void showOutMenu() {
		const auto devs = NonetSeqHost::midiOutputs();
		juce::PopupMenu m;
		m.addItem(1, "(none)", true, host.getMidiOutputId().isEmpty());
		for (int i = 0; i < devs.size(); ++i) m.addItem(100 + i, devs[i].name, true, host.getMidiOutputId() == devs[i].identifier);
		m.showMenuAsync(juce::PopupMenu::Options(), [this, devs](int result) {
			if (result == 0) return;
			host.setMidiOutputDevice(result == 1 ? juce::String() : devs[result - 100].identifier);
			repaint();
		});
	}

	NonetSeqHost &host;
	std::function<void()> onThemeChanged;
	std::function<void()> onExtraTracksToggled;
	std::function<void()> onSequencerModeChanged;
	juce::Rectangle<float> inRect, outRect, optionsRect, midiInLedBounds;
};

class MainComponent : public juce::Component {
public:
	MainComponent()
		: toolbar(
			  host,
			  [this] {
				  toolbar.repaint();
				  panel.repaint();
				  retroPanel.repaint();
				  keyboard.repaint();
				  repaint();
			  },
			  [this] { panel.toggleExtraTracks(); },
			  [this] {
				  panel.setVisible(!host.getSequencerRetroMode());
				  retroPanel.setVisible(host.getSequencerRetroMode());
				  panel.repaint();
				  retroPanel.repaint();
			  }),
		  panel(host), retroPanel(host), keyboard(host) {
		d110ui::setTheme(host.getUiThemeLight() ? d110ui::Theme::Light : d110ui::Theme::Dark);
		addAndMakeVisible(toolbar);
		addChildComponent(panel);
		addChildComponent(retroPanel);
		panel.setVisible(!host.getSequencerRetroMode());
		retroPanel.setVisible(host.getSequencerRetroMode());
		addAndMakeVisible(keyboard);
		setSize(760, kToolbarHeight + static_cast<int>(D110SequencerPanel::kRefH)
		                 + static_cast<int>(D110Keyboard::kRefH));
	}

	void resized() override {
		auto b = getLocalBounds();
		toolbar.setBounds(b.removeFromTop(kToolbarHeight));
		keyboard.setBounds(b.removeFromBottom(static_cast<int>(D110Keyboard::kRefH)));
		// Same bounds either way (D110SequencerRetroPanel::kRefH matches) - only the one
		// host.getSequencerRetroMode() picked is actually visible.
		panel.setBounds(b);
		retroPanel.setBounds(b);
	}

private:
	// Declared first, deliberately - Toolbar/D110SequencerPanel/D110Keyboard only hold a
	// reference to it, and member destruction runs in reverse declaration order, so this
	// must outlive all three.
	NonetSeqHost host;
	Toolbar toolbar;
	D110SequencerPanel panel;
	D110SequencerRetroPanel retroPanel;
	D110Keyboard keyboard;
};

class MainWindow : public juce::DocumentWindow {
public:
	MainWindow()
		: DocumentWindow("Nonet Sequencer", d110ui::palette().panelBg, DocumentWindow::allButtons) {
		setUsingNativeTitleBar(true);
		// MainComponent's own constructor applies the saved theme (d110ui::setTheme) before
		// this returns, so the background picked above may already be stale - set again now
		// that the real theme is known, rather than starting the window on the wrong palette
		// until the first toggle.
		setContentOwned(new MainComponent(), true);
		setBackgroundColour(d110ui::palette().panelBg);
		centreWithSize(getWidth(), getHeight());
		setResizable(false, false);
		setVisible(true);
	}

	void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class NonetSeqApplication : public juce::JUCEApplication {
public:
	const juce::String getApplicationName() override { return "Nonet Sequencer"; }
	const juce::String getApplicationVersion() override { return "1.0"; }
	bool moreThanOneInstanceAllowed() override { return true; }

	void initialise(const juce::String &) override {
		// Installed before MainWindow so every stock JUCE component this app opens (this
		// AlertWindow-based Options dialog, the Audio Settings dialog's
		// AudioDeviceSelectorComponent, any PopupMenu) is themed from the start rather
		// than needing a second pass - see UiTheme.h's own comment on why this exists and
		// isn't installed by the plugin (which has no stock-JUCE-chrome dialogs to theme).
		juce::LookAndFeel::setDefaultLookAndFeel(&d110ui::sharedLookAndFeel());
		mainWindow = std::make_unique<MainWindow>();
	}
	void shutdown() override {
		mainWindow = nullptr;
		juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
	}

private:
	std::unique_ptr<MainWindow> mainWindow;
};

} // namespace

START_JUCE_APPLICATION(NonetSeqApplication)
