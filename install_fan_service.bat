@echo off
setlocal
net session >nul 2>nul
if errorlevel 1 (
  echo This installer must be run as Administrator.
  exit /b 1
)
set "DEST=%LocalAppData%\Programs\X1 AI Island"
if not exist "%DEST%" mkdir "%DEST%"
sc stop X1FanService >nul 2>nul
copy /y "%~dp0X1FanService.exe" "%DEST%\X1FanService.exe" >nul
copy /y "%~dp0LpcACPIEC.bin" "%DEST%\LpcACPIEC.bin" >nul
sc query X1FanService >nul 2>nul
if errorlevel 1 (
  sc create X1FanService binPath= "\"%DEST%\X1FanService.exe\"" start= auto obj= LocalSystem DisplayName= "X1 Fan Telemetry Service"
) else (
  sc config X1FanService binPath= "\"%DEST%\X1FanService.exe\"" start= auto obj= LocalSystem
)
if errorlevel 1 exit /b 1
sc config X1FanService depend= PawnIO >nul
sc description X1FanService "Read-only dual-fan RPM telemetry for X1 AI Island through PawnIO." >nul
sc start X1FanService
exit /b %errorlevel%
