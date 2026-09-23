@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  echo.
  echo MSVC cl.exe was not found in this shell.
  echo Open "x64 Native Tools Command Prompt for VS 2022" and run build.bat again.
  echo Or install Visual Studio Build Tools - Desktop development with C++.
  echo.
  pause
  exit /b 1
)
rc /nologo x1_ai_island.rc
if errorlevel 1 (
  echo RESOURCE BUILD FAILED
  pause
  exit /b 1
)
cl /nologo /std:c++17 /O1 /GL /Gy /Gw /GR- /EHsc /DUNICODE /D_UNICODE x1_ai_island.cpp x1_ai_island.res /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /OUT:X1-AI-Island.exe
if errorlevel 1 (
  echo BUILD FAILED
  pause
  exit /b 1
)
cl /nologo /std:c++17 /O1 /GL /Gy /Gw /GR- /EHsc /DUNICODE /D_UNICODE x1_fan_service.cpp /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /OUT:X1FanService.exe
if errorlevel 1 (
  echo FAN SERVICE BUILD FAILED
  pause
  exit /b 1
)
echo.
echo Built successfully:
echo   %CD%\X1-AI-Island.exe
echo   %CD%\X1FanService.exe
echo.
pause
