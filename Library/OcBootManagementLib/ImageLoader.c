/** @file
  Copyright (C) 2019, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "BootManagementInternal.h"

#include <IndustryStandard/OcPeImage.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include <Guid/AppleVariable.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>
#include <Guid/OcVariable.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleSecureBootLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcMachoLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcPeCoffLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>

#if defined(MDE_CPU_IA32)
  #define OC_IMAGE_FILE_MACHINE  IMAGE_FILE_MACHINE_I386
#elif defined(MDE_CPU_X64)
  #define OC_IMAGE_FILE_MACHINE  IMAGE_FILE_MACHINE_X64
#else
  #error Unsupported architecture.
#endif

STATIC EFI_GUID mOcLoadedImageProtocolGuid = {
  0x1f3c963d, 0xf9dc, 0x4537, { 0xbb, 0x06, 0xd8, 0x08, 0x46, 0x4a, 0x85, 0x2e }
};

typedef struct {
  EFI_IMAGE_ENTRY_POINT  EntryPoint;
  EFI_PHYSICAL_ADDRESS   ImageArea;
  UINTN                  PageCount;
} OC_LOADED_IMAGE_PROTOCOL;

STATIC EFI_IMAGE_LOAD   mOriginalEfiLoadImage;
STATIC EFI_IMAGE_START  mOriginalEfiStartImage;
STATIC EFI_IMAGE_UNLOAD mOriginalEfiUnloadImage;
STATIC EFI_EXIT         mOriginalEfiExit;
STATIC BOOLEAN          mDirectImageLoaderEnabled;

STATIC
EFI_STATUS
InternalEfiLoadImageFile (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  OUT UINTN                     *FileSize,
  OUT VOID                      **FileBuffer
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  VOID               *Buffer;
  UINT32             Size;

  Status = OcOpenFileByDevicePath (
    &DevicePath,
    &File,
    EFI_FILE_MODE_READ,
    0
    );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = GetFileSize (
    File,
    &Size
    );
  if (EFI_ERROR (Status) || Size == 0) {
    File->Close (File);
    return EFI_UNSUPPORTED;
  }

  Buffer = AllocatePool (Size);
  if (Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = GetFileData (
    File,
    0,
    Size,
    Buffer
    );
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    File->Close (File);
    return EFI_DEVICE_ERROR;
  }

  *FileBuffer = Buffer;
  *FileSize   = Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InternalEfiLoadImageProtocol (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN  BOOLEAN                   UseLoadImage2,
  OUT UINTN                     *FileSize,
  OUT VOID                      **FileBuffer
  )
{
  //
  // TODO: Implement image load protocol if necessary.
  //
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
InternalUpdateLoadedImage (
  IN EFI_HANDLE                ImageHandle,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 DeviceHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *RemainingDevicePath;

  Status = gBS->HandleProtocol (
    ImageHandle,
    &gEfiLoadedImageProtocolGuid,
    (VOID **) &LoadedImage
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RemainingDevicePath = DevicePath;
  Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &RemainingDevicePath, &DeviceHandle);
  if (EFI_ERROR (Status)) {
    //
    // TODO: Handle load protocol if necessary.
    //
    return Status;
  }

  if (LoadedImage->DeviceHandle != DeviceHandle) {
    LoadedImage->DeviceHandle = DeviceHandle;
    LoadedImage->FilePath     = DuplicateDevicePath (RemainingDevicePath);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
OcDirectLoadImage (
  IN  BOOLEAN                  BootPolicy,
  IN  EFI_HANDLE               ParentImageHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN  VOID                     *SourceBuffer OPTIONAL,
  IN  UINTN                    SourceSize,
  OUT EFI_HANDLE               *ImageHandle
  )
{
  EFI_STATUS                   Status;
  IMAGE_STATUS                 ImageStatus;
  PE_COFF_LOADER_IMAGE_CONTEXT ImageContext;      
  EFI_PHYSICAL_ADDRESS         DestinationArea;
  VOID                         *DestinationBuffer;
  OC_LOADED_IMAGE_PROTOCOL     *OcLoadedImage;
  EFI_LOADED_IMAGE_PROTOCOL    *LoadedImage;
  
  ASSERT (SourceBuffer != NULL);

  //
  // Initialize the image context.
  //
  ImageStatus = OcPeCoffLoaderInitializeContext (
    &ImageContext,
    SourceBuffer,
    SourceSize
    );
  if (ImageStatus != IMAGE_ERROR_SUCCESS) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff init failure - %d\n", ImageStatus));
    return EFI_UNSUPPORTED;
  }
  //
  // Reject images that are not meant for the platform's architecture.
  //
  if (ImageContext.Machine != OC_IMAGE_FILE_MACHINE) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff wrong machine - %x\n", ImageContext.Machine));
    return EFI_UNSUPPORTED;
  }
  //
  // Reject RT drivers for the moment.
  //
  if (ImageContext.Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff no support for RT drivers\n"));
    return EFI_UNSUPPORTED;
  }
  //
  // Allocate the image destination memory.
  // FIXME: RT drivers require EfiRuntimeServicesCode.
  //
  Status = gBS->AllocatePages (
    AllocateAnyPages,
    EfiBootServicesCode,
    EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage),
    &DestinationArea
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DestinationBuffer = (VOID *)(UINTN) DestinationArea;

  //
  // Load SourceBuffer into DestinationBuffer.
  //
  ImageStatus = OcPeCoffLoaderLoadImage (
    &ImageContext,
    DestinationBuffer,
    ImageContext.SizeOfImage
    );
  if (ImageStatus != IMAGE_ERROR_SUCCESS) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff load image error - %d\n", ImageStatus));
    FreePool (DestinationBuffer);
    return EFI_UNSUPPORTED;
  }
  //
  // Relocate the loaded image to the destination address.
  //
  ImageStatus = OcPeCoffLoaderRelocateImage (
    &ImageContext,
    (UINTN) DestinationBuffer
    );
  if (ImageStatus != IMAGE_ERROR_SUCCESS) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff relocate image error - %d\n", ImageStatus));
    FreePool (DestinationBuffer);
    return EFI_UNSUPPORTED;
  }
  //
  // Construct a LoadedImage protocol for the image.
  //
  OcLoadedImage = AllocatePool (sizeof (*OcLoadedImage) + sizeof (*LoadedImage));
  if (OcLoadedImage == NULL) {
    FreePool (DestinationBuffer);
    return EFI_OUT_OF_RESOURCES;
  }

  OcLoadedImage->EntryPoint = (VOID *) ((UINTN) DestinationBuffer + ImageContext.AddressOfEntryPoint);
  OcLoadedImage->ImageArea  = DestinationArea;
  OcLoadedImage->PageCount  = EFI_SIZE_TO_PAGES (ImageContext.SizeOfImage);

  LoadedImage = (EFI_LOADED_IMAGE *)(OcLoadedImage + 1);

  LoadedImage->Revision        = EFI_LOADED_IMAGE_INFORMATION_REVISION;
  LoadedImage->ParentHandle    = ParentImageHandle;
  LoadedImage->SystemTable     = gST;
  LoadedImage->DeviceHandle    = NULL;
  LoadedImage->FilePath        = NULL;
  LoadedImage->Reserved        = NULL;
  LoadedImage->LoadOptionsSize = 0;
  LoadedImage->LoadOptions     = NULL;
  LoadedImage->ImageBase       = DestinationBuffer;
  LoadedImage->ImageSize       = ImageContext.SizeOfImage;
  //
  // FIXME: Support RT drivers.
  //
  LoadedImage->ImageCodeType   = EfiBootServicesCode;
  LoadedImage->ImageDataType   = EfiBootServicesData;
  //
  // FIXME: Support driver unloading.
  //
  LoadedImage->Unload          = NULL;
  //
  // Install LoadedImage and the image's entry point.
  //
  *ImageHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
    ImageHandle,
    &gEfiLoadedImageProtocolGuid,
    LoadedImage,
    &mOcLoadedImageProtocolGuid,
    OcLoadedImage,
    NULL
    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCB: PeCoff proto install error - %r\n", Status));
    FreePool (OcLoadedImage);
    FreePool (DestinationBuffer);
    return Status;
  }

  return EFI_SUCCESS;
}
/**
  Simplified start image routine for OcDirectLoadImage.

  @param[in]   OcLoadedImage     Our loaded image instance.
  @param[in]   ImageHandle       Handle of image to be started.
  @param[out]  ExitDataSize      The pointer to the size, in bytes, of ExitData.
  @param[out]  ExitData          The pointer to a pointer to a data buffer that includes a Null-terminated
                                 string, optionally followed by additional binary data.

  @retval EFI_SUCCESS on success.
**/
STATIC
EFI_STATUS
InternalDirectStartImage (
  IN  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage,
  IN  EFI_HANDLE                ImageHandle,
  OUT UINTN                     *ExitDataSize,
  OUT CHAR16                    **ExitData OPTIONAL
  )
{
  //
  // Invoke the manually loaded image entry point.
  //
  OcLoadedImage->EntryPoint (ImageHandle, gST);
  //
  // FIXME: Support gBS->Exit().
  //
  // NOTE: EFI 1.10 is not supported, refer to
  // https://github.com/tianocore/edk2/blob/d8dd54f071cfd60a2dcf5426764a89cd91213420/MdeModulePkg/Core/Dxe/Image/Image.c#L1686-L1697
  //
  if (ExitDataSize != NULL) {
    *ExitDataSize = 0;
  }

  if (ExitData != NULL) {
    *ExitData = NULL;
  }

  return EFI_SUCCESS;
}

/**
  Unload image routine for OcDirectLoadImage.

  @param[in]  OcLoadedImage     Our loaded image instance.
  @param[in]  ImageHandle       Handle that identifies the image to be unloaded.

  @retval EFI_SUCCESS           The image has been unloaded.
**/
STATIC
EFI_STATUS
InternalDirectUnloadImage (
  IN  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage,
  IN  EFI_HANDLE                ImageHandle
  )
{
  //
  // FIXME: Implement unloading.
  //
  DEBUG ((
    DEBUG_INFO,
    "OCB: Requested unsupported unloading"
    ));

  return EFI_INVALID_PARAMETER;
}

/**
  Unload image routine for OcDirectLoadImage.

  @param[in]  OcLoadedImage     Our loaded image instance.
  @param[in]  ImageHandle       Handle that identifies the image to be unloaded.
  @param[in]  ExitStatus        The image's exit code.
  @param[in]  ExitDataSize      The size, in bytes, of ExitData. Ignored if ExitStatus is EFI_SUCCESS.
  @param[in]  ExitData          The pointer to a data buffer that includes a Null-terminated string,
                                optionally followed by additional binary data. The string is a
                                description that the caller may use to further indicate the reason
                                for the image's exit. ExitData is only valid if ExitStatus
                                is something other than EFI_SUCCESS. The ExitData buffer
                                must be allocated by calling AllocatePool().

  @retval EFI_SUCCESS           The image has been unloaded.
**/
STATIC
EFI_STATUS
InternalDirectExit (
  IN  OC_LOADED_IMAGE_PROTOCOL     *OcLoadedImage,
  IN  EFI_HANDLE                   ImageHandle,
  IN  EFI_STATUS                   ExitStatus,
  IN  UINTN                        ExitDataSize,
  IN  CHAR16                       *ExitData     OPTIONAL
  )
{
  //
  // FIXME: Implement exit.
  //
  DEBUG ((
    DEBUG_INFO,
    "OCB: Requested unsupported exit"
    ));

  return EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
EFIAPI
InternalEfiLoadImage (
  IN  BOOLEAN                      BootPolicy,
  IN  EFI_HANDLE                   ParentImageHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL     *DevicePath,
  IN  VOID                         *SourceBuffer OPTIONAL,
  IN  UINTN                        SourceSize,
  OUT EFI_HANDLE                   *ImageHandle
  )
{
  EFI_STATUS                 SecureBootStatus;
  EFI_STATUS                 Status;
  VOID                       *AllocatedBuffer;
  UINT32                     RealSize;

  if (ParentImageHandle == NULL || ImageHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (SourceBuffer == NULL && DevicePath == NULL) {
    return EFI_NOT_FOUND;
  }

  if (SourceBuffer != NULL && SourceSize == 0) {
    return EFI_UNSUPPORTED;
  }

  AllocatedBuffer = NULL;
  if (SourceBuffer == NULL) {
    Status = InternalEfiLoadImageFile (
      DevicePath,
      &SourceSize,
      &SourceBuffer
      );
    if (EFI_ERROR (Status)) {
      Status = InternalEfiLoadImageProtocol (
        DevicePath,
        BootPolicy == FALSE,
        &SourceSize,
        &SourceBuffer
        );
    }

    if (!EFI_ERROR (Status)) {
      AllocatedBuffer = SourceBuffer;
    }
  }

  if (DevicePath != NULL && SourceBuffer != NULL && mDirectImageLoaderEnabled) {
    SecureBootStatus = OcAppleSecureBootVerify (
      DevicePath,
      SourceBuffer,
      SourceSize
      );
  } else {
    SecureBootStatus = EFI_UNSUPPORTED;
  }

  //
  // A security violation means we should just die.
  //
  if (SecureBootStatus == EFI_SECURITY_VIOLATION) {
    DEBUG ((
      DEBUG_WARN,
      "OCB: Apple Secure Boot prohibits this boot entry, enforcing!\n"
      ));
    return EFI_SECURITY_VIOLATION;
  }

  if (SourceBuffer != NULL) {
    RealSize = (UINT32) SourceSize;
#ifdef MDE_CPU_IA32
    Status = FatFilterArchitecture32 ((UINT8 **) &SourceBuffer, &RealSize);
#else
    Status = FatFilterArchitecture64 ((UINT8 **) &SourceBuffer, &RealSize);
#endif
    if (!EFI_ERROR (Status)) {
      SourceSize = RealSize;
    } else if (AllocatedBuffer != NULL) {
      SourceBuffer = NULL;
      SourceSize   = 0;
    }
  }

  //
  // Load the image ourselves in secure boot mode.
  //
  if (SecureBootStatus == EFI_SUCCESS) {
    if (SourceBuffer != NULL) {
      Status = OcDirectLoadImage (
        FALSE,
        ParentImageHandle,
        DevicePath,
        SourceBuffer,
        SourceSize,
        ImageHandle
        );
    } else {
      //
      // We verified the image, but contained garbage.
      // This should not happen, just abort.
      //
      Status = EFI_UNSUPPORTED;
    }
  } else {
    Status = mOriginalEfiLoadImage (
      BootPolicy,
      ParentImageHandle,
      DevicePath,
      SourceBuffer,
      SourceSize,
      ImageHandle
      );
  }

  if (AllocatedBuffer != NULL) {
    FreePool (AllocatedBuffer);
  }

  //
  // Some firmwares may not update loaded image protocol fields correctly
  // when loading via source buffer. Do it here.
  //
  if (!EFI_ERROR (Status) && SourceBuffer != NULL && DevicePath != NULL) {
    InternalUpdateLoadedImage (*ImageHandle, DevicePath);
  }

  return Status;
}

EFI_STATUS
EFIAPI
InternalEfiStartImage (
  IN  EFI_HANDLE  ImageHandle,
  OUT UINTN       *ExitDataSize,
  OUT CHAR16      **ExitData OPTIONAL
  )
{
  EFI_STATUS                Status;
  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage;

  //
  // If we loaded the image, invoke the entry point manually.
  //
  Status = gBS->HandleProtocol (
    ImageHandle,
    &mOcLoadedImageProtocolGuid,
    (VOID **) &OcLoadedImage
    );
  if (!EFI_ERROR (Status)) {
    return InternalDirectStartImage (
      OcLoadedImage,
      ImageHandle,
      ExitDataSize,
      ExitData
      );
  }

  return mOriginalEfiStartImage (ImageHandle, ExitDataSize, ExitData);
}

STATIC
EFI_STATUS
EFIAPI
InternalEfiUnloadImage (
  IN  EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                Status;
  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage;

  //
  // If we loaded the image, do the unloading manually.
  //
  Status = gBS->HandleProtocol (
    ImageHandle,
    &mOcLoadedImageProtocolGuid,
    (VOID **) &OcLoadedImage
    );
  if (!EFI_ERROR (Status)) {
    return InternalDirectUnloadImage (
      OcLoadedImage,
      ImageHandle
      );
  }

  return mOriginalEfiUnloadImage (ImageHandle);
}

STATIC
EFI_STATUS
EFIAPI
InternalEfiExit (
  IN  EFI_HANDLE                   ImageHandle,
  IN  EFI_STATUS                   ExitStatus,
  IN  UINTN                        ExitDataSize,
  IN  CHAR16                       *ExitData     OPTIONAL
  )
{
  EFI_STATUS                Status;
  OC_LOADED_IMAGE_PROTOCOL  *OcLoadedImage;

  //
  // If we loaded the image, do the exit manually.
  //
  Status = gBS->HandleProtocol (
    ImageHandle,
    &mOcLoadedImageProtocolGuid,
    (VOID **) &OcLoadedImage
    );
  if (!EFI_ERROR (Status)) {
    return InternalDirectExit (
      OcLoadedImage,
      ImageHandle,
      ExitStatus,
      ExitDataSize,
      ExitData
      );
  }

  return mOriginalEfiUnloadImage (ImageHandle);
}

VOID
OcInitDirectImageLoader (
  VOID
  )
{
  mOriginalEfiLoadImage   = gBS->LoadImage;
  mOriginalEfiStartImage  = gBS->StartImage;
  mOriginalEfiUnloadImage = gBS->UnloadImage;
  mOriginalEfiExit        = gBS->Exit;

  gBS->LoadImage   = InternalEfiLoadImage;
  gBS->StartImage  = InternalEfiStartImage;
  gBS->UnloadImage = InternalEfiUnloadImage;
  gBS->Exit        = InternalEfiExit;

  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
}

VOID
OcActivateDirectImageLoader (
  VOID
  )
{
  mDirectImageLoaderEnabled = TRUE;
}
