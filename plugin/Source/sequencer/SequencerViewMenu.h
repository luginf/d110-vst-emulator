#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

// One "Sequencer > Classic / Retro / Grid" context-menu entry shared by every front end that
// hosts the sequencer drawer - the D-110 plugin/Android app, the standalone Nonet Sequencer and
// the JV-880 emulator (a byte-identical copy of this header lives in each project's
// sequencer/ folder - keep them in sync).
//
// It only needs the three-way view choice the hosts already store as two mutually exclusive
// bools: get/setSequencerRetroMode() and get/setSequencerGridMode() (both setters clear the
// other one, so apply() below can just set both). Templated on the host type so it works for
// D110AudioProcessor, NonetSeqHost and VirtualJVProcessor alike without a shared base class.
namespace seqview {

enum class View { classic = 0, retro = 1, grid = 2 };

template <class Host>
View current(const Host &host) {
	if (host.getSequencerRetroMode()) return View::retro;
	if (host.getSequencerGridMode()) return View::grid;
	return View::classic;
}

template <class Host>
void apply(Host &host, View view) {
	host.setSequencerRetroMode(view == View::retro);
	host.setSequencerGridMode(view == View::grid);
}

inline const char *label(View v) {
	switch (v) {
		case View::retro: return "Retro";
		case View::grid: return "Grid";
		default: return "Classic";
	}
}

// Adds the three view items (the current one ticked) straight into `menu`. `onPicked` runs after
// the user chooses a view - the owner calls apply() there and refreshes its own layout.
inline void addItems(juce::PopupMenu &menu, View currentView, std::function<void(View)> onPicked) {
	auto add = [&](View v, const juce::String &text) {
		menu.addItem(text, true, currentView == v, [onPicked, v] { if (onPicked) onPicked(v); });
	};
	add(View::classic, "Classic");
	add(View::retro, "Retro (D-20 style LCD + buttons)");
	add(View::grid, "Grid (piano roll)");
}

// Adds a "Sequencer" submenu holding those items to `parent`.
inline void addSubmenu(juce::PopupMenu &parent, View currentView, std::function<void(View)> onPicked,
                       const juce::String &title = "Sequencer") {
	juce::PopupMenu sub;
	addItems(sub, currentView, std::move(onPicked));
	parent.addSubMenu(title, sub);
}

} // namespace seqview
