#include "SRoseZoneBrowser.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Views/STableRow.h"

void SRoseZoneBrowser::Construct(const FArguments &InArgs) {
  TSharedRef<SVerticalBox> MainLayout = SNew(SVerticalBox);

  // Filter Box
  MainLayout->AddSlot().AutoHeight().Padding(
      0, 0, 0,
      10)[SNew(SSearchBox)
              .OnTextChanged(this, &SRoseZoneBrowser::OnFilterTextChanged)];

  // List View
  MainLayout->AddSlot().FillHeight(
      1.0f)[SAssignNew(ListView, SListView<TSharedPtr<FZoneRow>>)
                .ItemHeight(24)
                .ListItemsSource(&FilteredRows)
                .OnGenerateRow(this, &SRoseZoneBrowser::OnGenerateRow)
                .OnSelectionChanged(this, &SRoseZoneBrowser::OnSelectionChanged)
                .HeaderRow(SNew(SHeaderRow) +
                           SHeaderRow::Column("ID")
                               .DefaultLabel(FText::FromString("ID"))
                               .FixedWidth(50) +
                           SHeaderRow::Column("Name")
                               .DefaultLabel(FText::FromString("Name"))
                               .FillWidth(0.4f) +
                           SHeaderRow::Column("ZON")
                               .DefaultLabel(FText::FromString("ZON File"))
                               .FillWidth(0.4f))];

  // Buttons
  MainLayout->AddSlot()
      .AutoHeight()
      .Padding(0, 10, 0, 0)
      .HAlign(HAlign_Right)
          [SNew(SHorizontalBox) +
           SHorizontalBox::Slot().AutoWidth().Padding(
               0, 0, 10,
               0)[SNew(SButton)
                      .Text(FText::FromString("Import"))
                      .IsEnabled_Lambda(
                          [this]() { return SelectedRow.IsValid(); })
                      .OnClicked(this, &SRoseZoneBrowser::OnImportClicked)] +
           SHorizontalBox::Slot().AutoWidth()
               [SNew(SButton)
                    .Text(FText::FromString("Cancel"))
                    .OnClicked(this, &SRoseZoneBrowser::OnCancelClicked)]];

  ChildSlot[SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .Padding(10)[MainLayout]];
}

TSharedPtr<FZoneRow> SRoseZoneBrowser::PickZone(const FRoseSTB &StbData) {
  // Create window
  TSharedRef<SWindow> Window =
      SNew(SWindow)
          .Title(FText::FromString("Select Zone to Import"))
          .ClientSize(FVector2D(600, 500))
          .SupportsMinimize(false)
          .SupportsMaximize(false);

  TSharedRef<SRoseZoneBrowser> Browser = SNew(SRoseZoneBrowser);
  Browser->ParentWindow = Window;

  // Parse STB data into rows
  // Identify ZON column dynamically
  int32 ZonCol = -1;
  int32 NameCol = 1; // Default JDT01

  // Find "ZON" column in header if exists, usually it's around index 3
  // Header/Data heuristics to find ZON column
  if (StbData.GetRowCount() > 1) {
    // 1. Try to find a column that actually contains .zon files in data
    for (int32 c = 0; c < StbData.GetColumnCount(); ++c) {
      // Check first few data rows (skip header 0)
      for (int32 r = 1; r < FMath::Min(10, StbData.GetRowCount()); ++r) {
        if (StbData.GetCell(r, c).ToLower().EndsWith(".zon")) {
          ZonCol = c;
          goto FoundZonCol; // Break out of nested loop
        }
      }
    }

    // 2. Fallback to Header Exact Match
    for (int32 c = 0; c < StbData.GetColumnCount(); ++c) {
      if (StbData.GetCell(0, c).ToUpper() == "ZON") {
        ZonCol = c;
        goto FoundZonCol;
      }
    }
  }

FoundZonCol:
  if (ZonCol == -1)
    ZonCol = 3; // Fallback default

  for (int32 i = 0; i < StbData.GetRowCount(); ++i) {
    // Basic validation: row must have a ZON file
    FString ZFile = StbData.GetCell(i, ZonCol);
    if (ZFile.IsEmpty())
      continue;

    TSharedPtr<FZoneRow> Row = MakeShared<FZoneRow>();
    Row->ID = i;
    Row->Name = StbData.GetCell(i, NameCol); // e.g. "JDT01"
    Row->ZonPath = ZFile;
    Row->DecoPath = StbData.GetCell(i, 12);
    Row->CnstPath = StbData.GetCell(i, 13);

    // Refine Name from Column 2 (usually full name) if available
    FString FullInit = StbData.GetCell(i, 2);
    if (!FullInit.IsEmpty())
      Row->Name = FullInit;

    Browser->ZoneRows.Add(Row);
  }

  Browser->RefreshList();
  Window->SetContent(Browser);

  GEditor->EditorAddModalWindow(Window);

  return Browser->SelectedRow;
}

void SRoseZoneBrowser::RefreshList() {
  FilteredRows.Empty();
  if (FilterString.IsEmpty()) {
    FilteredRows = ZoneRows;
  } else {
    for (auto &Row : ZoneRows) {
      if (Row->Name.Contains(FilterString) ||
          Row->ZonPath.Contains(FilterString)) {
        FilteredRows.Add(Row);
      }
    }
  }
  if (ListView)
    ListView->RequestListRefresh();
}

void SRoseZoneBrowser::OnFilterTextChanged(const FText &InFilterText) {
  FilterString = InFilterText.ToString();
  RefreshList();
}

TSharedRef<ITableRow>
SRoseZoneBrowser::OnGenerateRow(TSharedPtr<FZoneRow> Item,
                                const TSharedRef<STableViewBase> &OwnerTable) {
  auto Row = SNew(STableRow<TSharedPtr<FZoneRow>>, OwnerTable);

  Row->SetContent(
      SNew(SHorizontalBox) +
      SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(
          50.0f)[SNew(STextBlock).Text(FText::AsNumber(Item->ID))]] +
      SHorizontalBox::Slot().FillWidth(
          0.4f)[SNew(STextBlock).Text(FText::FromString(Item->Name))] +
      SHorizontalBox::Slot().FillWidth(
          0.4f)[SNew(STextBlock).Text(FText::FromString(Item->ZonPath))]);

  return Row;
}

void SRoseZoneBrowser::OnSelectionChanged(TSharedPtr<FZoneRow> Item,
                                          ESelectInfo::Type SelectInfo) {
  SelectedRow = Item;
}

FReply SRoseZoneBrowser::OnImportClicked() {
  if (ParentWindow.IsValid()) {
    ParentWindow.Pin()->RequestDestroyWindow();
  }
  return FReply::Handled();
}

FReply SRoseZoneBrowser::OnCancelClicked() {
  SelectedRow.Reset();
  if (ParentWindow.IsValid()) {
    ParentWindow.Pin()->RequestDestroyWindow();
  }
  return FReply::Handled();
}
