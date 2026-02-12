#pragma once

#include "BonsoirUnrealLog.h"
#include "CoreMinimal.h"
#include "Serialization/Archive.h"

// Helper struct for reading ROSE strings
struct FRoseArchive : public FArchive {
  FArchive &Inner;

  FRoseArchive(FArchive &InInner) : Inner(InInner) {
    this->SetIsLoading(true);
    this->SetIsPersistent(true);
  }

  // Forward calls
  virtual void Serialize(void *V, int64 Length) override {
    Inner.Serialize(V, Length);
  }

  virtual int64 TotalSize() override { return Inner.TotalSize(); }

  virtual int64 Tell() override { return Inner.Tell(); }

  virtual void Seek(int64 InPos) override { Inner.Seek(InPos); }

  // ROSE String Helpers
  FString ReadByteString() {
    uint8 Length = 0;
    Inner << Length;
    if (Length == 0)
      return FString();

    TArray<uint8> Buffer;
    Buffer.AddUninitialized(Length);
    Inner.Serialize(Buffer.GetData(), Length);

    // ROSE uses ANSI/ASCII (Windows-1252 usually), but for now assuming
    // compatible ASCII
    return FString(Length, (const ANSICHAR *)Buffer.GetData());
  }

  FString ReadShortString() {
    uint16 Length = 0;
    Inner << Length;
    if (Length == 0)
      return FString();

    TArray<uint8> Buffer;
    Buffer.AddUninitialized(Length);
    Inner.Serialize(Buffer.GetData(), Length);

    return FString(Length, (const ANSICHAR *)Buffer.GetData());
  }

  // Matches CGameStr::ReadString (bIgnoreWhiteSpace=true by default)
  FString ReadRoseString(bool bIgnoreWhiteSpace = true) {
    TArray<uint8> Buffer;
    bool bGetChar = false;
    bool bInDoubleQuote = false;
    int32 Count = 0;

    while (!Inner.AtEnd()) {
      uint8 Byte;
      Inner << Byte;

      if (Byte == 0)
        break; // CGameStr loop condition: while(cChar)

      if (Byte == '"') {
        bInDoubleQuote = !bInDoubleQuote;
        continue; // CGameStr toggles and continues
      }

      if (Byte == ' ' || Byte == '\t' || Byte == 0x0D || Byte == 0x0A) {
        if (!bInDoubleQuote && !bIgnoreWhiteSpace) {
          // Break if we already have chars (token end)
          if (bGetChar)
            break;
          // Else continue (skip leading)
          continue;
        }
        // CGameStr: if (!bGetChar) continue; (Skip leading whitespace)
        if (!bGetChar)
          continue;
      }

      Buffer.Add(Byte);
      bGetChar = true;

      if (++Count > 10000)
        break; // Safety
    }

    if (Buffer.Num() == 0)
      return FString();
    return FString(Buffer.Num(), (const ANSICHAR *)Buffer.GetData());
  }
};

/**
 * STB (String Table) Format
 * Used for ZONETYPEINFO.STB and TileSet files
 */
struct FRoseSTB {
  int32 RowSize;
  TArray<int16> ColumnSizes;
  TArray<FString> ColumnNames;
  TArray<TArray<FString>> Cells; // [Row][Column]

  bool Load(const FString &FilePath) {
    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath)) {
      UE_LOG(LogTemp, Warning, TEXT("Failed to load STB file: %s"), *FilePath);
      return false;
    }

    FMemoryReader Ar(FileData);
    Ar.SetByteSwapping(false); // Little endian

    // Read header "STB1"
    char Header[5] = {0};
    Ar.Serialize(Header, 4);
    if (FString(Header) != TEXT("STB1")) {
      UE_LOG(LogTemp, Error, TEXT("Invalid STB header: %s"), *FilePath);
      return false;
    }

    // Read offset (not used for loading)
    int32 DataOffset;
    Ar << DataOffset;

    // Read counts
    int32 RowCount, ColumnCount;
    Ar << RowCount << ColumnCount << RowSize;

    // Read column sizes
    ColumnSizes.SetNum(ColumnCount + 1);
    for (int32 i = 0; i < ColumnCount + 1; ++i) {
      Ar << ColumnSizes[i];
    }

    // Read column names
    ColumnNames.SetNum(ColumnCount + 1);
    for (int32 i = 0; i < ColumnCount + 1; ++i) {
      int16 NameLength;
      Ar << NameLength;

      if (NameLength > 0) {
        TArray<uint8> NameBuffer;
        NameBuffer.SetNumUninitialized(NameLength);
        Ar.Serialize(NameBuffer.GetData(), NameLength);
        ColumnNames[i] =
            FString(NameLength, (const ANSICHAR *)NameBuffer.GetData());
      } else {
        ColumnNames[i] = FString();
      }
    }

    // Initialize cells array
    Cells.SetNum(RowCount - 1);

    // Read first column (row names)
    for (int32 i = 0; i < RowCount - 1; ++i) {
      Cells[i].SetNum(ColumnCount);

      int16 CellLength;
      Ar << CellLength;

      if (CellLength > 0) {
        TArray<uint8> CellBuffer;
        CellBuffer.SetNumUninitialized(CellLength);
        Ar.Serialize(CellBuffer.GetData(), CellLength);
        Cells[i][0] =
            FString(CellLength, (const ANSICHAR *)CellBuffer.GetData());
      } else {
        Cells[i][0] = FString();
      }
    }

    // Read remaining cells
    for (int32 i = 0; i < RowCount - 1; ++i) {
      for (int32 j = 1; j < ColumnCount; ++j) {
        int16 CellLength;
        Ar << CellLength;

        if (CellLength > 0) {
          TArray<uint8> CellBuffer;
          CellBuffer.SetNumUninitialized(CellLength);
          Ar.Serialize(CellBuffer.GetData(), CellLength);
          Cells[i][j] =
              FString(CellLength, (const ANSICHAR *)CellBuffer.GetData());
        } else {
          Cells[i][j] = FString();
        }
      }
    }

    UE_LOG(LogTemp, Log, TEXT("Loaded STB: %d rows, %d columns"), Cells.Num(),
           ColumnCount);
    return true;
  }

  FString GetCell(int32 Row, int32 Column) const {
    if (Row >= 0 && Row < Cells.Num() && Column >= 0 &&
        Column < Cells[Row].Num()) {
      return Cells[Row][Column];
    }
    return FString();
  }

  int32 GetRowCount() const { return Cells.Num(); }
  int32 GetColumnCount() const { return Cells.Num() > 0 ? Cells[0].Num() : 0; }
};

/**
 * TileSet Brush Definition
 * Defines a "brush" (group of textures) for terrain painting
 */
struct FRoseTileBrush {
  uint8 MinimumBrush;
  uint8 MaximumBrush;
  int32 TileNumber0;
  uint8 TileCount0;
  int32 TileNumberF;
  uint8 TileCountF;
  int32 TileNumber;
  uint8 TileCount;
  int32 Direction;
};

/**
 * TileSet (TSI file loaded from STB)
 * Defines brushes and transition chains for intelligent texture blending
 */
struct FRoseTileSet {
  TArray<FRoseTileBrush> Brushes;
  TArray<TArray<uint8>> Chains; // [MaxBrush][MaxBrush]

  bool LoadFromSTB(const FRoseSTB &STB) {
    if (STB.GetRowCount() < 2) {
      UE_LOG(LogTemp, Error, TEXT("TileSet STB has insufficient rows"));
      return false;
    }

    // Row 0, Column 2: Brush count
    int32 BrushCount = FCString::Atoi(*STB.GetCell(0, 2));
    if (BrushCount <= 0) {
      UE_LOG(LogTemp, Error, TEXT("Invalid brush count: %d"), BrushCount);
      return false;
    }

    // Rows 1..BrushCount: Brush definitions
    Brushes.SetNum(BrushCount);
    for (int32 i = 0; i < BrushCount; ++i) {
      int32 Row = i + 1;
      FRoseTileBrush &Brush = Brushes[i];

      Brush.MinimumBrush = FCString::Atoi(*STB.GetCell(Row, 2));
      Brush.MaximumBrush = FCString::Atoi(*STB.GetCell(Row, 3));
      Brush.TileNumber0 = FCString::Atoi(*STB.GetCell(Row, 4));
      Brush.TileCount0 = FCString::Atoi(*STB.GetCell(Row, 5));
      Brush.TileNumberF = FCString::Atoi(*STB.GetCell(Row, 6));
      Brush.TileCountF = FCString::Atoi(*STB.GetCell(Row, 7));
      Brush.TileNumber = FCString::Atoi(*STB.GetCell(Row, 8));
      Brush.TileCount = FCString::Atoi(*STB.GetCell(Row, 9));
      Brush.Direction = FCString::Atoi(*STB.GetCell(Row, 10));
    }

    // Next row after brushes: max brush count for chains
    int32 ChainRow = BrushCount + 1;
    if (ChainRow >= STB.GetRowCount()) {
      // No chains defined, just brushes
      UE_LOG(LogTemp, Log, TEXT("Loaded TileSet: %d brushes, no chains"),
             BrushCount);
      return true;
    }

    int32 MaxBrushCount = FCString::Atoi(*STB.GetCell(ChainRow, 2));
    if (MaxBrushCount > 0) {
      // Initialize chains matrix
      Chains.SetNum(MaxBrushCount);
      for (int32 i = 0; i < MaxBrushCount; ++i) {
        Chains[i].SetNum(MaxBrushCount);

        int32 DataRow = ChainRow + 1 + i;
        if (DataRow < STB.GetRowCount()) {
          for (int32 j = 0; j < MaxBrushCount && j + 2 < STB.GetColumnCount();
               ++j) {
            Chains[i][j] = FCString::Atoi(*STB.GetCell(DataRow, j + 2));
          }
        }
      }
    }

    UE_LOG(LogTemp, Log, TEXT("Loaded TileSet: %d brushes, %dx%d chains"),
           BrushCount, MaxBrushCount, MaxBrushCount);
    return true;
  }
};

/**
 * HIM (Heightmap) Format
 */
struct FRoseHIM {
  int32 Width = 0;
  int32 Height = 0;
  int32 GridCount = 0;
  float GridSize = 0.0f;
  TArray<float> Heights;

  void Serialize(FArchive &Ar) {
    Ar << Width;
    Ar << Height;
    Ar << GridCount;
    Ar << GridSize;

    // Sanity check dimensions
    if (Width <= 0 || Height <= 0 || Width > 256 || Height > 256) {
      // Valid HIM files are usually 65x65
      return;
    }

    int32 TotalPoints = Width * Height;
    Heights.SetNumUninitialized(TotalPoints);

    // Read straight float array
    Ar.Serialize(Heights.GetData(), TotalPoints * sizeof(float));

    // Note: Skipping min/max patches generation as we will recalculate them in
    // UE
  }

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;

    FMemoryReader Reader(Data, true);
    FRoseArchive Ar(Reader);
    Serialize(Ar);
    return true;
  }
};

/**
 * ZON (Zone) Format
 */
struct FRoseZoneTile {
  int32 Layer1; // BaseID1 in C# editor
  int32 Layer2; // BaseID2 in C# editor
  int32 Offset1;
  int32 Offset2;
  int32 Blending; // bool as int32
  int32 Rotation; // Enum as int32
  int32 TileType;

  // Helper methods to get final texture indices
  int32 GetTextureID1() const { return Layer1 + Offset1; }
  int32 GetTextureID2() const { return Layer2 + Offset2; }
  bool IsBlending() const { return Blending > 0; }

  friend FArchive &operator<<(FArchive &Ar, FRoseZoneTile &Tile) {
    Ar << Tile.Layer1 << Tile.Layer2 << Tile.Offset1 << Tile.Offset2;
    Ar << Tile.Blending << Tile.Rotation << Tile.TileType;
    return Ar;
  }
};

struct FRoseZON {
  // Block Types
  enum class EZoneBlock : int32 {
    Info = 0,
    SpawnPoints = 1,
    Textures = 2,
    Tiles = 3,
    Economy = 4
  };

  // Info Block
  int32 ZoneType;
  int32 Width;
  int32 Height;
  int32 GridCount;
  float GridSize;
  FIntVector StartPosition; // 2x int32
  // Positions array skipped for struct simplicity, can be read on demand

  // Texture List
  TArray<FString> Textures;

  // Tiles
  TArray<FRoseZoneTile> Tiles;

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;

    FMemoryReader MemReader(Data, true);
    FRoseArchive Ar(MemReader);

    int32 BlockCount = 0;
    Ar << BlockCount;

    // Read Blocks Offsets
    struct FBlockInfo {
      EZoneBlock Type;
      int32 Offset;
    };
    TArray<FBlockInfo> Blocks;

    for (int32 i = 0; i < BlockCount; ++i) {
      int32 TypeVal, Offset;
      Ar << TypeVal << Offset;
      Blocks.Add({(EZoneBlock)TypeVal, Offset});
    }

    // Process Blocks
    for (const auto &Block : Blocks) {
      Ar.Seek(Block.Offset);

      switch (Block.Type) {
      case EZoneBlock::Info:
        Ar << ZoneType << Width << Height << GridCount << GridSize;
        Ar << StartPosition.X << StartPosition.Y;
        break;

      case EZoneBlock::Textures: {
        int32 Count;
        Ar << Count;
        Textures.Empty(Count);
        for (int32 i = 0; i < Count; ++i) {
          Textures.Add(Ar.ReadByteString());
        }
      } break;

      case EZoneBlock::Tiles: {
        int32 Count;
        Ar << Count;
        Tiles.SetNum(Count);
        for (int32 i = 0; i < Count; ++i) {
          Ar << Tiles[i];
        }
      } break;

      default:
        break;
      }
    }

    return true;
  }
};

/**
 * TIL (Tile) Format
 */
struct FRoseTilePatch {
  uint8 Brush;
  uint8 TileIndex;
  uint8 TileSet;
  int32 Tile;

  friend FArchive &operator<<(FArchive &Ar, FRoseTilePatch &Patch) {
    Ar << Patch.Brush << Patch.TileIndex << Patch.TileSet << Patch.Tile;
    return Ar;
  }
};

struct FRoseTIL {
  int32 Width;  // 16
  int32 Height; // 16
  TArray<FRoseTilePatch> Patches;

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;

    FMemoryReader Reader(Data, true);
    Reader << Width << Height;

    if (Width <= 0 || Height <= 0 || Width > 128 || Height > 128)
      return false;

    int32 Total = Width * Height;
    Patches.SetNum(Total);

    for (int h = 0; h < Height; h++) {
      for (int w = 0; w < Width; w++) {
        Reader << Patches[h * Width + w];
      }
    }
    return true;
  }
};

/**
 * IFO (Map Data) Format
 */
struct FRoseMapObject {
  FString Name;
  int16 WarpID;
  int16 EventID;
  int32 ObjectType;       // Index to ZSC
  int32 ObjectID;         // Index in ZSC
  FIntVector MapPosition; // 2x int32
  FQuat Rotation;
  FVector Position;
  FVector Scale;

  void Serialize(FRoseArchive &Ar) {
    Name = Ar.ReadRoseString(); // Was ReadByteString, but ROSE uses CGameStr
                                // almost everywhere
    Ar << WarpID << EventID << ObjectType << ObjectID;
    Ar << MapPosition.X << MapPosition.Y;

    // Manual float serialization for LWC (UE5 uses doubles)
    // Reference Plugin reads FQuat directly (4 floats).
    // ALIGNMENT FIXED: Reading 2 ints (MapX, MapY).

    float R1, R2, R3, R4;
    Ar << R1 << R2 << R3 << R4;
    // Strict Reference (rtuRotation): (-X, Y, -Z, W)
    // NOTE: If this fails, try (X, Y, Z, W) original.
    Rotation = FQuat(-R1, R2, -R3, R4);
    Rotation.Normalize();

    float P1, P2, P3;
    Ar << P1 << P2 << P3;
    // Strict Reference (rtuPosition): (X, -Y, Z)
    // Assumes ROSE is Z-Up (like UE), just Y-Inverted.
    Position = FVector(P1, -P2, P3);

    float S1, S2, S3;
    Ar << S1 << S2 << S3;
    Scale = FVector(S1, S2, S3);
  }
};

struct FRoseIFO {
  // Block Types (Subset)
  enum class EMapBlock : int32 {
    MapInformation = 0,
    Object = 1,
    NPC = 2,
    Building = 3,
    Sound = 4,
    Effect = 5,
    Animation = 6,
    MonsterSpawn = 7,
    WaterPlane = 8,
    WarpPoint = 9,
    CollisionObject = 10,
    EventObject = 11,
    WaterPatch = 12
  };

  FString ZoneName;
  TArray<FRoseMapObject> Objects;
  TArray<FRoseMapObject>
      Buildings; // Using same struct as they share properties
  TArray<FRoseMapObject> Animations; // Type 6

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;

    FMemoryReader MemReader(Data, true);
    FRoseArchive Ar(MemReader);

    int32 BlockCount = 0;
    Ar << BlockCount;

    struct FBlockInfo {
      EMapBlock Type;
      int32 Offset;
    };
    TArray<FBlockInfo> Blocks;

    for (int32 i = 0; i < BlockCount; ++i) {
      int32 TypeVal, Offset;
      Ar << TypeVal << Offset;
      Blocks.Add({(EMapBlock)TypeVal, Offset});
    }

    for (const auto &Block : Blocks) {
      Ar.Seek(Block.Offset);

      // Info Block Special Case
      if (Block.Type == EMapBlock::MapInformation) {
        int32 MapX, MapY, ZoneX, ZoneY;
        Ar << MapX << MapY << ZoneX << ZoneY;

        // Matrix 4x4 skip
        Ar.Seek(Ar.Tell() + 16 * 4);

        ZoneName = Ar.ReadRoseString();

      } else if (Block.Type == EMapBlock::Object ||
                 Block.Type == EMapBlock::Building ||
                 Block.Type == EMapBlock::Animation) {
        // Skip Block Specific Header (WaterPlane has WaterSize, others just
        // Count) Objects don't have extra header, just count.
        int32 Count;
        Ar << Count;

        for (int32 k = 0; k < Count; ++k) {
          FRoseMapObject Obj;
          Obj.Serialize(Ar);

          if (Block.Type == EMapBlock::Object)
            Objects.Add(Obj);
          else if (Block.Type == EMapBlock::Building)
            Buildings.Add(Obj);
          else if (Block.Type == EMapBlock::Animation)
            Animations.Add(Obj);
        }
      }
    }
    return true;
  }
};

/**
 * ZSC (Zone Static Component) Format - Model List
 */
struct FRoseZSC {
  struct FMeshEntry {
    FString MeshPath;
  };

  struct FMaterialEntry {
    FString TexturePath;
    bool AlphaEnabled;
    bool TwoSided;
    int32 AlphaTest;
    int32 AlphaRef;
    int32 ZTest;
    int32 ZWrite;
    int32 BlendType;
    int32 Specular;
    float AlphaValue;
    int32 GlowType;
    float Red;
    float Green;
    float Blue;
  };

  struct FObjectPart {
    int16 MeshIndex;
    int16 MaterialIndex;
    FVector3f Position = FVector3f::ZeroVector;
    FQuat4f Rotation = FQuat4f::Identity;
    FVector3f Scale = FVector3f::OneVector;
    FQuat4f AxisRotation = FQuat4f::Identity;
    int16 ParentID = -1;
    int16 CollisionMode = 0;
    int16 BoneIndex = 0;
    int16 DummyIndex = 0;
    FString AnimPath; // ConstantAnimation (ZSC property 30)
  };

  struct FObjectEntry {
    TArray<FObjectPart> Parts;
    FVector3f BBMin = FVector3f::ZeroVector;
    FVector3f BBMax = FVector3f::ZeroVector;
  };

  TArray<FMeshEntry> Meshes;
  TArray<FMaterialEntry> Materials;
  TArray<FString> Effects;
  TArray<FObjectEntry> Objects;

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;

    FMemoryReader MemReader(Data, true);
    FRoseArchive Ar(MemReader);

    // Header Check - Some clients omit "ZSC1"
    char Header[5] = {0};
    int64 StartPos = Ar.Tell();
    Ar.Serialize(Header, 4);

    // If not "ZSC1", assume headerless and rewind
    if (FString(Header) != TEXT("ZSC1")) {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("ZSC1 Header not found (Read: %s). Assuming "
                  "legacy/headerless format."),
             *FString(Header));
      Ar.Seek(StartPos);
    }

    uint16 MeshCount = 0;
    Ar << MeshCount;
    Meshes.Empty(MeshCount);
    for (int32 i = 0; i < MeshCount; ++i) {
      Meshes.Add({Ar.ReadRoseString()});
    }

    uint16 MatCount = 0;
    Ar << MatCount;
    Materials.Empty(MatCount);
    for (int32 i = 0; i < MatCount; ++i) {
      FMaterialEntry Mat;
      Mat.TexturePath = Ar.ReadRoseString();

      int16 nIsSkin, nIsAlpha, nIs2Side;
      int16 nAlphaTest, nAlphaRef, nZTest, nZWrite, nBlendType, nSpecular;
      float fAlphaValue;
      int16 nGlowType;
      float fGlowColor[3];

      Ar << nIsSkin << nIsAlpha << nIs2Side;
      Ar << nAlphaTest << nAlphaRef << nZTest << nZWrite << nBlendType
         << nSpecular;
      Ar << fAlphaValue;
      Ar << nGlowType;
      Ar << fGlowColor[0] << fGlowColor[1] << fGlowColor[2];

      Mat.AlphaEnabled = (nIsAlpha != 0);
      Mat.TwoSided = (nIs2Side != 0);
      Mat.AlphaTest = nAlphaTest;
      Mat.AlphaRef = nAlphaRef;
      Mat.ZTest = nZTest;
      Mat.ZWrite = nZWrite;
      Mat.BlendType = nBlendType;
      Mat.Specular = nSpecular;
      Mat.AlphaValue = fAlphaValue;
      Mat.GlowType = nGlowType;
      Mat.Red = fGlowColor[0];
      Mat.Green = fGlowColor[1];
      Mat.Blue = fGlowColor[2];

      Materials.Add(Mat);
    }

    uint16 EffectCount = 0;
    Ar << EffectCount;
    Effects.Empty(EffectCount);
    for (int32 i = 0; i < EffectCount; ++i) {
      Effects.Add(Ar.ReadRoseString());
    }

    uint16 ObjCount = 0;
    Ar << ObjCount;

    Objects.Empty(ObjCount);

    for (int32 i = 0; i < ObjCount; ++i) {
      FObjectEntry Obj;
      int32 Radius, CX, CY;
      Ar << Radius << CX << CY;

      uint16 PartCount = 0;
      Ar << PartCount;

      // CRITICAL FIX: If PartCount is 0, the object is empty/dummy.
      // Original source returns early here and does NOT read DummyCount or
      // BBox.
      if (PartCount == 0) {
        Objects.Add(Obj);
        continue;
      }

      for (int32 j = 0; j < PartCount; ++j) {
        FObjectPart Part;
        uint16 MeshIdx, MatIdx;
        Ar << MeshIdx << MatIdx;
        Part.MeshIndex = (int16)MeshIdx;
        Part.MaterialIndex = (int16)MatIdx;

        // Property Loop
        uint8 btTag = 0;
        Ar << btTag;
        int32 PropSafety = 0;
        while (btTag != 0 && PropSafety++ < 2000) {
          uint8 btLen = 0;
          Ar << btLen;

          switch (btTag) {
          case 1: // Position
          {
            float X, Y, Z;
            Ar << X << Y << Z;
            // Apply rtuPosition: (X, -Y, Z)
            Part.Position.X = X;
            Part.Position.Y = -Y;
            Part.Position.Z = Z;
          } break;
          case 2: // Rotation
          {
            // ROSE ZSC Rot is W, X, Y, Z (Confirmed by Ref Plugin readBadQuat)
            float W, X, Y, Z;
            Ar << W << X << Y << Z;
            // Construct Identity-mapped Quat: FQuat(X, Y, Z, W)
            // Apply rtuRotation: (-X, Y, -Z, W)
            Part.Rotation = FQuat4f(-X, Y, -Z, W);
            Part.Rotation.Normalize();
          } break;
          case 3: // Scale
            Ar << Part.Scale.X << Part.Scale.Y << Part.Scale.Z;
            break;
          case 4: // AxisRotation
          {
            // ROSE ZSC Rot is W, X, Y, Z
            float W, X, Y, Z;
            Ar << W << X << Y << Z;
            // Apply rtuRotation: (-X, Y, -Z, W)
            Part.AxisRotation = FQuat4f(-X, Y, -Z, W);
            Part.AxisRotation.Normalize();
          } break;
          case 5: // BoneIndex (was incorrectly ParentID)
            Ar << Part.BoneIndex;
            break;
          case 6: // DummyIndex (was incorrectly CollisionMode)
            Ar << Part.DummyIndex;
            break;
          case 7: // Parent
            Ar << Part.ParentID;
            break;
          case 8: // Animation (string, use btLen to skip)
            Ar.Seek(Ar.Tell() + btLen);
            break;
          case 29: // Collision
            Ar << Part.CollisionMode;
            break;
          case 30: // ConstantAnimation (string path to ZMO)
          {
            TArray<uint8> Buf;
            Buf.SetNum(btLen);
            Ar.Serialize(Buf.GetData(), btLen);
            Buf.Add(0); // null-terminate
            Part.AnimPath = FString(ANSI_TO_TCHAR((ANSICHAR *)Buf.GetData()));
          } break;
          case 31: // VisibleRangeSet
          case 32: // UseLightmap
            Ar.Seek(Ar.Tell() + btLen);
            break;
          default:
            Ar.Seek(Ar.Tell() + btLen);
            break;
          }
          Ar << btTag;
        }
        Obj.Parts.Add(Part);
      }

      uint16 DummyCount = 0;
      Ar << DummyCount;
      for (int32 k = 0; k < DummyCount; ++k) {
        uint16 EftIdx, EftType;
        Ar << EftIdx << EftType;

        // Dummy Properties Loop
        uint8 btTag = 0;
        Ar << btTag;
        int32 DummyPropSafety = 0;
        while (btTag != 0 && DummyPropSafety++ < 100) {
          uint8 btLen = 0;
          Ar << btLen;

          switch (btTag) {
          case 1: {
            float x, y, z;
            Ar << x << y << z;
          } break; // POS
          case 2: {
            float w, x, y, z;
            Ar << w << x << y << z;
          } break; // ROT
          case 3: {
            float x, y, z;
            Ar << x << y << z;
          } break;  // SCALE
          case 7: { // Parent (was incorrectly case 5)
            int16 p;
            Ar << p;
          } break;
          default:
            Ar.Seek(Ar.Tell() + btLen);
            break;
          }
          Ar << btTag;
        }
      }

      // Bounding Box (6 floats)
      Ar << Obj.BBMin.X << Obj.BBMin.Y << Obj.BBMin.Z;
      Ar << Obj.BBMax.X << Obj.BBMax.Y << Obj.BBMax.Z;

      Objects.Add(Obj);
    }

    return true;
  }
};

/**
 * ZMS (Static Mesh) Format
 */
struct FRoseZMS {
  FString FormatString;
  int32 Format = 0;

  // Bounding Box (floats for binary compatibility)
  FVector3f Min;
  FVector3f Max;

  int32 BoneCount = 0;
  int32 VertCount = 0;

  struct FVertex {
    FVector3f Position;
    FVector3f Normal;
    FVector2f UV1;
    FVector2f UV2;
    FVector2f UV3;
    FVector2f UV4;
    FColor Color;
    FVector4f Weights;
    FIntVector4 Indices;
  };
  TArray<FVertex> Vertices;

  int32 FaceCount = 0;
  TArray<uint16> Indices;

  int32 MaterialID = 0;

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;

    FMemoryReader Reader(Data, true);
    FRoseArchive Ar(Reader);

    FormatString = Ar.ReadRoseString();
    UE_LOG(LogRoseImporter, Display, TEXT("ZMS FormatString: %s"),
           *FormatString);

    Ar << Format;
    UE_LOG(LogRoseImporter, Display, TEXT("ZMS Format Flags: %d"), Format);

    // Bounding Box (6 floats)
    // Manually serialize components to ensure 32-bit float size (LWC safety)
    Ar << Min.X << Min.Y << Min.Z;
    Ar << Max.X << Max.Y << Max.Z;

    uint16 Count16 = 0;
    Ar << Count16;
    BoneCount = Count16;

    if (BoneCount > 0) {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("ZMS has %d bones. Skipping (Not Implemented)."), BoneCount);
      return false;
    }

    Ar << Count16;
    VertCount = Count16;

    // Log minimal
    UE_LOG(LogRoseImporter, Display,
           TEXT("ZMS Load: Flags=%d, Bones=%d, Verts=%d"), Format, BoneCount,
           VertCount);

    if (VertCount > 65535 || VertCount < 0) {
      UE_LOG(LogRoseImporter, Error, TEXT("Suspicious VertCount %d. Aborting."),
             VertCount);
      return false;
    }

    Vertices.SetNum(VertCount);

    // Correct Bitmasks from Revise
    bool bHasPos = (Format & (1 << 1)) != 0;
    bool bHasNorm = (Format & (1 << 2)) != 0;
    bool bHasColor = (Format & (1 << 3)) != 0;
    bool bHasSkin = (Format & (1 << 4)) != 0; // BlendWeight
    bool bHasBone = (Format & (1 << 5)) != 0; // BlendIndex
    bool bHasTan = (Format & (1 << 6)) != 0;
    bool bHasUV1 = (Format & (1 << 7)) != 0;
    bool bHasUV2 = (Format & (1 << 8)) != 0;
    bool bHasUV3 = (Format & (1 << 9)) != 0;
    bool bHasUV4 = (Format & (1 << 10)) != 0;

    UE_LOG(LogRoseImporter, Display,
           TEXT("ZMS Features: P=%d N=%d C=%d Skin=%d Bone=%d Tan=%d UV=%d"),
           bHasPos, bHasNorm, bHasColor, bHasSkin, bHasBone, bHasTan, bHasUV1);

    if (bHasPos) {
      for (int i = 0; i < VertCount; ++i)
        Ar << Vertices[i].Position.X << Vertices[i].Position.Y
           << Vertices[i].Position.Z;
    }
    if (bHasNorm) {
      for (int i = 0; i < VertCount; ++i)
        Ar << Vertices[i].Normal.X << Vertices[i].Normal.Y
           << Vertices[i].Normal.Z;
    }
    if (bHasColor) {
      for (int i = 0; i < VertCount; ++i) {
        // Red, Green, Blue, Alpha floats
        float r, g, b, a;
        Ar << a << r << g << b; // Correct order from Revise
        Vertices[i].Color = FColor(r * 255, g * 255, b * 255, a * 255);
      }
    }
    if (bHasSkin) { // Weights
      for (int i = 0; i < VertCount; ++i) {
        float W1, W2, W3, W4;
        Ar << W1 << W2 << W3 << W4;
        Vertices[i].Weights = FVector4f(W1, W2, W3, W4);
      }
    }
    if (bHasBone) { // Indices
      for (int i = 0; i < VertCount; ++i) {
        uint16 I1, I2, I3, I4;
        Ar << I1 << I2 << I3 << I4;
        Vertices[i].Indices.X = I1;
        Vertices[i].Indices.Y = I2;
        Vertices[i].Indices.Z = I3;
        Vertices[i].Indices.W = I4;
      }
    }
    if (bHasTan) {
      float TX, TY, TZ;
      for (int i = 0; i < VertCount; ++i)
        Ar << TX << TY << TZ;
    }
    if (bHasUV1) {
      for (int i = 0; i < VertCount; ++i)
        Ar << Vertices[i].UV1.X << Vertices[i].UV1.Y;
    }
    if (bHasUV2) {
      for (int i = 0; i < VertCount; ++i)
        Ar << Vertices[i].UV2.X << Vertices[i].UV2.Y;
    }
    if (bHasUV3) {
      for (int i = 0; i < VertCount; ++i)
        Ar << Vertices[i].UV3.X << Vertices[i].UV3.Y;
    }
    if (bHasUV4) {
      for (int i = 0; i < VertCount; ++i)
        Ar << Vertices[i].UV4.X << Vertices[i].UV4.Y;
    }

    uint16 FC = 0;
    Ar << FC;
    FaceCount = FC;
    UE_LOG(LogRoseImporter, Display, TEXT("ZMS Faces: %d"), FaceCount);

    Indices.SetNum(FaceCount * 3);
    Ar.Serialize(Indices.GetData(), Indices.Num() * sizeof(uint16));

    uint16 MatID = 0;
    Ar << MatID;
    MaterialID = MatID;

    // Strips (optional) - Check file end?

    return true;
  }
};

/**
 * ROSE Skeleton (ZMD)
 */
struct FRoseBone {
  int32 ParentID;
  FString Name;
  FVector Position;
  FQuat Rotation;
};

struct FRoseZMD {
  FString FormatString;
  TArray<FRoseBone> Bones;
  TArray<FRoseBone> Dummies;

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath)) {
      return false;
    }
    FMemoryReader R(Data, true);
    FRoseArchive Ar(R);

    FormatString = Ar.ReadRoseString();

    uint32 BoneCount = 0;
    Ar << BoneCount;

    Bones.SetNum(BoneCount);
    for (uint32 i = 0; i < BoneCount; ++i) {
      Ar << Bones[i].ParentID;
      Bones[i].Name = Ar.ReadRoseString();
      // ROSE Position is usually cm
      Ar << Bones[i].Position.X << Bones[i].Position.Y << Bones[i].Position.Z;
      // ROSE Quat is w, x, y, z
      float w, x, y, z;
      Ar << w << x << y << z;
      Bones[i].Rotation = FQuat(x, y, z, w);
    }

    uint32 DummyCount = 0;
    Ar << DummyCount;
    Dummies.SetNum(DummyCount);
    for (uint32 i = 0; i < DummyCount; ++i) {
      Dummies[i].Name = Ar.ReadRoseString();
      Ar << Dummies[i].ParentID;
      Ar << Dummies[i].Position.X << Dummies[i].Position.Y
         << Dummies[i].Position.Z;
      float w, x, y, z;
      Ar << w << x << y << z;
      Dummies[i].Rotation = FQuat(x, y, z, w);
    }
    return true;
  }
};

/**
 * ROSE Animation (ZMO)
 * Channel types use bitfield values matching the ROSE engine:
 *   Position = 1<<1 = 2
 *   Rotation = 1<<2 = 4
 *   Scale    = 1<<10 = 1024
 */
struct FRoseAnimChannel {
  int32 Type; // Bitfield: 2=Position, 4=Rotation, 1024=Scale
  int32 BoneID;
  TArray<FVector> PosKeys;
  TArray<FQuat> RotKeys;
  TArray<FVector> ScaleKeys;
};

struct FRoseZMO {
  FString FormatString;
  int32 FPS = 0;
  int32 FrameCount = 0;
  int32 ChannelCount = 0;
  TArray<FRoseAnimChannel> Channels;

  bool Load(const FString &FilePath) {
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *FilePath))
      return false;
    FMemoryReader R(Data, true);
    FRoseArchive Ar(R);

    FormatString = Ar.ReadRoseString();
    Ar << FPS << FrameCount << ChannelCount;

    Channels.SetNum(ChannelCount);
    for (int i = 0; i < ChannelCount; ++i) {
      Ar << Channels[i].Type << Channels[i].BoneID;
    }

    // Read frame data (frames are interleaved across channels)
    for (int f = 0; f < FrameCount; ++f) {
      for (int i = 0; i < ChannelCount; ++i) {
        if (Channels[i].Type == 2) { // Position
          FVector Pos;
          Ar << Pos.X << Pos.Y << Pos.Z;
          // Apply rtuPosition: (X, -Y, Z)
          Pos.Y = -Pos.Y;
          Channels[i].PosKeys.Add(Pos);
        } else if (Channels[i].Type == 4) { // Rotation
          float W, X, Y, Z;
          Ar << W << X << Y << Z;
          // Apply rtuRotation: (-X, Y, -Z, W)
          Channels[i].RotKeys.Add(FQuat(-X, Y, -Z, W));
        } else if (Channels[i].Type == 1024) { // Scale
          FVector S;
          Ar << S.X << S.Y << S.Z;
          Channels[i].ScaleKeys.Add(S);
        } else {
          // Unknown channel type - skip based on common sizes
          // Most unknown types are single floats or vectors
          UE_LOG(LogRoseImporter, Warning, TEXT("ZMO: Unknown channel type %d"),
                 Channels[i].Type);
        }
      }
    }
    return true;
  }
};
