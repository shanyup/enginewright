#include "EnginewrightSettings.h"

FSimpleMulticastDelegate& UEnginewrightSettings::OnChanged()
{
	static FSimpleMulticastDelegate D;
	return D;
}

#if WITH_EDITOR
void UEnginewrightSettings::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	if (HasAnyFlags(RF_ClassDefaultObject)) { OnChanged().Broadcast(); }
}
#endif
