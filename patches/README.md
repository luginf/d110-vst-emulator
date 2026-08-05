# Патчи к MAME

Плагин собирается против **неизменённого** дерева MAME - это его свойство, и оно остаётся
верным для одного из патчей ниже, но не для остальных: `mame_mcs96_stale_irq_level.patch`
**обязателен**, см. его собственный раздел. `mame_flopimg_missing_string_view_include.patch`
тоже **обязателен**, но только при сборке под Windows на MSVC - см. его раздел.
`mame_roland_d10_dropped_writes.patch` по-прежнему опционален - ничего из него не нужно для
работы плагина. Здесь лежат правки, найденные по ходу работы над D-110 и относящиеся к самой
MAME, а не к плагину. Хранятся они тут по простой причине: дерево MAME общее у нескольких
проектов, не вендорится сюда и держит чужие незакоммиченные изменения, так что оставленная в
нём правка легко теряется.

Применять к дереву MAME 0.288:

```
cd <mame-tree>
git apply <path>/mame_mcs96_stale_irq_level.patch
git apply <path>/mame_flopimg_missing_string_view_include.patch
git apply <path>/mame_roland_d10_dropped_writes.patch
```

## `mame_flopimg_missing_string_view_include.patch` — REQUIRED on Windows/MSVC

`src/lib/formats/flopimg.h` uses `std::string_view` (`extension_matches`, declared and
defined against it) but never includes `<string_view>` - only `<memory>`, `<vector>`,
`<cassert>`, `<cstddef>`, `<cstdint>`. It still compiles on toolchains where some other
standard header happens to transitively drag `<string_view>` in, which is exactly what
masked this for years: found building MAME's `formats` project (this fork's
`.github/workflows/build-windows.yml`) against a GitHub Actions `windows-latest` runner's
MSVC/STL, which apparently doesn't. The symptom is `error C2039: 'string_view': is not a
member of 'std'` at `flopimg.h`'s own declaration, immediately ruling out anything to do
with `/std:` flags or `LanguageStandard` project settings (the generated `formats.vcxproj`
already requested `stdcpp20` in every configuration - confirmed by printing it in CI before
concluding this was a real header bug, not a build-flag one). One-line fix: add the missing
`#include <string_view>`.

Not confirmed necessary on Linux/GCC or macOS/Clang - both apparently transitively pull in
`<string_view>` some other way - so it's flagged Windows-only above rather than folded into
the always-required patch, but applying it everywhere is harmless.

## `mame_mcs96_stale_irq_level.patch` — REQUIRED

Fixes a crash: MCS-96 core's interrupt-vector fetch (`src/devices/cpu/mcs96/mcs96ops.lst`,
the `fetch` block) computes which interrupt level to take from a **fresh** scan of
`PSW & pending_irq`, but only runs that scan because a **stale** flag (`irq_requested`,
snapshotted at the end of the previous instruction) said an interrupt was pending. If
something clears the one bit that flag was based on before this fetch's own re-scan runs -
which is exactly what `D110Core.cpp`'s `midiTick()` does to work around EXTINT never
self-clearing (see `docs/la32_interface.md`, 2026-07-31) - the scan finds nothing and `level`
is left at **-1**. That's taken as a real level anyway: `1<<(-1)` into `pending_irq`, `-1`
into `OP1`, and `-1` as the index into `standard_irq_callback()`'s `m_input[]` - undefined
behaviour, reproduced as a SIGSEGV inside `device_execute_interface::device_input::
default_irq_callback()` (confirmed via `coredumpctl`/gdb backtrace against a real Carla
crash report, thread running `mcs96_device::execute_run` at the time).

The fix is a defensive guard: if the re-scan finds nothing (`level < 0`), skip taking an
interrupt this cycle instead of acting on the sentinel value. `check_irq()` re-evaluates
`irq_requested` fresh after every subsequent instruction regardless, so this cannot lose a
real interrupt - it only skips the specific cycle where the flag had already gone stale.

Same patch also fixes `fe7f mulb indexed_2b` reading its byte operand through `any_r16()` -
which masks the address to even before reading a word - instead of `any_r8()`, the only one
of the eight `indexed_2b` byte ops in the file that didn't already use it. No observed
audible or crash effect from this one today (the destination is inside the LA32 register
window this project's stub routes around, and an unmirrored RAM offset - see
`docs/la32_interface.md`'s 2026-07-31 entry, which found the same bug independently and left
it unfixed as out of scope at the time), but it's an unambiguous, one-line copy-paste bug
fixed by the same read while already in this file for the crash above.

Verified after applying: `d110_longrun_test`'s heaviest phase (chords, notes forwarded to
the firmware - the phase that generates real, sustained EXTINT traffic through
`StuckPolicy::La32Ramps`) ran a full 28-second stress window with the panel responsive
throughout and no crash, where the unpatched build reproducibly segfaulted on a real user's
machine under the same plugin build within seconds of opening the editor.

## `mame_roland_d10_dropped_writes.patch` — optional

Драйвер `roland_d10.cpp` молча теряет две группы записей, которые прошивка D-110 делает
по-настоящему.

**Защёлка SO отвечает и по `0x0280`** - тот же адрес с точностью до A7, - а карта описывает
только `0x0200`, поэтому эти записи проваливаются как неотображённые. Они не случайны:
именно они поднимают R.SW (вывод на аналоговую плату) и выбирают программу микросхемы
ревербератора. Прошивка делает их трижды при загрузке, из ПЗУ `0x1C94`, `0x1CC1` и `0x20FF`,
со значениями `2C` и `0C`; по битовой раскладке, описанной в самом `so_w`, это «программа 2,
R.SW включён», тогда как обе записи по `0x0200` - нули.

**Записи по `0x021A` объявлены `nopw()`** и выбрасываются. Между тем прошивка кладёт туда
биты 1-3 типа ревербератора: развёртка всех восьми типов плюс OFF даёт `00 02 02 04 04 06 06
08`, то есть `тип & 0x0E`. Младший бит типа уходит отдельно - в бит 2 внешней защёлки
`0x0800`. Тот же байт пишет и опрос панели, меняя в нём только бит 0.

Проверено измерением, с контролем: при тех же нажатиях и той же ноте, но без смены типа, по
обоим адресам **ни одной записи**. Подробности и разбор подпрограммы ПЗУ `0x4C7B`-`0x4CC5` -
в [`../docs/service_notes_findings.md`](../docs/service_notes_findings.md).

Поведения патч не меняет: `so_w` в MAME и сейчас только логирует, а новый обработчик просто
принимает байт - потреблять эти биты пока нечему, микросхему ревербератора не эмулирует
никто. Смысл в том, что драйвер перестаёт терять данные и в коде записано, что они значат.
Проверено после применения: прибор загружается, все восемь типов дают те же значения, что и
до патча, демо-песня играет 75 секунд с живой панелью на 100% реального времени.

**Оговорка, которая должна ехать вместе с патчем:** зеркало `0x0280` выведено из того, что
значения осмысленно раскладываются по битам `so_w`, а не из схемы - декодирование адресов
сидит внутри вентильной матрицы IC16 и на принципиальной схеме не видно. Что бит 0 в
`0x021A` - это строб столбца панели, тоже согласуется с измерением, но отдельно не
доказывалось.
