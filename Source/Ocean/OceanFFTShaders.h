//Ocean FFT compute shaders - Tessendorf's method (see writeeup)
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

#define OCEAN_FFT_THREADGROUP_SIZE 16

//Builds H0(k) / H0(-k), the initial freq-domain spectrum, from Phillips + precomputed Gaussian noise. Only runs once at init or when wind changes.
class FOceanInitSpectrumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanInitSpectrumCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanInitSpectrumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GridSize)
		SHADER_PARAMETER(float, PatchLength)
		SHADER_PARAMETER(float, WindSpeed)
		SHADER_PARAMETER(FVector2f, WindDirection)
		SHADER_PARAMETER(float, Amplitude)
		SHADER_PARAMETER(float, SuppressSmallWaves)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, NoiseTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, H0Output)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("OCEAN_FFT_THREADGROUP_SIZE"), OCEAN_FFT_THREADGROUP_SIZE);
	}
};

//Evolves H0 to time t (deep water dispersion), plus the Gerstner style choppy displacement spectraDx/Dy = i*(k/|k|) * H(k,t). Feeds their own FFTs next.
class FOceanTimeSpectrumCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanTimeSpectrumCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanTimeSpectrumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GridSize)
		SHADER_PARAMETER(float, PatchLength)
		SHADER_PARAMETER(float, Time)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, H0Texture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, HktOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, DxOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, DyOutput)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("OCEAN_FFT_THREADGROUP_SIZE"), OCEAN_FFT_THREADGROUP_SIZE);
	}
};

//One stage of a Cooley-Tukey radix-2 FFT butterfly (Cooley & Tukey 1965), driven by a precomputed lookup texture (index pair + twiddle per stage). log2(N) horizontal passes then log2(N) vertical.
class FOceanFFTButterflyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanFFTButterflyCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanFFTButterflyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GridSize)
		SHADER_PARAMETER(uint32, Stage)
		SHADER_PARAMETER(uint32, Direction) // 0 = horizontal, 1 = vertical
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ButterflyTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, InputTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("OCEAN_FFT_THREADGROUP_SIZE"), OCEAN_FFT_THREADGROUP_SIZE);
	}
};

//Applies the (-1)^(x+y) recentering fix and packs height/Dx/Dy (RGB) into the render target the material samples.
class FOceanInversionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FOceanInversionCS);
	SHADER_USE_PARAMETER_STRUCT(FOceanInversionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GridSize)
		SHADER_PARAMETER(float, NormalizationScale)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, HeightFFTResult)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DxFFTResult)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, DyFFTResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, HeightOutput)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return true; }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("OCEAN_FFT_THREADGROUP_SIZE"), OCEAN_FFT_THREADGROUP_SIZE);
	}
};
