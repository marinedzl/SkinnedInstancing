#include "SIAnimationComponent.h"

#pragma optimize( "", off )

USIAnimationComponent::USIAnimationComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

USIAnimationComponent::~USIAnimationComponent()
{
}

void USIAnimationComponent::BeginPlay()
{
	Super::BeginPlay();
}

void USIAnimationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void USIAnimationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction * ThisTickFunction)
{
	// Tick ActorComponent first.
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

#pragma optimize( "", on )
