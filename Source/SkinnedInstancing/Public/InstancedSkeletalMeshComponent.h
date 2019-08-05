#pragma once

#include "CoreMinimal.h"
#include "InstancedSkeletalMeshComponent.generated.h"

UCLASS(hidecategories = (Object, LOD), meta = (BlueprintSpawnableComponent), ClassGroup = Rendering)
class ENGINE_API UInstancedSkeletalMeshComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Instances")
	TWeakObjectPtr<AActor> InstanceManagerObject;

public:
	virtual ~UInstancedSkeletalMeshComponent();

	//~ Override Functions
protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	//~ End UActorComponent Interface

public:
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedSkeletalMesh")
	void CrossFade(int Sequence, float FadeLength, bool Loop);

public:
	class FAnimtionPlayer;

private:
	int InstanceId;
	FAnimtionPlayer* AnimtionPlayer;
};
