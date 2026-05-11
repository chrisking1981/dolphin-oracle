# Dolphin Emulator bouwen met Visual Studio 2026

## Vereisten

- Visual Studio 2026 met:
  - Desktop development with C++
  - MSVC v143 - VS 2022 C++ x64/x86 build tools
  - Windows 11 SDK

## Probleem: VS 2026 en v143 toolset

Dolphin is gemaakt voor VS 2022 (v143 toolset), maar VS 2026 heeft standaard v145. De v143 compiler is wel geïnstalleerd, maar de MSBuild targets ontbreken.

## Oplossing: Junctions maken (eenmalig, als admin)

Open een **Administrator Command Prompt** en voer uit:

```cmd
mklink /J "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Microsoft\VC\v143" "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Microsoft\VC\v180"

mklink /J "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v143" "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v145"
```

## Configuratie aanpassingen

### 1. VCToolsVersion instellen

Bewerk `Source\VSProps\Configuration.Base.props` en voeg `VCToolsVersion` toe:

```xml
<PropertyGroup Label="Configuration">
  <PlatformToolset>v143</PlatformToolset>
  <VCToolsVersion>14.44.35207</VCToolsVersion>  <!-- DEZE REGEL TOEVOEGEN -->
  <CharacterSet>Unicode</CharacterSet>
  ...
</PropertyGroup>
```

### 2. Glslang fixen (cmake kent VS 18 niet)

Bewerk `Externals\glslang\glslang.vcxproj`:

**Generator en paths aanpassen (rond regel 30-36):**
```xml
<CmakeGenerator>Ninja</CmakeGenerator>
<CmakePath>"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"</CmakePath>
<NinjaPath>"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"</NinjaPath>
<CmakeReleaseCLI>call vsdevcmd.bat -arch=$(DevCmdArch)
  $(CmakePath) -G $(CmakeGenerator) -DCMAKE_BUILD_TYPE="Release" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_MAKE_PROGRAM=$(NinjaPath) -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_EXTERNAL=OFF -DENABLE_SPVREMAPPER=OFF -DENABLE_HLSL=OFF -DENABLE_OPT=OFF -DENABLE_EXCEPTIONS=OFF -S glslang -B "$(BuildRootDir)tmp\$(ProjectName)-$(Configuration)-$(Platform)"
</CmakeReleaseCLI>
```

**Build commands aanpassen (msbuild -> ninja):**
```xml
<NMakeBuildCommandLine>
  $(CmakeReleaseCLI)
  $(NinjaPath) -C $(IntDir).
  $(CmakeCopyCLI)
</NMakeBuildCommandLine>
```

**Copy paths aanpassen (geen $(CONFIGURATION) subdirs met Ninja):**
```xml
<CmakeCopyCLI>
  echo Copying..
  if not exist "$(BuildRootDir)$(Platform)\$(Configuration)\$(ProjectName)\bin\" mkdir "$(BuildRootDir)$(Platform)\$(Configuration)\$(ProjectName)\bin\"
  copy "$(BuildRootDir)tmp\$(ProjectName)-$(Configuration)-$(Platform)\SPIRV\*.lib" "$(BuildRootDir)$(Platform)\$(Configuration)\$(ProjectName)\bin"
  copy "$(BuildRootDir)tmp\$(ProjectName)-$(Configuration)-$(Platform)\glslang\OSDependent\Windows\*.lib" "$(BuildRootDir)$(Platform)\$(Configuration)\$(ProjectName)\bin"
  copy "$(BuildRootDir)tmp\$(ProjectName)-$(Configuration)-$(Platform)\glslang\*.lib" "$(BuildRootDir)$(Platform)\$(Configuration)\$(ProjectName)\bin"
</CmakeCopyCLI>
```

### 3. Glslang override.props aanpassen

Bewerk `Externals\glslang\override.props` en voeg na de Globals PropertyGroup toe:

```xml
<PropertyGroup Label="Configuration">
  <PlatformToolset>v143</PlatformToolset>
  <VCToolsVersion>14.44.35207</VCToolsVersion>
</PropertyGroup>
```

## Bouwen

### Optie 1: Via batch file

Maak `build_dolphin.bat`:
```batch
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
msbuild "C:\projects\dolphin-gdb\Source\dolphin-emu.sln" /p:Configuration=Release /p:Platform=x64 /m /verbosity:minimal
```

Voer uit:
```cmd
build_dolphin.bat
```

### Optie 2: Via Visual Studio

1. Open `Source\dolphin-emu.sln`
2. Selecteer Release | x64
3. Build > Build Solution (F7)

## Output

Na succesvolle build:
```
Binary\x64\Dolphin.exe        - Hoofdprogramma
Binary\x64\DolphinTool.exe    - CLI tool
```

## GDB gebruiken

Start met GDB poort:
```cmd
Dolphin.exe -C General.GDBPort=1234
```

Verbind met GDB:
```
gdb
target remote localhost:1234
```

Of via config file (`Dolphin.ini`):
```ini
[General]
GDBPort = 1234
```
