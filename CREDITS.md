# Credits

This project is based in part on the work of the Munt project and
contributors, as well as other open-source projects. We sincerely thank all
developers whose work made this project possible.

In particular:

- **Dean Beeler, Jerome Fisher, and Sergey V. Mikayev**, and the rest of the
  [Munt](https://github.com/munt/munt) project team - for `mt32emu`, the
  LA-synthesis emulation engine this plugin wraps and is built directly on
  top of.
- **davidhsilaban** - for the D-110-specific fork of munt this project
  vendors and builds against.
- **The MAME team**, and in particular **Olivier Galibert** and
  **Jonathan Gevaryahu** - for `roland_d10.cpp` and the `msm6222b` LCD
  controller. Their driver is what lets this plugin run the D-110's real
  Roland firmware, so that the menus, the display and all sixteen
  front-panel buttons are the hardware's own rather than a reimplementation.
- **The JUCE / Raw Material Software team**, for the JUCE framework this
  plugin is built on.

See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for full license
details of each component.
