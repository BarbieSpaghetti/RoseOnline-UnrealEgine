#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "RoseZoneFactory.generated.h"

/**
 * Factory for importing ROSE Online .ZON files.
 */
UCLASS()
class BONSOIRUNREAL_API URoseZoneFactory : public UFactory {
  GENERATED_BODY()

public:
  URoseZoneFactory();

  // UFactory Interface
  virtual bool FactoryCanImport(const FString &Filename) override;
  virtual UObject *FactoryCreateFile(UClass *InClass, UObject *InParent,
                                     FName InName, EObjectFlags Flags,
                                     const FString &Filename,
                                     const TCHAR *Parms, FFeedbackContext *Warn,
                                     bool &bOutOperationCanceled) override;
};
