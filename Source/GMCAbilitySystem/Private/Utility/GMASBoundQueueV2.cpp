#include "Utility/GMASBoundQueueV2.h"

#include <Utility/GMASBoundQueueV2.h>

#include "GMCAbilitySystem.h"
#include "GMCMovementUtilityComponent.h"


bool FGMASBoundQueueV2::IsValidGMASOperation(const FInstancedStruct& Data) const
{
	// Check if the operation is a valid type
	if (!OperationData.IsValid()) return false;

	const FGMASBoundQueueV2OperationBaseData* BaseData = OperationData.GetPtr<FGMASBoundQueueV2OperationBaseData>();
	if (!BaseData)
	{
		UE_LOG(LogGMCAbilitySystem, Error, TEXT("OperationData is not a valid type"));
		return false;
	}

	return true;
}

bool FGMASBoundQueueV2::IsValidClientOperation(const FInstancedStruct& Data) const
{
	if (Data.IsValid()) 
	{
		const UScriptStruct* StructType = Data.GetScriptStruct();
		if (ValidClientInputOperationTypes.Contains(StructType))
		{
			return true;
		}
		UE_LOG(LogGMCAbilitySystem, Error, TEXT("Client operation type %s is not allowed by server as client input"), *StructType->GetName());
	}
	return false;
}

void FGMASBoundQueueV2::ClearStaleOperationData()
{
	const int MaximumFreshMoveIndex = GMCMoveCounter - GMCMovementComponent->MoveHistoryMaxSize;
	for (auto It = OperationDataCacheExpiration.CreateIterator(); It; ++It)
	{
		const int64 MoveAddedAt = It->ModeAddedAt;
		if (MoveAddedAt < MaximumFreshMoveIndex)
		{
			const int OperationID = It->OperationID;
			if (!OperationPayloads.Contains(OperationID))
			{
				UE_LOG(LogGMCAbilitySystem, Warning, TEXT("OperationID %d not found in OperationPayloads, but still in cache expiration map"), OperationID);
				continue;
			}
			// Remove stale operation data
			OperationPayloads.Remove(OperationID);
			It.RemoveCurrent();
		}
	}
}

void FGMASBoundQueueV2::BindToGMC(UGMC_MovementUtilityCmp* MovementComponent)
{
	OperationData = FInstancedStruct::Make<FGMASBoundQueueV2OperationBaseData>();
	
	BI_OperationData = MovementComponent->BindInstancedStruct(
		OperationData,
		EGMC_PredictionMode::ClientAuth_InputOutput,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::TargetValue);
	
	GMCMovementComponent = MovementComponent;
}

void FGMASBoundQueueV2::GenPreLocalMoveExecution()
{
	// Only locally-controlled "client-like" pawns drive this path; remote pawns on
	// the server have their bound variable populated by GMC's replication layer.
	if (GMCMovementComponent->GetNetMode() != NM_Client &&
		GMCMovementComponent->GetNetMode() != NM_Standalone &&
		!GMCMovementComponent->IsLocallyControlledListenServerPawn() &&
		!GMCMovementComponent->IsLocallyControlledDedicatedServerPawn())
	{
		return;
	}

	if (ClientQueuedOperations.Num() > 0)
	{
		const int OperationIDToProcess = ClientQueuedOperations.Pop();
		if (OperationPayloads.Contains(OperationIDToProcess))
		{
			// Replicate the full derived payload (e.g. FGMASBoundQueueV2AbilityActivationOperation
			// with InputTag) so that ServerProcessOperation->IsValidClientOperation passes
			// on the receiving end. Sending only the base struct (just OperationID) causes
			// IsValidClientOperation to return false and the ability is never run server-side.
			OperationData = OperationPayloads[OperationIDToProcess];
			return;
		}
	}

	// On a remote client (NM_Client), ProcessOperation does NOT remove the payload
	// from the cache (the HasAuthority gate at GMCAbilityComponent.cpp:~1696 skips
	// it), so cache presence is not a reliable "consumed" signal — the op lingers
	// until ClearStaleOperationData ages it out. Always clobber here to avoid the
	// bound variable re-replicating the same activation op every frame, which
	// would cause the server to re-activate the ability immediately after a task
	// progress signal (e.g. WaitForInputKeyRelease) had ended it.
	if (GMCMovementComponent->GetNetMode() == NM_Client)
	{
		OperationData = FInstancedStruct::Make<FGMASBoundQueueV2OperationBaseData>();
		return;
	}

	// Authority side (host on listen server, AI on dedicated, standalone): only
	// clear OperationData if its current op has already been consumed (no longer
	// in cache). Otherwise leave it intact so a deferred op — one ProcessOperation
	// rejected on tick mismatch — survives until the matching tick consumes it.
	// Without this guard, a tick-mismatched op gets silently dropped because
	// ProcessOperation's bActivateOnMovementTick check returns false but the cache
	// has already been cleared.
	const FGMASBoundQueueV2OperationBaseData* CurrentBaseData =
		OperationData.GetPtr<FGMASBoundQueueV2OperationBaseData>();
	const int CurrentOpID = CurrentBaseData ? CurrentBaseData->OperationID : 0;
	if (CurrentOpID == 0 || !OperationPayloads.Contains(CurrentOpID))
	{
		OperationData = FInstancedStruct::Make<FGMASBoundQueueV2OperationBaseData>();
	}
}

void FGMASBoundQueueV2::GenAncillaryTick(const float DeltaTime)
{
	CheckValidState();

	if (GMCMovementComponent->GetNetMode() >= NM_Client)
	{
		GMCMoveCounter++;
		ClearStaleOperationData();
	}
	
	// Tick all Server Queued Operations
	for (auto It = ServerQueuedBoundOperationsGracePeriods.CreateIterator(); It; ++It)
	{
		It.Value() -= DeltaTime;
		
		if (It.Value() <= 0) 
		{
			if (OperationPayloads.Contains(It.Key()))
			{
				OnServerOperationForced.Broadcast(OperationPayloads[It.Key()]);
				OperationPayloads.Remove(It.Key());
			}
			It.RemoveCurrent();
		}
	}
}

void FGMASBoundQueueV2::CacheOperationPayload(const int OperationID, const FInstancedStruct& Payload)
{
	OperationPayloads.Add(OperationID, Payload);
	OperationDataCacheExpiration.Add({OperationID, GMCMoveCounter});
}

void FGMASBoundQueueV2::QueueClientOperation(const int OperationID)
{
	ClientQueuedOperations.Add(OperationID);
}

void FGMASBoundQueueV2::QueueServerOperation(const int OperationID, const float Timeout)
{
	if (!OperationPayloads.Contains(OperationID))
	{
		UE_LOG(LogTemp, Error, TEXT("Tried to queue server operation, but server operation %d not found in payloads"), OperationID);
		return;
	}

	const FInstancedStruct QueuedOperation = OperationPayloads[OperationID];

	// Add to server timeout map
	ServerQueuedBoundOperationsGracePeriods.Add(OperationID, Timeout);

	// Notify
	OnServerOperationAdded.Broadcast(OperationID, QueuedOperation);
}

void FGMASBoundQueueV2::ServerAcknowledgeOperation(int ID)
{
	if (OperationPayloads.Contains(ID))
	{
		OperationPayloads.Remove(ID);
	}
	
	if (ServerQueuedBoundOperationsGracePeriods.Contains(ID))
	{
		ServerQueuedBoundOperationsGracePeriods.Remove(ID);
	}
}

void FGMASBoundQueueV2::CheckValidState() const
{
	// Server Logic
	if (GMCMovementComponent->GetNetMode() < NM_Client)
	{
		// Check Client Queued Operations is empty
		if (ClientQueuedOperations.Num() > 0)
		{
			UE_LOG(LogGMCAbilitySystem, Error, TEXT("ClientQueuedOperations has %d pending operations on server"), ClientQueuedOperations.Num());
		}

		// Negative IDs in OperationPayloads on the server are normal and transient:
		// ServerProcessOperation caches the client's submitted op before dispatching,
		// and ProcessOperation may defer dispatch to the matching tick (Movement vs
		// Ancillary) to satisfy the ability's bActivateOnMovementTick preference.
		// During that defer the negative ID lingers for at most a few ticks until
		// the matching tick consumes it; ClearStaleOperationData() handles anything
		// stuck longer than MoveHistoryMaxSize. Only warn about IDs that have aged
		// past the cache horizon, since those indicate a real leak.
		const int64 MaximumFreshMoveIndex = GMCMoveCounter - GMCMovementComponent->MoveHistoryMaxSize;
		for (const auto& Expiration : OperationDataCacheExpiration)
		{
			if (Expiration.OperationID < 0 && Expiration.ModeAddedAt < MaximumFreshMoveIndex)
			{
				UE_LOG(LogGMCAbilitySystem, Error,
					TEXT("OperationPayloads has stale client-side operation ID %d on server (added at move %lld, cutoff %lld)"),
					Expiration.OperationID, Expiration.ModeAddedAt, MaximumFreshMoveIndex);
			}
		}
	}
	else
	{
		// Check Server Queued Operations is empty
		if (ServerQueuedBoundOperationsGracePeriods.Num() > 0)
		{
			UE_LOG(LogGMCAbilitySystem, Error, TEXT("ServerQueuedBoundOperationsGracePeriods has %d pending operations on client"), ServerQueuedBoundOperationsGracePeriods.Num());;
		}
	}
}
