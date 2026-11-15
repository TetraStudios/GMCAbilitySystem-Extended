// GMAS.Unit.ReplayResume: UGMCAbility::bReplayResumable.
//
// GMC replays rewind bound state but not ability instances. For a resumable ability the instance must
// exist on exactly the moves it existed on in the first run:
//   - ended itself, replay starts inside its life  -> re-created without BeginAbility, ticks, ends again
//   - live, replay starts before its activation    -> not ticked until that move, OnReplayRebegin on it
// The harness has no move loop, so the tests set ActionTimer themselves and use the replay test seam.

#include "Misc/AutomationTest.h"
#include "Components/GMCAbilityComponent.h"
#include "UGMAS_TestMovementCmp.h"
#include "UGMAS_TestSelfEndAbility.h"

#if WITH_AUTOMATION_WORKER

BEGIN_DEFINE_SPEC(FGMASReplayResumeSpec,
	"GMAS.Unit.ReplayResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	UGMAS_TestMovementCmp*       MoveCmp     = nullptr;
	UGMC_AbilitySystemComponent* AbilityComp = nullptr;

	// One prediction tick at a move timestamp: tick the instances, then drop the ended ones.
	void TickAt(double MoveTime)
	{
		AbilityComp->ActionTimer = MoveTime;
		AbilityComp->TickActiveAbilitiesForTest(0.1f);
		AbilityComp->CleanupStaleAbilitiesForTest();
	}
	bool Activate(int AbilityID, double MoveTime)
	{
		AbilityComp->ActionTimer = MoveTime;
		return AbilityComp->TryActivateAbility(UGMAS_TestSelfEndAbility::StaticClass(), nullptr, FGameplayTag(), true, AbilityID);
	}
	int32 LiveCount() { return AbilityComp->GetActiveAbilityCount(UGMAS_TestSelfEndAbility::StaticClass()); }

END_DEFINE_SPEC(FGMASReplayResumeSpec)

void FGMASReplayResumeSpec::Define()
{
	BeforeEach([this]()
	{
		MoveCmp = NewObject<UGMAS_TestMovementCmp>(GetTransientPackage());
		MoveCmp->AddToRoot();
		AbilityComp = NewObject<UGMC_AbilitySystemComponent>(GetTransientPackage());
		AbilityComp->AddToRoot();
		AbilityComp->GMCMovementComponent = MoveCmp;
		AbilityComp->BindReplicationData();

		UGMAS_TestSelfEndAbility::EndAtMoveTime = 1.3;
		UGMAS_TestSelfEndAbility::BeginCount = 0;
		UGMAS_TestSelfEndAbility::TickCount = 0;
		UGMAS_TestSelfEndAbility::ResumeCount = 0;
		UGMAS_TestSelfEndAbility::RebeginCount = 0;
	});

	AfterEach([this]()
	{
		AbilityComp->RemoveFromRoot();
		MoveCmp->RemoveFromRoot();
		AbilityComp = nullptr;
		MoveCmp = nullptr;
	});

	Describe("an instance that ended itself in its own Tick", [this]()
	{
		It("is re-created without BeginAbility when a replay starts inside its life, and ends again", [this]()
		{
			TestTrue("activated", Activate(7, 1.0));
			TickAt(1.0); TickAt(1.1); TickAt(1.2); TickAt(1.3);
			TestEqual("ended and removed after its last move", LiveCount(), 0);
			TestEqual("first run ticked four moves", UGMAS_TestSelfEndAbility::TickCount, 4);

			AbilityComp->bForceReplayingForTest = true;
			AbilityComp->ActionTimer = 1.2;
			AbilityComp->ResumeEndedAbilitiesForReplay(1.2);
			TestEqual("resumed", LiveCount(), 1);
			TestTrue("same ability id", AbilityComp->GetActiveAbilities().Contains(7));
			TestEqual("BeginAbility did not run again", UGMAS_TestSelfEndAbility::BeginCount, 1);
			TestEqual("resume hook ran once", UGMAS_TestSelfEndAbility::ResumeCount, 1);

			TickAt(1.2);
			TestEqual("still alive on the move before its end", LiveCount(), 1);
			TickAt(1.3);
			TestEqual("ended itself again on its end move", LiveCount(), 0);
			AbilityComp->bForceReplayingForTest = false;
		});

		It("is not re-created by a replay that starts after its end or on its activation move", [this]()
		{
			Activate(7, 1.0);
			TickAt(1.0); TickAt(1.1); TickAt(1.2); TickAt(1.3);

			AbilityComp->bForceReplayingForTest = true;
			AbilityComp->ResumeEndedAbilitiesForReplay(1.4);
			TestEqual("replay starts after the end", LiveCount(), 0);
			// The activation move re-delivers the activation operation, which creates the instance itself.
			AbilityComp->ResumeEndedAbilitiesForReplay(1.0);
			TestEqual("replay starts on the activation move", LiveCount(), 0);
			AbilityComp->bForceReplayingForTest = false;
		});

		It("is not re-created twice by one replay start", [this]()
		{
			Activate(7, 1.0);
			TickAt(1.0); TickAt(1.1); TickAt(1.2); TickAt(1.3);

			AbilityComp->bForceReplayingForTest = true;
			AbilityComp->ResumeEndedAbilitiesForReplay(1.1);
			AbilityComp->ResumeEndedAbilitiesForReplay(1.1);
			TestEqual("one instance", LiveCount(), 1);
			TestEqual("one resume", UGMAS_TestSelfEndAbility::ResumeCount, 1);
			AbilityComp->bForceReplayingForTest = false;
		});
	});

	Describe("an instance ended from outside its own Tick", [this]()
	{
		It("is never re-created: nothing in the replay would end it again", [this]()
		{
			Activate(7, 1.0);
			TickAt(1.0); TickAt(1.1);
			AbilityComp->GetActiveAbilities()[7]->EndAbility(); // the server's RPC, another ability, a cancel
			AbilityComp->CleanupStaleAbilitiesForTest();

			AbilityComp->bForceReplayingForTest = true;
			AbilityComp->ResumeEndedAbilitiesForReplay(1.05);
			TestEqual("not resumed", LiveCount(), 0);
			AbilityComp->bForceReplayingForTest = false;
		});
	});

	Describe("a live instance in a replay that starts before its activation", [this]()
	{
		It("is not ticked before its activation move, and is re-begun on that move only", [this]()
		{
			UGMAS_TestSelfEndAbility::EndAtMoveTime = 9.0;
			Activate(8, 2.0);
			TickAt(2.0);
			TestEqual("first run: one begin", UGMAS_TestSelfEndAbility::BeginCount, 1);
			TestEqual("first run: one tick", UGMAS_TestSelfEndAbility::TickCount, 1);

			AbilityComp->bForceReplayingForTest = true;
			TickAt(1.9);
			TestEqual("not ticked on a move before its activation", UGMAS_TestSelfEndAbility::TickCount, 1);

			TestTrue("the re-delivered activation is consumed by the live instance", Activate(8, 2.0));
			TestEqual("re-begun once", UGMAS_TestSelfEndAbility::RebeginCount, 1);
			TestEqual("BeginAbility itself did not run again", UGMAS_TestSelfEndAbility::BeginCount, 1);
			TestEqual("still one instance", LiveCount(), 1);
			TickAt(2.0);
			TestEqual("ticked from its activation move on", UGMAS_TestSelfEndAbility::TickCount, 2);

			TestFalse("a duplicate delivery on a later move is refused as before", Activate(8, 2.1));
			TestEqual("not re-begun again", UGMAS_TestSelfEndAbility::RebeginCount, 1);
			AbilityComp->bForceReplayingForTest = false;
		});

		It("does not refuse the re-delivered activation of an earlier, ended instance of its class", [this]()
		{
			// A chain: 7 lives 1.0 to 1.3, then 8 starts at 2.0 and is still live. A replay that starts
			// before 7's activation must get 7 back from its re-delivered activation.
			Activate(7, 1.0);
			TickAt(1.0); TickAt(1.1); TickAt(1.2); TickAt(1.3);
			UGMAS_TestSelfEndAbility::EndAtMoveTime = 9.0;
			Activate(8, 2.0);
			TickAt(2.0);
			TestFalse("outside a replay the single-instance rule still refuses", Activate(7, 1.0));

			AbilityComp->bForceReplayingForTest = true;
			TestTrue("re-delivered activation of the ended instance is accepted", Activate(7, 1.0));
			TestTrue("7 exists again", AbilityComp->GetActiveAbilities().Contains(7));
			TestTrue("8 is kept", AbilityComp->GetActiveAbilities().Contains(8));
			TestEqual("7 began afresh", UGMAS_TestSelfEndAbility::BeginCount, 3);
			const int32 TicksBefore = UGMAS_TestSelfEndAbility::TickCount;
			TickAt(1.0);
			TestEqual("only 7 ticks on its own moves", UGMAS_TestSelfEndAbility::TickCount, TicksBefore + 1);
			AbilityComp->bForceReplayingForTest = false;
		});

		It("is left alone outside a replay", [this]()
		{
			UGMAS_TestSelfEndAbility::EndAtMoveTime = 9.0;
			Activate(8, 2.0);
			TestFalse("duplicate delivery refused", Activate(8, 2.0));
			TestEqual("one begin", UGMAS_TestSelfEndAbility::BeginCount, 1);
			TestEqual("no re-begin", UGMAS_TestSelfEndAbility::RebeginCount, 0);
		});
	});
}

#endif // WITH_AUTOMATION_WORKER
