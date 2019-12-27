#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SIAnimationComponent.generated.h"

UCLASS(hidecategories = (Object, LOD), meta = (BlueprintSpawnableComponent), ClassGroup = Rendering)
class SKINNEDINSTANCING_API USIAnimationComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

public:
	virtual ~USIAnimationComponent();

	//~ Override Functions
protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	//~ End UActorComponent Interface
};
