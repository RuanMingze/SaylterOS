@echo off
setlocal EnableExtensions
set "SLYZ_LAST=0"
set "SLYZ_ASK="
set "SLYZ_FORCE_ANSWER=Y"
echo Hello from SLYZ on Windows
if not "%SLYZ_FORCE_ANSWER%"=="" (
  if /I "%SLYZ_FORCE_ANSWER%"=="Y" ( set "SLYZ_LAST=1" ) else ( set "SLYZ_LAST=0" )
) else (
  set /p "SLYZ_ASK=Continue? [Y/N]? "
  if /I "%SLYZ_ASK%"=="Y" ( set "SLYZ_LAST=1" ) else ( set "SLYZ_LAST=0" )
)
if /I "%SLYZ_LAST%"=="1" (
echo continue path
)
if /I "%SLYZ_LAST%"=="0" (
echo stop path
)
endlocal
