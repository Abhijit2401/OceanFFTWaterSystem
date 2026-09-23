#include "OceanFFTSubsystem.h"
#include "OceanFFTShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "TextureResource.h"
#include "GlobalShader.h"
#include "RHICommandList.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogOceanFFT, Log, All);

static TAutoConsoleVariable<float> CVarOceanHeightWeight0(TEXT("Ocean.HeightWeight0"), 5.0f, TEXT("Height contribution weight, cascade 0 (large swells)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarOceanHeightWeight1(TEXT("Ocean.HeightWeight1"), 2.0f, TEXT("Height contribution weight, cascade 1 (medium waves)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarOceanHeightWeight2(TEXT("Ocean.HeightWeight2"), 0.5f, TEXT("Height contribution weight, cascade 2 (fine ripples)."), ECVF_Default);

static TAutoConsoleVariable<float> CVarOceanNormalWeight0(TEXT("Ocean.NormalWeight0"), -0.3f, TEXT("Normal/slope contribution weight, cascade 0 (large swells)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarOceanNormalWeight1(TEXT("Ocean.NormalWeight1"), -1.5f, TEXT("Normal/slope contribution weight, cascade 1 (medium waves)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarOceanNormalWeight2(TEXT("Ocean.NormalWeight2"), -6.0f, TEXT("Normal/slope contribution weight, cascade 2 (fine ripples)."), ECVF_Default);

static TAutoConsoleVariable<float> CVarOceanChoppiness0(TEXT("Ocean.Choppiness0"), 3.0f, TEXT("Horizontal Gerstner displacement weight, cascade 0 (large swells)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarOceanChoppiness1(TEXT("Ocean.Choppiness1"), 4.0f, TEXT("Horizontal Gerstner displacement weight, cascade 1 (medium waves)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarOceanChoppiness2(TEXT("Ocean.Choppiness2"), 3.0f, TEXT("Horizontal Gerstner displacement weight, cascade 2 (fine ripples)."), ECVF_Default);

//Off by default, flip on with "Ocean.DebugLog 1" to confirm the sim's actually
//producing non-zero data if a height map ever looks wrong.
static TAutoConsoleVariable<int32> CVarOceanDebugLog(TEXT("Ocean.DebugLog"), 0, TEXT("Periodically make debug log entries."), ECVF_Default);

//PRESET TABLE
//The numbers behind the Ocean/Tropical/Murky dropdown. Wave related values were tuned by eye,
//optical values come from actual water-optics data not handpicked colours - Pope & Fry 97
//for open ocean absorption, Jerlov 76 water types for the coastal/turbid ones.
FWaterPresetSettings UOceanFFTSubsystem::GetPresetSettings(EWaterPreset Preset)
{
	FWaterPresetSettings Settings;
	switch (Preset)
	{
	case EWaterPreset::Ocean:
		//Clear open ocean. Pope & Fry 97 absorption spectrum + Jerlov Type I scattering,
		//converted from 1/m to Unreal's 1/cm.
		Settings.WindSpeed = 600.0f;
		Settings.WindDirection = FVector2D(1.0, 0.3);
		Settings.Amplitude = 6.0f;
		Settings.SuppressSmallWaves = 0.3f;
		Settings.AbsorptionCoefficients = FLinearColor(0.0034f, 0.0006f, 0.00015f);
		Settings.ScatteringCoefficients = FLinearColor(0.0003f, 0.0004f, 0.0005f);
		Settings.PhaseG = 0.1f;
		break;

	case EWaterPreset::Tropical:
		//Shallow coastal/lagoon water (Jerlov Type III). CDOM blue way more than
		//open ocean and green goes deepest, that's the reason this reads turquoise and not deep blue
		Settings.WindSpeed = 250.0f;
		Settings.WindDirection = FVector2D(1.0, 0.3);
		Settings.Amplitude = 2.5f;
		Settings.SuppressSmallWaves = 0.4f;
		Settings.AbsorptionCoefficients = FLinearColor(0.004f, 0.0012f, 0.0025f);
		Settings.ScatteringCoefficients = FLinearColor(0.002f, 0.0025f, 0.003f);
		Settings.PhaseG = 0.3f;
		break;

	case EWaterPreset::Murky:
	default:
		//Turbid sediment-heavy river/lake water. Suspended sediment scatters LONGER (red) wavelengths more than short ones, opposite of clear ocean.
		Settings.WindSpeed = 120.0f;
		Settings.WindDirection = FVector2D(0.6, 1.0);
		Settings.Amplitude = 0.8f;
		Settings.SuppressSmallWaves = 0.6f;
		Settings.AbsorptionCoefficients = FLinearColor(0.006f, 0.005f, 0.008f);
		Settings.ScatteringCoefficients = FLinearColor(0.008f, 0.006f, 0.004f);
		Settings.PhaseG = 0.5f;
		break;
	}
	return Settings;
}

//Pushes both halves of a preset at once - wave shape into this subsystem's sim params, colour into the bound water material.
void UOceanFFTSubsystem::ApplyWaterPreset(EWaterPreset Preset)
{
	ActiveWaterPreset = Preset;
	const FWaterPresetSettings Settings = GetPresetSettings(Preset);

	//feeds straight into the Phillips spectrum / dispersion relation next Tick, so cascade 0 re-initialises with the new wind/amplitude
	WindSpeed = Settings.WindSpeed;
	WindDirection = Settings.WindDirection;
	Amplitude = Settings.Amplitude;
	SuppressSmallWaves = Settings.SuppressSmallWaves;
	for (FOceanCascadeRuntime& Cascade : Cascades)
	{
		Cascade.bSpectrumInitialized = false;
	}

	//pushed straight to the SLW material params already wired up in the water material's graph (VectorParameter/ScalarParameter nodes -> SLW output node)
	if (UMaterialInstanceDynamic* WaterMID = CachedWaterMID.Get())
	{
		WaterMID->SetVectorParameterValue(TEXT("AbsorptionCoefficients"), Settings.AbsorptionCoefficients);
		WaterMID->SetVectorParameterValue(TEXT("ScatteringCoefficients"), Settings.ScatteringCoefficients);
		WaterMID->SetScalarParameterValue(TEXT("PhaseG"), Settings.PhaseG);
	}

	UE_LOG(LogOceanFFT, Log, TEXT("OceanFFT: applied water preset %s"), *UEnum::GetValueAsString(Preset));
}

void UOceanFFTSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogOceanFFT, Log, TEXT("OceanFFTSubsystem:Initialize called, world=%s"), *GetWorld()->GetName());

	//round grid size up to nearest power of two, butterfly pass below only works on power-of-two grids (radix-2 Cooley-Tukey)
	LogGridSize = FMath::CeilLogTwo(GridSize);
	GridSize = 1 << LogGridSize;

	const int32 NumCascades = CascadePatchLengths.Num();
	Cascades.SetNum(NumCascades);
	HeightRenderTargets.SetNum(NumCascades);

	for (int32 i = 0; i < NumCascades; i++)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, *FString::Printf(TEXT("OceanHeightRT_%d"), i));
		RT->RenderTargetFormat = RTF_RGBA32f;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;
		RT->InitAutoFormat(GridSize, GridSize);
		RT->UpdateResourceImmediate(true);
		Cascades[i].HeightRenderTarget = RT;
		HeightRenderTargets[i] = RT;
	}

	BuildButterflyData();
	BuildNoiseData();

	TArray<FVector4f> ButterflyCopy = ButterflyCPUData;
	TArray<FVector4f> NoiseCopy = NoiseCPUData;
	int32 LocalGridSize = GridSize;
	int32 LocalLogGridSize = LogGridSize;

	ENQUEUE_RENDER_COMMAND(OceanFFTCreateResources)(
		[this, ButterflyCopy, NoiseCopy, LocalGridSize, LocalLogGridSize, NumCascades](FRHICommandListImmediate& RHICmdList)
		{
			FRHITextureCreateDesc ButterflyDesc = FRHITextureCreateDesc::Create2D(
				TEXT("OceanButterfly"), LocalLogGridSize, LocalGridSize, PF_A32B32G32R32F);
			ButterflyDesc.SetFlags(ETextureCreateFlags::ShaderResource);
			ButterflyTextureRHI = RHICreateTexture(ButterflyDesc);

			FRHITextureCreateDesc NoiseDesc = FRHITextureCreateDesc::Create2D(
				TEXT("OceanNoise"), LocalGridSize, LocalGridSize, PF_A32B32G32R32F);
			NoiseDesc.SetFlags(ETextureCreateFlags::ShaderResource);
			NoiseTextureRHI = RHICreateTexture(NoiseDesc);

			{
				uint32 DestStride = 0;
				void* Dest = RHICmdList.LockTexture2D(ButterflyTextureRHI, 0, RLM_WriteOnly, DestStride, false);
				for (int32 Y = 0; Y < LocalGridSize; Y++)
				{
					FMemory::Memcpy((uint8*)Dest + Y * DestStride, ButterflyCopy.GetData() + Y * LocalLogGridSize, LocalLogGridSize * sizeof(FVector4f));
				}
				RHICmdList.UnlockTexture2D(ButterflyTextureRHI, 0, false);
			}
			{
				uint32 DestStride = 0;
				void* Dest = RHICmdList.LockTexture2D(NoiseTextureRHI, 0, RLM_WriteOnly, DestStride, false);
				for (int32 Y = 0; Y < LocalGridSize; Y++)
				{
					FMemory::Memcpy((uint8*)Dest + Y * DestStride, NoiseCopy.GetData() + Y * LocalGridSize, LocalGridSize * sizeof(FVector4f));
				}
				RHICmdList.UnlockTexture2D(NoiseTextureRHI, 0, false);
			}

			for (int32 i = 0; i < NumCascades; i++)
			{
				FRHITextureCreateDesc H0Desc = FRHITextureCreateDesc::Create2D(
					*FString::Printf(TEXT("OceanH0_%d"), i), LocalGridSize, LocalGridSize, PF_A32B32G32R32F);
				H0Desc.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV);
				Cascades[i].H0TextureRHI = RHICreateTexture(H0Desc);
			}

			UE_LOG(LogOceanFFT, Log, TEXT("OceanFFT RHI resources created (grid=%d, log2=%d, cascades=%d)"), LocalGridSize, LocalLogGridSize, NumCascades);
		});
}

void UOceanFFTSubsystem::Deinitialize()
{
	FlushRenderingCommands();
	ButterflyTextureRHI.SafeRelease();
	NoiseTextureRHI.SafeRelease();
	for (FOceanCascadeRuntime& Cascade : Cascades)
	{
		Cascade.H0TextureRHI.SafeRelease();
	}
	Super::Deinitialize();
}

TStatId UOceanFFTSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOceanFFTSubsystem, STATGROUP_Tickables);
}

void UOceanFFTSubsystem::BuildButterflyData()
{
	ButterflyCPUData.SetNumUninitialized(GridSize * LogGridSize);

	auto BitReverse = [](uint32 Value, int32 Bits) -> uint32
	{
		uint32 Result = 0;
		for (int32 B = 0; B < Bits; B++)
		{
			Result = (Result << 1) | (Value & 1);
			Value >>= 1;
		}
		return Result;
	};

	for (int32 Stage = 0; Stage < LogGridSize; Stage++)
	{
		int32 BlockSize = 1 << (Stage + 1);
		int32 HalfBlock = BlockSize / 2;

		for (int32 X = 0; X < GridSize; X++)
		{
			int32 BlockStart = (X / BlockSize) * BlockSize;
			int32 PosInBlock = X % BlockSize;
			int32 K = PosInBlock % HalfBlock;

			float Angle = -PI * 2.0f * (float)K / (float)BlockSize;
			float TwiddleRe = FMath::Cos(Angle);
			float TwiddleIm = FMath::Sin(Angle);

			int32 IndexA, IndexB;
			if (Stage == 0)
			{
				IndexA = (int32)BitReverse(BlockStart + K, LogGridSize);
				IndexB = (int32)BitReverse(BlockStart + K + HalfBlock, LogGridSize);
			}
			else
			{
				IndexA = BlockStart + K;
				IndexB = BlockStart + K + HalfBlock;
			}

			ButterflyCPUData[X * LogGridSize + Stage] = FVector4f((float)IndexA, (float)IndexB, TwiddleRe, TwiddleIm);
		}
	}
}

void UOceanFFTSubsystem::BuildNoiseData()
{
	NoiseCPUData.SetNumUninitialized(GridSize * GridSize);
	FRandomStream Rand(12345);

	auto Gauss = [&Rand]() -> float
	{
		float U1 = FMath::Max(Rand.GetFraction(), 1e-6f);
		float U2 = Rand.GetFraction();
		return FMath::Sqrt(-2.0f * FMath::Loge(U1)) * FMath::Cos(2.0f * PI * U2);
	};

	for (int32 Y = 0; Y < GridSize; Y++)
	{
		for (int32 X = 0; X < GridSize; X++)
		{
			NoiseCPUData[Y * GridSize + X] = FVector4f(Gauss(), Gauss(), Gauss(), Gauss());
		}
	}
}

static UMaterialInstanceDynamic* BindHeightMapToTaggedActor(UWorld* World, const TCHAR* Tag, const TCHAR* LogContext)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor->ActorHasTag(FName(Tag)))
		{
			continue;
		}

		UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC)
		{
			continue;
		}

		UMaterialInterface* BaseMat = SMC->GetMaterial(0);
		if (!BaseMat)
		{
			continue;
		}

		UMaterialInstanceDynamic* MID = SMC->CreateDynamicMaterialInstance(0, BaseMat);
		if (MID)
		{
			UE_LOG(LogOceanFFT, Log, TEXT("OceanFFT: bound %s actor '%s'"), LogContext, *Actor->GetName());
			return MID;
		}
	}
	return nullptr;
}

void UOceanFFTSubsystem::TryBindDebugPlane()
{
	UWorld* World = GetWorld();
	if (!World || Cascades.Num() == 0)
	{
		return;
	}

	if (!bDebugPlaneBound)
	{
		if (UMaterialInstanceDynamic* DebugMID = BindHeightMapToTaggedActor(World, TEXT("OceanHeightDebug"), TEXT("debug plane")))
		{
			DebugMID->SetTextureParameterValue(TEXT("HeightMap"), Cascades[0].HeightRenderTarget);
			bDebugPlaneBound = true;
		}
	}
	if (!bWaterSurfaceBound)
	{
		if (UMaterialInstanceDynamic* WaterMID = BindHeightMapToTaggedActor(World, TEXT("OceanWaterSurface"), TEXT("water surface")))
		{
			for (int32 i = 0; i < Cascades.Num(); i++)
			{
				WaterMID->SetTextureParameterValue(*FString::Printf(TEXT("HeightMap%d"), i), Cascades[i].HeightRenderTarget);
			}
			CachedWaterMID = WaterMID;
			bWaterSurfaceBound = true;
		}
	}
}

void UOceanFFTSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!ButterflyTextureRHI.IsValid() || !NoiseTextureRHI.IsValid() || Cascades.Num() == 0)
	{
		return;
	}
	for (const FOceanCascadeRuntime& Cascade : Cascades)
	{
		if (!Cascade.H0TextureRHI.IsValid() || !Cascade.HeightRenderTarget)
		{
			return;
		}
	}

	DiagnosticFrameCounter++;
	if (DiagnosticFrameCounter % 90 == 1 && Cascades.Num() > 0 && Cascades[0].HeightRenderTarget)
	{
		TArray<FLinearColor> OutPixels;
		FTextureRenderTargetResource* DiagRes = Cascades[0].HeightRenderTarget->GameThread_GetRenderTargetResource();
		if (DiagRes && DiagRes->ReadLinearColorPixels(OutPixels))
		{
			int32 Center = OutPixels.Num() / 2 + GridSize / 2;
			if (OutPixels.IsValidIndex(Center))
			{
				UE_LOG(LogOceanFFT, Warning, TEXT("OceanFFT DIAG cascade0 center pixel: Height(R)=%f Dx(G)=%f Dy(B)=%f (total pixels=%d)"),
					OutPixels[Center].R, OutPixels[Center].G, OutPixels[Center].B, OutPixels.Num());
			}
		}
		if (UMaterialInstanceDynamic* WaterMID = CachedWaterMID.Get())
		{
			UTexture* BoundTex = nullptr;
			WaterMID->GetTextureParameterValue(FName(TEXT("HeightMap0")), BoundTex);
			UE_LOG(LogOceanFFT, Warning, TEXT("OceanFFT DIAG WaterMID HeightMap0 bound texture = %s"), BoundTex ? *BoundTex->GetName() : TEXT("NULL"));
		}
	}

	if (!bDebugPlaneBound || !bWaterSurfaceBound)
	{
		TryBindDebugPlane();
	}

	if (UMaterialInstanceDynamic* WaterMID = CachedWaterMID.Get())
	{
		WaterMID->SetScalarParameterValue(TEXT("HeightWeight0"), CVarOceanHeightWeight0.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("HeightWeight1"), CVarOceanHeightWeight1.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("HeightWeight2"), CVarOceanHeightWeight2.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("NormalWeight0"), CVarOceanNormalWeight0.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("NormalWeight1"), CVarOceanNormalWeight1.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("NormalWeight2"), CVarOceanNormalWeight2.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("Choppiness0"), CVarOceanChoppiness0.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("Choppiness1"), CVarOceanChoppiness1.GetValueOnGameThread());
		WaterMID->SetScalarParameterValue(TEXT("Choppiness2"), CVarOceanChoppiness2.GetValueOnGameThread());
	}

	AccumulatedTime += DeltaTime;

	struct FCascadeRenderInfo
	{
		FTextureRHIRef H0RHI;
		FRHITexture* OutputRHI;
		float PatchLength;
		bool bNeedsInit;
	};

	TArray<FCascadeRenderInfo> RenderInfos;
	for (int32 i = 0; i < Cascades.Num(); i++)
	{
		FTextureRenderTargetResource* RTResource = Cascades[i].HeightRenderTarget->GameThread_GetRenderTargetResource();
		if (!RTResource)
		{
			return;
		}
		FCascadeRenderInfo Info;
		Info.H0RHI = Cascades[i].H0TextureRHI;
		Info.OutputRHI = RTResource->GetRenderTargetTexture();
		Info.PatchLength = CascadePatchLengths.IsValidIndex(i) ? CascadePatchLengths[i] : 2000.0f;
		Info.bNeedsInit = !Cascades[i].bSpectrumInitialized;
		RenderInfos.Add(Info);
	}
	for (int32 i = 0; i < Cascades.Num(); i++)
	{
		Cascades[i].bSpectrumInitialized = true;
	}

	int32 LocalGridSize = GridSize;
	int32 LocalLogGridSize = LogGridSize;
	float LocalTime = (float)AccumulatedTime;
	float LocalWindSpeed = WindSpeed;
	FVector2f LocalWindDir = FVector2f(WindDirection.GetSafeNormal());
	float LocalAmplitude = Amplitude;
	float LocalSuppress = SuppressSmallWaves;

	ENQUEUE_RENDER_COMMAND(OceanFFTTick)(
		[this, RenderInfos, LocalGridSize, LocalLogGridSize, LocalTime, LocalWindSpeed, LocalWindDir, LocalAmplitude, LocalSuppress](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef ButterflyRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(ButterflyTextureRHI, TEXT("OceanButterflyRDG")));
			FRDGTextureRef NoiseRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(NoiseTextureRHI, TEXT("OceanNoiseRDG")));

			FIntPoint GridExtent(LocalGridSize, LocalGridSize);
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(GridExtent, FIntPoint(OCEAN_FFT_THREADGROUP_SIZE, OCEAN_FFT_THREADGROUP_SIZE));

			TShaderMapRef<FOceanInitSpectrumCS> InitCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			TShaderMapRef<FOceanTimeSpectrumCS> TimeCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			TShaderMapRef<FOceanFFTButterflyCS> ButterflyCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			TShaderMapRef<FOceanInversionCS> InversionCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			for (const FCascadeRenderInfo& Info : RenderInfos)
			{
				if (!Info.OutputRHI)
				{
					continue;
				}

				FRDGTextureRef H0RDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Info.H0RHI, TEXT("OceanH0RDG")));
				FRDGTextureRef HeightOutRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Info.OutputRHI, TEXT("OceanHeightOutRDG")));

				FRDGTextureDesc PingPongDesc = FRDGTextureDesc::Create2D(
					GridExtent, PF_G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
				FRDGTextureRef HeightPingA = GraphBuilder.CreateTexture(PingPongDesc, TEXT("OceanFFTHeightPingA"));
				FRDGTextureRef HeightPingB = GraphBuilder.CreateTexture(PingPongDesc, TEXT("OceanFFTHeightPingB"));
				FRDGTextureRef DxPingA = GraphBuilder.CreateTexture(PingPongDesc, TEXT("OceanFFTDxPingA"));
				FRDGTextureRef DxPingB = GraphBuilder.CreateTexture(PingPongDesc, TEXT("OceanFFTDxPingB"));
				FRDGTextureRef DyPingA = GraphBuilder.CreateTexture(PingPongDesc, TEXT("OceanFFTDyPingA"));
				FRDGTextureRef DyPingB = GraphBuilder.CreateTexture(PingPongDesc, TEXT("OceanFFTDyPingB"));

				if (Info.bNeedsInit)
				{
					FOceanInitSpectrumCS::FParameters* InitParams = GraphBuilder.AllocParameters<FOceanInitSpectrumCS::FParameters>();
					InitParams->GridSize = LocalGridSize;
					InitParams->PatchLength = Info.PatchLength;
					InitParams->WindSpeed = LocalWindSpeed;
					InitParams->WindDirection = LocalWindDir;
					InitParams->Amplitude = LocalAmplitude;
					InitParams->SuppressSmallWaves = LocalSuppress;
					InitParams->NoiseTexture = NoiseRDG;
					InitParams->H0Output = GraphBuilder.CreateUAV(H0RDG);
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanInitSpectrum"), InitCS, InitParams, GroupCount);
				}

				FOceanTimeSpectrumCS::FParameters* TimeParams = GraphBuilder.AllocParameters<FOceanTimeSpectrumCS::FParameters>();
				TimeParams->GridSize = LocalGridSize;
				TimeParams->PatchLength = Info.PatchLength;
				TimeParams->Time = LocalTime;
				TimeParams->H0Texture = H0RDG;
				TimeParams->HktOutput = GraphBuilder.CreateUAV(HeightPingA);
				TimeParams->DxOutput = GraphBuilder.CreateUAV(DxPingA);
				TimeParams->DyOutput = GraphBuilder.CreateUAV(DyPingA);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanTimeSpectrum"), TimeCS, TimeParams, GroupCount);

				auto RunFFT = [&](FRDGTextureRef PingA, FRDGTextureRef PingB, const TCHAR* EventName) -> FRDGTextureRef
				{
					FRDGTextureRef CurrentInput = PingA;
					FRDGTextureRef CurrentOutput = PingB;
					for (int32 Stage = 0; Stage < LocalLogGridSize; Stage++)
					{
						FOceanFFTButterflyCS::FParameters* Params = GraphBuilder.AllocParameters<FOceanFFTButterflyCS::FParameters>();
						Params->GridSize = LocalGridSize;
						Params->Stage = Stage;
						Params->Direction = 0;
						Params->ButterflyTexture = ButterflyRDG;
						Params->InputTexture = CurrentInput;
						Params->OutputTexture = GraphBuilder.CreateUAV(CurrentOutput);
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%s_H", EventName), ButterflyCS, Params, GroupCount);
						::Swap(CurrentInput, CurrentOutput);
					}
					for (int32 Stage = 0; Stage < LocalLogGridSize; Stage++)
					{
						FOceanFFTButterflyCS::FParameters* Params = GraphBuilder.AllocParameters<FOceanFFTButterflyCS::FParameters>();
						Params->GridSize = LocalGridSize;
						Params->Stage = Stage;
						Params->Direction = 1;
						Params->ButterflyTexture = ButterflyRDG;
						Params->InputTexture = CurrentInput;
						Params->OutputTexture = GraphBuilder.CreateUAV(CurrentOutput);
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%s_V", EventName), ButterflyCS, Params, GroupCount);
						::Swap(CurrentInput, CurrentOutput);
					}
					return CurrentInput;
				};

				FRDGTextureRef HeightResult = RunFFT(HeightPingA, HeightPingB, TEXT("OceanFFTHeight"));
				FRDGTextureRef DxResult = RunFFT(DxPingA, DxPingB, TEXT("OceanFFTDx"));
				FRDGTextureRef DyResult = RunFFT(DyPingA, DyPingB, TEXT("OceanFFTDy"));

				FOceanInversionCS::FParameters* InvParams = GraphBuilder.AllocParameters<FOceanInversionCS::FParameters>();
				InvParams->GridSize = LocalGridSize;
				InvParams->NormalizationScale = 1.0f / (float)(LocalGridSize * LocalGridSize);
				InvParams->HeightFFTResult = HeightResult;
				InvParams->DxFFTResult = DxResult;
				InvParams->DyFFTResult = DyResult;
				InvParams->HeightOutput = GraphBuilder.CreateUAV(HeightOutRDG);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("OceanInversion"), InversionCS, InvParams, GroupCount);
			}

			GraphBuilder.Execute();
		});
}
