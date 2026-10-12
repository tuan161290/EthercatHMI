@echo off
rem Build / flash / monitor the EthercatHMI firmware on the JC1060P470C (ESP32-P4).
rem Usage: double-click, or "flash.bat [COMx]" from a terminal.
rem All logic is in tools\flash.ps1: menu, port detection, chip revision check and a full
rem 16 MB backup of the board's flash before the first write (docs\SETUP.md section 3).
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\flash.ps1" %*
if errorlevel 1 pause
