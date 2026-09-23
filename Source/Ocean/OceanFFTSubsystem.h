//Runs the FFT ocean sim (Tessendorf's method, SIGGRAPH course notes 2001). Builds the
//FFT butterfly/noise lookup data on CPU once, then dispatches spectrum+FFT+inversion
//compute shaders per cascade every frame into HeightRenderTargets, which the water
//material samples and sums together (big swells + medium waves + fine ripples).
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RHIResources.h"
#include "OceanFFTSubsystem.generated.h"

USTRUCT()
struct FOceanCascadeRuntime
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> HeightRenderTarget = nullptr;

	FTextureRHIRef H0TextureRHI;
	bool bSpectrumInitialized = false;
};

//The 3 built-in water looks. Each is just a bundle of wave params + optical params (see
//FWaterPresetSettings below). Switching preset doesnt touch any code, just the numbers.
UENUM(BlueprintType)
enum class EWaterPreset : uint8
{
	Ocean,
	Tropical,
	Murky
};

//Everything one water look needs - wave shape for the cascades, optical colour for the
//material. Plain data so adding a preset later is just adding a case, not new logic.
USTRUCT(BlueprintType)
struct FWaterPresetSettings
{
	GENERATED_BODY()

	//wave shapoe (FFT inputs)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Waves")
	float WindSpeed = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Waves")
	FVector2D WindDirection = FVector2D(1.0, 0.3);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Waves")
	float Amplitude = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Waves")
	float SuppressSmallWaves = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Optics")
	FLinearColor AbsorptionCoefficients = FLinearColor(0.0034f, 0.0006f, 0.00015f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Optics")
	FLinearColor ScatteringCoefficients = FLinearColor(0.0003f, 0.0004f, 0.0005f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Optics")
	float PhaseG = 0.1f;
};

UCLASS()
class WATERGRAPHICSV2_API UOceanFFTSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	//Index 0 = biggest/primary cascade (used for the debug plane).
	UPROPERTY(BlueprintReadOnly, Category = "Ocean")
	TArray<TObjectPtr<UTextureRenderTarget2D>> HeightRenderTargets;

	UPROPERTY(EditAnywhere, Category = "Ocean")
	int32 GridSize = 256;

	UPROPERTY(EditAnywhere, Category = "Ocean")
	TArray<float> CascadePatchLengths = { 2000.0f, 500.0f, 120.0f };

	UPROPERTY(EditAnywhere, Category = "Ocean")
	float WindSpeed = 600.0f;

	UPROPERTY(EditAnywhere, Category = "Ocean")
	FVector2D WindDirection = FVector2D(1.0, 0.3);

	UPROPERTY(EditAnywhere, Category = "Ocean")
	float Amplitude = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Ocean")
	float SuppressSmallWaves = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ocean|Preset")
	EWaterPreset ActiveWaterPreset = EWaterPreset::Ocean;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Ocean|Preset")
	void ApplyWaterPreset(EWaterPreset Preset);

	UFUNCTION(BlueprintPure, Category = "Ocean|Preset")
	static FWaterPresetSettings GetPresetSettings(EWaterPreset Preset);

private:
	void BuildButterflyData();
	void BuildNoiseData();
	void TryBindDebugPlane();

	int32 LogGridSize = 8;
	double AccumulatedTime = 0.0;
	bool bDebugPlaneBound = false;
	bool bWaterSurfaceBound = false;

	TWeakObjectPtr<class UMaterialInstanceDynamic> CachedWaterMID;
	int32 DiagnosticFrameCounter = 0;

	TArray<FVector4f> ButterflyCPUData;
	TArray<FVector4f> NoiseCPUData;

	FTextureRHIRef ButterflyTextureRHI;
	FTextureRHIRef NoiseTextureRHI;

	UPROPERTY()
	TArray<FOceanCascadeRuntime> Cascades;
};
