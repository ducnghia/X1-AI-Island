@echo off
setlocal
net session >nul 2>nul
if errorlevel 1 (
  echo This uninstaller must be run as Administrator.
  exit /b 1
)
sc stop X1FanService >nul 2>nul
sc delete X1FanService
