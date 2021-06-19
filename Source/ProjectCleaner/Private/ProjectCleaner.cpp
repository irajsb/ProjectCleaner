// Copyright 2021. Ashot Barkhudaryan. All Rights Reserved.

#include "ProjectCleaner.h"
#include "ProjectCleanerStyle.h"
#include "ProjectCleanerCommands.h"
#include "ProjectCleanerNotificationManager.h"
#include "ProjectCleanerUtility.h"
#include "ProjectCleanerHelper.h"
#include "UI/ProjectCleanerBrowserStatisticsUI.h"
#include "UI/ProjectCleanerExcludeOptionsUI.h"
#include "UI/ProjectCleanerUnusedAssetsBrowserUI.h"
#include "UI/ProjectCleanerNonUassetFilesUI.h"
#include "UI/ProjectCleanerSourceCodeAssetsUI.h"
#include "UI/ProjectCleanerCorruptedFilesUI.h"
#include "UI/ProjectCleanerExcludedAssetsUI.h"
#include "UI/ProjectCleanerConfigsUI.h"
// Engine Headers
#include "AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetRegistry.h"
#include "ToolMenus.h"
#include "ContentBrowserModule.h"
#include "Misc/MessageDialog.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "EditorStyleSet.h"
#include "IContentBrowserSingleton.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Engine/AssetManager.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Framework/Commands/UICommandList.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/PackageFileSummary.h"
#include "Settings/ContentBrowserSettings.h"
#include "Serialization/CustomVersion.h"

DEFINE_LOG_CATEGORY(LogProjectCleaner);

static const FName ProjectCleanerTabName("ProjectCleaner");
static const FName UnusedAssetsTab = FName{ TEXT("UnusedAssetsTab") };
static const FName NonUassetFilesTab = FName{ TEXT("NonUassetFilesTab") };
static const FName SourceCodeAssetTab = FName{ TEXT("SourceCodeAssetTab") };
static const FName CorruptedFilesTab = FName{ TEXT("CorruptedFilesTab") };

#define LOCTEXT_NAMESPACE "FProjectCleanerModule"

FProjectCleanerModule::FProjectCleanerModule() :
	CleanerConfigs(nullptr),
	ExcludeOptions(nullptr),
	AssetRegistry(nullptr),
	AssetManager(nullptr),
	ContentBrowser(nullptr)
{
}

void FProjectCleanerModule::StartupModule()
{
	// initializing styles
	FProjectCleanerStyle::Initialize();
	FProjectCleanerStyle::ReloadTextures();
	FProjectCleanerCommands::Register();

	// Registering plugin commands
	PluginCommands = MakeShareable(new FUICommandList);
	PluginCommands->MapAction(
		FProjectCleanerCommands::Get().OpenCleanerWindow,
		FExecuteAction::CreateRaw(this, &FProjectCleanerModule::PluginButtonClicked),
		FCanExecuteAction()
	);

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(
			this,
			&FProjectCleanerModule::RegisterMenus
		)
	);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ProjectCleanerTabName,
		FOnSpawnTab::CreateRaw(this, &FProjectCleanerModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("FProjectCleanerTabTitle", "ProjectCleaner"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	// this is for TabManager initialization only
	const auto DummyTab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	TabManager = FGlobalTabmanager::Get()->NewTabManager(DummyTab);
	TabManager->SetCanDoDragOperation(false);
	TabLayout = FTabManager::NewLayout("ProjectCleanerTabLayout")
	->AddArea
	(
		FTabManager::NewPrimaryArea()
		->SetOrientation(Orient_Vertical)
		->Split
		(
			FTabManager::NewStack()
			->SetSizeCoefficient(0.4f)
			->SetHideTabWell(true)
			->AddTab(UnusedAssetsTab, ETabState::OpenedTab)
			->AddTab(SourceCodeAssetTab, ETabState::OpenedTab)
			->AddTab(NonUassetFilesTab, ETabState::OpenedTab)
			->AddTab(CorruptedFilesTab, ETabState::OpenedTab)
			->SetForegroundTab(UnusedAssetsTab)
		)
	);

	TabManager->RegisterTabSpawner(UnusedAssetsTab, FOnSpawnTab::CreateRaw(
		this,
		&FProjectCleanerModule::OnUnusedAssetTabSpawn)
	);
	TabManager->RegisterTabSpawner(SourceCodeAssetTab, FOnSpawnTab::CreateRaw(
		this,
		&FProjectCleanerModule::OnSourceCodeAssetsTabSpawn)
	);
	TabManager->RegisterTabSpawner(NonUassetFilesTab, FOnSpawnTab::CreateRaw(
		this,
		&FProjectCleanerModule::OnNonUAssetFilesTabSpawn)
	);
	TabManager->RegisterTabSpawner(CorruptedFilesTab, FOnSpawnTab::CreateRaw(
		this,
		&FProjectCleanerModule::OnCorruptedFilesTabSpawn)
	);
	
	// initializing some objects
	NotificationManager = MakeShared<ProjectCleanerNotificationManager>();
	ExcludeOptions = GetMutableDefault<UExcludeOptions>();
	CleanerConfigs = GetMutableDefault<UCleanerConfigs>();
	AssetRegistry = &FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	if (AssetRegistry)
	{
		AssetRegistry->Get().OnFilesLoaded().AddRaw(this, &FProjectCleanerModule::OnFilesLoaded);
	}

	ContentBrowser = &FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

}

void FProjectCleanerModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FProjectCleanerStyle::Shutdown();
	FProjectCleanerCommands::Unregister();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ProjectCleanerTabName);
	TabManager->UnregisterTabSpawner(UnusedAssetsTab);
	TabManager->UnregisterTabSpawner(SourceCodeAssetTab);
	TabManager->UnregisterTabSpawner(NonUassetFilesTab);
	TabManager->UnregisterTabSpawner(CorruptedFilesTab);
	AssetRegistry = nullptr;
}

bool FProjectCleanerModule::IsGameModule() const
{
	return false;
}

void FProjectCleanerModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
	FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
	Section.AddMenuEntryWithCommandList(FProjectCleanerCommands::Get().OpenCleanerWindow, PluginCommands);

	UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar");
	FToolMenuSection& ToolbarSection = ToolbarMenu->FindOrAddSection("Settings");
	FToolMenuEntry& Entry = ToolbarSection.AddEntry(
		FToolMenuEntry::InitToolBarButton(FProjectCleanerCommands::Get().OpenCleanerWindow)
	);
	Entry.SetCommandList(PluginCommands);
}

void FProjectCleanerModule::PluginButtonClicked()
{
	if (!bCanOpenTab)
	{
		if (!NotificationManager) return;
		NotificationManager->AddTransient(
			TEXT("Asset Registry still working! Please wait..."),
			SNotificationItem::CS_Fail,
			3.0f
		);
		return;
	}

	FGlobalTabmanager::Get()->TryInvokeTab(ProjectCleanerTabName);
}

TSharedRef<SDockTab> FProjectCleanerModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	const auto NomadTab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	
	ensure(TabManager.IsValid());
	
	UpdateCleaner();

	const auto ExcludedAssetsUIRef = SAssignNew(ExcludedAssetsUI, SProjectCleanerExcludedAssetsUI)
		.ExcludedAssets(ExcludedAssets)
		.CleanerConfigs(CleanerConfigs)
		.LinkedAssets(LinkedAssets);

	ExcludedAssetsUIRef->OnUserIncludedAssets = FOnUserIncludedAsset::CreateRaw(
		this,
		&FProjectCleanerModule::OnUserIncludedAssets
	);
	
	const TSharedRef<SWidget> TabContents = TabManager->RestoreFrom(
		TabLayout.ToSharedRef(),
		TSharedPtr<SWindow>()
	).ToSharedRef();

	NomadTab->SetContent(
		SNew(SBorder)
		[
			SNew(SSplitter)
			+ SSplitter::Slot()
			.Value(0.35f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				.Padding(FMargin{ 20.0f })
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SBorder)
						.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.Padding(FMargin{ 20.0f, 20.0f })
							.AutoHeight()
							[
								SAssignNew(StatisticsUI, SProjectCleanerBrowserStatisticsUI)
								.Stats(CleaningStats)
							]
							+ SVerticalBox::Slot()
							.Padding(FMargin{ 20.0f, 20.0f })
							.AutoHeight()
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								[
									SNew(SButton)
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.Text(FText::FromString("Refresh"))
									.OnClicked_Raw(this, &FProjectCleanerModule::OnRefreshBtnClick)
								]
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.Padding(FMargin{ 40.0f, 0.0f, 40.0f, 0.0f })
								[
									SNew(SButton)
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.Text(FText::FromString("Delete Unused Assets"))
									.OnClicked_Raw(this, &FProjectCleanerModule::OnDeleteUnusedAssetsBtnClick)
								]
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								[
									SNew(SButton)
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.Text(FText::FromString("Delete Empty Folders"))
									.OnClicked_Raw(this, &FProjectCleanerModule::OnDeleteEmptyFolderClick)
								]
							]
							+ SVerticalBox::Slot()
							.Padding(FMargin{20.0f, 5.0f})
							.AutoHeight()
							[
								SAssignNew(CleanerConfigsUI, SProjectCleanerConfigsUI)
								.CleanerConfigs(CleanerConfigs)
							]
							+ SVerticalBox::Slot()
							.Padding(FMargin{20.0f, 5.0f})
							.AutoHeight()
							[
								SAssignNew(ExcludeOptionUI, SProjectCleanerExcludeOptionsUI)
								.ExcludeOptions(ExcludeOptions)
							]
						]
					]
					+ SScrollBox::Slot()
					.Padding(FMargin{0.0f, 20.0f})
					[
						SNew(SBorder)
						.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.Padding(FMargin{20.0f, 10.0f})
							.AutoHeight()
							[
								ExcludedAssetsUIRef
							]
						]
					]
				]
			]
			+ SSplitter::Slot()
			.Value(0.65f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				.Padding(FMargin{20.0f})
				[
					TabContents
				]
			]
		]
	);
	
	return NomadTab;
}

TSharedRef<SDockTab> FProjectCleanerModule::OnUnusedAssetTabSpawn(const FSpawnTabArgs& SpawnTabArgs)
{
	const auto UnusedAssetsUIRef =
		SAssignNew(UnusedAssetsBrowserUI, SProjectCleanerUnusedAssetsBrowserUI)
		.PrimaryAssetClasses(&PrimaryAssetClasses)
		.UnusedAssets(&UnusedAssets)
		.CleanerConfigs(CleanerConfigs);
	
	UnusedAssetsUIRef->OnUserDeletedAssets = FOnUserDeletedAssets::CreateRaw(
		this,
		&FProjectCleanerModule::OnUserDeletedAssets
	);

	UnusedAssetsUIRef->OnUserExcludedAssets = FOnUserExcludedAssets::CreateRaw(
		this,
		&FProjectCleanerModule::OnUserExcludedAssets
	);

	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.OnCanCloseTab_Lambda([] {return false; })
		.Label(NSLOCTEXT("UnusedAssetsTab", "TabTitle", "Unused Assets"))
		[
			UnusedAssetsUIRef
		];
}

TSharedRef<SDockTab> FProjectCleanerModule::OnNonUAssetFilesTabSpawn(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.OnCanCloseTab_Lambda([] {return false; })
		.Label(NSLOCTEXT("NonUAssetFilesTab", "TabTitle", "Non .uasset Files"))
		[
			SAssignNew(NonUassetFilesUI, SProjectCleanerNonUassetFilesUI)
			.NonUassetFiles(NonUAssetFiles)
		];
}

TSharedRef<SDockTab> FProjectCleanerModule::OnCorruptedFilesTabSpawn(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.OnCanCloseTab_Lambda([] {return false; })
		.Label(NSLOCTEXT("CorruptedFilesTab", "TabTitle", "Corrupted Files"))
		[
			SAssignNew(CorruptedFilesUI, SProjectCleanerCorruptedFilesUI)
			.CorruptedFiles(CorruptedFiles)
		];
}

TSharedRef<SDockTab> FProjectCleanerModule::OnSourceCodeAssetsTabSpawn(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.OnCanCloseTab_Lambda([] {return false; })
		.Label(NSLOCTEXT("SourceCodeAssets", "TabTitle", "Assets Used Indirectly"))
		[
			SAssignNew(SourceCodeAssetsUI, SProjectCleanerSourceCodeAssetsUI)
			.SourceCodeAssets(&SourceCodeAssets)
		];
}

void FProjectCleanerModule::OnScanDeveloperContentCheckboxChanged(ECheckBoxState State)
{
	CleanerConfigs->bScanDeveloperContents = (State == ECheckBoxState::Checked);

	UpdateCleanerData();
}

void FProjectCleanerModule::OnAutomaticallyRemoveEmptyFoldersCheckboxChanged(ECheckBoxState State)
{
	CleanerConfigs->bAutomaticallyDeleteEmptyFolders = (State == ECheckBoxState::Checked);
}

void FProjectCleanerModule::UpdateCleaner()
{
	ProjectCleanerUtility::SaveAllAssets();

	ProjectCleanerUtility::FixupRedirectors();

	UpdateCleanerData();
}

void FProjectCleanerModule::UpdateCleanerData()
{
	FScopedSlowTask SlowTask{ 100.0f, FText::FromString("Scanning...") };
	SlowTask.MakeDialog();

	Reset();
	
	if (!AssetRegistry) return;
	if (!CleanerConfigs) return;
	
	ProjectCleanerHelper::GetEmptyFolders(EmptyFolders, CleanerConfigs->bScanDeveloperContents);
	ProjectCleanerHelper::GetProjectFilesFromDisk(ProjectFilesFromDisk);

	ProjectCleanerUtility::GetInvalidProjectFiles(AssetRegistry, ProjectFilesFromDisk, CorruptedFiles, NonUAssetFiles);

	SlowTask.EnterProgressFrame(10.0f, FText::FromString("Finding invalid files..."));
	
	AssetManager = &UAssetManager::Get();
	ProjectCleanerUtility::GetAllPrimaryAssetClasses(*AssetManager, PrimaryAssetClasses);

	ProjectCleanerUtility::GetAllAssets(AssetRegistry, UnusedAssets);

	ProjectCleanerUtility::RemoveUsedAssets(UnusedAssets, PrimaryAssetClasses);
	ProjectCleanerUtility::RemoveMegascansPluginAssetsIfActive(UnusedAssets);


	// update content browser settings to show "Developers Contents"
	GetMutableDefault<UContentBrowserSettings>()->SetDisplayDevelopersFolder(CleanerConfigs->bScanDeveloperContents, true);
	GetMutableDefault<UContentBrowserSettings>()->PostEditChange();

	// filling graphs with unused assets data and creating relational map between them
	RelationalMap.Rebuild(UnusedAssets, CleanerConfigs);

	ProjectCleanerUtility::RemoveAssetsUsedIndirectly(UnusedAssets, RelationalMap, SourceCodeAssets);

	RelationalMap.Rebuild(UnusedAssets, CleanerConfigs);

	ProjectCleanerUtility::RemoveContentFromDeveloperFolder(UnusedAssets, RelationalMap, CleanerConfigs, NotificationManager);

	RelationalMap.Rebuild(UnusedAssets, CleanerConfigs);

	ProjectCleanerUtility::RemoveAssetsWithExternalReferences(UnusedAssets, RelationalMap);

	// 8) User excluded assets
	// This assets will be in Database, but wont be available for deletion
	// * Excluded by path
	//		all assets in given path and their linked assets will remain untouched
	// * Excluded single asset from  "UnusedAssets" content browser, asset and all their linked assets will remain untouched
	// * Excluded by asset class
	//		all assets with specified class and all their linked assets will remain untouched
	// Explicitly excluded assets will be shown in "Excluded Assets" Content browser <- include asset action, removing selected assets from exclusion list
	// Linked assets to those excluded assets will be shown in "Linked Assets" Content browser <- no action here, user just see , what we wont delete
	//ProjectCleanerUtility::RemoveAssetsExcludedByUser(
	//	AssetRegistry,
	//	UnusedAssets,
	//	ExcludedAssets,
	//	LinkedAssets,
	//	UserExcludedAssets,
	//	RelationalMap,
	//	ExcludeOptions
	//);

	// after all actions we rebuilding relational map to match unused assets
	RelationalMap.Rebuild(UnusedAssets, CleanerConfigs);

	SlowTask.EnterProgressFrame(90.0f, FText::FromString("Building assets relational map..."));

	UpdateStats();
}

void FProjectCleanerModule::UpdateStats()
{
	CleaningStats.Reset();

	CleaningStats.UnusedAssetsNum = UnusedAssets.Num();
	CleaningStats.EmptyFolders = EmptyFolders.Num();
	CleaningStats.NonUassetFilesNum = NonUAssetFiles.Num();
	CleaningStats.SourceCodeAssetsNum = SourceCodeAssets.Num();
	CleaningStats.UnusedAssetsTotalSize = ProjectCleanerUtility::GetTotalSize(UnusedAssets);
	CleaningStats.CorruptedFilesNum = CorruptedFiles.Num();
	CleaningStats.TotalAssetNum = CleaningStats.UnusedAssetsNum;

	if (StatisticsUI.IsValid())
	{
		StatisticsUI.Pin()->SetStats(CleaningStats);
	}

	if (UnusedAssetsBrowserUI.IsValid())
	{
		UnusedAssetsBrowserUI.Pin()->SetUIData(UnusedAssets, CleanerConfigs, PrimaryAssetClasses);
	}

	if (NonUassetFilesUI.IsValid())
	{
		NonUassetFilesUI.Pin()->SetNonUassetFiles(NonUAssetFiles);
	}

	if (CorruptedFilesUI.IsValid())
	{
		CorruptedFilesUI.Pin()->SetCorruptedFiles(CorruptedFiles);
	}

	if (SourceCodeAssetsUI.IsValid())
	{
		SourceCodeAssetsUI.Pin()->SetSourceCodeAssets(SourceCodeAssets);
	}

	if (ExcludedAssetsUI.IsValid())
	{
		ExcludedAssetsUI.Pin()->SetExcludedAssets(ExcludedAssets);
		ExcludedAssetsUI.Pin()->SetLinkedAssets(LinkedAssets);
		ExcludedAssetsUI.Pin()->SetCleanerConfigs(CleanerConfigs);
	}
}

void FProjectCleanerModule::Reset()
{
	UnusedAssets.Reset();
	NonUAssetFiles.Reset();
	SourceCodeAssets.Reset();
	CorruptedFiles.Reset();
	EmptyFolders.Reset();
	LinkedAssets.Reset();
	ExcludedAssets.Reset();
	RelationalMap.Reset();
	PrimaryAssetClasses.Reset();
	ProjectFilesFromDisk.Reset();
}

void FProjectCleanerModule::UpdateContentBrowser() const
{
	if (!AssetRegistry) return;
	if (!ContentBrowser) return;

	TArray<FString> FocusFolders;
	FocusFolders.Add("/Game");

	AssetRegistry->Get().ScanPathsSynchronous(FocusFolders, true);
	AssetRegistry->Get().SearchAllAssets(true);
	ContentBrowser->Get().SetSelectedPaths(FocusFolders, true);
}

void FProjectCleanerModule::CleanEmptyFolders()
{
	ProjectCleanerHelper::DeleteEmptyFolders(AssetRegistry, EmptyFolders);
	const FString PostFixText = CleaningStats.EmptyFolders > 1 ? TEXT(" empty folders") : TEXT(" empty folder");
	const FString DisplayText = FString{ "Deleted " } + FString::FromInt(CleaningStats.EmptyFolders) + PostFixText;
	NotificationManager->AddTransient(
		DisplayText,
		SNotificationItem::ECompletionState::CS_Success,
		10.0f
	);

	UpdateCleanerData();
	UpdateContentBrowser();
}

EAppReturnType::Type FProjectCleanerModule::ShowConfirmationWindow(const FText& Title, const FText& ContentText) const
{
	return FMessageDialog::Open(
		EAppMsgType::YesNo,
		ContentText,
		&Title
	);
}

bool FProjectCleanerModule::IsConfirmationWindowCanceled(EAppReturnType::Type Status)
{
	return Status == EAppReturnType::Type::No || Status == EAppReturnType::Cancel;
}

void FProjectCleanerModule::OnUserDeletedAssets()
{
	UpdateCleaner();
}

void FProjectCleanerModule::OnUserExcludedAssets(const TArray<FAssetData>& Assets)
{
	if (!Assets.Num()) return;

	for (const auto& Asset : Assets)
	{
		UserExcludedAssets.AddUnique(Asset);
	}
	
	UpdateCleanerData();
}

void FProjectCleanerModule::OnUserIncludedAssets(const TArray<FAssetData>& Assets)
{
	if (!Assets.Num()) return;

	UserExcludedAssets.RemoveAll([&](const FAssetData& Elem)
	{
		return Assets.Contains(Elem);
	});

	UpdateCleanerData();
}

FReply FProjectCleanerModule::OnRefreshBtnClick()
{
	UpdateCleaner();

	return FReply::Handled();
}

FReply FProjectCleanerModule::OnDeleteUnusedAssetsBtnClick()
{
	if (!NotificationManager.IsValid())
	{
		UE_LOG(LogProjectCleaner, Error, TEXT("Notification Manager is not valid"));
		return FReply::Handled();
	}

	if (UnusedAssets.Num() == 0)
	{
		NotificationManager->AddTransient(
			StandardCleanerText.NoAssetsToDelete.ToString(),
			SNotificationItem::ECompletionState::CS_Fail,
			3.0f
		);

		return FReply::Handled();
	}

	const auto ConfirmationWindowStatus = ShowConfirmationWindow(
		StandardCleanerText.AssetsDeleteWindowTitle,
		StandardCleanerText.AssetsDeleteWindowContent
	);
	if (IsConfirmationWindowCanceled(ConfirmationWindowStatus))
	{
		return FReply::Handled();
	}

	// Root assets has no referencers
	TArray<FAssetData> RootAssets;
	RootAssets.Reserve(UnusedAssets.Num());

	const auto CleaningNotificationPtr = NotificationManager->Add(
		StandardCleanerText.StartingCleanup.ToString(),
		SNotificationItem::ECompletionState::CS_Pending
	);

	while (UnusedAssets.Num() > 0)
	{
		const auto CircularNodes = RelationalMap.GetCircularNodes();
		const auto RootNodes = RelationalMap.GetRootNodes();

		if (CircularNodes.Num() > 0)
		{
			for (const auto& CircularNode : CircularNodes)
			{
				RootAssets.AddUnique(CircularNode.AssetData);
			}
		}
		else if (RootNodes.Num() > 0)
		{
			for (const auto& RootNode : RootNodes)
			{
				RootAssets.AddUnique(RootNode.AssetData);
			}
		}
		else
		{
			for (const auto& Node : RelationalMap.GetNodes())
			{
				if (RootAssets.Num() >= CleanerConfigs->DeleteChunkLimit) break;
				RootAssets.AddUnique(Node.AssetData);
			}
		}

		// Remaining assets are valid so we trying to delete them
		CleaningStats.DeletedAssetCount += ProjectCleanerUtility::DeleteAssets(RootAssets);
		NotificationManager->Update(CleaningNotificationPtr, CleaningStats);

		UnusedAssets.RemoveAll([&](const FAssetData& Elem)
		{
			return RootAssets.Contains(Elem);
		});

		// after chunk of assets deleted, we must update adjacency list
		RelationalMap.Rebuild(UnusedAssets, CleanerConfigs);
		RootAssets.Reset();
	}

	NotificationManager->Hide(
		CleaningNotificationPtr,
		FText::FromString(FString::Printf(TEXT("Deleted %d assets."), CleaningStats.DeletedAssetCount))
	);

	UpdateCleanerData();

	if (CleanerConfigs->bAutomaticallyDeleteEmptyFolders)
	{
		CleanEmptyFolders();
	}

	return FReply::Handled();
}

FReply FProjectCleanerModule::OnDeleteEmptyFolderClick()
{
	if (!NotificationManager.IsValid())
	{
		UE_LOG(LogProjectCleaner, Error, TEXT("Notification Manager is not valid"));
		return FReply::Handled();
	}

	if (EmptyFolders.Num() == 0)
	{
		NotificationManager->AddTransient(
			StandardCleanerText.NoEmptyFolderToDelete.ToString(),
			SNotificationItem::ECompletionState::CS_Fail,
			3.0f
		);

		return FReply::Handled();
	}

	const auto ConfirmationWindowStatus = ShowConfirmationWindow(
		StandardCleanerText.EmptyFolderWindowTitle,
		StandardCleanerText.EmptyFolderWindowContent
	);
	if (IsConfirmationWindowCanceled(ConfirmationWindowStatus))
	{
		return FReply::Handled();
	}

	CleanEmptyFolders();

	return FReply::Handled();
}

void FProjectCleanerModule::OnFilesLoaded()
{
	bCanOpenTab = true;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FProjectCleanerModule, ProjectCleaner)