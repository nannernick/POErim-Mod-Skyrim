# Diablo In Skyrim (fork)

Diablo-style randomized affix loot for Skyrim SE/AE (SKSE plugin), forked from
[tangwesley/DiabloRim](https://github.com/tangwesley/DiabloRim). All original
work and credit belong to the original author; this fork adds currency crafting
and tooling on top of it. Check the upstream repository for its licensing terms.

## Building

Builds automatically on GitHub Actions (`.github/workflows/build.yml`) on every
push. To get the DLL:

1. Open the repo's **Actions** tab and pick the latest successful **Build** run.
2. Download the `DiabloInSkyrim` artifact.
3. Install the zip with Vortex (or drop `SKSE/` into your Data folder).

The artifact contains `SKSE/Plugins/DiabloInSkyrim.dll`, the affix and name
CSVs, and `DiabloLoot.ini.default` (copy it to `DiabloLoot.ini` to customize).

Local build (Windows, Visual Studio 2022, vcpkg):

    cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static
    cmake --build build --config Release

Set `-DMOD_ROOT=<your mod folder>` to auto-copy the DLL after each build.

## Runtime

Skyrim SE/AE 1.5.x-1.7.x via Address Library, SKSE64.
