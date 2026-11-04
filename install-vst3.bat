@echo off
set SRC=%~dp0build\TR8SSideStream_artefacts\Release\VST3\TR-8S SideStream.vst3
set DST=%CommonProgramFiles%\VST3\TR-8S SideStream.vst3
echo Installing 0.4.3 to %DST%
if exist "%DST%" rmdir /s /q "%DST%"
xcopy /e /i /y "%SRC%" "%DST%"
echo Done. Size:
dir "%DST%\Contents\x86_64-win\TR-8S SideStream.vst3"
pause
