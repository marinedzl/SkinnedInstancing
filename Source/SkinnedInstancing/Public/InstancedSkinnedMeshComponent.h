#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "InstancedSkinnedMeshComponent.generated.h"

USTRUCT()
struct FInstancedSkinnedMeshInstanceData
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, Category = Instances)
	FMatrix Transform;

	int PrevFrame;
	int NextFrame;
	int FrameLerp;

	float Time;

	FInstancedSkinnedMeshInstanceData()
		: Transform(FMatrix::Identity)
		, PrevFrame(0)
		, NextFrame(0)
		, FrameLerp(0)
		, Time(0)
	{
	}

	friend FArchive& operator<<(FArchive& Ar, FInstancedSkinnedMeshInstanceData& InstanceData)
	{
		Ar << InstanceData.Transform;
		return Ar;
	}
};

UCLASS(hidecategories = (Object, LOD), meta = (BlueprintSpawnableComponent), ClassGroup = Rendering)
class ENGINE_API UInstancedSkinnedMeshComponent : public UMeshComponent
{
	GENERATED_UCLASS_BODY()
	
	//~ Override Functions
private:
	//~ Begin UPrimitiveComponent Interface.
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	//~ End UPrimitiveComponent Interface.

	//~ Begin UMeshComponent Interface.
	virtual int32 GetNumMaterials() const override;
	//~ End UMeshComponent Interface.

	//~ Begin UObject Interface
	//~ End UObject Interface.

	//~ Begin USceneComponent Interface.
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ Begin USceneComponent Interface.

protected:
	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void CreateRenderState_Concurrent() override;
	virtual void SendRenderDynamicData_Concurrent() override;
	virtual void DestroyRenderState_Concurrent() override;
	virtual bool RequiresGameThreadEndOfFrameRecreate() const override { return false; }
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	virtual UObject const* AdditionalStatObject() const override { return SkeletalMesh; }
	//~ End UActorComponent Interface

private:
	/** Internal version of AddInstance */
	int32 AddInstanceInternal(int32 InstanceIndex, FInstancedSkinnedMeshInstanceData* InNewInstanceData, const FTransform& Transform);

public:

	/** The skeletal mesh used by this component. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Instances")
	class USkeletalMesh* SkeletalMesh;

	/** The skeletal mesh used by this component. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Instances")
	class UAnimSequence* AnimSequence;

	/** Array of instances, bulk serialized. */
	UPROPERTY(EditAnywhere, SkipSerialization, DisplayName = "Instances", Category = Instances, meta = (MakeEditWidget = true, EditFixedOrder))
	TArray<FInstancedSkinnedMeshInstanceData> PerInstanceSMData;

	/** Object responsible for sending bone transforms, morph target state etc. to render thread. */
	class FInstancedSkinnedMeshObject* MeshObject;

public:
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedSkinMesh")
	virtual int32 AddInstance(const FTransform& Transform);

	UFUNCTION(BlueprintCallable, Category = "Components|InstancedSkinMesh")
	virtual void SetInstance(int32 Index, const FTransform& Transform, float Time);

private:
	friend class FInstancedSkinnedMeshSceneProxy;
};
