@echo off
title MeterBar Release Build (for sharing)
REM ===========================================================
REM  MeterBar - one-click RELEASE build for giving to friends.
REM  Builds Release, stages dist\MeterBar.exe, and if Inno
REM  Setup is installed, compiles dist\MeterBar-Setup.exe -
REM  the single file you send to friends. They double-click
REM  it; no compiler, no scripts, no Visual Studio for them.
REM  Inno Setup is free: https://jrsoftware.org/isdl.php
REM ===========================================================
setlocal
cd /d "%~dp0"
set "LOG=%CD%\build-release-log.txt"
echo MeterBar release build log > "%LOG%"
echo Date: %DATE% %TIME% >> "%LOG%"
echo.
echo ============================================================
echo  MeterBar RELEASE build - this window stays open when done.
echo ============================================================
echo.

REM ---- Find CMake (same proven discovery as build.bat: PATH, standalone,
REM ---- vswhere, then a manual folder scan - goto-based, no block traps) ----
set "CMAKE="
where cmake >nul 2>nul
if not errorlevel 1 set "CMAKE=cmake"
if defined CMAKE goto :have_cmake
for %%P in ("%ProgramFiles%\CMake\bin\cmake.exe" "%ProgramFiles(x86)%\CMake\bin\cmake.exe" "C:\CMake\bin\cmake.exe") do (
  if not defined CMAKE if exist %%P set "CMAKE=%%~P"
)
if defined CMAKE goto :have_cmake
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :scan
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%I"
if exist "%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if defined CMAKE goto :have_cmake
:scan
for %%Y in (2026 2022 2019) do (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined CMAKE if exist "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not defined CMAKE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
)
if defined CMAKE goto :have_cmake
echo [MeterBar] ERROR: No CMake found. Install Visual Studio 2022
echo with the "Desktop development with C++" workload first -
echo the same one the test builds use.
goto :end
:have_cmake
echo [MeterBar] Using CMake: %CMAKE%
echo Using CMake: %CMAKE% >> "%LOG%"

echo [MeterBar] Configuring - first run downloads JUCE 8, needs internet...
"%CMAKE%" -S . -B build-app -DMETERBAR_BUILD_APP=ON -A x64 >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [MeterBar] CONFIGURE FAILED. Send Milo build-release-log.txt.
  goto :end
)
echo [MeterBar] Building Release - this can take a few minutes...
"%CMAKE%" --build build-app --config Release -j >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [MeterBar] BUILD FAILED. Send Milo build-release-log.txt.
  goto :end
)

if not exist dist mkdir dist
copy /Y "build-app\app\MeterBar_artefacts\Release\MeterBar.exe" "dist\MeterBar.exe" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [MeterBar] ERROR: built exe not found where expected.
  echo Send Milo build-release-log.txt.
  goto :end
)
echo [MeterBar] Staged: dist\MeterBar.exe

REM Optional dynamic-link fallback: if a vc_redist.x64.exe sits in
REM this folder, stage it too - MeterBar.iss then bundles it and
REM installs it silently. Not needed with the static /MT build.
if exist "vc_redist.x64.exe" (
  copy /Y "vc_redist.x64.exe" "dist\vc_redist.x64.exe" >> "%LOG%" 2>&1
  echo [MeterBar] Found vc_redist.x64.exe - staged for the installer.
)

REM ---- Find Inno Setup's ISCC ----
set "ISCC="
where ISCC >nul 2>nul
if not errorlevel 1 set "ISCC=ISCC"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC (
  echo.
  echo [MeterBar] dist\MeterBar.exe is ready, but Inno Setup is not
  echo installed, so I cannot make the single-file setup yet.
  echo Install Inno Setup once (free) from https://jrsoftware.org/isdl.php
  echo then run build-release.bat again - you will get dist\MeterBar-Setup.exe.
  goto :end
)
echo [MeterBar] Compiling installer with Inno Setup...
"%ISCC%" "MeterBar.iss" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [MeterBar] INSTALLER COMPILE FAILED. Send Milo build-release-log.txt.
  goto :end
)
echo.
echo ============================================================
echo  DONE. Send this one file to your friends:
echo    dist\MeterBar-Setup.exe
echo  They double-click it - that is all. (Windows will show a
echo  blue SmartScreen prompt the first time because the app is
echo  unsigned: More info - Run anyway. Normal for free software.)
echo ============================================================

:end
echo.
echo Press any key to close this window.
pause >nul
endlocal
