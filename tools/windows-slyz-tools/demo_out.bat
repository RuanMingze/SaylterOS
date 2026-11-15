@echo off
setlocal EnableExtensions
set "SLYZ_LAST=0"
echo Hello from SLYZ on Windows
set /p SLYZ_ASK=Continue? [Y/N]? 
if /I "%SLYZ_ASK%"=="Y" ( set "SLYZ_LAST=1" ) else ( set "SLYZ_LAST=0" )
if /I "%SLYZ_LAST%"=="1" (
echo continue path
)
if /I "%SLYZ_LAST%"=="0" (
echo stop path
)
endlocal
