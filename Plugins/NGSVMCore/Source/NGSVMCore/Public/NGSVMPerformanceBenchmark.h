// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "NGSVMPerformanceBenchmark.generated.h"

/**
 * Lightweight performance-test harness for measuring NGSVM's real-world Tick cost while playing
 * back test videos. Deliberately does not touch UNGSVMManager or the video-playback pipeline --
 * orchestration (which video plays, when it starts/ends, looping N repeats) is left to a driving
 * Blueprint; this object only does precise timing and CSV output.
 *
 * Typical Blueprint usage per test video:
 *   1. StartSession(CsvPath) -- once, at the very start of the whole benchmark.
 *   2. For each of N repeats: BeginRun() -> play the video -> call RecordTick() every Tick while
 *      it plays -> EndRun() once the video's OnEndReached fires.
 *   3. After all N repeats: FinishVideo(VideoName) -- averages the N runs and appends one CSV row.
 *   4. Repeat steps 2-3 for the next test video.
 */
UCLASS(BlueprintType, Blueprintable)
class NGSVMCORE_API UNGSVMPerformanceBenchmark : public UObject
{
	GENERATED_BODY()

public:
	/** Convenience factory so Blueprint doesn't need a "Construct Object from Class" node. */
	UFUNCTION(BlueprintCallable, Category = "NGSVM|Benchmark", meta = (WorldContext = "WorldContextObject"))
	static UNGSVMPerformanceBenchmark* CreateNGSVMBenchmark(UObject* WorldContextObject);

	/** Creates/overwrites the CSV file at CsvFilePath and writes the header row. Call once before
	 *  testing any video. CsvFilePath should be an absolute path (e.g. built from
	 *  FPaths::ProjectSavedDir() in Blueprint). */
	UFUNCTION(BlueprintCallable, Category = "NGSVM|Benchmark")
	void StartSession(const FString& CsvFilePath);

	/** Starts timing a single playthrough. Call right before you start playing the video. */
	UFUNCTION(BlueprintCallable, Category = "NGSVM|Benchmark")
	void BeginRun();

	/** Call once per Tick while a run is in progress (e.g. from the driving Blueprint's Event
	 *  Tick). Records the real wall-clock time elapsed since the previous RecordTick/BeginRun
	 *  call as one loop sample. */
	UFUNCTION(BlueprintCallable, Category = "NGSVM|Benchmark")
	void RecordTick();

	/** Call once the video's playback has ended (bind to the Media Player's OnEndReached event).
	 *  Closes out the current run: stores its total duration and average per-tick time. */
	UFUNCTION(BlueprintCallable, Category = "NGSVM|Benchmark")
	void EndRun();

	/** Averages every run recorded since the last StartSession/FinishVideo call, appends one CSV
	 *  row (VideoName, RepeatCount, AvgTotalDurationSeconds, AvgPerTickMilliseconds), and resets
	 *  internal state so the next video's runs start clean. No-ops if no runs were recorded. */
	UFUNCTION(BlueprintCallable, Category = "NGSVM|Benchmark")
	void FinishVideo(const FString& VideoName);

private:
	FString CsvPath;

	double RunStartTime = 0.0;
	double LastTickTime = 0.0;
	TArray<double> CurrentRunTickDeltas;

	// One entry per completed run (BeginRun -> EndRun) for the video currently being tested.
	TArray<double> RunTotalDurations;
	TArray<double> RunAvgTickTimes;

	void AppendCsvRow(const FString& VideoName, int32 RepeatCount, double AvgTotalDuration, double AvgPerTick) const;
};
