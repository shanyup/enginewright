#include "EWTestSaveGame.h"

#include "Kismet/GameplayStatics.h"
#include "PlatformFeatures.h"
#include "SaveGameSystem.h"

bool UEWTestSaveGame::SaveWithFooter()
{
	if (SlotName.IsEmpty()) { return false; }
	TArray<uint8> Bytes;
	if (!UGameplayStatics::SaveGameToMemory(this, Bytes)) { return false; }
	const uint32 Footer = (uint32)(Value * 31 + 7);
	Bytes.Append(reinterpret_cast<const uint8*>(&Footer), sizeof(Footer));
	ISaveGameSystem* SGS = IPlatformFeaturesModule::Get().GetSaveGameSystem();
	return SGS && SGS->SaveGame(false, *SlotName, 0, Bytes);
}

bool UEWTestSaveGame::SaveStatic(UEWTestSaveGame* Save)
{
	return Save && Save->SaveWithFooter();
}
