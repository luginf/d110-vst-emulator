#include "D110Keyboard.h"

#include "UiTheme.h"

namespace {
constexpr int kWhiteKeysPerOctave = 7;
constexpr int kWhiteSemitones[kWhiteKeysPerOctave] = { 0, 2, 4, 5, 7, 9, 11 };
// True for the white key immediately to the LEFT of a black key (C, D, F, G, A).
constexpr bool kHasBlackToRight[kWhiteKeysPerOctave] = { true, true, false, true, true, true, false };
} // namespace

// The classic two-row tracker layout (FastTracker2, Impulse Tracker, OpenMPT, Renoise: all
// the same table since the 90s). Lower row starts at the keyboard's current base octave,
// upper row one octave above that - the two overlap by an octave, same as every tracker.
// AZERTY's characters are what a French keyboard's PHYSICAL key at that same position
// actually sends: unshifted digits on AZERTY are punctuation (&é"'(-è_çà), not digits, so
// the accidentals' upper row differs there, and three letters move (A/Q, W/Z, M/;).
const std::vector<D110Keyboard::TrackerKey> &D110Keyboard::trackerKeys() {
	static const std::vector<TrackerKey> keys = {
		// lower row: Z S X D C V G B H N J M , L . ; /  (base octave, semitones 0..16)
		{ 0, 'z', 'w' }, { 1, 's', 's' }, { 2, 'x', 'x' }, { 3, 'd', 'd' },
		{ 4, 'c', 'c' }, { 5, 'v', 'v' }, { 6, 'g', 'g' }, { 7, 'b', 'b' },
		{ 8, 'h', 'h' }, { 9, 'n', 'n' }, { 10, 'j', 'j' }, { 11, 'm', ',' },
		{ 12, ',', ';' }, { 13, 'l', 'l' }, { 14, '.', ':' }, { 15, ';', 'm' }, { 16, '/', '!' },
		// upper row: Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P  (one octave up, semitones 12..28)
		{ 12, 'q', 'a' }, { 13, '2', (juce::juce_wchar)0x00E9 /* é */ }, { 14, 'w', 'z' },
		{ 15, '3', '"' }, { 16, 'e', 'e' }, { 17, 'r', 'r' }, { 18, '5', '(' },
		{ 19, 't', 't' }, { 20, '6', '-' }, { 21, 'y', 'y' },
		{ 22, '7', (juce::juce_wchar)0x00E8 /* è */ }, { 23, 'u', 'u' }, { 24, 'i', 'i' },
		{ 25, '9', (juce::juce_wchar)0x00E7 /* ç */ }, { 26, 'o', 'o' },
		{ 27, '0', (juce::juce_wchar)0x00E0 /* à */ }, { 28, 'p', 'p' },
	};
	return keys;
}

D110Keyboard::D110Keyboard(D110KeyboardHost &h) : host(h) {
	// Restores whatever config the project/session was last saved with - see
	// D110KeyboardHost's own accessors for why the host, not this component, is the
	// actual source of truth.
	midiChannel = host.getKeyboardMidiChannel();
	omni = host.getKeyboardOmni();
	pcKeyboardEnabled = host.getKeyboardPcInputEnabled();
	pcLayout = host.getKeyboardPcLayout() == 1 ? PcLayout::azerty : PcLayout::qwerty;

	pcKeyDown.assign(trackerKeys().size(), false);
	setWantsKeyboardFocus(true);
	rebuildKeys();

	// Just fast enough that a short note visibly lights its key (see
	// D110KeyboardHost::isNoteActive()) without repainting this small strip needlessly often -
	// remote activity (external MIDI In, sequencer playback, a DAW host's own track) is
	// asynchronous, so nothing else would otherwise trigger a repaint when it starts or stops.
	// Mouse/PC-keyboard input on this component stays instant either way - those already
	// repaint() on their own the moment the state actually changes, see setHeldNote()/
	// keyStateChanged().
	startTimerHz(30);
}

D110Keyboard::~D110Keyboard() { stopTimer(); setHeldNote(-1); releaseAllPcNotes(); }

void D110Keyboard::sendNote(int note, float velocity, bool on) {
	if (omni) {
		for (int ch = 1; ch <= 16; ++ch) host.injectTestNote(ch, note, velocity, on);
	} else {
		host.injectTestNote(midiChannel, note, velocity, on);
	}
}

void D110Keyboard::releaseAllPcNotes() {
	for (size_t i = 0; i < pcKeyDown.size(); ++i) {
		if (!pcKeyDown[i]) continue;
		pcKeyDown[i] = false;
		sendNote(kLowestNote + octaveShift * 12 + trackerKeys()[i].semitoneFromBase, 0.0f, false);
	}
}

void D110Keyboard::showContextMenu() {
	juce::PopupMenu channelMenu;
	for (int ch = 1; ch <= 16; ++ch)
		channelMenu.addItem(1000 + ch, "Channel " + juce::String(ch), true, !omni && midiChannel == ch);

	juce::PopupMenu layoutMenu;
	layoutMenu.addItem(2001, "QWERTY", true, pcLayout == PcLayout::qwerty);
	layoutMenu.addItem(2002, "AZERTY", true, pcLayout == PcLayout::azerty);

	juce::PopupMenu m;
	m.addSubMenu("MIDI Channel", channelMenu, !omni);
	m.addItem(3000, "Omni (all 16 channels at once)", true, omni);
	m.addSeparator();
	m.addItem(4000, "PC keyboard input (tracker-style)", true, pcKeyboardEnabled);
	m.addSubMenu("PC keyboard layout", layoutMenu, pcKeyboardEnabled);

	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this](int result) {
		if (result >= 1001 && result <= 1016) {
			midiChannel = result - 1000;
			host.setKeyboardMidiChannel(midiChannel);
			return;
		}
		if (result == 3000) {
			omni = !omni;
			host.setKeyboardOmni(omni);
			return;
		}
		if (result == 4000) {
			pcKeyboardEnabled = !pcKeyboardEnabled;
			host.setKeyboardPcInputEnabled(pcKeyboardEnabled);
			if (pcKeyboardEnabled) grabKeyboardFocus();
			else releaseAllPcNotes();
			return;
		}
		if (result == 2001) {
			pcLayout = PcLayout::qwerty;
			host.setKeyboardPcLayout(0);
			return;
		}
		if (result == 2002) {
			pcLayout = PcLayout::azerty;
			host.setKeyboardPcLayout(1);
			return;
		}
	});
}

bool D110Keyboard::keyStateChanged(bool /*isKeyDown*/) {
	if (!pcKeyboardEnabled) return false;
	const auto &keys = trackerKeys();
	bool used = false;
	for (size_t i = 0; i < keys.size(); ++i) {
		const juce::juce_wchar c = pcLayout == PcLayout::qwerty ? keys[i].qwerty : keys[i].azerty;
		const bool down = juce::KeyPress::isKeyCurrentlyDown((int)c);
		if (down == pcKeyDown[i]) continue;
		pcKeyDown[i] = down;
		sendNote(kLowestNote + octaveShift * 12 + keys[i].semitoneFromBase, 0.85f, down);
		used = true;
	}
	if (used) repaint(); // a tracker key just lit/unlit - see isPcKeyDownForNote()
	return used;
}

// Whether any currently-down PC-tracker key plays this exact note - two physical keys can
// map to the same note (the lower/upper rows overlap by an octave), so this is a search
// rather than a single lookup. Only ever called from paint(), over ~29 visible keys.
bool D110Keyboard::isPcKeyDownForNote(int note) const {
	const auto &keys = trackerKeys();
	for (size_t i = 0; i < keys.size(); ++i)
		if (pcKeyDown[i] && kLowestNote + octaveShift * 12 + keys[i].semitoneFromBase == note) return true;
	return false;
}

void D110Keyboard::timerCallback() { repaint(); }

void D110Keyboard::focusLost(juce::Component::FocusChangeType) { releaseAllPcNotes(); }

void D110Keyboard::changeOctave(int delta) {
	const int shifted = juce::jlimit(-2, 3, octaveShift + delta);
	if (shifted == octaveShift) return;
	if (heldNote >= 0) setHeldNote(-1);
	releaseAllPcNotes(); // held notes are note NUMBERS already sent - shifting octave first
	                     // would leave them stuck on, since the physical key never "changed"
	octaveShift = shifted;
	rebuildKeys();
	repaint();
}

// Geometry only - no drawing, no note math beyond the note NUMBERS each key represents.
// Called on resize and on octave change, both of which invalidate every rectangle.
void D110Keyboard::rebuildKeys() {
	whiteKeys.clear();
	blackKeys.clear();

	auto area = getLocalBounds().toFloat();
	// A thin caption strip above everything, full width, so "OCT n" never has to sit on top
	// of a black key to be legible.
	captionBounds = area.removeFromTop(juce::jmin(16.0f, area.getHeight() * 0.16f));
	const float buttonW = juce::jmin(36.0f, area.getWidth() * 0.06f);
	octaveDownBounds = area.removeFromLeft(buttonW);
	octaveUpBounds = area.removeFromRight(buttonW);
	keysBounds = area;

	if (keysBounds.getWidth() < 1.0f || keysBounds.getHeight() < 1.0f) return;

	constexpr int kNumWhite = kOctaves * kWhiteKeysPerOctave + 1; // trailing C
	const float whiteW = keysBounds.getWidth() / float(kNumWhite);
	const float blackW = whiteW * 0.62f;
	const float blackH = keysBounds.getHeight() * 0.6f;

	for (int i = 0; i < kNumWhite; ++i) {
		const int octaveIndex = i / kWhiteKeysPerOctave;
		const int local = i % kWhiteKeysPerOctave;
		const int note = kLowestNote + (octaveShift + octaveIndex) * 12 + kWhiteSemitones[local];
		const float x = keysBounds.getX() + i * whiteW;
		whiteKeys.push_back({ { x, keysBounds.getY(), whiteW, keysBounds.getHeight() }, note, false });

		// i < kNumWhite - 1 excludes the trailing C: it's the terminal key of the range, with
		// no white key after it to sit between - without this guard its "black key to the
		// right" landed outside keysBounds altogether, on top of the OCT+ button.
		if (i < kNumWhite - 1 && kHasBlackToRight[local]) {
			const float bx = x + whiteW - blackW * 0.5f;
			blackKeys.push_back({ { bx, keysBounds.getY(), blackW, blackH }, note + 1, true });
		}
	}
}

int D110Keyboard::keyAt(juce::Point<float> p) const {
	for (const auto &k : blackKeys)
		if (k.bounds.contains(p)) return k.note;
	for (const auto &k : whiteKeys)
		if (k.bounds.contains(p)) return k.note;
	return -1;
}

void D110Keyboard::setHeldNote(int note) {
	if (note == heldNote) return;
	if (heldNote >= 0) sendNote(heldNote, 0.0f, false);
	heldNote = note;
	if (heldNote >= 0) sendNote(heldNote, 0.85f, true);
	repaint();
}

void D110Keyboard::paint(juce::Graphics &g) {
	const auto &pal = d110ui::palette();
	g.fillAll(pal.panelBg);

	auto paintButton = [&](juce::Rectangle<float> b, const char *label) {
		g.setColour(pal.keyButtonFill);
		g.fillRect(b.reduced(3.0f));
		g.setColour(pal.keyButtonText);
		g.setFont(juce::FontOptions(juce::jlimit(12.0f, 22.0f, b.getWidth() * 0.5f)));
		g.drawText(label, b, juce::Justification::centred);
	};
	// Single glyphs, not "OCT-"/"OCT+": the button is often not much wider than one
	// character once the drawer is squeezed down to the constrainer's minimum width.
	paintButton(octaveDownBounds, "-");
	paintButton(octaveUpBounds, "+");

	// Lit for three reasons, checked cheapest-first: held by the mouse, held by a PC-tracker
	// key, or currently sounding somewhere else in the app (external MIDI In, sequencer
	// playback, a DAW host track - see D110KeyboardHost::isNoteActive()). All three just mean
	// "this note is on right now" as far as the key's colour is concerned.
	auto isLit = [this](int note) {
		return note == heldNote || isPcKeyDownForNote(note) || host.isNoteActive(note);
	};
	for (const auto &k : whiteKeys) {
		g.setColour(isLit(k.note) ? pal.keyWhiteHeld : pal.keyWhite);
		g.fillRect(k.bounds.reduced(1.0f, 0.0f));
		g.setColour(pal.keyWhiteBorder);
		g.drawRect(k.bounds, 1.0f);
	}
	for (const auto &k : blackKeys) {
		g.setColour(isLit(k.note) ? pal.keyBlackHeld : pal.keyBlack);
		g.fillRect(k.bounds);
	}

	// Always shown, not just when shifted: it's the only hint of what -/+ actually do.
	g.setColour(pal.keyCaption);
	g.setFont(juce::FontOptions(11.0f));
	g.drawText("OCT " + (octaveShift > 0 ? juce::String("+") + juce::String(octaveShift)
	                                     : juce::String(octaveShift)),
	           captionBounds, juce::Justification::centred);
}

void D110Keyboard::resized() { rebuildKeys(); }

void D110Keyboard::mouseDown(const juce::MouseEvent &e) {
	// Grabbed on every click, not only when PC input is on: it costs nothing while off, and
	// it means turning PC input on from the menu (itself a click on this component) leaves
	// typing ready to go immediately, with no extra click needed to focus the strip.
	grabKeyboardFocus();

	if (e.mods.isPopupMenu()) { showContextMenu(); return; }
	if (octaveDownBounds.contains(e.position)) { changeOctave(-1); return; }
	if (octaveUpBounds.contains(e.position)) { changeOctave(1); return; }
	const int note = keyAt(e.position);
	if (note < 0) return;
	draggingKey = true;
	setHeldNote(note);
}

void D110Keyboard::mouseDrag(const juce::MouseEvent &e) {
	if (!draggingKey) return;
	const int note = keyAt(e.position);
	setHeldNote(note); // -1 when the mouse drags off every key, which releases cleanly
}

void D110Keyboard::mouseUp(const juce::MouseEvent &) {
	if (!draggingKey) return;
	draggingKey = false;
	setHeldNote(-1);
}

void D110Keyboard::mouseExit(const juce::MouseEvent &) {
	if (!draggingKey) return;
	draggingKey = false;
	setHeldNote(-1);
}
