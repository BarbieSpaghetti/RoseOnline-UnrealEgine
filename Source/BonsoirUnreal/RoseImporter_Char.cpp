#include "BonsoirUnrealLog.h"
#include "RoseFormats.h"
#include "RoseImporter.h"

// Unreal Engine includes
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "GameFramework/Character.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

// Mesh Description includes (UE 5.7)
#include "BoneWeights.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshDescription.h"
#include "StaticMeshAttributes.h" // Sometimes needed for shared attributes

// Asset Tools
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "UObject/SavePackage.h"

// Blueprint / Kismet
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "RoseCharacter.h"

// --- SKELETON IMPORT ---
USkeleton *URoseImporter::ImportSkeleton(const FString &Path) {
  // 1. Load ZMD
  FRoseZMD ZMD;
  if (!ZMD.Load(Path)) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to load ZMD: %s"), *Path);
    return nullptr;
  }

  FString Name = FPaths::GetBaseFilename(Path) + TEXT("_Skeleton");
  FString PackageName = TEXT("/Game/Rose/Imported/Characters/") + Name;

  // Check existing
  if (USkeleton *Existing = FindOrLoadSkeleton(PackageName)) {
    return Existing;
  }

  UPackage *Package = CreatePackage(*PackageName);
  USkeleton *Skeleton =
      NewObject<USkeleton>(Package, *Name, RF_Public | RF_Standalone);

  if (ZMD.Bones.Num() == 0) {
    UE_LOG(LogRoseImporter, Error, TEXT("ZMD has 0 bones: %s"), *Path);
    return nullptr;
  }

  // 2. Build Reference Skeleton
  // Cast away constness because we are initializing a new skeleton and need to
  // modify its reference skeleton
  FReferenceSkeleton &RefSkel =
      const_cast<FReferenceSkeleton &>(Skeleton->GetReferenceSkeleton());
  // 1. Convert to FMeshBoneInfo & Sort by Hierarchy
  // UE5 RefSkeleton requires parents to appear BEFORE children.
  // ROSE ZMD does not guarantee this.

  // Map [ZMD Index] -> [New RefSkeleton Index]
  TArray<int32> OldToNewIndex;

  // Resize to include Dummies as well (Bones + Dummies)
  int32 TotalNodeCount = ZMD.Bones.Num() + ZMD.Dummies.Num();
  OldToNewIndex.Init(INDEX_NONE, TotalNodeCount);

  FReferenceSkeletonModifier Modifier(Skeleton); // process

  int32 OriginalBoneCount = ZMD.Bones.Num();

  // List of bones to process
  // Struct to hold original index and data
  struct FPendingBone {
    int32 OriginalIndex;
    int32 ParentIndex; // Original Parent Index
  };
  TArray<FPendingBone> PendingBones;
  for (int32 i = 0; i < OriginalBoneCount; ++i) {
    PendingBones.Add({i, ZMD.Bones[i].ParentID});
  }

  // Iterative processing to ensure parents are added first
  // We make multiple passes or use a queue.
  // Since ZMDs are usually small (tens/hundreds of bones), a simple multi-pass
  // is fine. Optimization:
  // 1. Add roots (Parent < 0)
  // 2. Add children of added bones

  TArray<bool> Processed;
  Processed.Init(false, OriginalBoneCount);
  int32 ProcessedCount = 0;
  int32 SafetyCounter = 0;

  // Track Root
  bool bHasRoot = false;
  int32 RootIndex = 0; // The index of the first bone added (0)

  int32 CurrentBoneIndex =
      0; // Track locally as RefSkel.GetNum() might not update instantly

  TArray<FTransform> WorldTransforms; // Store Unreal-Space World Transforms
  WorldTransforms.SetNum(OriginalBoneCount + ZMD.Dummies.Num());
  for (int32 i = 0; i < WorldTransforms.Num(); ++i)
    WorldTransforms[i] = FTransform::Identity;

  while (ProcessedCount < OriginalBoneCount) {
    bool bProgress = false;

    for (int32 i = 0; i < OriginalBoneCount; ++i) {
      if (Processed[i])
        continue;

      int32 OriginalParent = PendingBones[i].ParentIndex;

      bool bCanAdd = false;
      int32 NewParentIndex = INDEX_NONE;

      // Root bone: Parent < 0 OR Parent == itself (common in ROSE)
      if (OriginalParent < 0 || OriginalParent == i) {
        bCanAdd = true;
      } else if (OriginalParent < OriginalBoneCount) {
        if (OldToNewIndex[OriginalParent] != INDEX_NONE) {
          bCanAdd = true;
          NewParentIndex = OldToNewIndex[OriginalParent];
        }
      } else {
        UE_LOG(LogRoseImporter, Warning,
               TEXT("Bone %d has invalid parent %d. Attaching to root."), i,
               OriginalParent);
        bCanAdd = true; // Attach to root logic below
      }

      if (bCanAdd) {
        // Resolve Parent Index
        if (NewParentIndex == INDEX_NONE) {
          if (bHasRoot) {
            UE_LOG(LogRoseImporter, Warning,
                   TEXT("Bone %d is a secondary root. Reparenting to primary "
                        "root (Index %d)."),
                   i, RootIndex);
            NewParentIndex = RootIndex;
          } else {
            // This is the first root
            bHasRoot = true;
            RootIndex = CurrentBoneIndex;
          }
        }

        // Add to modifiers
        const FRoseBone &RoseBone = ZMD.Bones[i];
        FMeshBoneInfo BoneInfo;
        BoneInfo.Name = FName(*RoseBone.Name);
        BoneInfo.ParentIndex = NewParentIndex;

        // ROSE ZMD stores LOCAL bone transforms (not world).
        // Apply RH→LH conversion: rtuPosition(X, -Y, Z), rtuRotation(-X, Y, -Z,
        // W)
        FQuat LocalQuatLHS(-RoseBone.Rotation.X, RoseBone.Rotation.Y,
                           -RoseBone.Rotation.Z, RoseBone.Rotation.W);
        FVector LocalPosLHS(RoseBone.Position.X, -RoseBone.Position.Y,
                            RoseBone.Position.Z);

        FTransform BoneTransform(LocalQuatLHS,
                                 LocalPosLHS); // No *100 — bones in ROSE units
        BoneTransform.SetScale3D(FVector::OneVector);

        // Accumulate world transform for rigid binding (face/hair)
        FTransform ParentWorldLHS = FTransform::Identity;
        if (OriginalParent >= 0 && OriginalParent < OriginalBoneCount) {
          ParentWorldLHS = WorldTransforms[OriginalParent];
        }
        FTransform WorldLHS = BoneTransform * ParentWorldLHS;
        WorldTransforms[i] = WorldLHS;
        // Store full world transform for rigid binding (rotation + translation)
        BoneWorldTransformsLHS.Add(FName(*RoseBone.Name), WorldLHS);

        Modifier.Add(BoneInfo, BoneTransform);

        // DEBUG: Log bone transforms
        UE_LOG(LogRoseImporter, Warning,
               TEXT("BONE DEBUG [%d->%d]: Name=%s, Parent=%d(new=%d)"), i,
               CurrentBoneIndex, *RoseBone.Name, OriginalParent,
               NewParentIndex);
        UE_LOG(LogRoseImporter, Warning, TEXT("  AccumWorldPos=(%f, %f, %f)"),
               WorldLHS.GetTranslation().X, WorldLHS.GetTranslation().Y,
               WorldLHS.GetTranslation().Z);

        OldToNewIndex[i] = CurrentBoneIndex;
        CurrentBoneIndex++;

        Processed[i] = true;
        ProcessedCount++;
        bProgress = true;
      }
    }

    if (!bProgress) {
      UE_LOG(LogRoseImporter, Error,
             TEXT("Cyclic dependency or missing parent detected in Skeleton. "
                  "Processed %d/%d bones."),
             ProcessedCount, OriginalBoneCount);
      break; // Avoid infinite loop
    }

    SafetyCounter++;
    if (SafetyCounter > OriginalBoneCount + 2) {
      UE_LOG(LogRoseImporter, Error, TEXT("Infinite loop in bone sorting."));
      break;
    }
  }

  // ZMD Dummies (Optional: Add as bones or sockets?)
  // For now, let's skip dummies to keep skeleton clean, or add them if needed
  // for attachments. ROSE dummies are often used for weapon attachment points.
  // If we add dummies, we must use OldToNewIndex for their parents.
  int32 DummyCount = ZMD.Dummies.Num();
  for (int32 i = 0; i < DummyCount; ++i) {
    const FRoseBone &Dummy = ZMD.Dummies[i];
    FMeshBoneInfo BoneInfo;
    BoneInfo.Name = FName(*Dummy.Name);

    // Parent Index Mapping
    // Use CurrentBoneIndex to see if we already have a root.
    // Dummies without valid bone parents should attach to the primary root.
    int32 ParentBoneIdx = (CurrentBoneIndex > 0) ? RootIndex : INDEX_NONE;

    if (Dummy.ParentID >= 0 && Dummy.ParentID < OriginalBoneCount) {
      if (OldToNewIndex[Dummy.ParentID] != INDEX_NONE) {
        ParentBoneIdx = OldToNewIndex[Dummy.ParentID];
      } else {
        // Parent bone was not imported? Attach to Root
        UE_LOG(
            LogRoseImporter, Warning,
            TEXT("Dummy %s has unimported parent bone %d. Attaching to Root."),
            *Dummy.Name, Dummy.ParentID);
      }
    }

    BoneInfo.ParentIndex = ParentBoneIdx;

    // 1. Get Unreal-Space World Transform
    FQuat WorldQuatRHS(Dummy.Rotation.X, Dummy.Rotation.Y, Dummy.Rotation.Z,
                       Dummy.Rotation.W);
    FQuat WorldQuatLHS(-WorldQuatRHS.X, WorldQuatRHS.Y, -WorldQuatRHS.Z,
                       WorldQuatRHS.W);
    FVector WorldPosLHS(Dummy.Position.X, -Dummy.Position.Y, Dummy.Position.Z);

    FTransform WorldTransform(WorldQuatLHS, WorldPosLHS);
    if (OriginalBoneCount + i < WorldTransforms.Num()) {
      WorldTransforms[OriginalBoneCount + i] = WorldTransform;
    }

    // 2. Convert to Local-Relative
    FTransform LocalTransform = WorldTransform;
    if (Dummy.ParentID >= 0 && Dummy.ParentID < OriginalBoneCount) {
      FTransform ParentWorld = WorldTransforms[Dummy.ParentID];
      LocalTransform = WorldTransform.GetRelativeTransform(ParentWorld);
    }

    FTransform BoneTransform = LocalTransform;
    BoneTransform.SetTranslation(BoneTransform.GetTranslation() * 100.0f);
    BoneTransform.SetScale3D(FVector::OneVector);

    Modifier.Add(BoneInfo, BoneTransform);

    // Register Dummy Mapping
    // ZMD Index for Dummy i is OriginalBoneCount + i
    if ((OriginalBoneCount + i) < OldToNewIndex.Num()) {
      OldToNewIndex[OriginalBoneCount + i] = CurrentBoneIndex;
    }

    CurrentBoneIndex++;
  }

  // 3. Finalize
  Skeleton->PostEditChange();
  Skeleton->MarkPackageDirty();
  FAssetRegistryModule::AssetCreated(Skeleton);
  SaveRoseAsset(Skeleton); // Use helper to save to disk

  // Update Cache
  CachedSkeletonRemap = OldToNewIndex;
  UE_LOG(LogRoseImporter, Log, TEXT("Cached Skeleton Remap for %d bones"),
         CachedSkeletonRemap.Num());

  return Skeleton;
}

USkeleton *URoseImporter::FindOrLoadSkeleton(const FString &PackageName) {
  FString ObjectPath =
      PackageName + TEXT(".") + FPaths::GetBaseFilename(PackageName);
  USkeleton *Split = LoadObject<USkeleton>(nullptr, *ObjectPath);
  return Split;
}

// --- SKELETAL MESH IMPORT ---
USkeletalMesh *URoseImporter::ImportSkeletalMesh(const FString &Path,
                                                 USkeleton *Skeleton) {
  if (!Skeleton)
    return nullptr;

  FRoseZMS ZMS;
  if (!ZMS.Load(Path)) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to load skeletal ZMS: %s"),
           *Path);
    return nullptr;
  }

  FString Name = FPaths::GetBaseFilename(Path);
  FString PackageName = TEXT("/Game/Rose/Imported/Characters/") + Name;

  UPackage *Package = CreatePackage(*PackageName);
  USkeletalMesh *SkeletalMesh =
      NewObject<USkeletalMesh>(Package, *Name, RF_Public | RF_Standalone);

  SkeletalMesh->SetSkeleton(Skeleton);

  // Initialize LOD 0
  FSkeletalMeshLODInfo &LODInfo = SkeletalMesh->AddLODInfo();
  LODInfo.BuildSettings.bRecomputeNormals = false;
  LODInfo.BuildSettings.bRecomputeTangents = true;
  LODInfo.BuildSettings.bUseFullPrecisionUVs = true;

#if WITH_EDITORONLY_DATA
  if (SkeletalMesh->GetImportedModel()->LODModels.Num() == 0) {
    SkeletalMesh->GetImportedModel()->LODModels.Add(
        new FSkeletalMeshLODModel());
  }
#endif
  SkeletalMesh->SetHasVertexColors(true); // ZMS has colors

  // Set RefSkeleton from the USkeleton
  // In UE5, we usually can set it via internal member if we have access,
  // Set RefSkeleton from the USkeleton
  // or via SetRefSkeleton if available.
  SkeletalMesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();

  if (SkeletalMesh->GetRefSkeleton().GetNum() == 0) {
    UE_LOG(LogRoseImporter, Error,
           TEXT("SkeletalMesh has 0 bones in RefSkeleton!"));
    return nullptr;
  }

  // Create Invite Ref Matrices (Crucial for rendering)
  // Must happen BEFORE PostEditChange/CommitMeshDescription
  const FReferenceSkeleton &RefSkeleton = SkeletalMesh->GetRefSkeleton();
  TArray<FMatrix44f> &RefBasesInvMatrix = SkeletalMesh->GetRefBasesInvMatrix();
  RefBasesInvMatrix.Empty();

  const int32 NumBones = RefSkeleton.GetNum();
  RefBasesInvMatrix.AddUninitialized(NumBones);

  for (int32 i = 0; i < NumBones; ++i) {
    FTransform BoneTransform = FAnimationRuntime::GetComponentSpaceTransform(
        RefSkeleton, RefSkeleton.GetRefBonePose(), i);
    RefBasesInvMatrix[i] =
        FMatrix44f(BoneTransform.ToMatrixWithScale().Inverse());
  }

  // MESH DESCRIPTION SETUP
  // For UE 5.0+, we use FMeshDescription to build the mesh
  FMeshDescription MeshDesc;
  FSkeletalMeshAttributes MeshAttributes(MeshDesc);
  MeshAttributes.Register();

  // Elements
  TVertexAttributesRef<FVector3f> VertexPositions =
      MeshAttributes.GetVertexPositions();
  TVertexInstanceAttributesRef<FVector3f> VertexNormals =
      MeshAttributes.GetVertexInstanceNormals();
  TVertexInstanceAttributesRef<FVector2f> VertexUVs =
      MeshAttributes.GetVertexInstanceUVs();
  TVertexInstanceAttributesRef<FVector4f> VertexColors =
      MeshAttributes.GetVertexInstanceColors();
  // Skin Weights

  // Create Vertices
  int32 VertCount = ZMS.Vertices.Num();
  TArray<FVertexID> VertexIDs;
  VertexIDs.SetNum(VertCount);

  for (int32 i = 0; i < VertCount; ++i) {
    FVertexID VertID = MeshDesc.CreateVertex();
    VertexIDs[i] = VertID;
    // Position X, -Y, Z (Scaled by 100.0f)
    FVector3f Pos = ZMS.Vertices[i].Position * 100.0f;
    // Pos.Y = -Pos.Y; // Align with Skeleton (ImportSkeleton does not flip Y)
    VertexPositions[VertID] = Pos;

    // Weights
    // Weights should be stored as BoneIndex/Weight pairs if possible, but
    // MeshDescription uses VertexSkinWeights array of TArray<FSkinWeightPair>
    // logic OR explicit attribute setter

    // In UE5, we can use simpler vertex attributes API:
    // Attributes().GetVertexSkinWeights().Set(VertID, Weights)

    // Weights
    // UE 5.x uses AnimationCore::FBoneWeight
    auto VertexSkinWeights = MeshAttributes.GetVertexSkinWeights();

    using FBoneWeight = UE::AnimationCore::FBoneWeight;
    TArray<FBoneWeight> Weights;

    FVector4f W = ZMS.Vertices[i].Weights;
    FIntVector4 Ind = ZMS.Vertices[i].Indices;

    // Auto-Rigid Bind for 0-bone parts (FACE/HAIR)
    if (ZMS.BoneCount == 0) {
      int32 RigidBoneIndex = 0; // Default to Root

      // Try to find Head for Face/Hair
      bool bIsFaceOrHair =
          Path.Contains(TEXT("FACE"), ESearchCase::IgnoreCase) ||
          Path.Contains(TEXT("HAIR"), ESearchCase::IgnoreCase);

      if (bIsFaceOrHair) {
        int32 HeadIdx =
            Skeleton->GetReferenceSkeleton().FindBoneIndex(FName("b1_head"));

        if (HeadIdx == INDEX_NONE) {
          UE_LOG(LogRoseImporter, Warning,
                 TEXT("Could not find bone 'b1_head' for Face/Hair. Trying "
                      "'b1_neck'."));
          HeadIdx =
              Skeleton->GetReferenceSkeleton().FindBoneIndex(FName("b1_neck"));
        }

        if (HeadIdx != INDEX_NONE) {
          RigidBoneIndex = HeadIdx;
          UE_LOG(LogRoseImporter, Log,
                 TEXT("Rigid Binding Face/Hair to Bone %d (%s)"), HeadIdx,
                 *Skeleton->GetReferenceSkeleton()
                      .GetBoneName(HeadIdx)
                      .ToString());
        } else {
          UE_LOG(LogRoseImporter, Error,
                 TEXT("Could not find 'b1_head' OR 'b1_neck'. Binding to Root "
                      "(Feet)."));
        }
      }

      Weights.Add(FBoneWeight(RigidBoneIndex, 1.0f));
    } else {
      auto AddWeight = [&](int32 MeshLocalBoneIndex, float Weight) {
        if (Weight > 0.0f && ZMS.BoneIndices.IsValidIndex(MeshLocalBoneIndex)) {
          int32 OriginalZMDIndex = ZMS.BoneIndices[MeshLocalBoneIndex];
          int32 GlobalBoneIndex = OriginalZMDIndex;

          bool bRemapped = false;
          // Apply Remap if available
          if (CachedSkeletonRemap.IsValidIndex(OriginalZMDIndex)) {
            int32 RemappedIndex = CachedSkeletonRemap[OriginalZMDIndex];
            if (RemappedIndex != INDEX_NONE) {
              GlobalBoneIndex = RemappedIndex;
              bRemapped = true;
            }
          }
          if (!bRemapped && GlobalBoneIndex == 0 && OriginalZMDIndex != 0) {
            // Warning suppressed
          }

          Weights.Add(FBoneWeight(GlobalBoneIndex, Weight));
        }
      };

      AddWeight(Ind.X, W.X);
      AddWeight(Ind.Y, W.Y);
      AddWeight(Ind.Z, W.Z);
      AddWeight(Ind.W, W.W);
    }

    VertexSkinWeights.Set(VertID, Weights);
  }

  // Polygon Group (Material)
  FName MaterialSlotName = TEXT("RoseMaterial_0");
  FPolygonGroupID PolyGroupID = MeshDesc.CreatePolygonGroup();
  MeshAttributes.GetPolygonGroupMaterialSlotNames()[PolyGroupID] =
      MaterialSlotName;

  FSkeletalMaterial SkeletalMaterial;
  SkeletalMaterial.MaterialSlotName = MaterialSlotName;
  SkeletalMaterial.ImportedMaterialSlotName = MaterialSlotName;
  SkeletalMesh->GetMaterials().Add(SkeletalMaterial);

  // Auto-Assign Material from Textures
  FString DDSPath = FPaths::ChangeExtension(Path, TEXT("DDS"));
  FString RelDDSPath = DDSPath;
  if (!RoseRootPath.IsEmpty()) {
    FPaths::MakePathRelativeTo(RelDDSPath, *RoseRootPath);

    // Fix: If MakePathRelativeTo keeps the root folder name as prefix (e.g.
    // "Rose Online/3Ddata"), strip it.
    FString RootName = FPaths::GetBaseFilename(RoseRootPath);
    if (RelDDSPath.StartsWith(RootName + TEXT("/")) ||
        RelDDSPath.StartsWith(RootName + TEXT("\\"))) {
      RelDDSPath = RelDDSPath.RightChop(RootName.Len() + 1);
    }
  }

  // Ensure Master Material is loaded
  EnsureMasterMaterial();

  UE_LOG(LogRoseImporter, Log,
         TEXT("[TextureDebug] Looking for Texture: %s (Rel: %s) (Root: %s)"),
         *DDSPath, *RelDDSPath, *RoseRootPath);

  if (FPaths::FileExists(DDSPath)) {
    if (UTexture2D *Texture = LoadRoseTexture(RelDDSPath)) {
      // ... (rest of logic)
    }
  } else {
    // Fallback: Texture might be in a common folder or named differently.
    // Try loading by Filename in standard paths
    FString Filename = FPaths::GetCleanFilename(DDSPath);
    if (UTexture2D *Texture = LoadRoseTexture(Filename)) {
      // Success with filename only (will search prefixes)
      UE_LOG(LogRoseImporter, Log, TEXT("Texture found by filename: %s"),
             *Filename);

      // ... Copy-paste the MIC creation logic or refactor (since I can't
      // refactor easily in replace, I will duplicate or assume LoadTex returns
      // object) Actually, I need to assign it. Let's refactor the assignment
      // into a lambda or just do it.

      FString MatName =
          FString::Printf(TEXT("M_%s"), *FPaths::GetBaseFilename(DDSPath));
      FString MatPackageName = TEXT("/Game/Rose/Imported/Materials/") + MatName;
      FString FullMatName = MatPackageName + TEXT(".") + MatName;

      UMaterialInstanceConstant *MIC =
          FindObject<UMaterialInstanceConstant>(nullptr, *FullMatName);
      if (!MIC)
        MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *FullMatName);

      if (!MIC && MasterMaterial) {
        UMaterial *ParentMat = MasterMaterial;
        if (Texture && Texture->HasAlphaChannel()) {
          ParentMat = MasterMaterial_Masked;
        }

        UPackage *MatPkg = CreatePackage(*MatPackageName);
        auto *MatFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
        MIC = (UMaterialInstanceConstant *)MatFactory->FactoryCreateNew(
            UMaterialInstanceConstant::StaticClass(), MatPkg, *MatName,
            RF_Public | RF_Standalone, nullptr, GWarn);
        if (MIC) {
          MIC->SetParentEditorOnly(ParentMat);
          MIC->SetTextureParameterValueEditorOnly(
              FMaterialParameterInfo(TEXT("BaseTexture")), Texture);
          MIC->BasePropertyOverrides.bOverride_TwoSided = true;
          MIC->BasePropertyOverrides.TwoSided = true;

          if (ParentMat == MasterMaterial_Masked) {
            MIC->BasePropertyOverrides.bOverride_BlendMode = true;
            MIC->BasePropertyOverrides.BlendMode = BLEND_Masked;
            MIC->SetScalarParameterValueEditorOnly(
                FMaterialParameterInfo(TEXT("AlphaRef")), 0.5f);
          }

          MIC->PostEditChange();
          FAssetRegistryModule::AssetCreated(MIC);
          // Save
          FString PackageFileName = FPackageName::LongPackageNameToFilename(
              MatPackageName, FPackageName::GetAssetPackageExtension());
          FSavePackageArgs SaveArgs;
          SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
          SaveArgs.SaveFlags = SAVE_NoError;
          UPackage::SavePackage(MatPkg, MIC, *PackageFileName, SaveArgs);
        }
      }
      if (MIC) {
        if (SkeletalMesh->GetMaterials().Num() > 0)
          SkeletalMesh->GetMaterials().Last().MaterialInterface = MIC;
        else
          SkeletalMesh->GetMaterials().Add(FSkeletalMaterial(MIC));
      }
    }
  }

  // Faces / Triangles
  int32 IndexCount = ZMS.Indices.Num();
  for (int32 i = 0; i < IndexCount; i += 3) {
    uint32 I0 = ZMS.Indices[i];
    uint32 I1 = ZMS.Indices[i + 1];
    uint32 I2 = ZMS.Indices[i + 2];

    FVertexInstanceID VI0 = MeshDesc.CreateVertexInstance(VertexIDs[I0]);
    FVertexInstanceID VI1 = MeshDesc.CreateVertexInstance(VertexIDs[I1]);
    FVertexInstanceID VI2 = MeshDesc.CreateVertexInstance(VertexIDs[I2]);

    // Normals & UVs
    const auto &V0 = ZMS.Vertices[I0];
    const auto &V1 = ZMS.Vertices[I1];
    const auto &V2 = ZMS.Vertices[I2];

    VertexNormals[VI0] = FVector3f(V0.Normal.X, -V0.Normal.Y, V0.Normal.Z);
    VertexNormals[VI1] = FVector3f(V1.Normal.X, -V1.Normal.Y, V1.Normal.Z);
    VertexNormals[VI2] = FVector3f(V2.Normal.X, -V2.Normal.Y, V2.Normal.Z);

    VertexUVs[VI0] = V0.UV1;
    VertexUVs[VI1] = V1.UV1;
    VertexUVs[VI2] = V2.UV1;

    MeshDesc.CreateTriangle(PolyGroupID, {VI0, VI1, VI2});
  }

  // Commit to Mesh
  // Define proper build settings
  // Create Mesh from Description
  SkeletalMesh->CreateMeshDescription(0, MoveTemp(MeshDesc));
  SkeletalMesh->CommitMeshDescription(0);

  // Rebuild
  SkeletalMesh->PostEditChange();

  SkeletalMesh->MarkPackageDirty();
  FAssetRegistryModule::AssetCreated(SkeletalMesh);
  SaveRoseAsset(SkeletalMesh); // Ensure it's saved to disk

  return SkeletalMesh;
}

USkeletalMesh *
URoseImporter::ImportUnifiedCharacter(const TArray<FString> &PartPaths,
                                      USkeleton *Skeleton) {
  if (PartPaths.Num() == 0 || !Skeleton)
    return nullptr;

  // Ensure master materials are loaded for texture support
  EnsureMasterMaterial();

  FString AssetName = TEXT("Char_Default");
  FString PackageName = TEXT("/Game/Rose/Imported/Characters/") + AssetName;

  UPackage *Package = CreatePackage(*PackageName);
  USkeletalMesh *SkeletalMesh =
      NewObject<USkeletalMesh>(Package, *AssetName, RF_Public | RF_Standalone);
  SkeletalMesh->SetSkeleton(Skeleton);

  // Initialize LOD 0 (Copied from ImportSkeletalMesh)
  FSkeletalMeshLODInfo &LODInfo = SkeletalMesh->AddLODInfo();
  LODInfo.ScreenSize = FPerPlatformFloat(1.0f);
  LODInfo.LODHysteresis = 0.02f;

#if WITH_EDITORONLY_DATA
  if (SkeletalMesh->GetImportedModel()->LODModels.Num() == 0) {
    SkeletalMesh->GetImportedModel()->LODModels.Add(
        new FSkeletalMeshLODModel());
  }
#endif

  // Initialize Mesh Description
  FMeshDescription MeshDesc;
  FSkeletalMeshAttributes MeshAttributes(MeshDesc);
  MeshAttributes.Register();

  // Prepare Attributes
  TVertexAttributesRef<FVector3f> VertexPositions =
      MeshAttributes.GetVertexPositions();
  TVertexInstanceAttributesRef<FVector3f> VertexNormals =
      MeshAttributes.GetVertexInstanceNormals();
  TVertexInstanceAttributesRef<FVector2f> VertexUVs =
      MeshAttributes.GetVertexInstanceUVs();
  TVertexInstanceAttributesRef<FVector4f> VertexColors =
      MeshAttributes.GetVertexInstanceColors();
  auto VertexSkinWeights = MeshAttributes.GetVertexSkinWeights();

  using FBoneWeight = UE::AnimationCore::FBoneWeight;

  // Cache Skeleton Remap (shared for all parts)
  const FReferenceSkeleton &RefSkeleton = Skeleton->GetReferenceSkeleton();
  SkeletalMesh->SetRefSkeleton(RefSkeleton);

  // DEBUG: Log all bones to help identify Head bone name
  const int32 NumBones = RefSkeleton.GetNum();
  UE_LOG(LogRoseImporter, Log, TEXT("Skeleton has %d bones:"), NumBones);
  for (int32 i = 0; i < NumBones; ++i) {
    UE_LOG(LogRoseImporter, Log, TEXT("  Bone %d: %s"), i,
           *RefSkeleton.GetBoneName(i).ToString());
  }

  // Fix: Calculate Inverse Ref Matrices for Skinning
  // This block must be unique. The previous duplication caused compilation
  // errors.
  TArray<FMatrix44f> &RefBasesInvMatrix = SkeletalMesh->GetRefBasesInvMatrix();
  RefBasesInvMatrix.Empty();
  RefBasesInvMatrix.AddUninitialized(NumBones);

  for (int32 i = 0; i < NumBones; ++i) {
    FTransform BoneTransform = FAnimationRuntime::GetComponentSpaceTransform(
        RefSkeleton, RefSkeleton.GetRefBonePose(), i);
    RefBasesInvMatrix[i] =
        FMatrix44f(BoneTransform.ToMatrixWithScale().Inverse());
  }

  int32 MaterialSlotOffset = 0;

  // Iterate through all parts and merge
  for (const FString &Path : PartPaths) {
    FRoseZMS ZMS;
    if (!ZMS.Load(Path))
      continue;

    UE_LOG(LogRoseImporter, Log, TEXT("Merging Part: %s"), *Path);

    // -- MATERIAL HANDLING --
    // Try to load texture and create material
    FString DDSPath = FPaths::ChangeExtension(Path, TEXT("DDS"));
    FString Folder = FPaths::GetPath(Path); // e.g., .../AVATAR/BODY
    FString Filename = FPaths::GetBaseFilename(DDSPath) + TEXT(".DDS");

    UE_LOG(LogRoseImporter, Log, TEXT("DEBUG: Resolving Texture for %s"),
           *Path);

    // Robust Texture Search
    UTexture2D *Texture = LoadRoseTexture(DDSPath);
    if (!Texture) {
      // Try filename only fallback
      Texture = LoadRoseTexture(Filename);
    }

    UE_LOG(LogRoseImporter, Log,
           TEXT("DEBUG: Texture Search for Part %s -> %s"), *Path, *DDSPath);
    if (Texture) {
      UE_LOG(LogRoseImporter, Log,
             TEXT("DEBUG: Texture LOADED: %s (Alpha: %d)"), *Texture->GetName(),
             Texture->HasAlphaChannel());
    } else {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("DEBUG: Texture FAILED to load for %s"), *DDSPath);
    }

    UMaterialInstanceConstant *MIC = nullptr;
    FString MatName =
        FString::Printf(TEXT("M_%s"), *FPaths::GetBaseFilename(DDSPath));
    if (Texture) {
      FString MatPkgName = TEXT("/Game/Rose/Imported/Materials/") + MatName;

      MIC = LoadObject<UMaterialInstanceConstant>(
          nullptr, *(MatPkgName + TEXT(".") + MatName));
      if (!MIC) {
        UMaterial *ParentMat = MasterMaterial;
        if (Texture->HasAlphaChannel())
          ParentMat = MasterMaterial_Masked;

        UPackage *MatPkg = CreatePackage(*MatPkgName);
        auto *MatFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
        MIC = (UMaterialInstanceConstant *)MatFactory->FactoryCreateNew(
            UMaterialInstanceConstant::StaticClass(), MatPkg, *MatName,
            RF_Public | RF_Standalone, nullptr, GWarn);

        if (MIC) {
          MIC->SetParentEditorOnly(ParentMat);
          MIC->SetTextureParameterValueEditorOnly(
              FMaterialParameterInfo(TEXT("BaseTexture")), Texture);
          MIC->BasePropertyOverrides.bOverride_TwoSided = true;
          MIC->BasePropertyOverrides.TwoSided = true;

          if (ParentMat == MasterMaterial_Masked) {
            MIC->BasePropertyOverrides.bOverride_BlendMode = true;
            MIC->BasePropertyOverrides.BlendMode = BLEND_Masked;
            MIC->SetScalarParameterValueEditorOnly(
                FMaterialParameterInfo(TEXT("AlphaRef")), 0.5f);
          }
          MIC->PostEditChange();
          FAssetRegistryModule::AssetCreated(MIC);
          // Save Material
          FString PackageFileName = FPackageName::LongPackageNameToFilename(
              MatPkgName, FPackageName::GetAssetPackageExtension());
          FSavePackageArgs SaveArgs;
          SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
          SaveArgs.SaveFlags = SAVE_NoError;
          UPackage::SavePackage(MatPkg, MIC, *PackageFileName, SaveArgs);
        }
      }
    }

    FName SlotName(*MatName);
    if (MIC) {
      FSkeletalMaterial SkeletalMat(MIC, true, false, SlotName, SlotName);
      SkeletalMesh->GetMaterials().Add(SkeletalMat);
    } else {
      FSkeletalMaterial SkeletalMat(MasterMaterial, true, false, SlotName,
                                    SlotName);
      SkeletalMesh->GetMaterials().Add(SkeletalMat);
    }

    FPolygonGroupID PolyGroupID = MeshDesc.CreatePolygonGroup();
    FStaticMeshAttributes(MeshDesc)
        .GetPolygonGroupMaterialSlotNames()[PolyGroupID] = SlotName;

    // -- GEOMETRY MERGING --
    TArray<FVertexID> LocalVertexIDs;
    int32 VertCount = ZMS.Vertices.Num();
    LocalVertexIDs.SetNum(VertCount);

    // Debug Head Bone Index
    int32 DebugHeadIdx = RefSkeleton.FindBoneIndex(FName("b1_head"));
    UE_LOG(LogRoseImporter, Log, TEXT("DEBUG: b1_head Index = %d"),
           DebugHeadIdx);

    for (int32 i = 0; i < VertCount; ++i) {
      FVertexID VertID = MeshDesc.CreateVertex();
      LocalVertexIDs[i] = VertID;

      // Restore Scale * 100.0f and Apply Chirality Flip (Y)
      FVector3f Pos = ZMS.Vertices[i].Position * 100.0f;
      Pos.Y = -Pos.Y; // RH -> LH conversion

      FVector3f Normal =
          FVector3f(ZMS.Vertices[i].Normal.X, -ZMS.Vertices[i].Normal.Y,
                    ZMS.Vertices[i].Normal.Z);

      // Auto-Rigid Bind Logic
      bool bIsRigid = ZMS.BoneCount == 0 ||
                      (Path.Contains(TEXT("FACE"), ESearchCase::IgnoreCase) ||
                       Path.Contains(TEXT("HAIR"), ESearchCase::IgnoreCase));

      int32 RigidBoneIdx = 0;

      if (bIsRigid) {
        RigidBoneIdx = RefSkeleton.FindBoneIndex(FName("b1_head"));
        if (RigidBoneIdx == INDEX_NONE)
          RigidBoneIdx = RefSkeleton.FindBoneIndex(FName("b1_neck"));
        if (RigidBoneIdx == INDEX_NONE)
          RigidBoneIdx = 0;

        // Use stored ROSE world transforms for face/hair
        // (applies both rotation and translation to fix head orientation)
        FName BoneName = RefSkeleton.GetBoneName(RigidBoneIdx);
        FTransform BoneWorldT = FTransform::Identity;
        if (FTransform *Found = BoneWorldTransformsLHS.Find(BoneName)) {
          BoneWorldT = *Found;
        }

        // DEBUG: Log once per mesh part
        if (i == 0) {
          FVector BonePos = BoneWorldT.GetTranslation();
          UE_LOG(LogRoseImporter, Warning,
                 TEXT("RIGID DEBUG: Path=%s, bIsRigid=true, RigidBoneIdx=%d, "
                      "BoneName=%s"),
                 *Path, RigidBoneIdx, *BoneName.ToString());
          UE_LOG(LogRoseImporter, Warning,
                 TEXT("RIGID DEBUG: BoneWorldPos LHS=(%f, %f, %f)"), BonePos.X,
                 BonePos.Y, BonePos.Z);
          UE_LOG(LogRoseImporter, Warning,
                 TEXT("RIGID DEBUG: Pos BEFORE transform=(%f, %f, %f)"), Pos.X,
                 Pos.Y, Pos.Z);
        }

        // Apply full bone world transform (rotate + translate)
        FVector TransformedPos = BoneWorldT.TransformPosition(FVector(Pos));
        Pos = (FVector3f)TransformedPos;

        if (i == 0) {
          UE_LOG(LogRoseImporter, Warning,
                 TEXT("RIGID DEBUG: Pos AFTER transform=(%f, %f, %f)"), Pos.X,
                 Pos.Y, Pos.Z);
        }
      }

      VertexPositions[VertID] = Pos;

      // Weights
      TArray<FBoneWeight> Weights;
      FVector4f W = ZMS.Vertices[i].Weights;
      FIntVector4 Ind = ZMS.Vertices[i].Indices;

      if (bIsRigid) {
        Weights.Add(FBoneWeight(RigidBoneIdx, 1.0f));
      } else {
        // Standard weighting logic WITH REMAP
        auto AddWeight = [&](int32 BoneIdx, float WeightVal) {
          if (WeightVal > 0.0f && ZMS.BoneIndices.IsValidIndex(BoneIdx)) {
            int32 OrigBoneIdx = ZMS.BoneIndices[BoneIdx];
            int32 GlobalBoneIndex = OrigBoneIdx;

            // Apply Cached Remap
            if (CachedSkeletonRemap.IsValidIndex(OrigBoneIdx)) {
              int32 Remapped = CachedSkeletonRemap[OrigBoneIdx];
              if (Remapped != INDEX_NONE) {
                GlobalBoneIndex = Remapped;
              }
            }

            Weights.Add(FBoneWeight(GlobalBoneIndex, WeightVal));
          }
        };
        AddWeight(Ind.X, W.X);
        AddWeight(Ind.Y, W.Y);
        AddWeight(Ind.Z, W.Z);
        AddWeight(Ind.W, W.W);
      }
      VertexSkinWeights.Set(VertID, Weights);
    }

    // Triangles
    int32 IndexCount = ZMS.Indices.Num();
    for (int32 i = 0; i < IndexCount; i += 3) {
      uint32 I0 = ZMS.Indices[i];
      uint32 I1 = ZMS.Indices[i + 1];
      uint32 I2 = ZMS.Indices[i + 2];

      FVertexInstanceID VI0 = MeshDesc.CreateVertexInstance(LocalVertexIDs[I0]);
      FVertexInstanceID VI1 = MeshDesc.CreateVertexInstance(LocalVertexIDs[I1]);
      FVertexInstanceID VI2 = MeshDesc.CreateVertexInstance(LocalVertexIDs[I2]);

      const auto &V0 = ZMS.Vertices[I0];
      const auto &V1 = ZMS.Vertices[I1];
      const auto &V2 = ZMS.Vertices[I2];

      bool bIsRigid = ZMS.BoneCount == 0 ||
                      (Path.Contains(TEXT("FACE"), ESearchCase::IgnoreCase) ||
                       Path.Contains(TEXT("HAIR"), ESearchCase::IgnoreCase));

      if (bIsRigid) {
        // Same Y-flip as non-rigid (matches working static mesh import)
        VertexNormals[VI0] = FVector3f(V0.Normal.X, -V0.Normal.Y, V0.Normal.Z);
        VertexNormals[VI1] = FVector3f(V1.Normal.X, -V1.Normal.Y, V1.Normal.Z);
        VertexNormals[VI2] = FVector3f(V2.Normal.X, -V2.Normal.Y, V2.Normal.Z);
      } else {
        VertexNormals[VI0] = FVector3f(V0.Normal.X, -V0.Normal.Y, V0.Normal.Z);
        VertexNormals[VI1] = FVector3f(V1.Normal.X, -V1.Normal.Y, V1.Normal.Z);
        VertexNormals[VI2] = FVector3f(V2.Normal.X, -V2.Normal.Y, V2.Normal.Z);
      }

      VertexUVs[VI0] = V0.UV1;
      VertexUVs[VI1] = V1.UV1;
      VertexUVs[VI2] = V2.UV1;

      // Revert Winding Order to what was "corrigé" (Standard Order)
      MeshDesc.CreateTriangle(PolyGroupID, {VI0, VI1, VI2});
    }

    MaterialSlotOffset++;
  }

  SkeletalMesh->CreateMeshDescription(0, MoveTemp(MeshDesc));
  SkeletalMesh->CommitMeshDescription(0);
  SkeletalMesh->PostEditChange();
  SkeletalMesh->MarkPackageDirty();
  FAssetRegistryModule::AssetCreated(SkeletalMesh);
  SaveRoseAsset(SkeletalMesh);

  return SkeletalMesh;
}

UAnimSequence *URoseImporter::ImportAnimation(const FString &Path,
                                              USkeleton *Skeleton,
                                              USkeletalMesh *Mesh) {
  if (!Skeleton)
    return nullptr;

  FRoseZMO ZMO;
  if (!ZMO.Load(Path))
    return nullptr;

  FString Name = FPaths::GetBaseFilename(Path);
  FString PackageName =
      TEXT("/Game/Rose/Imported/Characters/Animations/") + Name;

  UPackage *Package = CreatePackage(*PackageName);
  UAnimSequence *AnimSequence =
      NewObject<UAnimSequence>(Package, *Name, RF_Public | RF_Standalone);

  AnimSequence->SetSkeleton(Skeleton);

  // SETUP CONTROLLER
  IAnimationDataController &Controller = AnimSequence->GetController();
  Controller.OpenBracket(FText::FromString("Import Rose Animation"));

  // 1. Set Rate and Length
  Controller.SetFrameRate(FFrameRate(ZMO.FPS, 1));
  Controller.SetNumberOfFrames(FFrameNumber(ZMO.FrameCount));
  Controller.NotifyPopulated(); // Important to notify changes

  // 2. Add Tracks
  const FReferenceSkeleton &RefSkel = Skeleton->GetReferenceSkeleton();

  for (const FRoseAnimChannel &Chan : ZMO.Channels) {
    int32 TargetBoneIndex = Chan.BoneID;

    // Apply Cached Remap
    if (CachedSkeletonRemap.IsValidIndex(Chan.BoneID)) {
      int32 Remapped = CachedSkeletonRemap[Chan.BoneID];
      if (Remapped != INDEX_NONE) {
        TargetBoneIndex = Remapped;
      } else {
        // Bone was culled or not imported
        continue;
      }
    }

    if (TargetBoneIndex < 0 || TargetBoneIndex >= RefSkel.GetNum()) {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("Anim Channel references invalid bone %d (Original %d)"),
             TargetBoneIndex, Chan.BoneID);
      continue;
    }

    FName BoneName = RefSkel.GetBoneName(TargetBoneIndex);

    // Create Controller if needed (UE 5.7 specific?)
    // Actually, we need to add the curve to the controller.
    // Controller.AddBoneCurve(BoneName); // This might be needed if track
    // doesn't exist? In 5.x, AddBoneCurve adds the track.

    Controller.AddBoneCurve(BoneName);

    // 3. Add Keys
    // ZMO data is dense (one key per frame)
    TArray<FVector3f> PosKeys;
    TArray<FQuat4f> RotKeys;
    TArray<FVector3f> ScaleKeys;

    PosKeys.SetNum(ZMO.FrameCount);
    RotKeys.SetNum(ZMO.FrameCount);
    ScaleKeys.SetNum(ZMO.FrameCount);

    bool bHasPos = (Chan.Type == 2 && Chan.PosKeys.Num() == ZMO.FrameCount);
    bool bHasRot = (Chan.Type == 4 && Chan.RotKeys.Num() == ZMO.FrameCount);
    bool bHasScale =
        (Chan.Type == 1024 && Chan.ScaleKeys.Num() == ZMO.FrameCount);

    // World-to-Local Cache for Relative Keys
    int32 ParentIndex = RefSkel.GetParentIndex(TargetBoneIndex);
    FTransform ParentWorld =
        (ParentIndex != INDEX_NONE)
            ? FAnimationRuntime::GetComponentSpaceTransform(
                  RefSkel, RefSkel.GetRefBonePose(), ParentIndex)
            : FTransform::Identity;

    for (int32 f = 0; f < ZMO.FrameCount; ++f) {
      // 1. Get World Key (Mirror Y and Scale 100x)
      FVector3f WorldPos =
          bHasPos ? (FVector3f)Chan.PosKeys[f]
                  : (FVector3f)RefSkel.GetRefBonePose()[TargetBoneIndex]
                            .GetLocation() /
                        100.0f;
      WorldPos.Y = -WorldPos.Y;

      FQuat4f WorldQuat =
          bHasRot ? (FQuat4f)Chan.RotKeys[f]
                  : (FQuat4f)RefSkel.GetRefBonePose()[TargetBoneIndex]
                        .GetRotation();
      FQuat4f WorldQuatLHS =
          FQuat4f(-WorldQuat.X, WorldQuat.Y, -WorldQuat.Z, WorldQuat.W);

      FTransform WorldTransform((FQuat)WorldQuatLHS,
                                (FVector)WorldPos * 100.0f);

      // 2. Make Local-Relative to Parent Bind Pose (as approximation)
      FTransform LocalTransform =
          WorldTransform.GetRelativeTransform(ParentWorld);

      PosKeys[f] = (FVector3f)LocalTransform.GetLocation();
      RotKeys[f] = (FQuat4f)LocalTransform.GetRotation();
      ScaleKeys[f] =
          bHasScale ? (FVector3f)Chan.ScaleKeys[f] : FVector3f::OneVector;
    }

    Controller.SetBoneTrackKeys(BoneName, PosKeys, RotKeys, ScaleKeys);
  }

  Controller.CloseBracket();

  AnimSequence->MarkPackageDirty();
  FAssetRegistryModule::AssetCreated(AnimSequence);

  return AnimSequence;
}

// --- DEFAULT CHARACTER ---
// --- BLUEPRINT CREATION ---
// Forward declare if simple, but we need KismetEditorUtilities

// Forward declarations
class USkeletalMesh;
class UAnimSequence;

void CreateRoseCharacterBlueprint(TMap<FString, USkeletalMesh *> Meshes,
                                  UAnimSequence *IdleAnim) {
  FString Name = TEXT("BP_RoseCharacter");
  FString PackageName = TEXT("/Game/Rose/Imported/Characters/") + Name;

  UPackage *Package = CreatePackage(*PackageName);

  // Create Blueprint
  UBlueprint *BP = FKismetEditorUtilities::CreateBlueprint(
      ARoseCharacter::StaticClass(), Package, *Name, BPTYPE_Normal,
      UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

  if (BP) {
    // 1. Edit CDO for Mesh & Anim
    ACharacter *CDO = Cast<ACharacter>(BP->GeneratedClass->GetDefaultObject());
    if (CDO) {
      // Set Body as the main mesh (if available)
      if (Meshes.Contains("BODY") && Meshes["BODY"]) {
        CDO->GetMesh()->SetSkeletalMesh(Meshes["BODY"]);
        CDO->GetMesh()->SetRelativeLocation(FVector(0, 0, -90));
        CDO->GetMesh()->SetRelativeRotation(FRotator(0, -90, 0));

        if (IdleAnim) {
          CDO->GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
          CDO->GetMesh()->SetAnimation(IdleAnim);
        }
      }

      CDO->AutoPossessPlayer = EAutoReceiveInput::Player0;
    }

    // 2. Add Other Parts via SCS
    if (USimpleConstructionScript *SCS = BP->SimpleConstructionScript) {
      for (const auto &Elem : Meshes) {
        FString PartName = Elem.Key;
        USkeletalMesh *PartMesh = Elem.Value;

        if (PartName == "BODY")
          continue; // Already handled as main mesh

        USCS_Node *MeshNode =
            SCS->CreateNode(USkeletalMeshComponent::StaticClass(), *PartName);
        if (MeshNode) {
          SCS->AddNode(MeshNode);
          // Attach to proper parent if needed, or root
          // For character parts, they usually attach to the main mesh and use
          // Master Pose But initially, let's just add them.

          USkeletalMeshComponent *SkelComp =
              Cast<USkeletalMeshComponent>(MeshNode->ComponentTemplate);
          if (SkelComp) {
            if (!PartMesh) {
              UE_LOG(LogRoseImporter, Error, TEXT("PartMesh for %s is NULL!"),
                     *PartName);
            } else {
              UE_LOG(LogRoseImporter, Log, TEXT("Adding Part: %s to BP"),
                     *PartName);
              SkelComp->SetSkeletalMesh(PartMesh);
              SkelComp->SetRelativeLocation(FVector(0, 0, -90));
              SkelComp->SetRelativeRotation(FRotator(0, -90, 0));
              // Scale baked into asset now
            }
          }
        }
      }
      // SpotLight
      USCS_Node *LightNode = SCS->CreateNode(USpotLightComponent::StaticClass(),
                                             TEXT("CharacterLight"));
      if (LightNode) {
        SCS->AddNode(LightNode);
        USpotLightComponent *Light =
            Cast<USpotLightComponent>(LightNode->ComponentTemplate);
        if (Light) {
          Light->Intensity = 5000.0f;
          Light->OuterConeAngle = 45.0f;
          Light->AttenuationRadius = 500.0f;
          Light->SetRelativeLocation(FVector(0, 200, 200)); // Front-ish
          Light->SetRelativeRotation(
              FRotator(-45, -90, 0)); // Pointing down at char
        }
      }

      // 3. Add PointLight (Fill Light)
      USCS_Node *PointLightNode = SCS->CreateNode(
          UPointLightComponent::StaticClass(), TEXT("FillLight"));
      if (PointLightNode) {
        SCS->AddNode(PointLightNode);
        UPointLightComponent *FillLight =
            Cast<UPointLightComponent>(PointLightNode->ComponentTemplate);
        if (FillLight) {
          FillLight->Intensity = 3000.0f;
          FillLight->AttenuationRadius = 1000.0f;
          FillLight->SetRelativeLocation(
              FVector(0, -100, 100)); // Back/Side fill
        }
      }
    }

    FKismetEditorUtilities::CompileBlueprint(BP);

    FAssetRegistryModule::AssetCreated(BP);
    BP->MarkPackageDirty();

    UE_LOG(LogRoseImporter, Log, TEXT("Created Character Blueprint: %s"),
           *PackageName);
  }
}

void URoseImporter::ImportDefaultCharacter(const FString &ZMDPath) {
  // ... [Previous Implementation] ...
  // Copying context from previous step and appending BP creation

  UE_LOG(LogRoseImporter, Log, TEXT("Importing Default Character from ZMD: %s"),
         *ZMDPath);

  FString AbsZMDPath =
      IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*ZMDPath);
  FString AvatarDir = FPaths::GetPath(AbsZMDPath);
  FString ThreeDDataDir = FPaths::GetPath(AvatarDir);
  this->RoseRootPath = FPaths::GetPath(ThreeDDataDir);

  UE_LOG(LogRoseImporter, Log, TEXT("AvatarDir: %s"), *AvatarDir);

  // 2. Import Skeleton
  UE_LOG(LogRoseImporter, Log, TEXT("Starting ImportSkeleton..."));
  USkeleton *Skeleton = ImportSkeleton(AbsZMDPath);
  if (!Skeleton) {
    UE_LOG(LogRoseImporter, Error, TEXT("ImportSkeleton failed."));
    return;
  }
  UE_LOG(LogRoseImporter, Log, TEXT("ImportSkeleton finished."));

  // Helper lambda for case-insensitive search
  auto FindFileCaseInsensitive = [](const FString &Directory,
                                    const FString &Filename) -> FString {
    FString Result = FPaths::Combine(Directory, Filename);
    if (FPaths::FileExists(Result))
      return Result;

    // Iterate directory to find match
    IFileManager &FileManager = IFileManager::Get();
    TArray<FString> FoundFiles;
    // FindFiles expects a path with wildcard, e.g. "Path/*"
    FString SearchPath = FPaths::Combine(Directory, TEXT("*"));
    FileManager.FindFiles(FoundFiles, *SearchPath, true, false);

    for (const FString &Found : FoundFiles) {
      if (Found.Equals(Filename, ESearchCase::IgnoreCase)) {
        return FPaths::Combine(Directory, Found);
      }
    }
    return FString();
  };

  // 3. Import Character Parts (BODY, ARMS, FACE, FOOT, HAIR)
  struct FPartDef {
    FString SlotName;
    FString SearchPattern;
  };

  TArray<FPartDef> Parts = {
      {TEXT("BODY"), TEXT("BODY1_001*.ZMS")},
      {TEXT("ARMS"), TEXT("ARM1_001*.ZMS")},
      {TEXT("FACE"), TEXT("FACE1_001*.ZMS")},
      {TEXT("FOOT"), TEXT("FOOT1_001*.ZMS")},
      {TEXT("HAIR"), TEXT("HAIR1_001*.ZMS")},
  };

  TMap<FString, USkeletalMesh *> ImportedMeshes;

  TArray<FString> PartPaths;

  // Collect Paths for All Parts (Priority: Body -> Arms -> Hands -> Feet ->
  // Face -> Hair)
  for (const FPartDef &Part : Parts) {
    TArray<FString> FoundFiles;
    FString Folder = AvatarDir;

    // Search in Folder/Part.SearchPattern
    FString Pattern = Folder / Part.SearchPattern;
    IFileManager::Get().FindFiles(FoundFiles, *Pattern, true, false);

    if (FoundFiles.Num() == 0) {
      // Re-try with specific subfolders
      FString SubFolder = Folder / Part.SlotName;
      FString SubPattern = SubFolder / Part.SearchPattern;
      IFileManager::Get().FindFiles(FoundFiles, *SubPattern, true, false);

      // If found in subfolder, update folder path for full path construction
      if (FoundFiles.Num() > 0)
        Folder = SubFolder;
    }

    if (FoundFiles.Num() > 0) {
      FoundFiles.Sort();

      // Fix: If this is the BODY slot, collect ALL found parts (e.g. Upper and
      // Lower body) Otherwise, just take the first one (e.g. Default Face,
      // Default Hair)
      if (Part.SlotName == "BODY") {
        for (const FString &Found : FoundFiles) {
          FString FullPath = Folder / Found;
          PartPaths.Add(FullPath);
          UE_LOG(LogRoseImporter, Log, TEXT("Added Unified Body Part: %s"),
                 *FullPath);
        }
      } else {
        FString FullPath = Folder / FoundFiles[0];
        PartPaths.Add(FullPath);
        UE_LOG(LogRoseImporter, Log, TEXT("Added Unified Part: %s"), *FullPath);
      }
    }
  }

  // Unified Import
  USkeletalMesh *UnifiedMesh = ImportUnifiedCharacter(PartPaths, Skeleton);

  if (UnifiedMesh && Skeleton) {
    // Import all animations found in MOTION folder
    TArray<FString> AnimFiles;
    IFileManager::Get().FindFiles(
        AnimFiles, *(AvatarDir / TEXT("MOTION/*.ZMO")), true, false);
    for (const FString &AnimFile : AnimFiles) {
      ImportAnimation(AvatarDir / TEXT("MOTION") / AnimFile, Skeleton,
                      UnifiedMesh);
    }
  }
}
