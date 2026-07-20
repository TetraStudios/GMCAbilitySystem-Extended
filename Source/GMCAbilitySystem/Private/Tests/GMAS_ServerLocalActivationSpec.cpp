// Regression spec for server-local ability activation on pawns with no owning
// client connection (unpossessed / server-spawned actors, e.g. an ordnance actor
// the server activates an ability on before any possession).
//
// Root cause being pinned: QueueAbility on the authority routed EVERY activation
// through QueueServerOperation, whose ack round-trip assumes some machine's GMC
// move stream will echo the op back. For an unowned actor the Client RPC
// (RPCOnServerOperationAdded) self-executes on the server (UE runs server-invoked
// Client RPCs locally when there is no owning connection), stranding the op in the
// server's ClientQueuedOperations where nothing drains it: CheckValidState errors
// every ancillary tick forever and the ability only fires ~1s late via the
// grace-timeout force.
//
// Covered here:
//   1. ShouldBypassServerOperationQueue — the pure routing decision table.
//   2. Grace-expiry force dropping a stranded ClientQueuedOperations entry.
//   3. The direct-activation call the bypass performs (TryActivateAbilitiesByInputTag
//      with bForce=true, SourceOperationID=0): activates immediately from an
//      ancillary/event context and touches no bound-queue state.
//   4. WouldDrainClientQueueLocally — the drain-routing decision table that gates
//      CheckValidState's pending-client-ops error: self-draining machines
//      (standalone, listen host, server-controlled pawn) hold ops transiently by
//      design, so only a remote-controlled server pawn may treat them as invalid.
//   5. The transient self-drain window: an op parked by a self-executed Client RPC
//      and awaiting the next GenPreLocalMoveExecution pack must NOT raise the
//      CheckValidState error (the false positive that spammed standalone/listen
//      sessions once per ability activation).

#include "Misc/AutomationTest.h"
#include "NativeGameplayTags.h"
#include "Components/GMCAbilityComponent.h"
#include "UGMAS_TestMovementCmp.h"
#include "UGMAS_TestAbility.h"

#if WITH_AUTOMATION_WORKER

BEGIN_DEFINE_SPEC(FGMASServerLocalActivationSpec,
    "GMAS.Unit.ServerLocalActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

    UGMAS_TestMovementCmp*       MoveCmp     = nullptr;
    UGMC_AbilitySystemComponent* AbilityComp = nullptr;

    FGameplayTag AbilityTag;

    void SetupHarness();
    void TeardownHarness();

END_DEFINE_SPEC(FGMASServerLocalActivationSpec)

void FGMASServerLocalActivationSpec::SetupHarness()
{
    static FNativeGameplayTag STag(
        TEXT("GMCAbilitySystem"), TEXT("GMCAbilitySystem"),
        TEXT("GMAS.Test.ServerLocal.Ordnance"), TEXT("Server-local activation tag for GMAS tests"),
        ENativeGameplayTagToken::PRIVATE_USE_MACRO_INSTEAD);
    AbilityTag = STag.GetTag();

    MoveCmp = NewObject<UGMAS_TestMovementCmp>(GetTransientPackage());
    MoveCmp->AddToRoot();

    AbilityComp = NewObject<UGMC_AbilitySystemComponent>(GetTransientPackage());
    AbilityComp->AddToRoot();
    AbilityComp->GMCMovementComponent = MoveCmp;
    AbilityComp->BindReplicationData();
    // Positive ActionTimer so TryActivateAbility's fallback GenerateAbilityID()
    // (SourceOperationID == 0 path — the one the bypass uses) produces valid IDs.
    AbilityComp->ActionTimer = 1.0;

    GetMutableDefault<UGMAS_TestAbility>()->AbilityTag              = AbilityTag;
    GetMutableDefault<UGMAS_TestAbility>()->CooldownTime            = 0.f;
    GetMutableDefault<UGMAS_TestAbility>()->bAllowMultipleInstances = false;
    // The production scenario is a movement-tick ability activated from an
    // ancillary/event context — bForce=true must bypass the tick-context match
    // exactly like the grace-timeout force path does.
    GetMutableDefault<UGMAS_TestAbility>()->bActivateOnMovementTick = true;

    FAbilityMapData MapData;
    MapData.InputTag         = AbilityTag;
    MapData.Abilities        = { UGMAS_TestAbility::StaticClass() };
    MapData.bGrantedByDefault = true;
    AbilityComp->AddAbilityMapData(MapData);
}

void FGMASServerLocalActivationSpec::TeardownHarness()
{
    UGMCAbility* CDO = GetMutableDefault<UGMAS_TestAbility>();
    CDO->AbilityTag              = FGameplayTag();
    CDO->CooldownTime            = 0.f;
    CDO->bAllowMultipleInstances = false;
    CDO->ActivationRequiredTags  = FGameplayTagContainer();
    CDO->ActivationBlockedTags   = FGameplayTagContainer();
    CDO->BlockOtherAbility       = FGameplayTagContainer();
    CDO->BlockedByOtherAbility   = FGameplayTagContainer();
    CDO->CancelAbilitiesWithTag  = FGameplayTagContainer();
    CDO->bActivateOnMovementTick = true; // engine default

    if (AbilityComp)
    {
        AbilityComp->bForceAuthorityForTest = false;
        AbilityComp->RemoveFromRoot();
        AbilityComp = nullptr;
    }
    if (MoveCmp)
    {
        MoveCmp->RemoveFromRoot();
        MoveCmp = nullptr;
    }
}

void FGMASServerLocalActivationSpec::Define()
{
    // ── Routing decision table ───────────────────────────────────────────────
    Describe("ShouldBypassServerOperationQueue", [this]()
    {
        It("keeps the queue path on non-authority", [this]()
        {
            TestFalse(TEXT("Client must keep the client-op queue path"),
                UGMC_AbilitySystemComponent::ShouldBypassServerOperationQueue(false, false, false, false));
            TestFalse(TEXT("Non-authority on a networked server context must keep the queue path"),
                UGMC_AbilitySystemComponent::ShouldBypassServerOperationQueue(false, true, false, false));
        });

        It("keeps the queue path in standalone", [this]()
        {
            TestFalse(TEXT("Standalone (authority, not a networked server) drains its own queue"),
                UGMC_AbilitySystemComponent::ShouldBypassServerOperationQueue(true, false, false, false));
        });

        It("keeps the queue path for a locally controlled server pawn", [this]()
        {
            TestFalse(TEXT("Listen host / server-side AI pawn drains its own queue"),
                UGMC_AbilitySystemComponent::ShouldBypassServerOperationQueue(true, true, true, false));
        });

        It("keeps the queue path for a remotely controlled server pawn", [this]()
        {
            TestFalse(TEXT("Remote player pawn completes the RPC + move-stream ack round-trip"),
                UGMC_AbilitySystemComponent::ShouldBypassServerOperationQueue(true, true, false, true));
        });

        It("bypasses the queue for an authority pawn no machine's move stream will ack", [this]()
        {
            TestTrue(TEXT("Unpossessed / unowned server pawn must activate directly"),
                UGMC_AbilitySystemComponent::ShouldBypassServerOperationQueue(true, true, false, false));
        });
    });

    // ── Client-queue drain routing ───────────────────────────────────────────
    Describe("WouldDrainClientQueueLocally", [this]()
    {
        It("drains on an autonomous client", [this]()
        {
            TestTrue(TEXT("NM_Client drains its own queue"),
                FGMASBoundQueueV2::WouldDrainClientQueueLocally(true, false, false, false));
        });

        It("drains in standalone", [this]()
        {
            TestTrue(TEXT("NM_Standalone drains its own queue"),
                FGMASBoundQueueV2::WouldDrainClientQueueLocally(false, true, false, false));
        });

        It("drains for a locally controlled listen-host pawn", [this]()
        {
            TestTrue(TEXT("Listen-host pawn drains its own queue"),
                FGMASBoundQueueV2::WouldDrainClientQueueLocally(false, false, true, false));
        });

        It("drains for a locally controlled dedicated-server pawn", [this]()
        {
            TestTrue(TEXT("Server-controlled dedicated pawn drains its own queue"),
                FGMASBoundQueueV2::WouldDrainClientQueueLocally(false, false, false, true));
        });

        It("does not drain for a remote-controlled server pawn", [this]()
        {
            TestFalse(TEXT("Nothing on the server drains a remote pawn's client queue -- pending ops there are an invariant violation"),
                FGMASBoundQueueV2::WouldDrainClientQueueLocally(false, false, false, false));
        });
    });

    // ── Grace-expiry force cleanup ───────────────────────────────────────────
    Describe("Grace-expiry force", [this]()
    {
        BeforeEach([this]() { SetupHarness(); });
        AfterEach([this]() { TeardownHarness(); });

        It("drops a ClientQueuedOperations entry stranded by a self-executed Client RPC", [this]()
        {
            // The harness is NM_Standalone — a self-draining machine — so
            // CheckValidState stays SILENT about the transiently-parked entry
            // (see the "Transient self-drain window" spec below). No expected
            // error is registered: the automation framework's unexpected-error
            // rule guards that silence. The assertions below pin the force
            // path: it CLEANS UP the stranded entry so it cannot linger.

            FGMASBoundQueueV2& Q = AbilityComp->GetBoundQueueV2ForTest();

            FGMASBoundQueueV2AbilityActivationOperation Op;
            Op.InputTag = AbilityTag;
            const int OpID = Q.MakeOperationData<FGMASBoundQueueV2AbilityActivationOperation>(Op);

            Q.QueueServerOperation(OpID, 0.05f);
            // Unowned actor: UE executes the Client RPC locally on the server,
            // landing the op in the server's own ClientQueuedOperations (see
            // RPCOnServerOperationAdded_Implementation). Nothing drains it there.
            Q.ClientQueuedOperations.Add(OpID);

            Q.GenAncillaryTick(0.1f); // grace expires -> force

            TestFalse(TEXT("Forced op payload must be consumed"),
                Q.HasPayloadByID(OpID));
            TestEqual(TEXT("Grace-period entry must be consumed"),
                Q.ServerQueuedBoundOperationsGracePeriods.Num(), 0);
            TestEqual(TEXT("Stranded ClientQueuedOperations entry must be dropped by the force"),
                Q.ClientQueuedOperations.Num(), 0);
        });
    });

    // ── Transient self-drain window ──────────────────────────────────────────
    Describe("Transient self-drain window", [this]()
    {
        BeforeEach([this]() { SetupHarness(); });
        AfterEach([this]() { TeardownHarness(); });

        It("does not error while an op awaits the next local drain", [this]()
        {
            // The user-visible false positive: on a self-draining machine (this
            // harness runs NM_Standalone) QueueServerOperation's Client RPC lands
            // the op in our own ClientQueuedOperations, and CheckValidState used
            // to error in the one-move window before GenPreLocalMoveExecution
            // packs it. Deliberately NO AddExpectedError here: an error logged
            // during this test fails it via the framework's unexpected-error rule.
            FGMASBoundQueueV2& Q = AbilityComp->GetBoundQueueV2ForTest();

            FGMASBoundQueueV2AbilityActivationOperation Op;
            Op.InputTag = AbilityTag;
            const int OpID = Q.MakeOperationData<FGMASBoundQueueV2AbilityActivationOperation>(Op);

            Q.QueueServerOperation(OpID, 1.0f);
            Q.ClientQueuedOperations.Add(OpID); // self-executed Client RPC landing

            Q.GenAncillaryTick(0.01f); // grace NOT expired -> no force, just the state check

            TestEqual(TEXT("Op must still be queued for the next local drain"),
                Q.ClientQueuedOperations.Num(), 1);

            Q.GenPreLocalMoveExecution(); // the drain the window was waiting for

            TestEqual(TEXT("Local drain must consume the queued op"),
                Q.ClientQueuedOperations.Num(), 0);
            const FGMASBoundQueueV2OperationBaseData* Packed =
                Q.OperationData.GetPtr<FGMASBoundQueueV2OperationBaseData>();
            TestTrue(TEXT("Drained op must be packed into OperationData"),
                Packed != nullptr && Packed->OperationID == OpID);
        });
    });

    // ── Direct activation contract used by the bypass ────────────────────────
    Describe("Direct server-local activation", [this]()
    {
        BeforeEach([this]() { SetupHarness(); });
        AfterEach([this]() { TeardownHarness(); });

        It("activates immediately with bForce from ancillary context and leaves the bound queue untouched", [this]()
        {
            AbilityComp->bForceAuthorityForTest = true;

            const bool bActivated = AbilityComp->TryActivateAbilitiesByInputTag(
                AbilityTag, nullptr, /*bFromMovementTick =*/ false, /*bForce =*/ true, /*SourceOperationID =*/ 0);

            TestTrue(TEXT("Direct activation must succeed from ancillary context"), bActivated);
            TestEqual(TEXT("Exactly one instance must be active"),
                AbilityComp->GetActiveAbilityCount(UGMAS_TestAbility::StaticClass()), 1);

            FGMASBoundQueueV2& Q = AbilityComp->GetBoundQueueV2ForTest();
            TestEqual(TEXT("No payloads may be cached"), Q.GetPayloadCount(), 0);
            TestEqual(TEXT("No client-queued ops may exist"), Q.ClientQueuedOperations.Num(), 0);
            TestEqual(TEXT("No grace periods may exist"), Q.ServerQueuedBoundOperationsGracePeriods.Num(), 0);
        });
    });
}

#endif // WITH_AUTOMATION_WORKER
