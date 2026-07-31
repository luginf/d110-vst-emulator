@echo off
REM Rebuild the d110 subset of the shared MAME tree into static libs.
REM
REM The tree is SHARED with the D-70, MU-128 and MU-100R projects
REM (C:\Users\bd260\Projects\MU128-VST\mame-master), so a CPU-core fix made in
REM one project only reaches this one after this script is run.
REM
REM The regeneration step below is not optional bookkeeping: editing
REM src/devices/cpu/mcs96/mcs96ops.lst regenerates only mcs96.hxx and leaves
REM i8x9x.hxx / i8xc196.hxx stale, which silently builds the OLD opcode
REM behaviour and looks exactly like "the fix did not work". It is a no-op when
REM nothing changed.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files\Python312;%PATH%;C:\msys64\mingw64\bin;C:\msys64\usr\bin
cd /d C:\Users\bd260\Projects\MU128-VST\mame-master

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$lst = 'C:\Users\bd260\Projects\MU128-VST\mame-master\src\devices\cpu\mcs96\mcs96ops.lst';" ^
  "$gen = 'C:\Users\bd260\Projects\MU128-VST\mame-master\build\generated\emu\cpu\mcs96';" ^
  "if ((Test-Path $lst) -and (Test-Path $gen)) {" ^
  "  $t = (Get-Item $lst).LastWriteTime;" ^
  "  $stale = @(Get-ChildItem -Path $gen -Filter *.hxx | Where-Object { $_.LastWriteTime -lt $t });" ^
  "  if ($stale.Count -gt 0) {" ^
  "    $stale | Remove-Item -Force;" ^
  "    Write-Host ('MCS96_TABLES=regenerating (' + $stale.Count + ' stale)') }" ^
  "  else { Write-Host 'MCS96_TABLES=current' } }" ^
  "else { Write-Host 'MCS96_TABLES=nothing-to-check' }"

if not exist "%~dp0..\build_logs" mkdir "%~dp0..\build_logs"
make vs2022 MSBUILD=1 PTR64=1 MINGW64=C:/msys64/mingw64 MINGW32=C:/msys64/mingw32 NOWERROR=1 PYTHON_EXECUTABLE=python SUBTARGET=d110 SOURCES=src/mame/roland/roland_d10.cpp USE_BGFX=0 > "%~dp0..\build_logs\mame_rebuild.log" 2>&1
echo MAME_EXIT=%ERRORLEVEL%

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$log = '%~dp0..\build_logs\mame_rebuild.log';" ^
  "$err = @(Select-String -Path $log -Pattern 'error C|error LNK|error MSB');" ^
  "if ($err.Count -gt 0) { Write-Host ('BUILD_ERRORS=' + $err.Count) ; $err | Select-Object -First 10 | ForEach-Object { Write-Host ('  ' + $_.Line.Trim()) } }" ^
  "else { Write-Host 'BUILD_ERRORS=0' }"
