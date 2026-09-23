# Ocean FFT Water System for Unreal Engine 5

A custom GPU compute-shader ocean sim for UE5, using Tessendorf's FFT ocean
method, plus a water colour model driven by real water-optics measurements instead of
a hand picked gradient. Built from Unreal's raw primitives: compute shaders, the Render
Dependency Graph and the Single Layer Water shading model. No Epic Water plugin, no
`WaterBodyOcean`, no Gerstner wave actors.

Full write-up (equations, algorithms, references, and an honest comparison against
Epic's Water plugin and techniques used in shipped titles like *Sea of Thieves* and
*Horizon Forbidden West*): [`Docs/WaterGraphicsV2_Dissertation.pdf`](Docs/WaterGraphicsV2_Dissertation.pdf).

## What's in this project

- **Multi-cascade GPU FFT ocean sim.** Phillips spectrum, dispersion relation time
  evolution, Cooley-Tukey radix-2 FFT, inversion: four `FGlobalShader` compute passes
  across three cascades (large swells, medium waves, fine ripples), run through
  Unreal's RDG every frame.
- **Water colour sourced from real data, not a picked gradient.** The Single Layer
  Water shading model's absorption/scattering inputs come from Pope & Fry's (1997)
  pure seawater absorption spectrum and Jerlov's water-type classification.
- **One-call preset switching.** `ApplyWaterPreset(EWaterPreset::Tropical)` (or a
  dropdown and button in the editor) re-tunes both the wave shape and the water colour
  between Ocean, Tropical and Murky at runtime.

## Usage Example

Once the system is integrated, changing the entire ocean's simulation and optics at runtime takes just two lines of code:

```cpp
UOceanFFTSubsystem* Ocean = GetWorld()->GetSubsystem<UOceanFFTSubsystem>();
Ocean->ApplyWaterPreset(EWaterPreset::Ocean);
```

See [`Source/Ocean/README.md`](Source/Ocean/README.md) for the full integration guide:
what to add to your `.Build.cs`, how to wire up the material parameters and how to tag
the water mesh so the subsystem finds it automatically.

## Repo layout

```
OceanFFTWaterSystem/
├── Source/Ocean/          C++: the subsystem (orchestration, presets) + compute shader declarations
├── Shaders/Ocean/         HLSL: the actual FFT / spectrum / inversion compute shader code
├── Docs/                  The full technical write-up (PDF) - includes the material graph diagrams
├── LICENSE
└── README.md              (this file)
```

To set up the water material, just follow the node diagrams in Chapter 4 of Docs/WaterGraphicsV2_Dissertation.pdf.

## Requirements

- Unreal Engine 5.4+ (uses `FRHITextureCreateDesc`, RDG, `UTickableWorldSubsystem`)
- A project module with `RenderCore`, `RHI` and `Renderer` as private dependencies
- A material using the Single Layer Water shading model

## License

MIT - see [`LICENSE`](LICENSE)

## References

The technique and the optical coefficients aren't invented, they trace back to
published sources. Full citations are in the write-up:

- Jerry Tessendorf, 'Simulating Ocean Water', *SIGGRAPH 2001 Course Notes* (ACM, 2001).
- James W. Cooley and John W. Tukey, 'An Algorithm for the Machine Calculation of
  Complex Fourier Series', *Mathematics of Computation*, 19.90 (1965), 297-301.
- R. M. Pope and E. S. Fry, 'Absorption Spectrum (380-700 nm) of Pure Water. II.',
  *Applied Optics*, 36.33 (1997), 8710-8723.
- N. G. Jerlov, *Marine Optics*, 2nd edn (Elsevier, 1976).
- Nigel Ang and others, 'The Technical Art of *Sea of Thieves*', *SIGGRAPH '18 Talks*
  (ACM, 2018) - confirms this is the same technique used in a shipped AAA title.
