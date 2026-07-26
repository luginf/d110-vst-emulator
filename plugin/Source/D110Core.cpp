#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1

#include "D110Core.h"

// --- MAME core ---
#include "emu.h"
#include "osd/modules/lib/osdobj_common.h"
#include "osdepend.h"
#include "emuopts.h"
#include "render.h"
#include "ioport.h"
#include "video/msm6222b.h"
#include "frontend/mame/ui/menuitem.h"
#include "frontend/mame/mame.h"
#include "frontend/mame/clifront.h"
#include "frontend/mame/mameopts.h"
#include "drivenum.h"

#include <array>
#include <chrono>
#include <cstring>

// MAME binaries embed a Common-Controls v6 manifest via their .rc; without it comctl32
// (imported by ordinal) binds to v5 and LoadLibrary fails with ERROR_INVALID_ORDINAL.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- version + emulator_info symbols the linker needs (the app must supply these) ----
// The d110 subset was built out of the MAME 0.288 tree at MU128-VST/mame-master.
extern const char bare_build_version[] = "0.288";
extern const char bare_vcs_revision[] = "0.288";
extern const char build_version[] = "0.288";

const char *emulator_info::get_appname() { return "mame"; }
const char *emulator_info::get_appname_lower() { return "mame"; }
const char *emulator_info::get_configname() { return "mame"; }
const char *emulator_info::get_copyright() { return "Copyright"; }
const char *emulator_info::get_copyright_info() { return "Copyright"; }
const char *emulator_info::get_bare_build_version() { return bare_build_version; }
const char *emulator_info::get_build_version() { return build_version; }
bool emulator_info::standalone() { return false; }
void emulator_info::periodic_check() {}
bool emulator_info::frame_hook() { return false; }
void emulator_info::sound_hook(const std::map<std::string, std::vector<std::pair<const float *, int>>> &) {}
void emulator_info::layout_script_cb(layout_file &, const char *) {}
bool emulator_info::draw_user_interface(running_machine &) { return false; }
void emulator_info::display_ui_chooser(running_machine &) {}

namespace {

// In two-line mode the MSM6222B's render buffer puts line 1 at character 0 and line 2
// at character 40, 16 bytes per character (msm6222b.h).
constexpr int kRenderLine0 = 0;
constexpr int kRenderLine1 = 40;
constexpr int kRenderStride = 16;

// Headless OSD: injects button presses, and lifts the LCD and the battery RAM out of
// the running machine once per frame.
class D110Osd : public osd_common_t {
	D110Core *core;
	running_machine *m_machine = nullptr;
	render_target *m_target = nullptr;
	msm6222b_device *m_lcd = nullptr;
	uint8_t *m_ram = nullptr;
	std::array<ioport_field *, D110Core::kNumButtons> m_buttonField{};
	bool m_resolved = false;
	std::vector<uint8_t> m_lcdScratch;

	// What is currently asserted in the machine, and how many more frames each button
	// must stay closed before it may open again. The minimum hold exists because the
	// panel is polled once per frame: a click shorter than a frame would otherwise be
	// applied and released between the same two polls and never reach the firmware.
	uint32_t m_appliedButtons = 0;
	std::array<int, D110Core::kNumButtons> m_holdFrames{};
	static constexpr int kMinHoldFrames = 3;

	// Resolve the panel ioports and the two devices we read. Retried every update()
	// until they exist: locking in a failed lookup on the first call would silently
	// leave the panel dead for the whole session.
	void resolveDevices() {
		if (m_resolved || !m_machine) return;
		device_t &root = m_machine->root_device();
		if (!root.ioport("SC0")) return;

		static const char *kTags[D110Core::kNumPorts] = {"SC0", "SC1"};
		for (int p = 0; p < D110Core::kNumPorts; ++p) {
			ioport_port *port = root.ioport(kTags[p]);
			if (!port) continue;
			for (int b = 0; b < D110Core::kNumBits; ++b) {
				const ioport_value mask = 1u << b;
				for (ioport_field &f : port->fields())
					if (f.mask() == mask) {
						m_buttonField[D110Core::buttonIndex(p, b)] = &f;
						break;
					}
			}
		}

		m_lcd = root.subdevice<msm6222b_device>("lcd");
		if (memory_share *share = root.memshare("rams"))
			if (share->bytes() >= D110Core::kRamSize)
				m_ram = static_cast<uint8_t *>(share->ptr());

		m_resolved = true;
	}

public:
	D110Osd(D110Core *c, osd_options &options) : osd_common_t(options), core(c) {}

	virtual void init(running_machine &machine) override {
		osd_common_t::init(machine);
		m_machine = &machine;
		// A render target has to exist even headless, or the machine has nothing to
		// draw into and video update asserts.
		m_target = machine.render().target_alloc();
		m_target->set_bounds(320, 240);
		m_lcdScratch.resize(D110Core::kLcdBytes, 0);
	}

	virtual void osd_exit() override {
		if (m_machine && m_target) {
			m_machine->render().target_free(m_target);
			m_target = nullptr;
		}
		osd_common_t::osd_exit();
	}

	virtual void update(bool) override {
		if (m_machine && core->shouldStop()) {
			m_machine->schedule_exit();
			return;
		}
		if (!m_machine) return;

		resolveDevices();

		// Re-assert the panel's desired state rather than replaying events. Applying a
		// state that is already correct is free, and a switch can never be stranded
		// closed by a lost or reordered release.
		const uint32_t want = core->buttonMask();
		for (int i = 0; i < D110Core::kNumButtons; ++i) {
			if (!m_buttonField[i]) continue;
			const uint32_t bit = 1u << i;
			const bool wantDown = (want & bit) != 0;
			const bool isDown = (m_appliedButtons & bit) != 0;

			if (wantDown && !isDown) {
				// The scan ports are IP_ACTIVE_LOW; set_value/clear_value handle polarity.
				m_buttonField[i]->set_value(m_buttonField[i]->mask());
				m_appliedButtons |= bit;
				m_holdFrames[i] = kMinHoldFrames;
			} else if (!wantDown && isDown) {
				if (m_holdFrames[i] > 0) { --m_holdFrames[i]; continue; }
				m_buttonField[i]->clear_value();
				m_appliedButtons &= ~bit;
			} else if (wantDown) {
				if (m_holdFrames[i] > 0) --m_holdFrames[i];
			}
		}

		if (m_lcd) {
			const uint8_t *rendered = m_lcd->render();
			if (rendered) {
				for (int line = 0; line < D110Core::kLines; ++line) {
					const int first = (line == 0) ? kRenderLine0 : kRenderLine1;
					for (int col = 0; col < D110Core::kCols; ++col) {
						const uint8_t *src = rendered + (size_t)(first + col) * kRenderStride;
						uint8_t *dst = m_lcdScratch.data()
						             + ((size_t)line * D110Core::kCols + col) * D110Core::kRowsPerChar;
						for (int row = 0; row < D110Core::kRowsPerChar; ++row)
							dst[row] = src[row] & 0x1f;
					}
				}
				core->osdSnapshotLcd(m_lcdScratch.data());
			}
		}

		if (m_ram) core->osdSnapshotRam(m_ram);
	}

	// ---- input / debugger: nothing to do headless ----
	virtual void process_events() override {}
	virtual bool has_focus() const override { return true; }
	virtual void input_update(bool) override {}
	virtual void check_osd_inputs() override {}
	virtual void init_debugger() override {}
	virtual void wait_for_debugger(device_t &, bool) override {}

	// ---- sound: the D-110 driver produces none, and we do not want any ----
	virtual bool no_sound() override { return true; }
	virtual bool sound_external_per_channel_volume() override { return false; }
	virtual bool sound_split_streams_per_source() override { return false; }
	virtual osd::audio_info sound_get_information() override {
		osd::audio_info info;
		osd::audio_info::node_info node;
		node.m_name = "null";
		node.m_display_name = "No Audio";
		node.m_id = 1;
		node.m_rate = {44100u};
		node.m_sinks = 1;
		node.m_sources = 0;
		info.m_nodes.push_back(node);
		info.m_default_sink = 1;
		info.m_generation = 1;
		return info;
	}
	virtual uint32_t sound_stream_sink_open(uint32_t, std::string, uint32_t) override { return 1; }
	virtual void sound_stream_close(uint32_t) override {}
	virtual void add_audio_to_recording(const int16_t *, int) override {}
	virtual uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
	virtual uint32_t sound_get_generation() override { return 1; }
	virtual void sound_stream_source_update(uint32_t, int16_t *, int) override {}
	virtual void sound_stream_set_volumes(uint32_t, const std::vector<float> &) override {}
	virtual void sound_begin_update() override {}
	virtual void sound_end_update() override {}
	virtual void sound_stream_sink_update(uint32_t, const int16_t *, int) override {}

	// ---- UI neutered ----
	virtual void customize_input_type_list(std::vector<input_type_entry> &t) override { t.clear(); }
	virtual std::vector<ui::menu_item> get_slider_list() override { return {}; }
	virtual osd_font::ptr font_alloc() override { return nullptr; }
	virtual bool get_font_families(std::string const &,
	                               std::vector<std::pair<std::string, std::string>> &) override { return false; }
	virtual bool execute_command(const char *) override { return false; }
	virtual void set_verbose(bool) override {}

	// ---- MIDI: the D-110 driver mounts no MIDI image slots, so nothing asks for a
	// port. Feeding host MIDI into the firmware (so its display tracks Program
	// Changes from the DAW) needs a small driver patch and comes later.
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view) override { return {}; }
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view) override { return {}; }
	virtual std::vector<osd::midi_port_info> list_midi_ports() override { return {}; }

	virtual std::unique_ptr<osd::network_device> open_network_device(int, osd::network_handler &) override { return {}; }
	virtual std::vector<osd::network_device_info> list_network_devices() override { return {}; }
};

} // namespace

// ---------------------------------------------------------------------------

// The blocks of the firmware's RAM that are mirrored into the LA engine, with the
// Roland exclusive address each one lives at. Both bases below were measured, not
// assumed - `plugin/bridge_probe.cpp` walks the firmware's menus and diffs the RAM,
// and the per-part block agreed on three consecutive bytes in the order Roland
// documents (timbreNumber, keyShift, fineTune). See docs/sysex_address_map.md.
//
// Mirroring a whole region rather than individual parameters is deliberate: it means
// nothing here has to know what any given byte means, so every parameter inside the
// block comes across for free.
//
// The Tone Temporary Area is split into one region PER PART rather than one big block,
// for two reasons: a Roland DT1 is limited to 256 bytes and a tone is 246, and sending
// only the part that actually changed keeps the engine from re-caching all eight tones
// on every edit.
//
// Its base and stride were measured, not assumed: changing the timbre number makes the
// firmware reload a whole tone, and doing that on part 1 then part 2 gave changed spans
// ending at 0x22D9 and 0x23CE - 245 apart, i.e. a 246-byte stride, putting part 1's
// tone at 0x22D9-245 = 0x21E4. That also matches the arithmetic (0x2000 + 144 timbre
// temp + 340 rhythm setup) and the manual's own part 7 and part 8 addresses.
#define D110_TONE(part) \
	{ uint16_t(0x21E4 + (part) * 246), \
	  0x040000u | (uint32_t(((part) * 246) / 128) << 8) | uint32_t(((part) * 246) % 128), \
	  246, "Tone Temporary" }

const D110Core::MirrorRegion D110Core::kMirrorRegions[] = {
	// 9 parts (8 voice + rhythm) of 16 bytes; SysEx 0x030000, part 2 at 0x030010,
	// rhythm at 0x030100. 144 bytes fits one DT1, so it goes as a single block.
	{ 0x2000, 0x030000, 9 * 16, "Timbre Temporary" },

	// The eight Tone Temporary regions belong here and the SysEx side of them is right
	// (the generated addresses reproduce the manual's own part 7 and part 8 entries,
	// 0x040B44 and 0x040D3A). They are OUT until the RAM base is positively identified.
	//
	// 0x21E4 was wrong to trust: the span measurement only ever gave a LOWER BOUND for
	// it, because the last bytes of a tone need not differ between two tones. Mirroring
	// from there wrote shifted data, and the test caught it - lowering Fine Tune raised
	// the pitch by 460 cents instead of lowering it slightly. A wrong base here is far
	// worse than no mirroring at all, because it corrupts a sound that was correct.
	//
	// The base must be found positively, not by inference: a tone begins with its
	// 10-character ASCII name, so searching RAM for the name the display is showing
	// pins it exactly.
	// D110_TONE(0), ... D110_TONE(7),
};
#undef D110_TONE
constexpr int D110Core::kNumMirrorRegions =
	int(sizeof(D110Core::kMirrorRegions) / sizeof(D110Core::kMirrorRegions[0]));

D110Core::D110Core()
	: lcd(kLcdBytes, 0), ram(kRamSize, 0),
	  sysexBuf((size_t)kSysexSlots * kMaxSysexBytes, 0), sysexLen(kSysexSlots, 0) {
	mirrorPrev.resize(kNumMirrorRegions);
	for (int i = 0; i < kNumMirrorRegions; ++i)
		mirrorPrev[(size_t)i].assign(kMirrorRegions[i].length, 0);
}

D110Core::~D110Core() {
	stop();
	if (resetThread.joinable()) resetThread.join();
}

void D110Core::start(const std::string &rom, const std::string &nvram) {
	if (running.load(std::memory_order_acquire)) return;
	romPath = rom;
	nvramDir = nvram;
	stopFlag.store(false, std::memory_order_release);
	mameThread = std::thread(&D110Core::threadFunc, this);
}

void D110Core::stop() {
	// Open every switch before the machine goes away, so nothing can be left asserted
	// if the panel is still showing a cap as held.
	releaseAllButtons();
	stopFlag.store(true, std::memory_order_release);
	if (mameThread.joinable()) mameThread.join();
	running.store(false, std::memory_order_release);
}

void D110Core::threadFunc() {
	running.store(true, std::memory_order_release);

	// No -nothrottle: with no audio ring to apply back-pressure, MAME's own throttle
	// is what keeps the firmware running at real speed, which is what makes key
	// repeat and display timing feel like the hardware.
	std::vector<std::string> args = {
		"d110", "d110",
		"-rompath", romPath,
		"-nvram_directory", nvramDir,
		"-video", "none",
		"-sound", "none",
		"-noreadconfig", "-skip_gameinfo",
		"-keyboardprovider", "none",
		"-mouseprovider", "none",
		"-joystickprovider", "none",
	};

	auto *opts = new osd_options();
	auto *osd = new D110Osd(this, *opts);
	auto *fe = new cli_frontend(*opts, *osd);
	fe->execute(args);
	delete fe;
	delete osd;
	delete opts;

	running.store(false, std::memory_order_release);
}

// The documented cold start. Write/Copy is held from the moment the machine comes up
// rather than pressed afterwards, which the state-based button model makes easy: the
// switch is simply asserted before the machine starts, and the OSD applies it as soon
// as the ioports resolve - exactly like holding the real button while switching on.
void D110Core::factoryReset() {
	if (resetting.exchange(true, std::memory_order_acq_rel)) return; // already running
	if (resetThread.joinable()) resetThread.join();

	resetThread = std::thread([this] {
		const std::string rom = romPath, nv = nvramDir;
		if (isRunning()) stop();

		releaseAllButtons();
		setButton(buttonIndex(0, 0), true); // Write/Copy, held across the reset
		start(rom, nv);

		std::this_thread::sleep_for(std::chrono::seconds(5)); // let the firmware come up
		setButton(buttonIndex(0, 0), false);
		std::this_thread::sleep_for(std::chrono::milliseconds(800));

		setButton(buttonIndex(1, 0), true); // Enter, to confirm
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		setButton(buttonIndex(1, 0), false);

		// Give the firmware time to rewrite its patch and timbre memory before anything
		// else touches it.
		std::this_thread::sleep_for(std::chrono::seconds(4));
		resetting.store(false, std::memory_order_release);
	});
}

// ---- buttons --------------------------------------------------------------

void D110Core::setButton(int index, bool down) {
	if (index < 0 || index >= kNumButtons) return;
	const uint32_t bit = 1u << index;
	uint32_t cur = wantButtons.load(std::memory_order_relaxed);
	uint32_t next;
	do {
		next = down ? (cur | bit) : (cur & ~bit);
	} while (!wantButtons.compare_exchange_weak(cur, next, std::memory_order_release,
	                                            std::memory_order_relaxed));
}

// ---- LCD ------------------------------------------------------------------

void D110Core::osdSnapshotLcd(const uint8_t *renderBuf) {
	std::lock_guard<std::mutex> lock(lcdMutex);
	std::memcpy(lcd.data(), renderBuf, kLcdBytes);
	lcdValid.store(true, std::memory_order_release);
}

bool D110Core::getLcd(uint8_t *out) const {
	if (!lcdValid.load(std::memory_order_acquire)) return false;
	std::lock_guard<std::mutex> lock(lcdMutex);
	std::memcpy(out, lcd.data(), kLcdBytes);
	return true;
}

// ---- battery RAM ----------------------------------------------------------

void D110Core::osdSnapshotRam(const uint8_t *src) {
	// Only the mirrored regions decide whether anything is sent onward. Diffing the
	// whole 32 KB would fire dozens of times a second on the firmware's scratch area,
	// which has nothing to do with the sound.
	for (int i = 0; i < kNumMirrorRegions; ++i) {
		const auto &region = kMirrorRegions[i];
		auto &prev = mirrorPrev[(size_t)i];
		const uint8_t *now = src + region.ramOffset;
		if (mirrorPrimed && std::memcmp(prev.data(), now, region.length) == 0)
			continue;
		std::memcpy(prev.data(), now, region.length);
		// The very first snapshot only establishes the baseline: the engine already
		// holds whatever the ROMs booted with, and blasting it at startup would fight
		// the host's own program changes.
		if (mirrorPrimed)
			emitRegionSysex(region, src);
	}
	mirrorPrimed = true;

	std::lock_guard<std::mutex> lock(ramMutex);
	if (ramValid.load(std::memory_order_relaxed) && std::memcmp(ram.data(), src, kRamSize) == 0)
		return; // unchanged - leave the generation alone so callers can skip work
	std::memcpy(ram.data(), src, kRamSize);
	ramValid.store(true, std::memory_order_release);
	ramGen.fetch_add(1, std::memory_order_release);
}

// Builds the Roland "Data set 1" message the hardware itself would send for this
// block, and queues it. Format per the D-110 owner's manual and accepted verbatim by
// mt32emu's Synth::playSysexWithoutFraming: F0 41 <dev> 16 12 <a1 a2 a3> <data> <sum> F7,
// manufacturer 0x41 Roland, model 0x16, command 0x12 DT1, device ID 0x10 (the default
// unit). The checksum makes address plus data plus checksum a multiple of 128.
void D110Core::emitRegionSysex(const MirrorRegion &region, const uint8_t *ramImage) {
	uint8_t msg[kMaxSysexBytes];
	int n = 0;
	msg[n++] = 0xF0;
	msg[n++] = 0x41; // Roland
	msg[n++] = 0x10; // device ID
	msg[n++] = 0x16; // model: MT-32 family, which is what the D-110 answers to
	msg[n++] = 0x12; // DT1

	// A Roland address is written as three SEPARATE seven-bit bytes - 0x030000 means the
	// bytes 03 00 00 - so it splits on byte boundaries, not on seven-bit ones. Packing it
	// as a 21-bit number instead sends 0x030000 as 0C 00 00, which mt32emu rejects with
	// "Sysex write to unrecognised address 0c0000". That was the whole reason the first
	// version of this bridge fired correctly and still changed nothing.
	const uint32_t addr = region.sysexAddress;
	const uint8_t a1 = uint8_t((addr >> 16) & 0x7f);
	const uint8_t a2 = uint8_t((addr >> 8) & 0x7f);
	const uint8_t a3 = uint8_t(addr & 0x7f);
	msg[n++] = a1;
	msg[n++] = a2;
	msg[n++] = a3;

	uint32_t sum = a1 + a2 + a3;
	const int payload = std::min<int>(region.length, kMaxSysexBytes - n - 2);
	for (int i = 0; i < payload; ++i) {
		// Data bytes are seven bits on the wire; the firmware's own values already fit,
		// but mask defensively so a stray high bit cannot break the framing.
		const uint8_t v = ramImage[region.ramOffset + i] & 0x7f;
		msg[n++] = v;
		sum += v;
	}
	msg[n++] = uint8_t((128 - (sum & 0x7f)) & 0x7f);
	msg[n++] = 0xF7;

	pushSysex(msg, n);
}

void D110Core::pushSysex(const uint8_t *msg, int len) {
	if (len <= 0 || len > kMaxSysexBytes) return;
	const int w = sW.load(std::memory_order_relaxed);
	const int r = sR.load(std::memory_order_acquire);
	if (((w + 1) & kSysexMask) == r) return; // full: drop rather than stall the machine
	std::memcpy(&sysexBuf[(size_t)w * kMaxSysexBytes], msg, (size_t)len);
	sysexLen[(size_t)w] = uint16_t(len);
	sW.store((w + 1) & kSysexMask, std::memory_order_release);
	sysexCount.fetch_add(1, std::memory_order_release);
}

int D110Core::popSysex(uint8_t *out) {
	const int r = sR.load(std::memory_order_relaxed);
	if (r == sW.load(std::memory_order_acquire)) return 0;
	const int len = sysexLen[(size_t)r];
	std::memcpy(out, &sysexBuf[(size_t)r * kMaxSysexBytes], (size_t)len);
	sR.store((r + 1) & kSysexMask, std::memory_order_release);
	return len;
}

bool D110Core::getRam(uint8_t *out) const {
	if (!ramValid.load(std::memory_order_acquire)) return false;
	std::lock_guard<std::mutex> lock(ramMutex);
	std::memcpy(out, ram.data(), kRamSize);
	return true;
}
