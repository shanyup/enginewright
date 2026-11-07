#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

namespace EW
{
	/**
	 * Parses a type string into a Blueprint pin type. Forms (case-insensitive):
	 * bool, int, int64, float, double, byte, string, name, text, vector, vector2d, rotator, transform, color,
	 * object:<Class>, class:<Class>, softobject:<Class>, softclass:<Class>, struct:<Struct>, enum:<Enum>,
	 * and a container: <type>[] or array:<type>, set:<type>, map:<keytype>,<valuetype>.
	 */
	bool ParsePinType(const FString& Text, FEdGraphPinType& Out, FString& OutError);

	/** The inverse of ParsePinType, for readback. */
	FString PinTypeToString(const FEdGraphPinType& Type);

	/** Finds a struct by /Script path, /Game path (Blueprint struct) or short name with or without F. */
	UScriptStruct* FindStruct(const FString& Name, FString& OutError);

	/** Finds an enum by /Script path, /Game path (Blueprint enum) or short name with or without E. */
	UEnum* FindEnum(const FString& Name, FString& OutError);
}
