// Replay-resumable ability stub for GMAS.Unit.ReplayResume: ends itself inside its own Tick once the
// component's ActionTimer reaches EndAtMoveTime, and counts its begins, ticks and resumes.

#pragma once

#include "CoreMinimal.h"
#include "Ability/GMCAbility.h"
#include "UGMAS_TestSelfEndAbility.generated.h"

UCLASS(NotBlueprintable, NotBlueprintType)
class UGMAS_TestSelfEndAbility : public UGMCAbility
{
	GENERATED_BODY()

public:
	UGMAS_TestSelfEndAbility() { bReplayResumable = true; }

	// Per-test state, reset by the spec before each test.
	static double EndAtMoveTime;
	static int32 BeginCount;
	static int32 TickCount;
	static int32 ResumeCount;
	static int32 RebeginCount;

	virtual void BeginAbility() override;
	virtual void Tick(float DeltaTime) override;
	virtual void OnResumedForReplay() override { ++ResumeCount; }
	virtual void OnReplayRebegin() override { ++RebeginCount; }
};
