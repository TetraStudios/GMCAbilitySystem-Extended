#pragma once

#include "StructUtils/InstancedStruct.h"
#include "GMASBoundQueueV2_Operations.h"
#include "GMCMovementUtilityComponent.h"
#include "GMASBoundQueueV2.generated.h"

class UGMC_MovementUtilityCmp;
class UGMCMovementComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnServerOperationAdded, int, OperationID, FInstancedStruct, OperationData);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnServerOperationForced, FInstancedStruct, OperationData);


USTRUCT()
struct FOperationDataCacheExpiration
{
	GENERATED_BODY()
	
	// Operation ID
	int OperationID = -1;

	// The GMC move # (GMCMoveCounter) when this operation was added
	int64 ModeAddedAt = -1;
};

USTRUCT()
struct  FGMASBoundQueueV2
{
	GENERATED_BODY()
	// Events
	FOnServerOperationAdded OnServerOperationAdded;
	FOnServerOperationForced OnServerOperationForced;

	UPROPERTY()
	UGMC_MovementUtilityCmp* GMCMovementComponent = nullptr;

	// Every time a GMC move is processed, this counter is incremented
	// Used to expire stale operation data
	int64 GMCMoveCounter = 0;
	TArray<FOperationDataCacheExpiration> OperationDataCacheExpiration;

	void ClearStaleOperationData();

	// Idempotency for ability activation operations. Once an OperationID has been
	// dispatched to TryActivateAbilitiesByInputTag, ProcessOperation drops every
	// subsequent re-encounter of that ID until the marker ages out via
	// ClearStaleProcessedOperations. Closes two re-feed paths that otherwise
	// re-activate an ability whose Ended instance has already been reaped by
	// CleanupStaleAbilities, briefly bringing it back to life right after the
	// player's release path ended it:
	//   - Client replay (CL_ReplayMoves re-runs GenPredictionTick on saved moves)
	//   - Server SV_GetLastClientData() returning the same OutputState across
	//     consecutive ticks before the next remote move arrives
	// Effect / Impulse / Location ops are scoped out — those carry their own
	// idempotency through ProcessEffectApplicationFromOperation's
	// ActiveEffects.Contains check and similar.
	TMap<int, int64> ProcessedOperations;

	// Independent tick counter for ProcessedOperations expiration. The pre-existing
	// GMCMoveCounter only increments on remote clients (the NM_Client gate in
	// GenAncillaryTick), but the idempotency markers need expiration on every role
	// — server, listen-server host, and standalone — so we track our own clock here.
	int64 ProcessedOpTickCounter = 0;

	void MarkOperationProcessed(int OperationID)
	{
		ProcessedOperations.Add(OperationID, ProcessedOpTickCounter);
	}

	bool IsOperationProcessed(int OperationID) const
	{
		return ProcessedOperations.Contains(OperationID);
	}

	void ClearStaleProcessedOperations();

	int NextOperationID = 0;
	
	// Get the next operation ID
	// Any positive ID is a server generated operation
	// Any negative ID is a client generated operation
	int GetNextOperationID()
	{
		if (GMCMovementComponent->GetNetMode() != NM_Client)
		{
			return ++NextOperationID;
		}
		return --NextOperationID;
	}

	// OperationID, OperationPayload
protected:
	TMap<int, FInstancedStruct> OperationPayloads;

public:
	// Accessors for OperationPayloads
	
	void RemovePayloadByID(const int OperationID)
	{
		if (OperationPayloads.Contains(OperationID))
		{
			OperationPayloads.Remove(OperationID);
		}

		OperationDataCacheExpiration.RemoveAll([OperationID](const FOperationDataCacheExpiration& Expiration)
		{
			return Expiration.OperationID == OperationID;
		});
	}

	// Make a GetOperationByID
	FInstancedStruct GetPayloadByID(const int OperationID)
	{
		if (OperationPayloads.Contains(OperationID))
		{
			return OperationPayloads[OperationID];
		}
		return FInstancedStruct();
	}

	bool HasPayloadByID(const int OperationID) const
	{
		return OperationPayloads.Contains(OperationID);
	}

	int GetPayloadCount() const
	{
		return OperationPayloads.Num();
	}

	
	TArray<int> OperationQueue;
	
	// GMC
	void BindToGMC(UGMC_MovementUtilityCmp* MovementComponent);
	void GenPreLocalMoveExecution();
	void GenAncillaryTick(float DeltaTime);
	
	//// GMC Bound
	//
	// Operation Data used to actually process the operation
	// These are only ever sent via RPC from the server to the client then cached
	// GMC moves will access the caches instead of storing it all in moves
	int BI_OperationData;
	FInstancedStruct OperationData;
	//// End GMC Bound

	void CacheOperationPayload(const int OperationID, const FInstancedStruct& Payload);
	
	// Wrappers for building instanced structs for each data type
	// Adds the Operation ID to the data and stores the payload in OperationPayloads
	template <typename T>
	int MakeOperationData(const T& Data)
	{
		static_assert(TIsDerivedFrom<T, FGMASBoundQueueV2OperationBaseData>::IsDerived, "T must be derived from FGMASBoundQueueV2BaseData");

		T BuiltData = Data;
		BuiltData.OperationID = GetNextOperationID();
		
		FInstancedStruct OutStruct;
		OutStruct.InitializeAs<T>(BuiltData);

		// Add to payload cache to reference it later
		CacheOperationPayload(BuiltData.OperationID, OutStruct);
		
		return BuiltData.OperationID;
	}

	// Operation types that the client can supply for the server to run
	// Treat these as unsafe client-supplied data
	TArray<UScriptStruct*> ValidClientInputOperationTypes = {
		FGMASBoundQueueV2AbilityActivationOperation::StaticStruct(),
		FGMASBoundQueueV2AcknowledgeOperation::StaticStruct()
	};

	bool IsValidGMASOperation(const FInstancedStruct& Data) const;
	
	bool IsValidClientOperation(const FInstancedStruct& Data) const;

	// Queue a Client operation
	void QueueClientOperation(const int OperationID);

	// Queue a server-auth operation received by the client. These are local client
	// mirror ops and must not share the outbound client operation stream.
	void QueueServerMirrorOperation(const int OperationID);
	bool PopNextServerMirrorOperation(FInstancedStruct& OutOperation);

	// Queue an acknowledgement for a server-auth mirror op processed on the client.
	void QueueAcknowledgement(const int OperationID);

	// Queue a ServerAuth operation
	void QueueServerOperation(const int OperationID, const float Timeout = 1.0f);
	
	bool CurrentOperationIsOfType(const UScriptStruct* T) const
	{
		return OperationData.GetScriptStruct() == T;
	}

	// Process a server operation that the client has sent an ack for
	void ServerAcknowledgeOperation(int ID);
	
	// Operations (referenced by ID to OperationPayloads) that the Client has queued
	// Key: Operation Id
	TArray<int> ClientQueuedOperations;

	// Server-auth operations received by the client through RPC and mirrored locally.
	TArray<int> ServerMirrorQueuedOperations;

	// ACKs waiting to be emitted through the client-auth bound OperationData slot.
	TArray<int> PendingAckOperations;

	// Operations that the server has sent to the client but haven't been acknowledged yet
	// If the client doesn't acknowledge the operation in time, the server will force it
	// Map: OperationId -> GracePeriod
	TMap<int, float> ServerQueuedBoundOperationsGracePeriods;

	// Runs checks on the current state of the queue and logs any issues found
	void CheckValidState() const;
};

// Operations
#pragma region Operations

#pragma endregion Operations
