// Test listener for the component's OnInitialEffectApplied broadcast. Records whether
// the effect was already registered/queryable in ActiveEffects at broadcast time, and
// exercises the re-entrant "update the variables" pattern: a handler that calls the
// coalescing apply-or-update during the initial-apply broadcast must land on the
// applying instance instead of queueing a duplicate application.

#pragma once

#include "CoreMinimal.h"
#include "Components/GMCAbilityComponent.h"
#include "Effects/GMCAbilityEffect.h"
#include "UGMAS_TestEffectListener.generated.h"

UCLASS(NotBlueprintable, NotBlueprintType)
class UGMAS_TestEffectListener : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	UGMC_AbilitySystemComponent* Comp = nullptr;

	int32 BroadcastCount = 0;
	bool  bRegisteredAtBroadcast = false;
	bool  bReentrantCallSucceeded = false;
	int32 ReentrantOutId = -1;

	UFUNCTION()
	void OnInitialApply(UGMCAbilityEffect* AppliedEffect)
	{
		++BroadcastCount;
		if (!Comp || !AppliedEffect)
		{
			return;
		}
		bRegisteredAtBroadcast = Comp->GetActiveEffects().Contains(AppliedEffect->EffectData.EffectID);
		bReentrantCallSucceeded = Comp->QueueOrUpdateEffectByClass(
			AppliedEffect->GetClass(), AppliedEffect->EffectData, ReentrantOutId);
	}

	// Regression seam for the ProcessEffectApplicationFromOperation FindChecked crash.
	// Counts invocations so specs can assert the apply-broadcast path was exercised.
	int32 UntrackCallCount = 0;

	// Simulates a re-entrant BP handler / gameplay-tag listener that untracks an effect
	// mid-apply: fired on OnEffectApplied (broadcast from inside StartEffect, itself inside
	// ApplyAbilityEffect), it removes the just-added ProcessedEffectIDs entry -- the exact
	// production re-entrancy TickActiveEffects guards against. Lets a spec verify that
	// ProcessEffectApplicationFromOperation no longer FindChecked-asserts when the entry
	// vanishes before the auto-validate write.
	UFUNCTION()
	void OnApplied_UntrackProcessedEntry(UGMCAbilityEffect* AppliedEffect)
	{
		++UntrackCallCount;
#if WITH_AUTOMATION_WORKER
		if (Comp && AppliedEffect)
		{
			Comp->GetProcessedEffectIDsForTest().Remove(AppliedEffect->EffectData.EffectID);
		}
#endif
	}
};
