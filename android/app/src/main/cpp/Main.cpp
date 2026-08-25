// Second Android milestone: the real engine (D110AudioProcessor/D110CoreNative), the real
// photographed front panel (D110Panel) with its original hit-tested buttons, the real
// on-screen keyboard (D110Keyboard), and (2026-08-22) the real sequencer - both its normal
// grid view (D110SequencerPanel, the default here, matching the desktop plugin's own default)
// and its retro D-pad+LCD view (D110SequencerRetroPanel, one Options tap away) - all reused
// unchanged from the desktop plugin, since all of them are already touch-compatible as-is
// (see the Android port investigation this follows up on). The grid view's right-click menus
// (quantize, tempo, load/save, ...) have no right mouse button to fire them on a touchscreen,
// so D110SequencerPanel itself grew a long-press equivalent (see its own mouseDown() comment)
// reaching the exact same menus - the retro view never needed this, having none at all (its
// own D-pad/button cluster covers everything a real D-20 does, only in a smaller frame). A
// long PRESS still doesn't belong on the piano keys themselves (see D110Keyboard.h's own note
// on why holding a key can't double as one - it's an ordinary sustained note), which is why
// that menu stays reachable from the hamburger instead. Deliberately NOT included: the
// extended editor drawer (Utility/Tone/Patches/... tabs), the memory card - out of scope for
// "as simple as possible" per the original brief, and nothing added since has needed them.
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Source/PluginProcessor.h"
#include "Source/PluginEditor.h"
#include "Source/D110Keyboard.h"
#include "Source/sequencer/D110SequencerPanel.h"
#include "Source/sequencer/D110SequencerRetroPanel.h"

class MainComponent : public juce::Component, private juce::Timer, private juce::MidiInputCallback {
public:
	// The app's own external files dir - the one location the native core can always read
	// with a plain filesystem path on Android (see chooseRomFolder()'s own comment for why
	// that rules out pointing setCustomRomFolder() at an arbitrary SAF folder directly).
	static juce::File romBringUpDir() {
		return juce::File("/storage/emulated/0/Android/data/com.d110emulator.android/files/roms");
	}

	MainComponent() : panel(processor), keyboard(processor), gridSeq(processor), retroSeq(processor) {
		// Bring-up shortcut, still the path chooseRomFolder() (hamburger menu) now also copies
		// into - see its own comment. Originally ROMs only got there via `adb push`; the
		// picker below is the "proper SAF file-picker" this comment used to say was still
		// missing.
		if (romBringUpDir().isDirectory())
			D110AudioProcessor::setCustomRomFolder(romBringUpDir().getFullPathName());
		processor.reloadRomsAndPowerOn();
		loadPersistedState();

		// Full panel (kRefW=2124) squeezes to illegibility on a phone-portrait width - the LCD
		// and part indicators end up a few pixels tall. Compact mode (see D110Panel::kCompactRefW,
		// PluginEditor.h) splices out the purely decorative Roland wordmark/PHONES jack and the
		// MEMORY CARD slot - nothing this app needs (no memory card UI here at all) - leaving
		// VOLUME/LCD/buttons/POWER/MIDI lamp at a noticeably larger effective zoom for the same
		// screen width. resized() below reads currentRefW(true) to match.
		processor.setCompactPanelMode(true);

		addAndMakeVisible(panel);
		// Both hidden until the hamburger menu's "Sequencer" is picked; which of the two then
		// shows follows processor.getSequencerRetroMode() (default false - normal/grid),
		// the exact same flag and default the desktop editor's own Options menu uses.
		addChildComponent(gridSeq);
		addChildComponent(retroSeq);
		// See buildAppMenu()'s own comment: the grid sequencer hides the app's whole
		// Play/Stop/hamburger row to get its full height, so its own bar-navigation menu
		// button becomes the only way back to it.
		gridSeq.onBarMenuButtonExtra = [this](juce::PopupMenu &m) { buildAppMenu(m); };
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
		scanForMidiInputs(); // catches a keyboard already plugged in before launch immediately,
		                     // rather than waiting for the first periodic scan
		startTimerHz(30);
	}

	~MainComponent() override {
		saveState();
		deviceManager.removeAudioCallback(&player);
		player.setProcessor(nullptr);
		for (const auto &id : knownMidiInputs) {
			deviceManager.removeMidiInputDeviceCallback(id, this);
			deviceManager.setMidiInputDeviceEnabled(id, false);
		}
	}

	// Unlike the desktop Standalone target (a real juce::StandaloneFilterApp, which
	// autosaves/reloads processor.getStateInformation()/setStateInformation() through its own
	// settings file), this Android app is a bare JUCEApplication with nothing wired up to
	// either call at all - confirmed as the root cause of Alan's 2026-08-23 report that
	// quitting the Android app loses every song ("comme si on avait réinitialisé la
	// mémoire"). Fixed the same way NonetSeqHost persists its own settings: one flat file in
	// the app's private data dir holding the exact same binary blob getStateInformation()/
	// setStateInformation() already produce/consume everywhere else (sequencer songs/tracks,
	// tempo, retro key bindings, LCD mode, keyboard config, theme - literally everything that
	// blob covers). Loaded once here, right after reloadRomsAndPowerOn() has already booted
	// the firmware fresh - restoring firmware NVRAM bytes into files on disk after boot only
	// takes visible effect on the NEXT power-on, but the D-110 core already flushes its own
	// NVRAM to disk continuously during normal operation independent of this (see
	// project_standalone_nvram_persistence_fix in project memory), so the only thing this
	// call actually needs to restore here is the higher-level state - which it does
	// unconditionally, regardless of that NVRAM-timing nuance.
	//
	// Loading on construction isn't enough by itself: Android can (and does) kill this whole
	// process without warning once it's backgrounded, well before any orderly C++ destructor
	// chain would run - see D110AndroidApp::suspended(), the actual save point that matters,
	// which calls this too. The destructor above only covers the rarer case of a clean
	// in-app quit.
	void saveState() {
		juce::MemoryBlock data;
		processor.getStateInformation(data);
		stateFile().getParentDirectory().createDirectory();
		stateFile().replaceWithData(data.getData(), data.getSize());
	}

private:
	static juce::File stateFile() {
		return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
			.getChildFile("d110-state.bin");
	}

	void loadPersistedState() {
		auto f = stateFile();
		if (!f.existsAsFile()) return;
		juce::MemoryBlock data;
		if (f.loadFileAsData(data) && data.getSize() > 0)
			processor.setStateInformation(data.getData(), (int) data.getSize());
	}

public:

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
		//
		// getDisplayForRect()'s live safeAreaInsets turned out too unreliable to build this on
		// (2026-08-22): occasionally null right after rotating or switching to/from the
		// sequencer, and a caching fix on top of that (remember the last successful reading,
		// reuse it whenever a fresh lookup failed) had its own gap - caching blindly across a
		// ROTATION could carry over a WRONG-SHAPED inset (landscape's right inset, reapplied
		// after rotating back to portrait, where the real nav bar has moved to the bottom) if
		// the live lookup happened to succeed-but-stale mid-rotation, or kept failing across a
		// full rotate-and-back cycle - worse than no inset, since it leaves controls sitting
		// under the real bar while looking like it should be clear of it. That's what Alan hit
		// ("apres un aller-retour ca deborde dans le taskbar, ca rend le sequenceur inutilisable").
		//
		// Replaced with something that has no state to go stale in the first place: a fixed
		// guess, recomputed fresh from isLandscape on every single resize, no live lookup at
		// all. Less precise than a real per-device measurement would be, but a comfortably
		// oversized guess costs a few dp of unused margin on devices that don't need it, where
		// a wrong live reading costs an unreachable button - the guess is the safer failure
		// mode. Alan also found this can't be fully auto-detected anyway: some Android tablets
		// keep the nav bar bottom-anchored even in landscape rather than moving it to a side
		// the way phones do, and there's no generic signal that distinguishes that from a
		// normal phone. navTop/Bottom/Left/Right below are the manual escape hatch - four
		// independent checkboxes (not a single "which side" choice) because a real device can
		// need two at once (status bar on top AND a bottom-anchored nav bar, even rotated).
		auto area = getLocalBounds();
		const bool isLandscape = getWidth() > getHeight();
		constexpr int kStatusBarInset = 32; // logical px - comfortably more than a status bar
		constexpr int kNavBarInset = 64;    // logical px - comfortably more than a 3-button/gesture bar
		juce::BorderSize<int> insets;
		if (navTop || navBottom || navLeft || navRight) {
			insets = { navTop ? kStatusBarInset : 0, navLeft ? kNavBarInset : 0,
			           navBottom ? kNavBarInset : 0, navRight ? kNavBarInset : 0 };
		} else {
			// Auto: the ordinary phone layout - status bar on top always, nav bar on the
			// "closing" edge for the current orientation (right in landscape, bottom in
			// portrait).
			insets = isLandscape ? juce::BorderSize<int>(kStatusBarInset, 0, 0, kNavBarInset)
			                     : juce::BorderSize<int>(kStatusBarInset, 0, kNavBarInset, 0);
		}
		area = insets.subtractedFrom(area);

		// Play/Stop/hamburger and the status line are hidden entirely while the GRID sequencer
		// is showing (Alan's request, 2026-08-22): it has its own STOP/PLAY/REC transport
		// already, so the app's copies were pure duplication, and landscape doesn't have height
		// to spare for duplication - every pixel they used to take goes to the sequencer
		// instead. The hamburger's own role (Load MIDI file/Panel switch/Keyboard channel/
		// Options) doesn't disappear with it: onBarMenuButtonExtra below feeds those same items
		// into the grid's own new bar-navigation menu button instead, so there's still exactly
		// one way to reach them, just relocated.
		//
		// Retro keeps the hamburger (it has no equivalent menu button of its own to relocate
		// Load MIDI file/Panel switch/etc onto - its whole design is a D-pad/button cluster
		// with no free-text menus at all, so hiding the app's own hamburger there would leave
		// no way back to Panel view). Play/Stop and the status line DO still hide in retro,
		// though (Alan's request, 2026-08-23): those are for the app's OWN "Load MIDI file..."
		// playback feature, not the sequencer transport - sitting right above retro's own
		// STOP/PLAY/REC, they read as duplicates of it even though they do something
		// completely unrelated, which is exactly the confusion Alan reported.
		const bool inGridSequencer = showingSequencer && !processor.getSequencerRetroMode();
		const bool inRetroSequencer = showingSequencer && processor.getSequencerRetroMode();
		playButton.setVisible(!inGridSequencer && !inRetroSequencer);
		stopButton.setVisible(!inGridSequencer && !inRetroSequencer);
		menuButton.setVisible(!inGridSequencer);
		statusLabel.setVisible(!inGridSequencer && !inRetroSequencer);
		if (!inGridSequencer) {
			// Retro landscape's top strip only ever holds the hamburger (Play/Stop/status
			// stay hidden there - see this function's own comment above on why) - the full
			// 72px transport-row height was sized for Panel mode's 3-button row, wasted here
			// on one corner button. Alan's request, 2026-08-24: shrink the button by half and
			// hand every pixel reclaimed from the strip to the sequencer/keyboard below
			// (nothing else needed to explicitly "shift up" - less removed from the top of
			// `area` here means more of it left for them further down).
			const bool shrinkHamburger = inRetroSequencer && isLandscape;
			const int transportH = shrinkHamburger ? 36 : 72;
			const int menuButtonW = shrinkHamburger ? 36 : 72;
			auto transport = area.removeFromTop(transportH);
			menuButton.setBounds(transport.removeFromRight(menuButtonW).reduced(shrinkHamburger ? 3 : 6));
			if (!inRetroSequencer) {
				const int halfWidth = transport.getWidth() / 2;
				playButton.setBounds(transport.removeFromLeft(halfWidth).reduced(6));
				stopButton.setBounds(transport.reduced(6));

				statusLabel.setBounds(area.removeFromTop(40).reduced(10, 0));
			}
		}

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

		// The hamburger menu's "Sequencer" entry swaps this whole band between the front panel
		// and the sequencer (Alan's request, 2026-08-22) - only one at a time, since a phone
		// doesn't have room for both plus the keyboard the way the desktop editor stacks its
		// drawers. Grid (D110SequencerPanel) is the default, matching the desktop editor's own
		// default - retro is one Options tap away (processor.getSequencerRetroMode(), the exact
		// same flag). Both sequencer views self-scale from real pixel bounds exactly like
		// D110Keyboard, unlike D110Panel's fixed-reference-space+transform below.
		if (showingSequencer) {
			panel.setVisible(false);
			const bool retro = processor.getSequencerRetroMode();
			gridSeq.setVisible(!retro);
			retroSeq.setVisible(retro);

			// Unlike Panel mode below, the sequencer here is NOT capped at its own reference
			// height (kRefH) - both D110SequencerPanel and D110SequencerRetroPanel lay
			// themselves out in PERCENTAGES of whatever real pixel bounds they're given
			// (self-scaling, exactly like D110Keyboard below, not fixed-reference-space like
			// D110Panel), so handing them extra height makes every row - the 9 track rows
			// especially - proportionally taller rather than just leaving it centred in unused
			// space. The keyboard also only gets ITS OWN small reference height here
			// (D110Keyboard::kRefH = 130) rather than borrowing D110Panel::kRefH's much taller
			// one (256, sized for sitting under the panel PHOTO) the way Panel mode's keyboard
			// does below - that mismatch was exactly the two complaints Alan raised 2026-08-22:
			// the keyboard sitting taller than it needed to, and the leftover gap under it that
			// created neither made it to the sequencer.
			//
			// `scale` collapses in portrait for the exact same reason it does in Panel mode
			// below (it's derived from width relative to the PANEL's reference width, which
			// portrait squeezes to a fraction of its natural size) - Alan's "le clavier est
			// toujours ridiculement petit" after the Panel-mode fix already went in, because
			// that fix never touched this second copy of the same formula. Landscape keeps the
			// scale-based height (already tuned and confirmed there); portrait gets the same
			// width-based cap Panel mode uses, just a bit tighter (width/5 not /4) to leave more
			// of the screen to the 9 track rows, which is the whole point of this view.
			//
			// Retro landscape specifically gets a further x1.6 on top of that (Alan's request,
			// 2026-08-24, "environ 60% plus haut") - Grid keeps the plain scale-based height,
			// this is retro only.
			const float retroLandscapeBoost = (retro && isLandscape) ? 1.6f : 1.0f;
			const int keyboardHeight =
				isLandscape
					? juce::jmin(area.getHeight(), juce::roundToInt(D110Keyboard::kRefH * scale * retroLandscapeBoost))
					: juce::jmin(area.getHeight(), juce::roundToInt(area.getWidth() / 5.0f));
			auto seqBounds = area;
			seqBounds.removeFromBottom(keyboardHeight);
			gridSeq.setBounds(seqBounds);
			retroSeq.setBounds(seqBounds);
			keyboard.setBounds(area.removeFromBottom(keyboardHeight));
		} else {
			gridSeq.setVisible(false);
			retroSeq.setVisible(false);
			panel.setVisible(true);
			const int panelY = area.getY();
			panel.setBounds(0, 0, D110Panel::currentRefW(true), D110Panel::kRefH);
			panel.setTransform(juce::AffineTransform::scale(scale).translated(0.0f, float(panelY)));
			panel.setDisplayScale(scale);
			area.removeFromTop(juce::roundToInt(D110Panel::kRefH * scale));

			// D110Keyboard, unlike D110Panel, scales itself internally from whatever real pixel
			// bounds it's given - same real-pixel setBounds() call the desktop editor uses. The
			// old cap here (D110Panel::kRefH*scale) tied keyboard height to the PANEL's own
			// reference height through `scale`, which is derived from width relative to the
			// panel's reference width (1497) - fine in landscape, where that width fills most of
			// the screen, but in portrait the panel is squeezed to a fraction of its natural
			// width, dragging `scale` (and therefore the keyboard height riding on it) down to
			// almost nothing: Alan's "trop petit" in portrait. Filling ALL remaining space
			// unconditionally (tried first) swung the other way - on a tall portrait screen the
			// keyboard has since sizes was the entire dead space, keys several times taller than
			// wide. This splits the difference: capped at a quarter of the keyboard's own WIDTH,
			// independent of the panel's `scale` entirely, so it can't inherit portrait's
			// squeeze - generous enough to fix "too small", nowhere near unbounded's stretch,
			// and if there's still leftover height below that (Alan's other complaint, the gap),
			// it's now bounded by width rather than open-ended, so any remaining gap should be
			// modest rather than "most of the screen" the way portrait's old cap made it.
			const int keyboardHeight = juce::jmin(area.getHeight(),
			                                       juce::roundToInt(area.getWidth() / 4.0f));
			keyboard.setBounds(area.removeFromTop(keyboardHeight));
		}
	}

private:
	// Everything that isn't Play/Stop lives behind this one button (Alan's request,
	// 2026-08-22, replacing a dedicated "Load MIDI file..." button and a dedicated "Options"
	// button - one row of controls, not two, since landscape leaves the panel/keyboard little
	// enough vertical room already). "Sequencer" and "Keyboard channel..." added the same day,
	// once a USB MIDI keyboard became a real input source and not just the on-screen one -
	// Omni/channel selection actually matters now, and so does having a sequencer view to
	// record into.
	// Shared between the app's own hamburger button and (2026-08-22) the grid sequencer's own
	// bar-navigation menu button - see D110SequencerPanel::onBarMenuButtonExtra's own comment
	// for why the sequencer needs this too (its own transport replaces the app's hamburger
	// entirely while showing, so this is the only way back to Panel view or any of the rest).
	// Every item is a self-contained action callback rather than a numeric result ID precisely
	// so it can be dropped into either menu with no ID-space coordination between them.
	void buildAppMenu(juce::PopupMenu &m) {
		// Reachable up top and unconditionally (not gated behind processor.isSynthReady()) -
		// this is exactly the thing to reach for when the ROMs are missing and everything else
		// in the app is sitting there showing the "ROMs not found" error instead of the panel.
		m.addItem("Choose ROM files...", [this] { chooseRomFolder(); });
		m.addItem("Load MIDI file...", [this] { chooseMidiFile(); });
		m.addItem(showingSequencer ? "Front Panel" : "Sequencer", [this] { toggleSequencerView(); });
		// D110Keyboard::showContextMenu() is the exact same channel/omni/PC-keyboard menu the
		// desktop keyboard's right-click shows - reached here directly instead of reimplementing
		// it, since there's no right mouse button (or safe long-press substitute - see
		// D110Keyboard.h) on a touchscreen.
		m.addItem("Keyboard channel...", [this] { keyboard.showContextMenu(); });
		m.addSeparator();

		// Drive an external hardware synth off whatever the D-110 itself is playing (Alan's
		// request, 2026-08-22) - setMidiOutputDevice() is the exact mechanism the desktop
		// Standalone build already uses for its own directly-opened MIDI Out port; every note
		// this app injects (on-screen keyboard, USB MIDI keyboard input, file/sequencer
		// playback) reaches handleIncomingMidiMessage() the same way regardless of source, and
		// that now also echoes to osMidiOut when one is set (PluginProcessor.cpp), so nothing
		// else has to change here beyond picking a device. Queried fresh every time this menu
		// opens rather than cached, so a USB device plugged in since the last time shows up
		// without needing its own periodic scan the way MIDI INPUT devices do (those need to be
		// enabled before they can deliver anything at all; output devices just need opening at
		// the moment of sending, which setMidiOutputDevice already does).
		{
			juce::PopupMenu midiOut;
			midiOut.addItem("None", true, processor.getMidiOutputId().isEmpty(),
			                 [this] { processor.setMidiOutputDevice({}); });
			for (const auto &device : juce::MidiOutput::getAvailableDevices())
				midiOut.addItem(device.name, true, processor.getMidiOutputId() == device.identifier,
				                 [this, id = device.identifier] { processor.setMidiOutputDevice(id); });
			m.addSubMenu("MIDI Output (external synth)", midiOut);
		}
		m.addSeparator();

		juce::PopupMenu options;
		options.addItem("1 Octave", true, keyboard.getNumOctaves() == 1, [this] { keyboard.setNumOctaves(1); });
		options.addItem("2 Octaves", true, keyboard.getNumOctaves() == 2, [this] { keyboard.setNumOctaves(2); });
		options.addSeparator();
		// Grid is the default (matches the desktop editor's own default) - this is the one
		// Alan asked to keep reachable first, 2026-08-22, with retro kept as a fallback rather
		// than the primary view that same request had originally put it as.
		options.addItem("Retro Sequencer (D-pad style)", true, processor.getSequencerRetroMode(),
		                 [this] { toggleSequencerRetroMode(); });
		options.addSeparator();
		// Manual escape hatch for resized()'s own nav-bar-avoidance guess (see its comment) -
		// Alan's request, 2026-08-22, after a tablet where the nav bar apparently stays
		// bottom-anchored even in landscape (rather than moving to a side the way phones do)
		// made the automatic side-guessing unreliable there. Four independent checkboxes, not
		// a single "which side" choice - a real device can need two margins at once (e.g. a
		// status bar on top AND a bottom-anchored nav bar). Flat items here rather than a
		// further-nested submenu on purpose: a 3-level-deep popup (Options -> this submenu ->
		// its items) was the one Alan reported rendering partly off-screen and needing a drag
		// to keep open - one fewer nesting level side-steps that.
		options.addSeparator();
		options.addItem("Auto margins (Recommended)", true, !navTop && !navBottom && !navLeft && !navRight,
		                 [this] { navTop = navBottom = navLeft = navRight = false; resized(); });
		options.addItem("Margin: top", true, navTop, [this] { navTop = !navTop; resized(); });
		options.addItem("Margin: bottom", true, navBottom, [this] { navBottom = !navBottom; resized(); });
		options.addItem("Margin: left", true, navLeft, [this] { navLeft = !navLeft; resized(); });
		options.addItem("Margin: right", true, navRight, [this] { navRight = !navRight; resized(); });
		m.addSubMenu("Options", options);
	}

	void showMainMenu() {
		juce::PopupMenu m;
		buildAppMenu(m);
		m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menuButton));
	}

	void toggleSequencerRetroMode() {
		processor.setSequencerRetroMode(!processor.getSequencerRetroMode());
		if (showingSequencer) resized();
	}

	void toggleSequencerView() {
		showingSequencer = !showingSequencer;
		resized();
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

	// Android's equivalent of the desktop Utility tab's "ROM FOLDER" picker (PluginEditor.cpp,
	// id 16) - but it can't work the same way desktop does (pointing setCustomRomFolder()
	// straight at whatever folder the user picks), and not only for the reason chooseMidiFile()'s
	// own comment already gives (content:// URIs aren't real filesystem paths, and
	// D110CoreNative::start() needs one). A canSelectDirectories FileChooser (tried first) hands
	// back a SAF *tree* URI, and this build's JUCE has no working way to list a tree's contents:
	// the public juce::AndroidDocumentIterator - despite its class comment - turns out to be
	// backed by plain juce::File/DirectoryIterator under the hood (see
	// AndroidDocumentIterator::makeRecursive()/makeNonRecursive() in
	// juce_AndroidDocument_android.cpp, both call dir.getUrl().getLocalFile() and iterate that),
	// which silently visits zero children for a real content:// tree (confirmed empirically on a
	// real device - a picked folder with 7 real ROM files inside came back "seen=0"). The
	// DocumentsContract-based recursive engine that WOULD walk a tree correctly
	// (AndroidDocumentDetail::RecursiveEngine) exists in that same file but is never actually
	// wired up to the public API - dead code in this JUCE version.
	//
	// So: ask for the ROM FILES themselves (multi-select), not a folder. That sidesteps tree
	// listing entirely and reuses AndroidDocument::fromDocument()+createInputStream() - the exact
	// per-file mechanism loadAndPlayMidi() already relies on successfully. Each selected file
	// gets copied into romBringUpDir() (a real path the native core can open), then
	// setCustomRomFolder() points there - same as before, and still the same folder the
	// constructor auto-detects on every future launch with no extra step.
	void chooseRomFolder() {
		fileChooser = std::make_unique<juce::FileChooser>(
			"Choose your D-110 ROM files (select all of them at once)", juce::File());
		fileChooser->launchAsync(
			juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
				| juce::FileBrowserComponent::canSelectMultipleItems,
			[this](const juce::FileChooser &fc) { copyRomFilesAndReload(fc.getURLResults()); });
	}

	void copyRomFilesAndReload(const juce::Array<juce::URL> &urls) {
		// Same whole-image/chip-dump size set PluginProcessor.cpp's materializeLooseRomsIfNeeded
		// filters loose ROM candidates by - lets the user select more than strictly needed (e.g.
		// their whole extracted romset) without copying in anything unrecognised. File CONTENT
		// (which chip a same-sized dump actually is) and Control-image splitting are sorted out
		// afterwards by tryAutoLoadRoms() itself, same as every other ROM location.
		auto isKnownRomSize = [](juce::int64 n) {
			return n == 4096 || n == 32768 || n == 131072 || n == 163840 || n == 524288 || n == 1048576;
		};

		const auto dest = romBringUpDir();
		dest.createDirectory();

		int copied = 0;
		for (const auto &url : urls) {
			auto doc = juce::AndroidDocument::fromDocument(url);
			auto in = doc.hasValue() ? doc.createInputStream() : nullptr;
			if (in == nullptr) continue;

			auto outFile = dest.getChildFile(url.getFileName());
			outFile.deleteFile();
			auto out = outFile.createOutputStream();
			if (out == nullptr || out->writeFromInputStream(*in, -1) <= 0) {
				outFile.deleteFile();
				continue;
			}
			out.reset();
			if (isKnownRomSize(outFile.getSize())) ++copied;
			else outFile.deleteFile();
		}

		if (copied == 0) {
			statusLabel.setText("None of those looked like ROM files", juce::dontSendNotification);
			return;
		}

		D110AudioProcessor::setCustomRomFolder(dest.getFullPathName());
		processor.reloadRomsAndPowerOn();
		statusLabel.setText(processor.isSynthReady() ? "Ready" : processor.getLastError(),
		                     juce::dontSendNotification);
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

	// A USB MIDI keyboard plugged in after launch needs to actually be noticed - Android's
	// android.media.midi API (what JUCE's MidiInput::getAvailableDevices() wraps here) has no
	// "device changed" callback exposed through JUCE, only a snapshot list, so this polls it.
	// Gated to once every ~2 seconds (60 ticks at this 30Hz timer) rather than every tick:
	// enumerating devices isn't free, and a keyboard being noticed a second late doesn't matter.
	// Every discovered device is enabled and given a callback straight into the same
	// injectMidiMessage() path the on-screen keyboard and MIDI-file playback already use -
	// class-compliant USB MIDI needs no pairing/permission dialog the way Bluetooth would.
	void scanForMidiInputs() {
		for (const auto &device : juce::MidiInput::getAvailableDevices()) {
			if (knownMidiInputs.contains(device.identifier)) continue;
			knownMidiInputs.add(device.identifier);
			deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
			deviceManager.addMidiInputDeviceCallback(device.identifier, this);
		}
	}

	void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &message) override {
		// Called on the MIDI thread, not the message thread - injectMidiMessage() only ever
		// queues onto osMidiCollector (MidiMessageCollector, built for exactly this: safe to
		// feed from any thread, drained later on the audio thread), so no locking of our own
		// is needed here.
		processor.injectMidiMessage(message);
	}

	void timerCallback() override {
		if (++midiScanCountdown >= 60) {
			midiScanCountdown = 0;
			scanForMidiInputs();
		}

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
	D110SequencerPanel gridSeq;
	D110SequencerRetroPanel retroSeq;
	bool showingSequencer = false;
	// Manual per-side margin overrides - see resized()'s own comment and the "Options" menu's
	// four checkboxes above. All false = Auto.
	bool navTop = false, navBottom = false, navLeft = false, navRight = false;
	juce::TextButton playButton, stopButton, menuButton;
	juce::Label statusLabel;
	std::unique_ptr<juce::FileChooser> fileChooser;

	juce::AudioDeviceManager deviceManager;
	juce::AudioProcessorPlayer player;
	juce::StringArray knownMidiInputs;
	int midiScanCountdown = 0;

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
		auto *content = new MainComponent();
		mainComponent = content;
		setContentOwned(content, true);
		setFullScreen(true);
		setVisible(true);
	}

	void closeButtonPressed() override {
		juce::JUCEApplication::getInstance()->systemRequestedQuit();
	}

	// See MainComponent::saveState()'s own comment - D110AndroidApp::suspended() is the save
	// point that actually matters on Android.
	void saveState() { mainComponent->saveState(); }

private:
	MainComponent *mainComponent = nullptr;
};

class D110AndroidApp : public juce::JUCEApplication {
public:
	const juce::String getApplicationName() override { return "d110"; }
	const juce::String getApplicationVersion() override { return "0.1"; }

	void initialise(const juce::String &) override {
		mainWindow = std::make_unique<MainWindow>(getApplicationName());
	}

	void shutdown() override {
		if (mainWindow) mainWindow->saveState();
		mainWindow = nullptr;
	}

	// The actual save point that matters on Android (see MainComponent::saveState()'s own
	// comment): called when the OS backgrounds this app, and the process can be killed with
	// no further warning at any point afterwards, well before shutdown() above would ever
	// run - a plain in-app quit is the rare case, going to the home screen or switching apps
	// is the common one, and only this callback reliably fires for that.
	void suspended() override {
		if (mainWindow) mainWindow->saveState();
	}

private:
	std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(D110AndroidApp)
