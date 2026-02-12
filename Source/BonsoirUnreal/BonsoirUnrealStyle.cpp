#include "BonsoirUnrealStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "Slate/SlateGameResources.h"
#include "Styling/SlateStyleRegistry.h"


TSharedPtr<FSlateStyleSet> FBonsoirUnrealStyle::StyleInstance = nullptr;

void FBonsoirUnrealStyle::Initialize() {
  if (!StyleInstance.IsValid()) {
    StyleInstance = Create();
    FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
  }
}

void FBonsoirUnrealStyle::Shutdown() {
  FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
  ensure(StyleInstance.IsUnique());
  StyleInstance.Reset();
}

void FBonsoirUnrealStyle::ReloadTextures() {
  if (FSlateApplication::IsInitialized()) {
    FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
  }
}

const ISlateStyle &FBonsoirUnrealStyle::Get() { return *StyleInstance; }

FName FBonsoirUnrealStyle::GetStyleSetName() {
  static FName StyleSetName(TEXT("BonsoirUnrealStyle"));
  return StyleSetName;
}

TSharedRef<FSlateStyleSet> FBonsoirUnrealStyle::Create() {
  TSharedRef<FSlateStyleSet> Style =
      MakeShareable(new FSlateStyleSet("BonsoirUnrealStyle"));

  // Find plugin content directory
  TSharedPtr<IPlugin> Plugin =
      IPluginManager::Get().FindPlugin("BonsoirUnreal");
  if (Plugin.IsValid()) {
    Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Content"));
  }

  // Register icon - looking for icon in Content/Collections/Bonsoir.ico
  Style->Set(
      "BonsoirUnreal.ImportZoneAction",
      new FSlateImageBrush(
          Style->RootToContentDir(TEXT("Collections/Bonsoir"), TEXT(".png")),
          FVector2D(40.0f, 40.0f)));

  return Style;
}
