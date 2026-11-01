@echo off
title MeterBar Live Captions - Runtime Setup
REM ===========================================================
REM  One-time, EXPLICIT setup for MeterBar Live Captions.
REM  Creates a dedicated Python venv and downloads the two
REM  Whisper models (small + base) under
REM  %LOCALAPPDATA%\MeterBar\Captions. Ordinary caption use
REM  never downloads anything - this window is the only place
REM  the network is touched. Safe to run again.
REM ===========================================================
setlocal
set "CAPDIR=%LOCALAPPDATA%\MeterBar\Captions"
set "VENV=%CAPDIR%\.venv"
set "MODELS=%CAPDIR%\models"
echo MeterBar Live Captions runtime setup
echo Target: %CAPDIR%
echo.

REM ---- Find a Python 3.10+ (py launcher first, then python) ----
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
  echo ERROR: No Python found. Install Python 3.10+ from python.org
  echo        (tick "Add python.exe to PATH"), then run this again.
  goto :end
)

if not exist "%VENV%\Scripts\python.exe" (
  echo Creating venv...
  %PY% -m venv "%VENV%"
  if errorlevel 1 (
    echo ERROR: venv creation failed.
    goto :end
  )
)
set "VPY=%VENV%\Scripts\python.exe"

echo Installing packages (faster-whisper, PyAV, CUDA wheels)...
"%VPY%" -m pip install --upgrade pip
if errorlevel 1 goto :pipfail
"%VPY%" -m pip install -r "%~dp0requirements.txt"
if errorlevel 1 goto :pipfail

echo Downloading models (small + base, one-time)...
"%VPY%" "%~dp0download_models.py" "%MODELS%"
if errorlevel 1 (
  echo ERROR: model download failed - check the connection and retry.
  goto :end
)

echo.
echo ===========================================================
echo  DONE. Live Captions is ready: in MeterBar, open the Menu
echo  and choose Live Captions - Captions (or Translate).
echo ===========================================================
goto :end

:pipfail
echo ERROR: package install failed - check the connection and retry.

:end
echo.
pause
endlocal
