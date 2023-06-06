// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Sensor/PixelReader.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Async/Async.h"
#include "HighResScreenshot.h"
#include "Runtime/ImageWriteQueue/Public/ImageWriteQueue.h"
#include "Carla/Sensor/ImageUtil.h"

// =============================================================================
// -- FPixelReader -------------------------------------------------------------
// =============================================================================

void FPixelReader::WritePixelsToBuffer(
    const UTextureRenderTarget2D &RenderTarget,
    uint32 Offset,
    FRHICommandListImmediate &RHICmdList,
    FPixelReader::Payload FuncForSending)
{
  TRACE_CPUPROFILER_EVENT_SCOPE_STR("WritePixelsToBuffer");
  check(IsInRenderingThread());
  
  auto Resource = RenderTarget.GetResource();
  if (!Resource)
      return;

  auto Texture = Resource->GetTexture2DRHI();
  if (!Texture)
    return;

  auto BackBufferReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("CameraBufferReadback"));
  FIntPoint BackBufferSize = Texture->GetSizeXY();
  EPixelFormat BackBufferPixelFormat = Texture->GetFormat();
  {
    TRACE_CPUPROFILER_EVENT_SCOPE_STR("EnqueueCopy");
    BackBufferReadback->EnqueueCopy(RHICmdList, Texture);
  }

  // workaround to force RHI with Vulkan to refresh the fences state in the middle of frame:
  {
      static thread_local auto QueryPool = RHICreateRenderQueryPool(RQT_AbsoluteTime);

      auto Query = QueryPool->AllocateQuery();
      auto QueryPtr = Query.GetQuery();

      {
          TRACE_CPUPROFILER_EVENT_SCOPE_STR("create query");
          RHICmdList.EndRenderQuery(QueryPtr);
      }
      {
          TRACE_CPUPROFILER_EVENT_SCOPE_STR("Flush");
          RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
      }
      {
          TRACE_CPUPROFILER_EVENT_SCOPE_STR("query result");
          uint64 Unused = 0;
          RHIGetRenderQueryResult(QueryPtr, Unused, true);
      }
  }

  AsyncTask(ENamedThreads::ActualRenderingThread, [=, Readback = MoveTemp(BackBufferReadback)]() mutable {

    {
      TRACE_CPUPROFILER_EVENT_SCOPE_STR("Wait GPU transfer");

      while (!Readback->IsReady())
        std::this_thread::yield();
    }
    
    FPixelFormatInfo PixelFormat = GPixelFormats[BackBufferPixelFormat];
    uint32 ExpectedRowBytes = BackBufferSize.X * PixelFormat.BlockBytes;
    int32 RowPitch = 0;
    int32 BufferHeight = 0;
    auto LockedData = Readback->Lock(RowPitch, &BufferHeight);
    auto Size = (uint32)RowPitch * (uint32)BufferHeight;

#if 0 // @CARLA_UE5
    if (LockedData)
    {
        try
        {
            FuncForSending(LockedData, Size, Offset, ExpectedRowBytes);
        }
        catch (std::exception& e)
        {
            UE_LOG(LogCarla, Warning, TEXT("Exception thrown from PixelReader send callback: %s"), e.what());
        }
    }
#else
    static std::atomic_size_t counter = 0;
    auto PixelData = MakeUnique<TImagePixelData<FColor>>(BackBufferSize);
    PixelData->Pixels.SetNum(BackBufferSize.X * BackBufferSize.Y);
    ImageUtil::DecodePixelsByFormat(
        LockedData,
        RowPitch,
        BackBufferSize,
        BackBufferSize,
        BackBufferPixelFormat,
        {},
        PixelData->Pixels);
    TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
    ImageTask->PixelData = MoveTemp(PixelData);
    ImageTask->Filename = FString::Printf(TEXT("F:/_out_images/%llu.png"), counter.fetch_add(1));
    ImageTask->Format = EImageFormat::PNG;
    ImageTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
    ImageTask->bOverwriteFile = true;
    ImageTask->PixelPreProcessors.Add(TAsyncAlphaWrite<FColor>(255));

    FHighResScreenshotConfig& HighResScreenshotConfig = GetHighResScreenshotConfig();
    return HighResScreenshotConfig.ImageWriteQueue->Enqueue(MoveTemp(ImageTask));
#endif

    Readback->Unlock();
  });
}

bool FPixelReader::WritePixelsToArray(
    UTextureRenderTarget2D &RenderTarget,
    TArray<FColor> &BitMap)
{
  check(IsInGameThread());
  FTextureRenderTargetResource *RTResource =
      RenderTarget.GameThread_GetRenderTargetResource();
  if (RTResource == nullptr)
  {
    UE_LOG(LogCarla, Error, TEXT("FPixelReader: UTextureRenderTarget2D missing render target"));
    return false;
  }
  FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
  ReadPixelFlags.SetLinearToGamma(true);
  return RTResource->ReadPixels(BitMap, ReadPixelFlags);
}

TUniquePtr<TImagePixelData<FColor>> FPixelReader::DumpPixels(
    UTextureRenderTarget2D &RenderTarget)
{
  const FIntPoint DestSize(RenderTarget.GetSurfaceWidth(), RenderTarget.GetSurfaceHeight());
  TUniquePtr<TImagePixelData<FColor>> PixelData = MakeUnique<TImagePixelData<FColor>>(DestSize);
  TArray<FColor> Pixels(PixelData->Pixels.GetData(), PixelData->Pixels.Num());
  if (!WritePixelsToArray(RenderTarget, Pixels))
  {
    return nullptr;
  }
  PixelData->Pixels = MoveTemp(Pixels);
  return PixelData;
}

TFuture<bool> FPixelReader::SavePixelsToDisk(
    UTextureRenderTarget2D &RenderTarget,
    const FString &FilePath)
{
  return SavePixelsToDisk(DumpPixels(RenderTarget), FilePath);
}

TFuture<bool> FPixelReader::SavePixelsToDisk(
    TUniquePtr<TImagePixelData<FColor>> PixelData,
    const FString &FilePath)
{
  TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
  ImageTask->PixelData = MoveTemp(PixelData);
  ImageTask->Filename = FilePath;
  ImageTask->Format = EImageFormat::PNG;
  ImageTask->CompressionQuality = (int32) EImageCompressionQuality::Default;
  ImageTask->bOverwriteFile = true;
  ImageTask->PixelPreProcessors.Add(TAsyncAlphaWrite<FColor>(255));

  FHighResScreenshotConfig &HighResScreenshotConfig = GetHighResScreenshotConfig();
  return HighResScreenshotConfig.ImageWriteQueue->Enqueue(MoveTemp(ImageTask));
}
