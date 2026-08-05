// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#include "NGSVMPerformanceBenchmark.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformTime.h"

UNGSVMPerformanceBenchmark* UNGSVMPerformanceBenchmark::CreateNGSVMBenchmark(UObject* WorldContextObject)
{
	return NewObject<UNGSVMPerformanceBenchmark>(WorldContextObject ? WorldContextObject : (UObject*)GetTransientPackage());
}

void UNGSVMPerformanceBenchmark::StartSession(const FString& CsvFilePath)
{
	CsvPath = CsvFilePath;
	RunTotalDurations.Reset();
	RunAvgTickTimes.Reset();
	CurrentRunTickDeltas.Reset();

	static const FString Header = TEXT("VideoName,RepeatCount,AvgTotalDurationSeconds,AvgPerTickMilliseconds\n");
	if (!FFileHelper::SaveStringToFile(Header, *CsvPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_None))
	{
		UE_LOG(LogTemp, Error, TEXT("[NGSVM Benchmark] Failed to create CSV file at: %s"), *CsvPath);
	}
}

void UNGSVMPerformanceBenchmark::BeginRun()
{
	RunStartTime = FPlatformTime::Seconds();
	LastTickTime = RunStartTime;
	CurrentRunTickDeltas.Reset();
}

void UNGSVMPerformanceBenchmark::RecordTick()
{
	const double Now = FPlatformTime::Seconds();
	CurrentRunTickDeltas.Add(Now - LastTickTime);
	LastTickTime = Now;
}

void UNGSVMPerformanceBenchmark::EndRun()
{
	const double TotalDuration = FPlatformTime::Seconds() - RunStartTime;

	double SumTicks = 0.0;
	for (double Delta : CurrentRunTickDeltas)
	{
		SumTicks += Delta;
	}
	const double AvgTickTime = CurrentRunTickDeltas.Num() > 0 ? SumTicks / CurrentRunTickDeltas.Num() : 0.0;

	RunTotalDurations.Add(TotalDuration);
	RunAvgTickTimes.Add(AvgTickTime);

	UE_LOG(LogTemp, Display, TEXT("[NGSVM Benchmark] Run #%d finished: TotalDuration=%.3fs AvgTickTime=%.2fms (%d ticks)"),
		RunTotalDurations.Num(), TotalDuration, AvgTickTime * 1000.0, CurrentRunTickDeltas.Num());
}

void UNGSVMPerformanceBenchmark::FinishVideo(const FString& VideoName)
{
	if (RunTotalDurations.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[NGSVM Benchmark] FinishVideo(%s) called with no recorded runs -- skipping."), *VideoName);
		return;
	}

	double SumTotal = 0.0;
	for (double D : RunTotalDurations)
	{
		SumTotal += D;
	}
	double SumAvgTick = 0.0;
	for (double D : RunAvgTickTimes)
	{
		SumAvgTick += D;
	}

	const int32 RepeatCount = RunTotalDurations.Num();
	const double AvgTotalDuration = SumTotal / RepeatCount;
	const double AvgPerTick = SumAvgTick / RunAvgTickTimes.Num();

	AppendCsvRow(VideoName, RepeatCount, AvgTotalDuration, AvgPerTick);

	UE_LOG(LogTemp, Display, TEXT("[NGSVM Benchmark] %s: AvgTotalDuration=%.3fs AvgPerTick=%.2fms over %d run(s)"),
		*VideoName, AvgTotalDuration, AvgPerTick * 1000.0, RepeatCount);

	RunTotalDurations.Reset();
	RunAvgTickTimes.Reset();
}

void UNGSVMPerformanceBenchmark::AppendCsvRow(const FString& VideoName, int32 RepeatCount, double AvgTotalDuration, double AvgPerTick) const
{
	const FString Row = FString::Printf(TEXT("%s,%d,%.4f,%.4f\n"), *VideoName, RepeatCount, AvgTotalDuration, AvgPerTick * 1000.0);
	if (!FFileHelper::SaveStringToFile(Row, *CsvPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append))
	{
		UE_LOG(LogTemp, Error, TEXT("[NGSVM Benchmark] Failed to append CSV row to: %s"), *CsvPath);
	}
}
