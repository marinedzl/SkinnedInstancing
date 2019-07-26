#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "InstancedSkinnedMeshComponent.generated.h"

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
	virtual void PostLoad() override;
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

public:

	/** The skeletal mesh used by this component. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mesh")
	class USkeletalMesh* SkeletalMesh;

	/** Object responsible for sending bone transforms, morph target state etc. to render thread. */
	class FInstancedSkinnedMeshObject* MeshObject;

private:
	friend class FInstancedSkinnedMeshSceneProxy;
};
