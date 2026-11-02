@echo off
rem Runs build.bat and writes everything it prints to build.log (for reporting a build problem).
cd /d "%~dp0"
(
  echo === build.bat %1 ===
  if exist "..\..\zig\zig.exe" "..\..\zig\zig.exe" version
  call build.bat %1
  echo === exit code %ERRORLEVEL% ===
) > build.log 2>&1
