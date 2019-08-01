#include "Components/InstancedSkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Components/InstancedSkinnedMeshComponent.h"

#pragma optimize( "", off )

UInstancedSkeletalMeshComponent::UInstancedSkeletalMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	InstanceManagerObject = nullptr;
	InstanceId = -1;
	AnimTime = 0;
}

void UInstancedSkeletalMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction * ThisTickFunction)
{
	// Tick ActorComponent first.
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (InstanceManagerObject)
	{
		if (InstanceId < 0)
		{
			UInstancedSkinnedMeshComponent* InstanceManager = Cast<UInstancedSkinnedMeshComponent>(
				InstanceManagerObject->GetComponentByClass(UInstancedSkinnedMeshComponent::StaticClass())
				);
			if (InstanceManager)
			{
				InstanceId = InstanceManager->AddInstance(GetComponentTransform());
			}
		}

		if (InstanceId >= 0)
		{
			UInstancedSkinnedMeshComponent* InstanceManager = Cast<UInstancedSkinnedMeshComponent>(
				InstanceManagerObject->GetComponentByClass(UInstancedSkinnedMeshComponent::StaticClass())
				);
			if (InstanceManager)
			{
				FInstancedSkinnedMeshInstanceData& Instance = InstanceManager->PerInstanceSMData[InstanceId];
				Instance.Transform = GetComponentTransform().ToMatrixWithScale();

				UAnimSequence* AnimSequence = InstanceManager->AnimSequence1;
				if (AnimSequence)
				{
					int NumFrames = AnimSequence->GetNumberOfFrames();
					float SequenceLength = AnimSequence->SequenceLength;
					float Interval = (NumFrames > 1) ? (SequenceLength / (NumFrames - 1)) : MINIMUM_ANIMATION_LENGTH;

					AnimTime += DeltaTime;

					float Time = FMath::Fmod(AnimTime, SequenceLength);
					int Frame = Time / Interval;
					float Lerp = Time - Frame * Interval;

					Instance.AnimDatas[0].Sequence = 1;
					Instance.AnimDatas[0].PrevFrame = FMath::Clamp(Frame, 0, NumFrames - 1);
					Instance.AnimDatas[0].NextFrame = FMath::Clamp(Frame + 1, 0, NumFrames - 1);
					Instance.AnimDatas[0].FrameLerp = FMath::Clamp(Lerp, 0.0f, 1.0f);
					Instance.AnimDatas[0].BlendWeight = 1;

					Instance.AnimDatas[1].Sequence = 0;
					Instance.AnimDatas[1].PrevFrame = 0;
					Instance.AnimDatas[1].NextFrame = 0;
					Instance.AnimDatas[1].FrameLerp = 0;
					Instance.AnimDatas[1].BlendWeight = 0;
				}
			}
		}
	}
}

#pragma optimize( "", on )
