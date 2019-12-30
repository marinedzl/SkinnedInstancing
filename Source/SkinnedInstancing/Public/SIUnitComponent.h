#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SIUnitComponent.generated.h"

UCLASS(hidecategories = (Object, LOD), meta = (BlueprintSpawnableComponent), ClassGroup = Rendering)
class SKINNEDINSTANCING_API USIUnitComponent : public USceneComponent
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkinnedInstancing")
	TWeakObjectPtr<USIMeshComponent> MeshComponent;

public:
	virtual ~USIUnitComponent();

	//~ Override Functions
protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	//~ End UActorComponent Interface

public:
	UFUNCTION(BlueprintCallable, Category = "Components|SkinnedInstancing")
	void CrossFade(int Sequence, float FadeLength, bool Loop);

public:
	class FAnimtionPlayer;

private:
	int InstanceId;
	FAnimtionPlayer* AnimtionPlayer;
};
