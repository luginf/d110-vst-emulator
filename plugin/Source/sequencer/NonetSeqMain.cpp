// Nonet Sequencer: a bare window around D110SequencerPanel and a small toolbar to pick
// the direct system MIDI In/Out ports, backed by NonetSeqHost - no firmware, no ROMs, no
// plugin wrapper. See docs/sequencer.md's "MIDI Out" section for why this exists (driving
// a real D-110, or any other synth, without the emulation/DAW in the loop at all) and
// NonetSeqHost.h for what it does and doesn't do.

#include <juce_gui_extra/juce_gui_extra.h>

#include "../D110Keyboard.h"
#include "../UiTheme.h"
#include "D110SequencerPanel.h"
#include "NonetSeqHost.h"

namespace {

constexpr int kToolbarHeight = 28;

// Three clickable fields: MIDI In and MIDI Out (the same idea as
// D110Panel::showOptionsMenu()'s own "MIDI In"/"MIDI Out" submenus, just promoted to
// always-visible since this app has no other menu to tuck them into) and a mini utility
// field for THEME, the one setting this standalone app needs a way to change that the
// D-110 plugin tucks into its Utility tab instead.
class Toolbar : public juce::Component {
public:
	Toolbar(NonetSeqHost &h, std::function<void()> onThemeChangedIn)
		: host(h), onThemeChanged(std::move(onThemeChangedIn)) {}

	void paint(juce::Graphics &g) override {
		const auto &pal = d110ui::palette();
		g.fillAll(pal.panelBg);

		auto b = getLocalBounds().toFloat().reduced(4.0f, 3.0f);
		themeRect = b.removeFromRight(90.0f).reduced(3.0f, 0.0f);
		inRect = b.removeFromLeft(b.getWidth() * 0.5f).reduced(3.0f, 0.0f);
		outRect = b.reduced(3.0f, 0.0f);

		paintField(g, inRect, "MIDI In: " + labelFor(NonetSeqHost::midiInputs(), host.getMidiInputId()));
		paintField(g, outRect, "MIDI Out: " + labelFor(NonetSeqHost::midiOutputs(), host.getMidiOutputId()));
		paintField(g, themeRect, d110ui::getTheme() == d110ui::Theme::Light ? "LIGHT" : "DARK");
	}

	void mouseDown(const juce::MouseEvent &e) override {
		if (inRect.contains(e.position)) showInMenu();
		else if (outRect.contains(e.position)) showOutMenu();
		else if (themeRect.contains(e.position)) {
			const bool light = d110ui::getTheme() != d110ui::Theme::Light;
			d110ui::setTheme(light ? d110ui::Theme::Light : d110ui::Theme::Dark);
			host.setUiThemeLight(light);
			if (onThemeChanged) onThemeChanged();
		}
	}

private:
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
	juce::Rectangle<float> inRect, outRect, themeRect;
};

class MainComponent : public juce::Component {
public:
	MainComponent()
		: toolbar(host, [this] {
			  toolbar.repaint();
			  panel.repaint();
			  keyboard.repaint();
			  repaint();
		  }),
		  panel(host), keyboard(host) {
		d110ui::setTheme(host.getUiThemeLight() ? d110ui::Theme::Light : d110ui::Theme::Dark);
		addAndMakeVisible(toolbar);
		addAndMakeVisible(panel);
		addAndMakeVisible(keyboard);
		setSize(760, kToolbarHeight + static_cast<int>(D110SequencerPanel::kRefH)
		                 + static_cast<int>(D110Keyboard::kRefH));
	}

	void resized() override {
		auto b = getLocalBounds();
		toolbar.setBounds(b.removeFromTop(kToolbarHeight));
		keyboard.setBounds(b.removeFromBottom(static_cast<int>(D110Keyboard::kRefH)));
		panel.setBounds(b);
	}

private:
	// Declared first, deliberately - Toolbar/D110SequencerPanel/D110Keyboard only hold a
	// reference to it, and member destruction runs in reverse declaration order, so this
	// must outlive all three.
	NonetSeqHost host;
	Toolbar toolbar;
	D110SequencerPanel panel;
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

	void initialise(const juce::String &) override { mainWindow = std::make_unique<MainWindow>(); }
	void shutdown() override { mainWindow = nullptr; }

private:
	std::unique_ptr<MainWindow> mainWindow;
};

} // namespace

START_JUCE_APPLICATION(NonetSeqApplication)
