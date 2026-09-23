@echo off
setlocal
net session >nul 2>nul
if errorlevel 1 (
  echo This installer must be run as Administrator.
  exit /b 1
)
set "DEST=%LocalAppData%\Programs\X1 AI Island"
if not exist "%DEST%" mkdir "%DEST%"
if errorlevel 1 exit /b 1

sc query X1FanService >nul 2>nul
if not errorlevel 1 (
  sc stop X1FanService >nul 2>nul
  powershell -NoProfile -Command "$s=Get-Service -Name X1FanService -ErrorAction SilentlyContinue;if($s -and $s.Status -ne 'Stopped'){$s.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))};if($s -and $s.Status -ne 'Stopped'){exit 1}"
  if errorlevel 1 (
    echo X1FanService did not stop in time.
    exit /b 1
  )
)

copy /y "%~dp0X1FanService.exe" "%DEST%\X1FanService.exe" >nul
if errorlevel 1 exit /b 1
copy /y "%~dp0LpcACPIEC.bin" "%DEST%\LpcACPIEC.bin" >nul
if errorlevel 1 exit /b 1

sc query X1FanService >nul 2>nul
if errorlevel 1 (
  sc create X1FanService binPath= "\"%DEST%\X1FanService.exe\"" start= auto obj= LocalSystem DisplayName= "X1 Fan Telemetry Service"
) else (
  sc config X1FanService binPath= "\"%DEST%\X1FanService.exe\"" start= auto obj= LocalSystem
)
if errorlevel 1 exit /b 1
sc config X1FanService depend= PawnIO >nul
sc description X1FanService "Dual-fan RPM telemetry and safe optional fan modes for X1 AI Island." >nul
sc failure X1FanService reset= 86400 actions= restart/1000/restart/3000/restart/10000 >nul
sc start X1FanService
if errorlevel 1 exit /b 1
powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Programs')+'\X1 Fan Control.lnk');$s.TargetPath='%DEST%\X1FanService.exe';$s.WorkingDirectory='%DEST%';$s.Save()"
exit /b %errorlevel%
