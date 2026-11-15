#include "Tests/UGMAS_TestSelfEndAbility.h"

double UGMAS_TestSelfEndAbility::EndAtMoveTime = 0.0;
int32 UGMAS_TestSelfEndAbility::BeginCount = 0;
int32 UGMAS_TestSelfEndAbility::TickCount = 0;
int32 UGMAS_TestSelfEndAbility::ResumeCount = 0;
int32 UGMAS_TestSelfEndAbility::RebeginCount = 0;

void UGMAS_TestSelfEndAbility::BeginAbility()
{
	Super::BeginAbility();
	++BeginCount;
}

void UGMAS_TestSelfEndAbility::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (AbilityState == EAbilityState::Ended)
	{
		return;
	}
	++TickCount;
	if (OwnerAbilityComponent->ActionTimer >= EndAtMoveTime)
	{
		EndAbility();
	}
}
