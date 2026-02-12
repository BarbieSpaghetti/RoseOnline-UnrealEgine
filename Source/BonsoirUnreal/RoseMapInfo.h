#pragma once

#include "CoreMinimal.h"
#include "RoseMapInfo.generated.h"

/**
 * Stores information about an imported ROSE Online Zone.
 */
UCLASS(BlueprintType)
class BONSOIRUNREAL_API URoseMapInfo : public UObject {
  GENERATED_BODY()

public:
  // Path to the original .ZON file
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rose Import")
  FString OriginalZONPath;
};
