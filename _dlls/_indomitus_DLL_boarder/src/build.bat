@echo off
rem moved: the build now lives one folder up (build.bat [payload|test|inject])
call "%~dp0..\build.bat" %*
