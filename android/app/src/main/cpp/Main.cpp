// Second Android milestone: the real engine (D110AudioProcessor/D110CoreNative), the real
// photographed front panel (D110Panel) with its original hit-tested buttons, the real
// on-screen keyboard (D110Keyboard) - all reused unchanged from the desktop plugin, since
// both are already touch-compatible as-is (see the Android port investigation this follows
// up on). Deliberately NOT included: the sequencer UI, the extended editor drawer (Utility/
// Tone/Patches/... tabs), the memory card - out of scope for "as simple as possible, just
// play MIDI files" per the brief. D110AudioProcessor still owns a sequencer engine instance
// internally (it always does, see plugin/CLAUDE.md) - it's just never surfaced here.
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Source/PluginProcessor.h"
#include "Source/PluginEditor.h"
#include "Source/D110Keyboard.h"

class MainComponent : public juce::Component, private juce::Timer {
public:
	MainComponent() : panel(processor), keyboard(processor) {
		// Bring-up shortcut: ROMs were adb-pushed to the app's own external files dir
		// (see docs/roms.md's automatic-locations list, none of which know about Android's
		// storage model yet). A real Android release needs a proper SAF file-picker here,
		// the same job the desktop ROM setup dialog does with a plain folder chooser -
		// deliberately not built yet, this milestone is about the engine, not ROM setup UX.
		auto romDir = juce::File(
			"/storage/emulated/0/Android/data/com.d110emulator.android/files/roms");
		if (romDir.isDirectory())
			D110AudioProcessor::setCustomRomFolder(romDir.getFullPathName());
		processor.reloadRomsAndPowerOn();

		// Full panel (kRefW=2124) squeezes to illegibility on a phone-portrait width - the LCD
		// and part indicators end up a few pixels tall. Compact mode (see D110Panel::kCompactRefW,
		// PluginEditor.h) splices out the purely decorative Roland wordmark/PHONES jack and the
		// MEMORY CARD slot - nothing this app needs (no memory card UI here at all) - leaving
		// VOLUME/LCD/buttons/POWER/MIDI lamp at a noticeably larger effective zoom for the same
		// screen width. resized() below reads currentRefW(true) to match.
		processor.setCompactPanelMode(true);

		addAndMakeVisible(panel);
		addAndMakeVisible(keyboard);

		playButton.setButtonText("Play");
		playButton.onClick = [this] { togglePlayPause(); };
		addAndMakeVisible(playButton);

		stopButton.setButtonText("Stop");
		stopButton.onClick = [this] { stopPlayback(); };
		addAndMakeVisible(stopButton);

		// "Load MIDI file..." and the octave Options both moved in here (Alan's request,
		// 2026-08-22): landscape has little enough vertical room that even one dedicated
		// button per action was squeezing the panel/keyboard below it. Play/Stop stay as
		// real buttons since those are what gets pressed constantly during playback; the
		// other two are one-tap-then-done settings, exactly what a menu is for.
		menuButton.setButtonText(juce::String::fromUTF8("\xe2\x98\xb0")); // U+2630 TRIGRAM FOR HEAVEN ("hamburger")
		menuButton.onClick = [this] { showMainMenu(); };
		addAndMakeVisible(menuButton);

		statusLabel.setJustificationType(juce::Justification::centredLeft);
		statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
		statusLabel.setText(processor.isSynthReady() ? "Ready" : processor.getLastError(),
		                     juce::dontSendNotification);
		addAndMakeVisible(statusLabel);

		// AudioProcessorPlayer is what a JUCE Standalone build's own StandaloneFilterWindow
		// uses internally to bridge a real AudioProcessor to a live device - same idea here,
		// minus the desktop window chrome (title bar, audio-settings dialog, resize
		// constrainer) that doesn't correspond to anything on a fixed, OS-managed Activity
		// surface. Its audioDeviceAboutToStart() is what actually calls prepareToPlay(),
		// which calls setPoweredOn(true) - idempotent, so doing it twice (once explicitly
		// above via reloadRomsAndPowerOn(), once here once the device starts) is harmless.
		deviceManager.initialiseWithDefaultDevices(0, 2);
		player.setProcessor(&processor);
		deviceManager.addAudioCallback(&player);

		// No setSize() call needed here: MainWindow's setContentOwned()/setFullScreen(true)
		// (ResizableWindow::resized() -> setBoundsInset()) keep this synced to the actual
		// window bounds on every resize/rotation, so a hardcoded guess here would only ever
		// be a one-frame placeholder immediately overwritten.
		startTimerHz(30);
	}

	~MainComponent() override {
		deviceManager.removeAudioCallback(&player);
		player.setProcessor(nullptr);
	}

	void paint(juce::Graphics &g) override { g.fillAll(juce::Colour(0xff1e1e22)); }

	void resized() override {
		// In landscape, the 3-button nav bar (back/home/recents) sits as a vertical strip
		// down one side of the screen rather than along the bottom, and since this window
		// draws edge-to-edge (see AndroidManifest's default window flags), that strip was
		// overlapping the right end of the panel/keyboard - specifically the keyboard's own
		// "+" octave button, unreachable underneath the system nav bar. safeAreaInsets is
		// JUCE's own cross-platform answer to exactly this (also covers notches/status bar,
		// though those don't collide with anything here) - insetting the whole layout by it
		// keeps every control clear of system UI on any device/orientation, not just this one.
		auto area = getLocalBounds();
		if (auto *display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(
		        getScreenBounds()))
			area = display->safeAreaInsets.subtractedFrom(area);

		auto transport = area.removeFromTop(72);
		menuButton.setBounds(transport.removeFromRight(72).reduced(6));
		const int halfWidth = transport.getWidth() / 2;
		playButton.setBounds(transport.removeFromLeft(halfWidth).reduced(6));
		stopButton.setBounds(transport.reduced(6));

		statusLabel.setBounds(area.removeFromTop(40).reduced(10, 0));

		// D110Panel paints and hit-tests entirely in its own fixed reference-pixel space
		// (kCompactRefW x kRefH) and relies on the PARENT applying a Component::setTransform
		// scale to make that visually fit whatever window size there actually is - it does not
		// accept being resized to arbitrary pixel bounds itself (its paint() draws the panel
		// photo untransformed via drawImageAt(0,0), so a smaller setBounds just clips to a
		// corner instead of scaling down - this is what produced the giant single LCD segments
		// filling the whole screen before this fix). The desktop editor's own resized() gets
		// away with a bare scale(s) transform (no translation) because its panel always sits at
		// parent-space y=0 there - a pure scale is centred on the PARENT's origin, not the
		// component's own bounds position, so with any other y offset the scaled content lands
		// at y*s, not y, sliding it back up under whatever's above it (here, hidden entirely
		// behind the load/stop buttons added later - later-added children paint on top). Folding
		// the real pixel offset into the transform itself (scaled first, then translated) is
		// what actually places it where setBounds' own y argument suggests it should be.
		const float scale = float(area.getWidth()) / float(D110Panel::currentRefW(true));
		const int panelY = area.getY();
		panel.setBounds(0, 0, D110Panel::currentRefW(true), D110Panel::kRefH);
		panel.setTransform(juce::AffineTransform::scale(scale).translated(0.0f, float(panelY)));
		panel.setDisplayScale(scale);
		area.removeFromTop(juce::roundToInt(D110Panel::kRefH * scale));

		// D110Keyboard, unlike D110Panel, scales itself internally from whatever real pixel
		// bounds it's given - same real-pixel setBounds() call the desktop editor uses. The
		// desktop editor only gives it kRefH*scale (a thin strip, sized to match the panel's
		// own key art) because it's squeezed between other foldable drawers there; this app has
		// nothing below it, so it can afford taller keys - more finger room on a phone
		// touchscreen - capped at the panel's own height rather than all remaining space: in
		// portrait that remaining space is most of the screen, and keys that tall are no easier
		// to play, just mostly empty finger travel between touch-down and the key itself.
		keyboard.setBounds(area.removeFromTop(juce::jmin(area.getHeight(),
		                                                  juce::roundToInt(D110Panel::kRefH * scale))));
	}

private:
	// The only setting this "as simple as possible" app exposes so far - Alan's request
	// (2026-08-22): a phone-portrait screen only comfortably fits one octave's worth of keys
	// at a finger-sized width, but the two-octave default (unchanged from the desktop
	// keyboard) stays available for anyone who'd rather have the range. A touch UI has no
	// right-click for D110Keyboard's own desktop options menu (channel/omni/PC-keyboard,
	// none of which apply here anyway - no physical keyboard, and Omni doesn't matter with
	// only ever one MIDI destination), so this is a dedicated button instead.
	// Everything that isn't Play/Stop lives behind this one button (Alan's request,
	// 2026-08-22, replacing a dedicated "Load MIDI file..." button and a dedicated "Options"
	// button - one row of controls, not two, since landscape leaves the panel/keyboard little
	// enough vertical room already).
	void showMainMenu() {
		juce::PopupMenu octaves;
		octaves.addItem(1, "1 Octave", true, keyboard.getNumOctaves() == 1);
		octaves.addItem(2, "2 Octaves", true, keyboard.getNumOctaves() == 2);

		juce::PopupMenu m;
		m.addItem(100, "Load MIDI file...");
		m.addSeparator();
		m.addSubMenu("Options", octaves);
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menuButton),
		                [this](int result) {
			                if (result == 100) chooseMidiFile();
			                else if (result == 1) keyboard.setNumOctaves(1);
			                else if (result == 2) keyboard.setNumOctaves(2);
		                });
	}

	void chooseMidiFile() {
		// Async dialog, so the chooser has to outlive this call - same trick as every
		// other FileChooser in the desktop editor.
		fileChooser = std::make_unique<juce::FileChooser>("Choose a MIDI file to play",
		                                                  juce::File(), "*.mid;*.midi");
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
			[this](const juce::FileChooser &fc) {
				// getResult() (a juce::File) is what the desktop pickers hand back, but on
				// Android the system picker returns a content:// SAF URI with no real path on
				// disk. File::createInputStream() on it silently fails, AND (first attempt at
				// this fix) so does plain URL::createInputStream() - that path is for HTTP(S)
				// URLs; it only checks isLocalFile() (scheme=="file") before falling through to
				// a WebInputStream, which naturally can't open a "content://" address either.
				// juce::AndroidDocument is JUCE's actual answer to a SAF result (its own class
				// comment names this exact use case: "pass the FileChooser's URL result to
				// AndroidDocument::fromDocument... createInputStream to read from the file").
				auto url = fc.getURLResult();
				if (url != juce::URL()) loadAndPlayMidi(url);
			});
	}

	// Roland's own factory default is Part N -> MIDI channel N+1 (docs/factory_defaults.md,
	// "Part 1 listens on channel 2... up to part 8 on channel 9") - not a straightforward
	// per-track "channel N" convention. But every part-authoring tool that just numbers its
	// tracks/channels 1-8 sequentially (this app's own test file among them - Alan confirmed
	// he never remapped anything, and the file itself carries no SysEx touching the SYSTEM
	// area's channel table either) implicitly assumes a D-110 already reconfigured that way.
	// Doing that reconfiguration ourselves before playback - and only for as long as playback
	// needs it - means a plain "channel N for part N" MIDI file just works, without asking
	// the user to go press SYSTEM buttons on the panel first.
	//
	// SysEx field layout (D110CoreType::kRamSystem+13.. per native_editor_tone_repro_probe.cpp,
	// matching docs/sysex_address_map.md's "0x2D98 == SysEx 0x100004, partial reserve + MIDI
	// channel map, 18 bytes" - 9 bytes partial reserve at fields 4-12, 9 bytes channel map at
	// fields 13-21 for Parts 1-8 + Rhythm, stored 0-based (value = channel-1)): field 13+p is
	// Part (p+1)'s channel. Rhythm (field 21) already defaults to channel 10 either way, so
	// it's left untouched.
	void setSequentialPartChannelMap(bool sequential) {
		for (int part = 0; part < 8; ++part)
			processor.sendSystemParam(13 + part, juce::uint8(sequential ? part : part + 1));
	}

	void loadAndPlayMidi(const juce::URL &url) {
		auto document = juce::AndroidDocument::fromDocument(url);
		auto stream = document.hasValue() ? document.createInputStream() : nullptr;
		if (stream == nullptr) {
			statusLabel.setText("Could not open: " + url.getFileName(), juce::dontSendNotification);
			return;
		}

		juce::MidiFile midiFile;
		if (!midiFile.readFrom(*stream)) {
			statusLabel.setText("Could not read: " + url.getFileName(), juce::dontSendNotification);
			return;
		}
		midiFile.convertTimestampTicksToSeconds();

		sequence.clear();
		for (int i = 0; i < midiFile.getNumTracks(); ++i)
			sequence.addSequence(*midiFile.getTrack(i), 0.0);
		sequence.updateMatchedPairs();

		loadedFileName = url.getFileName();
		eventIndex = 0;
		pausedElapsedSeconds = 0.0;
		playing = sequence.getNumEvents() > 0;
		if (playing) setSequentialPartChannelMap(true);
		playStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
		statusLabel.setText(playing ? "Playing: " + loadedFileName : "Empty MIDI file",
		                     juce::dontSendNotification);
	}

	// Play doubles as Pause: pressed while playing, it stops advancing but remembers position
	// (pausedElapsedSeconds), same as Stop used to - the difference is Stop now always rewinds
	// to the top instead, so Play is the only way to leave off mid-file and pick back up later.
	// Either transition needs a MIDI panic - the file player only ever sends note-ONs/offs as
	// the sequence dictates, so stopping mid-note (or mid-sustain-pedal) would otherwise leave
	// it ringing forever with nothing left to send the matching note-off.
	void togglePlayPause() {
		if (playing) {
			pausedElapsedSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001 - playStartSeconds;
			playing = false;
			processor.midiPanic();
			statusLabel.setText("Paused: " + loadedFileName, juce::dontSendNotification);
			return;
		}
		if (sequence.getNumEvents() == 0) return;
		if (eventIndex >= sequence.getNumEvents()) {
			eventIndex = 0;
			pausedElapsedSeconds = 0.0;
		}
		setSequentialPartChannelMap(true);
		playStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001 - pausedElapsedSeconds;
		playing = true;
		statusLabel.setText("Playing: " + loadedFileName, juce::dontSendNotification);
	}

	// Always rewinds to the top, unlike Play-as-pause - Stop means "start over", not "leave off
	// here". The panic is what actually guarantees silence: without it, any note or sustain
	// pedal active at the moment of stopping would otherwise never get its matching note-off.
	void stopPlayback() {
		playing = false;
		eventIndex = 0;
		pausedElapsedSeconds = 0.0;
		processor.midiPanic();
		setSequentialPartChannelMap(false); // back to factory default (Part N -> channel N+1)
		statusLabel.setText("Stopped", juce::dontSendNotification);
	}

	// Deliberately not the sequencer engine (D110SequencerEngine) - that's a full transport/
	// undo/quantize model built for interactive editing, out of scope here. Just a plain
	// cursor over a merged MidiMessageSequence, timed against a wall-clock start - the
	// simplest thing that plays a file's notes/program changes/CCs in order.
	// The D-110 has no live-MIDI Channel Volume/Pan concept at all - confirmed on a real DAW
	// session during the desktop plugin's own MIDI-reimport work (see
	// project_midi_reimport_volume_fix memory): CC7/CC10 sent over the wire are silently
	// ignored by the firmware. That fix's answer was a DIRECT SysEx write to the part's own
	// TimbreTemp Level/Pan fields (sendTimbreTempParam, fields 8/9) instead of a live CC -
	// the exact same call the Parts tab's own Level/Pan columns use. This simple player needs
	// the same substitution: without it, a file's CC7 (e.g. track 7's Volume=43, meant to tame
	// "Bombastic" down from full scale) is a no-op, and the part keeps playing at whatever
	// level its patch already had - which is what Alan heard as "un peu trop fort".
	//
	// Only meaningful for Parts 1-8 - channel-1 here is a part index (0-7) because playback
	// keeps the SYSTEM channel map remapped sequentially (setSequentialPartChannelMap) for as
	// long as it's running, same assumption the rest of this player already makes. Channels
	// outside 1-8 (rhythm's channel 10, or anything unused) fall through as an ordinary,
	// harmless live CC.
	void injectPlaybackEvent(const juce::MidiMessage &message) {
		const int part = message.getChannel() - 1;
		if (message.isController() && part >= 0 && part < 8) {
			if (message.getControllerNumber() == 7) {
				const int level = juce::jlimit(0, 100,
					juce::roundToInt(message.getControllerValue() * 100.0f / 127.0f));
				processor.sendTimbreTempParam(part, 8, juce::uint8(level));
				return;
			}
			if (message.getControllerNumber() == 10) {
				const int pan = juce::jlimit(0, 14,
					juce::roundToInt(message.getControllerValue() * 14.0f / 127.0f));
				processor.sendTimbreTempParam(part, 9, juce::uint8(pan));
				return;
			}
		}
		processor.injectMidiMessage(message);
	}

	void timerCallback() override {
		if (!playing) return;
		const double elapsed = juce::Time::getMillisecondCounterHiRes() * 0.001 - playStartSeconds;
		while (eventIndex < sequence.getNumEvents()
		       && sequence.getEventPointer(eventIndex)->message.getTimeStamp() <= elapsed) {
			injectPlaybackEvent(sequence.getEventPointer(eventIndex)->message);
			++eventIndex;
		}
		if (eventIndex >= sequence.getNumEvents()) {
			playing = false;
			statusLabel.setText("Finished", juce::dontSendNotification);
		}
	}

	D110AudioProcessor processor;
	D110Panel panel;
	D110Keyboard keyboard;
	juce::TextButton playButton, stopButton, menuButton;
	juce::Label statusLabel;
	std::unique_ptr<juce::FileChooser> fileChooser;

	juce::AudioDeviceManager deviceManager;
	juce::AudioProcessorPlayer player;

	juce::MidiMessageSequence sequence;
	juce::String loadedFileName;
	int eventIndex = 0;
	double playStartSeconds = 0;
	double pausedElapsedSeconds = 0;
	bool playing = false;
};

class MainWindow : public juce::DocumentWindow {
public:
	MainWindow(const juce::String &name)
		: DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::allButtons) {
		setUsingNativeTitleBar(true);
		setContentOwned(new MainComponent(), true);
		setFullScreen(true);
		setVisible(true);
	}

	void closeButtonPressed() override {
		juce::JUCEApplication::getInstance()->systemRequestedQuit();
	}
};

class D110AndroidApp : public juce::JUCEApplication {
public:
	const juce::String getApplicationName() override { return "d110"; }
	const juce::String getApplicationVersion() override { return "0.1"; }

	void initialise(const juce::String &) override {
		mainWindow = std::make_unique<MainWindow>(getApplicationName());
	}

	void shutdown() override { mainWindow = nullptr; }

private:
	std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(D110AndroidApp)
