#pragma once

#include "CoreMinimal.h"
#include "RoseFormats.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

struct FZoneRow {
  int32 ID;
  FString Name;
  FString ZonPath;
  FString DecoPath;
  FString CnstPath;
};

class SRoseZoneBrowser : public SCompoundWidget {
public:
  SLATE_BEGIN_ARGS(SRoseZoneBrowser) {}
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs);

  // Open the dialog and return the selected row (or nullptr if canceled)
  static TSharedPtr<FZoneRow> PickZone(const FRoseSTB &StbData);

private:
  TArray<TSharedPtr<FZoneRow>> ZoneRows;
  TSharedPtr<SListView<TSharedPtr<FZoneRow>>> ListView;
  TSharedPtr<FZoneRow> SelectedRow;
  TWeakPtr<SWindow> ParentWindow;

  // Filter text
  FString FilterString;
  void OnFilterTextChanged(const FText &InFilterText);
  void RefreshList();
  TArray<TSharedPtr<FZoneRow>> FilteredRows;

  TSharedRef<ITableRow>
  OnGenerateRow(TSharedPtr<FZoneRow> Item,
                const TSharedRef<STableViewBase> &OwnerTable);
  void OnSelectionChanged(TSharedPtr<FZoneRow> Item,
                          ESelectInfo::Type SelectInfo);
  FReply OnImportClicked();
  FReply OnCancelClicked();
};
