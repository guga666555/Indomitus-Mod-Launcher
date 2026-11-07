@echo off
rem In-house test: inject gem_boardfilter.dll into a running game/editor.
rem Works from the source folder, out\, or the package's tools\ (dll + ini in ..\bin).
cd /d "%~dp0"
if not exist gem_inject.exe if exist out\gem_inject.exe cd out
set DLL=gem_boardfilter.dll
if not exist "%DLL%" if exist ..\bin\gem_boardfilter.dll set DLL=..\bin\gem_boardfilter.dll
gem_inject.exe "%DLL%"
pause
