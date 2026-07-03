#include "Attributes/GMCAttributeClamp.h"

#include "GMCAbilityComponent.h"

bool FAttributeClamp::IsSet() const
{
	return bClampMin || bClampMax;
}

float FAttributeClamp::ClampValue(float Value) const
{
	// Neither bound is active — return Value untouched.
	if (!bClampMin && !bClampMax) { return Value; }

	// Migration compat: the pre-fork clamp had no bClampMin/bClampMax — clamping was implied by
	// non-zero bounds or bound tags. Attribute assets saved under that scheme load the new bools
	// at their class default (true) alongside their old values, so a pre-fork "unset" clamp
	// (Min == 0, Max == 0, no tags) would crush every value into [0,0]. Treat that exact shape —
	// which is also the default-constructed shape — as unset, matching pre-fork behavior. A
	// deliberate single-bound clamp at 0 (one flag unchecked) is not this shape and still applies.
	if (bClampMin && bClampMax &&
		Min == 0.f && Max == 0.f && !MinAttributeTag.IsValid() && !MaxAttributeTag.IsValid())
	{
		return Value;
	}

	float Result = Value;

	if (bClampMin)
	{
		// MinAttributeTag takes priority over the literal Min when an
		// AbilityComponent is available to resolve it.
		float MinBound = Min;
		if (AbilityComponent && MinAttributeTag.IsValid())
		{
			MinBound = AbilityComponent->GetAttributeValueByTag(MinAttributeTag);
		}
		Result = FMath::Max(Result, MinBound);
	}

	if (bClampMax)
	{
		float MaxBound = Max;
		if (AbilityComponent && MaxAttributeTag.IsValid())
		{
			MaxBound = AbilityComponent->GetAttributeValueByTag(MaxAttributeTag);
		}
		Result = FMath::Min(Result, MaxBound);
	}

	return Result;
}
