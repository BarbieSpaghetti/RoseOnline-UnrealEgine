#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RoseCharacter.generated.h"

UCLASS()
class BONSOIRUNREAL_API ARoseCharacter : public ACharacter {
  GENERATED_BODY()

public:
  ARoseCharacter();

protected:
  virtual void PostInitializeComponents() override;
  virtual void BeginPlay() override;

public:
  // Helper to force update
  UFUNCTION(BlueprintCallable, Category = "Rose")
  void AssembleCharacter();
};
