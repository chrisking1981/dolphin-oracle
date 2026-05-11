@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
msbuild "C:\projects\dolphin-gdb\Source\dolphin-emu.sln" /p:Configuration=Release /p:Platform=x64 /m /verbosity:minimal
