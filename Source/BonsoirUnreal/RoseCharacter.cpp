#include "RoseCharacter.h"
#include "Components/SkeletalMeshComponent.h"

ARoseCharacter::ARoseCharacter() { PrimaryActorTick.bCanEverTick = true; }

void ARoseCharacter::PostInitializeComponents() {
  Super::PostInitializeComponents();
  AssembleCharacter();
}

void ARoseCharacter::BeginPlay() {
  Super::BeginPlay();
  AssembleCharacter();
}

void ARoseCharacter::AssembleCharacter() {
  USkeletalMeshComponent *MainMesh = GetMesh();
  if (!MainMesh)
    return;

  TArray<UActorComponent *> SkelComps;
  GetComponents(USkeletalMeshComponent::StaticClass(), SkelComps);

  for (UActorComponent *Comp : SkelComps) {
    USkeletalMeshComponent *SkelComp = Cast<USkeletalMeshComponent>(Comp);
    if (SkelComp && SkelComp != MainMesh) {
      // Set Leader Pose to the main body
      SkelComp->SetLeaderPoseComponent(MainMesh);

      // Ensure they share the same physics and LODs if possible
      SkelComp->bUseAttachParentBound = true;
    }
  }
}
