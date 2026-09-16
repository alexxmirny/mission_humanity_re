@echo off
rem Build BOTH offline selftest executables (fork F5I S2).
rem
rem Since refactor Phase 5 mh_nettest is a real project in mh.sln, so
rem this bat is a THIN WRAPPER around msbuild -- the source list lives in the vcxproj files ONLY
rem (it used to be duplicated here and needed updating every time a seam TU was added). The
rem historical run contract is preserved: the exes land in [outdir] (default %TEMP%\mh_nettest).
rem
rem TWO PROJECTS, ONE BAT, TWO MSBUILD INVOCATIONS:
rem   mh_nettest\mh_nettest.vcxproj    -> net_selftest.exe     the HOSTED arm (no MH_LIBMH_BUILD):
rem                                       mh.dll's own machinery -- transport, marshalling thunks,
rem                                       the patch and tombstone instruments, the hook table.
rem   libmh_test\libmh_test.vcxproj    -> libmh_selftest.exe   the STANDALONE arm (MH_LIBMH_BUILD +
rem                                       MH_SPINE_IN_IMAGE): the same 628 roster TUs compiled as
rem                                       the whole program, running the suites about libmh itself.
rem Which suite runs on which is tools/data/selftest_roster.json's `exe` column. BOTH are staged
rem into the SAME outdir, because that directory is the run contract tools/run_selftests.py checks
rem and a second directory would only be a second thing to keep in step.
rem
rem A FAILED BUILD OF EITHER IS A FAILED BUILD. Staging whichever exe compiled would leave the
rem driver running half the roster and reporting green, which is the failure the roster assertion
rem exists to make impossible.
rem
rem Usage:  build_selftest.bat  [outdir]  [--asan]
rem Then:   net_selftest.exe selftest       (2-node PING/PONG round-trip)
rem         net_selftest.exe selftest3      (3-node broadcast relay fan-out)
rem         libmh_selftest.exe --list-suites (what the standalone arm answers to)
rem NODE REUSE IS DISABLED ON PURPOSE -- do not "restore" it for build speed. With `/m` and default
rem node reuse, MSBuild's worker nodes outlive the build by ~15 minutes AND inherit whatever stdout
rem handle this bat was started with. A caller using subprocess.run(capture_output=True) then reads
rem the pipe waiting for EOF that never comes: the build looks like it hangs at 0% CPU with an empty
rem log, the caller gets killed, and the cmd/MSBuild/net_selftest tree survives it because nothing
rem puts them in a job object. That is how the 2026-08-02 loop accumulated several stray trees.
rem
rem ---- --asan ------------------------------------------------------------------------------------
rem Builds the same sources with /fsanitize=address into a SEPARATE output dir, so the ASan exe can
rem never be mistaken for the one the gate times or the rig runs.
rem
rem WHY IT IS WORTH HAVING. `aitest` is the primary oracle for ~215 reimplemented AI functions, and
rem an out-of-bounds read/write is exactly what a translation with a wrong extent produces. (Since
rem F5I S2 that suite is libmh_selftest.exe's, so the worked example below is now a crash trace from
rem the OTHER exe this bat builds -- the bug, the tool and the argument are unchanged.) Without
rem ASan that surfaces as DELAYED, NON-LOCAL, INTERMITTENT heap damage: on 2026-08-03 a one-element
rem `std::vector<uint32_t> ring_counts{128}` (initializer_list ctor, not a size) made aitest die
rem 0xC0000374 on ~60% of runs, with a stack pointing at `recorder::clear` freeing a
rem `std::vector<double>` that does not appear anywhere in the source. Rebuilt with --asan and the
rem bug reintroduced, the SAME run reports, first time and deterministically:
rem
rem   ERROR: AddressSanitizer: heap-buffer-overflow ... WRITE of size 4
rem     #0 in `test_plan_turret_upgrade'::`2'::<lambda_1>::operator()  ai_selftest.cpp:5683
rem
rem Neither clang-tidy (bugprone/clang-analyzer/cppcoreguidelines/modernize/readability) nor MSVC
rem /analyze flags that line -- both were run against it and both came back clean, because the bug is
rem legal overload resolution, not a suspicious subscript. ASan is the tool that sees it.
rem
rem NOT for every iteration: it is slower and rebuilds from scratch (LTCG is incompatible, MSBuild
rem says so and proceeds). Reach for it when a selftest CRASHES rather than fails, when adding a
rem fixture buffer, and periodically over the AI suite.
rem
rem The ASan runtime DLL is copied next to the exe on purpose. Without it on PATH the process dies
rem with a bare 0xC0000135 (DLL not found) and no message at all, which reads exactly like a crash in
rem the test -- a trap worth spending three lines of batch to remove.
rem
rem THE TWO MODES NO LONGER SHARE A STAGING PATH (2026-08-23). ASan builds into ..\Release_asan\ and
rem plain into ..\Release\, with separate IntDirs, so neither can be mistaken for the other AND
rem neither forces a full rebuild of the other -- which is what made every gate run pay two full
rem 669-TU passes. Both projects follow the same rule and share those two output directories while
rem keeping their own IntDirs. Still run the copy in the outdir this script reports rather than
rem reading ..\Release\ directly: that is the contract run_selftests.py checks.
setlocal enabledelayedexpansion
set HERE=%~dp0
rem Resolve the VS/BuildTools install via vswhere (portable across machines); fall back to this box's
rem path only if vswhere is absent. Any VS 2022+ edition or Build Tools with the v143 x86 toolset works.
set VSDIR=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set VSDIR=%%i
if "%VSDIR%"=="" set VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
set OUT=
set ASAN=0
for %%A in (%*) do (
  if /i "%%~A"=="--asan" (set ASAN=1) else (if "!OUT!"=="" set OUT=%%~A)
)
if "%ASAN%"=="1" (
  if "%OUT%"=="" set OUT=%TEMP%\mh_nettest_asan
) else (
  if "%OUT%"=="" set OUT=%TEMP%\mh_nettest
)
if not exist "%OUT%" mkdir "%OUT%"
set MSBUILDDISABLENODEREUSE=1

set EXTRA=
if "%ASAN%"=="1" set EXTRA=/p:EnableASAN=true /p:AdditionalOptions="/Zi"

"%VSDIR%\MSBuild\Current\Bin\MSBuild.exe" "%HERE%mh_nettest.vcxproj" /p:Configuration=Release /p:Platform=Win32 %EXTRA% /m /nodeReuse:false /v:minimal /nologo
if errorlevel 1 (
  echo BUILD FAILED ^(mh_nettest^)
  exit /b 1
)
"%VSDIR%\MSBuild\Current\Bin\MSBuild.exe" "%HERE%..\libmh_test\libmh_test.vcxproj" /p:Configuration=Release /p:Platform=Win32 %EXTRA% /m /nodeReuse:false /v:minimal /nologo
if errorlevel 1 (
  echo BUILD FAILED ^(libmh_test^)
  exit /b 1
)
rem Each mode builds into its OWN ..\Release[_asan]\ (both vcxproj split OutDir/IntDir
rem on EnableASAN), so the two no longer invalidate each other's objects -- an unchanged
rem rebuild is ~1 s instead of a full 669-TU pass -- and neither can be mistaken for the other.
set STAGEDIR=%HERE%..\Release
if "%ASAN%"=="1" set STAGEDIR=%HERE%..\Release_asan
copy /y "%STAGEDIR%\net_selftest.exe" "%OUT%\" >nul
if errorlevel 1 (
  echo STAGING FAILED: %STAGEDIR%\net_selftest.exe
  exit /b 1
)
copy /y "%STAGEDIR%\libmh_selftest.exe" "%OUT%\" >nul
if errorlevel 1 (
  echo STAGING FAILED: %STAGEDIR%\libmh_selftest.exe
  exit /b 1
)

if "%ASAN%"=="1" (
  rem Newest toolset wins: /o:n sorts ascending, so the last match is the highest version. Resolved
  rem at build time rather than pinned, because the version in the path changes on every VS update.
  rem NOTE the search root: `dir /s` recurses looking for a FILENAME and does not accept wildcards in
  rem intermediate directories, so "...\MSVC\*\bin\Hostx86\x86\clang_rt..." silently finds nothing.
  rem Recurse from MSVC\ and filter the host dir afterwards instead.
  set ASANRT=
  for /f "delims=" %%D in ('dir /b /s /o:n "!VSDIR!\VC\Tools\MSVC\clang_rt.asan_dynamic-i386.dll" 2^>nul ^| findstr /i "Hostx86"') do set ASANRT=%%D
  if "!ASANRT!"=="" (
    echo WARNING: clang_rt.asan_dynamic-i386.dll not found under !VSDIR!\VC\Tools\MSVC
    echo          the exe will die with 0xC0000135 ^(DLL not found^), which looks like a crash in the test
  ) else (
    copy /y "!ASANRT!" "%OUT%\" >nul
    echo asan runtime: !ASANRT!
  )
)
echo built: %OUT%\net_selftest.exe and %OUT%\libmh_selftest.exe
endlocal
