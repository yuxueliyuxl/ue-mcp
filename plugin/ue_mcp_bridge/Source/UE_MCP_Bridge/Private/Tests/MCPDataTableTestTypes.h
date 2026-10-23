#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/Texture2D.h"
#include "Templates/SubclassOf.h"
#include "MCPDataTableTestTypes.generated.h"

/** Row struct used only by the DataTable reference tests: one field of each
 *  reference kind a row names by path, and one plain value beside them. */
USTRUCT()
struct FUEMCPDataTableReferenceRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY()
	TSoftClassPtr<UObject> SoftClass;

	UPROPERTY()
	TObjectPtr<UTexture2D> HardObject = nullptr;

	UPROPERTY()
	TSubclassOf<UObject> HardClass;

	UPROPERTY()
	int32 Count = 0;
};

/** Row hook used by the batch test to expose notifications to untouched rows. */
USTRUCT()
struct FUEMCPDataTableDerivedRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Value = 0;

	UPROPERTY()
	int32 Derived = 0;

	virtual void OnDataTableChanged(const UDataTable* InDataTable, const FName InRowName) override
	{
		Derived = Value * 2;
	}
};
