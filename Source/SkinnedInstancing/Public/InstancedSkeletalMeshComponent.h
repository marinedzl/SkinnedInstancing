#pragma once

#include "CoreMinimal.h"
#include "InstancedSkeletalMeshComponent.generated.h"

UCLASS(hidecategories = (Object, LOD), meta = (BlueprintSpawnableComponent), ClassGroup = Rendering)
class ENGINE_API UInstancedSkeletalMeshComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Instances")
	AActor* InstanceManagerObject;

	//~ Override Functions
protected:
	//~ Begin UActorComponent Interface
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	//~ End UActorComponent Interface

private:
	int InstanceId;
	float AnimTime;
};
