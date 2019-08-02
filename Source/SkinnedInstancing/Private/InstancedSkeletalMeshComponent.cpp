#include "Components/InstancedSkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Components/InstancedSkinnedMeshComponent.h"

#pragma optimize( "", off )

class UInstancedSkeletalMeshComponent::FAnimtionPlayer
{
public:
	struct Sequence
	{
		int Id;
		float Time;
		float Length;
		int NumFrames;

		Sequence() : Id(-1), Time(0), Length(0), NumFrames(0) {}
		Sequence(int Id, float Length, int NumFrames) : Id(Id), Time(0), Length(Length), NumFrames(NumFrames) {}

		void Tick(float DeltaTime, bool Loop = false)
		{
			Time += DeltaTime;
			if (Loop)
				Time = FMath::Fmod(Time, Length);
			else
				Time = FMath::Max(Time, Length);
		}
	};

public:
	const Sequence& GetCurrentSeq() const { return CurrentSeq; }
	const Sequence& GetNextSeq() const { return NextSeq; }
	float GetFadeTime() const { return FadeTime; }
	float GetFadeLength() const { return FadeLength; }

public:
	void Tick(float DeltaTime)
	{
		CurrentSeq.Tick(DeltaTime, Loop);

		if (FadeTime > 0 && FadeLength > 0)
		{
			FadeTime = FMath::Max(FadeTime - DeltaTime, 0.0f);

			NextSeq.Tick(DeltaTime);

			if (FadeTime <= 0)
			{
				CurrentSeq = NextSeq;
			}
		}
	}

	void Play(const Sequence& Seq, bool Loop)
	{
		this->Loop = Loop;
		CurrentSeq = NextSeq = Seq;
		FadeLength = FadeTime = 0;
	}

	void CrossFade(const Sequence& Seq, bool Loop, float Fade)
	{
		if (CurrentSeq.Id < 0)
		{
			Play(Seq, Loop);
			return;
		}
		NextSeq = Seq;
		FadeLength = FadeTime = Fade;
	}

private:
	bool Loop = false;
	float FadeLength = 0;

private:
	Sequence CurrentSeq;
	Sequence NextSeq;
	float FadeTime = 0;
};

namespace
{
	void GetInstanceDataFromPlayer(FInstancedSkinnedMeshInstanceData::FAnimData& Data, 
		const UInstancedSkeletalMeshComponent::FAnimtionPlayer::Sequence& Seq)
	{
		int NumFrames = Seq.NumFrames;
		float SequenceLength = Seq.Length;
		float Interval = (NumFrames > 1) ? (SequenceLength / (NumFrames - 1)) : MINIMUM_ANIMATION_LENGTH;

		float Time = Seq.Time;
		int Frame = Time / Interval;
		float Lerp = Time - Frame * Interval;

		Data.Sequence = Seq.Id;
		Data.PrevFrame = FMath::Clamp(Frame, 0, NumFrames - 1);
		Data.NextFrame = FMath::Clamp(Frame + 1, 0, NumFrames - 1);
		Data.FrameLerp = FMath::Clamp(Lerp, 0.0f, 1.0f);
	}
}

UInstancedSkeletalMeshComponent::UInstancedSkeletalMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	InstanceManagerObject = nullptr;
	InstanceId = -1;

	AnimtionPlayer = new FAnimtionPlayer();
}

UInstancedSkeletalMeshComponent::~UInstancedSkeletalMeshComponent()
{
	delete AnimtionPlayer;
}

void UInstancedSkeletalMeshComponent::CrossFade(int Sequence, float FadeLength)
{
	UInstancedSkinnedMeshComponent* InstanceManager = Cast<UInstancedSkinnedMeshComponent>(
		InstanceManagerObject->GetComponentByClass(UInstancedSkinnedMeshComponent::StaticClass())
		);
	if (InstanceManager)
	{
		UAnimSequence* AnimSequence = InstanceManager->GetSequence(Sequence);
		int NumFrames = AnimSequence->GetNumberOfFrames();
		check(AnimSequence);
		FAnimtionPlayer::Sequence Seq(Sequence, AnimSequence->SequenceLength, NumFrames);
		AnimtionPlayer->CrossFade(Seq, true, FadeLength);
	}
}

void UInstancedSkeletalMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction * ThisTickFunction)
{
	// Tick ActorComponent first.
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AnimtionPlayer->Tick(DeltaTime);

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

				GetInstanceDataFromPlayer(Instance.AnimDatas[0], AnimtionPlayer->GetCurrentSeq());
				GetInstanceDataFromPlayer(Instance.AnimDatas[1], AnimtionPlayer->GetNextSeq());

				float BlendWeight = 1;

				if (AnimtionPlayer->GetCurrentSeq().Id != AnimtionPlayer->GetNextSeq().Id)
				{
					float FadeTime = AnimtionPlayer->GetFadeTime();
					float FadeLength = FMath::Max(AnimtionPlayer->GetFadeLength(), 0.001f);
					BlendWeight = FadeTime / FadeLength;
				}

				Instance.AnimDatas[0].BlendWeight = BlendWeight;
				Instance.AnimDatas[1].BlendWeight = 1 - BlendWeight;
			}
		}
	}
}

#pragma optimize( "", on )
