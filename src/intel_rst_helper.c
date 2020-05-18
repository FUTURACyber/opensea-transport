//
// Do NOT modify or remove this copyright and license
//
// Copyright (c) 2012 - 2020 Seagate Technology LLC and/or its Affiliates, All Rights Reserved
//
// This software is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// ******************************************************************************************
// 
#if defined (ENABLE_INTEL_RST)
#include "common.h"
#include "intel_rst_defs.h"
#include "intel_rst_helper.h"

#if defined (ENABLE_CSMI)
#include "csmi_helper.h"
#endif

//generic function to handle taking in the various RAID FW Requests to keep code from being dumplicated
static int intel_RAID_FW_Request(tDevice *device, void *ptrDataRequest, uint32_t dataRequestLength, uint32_t timeoutSeconds, uint32_t intelFirmwareFunction, uint32_t intelFirmwareFlags, bool readFirmwareInfo, uint32_t *returnCode)
{
    int ret = OS_PASSTHROUGH_FAILURE;
    if (device)
    {
        size_t allocationSize = sizeof(IOCTL_RAID_FIRMWARE_BUFFER) + dataRequestLength;
        IOCTL_RAID_FIRMWARE_BUFFER *raidFirmwareRequest = (IOCTL_RAID_FIRMWARE_BUFFER*)calloc_aligned(allocationSize, sizeof(uint8_t), device->os_info.minimumAlignment);
        if (raidFirmwareRequest)
        {
            seatimer_t commandTimer;
            //fill in SRB_IO_HEADER first
            raidFirmwareRequest->Header.HeaderLength = sizeof(SRB_IO_CONTROL);
            memcpy(raidFirmwareRequest->Header.Signature, INTEL_RAID_FW_SIGNATURE, 8);
            raidFirmwareRequest->Header.Timeout = timeoutSeconds;
            if (device->drive_info.defaultTimeoutSeconds > 0 && device->drive_info.defaultTimeoutSeconds > timeoutSeconds)
            {
                raidFirmwareRequest->Header.Timeout = device->drive_info.defaultTimeoutSeconds;
            }
            else
            {
                if (timeoutSeconds != 0)
                {
                    raidFirmwareRequest->Header.Timeout = timeoutSeconds;
                }
                else
                {
                    raidFirmwareRequest->Header.Timeout = 15;
                }
            }
            raidFirmwareRequest->Header.ControlCode = IOCTL_RAID_FIRMWARE;
            raidFirmwareRequest->Header.Length = (ULONG)(allocationSize - sizeof(SRB_IO_CONTROL));
            //Next fill in IOCTL_RAID_FIRMWARE_BUFFER, then work down from there and memcpy the input data to this function since it will have the specific download function data in it
            raidFirmwareRequest->Request.Version = RAID_FIRMWARE_REQUEST_BLOCK_VERSION;
            raidFirmwareRequest->Request.TargetId = RESERVED;
            raidFirmwareRequest->Request.Lun = RESERVED;
            if (device->os_info.csmiDeviceData && device->os_info.csmiDeviceData->csmiDeviceInfoValid)
            {
                raidFirmwareRequest->Request.PathId = device->os_info.csmiDeviceData->portIdentifier;
            }
            else
            {
                //use Windows pathId
                raidFirmwareRequest->Request.PathId = device->os_info.scsi_addr.PathId;
                //TODO: may need to add in remaining scsi address in the future, but for now these other fields are reserved
            }
            //setup the firmware request
            raidFirmwareRequest->Request.FwRequestBlock.Version = INTEL_FIRMWARE_REQUEST_BLOCK_STRUCTURE_VERSION;
            raidFirmwareRequest->Request.FwRequestBlock.Size = sizeof(INTEL_FIRMWARE_REQUEST_BLOCK);
            raidFirmwareRequest->Request.FwRequestBlock.Function = intelFirmwareFunction;
            raidFirmwareRequest->Request.FwRequestBlock.Flags = intelFirmwareFlags;
            if (ptrDataRequest)
            {
                //NOTE: The offset should be a multiple of sizeof(void), which reading the structure types indicated that this should be the case for 64bit and 32bit builds. Anything else will need additional byte padding.
                raidFirmwareRequest->Request.FwRequestBlock.DataBufferOffset = sizeof(SRB_IO_CONTROL) + sizeof(RAID_FIRMWARE_REQUEST_BLOCK);
                raidFirmwareRequest->Request.FwRequestBlock.DataBufferLength = dataRequestLength;
                memcpy(raidFirmwareRequest + raidFirmwareRequest->Request.FwRequestBlock.DataBufferOffset, ptrDataRequest, dataRequestLength);
            }
            //send the command
            DWORD bytesReturned = 0;
            OVERLAPPED overlappedStruct;
            memset(&overlappedStruct, 0, sizeof(OVERLAPPED));
            overlappedStruct.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (overlappedStruct.hEvent == NULL)
            {
                safe_Free(raidFirmwareRequest);
                return OS_PASSTHROUGH_FAILURE;
            }
            start_Timer(&commandTimer);
            BOOL success = DeviceIoControl(device->os_info.fd,
                IOCTL_SCSI_MINIPORT,
                raidFirmwareRequest,
                (DWORD)allocationSize,
                raidFirmwareRequest,
                (DWORD)allocationSize,
                &bytesReturned,
                &overlappedStruct);
            device->os_info.last_error = GetLastError();
            if (ERROR_IO_PENDING == device->os_info.last_error)//This will only happen for overlapped commands. If the drive is opened without the overlapped flag, everything will work like old synchronous code.-TJE
            {
                success = GetOverlappedResult(device->os_info.fd, &overlappedStruct, &bytesReturned, TRUE);
            }
            else if (device->os_info.last_error != ERROR_SUCCESS)
            {
                ret = OS_PASSTHROUGH_FAILURE;
            }
            stop_Timer(&commandTimer);
            CloseHandle(overlappedStruct.hEvent);//close the overlapped handle since it isn't needed any more...-TJE
            overlappedStruct.hEvent = NULL;
            if (!success)
            {
                ret = FAILURE;
            }
            else
            {
                ret = SUCCESS;
                //should have completion data here
                if (returnCode)
                {
                    *returnCode = raidFirmwareRequest->Header.ReturnCode;
                }
                if (readFirmwareInfo)
                {
                    memcpy(ptrDataRequest, raidFirmwareRequest + raidFirmwareRequest->Request.FwRequestBlock.DataBufferOffset, dataRequestLength);
                }
            }
            safe_Free_aligned(raidFirmwareRequest);
        }
        else
        {
            ret = MEMORY_FAILURE;
        }
    }
    else
    {
        ret = BAD_PARAMETER;
    }
    return ret;
}

bool supports_Intel_Firmware_Download(tDevice *device)
{
    bool supported = false;
    uint32_t allocationSize = sizeof(INTEL_STORAGE_FIRMWARE_INFO_V2) + (sizeof(INTEL_STORAGE_FIRMWARE_SLOT_INFO_V2) * 7);//max of 7 slots
    PINTEL_STORAGE_FIRMWARE_INFO_V2 firmwareInfo = (PINTEL_STORAGE_FIRMWARE_INFO_V2)calloc(allocationSize, sizeof(uint8_t));//alignment not needed since this is passed to another function where it will be copied as needed
    if (firmwareInfo)
    {
        uint32_t flags = 0;
        uint32_t returnCode = 0;
        if (device->drive_info.drive_type == NVME_DRIVE)
        {
            flags |= INTEL_FIRMWARE_REQUEST_FLAG_CONTROLLER;
        }
        //do some setup first, then call intel_RAID_FW_Request to issue the command
        firmwareInfo->Version = INTEL_STORAGE_FIRMWARE_INFO_STRUCTURE_VERSION_V2;
        firmwareInfo->Size = sizeof(INTEL_STORAGE_FIRMWARE_INFO_V2);
        //nothing else needs setup, just issue the command
        if (SUCCESS == intel_RAID_FW_Request(device, firmwareInfo, allocationSize, 15, INTEL_FIRMWARE_FUNCTION_GET_INFO, flags, true, &returnCode))
        {
            supported = firmwareInfo->UpgradeSupport;
            //TODO: Need to store other things like alignment requirements similar to what is done in Windows 10 API today!
            if (device->os_info.csmiDeviceData)
            {
                device->os_info.csmiDeviceData->intelRSTSupport.intelRSTSupported = true;
                device->os_info.csmiDeviceData->intelRSTSupport.fwdlIOSupported = firmwareInfo->UpgradeSupport;
                device->os_info.csmiDeviceData->intelRSTSupport.maxXferSize = firmwareInfo->ImagePayloadMaxSize;
                device->os_info.csmiDeviceData->intelRSTSupport.payloadAlignment = firmwareInfo->ImagePayloadAlignment;
            }
        }
        safe_Free(firmwareInfo);
    }
    return supported;
}

//The idea with this function is that it can handle NVMe or SCSI with generic inputs that will work to reduce code
static int internal_Intel_FWDL_Function_Download(tDevice *device, uint32_t flags, uint32_t *returnCode, uint8_t* imagePtr, uint32_t imageDataLength, uint32_t imageOffset, uint8_t firmwareSlot, uint32_t timeoutSeconds)
{
    int ret = OS_COMMAND_NOT_AVAILABLE;
    if (device && imagePtr)
    {
        uint32_t allocationSize = sizeof(INTEL_STORAGE_FIRMWARE_DOWNLOAD_V2) + imageDataLength;
        PINTEL_STORAGE_FIRMWARE_DOWNLOAD_V2 download = (PINTEL_STORAGE_FIRMWARE_DOWNLOAD_V2)calloc(allocationSize, sizeof(uint8_t));//alignment not needed since this will get copied to an aligned location
        if (download)
        {
            download->Version = INTEL_STORAGE_FIRMWARE_DOWNLOAD_STRUCTURE_VERSION_V2;
            download->Size = sizeof(INTEL_STORAGE_FIRMWARE_DOWNLOAD_V2);
            download->Offset = imageOffset;
            download->BufferSize = imageDataLength;
            download->Slot = firmwareSlot;
            download->ImageSize = imageDataLength;//TODO: Not sure if this is supposed to be the same or different from the buffersize listed above
            memcpy(download->ImageBuffer, imagePtr, imageDataLength);
            ret = intel_RAID_FW_Request(device, download, allocationSize, timeoutSeconds, INTEL_FIRMWARE_FUNCTION_DOWNLOAD, flags, false, returnCode);
            safe_Free(download);
        }
        else
        {
            ret = BAD_PARAMETER;
        }
    }
    else
    {
        ret = BAD_PARAMETER;
    }
    return ret;
}

static int internal_Intel_FWDL_Function_Activate(tDevice *device, uint32_t flags, uint32_t *returnCode, uint8_t firmwareSlot, uint32_t timeoutSeconds)
{
    int ret = OS_COMMAND_NOT_AVAILABLE;
    if (device)
    {
        uint32_t allocationSize = sizeof(INTEL_STORAGE_FIRMWARE_ACTIVATE);
        PINTEL_STORAGE_FIRMWARE_ACTIVATE activate = (PINTEL_STORAGE_FIRMWARE_ACTIVATE)calloc(allocationSize, sizeof(uint8_t));//alignment not needed since this will get copied to an aligned location
        if (activate)
        {
            activate->Version = INTEL_STORAGE_FIRMWARE_ACTIVATE_STRUCTURE_VERSION;
            activate->Size = sizeof(INTEL_STORAGE_FIRMWARE_ACTIVATE);
            activate->SlotToActivate = firmwareSlot;
            ret = intel_RAID_FW_Request(device, activate, allocationSize, timeoutSeconds, INTEL_FIRMWARE_FUNCTION_ACTIVATE, flags, false, returnCode);
            safe_Free(activate);
        }
        else
        {
            ret = BAD_PARAMETER;
        }
    }
    else
    {
        ret = BAD_PARAMETER;
    }
    return ret;
}

//This must also check to ensure that the alignment requirements and transfer length requirements are also met
//This is really only needed on SCSI or ATA since NVMe will only take this API to update firmware
static bool is_Compatible_SCSI_FWDL_IO(ScsiIoCtx *scsiIoCtx, bool *isActivate)
{
    bool compatible = false;
    uint32_t transferLengthBytes = 0;
    //check if this is a SCSI Write buffer command or ATA download microcode. Can only support deferred and activate subcommands.
    if (scsiIoCtx->cdb[OPERATION_CODE] == WRITE_BUFFER_CMD)
    {
        uint8_t wbMode = M_GETBITRANGE(scsiIoCtx->cdb[1], 4, 0);
        if (wbMode == SCSI_WB_DL_MICROCODE_OFFSETS_SAVE_DEFER)
        {
            compatible = true;
            transferLengthBytes = M_BytesTo4ByteValue(0, scsiIoCtx->cdb[6], scsiIoCtx->cdb[7], scsiIoCtx->cdb[8]);
        }
        else if (wbMode == SCSI_WB_ACTIVATE_DEFERRED_MICROCODE)
        {
            compatible = true;
            if (isActivate)
            {
                *isActivate = true;
            }
        }
    }
    else if (scsiIoCtx->pAtaCmdOpts && (scsiIoCtx->pAtaCmdOpts->tfr.CommandStatus == ATA_DOWNLOAD_MICROCODE || scsiIoCtx->pAtaCmdOpts->tfr.CommandStatus == ATA_DOWNLOAD_MICROCODE_DMA))
    {
        if (scsiIoCtx->pAtaCmdOpts->tfr.ErrorFeature == 0x0E)
        {
            compatible = true;
            transferLengthBytes = M_BytesTo2ByteValue(scsiIoCtx->pAtaCmdOpts->tfr.LbaLow, scsiIoCtx->pAtaCmdOpts->tfr.SectorCount) * LEGACY_DRIVE_SEC_SIZE;
        }
        else if (scsiIoCtx->pAtaCmdOpts->tfr.ErrorFeature == 0x0F)
        {
            compatible = true;
            if (isActivate)
            {
                *isActivate = true;
            }
        }
    }
    if (compatible)
    {
        //before we call it a supported command, we need to validate if this meets the alignment requirements, etc
        if (!(*isActivate))
        {
            if (!(transferLengthBytes < scsiIoCtx->device->os_info.fwdlIOsupport.maxXferSize && (transferLengthBytes % scsiIoCtx->device->os_info.fwdlIOsupport.payloadAlignment == 0)))
            {
                compatible = false;
            }
        }
    }
    return compatible;
}

int send_Intel_Firmware_Download(ScsiIoCtx *scsiIoCtx)
{
    int ret = OS_COMMAND_NOT_AVAILABLE;
    bool isActivate = false;
    if (is_Compatible_SCSI_FWDL_IO(scsiIoCtx, &isActivate))
    {
        uint32_t returnCode = 0;//will most likely help with debugging, but we should check this to dummy up sense data or rtfrs as necessary.
        uint32_t flags = 0;//flags aren't required
        uint32_t timeout = scsiIoCtx->timeout;
        uint8_t firmwareSlot = 0;//TODO: Some intel documentation suggests that this should be INTEL_STORAGE_FIRMWARE_INFO_INVALID_SLOT, but we should test before using that
        //special case, if running in SCSI translation mode for NVMe, we should set the controller flag
        if (strcmp(scsiIoCtx->device->drive_info.T10_vendor_ident, "NVMe") == 0)
        {
            flags |= INTEL_FIRMWARE_REQUEST_FLAG_CONTROLLER;
        }
        if (isActivate)
        {
            //send activate command
            //set up the things we need to input to the activate from SCSI or ATA info as appropriate
            if (scsiIoCtx->pAtaCmdOpts)
            {
                timeout = scsiIoCtx->pAtaCmdOpts->timeout;
            }
            else
            {
                //assume SCSI write buffer if we made it this far. The is_Compatible_SCSI_FWDL_IO will filter out other commands since the opcode won't match
                firmwareSlot = scsiIoCtx->cdb[2];//firmware slot or buffer ID are "the same" in SNTL
            }
            //special case, if running in SCSI translation mode for NVMe, we should set the controller flag
            if (strcmp(scsiIoCtx->device->drive_info.T10_vendor_ident, "NVMe") == 0)
            {
                flags |= INTEL_FIRMWARE_REQUEST_FLAG_CONTROLLER;
            }
            ret = internal_Intel_FWDL_Function_Activate(scsiIoCtx->device, flags, &returnCode, firmwareSlot, timeout);
            //TODO: Dummy up sense data!
        }
        else
        {
            uint8_t *imagePtr = scsiIoCtx->pdata;
            uint32_t imageDataLength = scsiIoCtx->dataLength;
            uint32_t imageOffset = 0;
            //send download command
            if (scsiIoCtx->fwdlFirstSegment)
            {
                flags |= INTEL_FIRMWARE_REQUEST_FLAG_FIRST_SEGMENT;
            }
            if (scsiIoCtx->fwdlLastSegment)
            {
                flags |= INTEL_FIRMWARE_REQUEST_FLAG_LAST_SEGMENT;
            }
            //set up the things we need to input to the activate from SCSI or ATA info as appropriate
            if (scsiIoCtx->pAtaCmdOpts)
            {
                timeout = scsiIoCtx->pAtaCmdOpts->timeout;
                imagePtr = scsiIoCtx->pAtaCmdOpts->ptrData;
                imageDataLength = scsiIoCtx->pAtaCmdOpts->dataSize;
                imageOffset = M_BytesTo2ByteValue(scsiIoCtx->pAtaCmdOpts->tfr.LbaHi, scsiIoCtx->pAtaCmdOpts->tfr.LbaMid) * LEGACY_DRIVE_SEC_SIZE;
            }
            else
            {
                //assume SCSI write buffer if we made it this far. The is_Compatible_SCSI_FWDL_IO will filter out other commands since the opcode won't match
                firmwareSlot = scsiIoCtx->cdb[2];//firmware slot or buffer ID are "the same" in SNTL
                imageOffset = M_BytesTo4ByteValue(0, scsiIoCtx->cdb[3], scsiIoCtx->cdb[4], scsiIoCtx->cdb[5]);
            }
            ret = internal_Intel_FWDL_Function_Download(scsiIoCtx->device, flags, &returnCode, imagePtr, imageDataLength, imageOffset, firmwareSlot, timeout);
            //TODO: Dummy up sense data!
        }
    }
    return ret;
}

#if !defined (DISABLE_NVME_PASSTHROUGH)
    //NOTE: This function will handle calling appropriate NVMe firmware update function as well
    //NOTE2: This will not issue whatever command you want. Only certain commands are supported by the driver. This function will attempt any command given in case driver updates allow other commands in the future.
static int send_Intel_NVM_Passthrough_Command(nvmeCmdCtx *nvmeIoCtx)
{
    int ret = OS_PASSTHROUGH_FAILURE;
    if (nvmeIoCtx)
    {
        seatimer_t commandTimer;
        NVME_IOCTL_PASS_THROUGH *nvmPassthroughCommand = NULL;
        size_t allocationSize = sizeof(NVME_IOCTL_PASS_THROUGH) + nvmeIoCtx->dataSize;
        nvmPassthroughCommand = (NVME_IOCTL_PASS_THROUGH*)calloc_aligned(allocationSize, sizeof(uint8_t), nvmeIoCtx->device->os_info.minimumAlignment);
        if (nvmPassthroughCommand)
        {
            memset(&commandTimer, 0, sizeof(seatimer_t));
            //setup the header (SRB_IO_CONTROL) first
            nvmPassthroughCommand->Header.HeaderLength = sizeof(SRB_IO_CONTROL);
            memcpy(nvmPassthroughCommand->Header.Signature, INTELNVM_SIGNATURE, strlen(INTELNVM_SIGNATURE));
            nvmPassthroughCommand->Header.Timeout = nvmeIoCtx->timeout;
            if (nvmeIoCtx->device->drive_info.defaultTimeoutSeconds > 0 && nvmeIoCtx->device->drive_info.defaultTimeoutSeconds > nvmeIoCtx->timeout)
            {
                nvmPassthroughCommand->Header.Timeout = nvmeIoCtx->device->drive_info.defaultTimeoutSeconds;
            }
            else
            {
                if (nvmeIoCtx->timeout != 0)
                {
                    nvmPassthroughCommand->Header.Timeout = nvmeIoCtx->timeout;
                }
                else
                {
                    nvmPassthroughCommand->Header.Timeout = 15;
                }
            }
            nvmPassthroughCommand->Header.ControlCode = IOCTL_NVME_PASSTHROUGH;
            nvmPassthroughCommand->Header.Length = (ULONG)(allocationSize - sizeof(SRB_IO_CONTROL));
            //srb_io_control has been setup, now to main struct fields (minus data which is set when configuring the NVMe command)
            nvmPassthroughCommand->Version = NVME_PASS_THROUGH_VERSION;
            //setup the SCSI address, pathID is only part needed at time of writing, the rest is currently reserved, so leave as zeros
            //pathId must be from the SCSI address in non-raid mode, or CSMI port number, so this is where it begins to get a little messy
            nvmPassthroughCommand->TargetId = RESERVED;
            nvmPassthroughCommand->Lun = RESERVED;
            if (nvmeIoCtx->device->os_info.csmiDeviceData && nvmeIoCtx->device->os_info.csmiDeviceData->csmiDeviceInfoValid)
            {
                nvmPassthroughCommand->PathId = nvmeIoCtx->device->os_info.csmiDeviceData->portIdentifier;
            }
            else
            {
                //use Windows pathId
                nvmPassthroughCommand->PathId = nvmeIoCtx->device->os_info.scsi_addr.PathId;
                //TODO: may need to add in remaining scsi address in the future, but for now these other fields are reserved
            }
            //time to start setting up the command!
            nvmPassthroughCommand->Parameters.Command.DWord0 = nvmeIoCtx->cmd.dwords.cdw0;
            nvmPassthroughCommand->Parameters.Command.DWord1 = nvmeIoCtx->cmd.dwords.cdw1;
            nvmPassthroughCommand->Parameters.Command.DWord2 = nvmeIoCtx->cmd.dwords.cdw2;
            nvmPassthroughCommand->Parameters.Command.DWord3 = nvmeIoCtx->cmd.dwords.cdw3;
            nvmPassthroughCommand->Parameters.Command.DWord4 = nvmeIoCtx->cmd.dwords.cdw4;
            nvmPassthroughCommand->Parameters.Command.DWord5 = nvmeIoCtx->cmd.dwords.cdw5;
            nvmPassthroughCommand->Parameters.Command.DWord6 = nvmeIoCtx->cmd.dwords.cdw6;
            nvmPassthroughCommand->Parameters.Command.DWord7 = nvmeIoCtx->cmd.dwords.cdw7;
            nvmPassthroughCommand->Parameters.Command.DWord8 = nvmeIoCtx->cmd.dwords.cdw8;
            nvmPassthroughCommand->Parameters.Command.DWord9 = nvmeIoCtx->cmd.dwords.cdw9;
            nvmPassthroughCommand->Parameters.Command.DWord10 = nvmeIoCtx->cmd.dwords.cdw10;
            nvmPassthroughCommand->Parameters.Command.DWord11 = nvmeIoCtx->cmd.dwords.cdw11;
            nvmPassthroughCommand->Parameters.Command.DWord12 = nvmeIoCtx->cmd.dwords.cdw12;
            nvmPassthroughCommand->Parameters.Command.DWord13 = nvmeIoCtx->cmd.dwords.cdw13;
            nvmPassthroughCommand->Parameters.Command.DWord14 = nvmeIoCtx->cmd.dwords.cdw14;
            nvmPassthroughCommand->Parameters.Command.DWord15 = nvmeIoCtx->cmd.dwords.cdw15;
            if (nvmeIoCtx->commandType == NVM_ADMIN_CMD)
            {
                nvmPassthroughCommand->Parameters.IsIOCommandSet = FALSE;
            }
            else
            {
                nvmPassthroughCommand->Parameters.IsIOCommandSet = TRUE;
            }
            switch (nvmeIoCtx->commandDirection)
            {
            case XFER_DATA_OUT:
                memcpy(nvmPassthroughCommand->data, nvmeIoCtx->ptrData, nvmeIoCtx->dataSize);
                //Fallthrough to setup the rest since it's the same
            case XFER_DATA_IN:
                //set the data length and offset
                nvmPassthroughCommand->Parameters.DataBufferLength = nvmeIoCtx->dataSize;
                nvmPassthroughCommand->Parameters.DataBufferOffset = offsetof(NVME_IOCTL_PASS_THROUGH, data);//NOTE: This must be DWORD aligned. The structure being used SHOULD be, but if things aren't working, this could be the problem
                break;
            case XFER_NO_DATA:
                nvmPassthroughCommand->Parameters.DataBufferLength = 0;
                nvmPassthroughCommand->Parameters.DataBufferOffset = 0;//this should be ok since we aren't doing a transfer
                break;
            default:
                safe_Free_aligned(nvmPassthroughCommand);
                return OS_COMMAND_NOT_AVAILABLE;
            }
            DWORD bytesReturned = 0;
            OVERLAPPED overlappedStruct;
            memset(&overlappedStruct, 0, sizeof(OVERLAPPED));
            overlappedStruct.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (overlappedStruct.hEvent == NULL)
            {
                safe_Free(nvmPassthroughCommand);
                return OS_PASSTHROUGH_FAILURE;
            }
            start_Timer(&commandTimer);
            BOOL success = DeviceIoControl(nvmeIoCtx->device->os_info.fd,
                IOCTL_SCSI_MINIPORT,
                nvmPassthroughCommand,
                (DWORD)allocationSize,
                nvmPassthroughCommand,
                (DWORD)allocationSize,
                &bytesReturned,
                &overlappedStruct);
            nvmeIoCtx->device->os_info.last_error = GetLastError();
            if (ERROR_IO_PENDING == nvmeIoCtx->device->os_info.last_error)//This will only happen for overlapped commands. If the drive is opened without the overlapped flag, everything will work like old synchronous code.-TJE
            {
                success = GetOverlappedResult(nvmeIoCtx->device->os_info.fd, &overlappedStruct, &bytesReturned, TRUE);
            }
            else if (nvmeIoCtx->device->os_info.last_error != ERROR_SUCCESS)
            {
                ret = OS_PASSTHROUGH_FAILURE;
            }
            stop_Timer(&commandTimer);
            CloseHandle(overlappedStruct.hEvent);//close the overlapped handle since it isn't needed any more...-TJE
            overlappedStruct.hEvent = NULL;
            if (!success)
            {
                ret = FAILURE;
            }
            else
            {
                ret = SUCCESS;
            }
            //TODO: Handle unsupported commands error codes and others that don't fill in the completion data
            //TODO: Verbose output including windows error codes and the SRB_IO header return code, etc.
            //now handle error codes and completion and copying back read data
            //if(nvmPassthroughCommand->Header.ReturnCode)
            if (ret == SUCCESS)
            {
                if (nvmeIoCtx->commandDirection == XFER_DATA_IN && nvmeIoCtx->ptrData)
                {
                    memcpy(nvmeIoCtx->ptrData, nvmPassthroughCommand->data, nvmeIoCtx->dataSize);
                }
            }
            //copy completion data
            nvmeIoCtx->commandCompletionData.dw0Valid = true;
            nvmeIoCtx->commandCompletionData.dw1Valid = true;
            nvmeIoCtx->commandCompletionData.dw2Valid = true;
            nvmeIoCtx->commandCompletionData.dw3Valid = true;
            nvmeIoCtx->commandCompletionData.commandSpecific = nvmPassthroughCommand->Parameters.Completion.completion0;
            nvmeIoCtx->commandCompletionData.dw1Reserved = nvmPassthroughCommand->Parameters.Completion.completion1;
            nvmeIoCtx->commandCompletionData.sqIDandHeadPtr = nvmPassthroughCommand->Parameters.Completion.completion2;
            nvmeIoCtx->commandCompletionData.statusAndCID = nvmPassthroughCommand->Parameters.Completion.completion3;
            //set command time
            nvmeIoCtx->device->drive_info.lastCommandTimeNanoSeconds = get_Nano_Seconds(commandTimer);
            safe_Free_aligned(nvmPassthroughCommand);
        }
        else
        {
            ret = MEMORY_FAILURE;
        }
    }
    else
    {
        ret = BAD_PARAMETER;
    }
    return ret;
}


int send_Intel_NVM_Firmware_Download(nvmeCmdCtx *nvmeIoCtx)
{
    int ret = OS_PASSTHROUGH_FAILURE;
    if (nvmeIoCtx)
    {
        if (nvmeIoCtx->commandType == NVM_ADMIN_CMD)
        {
            uint32_t flags = INTEL_FIRMWARE_REQUEST_FLAG_CONTROLLER;//always set this for NVMe
            uint32_t returnCode = 0;
            uint8_t firmwareSlot = INTEL_STORAGE_FIRMWARE_INFO_INVALID_SLOT;//Starting with this...we may need to change this later!!!
            //check if sending download or if sending activate
            if (nvmeIoCtx->cmd.adminCmd.opcode == NVME_ADMIN_CMD_DOWNLOAD_FW)
            {
                if (nvmeIoCtx->fwdlFirstSegment)
                {
                    flags |= INTEL_FIRMWARE_REQUEST_FLAG_FIRST_SEGMENT;
                }
                if (nvmeIoCtx->fwdlLastSegment)
                {
                    flags |= INTEL_FIRMWARE_REQUEST_FLAG_LAST_SEGMENT;
                }
                //send download command API
                ret = internal_Intel_FWDL_Function_Download(nvmeIoCtx->device, flags, &returnCode, nvmeIoCtx->ptrData, nvmeIoCtx->cmd.adminCmd.cdw10, nvmeIoCtx->cmd.adminCmd.cdw11, firmwareSlot, nvmeIoCtx->timeout);
                //TODO: Dummy up output data based on return code.
            }
            else if (nvmeIoCtx->cmd.adminCmd.opcode == NVME_ADMIN_CMD_ACTIVATE_FW)
            {
                uint8_t activateAction = M_GETBITRANGE(nvmeIoCtx->cmd.adminCmd.cdw10, 5, 3);
                if (activateAction == NVME_CA_ACTIVITE_ON_RST || activateAction == NVME_CA_ACTIVITE_IMMEDIATE)//check the activate action
                {
                    //Activate actions 2, & 3 sound like the closest match to this flag. Each of these requests switching to the a firmware already on the drive.
                    //Activate action 0 & 1 say to replace a firmware image in a specified slot (and to or not to activate).
                    flags |= STORAGE_HW_FIRMWARE_REQUEST_FLAG_SWITCH_TO_EXISTING_FIRMWARE;
                }
                firmwareSlot = M_GETBITRANGE(nvmeIoCtx->cmd.adminCmd.cdw10, 2, 0);
                //send activate command API
                ret = internal_Intel_FWDL_Function_Activate(nvmeIoCtx->device, flags, &returnCode, firmwareSlot, nvmeIoCtx->timeout);
                //TODO: Dummy up output data based on return code.
            }
            else
            {
                ret = BAD_PARAMETER;
            }
        }
        else
        {
            ret = BAD_PARAMETER;
        }
    }
    else
    {
        ret = BAD_PARAMETER;
    }
    return ret;
}

int send_Intel_NVM_Command(nvmeCmdCtx *nvmeIoCtx)
{
    int ret = OS_PASSTHROUGH_FAILURE;
    if (nvmeIoCtx)
    {
        if (nvmeIoCtx->commandType == NVM_ADMIN_CMD)
        {
            switch (nvmeIoCtx->cmd.adminCmd.opcode)
            {
            case NVME_ADMIN_CMD_DOWNLOAD_FW:
            case NVME_ADMIN_CMD_ACTIVATE_FW:
                ret = send_Intel_NVM_Firmware_Download(nvmeIoCtx);
                break;
            default:
                ret = send_Intel_NVM_Passthrough_Command(nvmeIoCtx);
                break;
            }
        }
        else //IO command
        {
            ret = send_Intel_NVM_Passthrough_Command(nvmeIoCtx);
        }
    }
    else
    {
        ret = BAD_PARAMETER;
    }
    return ret;
}

#endif //!DISABLE_NVME_PASSTHROUGH

#endif //ENABLE_INTEL_RST