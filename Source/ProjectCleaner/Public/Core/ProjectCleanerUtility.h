﻿// Copyright 2021. Ashot Barkhudaryan. All Rights Reserved.

#pragma once

#include "StructsContainer.h"
// Engine Headers
#include "CoreMinimal.h"

class UCleanerConfigs;
class UIndirectAsset;
class UExcludeOptions;
class AssetRelationalMap;
class FAssetRegistryModule;
class ProjectCleanerNotificationManager;
struct FAssetData;
struct FProjectCleanerData;

/**
 * This class responsible for different utility operations in unreal engine context
 */
class PROJECTCLEANER_API ProjectCleanerUtility
{
public:
	static int64 GetTotalSize(const TArray<FAssetData>& Assets);
	static FString ConvertAbsolutePathToInternal(const FString& InPath);
	static FString ConvertInternalToAbsolutePath(const FString& InPath);
	static void FixupRedirectors();
	static void SaveAllAssets(const bool PromptUser);
	static void UpdateAssetRegistry(bool bSyncScan);
	static bool DeleteEmptyFolders(TSet<FName>& EmptyFolders);
	static int32 DeleteAssets(TArray<FAssetData>& Assets, const bool ForceDelete);
	static bool IsEngineExtension(const FString& Extension);
private:
	static FString ConvertPathInternal(const FString& From, const FString To, const FString& Path);
};