@echo off
REM =========================================================
REM  Build everything: firmware + Android app -> out\
REM
REM  Usage:  build_all.bat [fwLabel] [appLabel]
REM          e.g.  build_all.bat v7 v33
REM =========================================================

call "%~dp0build_fw.bat" %1
if errorlevel 1 exit /b 1

call "%~dp0build_app.bat" %2
if errorlevel 1 exit /b 1

echo [DONE] artifacts are in out\
exit /b 0
