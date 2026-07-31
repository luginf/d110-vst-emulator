@echo off
REM Rebuild and install the D-110 Emulator plugin (VST3 + Standalone) plus the
REM console test harnesses used to verify it.
REM
REM Run scripts\build_mame.bat FIRST if anything in the shared MAME tree changed
REM - this script only relinks against whatever libs are already there.
REM
REM The install check at the end exists because the VST3 post-build copy into
REM "C:\Program Files\Common Files\VST3" fails silently while a DAW holds the
REM DLL open, leaving an old plugin installed while the build reports success.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files\Python312;%PATH%;C:\msys64\mingw64\bin;C:\msys64\usr\bin
cd /d C:\Users\bd260\Projects\D110-VST\plugin\build
if not exist "%~dp0..\build_logs" mkdir "%~dp0..\build_logs"

cmake --build . --config Release --target D110Emulator_VST3 > "%~dp0..\build_logs\plugin_vst3.log" 2>&1
echo VST3_EXIT=%ERRORLEVEL%
cmake --build . --config Release --target D110Emulator_Standalone > "%~dp0..\build_logs\plugin_standalone.log" 2>&1
echo STANDALONE_EXIT=%ERRORLEVEL%
REM EVERY harness, not a chosen few. Building only some of them once produced a
REM "regression check passed" from a binary compiled before the change under test -
REM a fatal boot bug (install_write_tap on an unaligned range) shipped that way,
REM because the test that would have caught it was stale. A test you did not
REM rebuild is not evidence.
cmake --build . --config Release --target d110_core_test d110_longrun_test ^
    d110_la32_realistic d110_demo_song_repro d110_demo_wav d110_hang_probe ^
    d110_la32_ctx d110_la32_lifecycle d110_note_source d110_pan_verify ^
    d110_audio_test d110_coldstart_test d110_state_test d110_sysex_test ^
    d110_part_audio d110_solo_level d110_part_state d110_tone_clobber ^
    d110_bridge_probe d110_tone_probe d110_la32_probe d110_two_instance_test ^
    d110_rhythm_map d110_reverb_path d110_so_trace ^
    > "%~dp0..\build_logs\tests.log" 2>&1
echo TESTS_EXIT=%ERRORLEVEL%

REM The boot test is cheap and catches the whole class of "the machine will not
REM start at all" faults, which no amount of later checking can work around.
"%~dp0..\plugin\build\Release\d110_core_test.exe" > "%~dp0..\build_logs\boot_check.log" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$log = Get-Content '%~dp0..\build_logs\boot_check.log' -Raw;" ^
  "if ($log -match 'running: yes') { Write-Host 'BOOT=ok' }" ^
  "elseif ($log -match 'Fatal error: (.+)') { Write-Host ('BOOT=FAILED - ' + $Matches[1]) }" ^
  "else { Write-Host 'BOOT=FAILED (see build_logs\boot_check.log)' }"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "foreach ($l in @('plugin_vst3','plugin_standalone','tests')) {" ^
  "  $p = Join-Path '%~dp0..\build_logs' ($l + '.log');" ^
  "  if (Test-Path $p) { $e = @(Select-String -Path $p -Pattern 'error C|error LNK|error MSB');" ^
  "    if ($e.Count -gt 0) { Write-Host ($l + '=' + $e.Count + ' ERRORS'); $e | Select-Object -First 6 | ForEach-Object { Write-Host ('  ' + $_.Line.Trim()) } }" ^
  "    else { Write-Host ($l + '=ok') } } }"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$built = 'C:\Users\bd260\Projects\D110-VST\plugin\build\D110Emulator_artefacts\Release\VST3\D-110 Emulator.vst3\Contents\x86_64-win\D-110 Emulator.vst3';" ^
  "$inst  = 'C:\Program Files\Common Files\VST3\D-110 Emulator.vst3\Contents\x86_64-win\D-110 Emulator.vst3';" ^
  "if (-not (Test-Path $built)) { Write-Host 'INSTALLED=unknown (built bundle not found)'; exit }" ^
  "if (-not (Test-Path $inst))  { Write-Host 'INSTALLED=MISSING'; exit }" ^
  "$b = Get-Item $built; $i = Get-Item $inst;" ^
  "$dt = [Math]::Abs(($b.LastWriteTime - $i.LastWriteTime).TotalSeconds);" ^
  "if ($b.Length -eq $i.Length -and $dt -lt 120) { Write-Host ('INSTALLED=fresh (' + $i.LastWriteTime + ')') }" ^
  "else { Write-Host ('INSTALLED=STALE - built ' + $b.LastWriteTime + ' but installed ' + $i.LastWriteTime + ' -- close the DAW and run this script again') }"
