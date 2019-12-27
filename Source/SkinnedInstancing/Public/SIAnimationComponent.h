#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SIAnimationComponent.generated.h"

class FSIAnimationData;

UCLASS(hidecategories = (Object, LOD), meta = (BlueprintSpawnableComponent), ClassGroup = Rendering)
class SKINNEDINSTANCING_API USIAnimationComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

	/** The Skeleton used by this component. */
	UPROPERTY(Category = Mesh, AssetRegistrySearchable, VisibleAnywhere, BlueprintReadOnly)
	class USkeleton* Skeleton;

	/** The AnimSequence used by this component. */
	UPROPERTY(Category = Mesh, AssetRegistrySearchable, VisibleAnywhere, BlueprintReadOnly)
	TArray<class UAnimSequence*> AnimSequences;

public:
	virtual ~USIAnimationComponent();

	const FSIAnimationData* GetAnimationData() const { return AnimationData; }

	//~ Override Functions
protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void CreateRenderState_Concurrent() override;
	virtual void DestroyRenderState_Concurrent() override;
	virtual bool RequiresGameThreadEndOfFrameRecreate() const override { return false; }
	//~ End UActorComponent Interface

private:
	void CreateAnimationData();

private:
	FSIAnimationData* AnimationData;
};
