# Ocean FFT Water System

A self-contained, custom-built GPU ocean simulation for Unreal Engine 5, plus a
physically based water colour model layered on top of it. No Epic Water plugin, no
`WaterBodyOcean`, no Gerstner wave actors: every wave value here is produced by the
code in this folder, and every colour value is produced by the materail graph it
drives (see the write-up in `Docs/` for a full diagram of that graph's wiring).

This module is written to be put into another UE5 project with ease.

## Content

| File | What it does |
| `OceanFFTShaders.h` / `.cpp` | Four `FGlobalShader` compute shader declarations: spectrum init, time evolution, FFT butterfly, inversion. |
| `OceanFFTSubsystem.h` / `.cpp` | A `UTickableWorldSubsystem` that builds the CPU-side FFT lookup tables once, then dispatches the four shaders per cascade every frame and writes the result into render targets the water material samples. |
| `../../Shaders/Ocean/OceanFFT.usf` | The actual HLSL for all four passes. |

The technique is Jerry Tessendorf's FFT ocean method, the same published approach
behind the water in *Sea of Thieves*, not a simplified "sum of sines" approx.
See the project write-up (`Docs/`) for the full derivation, references and a
comparison against other real-time water techniques.

## Quick start: switching water type

Everything needed to go from open ocean to a tropical lagoon to a murky river is one
call:

```cpp
UOceanFFTSubsystem* Ocean = GetWorld()->GetSubsystem<UOceanFFTSubsystem>();
Ocean->ApplyWaterPreset(EWaterPreset::Tropical);
```

Or with no code at all: select the subsystem in the World Settings / editor outliner,
pick a value from the Active Water Preset dropdown, and press the Apply Water Preset
button that appears in its details panel (`CallInEditor`). It updates both the wave
shape (wind, amplitude, ripple suppression) and the water's optical colour
(absorption/scattering coefficients, phase function) in one go live with no material
editor work at all.

Preset numbers aren't arbitrary. The optical values are absorption/scattering
coefficients taken from published water-optics measurements (Pope & Fry's pure
seawater spectrum, Jerlov's water-type classification), not hand picked colours to implement realism. See
`UOceanFFTSubsystem::GetPresetSettings` and the write-up for the sourcing.

## Using this in your own project

1. Copy `Source/<YourModule>/Ocean/` and `Shaders/Ocean/OceanFFT.usf` into your
   project, with the shader going under your project's `Shaders/Private/Ocean/`
   folder (this repo keeps it at `Shaders/Ocean/` directly since it's a source
   drop-in rather than a full project - see the note in `.gitignore`).
2. Add `"RenderCore", "RHI", "Renderer"` to `PrivateDependencyModuleNames` in your `.Build.cs` (see this project's `WaterGraphicsV2.Build.cs` for a working example).
3. Make sure your project maps the `/Project/Private/...` virtual shader path to your `Shaders/Private/` folder (this is UE5's default project shader mapping, most projects already have it).
4. Give your water's material a Single Layer Water shading model, and expose `AbsorptionCoefficients` (vector), `ScatteringCoefficients` (vector) and `PhaseG` (scalar) as material parameters wired into the Single Layer Water output node - that's what `ApplyWaterPreset` drives.
5. Tag your water mesh actor `OceanWaterSurface` (the subsystem finds it via `ActorHasTag`) and sample `HeightMap0`/`HeightMap1`/`HeightMap2` (one per cascade) in your material's World Position Offset to displace vertices: R = height, G = X displacement, B = Y displacement.
6. Tag an optional flat debug plane `OceanHeightDebug` to visualise cascade 0's raw output while you're setting things up.

No Blueprint setup is required for the simulation itself.

## Tuning at runtime

Every cascade's contribution to the final surface is exposed as console variables, so
you can tune the look without touching a single line of code or recompiling:

```
Ocean.HeightWeight0 5.0    // how much cascade 0 (large swells) contributes to surface height
Ocean.NormalWeight1 -1.5   // how much cascade 1 (medium waves) contributes to the normal/slope
Ocean.Choppiness2 3.0      // horizontal Gerstner displacement strength, cascade 2 (fine ripples)
Ocean.DebugLog 1           // periodically logs cascade 0's centre height/displacement values
```

## Known limitations

- Grid size and patch lengths are fixed at `Initialize` time - changing `GridSize` at
  runtime requires a subsystem re-init.
- The FFT runs at full resolution on all three cascades every frame; there's no
  distance-based LOD or update-rate throttling for cascades far from the camera.
- Presets set one uniform look for the whole water body. Spatial blending between
  presets across a single large body of water (e.g. river mouth fading into open
  ocean) is possible - the material already supports it via `Biome_*` parameters (see
  the write-up's material graph diagram) - but isn't wired into teh `ApplyWaterPreset`
  itself.
