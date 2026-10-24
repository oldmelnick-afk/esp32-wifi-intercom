@echo off
rem Runs the intercom voice assistant; restarts it if it ever stops.
cd /d "%~dp0"
set PYTHONIOENCODING=utf-8
set PYTHONUNBUFFERED=1
:loop
py -3.11 gateway.py >> gateway.log 2>&1
echo %date% %time% gateway stopped, restarting in 10 s >> gateway.log
timeout /t 10 /nobreak > nul
goto loop
