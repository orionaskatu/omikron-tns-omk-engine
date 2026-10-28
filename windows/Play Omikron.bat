@echo off
rem Starts Omikron with the OMK engine.
rem If your game is not in the Steam folder below, change the path on the "set GAME" line
rem (or set the OMIKRON_DATA variable). The game's files are only read, never changed.
rem Settings: copy omk.ini.example to omk.ini and edit it. This file picks it up by itself.
rem Extra arguments are passed on, e.g.  "Play Omikron.bat" --d3d12
setlocal
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Omikron"
if not "%OMIKRON_DATA%"=="" set "GAME=%OMIKRON_DATA%"
cd /d "%~dp0"
if not exist omk-saves mkdir omk-saves
set "CFG="
if exist omk.ini set "CFG=--config omk.ini"
omk-play.exe "%GAME%" tables %CFG% --saves omk-saves\GAMES %* > omk-play.log 2> omk-play-errors.log
if errorlevel 1 (
    echo.
    echo Omikron stopped with an error:
    echo -----------------------------------------------------------------
    type omk-play-errors.log
    echo -----------------------------------------------------------------
    echo The full log is omk-play.log in this folder.
    pause
)
endlocal
