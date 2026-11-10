// Test fixture for savegame_get / savegame_set (tests/test_savegame.py): a SaveGame that wraps its file like a game with a
// checksum does: the engine's bytes, then a 4-byte footer (Value * 31 + 7). A plain engine write would drop the footer, so
// savegame_set must refuse such a slot unless it is given the game's own save function (SaveWithFooter / SaveStatic).
// Copy this file and EWTestSaveGame.cpp into the test project's module (for example Source/EnginewrightTest/) and rebuild.
// test_savegame.py looks for /Script/EnginewrightTest.EWTestSaveGame and skips the checks when it is missing.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "EWTestSaveGame.generated.h"

UCLASS()
class UEWTestSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 Value = 0;
	UPROPERTY() FString Label;
	UPROPERTY() bool bFlag = false;
	/** The slot the game's own save writes to (a real game keeps this in a constant). */
	UPROPERTY() FString SlotName;

	/** The game's own save: engine bytes + footer, written to SlotName. False when the write failed. */
	UFUNCTION() bool SaveWithFooter();
	/** Same, as a static function taking the save object. */
	UFUNCTION() static bool SaveStatic(UEWTestSaveGame* Save);
};
