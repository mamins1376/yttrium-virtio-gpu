/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#include "viogpu_vidpn.h"
#include "viogpu_adapter.h"
#include "bitops.h"
#include "baseobj.h"
#include "edid.h"
#include "trace.h"

static const LONGLONG kVsyncPeriod100ns = 166666LL; // 60 Hz
static const LONGLONG kVsyncMaxLead100ns = 333333LL;

PAGED_CODE_SEG_BEGIN

VioGpuVidPN::VioGpuVidPN(VioGpuAdapter *adapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    m_pAdapter = adapter;
    m_pDxgkInterface = adapter->GetDxgkInterface();

    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    m_pModes = NULL;
    m_ModeGeneration = 0;
    ExInitializeFastMutex(&m_ModesLock);
    m_pFrameBuf = NULL;

    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;
    KeInitializeSpinLock(&m_vsyncTimerLock);
    KeInitializeTimerEx(&m_vsyncNotifyTimer, SynchronizationTimer);
    KeInitializeDpc(&m_vsyncNotifyDpc, VioGpuVidPN::VsyncNotifyTimerDpc, this);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
}

VioGpuVidPN::~VioGpuVidPN()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    DestroyFrameBufferObj(TRUE);

    // Drop the published snapshot.  The adapter must already have stopped the config work thread
    // and stopped dxgkrnl from entering our DDIs: the reference count keeps a snapshot alive for
    // readers, it does not keep this object alive.
    VIOGPU_MODE_SNAPSHOT *pSnapshot = NULL;
    ExAcquireFastMutex(&m_ModesLock);
    pSnapshot = m_pModes;
    m_pModes = NULL;
    ExReleaseFastMutex(&m_ModesLock);
    if (pSnapshot != NULL && InterlockedDecrement(&pSnapshot->Refs) == 0)
    {
        FreeSnapshot(pSnapshot);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
}

NTSTATUS VioGpuVidPN::Start(ULONG *pNumberOfViews, ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    m_CurrentModes[0].DispInfo.TargetId = D3DDDI_ID_UNINITIALIZED;

    NTSTATUS SnapshotStatus = RefreshModeSnapshot(&m_CurrentModes[0].DispInfo);
    NTSTATUS Status = SnapshotStatus;
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s GetModeList failed with %x\n", __FUNCTION__, Status));
        VioGpuDbgBreak();
    }

    if (m_pAdapter->IsVgaDevice())
    {
        Status = AcquirePostDisplayOwnership();
        if (!NT_SUCCESS(Status))
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    DbgPrint(TRACE_LEVEL_FATAL,
             ("DxgkCbAcquirePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %d\n",
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    if (m_SystemDisplayInfo.Width == 0)
    {
        m_SystemDisplayInfo.Width = NOM_WIDTH_SIZE;
        m_SystemDisplayInfo.Height = NOM_HEIGHT_SIZE;
        m_SystemDisplayInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
        m_SystemDisplayInfo.Pitch = (BPPFromPixelFormat(m_SystemDisplayInfo.ColorFormat) / BITS_PER_BYTE) *
                                    m_SystemDisplayInfo.Width;
        m_SystemDisplayInfo.TargetId = 0;
        if (m_SystemDisplayInfo.PhysicAddress.QuadPart == 0LL)
        {
            m_SystemDisplayInfo.PhysicAddress = m_pAdapter->GetFrameBufferPA();
        }
    }

    m_CurrentModes[0].DispInfo.Width = max(MIN_WIDTH_SIZE, m_SystemDisplayInfo.Width);
    m_CurrentModes[0].DispInfo.Height = max(MIN_HEIGHT_SIZE, m_SystemDisplayInfo.Height);
    m_CurrentModes[0].DispInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
    m_CurrentModes[0].DispInfo.Pitch = (BPPFromPixelFormat(m_CurrentModes[0].DispInfo.ColorFormat) / BITS_PER_BYTE) *
                                       m_CurrentModes[0].DispInfo.Width;
    m_CurrentModes[0].DispInfo.TargetId = 0;
    if (m_CurrentModes[0].DispInfo.PhysicAddress.QuadPart == 0LL && m_SystemDisplayInfo.PhysicAddress.QuadPart != 0LL)
    {
        m_CurrentModes[0].DispInfo.PhysicAddress = m_SystemDisplayInfo.PhysicAddress;
    }

    *pNumberOfViews = MAX_VIEWS;
    *pNumberOfChildren = MAX_CHILDREN;

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s ColorFormat = %d\n", __FUNCTION__, m_CurrentModes[0].DispInfo.ColorFormat));

    InterlockedExchange(&m_sourceAddressQueueHead, 0);
    InterlockedExchange(&m_sourceAddressQueueTail, 0);
    InterlockedExchange(&m_sourceQueueFullCount, 0);

    // Losing the initial snapshot is not recoverable the way a later refresh is: there is no
    // previous generation for the DDIs to read, so do not pretend to have started.
    VIOGPU_MODE_SNAPSHOT *pProbe = AcquireModes();
    if (pProbe == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s: no mode snapshot available, failing start (0x%x)\n", __FUNCTION__, SnapshotStatus));
        StopVsyncTimer();
        return NT_SUCCESS(SnapshotStatus) ? STATUS_UNSUCCESSFUL : SnapshotStatus;
    }
    ReleaseModes(pProbe);

    StartVsyncTimer();
    return Status;
}

NTSTATUS VioGpuVidPN::Stop(void)
{
    PAGED_CODE();
    NTSTATUS Status = STATUS_SUCCESS;

    StopVsyncTimer();
    DestroyFrameBufferObj(TRUE);

    return Status;
}

NTSTATUS VioGpuVidPN::AcquirePostDisplayOwnership()
{
    NTSTATUS Status = m_pDxgkInterface->DxgkCbAcquirePostDisplayOwnership(m_pDxgkInterface->DeviceHandle,
                                                                          &m_SystemDisplayInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("DxgkCbAcquirePostDisplayOwnership failed with status 0x%X Width = %d\n",
                  Status,
                  m_SystemDisplayInfo.Width));
        VioGpuDbgBreak();
    }

    return Status;
}

void VioGpuVidPN::ReleasePostDisplayOwnership(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                              DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = FindSourceForTarget(TargetId, TRUE);

    BlackOutScreen(&m_CurrentModes[SourceId]);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("StopDeviceAndReleasePostDisplayOwnership Width = %d Height = %d Pitch = %d ColorFormat = %d\n",
              m_SystemDisplayInfo.Width,
              m_SystemDisplayInfo.Height,
              m_SystemDisplayInfo.Pitch,
              m_SystemDisplayInfo.ColorFormat));

    *pDisplayInfo = m_SystemDisplayInfo;
    pDisplayInfo->TargetId = TargetId;
    pDisplayInfo->AcpiId = m_CurrentModes[0].DispInfo.AcpiId;
}

void VioGpuVidPN::Powerdown()
{
    PAGED_CODE();

    StopVsyncTimer();
    DestroyFrameBufferObj(TRUE);
    m_CurrentModes[0].Flags.FrameBufferIsActive = FALSE;
    m_CurrentModes[0].FrameBuffer.Ptr = NULL;
}

NTSTATUS VioGpuVidPN::CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pCommitVidPn != NULL);
    VIOGPU_ASSERT(pCommitVidPn->AffectedVidPnSourceId < MAX_VIEWS);

    NTSTATUS Status;
    SIZE_T NumPaths = 0;
    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pPinnedVidPnSourceModeInfo = NULL;

    if (pCommitVidPn->Flags.PathPoweredOff)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = m_pDxgkInterface->DxgkCbQueryVidPnInterface(pCommitVidPn->hFunctionalVidPn,
                                                         DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                         &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnInterface->pfnGetTopology(pCommitVidPn->hFunctionalVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
        goto CommitVidPnExit;
    }

    Status = pVidPnTopologyInterface->pfnGetNumPaths(hVidPnTopology, &NumPaths);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPaths failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    if (NumPaths != 0)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pCommitVidPn->hFunctionalVidPn,
                                                          pCommitVidPn->AffectedVidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hFunctionalVidPn = 0x%llu, SourceId = "
                      "0x%I64x\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn),
                      pCommitVidPn->AffectedVidPnSourceId));
            goto CommitVidPnExit;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                      Status,
                      LONG_PTR(pCommitVidPn->hFunctionalVidPn)));
            goto CommitVidPnExit;
        }
    }
    else
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s no vidpn paths found", __FUNCTION__));
        pPinnedVidPnSourceModeInfo = NULL;
    }

    if (pPinnedVidPnSourceModeInfo == NULL)
    {
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = IsVidPnSourceModeFieldsValid(pPinnedVidPnSourceModeInfo);
    if (!NT_SUCCESS(Status))
    {
        goto CommitVidPnExit;
    }

    SIZE_T NumPathsFromSource = 0;
    Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology,
                                                               pCommitVidPn->AffectedVidPnSourceId,
                                                               &NumPathsFromSource);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetNumPathsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        goto CommitVidPnExit;
    }

    for (SIZE_T PathIndex = 0; PathIndex < NumPathsFromSource; ++PathIndex)
    {
        D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId = D3DDDI_ID_UNINITIALIZED;
        Status = pVidPnTopologyInterface->pfnEnumPathTargetsFromSource(hVidPnTopology,
                                                                       pCommitVidPn->AffectedVidPnSourceId,
                                                                       PathIndex,
                                                                       &TargetId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnEnumPathTargetsFromSource failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = "
                      "0x%I64x, PathIndex = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      PathIndex));
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnAcquirePathInfo(hVidPnTopology,
                                                             pCommitVidPn->AffectedVidPnSourceId,
                                                             TargetId,
                                                             &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, SourceId = 0x%I64x, "
                      "TargetId = 0x%I64x\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pCommitVidPn->AffectedVidPnSourceId,
                      TargetId));
            goto CommitVidPnExit;
        }

        Status = IsVidPnPathFieldsValid(pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = SetSourceModeAndPath(pPinnedVidPnSourceModeInfo, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopoogy = 0x%llu, pVidPnPresentPath = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPath));
            goto CommitVidPnExit;
        }
        pVidPnPresentPath = NULL;
    }

CommitVidPnExit:

    NTSTATUS TempStatus = STATUS_SUCCESS;

    if ((pVidPnSourceModeSetInterface != NULL) && (hVidPnSourceModeSet != 0) && (pPinnedVidPnSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnInterface != NULL) && (pCommitVidPn->hFunctionalVidPn != 0) && (hVidPnSourceModeSet != 0))
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pCommitVidPn->hFunctionalVidPn, hVidPnSourceModeSet);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTopologyInterface != NULL) && (hVidPnTopology != 0) && (pVidPnPresentPath != NULL))
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return Status;
}

NTSTATUS VioGpuVidPN::SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                           CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;

    CURRENT_MODE *pCurrentMode = &m_CurrentModes[pPath->VidPnSourceId];
    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s (%dx%d)\n",
              __FUNCTION__,
              pSourceMode->Format.Graphics.VisibleRegionSize.cx,
              pSourceMode->Format.Graphics.VisibleRegionSize.cy));
    pCurrentMode->Scaling = pPath->ContentTransformation.Scaling;
    pCurrentMode->SrcModeWidth = pSourceMode->Format.Graphics.VisibleRegionSize.cx;
    pCurrentMode->SrcModeHeight = pSourceMode->Format.Graphics.VisibleRegionSize.cy;
    pCurrentMode->Rotation = pPath->ContentTransformation.Rotation;

    pCurrentMode->DispInfo.Width = pSourceMode->Format.Graphics.PrimSurfSize.cx;
    pCurrentMode->DispInfo.Height = pSourceMode->Format.Graphics.PrimSurfSize.cy;
    pCurrentMode->DispInfo.Pitch = pSourceMode->Format.Graphics.PrimSurfSize.cx *
                                   BPPFromPixelFormat(pCurrentMode->DispInfo.ColorFormat) / BITS_PER_BYTE;

    if (NT_SUCCESS(Status))
    {
        VIOGPU_MODE_SNAPSHOT *pSnapshot = AcquireModes();
        if (pSnapshot == NULL)
        {
            return STATUS_UNSUCCESSFUL;
        }

        pCurrentMode->Flags.FullscreenPresent = TRUE;
        BOOLEAN Matched = FALSE;
        for (USHORT ModeIndex = 0; ModeIndex < pSnapshot->ModeCount; ++ModeIndex)
        {
            PVIDEO_MODE_INFORMATION pModeInfo = &pSnapshot->Modes[ModeIndex];
            if (pCurrentMode->DispInfo.Width == pModeInfo->VisScreenWidth &&
                pCurrentMode->DispInfo.Height == pModeInfo->VisScreenHeight)
            {
                // The preferred mode for the generation is chosen by the builder from the
                // host-requested size; do not overwrite it here with whatever dxgkrnl pinned.
                Status = SetCurrentMode(pSnapshot->ModeNumbers[ModeIndex], pCurrentMode, pSnapshot);
                Matched = TRUE;
                break;
            }
        }
        if (!Matched)
        {
            // dxgkrnl can pin a mode from an enumeration that a newer snapshot has since replaced.
            // Keep the vendor's "success" contract for a commit we cannot map (failing here would
            // make dxgkrnl re-negotiate the whole VidPN), but never let it pass silently.
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s: pinned %ux%u is not in snapshot generation %u (%u modes)\n",
                      __FUNCTION__,
                      pCurrentMode->DispInfo.Width,
                      pCurrentMode->DispInfo.Height,
                      pSnapshot->Generation,
                      pSnapshot->ModeCount));
        }
        ReleaseModes(pSnapshot);
    }

    return Status;
}

NTSTATUS VioGpuVidPN::IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pPath->VidPnSourceId >= MAX_VIEWS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnSourceId is 0x%I64x is too high (MAX_VIEWS is 0x%I64x)", pPath->VidPnSourceId, MAX_VIEWS));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    else if (pPath->VidPnTargetId >= MAX_CHILDREN)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("VidPnTargetId is 0x%I64x is too high (MAX_CHILDREN is 0x%I64x)",
                  pPath->VidPnTargetId,
                  MAX_CHILDREN));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    else if (pPath->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath contains a gamma ramp (0x%I64x)", pPath->GammaRamp.Type));
        return STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_CENTERED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a non-identity scaling (0x%I64x)", pPath->ContentTransformation.Scaling));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE90) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) &&
             (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pPath contains a not-supported rotation (0x%I64x)", pPath->ContentTransformation.Rotation));
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->VidPnTargetColorBasis != D3DKMDT_CB_SCRGB) &&
             (pPath->VidPnTargetColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pPath has a non-linear RGB color basis (0x%I64x)", pPath->VidPnTargetColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pSourceMode->Type != D3DKMDT_RMT_GRAPHICS)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pSourceMode is a non-graphics mode (0x%I64x)", pSourceMode->Type));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if ((pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_SCRGB) &&
             (pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a non-linear RGB color basis (0x%I64x)", pSourceMode->Format.Graphics.ColorBasis));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if (pSourceMode->Format.Graphics.PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pSourceMode has a palettized access mode (0x%I64x)",
                  pSourceMode->Format.Graphics.PixelValueAccessMode));
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        if (pSourceMode->Format.Graphics.PixelFormat == D3DDDIFMT_A8R8G8B8 ||
            pSourceMode->Format.Graphics.PixelFormat == D3DDDIFMT_A8B8G8R8)
        {
            return STATUS_SUCCESS;
        }
    }

    DbgPrint(TRACE_LEVEL_ERROR,
             ("pSourceMode has an unknown pixel format (0x%I64x)", pSourceMode->Format.Graphics.PixelFormat));

    return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
}

NTSTATUS
VioGpuVidPN::UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pUpdateActiveVidPnPresentPath != NULL);

    NTSTATUS Status = IsVidPnPathFieldsValid(&(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    m_CurrentModes[pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId].Flags.FullscreenPresent = TRUE;

    m_CurrentModes[pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId].Rotation = pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.ContentTransformation.Rotation;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

// Rebuild the EDID + mode tables from the host and publish them as a new generation.
// This is the ONLY place the EDID and mode tables are produced.  It blocks on the virtio control
// queue, so it must run on a PASSIVE_LEVEL path that is allowed to block: the config work thread
// (a display event) or Start().  Never from a dxgkrnl callback.
NTSTATUS VioGpuVidPN::RefreshModeSnapshot(DXGK_DISPLAY_INFORMATION *pDispInfo)
{
    PAGED_CODE();

    VIOGPU_MODE_SNAPSHOT *pNew = NULL;
    NTSTATUS Status = BuildModeSnapshot(&pNew);
    if (!NT_SUCCESS(Status))
    {
        // Deliberately keep the last known-good generation instead of publishing a partial one.
        DbgPrint(TRACE_LEVEL_ERROR, ("%s: build failed 0x%x, keeping previous snapshot\n", __FUNCTION__, Status));
        return Status;
    }

    if (pDispInfo != NULL)
    {
        pDispInfo->Height = max(pDispInfo->Height, MIN_HEIGHT_SIZE);
        pDispInfo->Width = max(pDispInfo->Width, MIN_WIDTH_SIZE);
        pDispInfo->ColorFormat = D3DDDIFMT_X8R8G8B8;
        pDispInfo->Pitch = (BPPFromPixelFormat(pDispInfo->ColorFormat) / BITS_PER_BYTE) * pDispInfo->Width;
    }

    VIOGPU_MODE_SNAPSHOT *pOld = NULL;
    ExAcquireFastMutex(&m_ModesLock);
    pOld = m_pModes;
    m_pModes = pNew;
    ExReleaseFastMutex(&m_ModesLock);

    // Drop the publication reference held on the previous generation.  Readers that already took
    // their own reference keep it alive; the last one out frees it.
    if (pOld != NULL && InterlockedDecrement(&pOld->Refs) == 0)
    {
        FreeSnapshot(pOld);
    }
    return STATUS_SUCCESS;
}

// Take a reference on the current snapshot.  The pointer lookup and the reference count are taken
// under the same lock, and the writer only drops its own reference after the swap, so a snapshot
// cannot be freed while it is still reachable through m_pModes.  No grace period is needed.
// FAST_MUTEX, not a KSPIN_LOCK: the snapshot is paged, and a spinlock would raise IRQL before the
// reference count is touched.
VIOGPU_MODE_SNAPSHOT *VioGpuVidPN::AcquireModes(void)
{
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    ExAcquireFastMutex(&m_ModesLock);
    VIOGPU_MODE_SNAPSHOT *pSnapshot = m_pModes;
    if (pSnapshot != NULL)
    {
        InterlockedIncrement(&pSnapshot->Refs);
    }
    ExReleaseFastMutex(&m_ModesLock);
    return pSnapshot;
}

void VioGpuVidPN::ReleaseModes(VIOGPU_MODE_SNAPSHOT *pSnapshot)
{
    if (pSnapshot != NULL && InterlockedDecrement(&pSnapshot->Refs) == 0)
    {
        FreeSnapshot(pSnapshot);
    }
}

void VioGpuVidPN::FreeSnapshot(VIOGPU_MODE_SNAPSHOT *pSnapshot)
{
    PAGED_CODE();
    if (pSnapshot == NULL)
    {
        return;
    }
    delete[] pSnapshot->Modes;
    delete[] pSnapshot->ModeNumbers;
    delete pSnapshot;
}

// Assemble a complete snapshot off to the side.  Nothing already published is touched, so a
// failure here leaves the previous generation intact and serviceable.
NTSTATUS VioGpuVidPN::BuildModeSnapshot(VIOGPU_MODE_SNAPSHOT **ppSnapshot)
{
    PAGED_CODE();

    *ppSnapshot = NULL;

    // Hard bound on enumerated modes, mirroring the 64 the vendor's global table had.  The EDID
    // parser cannot append past this, and one slot is kept aside for the host-requested size.
    const int kMaxModes = 64;
    int ModeCount = 0;

    VIOGPU_DISP_MODE *pModes = new (PagedPool) VIOGPU_DISP_MODE[kMaxModes];
    if (pModes == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(pModes, sizeof(VIOGPU_DISP_MODE) * kMaxModes);

    UCHAR Edids[MAX_CHILDREN][EDID_RAW_BLOCK_SIZE];
    RtlZeroMemory(Edids, sizeof(Edids));
    BOOLEAN HasEdid = FALSE;

    const UCHAR *pPrimaryEdid = NULL;
    if (virtio_is_feature_enabled(m_pAdapter->m_u64HostFeatures, VIRTIO_GPU_F_EDID))
    {
        GetEdids(Edids, MAX_CHILDREN, &HasEdid);
        // A fetch that did not succeed must fall back to the built-in table, exactly as the
        // vendor's GetEdidData() did (m_bEDID == FALSE -> g_gpu_edid).  Parsing the still-zeroed
        // Edids[0] instead would build the mode list from an all-zero EDID.
        pPrimaryEdid = HasEdid ? Edids[0] : g_gpu_edid;
    }
    else
    {
        // No EDID feature: parse the built-in table, with its usual correctness fixes applied.
        RtlCopyMemory(Edids[0], g_gpu_edid, EDID_V1_BLOCK_SIZE);
        FixEdid(Edids[0]);
        pPrimaryEdid = Edids[0];
    }

    ModeCount = AddEdidModes(pPrimaryEdid, pModes, kMaxModes);
    if (ModeCount < 0 || ModeCount >= kMaxModes)
    {
        delete[] pModes;
        DbgPrint(TRACE_LEVEL_ERROR, ("%s: implausible mode count %d\n", __FUNCTION__, ModeCount));
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_MODE_SNAPSHOT *pSnapshot = new (PagedPool) VIOGPU_MODE_SNAPSHOT;
    if (pSnapshot == NULL)
    {
        delete[] pModes;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(pSnapshot, sizeof(*pSnapshot));

    // One trailing entry carries the size the SPICE client is asking for.
    ULONG Total = (ULONG)ModeCount + 1;
    pSnapshot->Modes = new (PagedPool) VIDEO_MODE_INFORMATION[Total];
    pSnapshot->ModeNumbers = new (PagedPool) USHORT[Total];
    if (pSnapshot->Modes == NULL || pSnapshot->ModeNumbers == NULL)
    {
        delete[] pModes;
        FreeSnapshot(pSnapshot);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(pSnapshot->Modes, sizeof(VIDEO_MODE_INFORMATION) * Total);
    RtlZeroMemory(pSnapshot->ModeNumbers, sizeof(USHORT) * Total);

    // m_ModeGeneration is only ever touched by the single serialized snapshot writer: Start()
    // runs before the config work thread exists, and that thread is stopped before teardown.
    pSnapshot->Generation = ++m_ModeGeneration;
    pSnapshot->ModeCount = Total;
    pSnapshot->HasEdid = HasEdid;
    RtlCopyMemory(pSnapshot->Edids, Edids, sizeof(Edids));

    ULONG Preferred = 0;
    for (int idx = 0; idx < ModeCount; idx++)
    {
        pSnapshot->ModeNumbers[idx] = (USHORT)idx;
        SetVideoModeInfo(&pSnapshot->Modes[idx], (UINT)idx, &pModes[idx]);
        if (pModes[idx].XResolution == NOM_WIDTH_SIZE && pModes[idx].YResolution == NOM_HEIGHT_SIZE)
        {
            Preferred = (ULONG)idx;
        }
    }

    pSnapshot->CustomModeIndex = (ULONG)ModeCount;
    pSnapshot->ModeNumbers[ModeCount] = (USHORT)ModeCount;
    RtlCopyMemory(&pSnapshot->Modes[ModeCount], &pSnapshot->Modes[Preferred], sizeof(VIDEO_MODE_INFORMATION));

    // Fold in the size the host currently wants; this is what makes the desktop follow the window.
    GetDisplayInfo(pSnapshot);

    pSnapshot->CurrentModeIndex =
        (pSnapshot->CustomModeIndex < pSnapshot->ModeCount) ? pSnapshot->CustomModeIndex : Preferred;

    // Published snapshots start holding the writer's own reference.
    InterlockedExchange(&pSnapshot->Refs, 1);

    for (ULONG idx = 0; idx < pSnapshot->ModeCount; idx++)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("type %d, XRes = %d, YRes = %d\n",
                  pSnapshot->ModeNumbers[idx],
                  pSnapshot->Modes[idx].VisScreenWidth,
                  pSnapshot->Modes[idx].VisScreenHeight));
    }

    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("%s: generation %u, %u modes, preferred %u, custom %u, edid %u\n",
              __FUNCTION__,
              pSnapshot->Generation,
              pSnapshot->ModeCount,
              pSnapshot->CurrentModeIndex,
              pSnapshot->CustomModeIndex,
              pSnapshot->HasEdid));

    delete[] pModes;
    *ppSnapshot = pSnapshot;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode, VIOGPU_MODE_SNAPSHOT *pSnapshot)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: Mode = %d\n", __FUNCTION__, Mode));
    if (pSnapshot == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }
    for (ULONG idx = 0; idx < pSnapshot->ModeCount; idx++)
    {
        if (Mode == pSnapshot->ModeNumbers[idx])
        {
            if (pCurrentMode->Flags.FrameBufferIsActive)
            {
                DestroyFrameBufferObj(FALSE);
                pCurrentMode->Flags.FrameBufferIsActive = FALSE;
            }
            CreateFrameBufferObj(&pSnapshot->Modes[idx], pCurrentMode);
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s device: setting current mode %d (%d x %d)\n",
                      __FUNCTION__,
                      Mode,
                      pSnapshot->Modes[idx].VisScreenWidth,
                      pSnapshot->Modes[idx].VisScreenHeight));
            return STATUS_SUCCESS;
        }
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s failed\n", __FUNCTION__));
    return STATUS_UNSUCCESSFUL;
}

void VioGpuVidPN::CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode)
{
    UINT resid, format, size;
    VioGpuObj *obj;
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s : (%d x %d)\n", __FUNCTION__, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight));
    ASSERT(m_pFrameBuf == NULL);
    size = pModeInfo->ScreenStride * pModeInfo->VisScreenHeight;
    format = ColorFormat(pCurrentMode->DispInfo.ColorFormat);
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("---> %s - (%d -> %d)\n", __FUNCTION__, pCurrentMode->DispInfo.ColorFormat, format));
    resid = m_pAdapter->resourceIdr.GetId();
    m_pAdapter->ctrlQueue.CreateResource(resid, format, pModeInfo->VisScreenWidth, pModeInfo->VisScreenHeight);
    obj = new (NonPagedPoolNx) VioGpuObj();
    if (!obj->Init(size, &m_pAdapter->frameSegment))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s Failed to init obj size = %d\n", __FUNCTION__, size));
        delete obj;
        return;
    }

    GpuObjectAttach(resid, obj);
    // long* pvAddr = (long*)m_FrameSegment.GetVirtualAddress();
    // for (int i = 0; i < 0x8000 / 4; i += 1) {
    //     pvAddr[i] = 0x00ff8800;
    // };
    resid = 1;

    m_pFrameBuf = obj;
    pCurrentMode->FrameBuffer.Ptr = obj->GetVirtualAddress();
    pCurrentMode->Flags.FrameBufferIsActive = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void VioGpuVidPN::DestroyFrameBufferObj(BOOLEAN bReset)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UINT resid = 0;

    if (m_pFrameBuf != NULL)
    {
        resid = (UINT)m_pFrameBuf->GetId();
        m_pAdapter->ctrlQueue.DetachBacking(resid);
        m_pAdapter->ctrlQueue.DestroyResource(resid);
        if (bReset == TRUE)
        {
            m_pAdapter->ctrlQueue.SetScanout(0, 0, 0, 0, 0, 0);
        }
        delete m_pFrameBuf;
        m_pFrameBuf = NULL;
        m_pAdapter->resourceIdr.PutId(resid);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuVidPN::GpuObjectAttach(UINT res_id, VioGpuObj *obj)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_MEM_ENTRY ents = NULL;
    PSCATTER_GATHER_LIST sgl = NULL;
    UINT size = 0;
    sgl = obj->GetSGList();
    size = sizeof(GPU_MEM_ENTRY) * sgl->NumberOfElements;
    ents = reinterpret_cast<PGPU_MEM_ENTRY>(new (NonPagedPoolNx) BYTE[size]);

    if (!ents)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s cannot allocate memory %x bytes numberofentries = %d\n",
                  __FUNCTION__,
                  size,
                  sgl->NumberOfElements));
        return FALSE;
    }
    // FIXME
    RtlZeroMemory(ents, size);

    for (UINT i = 0; i < sgl->NumberOfElements; i++)
    {
        ents[i].addr = sgl->Elements[i].Address.QuadPart;
        ents[i].length = sgl->Elements[i].Length;
        ents[i].padding = 0;
    }

    m_pAdapter->ctrlQueue.AttachBacking(res_id, ents, sgl->NumberOfElements);
    obj->SetId(res_id);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

NTSTATUS VioGpuVidPN::EscapeCustomResoulution(VIOGPU_DISP_MODE *resolution)
{
    PAGED_CODE();

    VIOGPU_MODE_SNAPSHOT *pSnapshot = AcquireModes();
    if (pSnapshot == NULL || pSnapshot->CustomModeIndex >= pSnapshot->ModeCount)
    {
        ReleaseModes(pSnapshot);
        return STATUS_UNSUCCESSFUL;
    }

    resolution->XResolution = (USHORT)pSnapshot->Modes[pSnapshot->CustomModeIndex].VisScreenWidth;
    resolution->YResolution = (USHORT)pSnapshot->Modes[pSnapshot->CustomModeIndex].VisScreenHeight;

    ReleaseModes(pSnapshot);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pVidPnHWCaps != NULL);
    VIOGPU_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    VIOGPU_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

    pVidPnHWCaps->VidPnHWCaps.DriverRotation = 1;
    pVidPnHWCaps->VidPnHWCaps.DriverScaling = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverCloning = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert = 1;
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0;
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay = 0;
    pVidPnHWCaps->VidPnHWCaps.Reserved = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pIsSupportedVidPn != NULL);

    if (pIsSupportedVidPn->hDesiredVidPn == 0)
    {
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }

    pIsSupportedVidPn->IsVidPnSupported = FALSE;
    // DbgPrint(TRACE_LEVEL_ERROR, ("vidpn %d\n", pIsSupportedVidPn->hDesiredVidPn));
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface;
    NTSTATUS Status = m_pDxgkInterface->DxgkCbQueryVidPnInterface(pIsSupportedVidPn->hDesiredVidPn,
                                                                  DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                  &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface;
    Status = pVidPnInterface->pfnGetTopology(pIsSupportedVidPn->hDesiredVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hDesiredVidPn = %llu\n",
                  Status,
                  LONG_PTR(pIsSupportedVidPn->hDesiredVidPn)));
        return Status;
    }

    for (D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        SIZE_T NumPathsFromSource = 0;
        Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, SourceId, &NumPathsFromSource);
        if (Status == STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY)
        {
            continue;
        }
        else if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnGetNumPathsFromSource failed with Status = 0x%X hVidPnTopology = %llu, SourceId = %llu",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      LONG_PTR(SourceId)));
            return Status;
        }
        else if (NumPathsFromSource > MAX_CHILDREN)
        {
            return STATUS_SUCCESS;
        }
    }

    pIsSupportedVidPn->IsVidPnSupported = TRUE;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpuVidPN::RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pRecommendFunctionalVidPn == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuVidPN::RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pRecommendVidPnTopology == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS VioGpuVidPN::RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_MODE_SNAPSHOT *pSnapshot = AcquireModes();
    NTSTATUS Status = AddSingleMonitorMode(pSnapshot, pRecommendMonitorModes);
    ReleaseModes(pSnapshot);
    return Status;
}

NTSTATUS VioGpuVidPN::AddSingleSourceMode(VIOGPU_MODE_SNAPSHOT *pSnapshot,
                                          _In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                          D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                          D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(SourceId);

    if (pSnapshot == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    for (ULONG idx = 0; idx < pSnapshot->ModeCount; ++idx)
    {
        PVIDEO_MODE_INFORMATION pModeInfo = &pSnapshot->Modes[idx];
        // Expose both BGRA and RGBA source formats so DXGI callers asking
        // for DXGI_FORMAT_R8G8B8A8_UNORM can receive non-empty mode lists.
        D3DDDIFORMAT pixelFormats[] = {D3DDDIFMT_A8R8G8B8, D3DDDIFMT_A8B8G8R8};

        for (UINT fmtIdx = 0; fmtIdx < (sizeof(pixelFormats) / sizeof(pixelFormats[0])); ++fmtIdx)
        {
            D3DKMDT_VIDPN_SOURCE_MODE *pVidPnSourceModeInfo = NULL;
            NTSTATUS Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet,
                                                                                 &pVidPnSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = %llu",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet)));
                return Status;
            }

            pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
            pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = pModeInfo->VisScreenWidth;
            pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = pModeInfo->VisScreenHeight;
            pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
            pVidPnSourceModeInfo->Format.Graphics.Stride = pModeInfo->ScreenStride;
            pVidPnSourceModeInfo->Format.Graphics.PixelFormat = pixelFormats[fmtIdx];
            pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
            pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

            Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                NTSTATUS TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet,
                                                                                       pVidPnSourceModeInfo);
                UNREFERENCED_PARAMETER(TempStatus);
                NT_ASSERT(NT_SUCCESS(TempStatus));

                if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAddMode failed with Status = 0x%X, hVidPnSourceModeSet = %llu, pVidPnSourceModeInfo = %p",
                              Status,
                              LONG_PTR(hVidPnSourceModeSet),
                              pVidPnSourceModeInfo));
                    return Status;
                }
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

VOID VioGpuVidPN::BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo)
{
    PAGED_CODE();

    pVideoSignalInfo->VideoStandard = D3DKMDT_VSS_OTHER;
    pVideoSignalInfo->TotalSize.cx = pModeInfo->VisScreenWidth;
    pVideoSignalInfo->TotalSize.cy = pModeInfo->VisScreenHeight;
    pVideoSignalInfo->ActiveSize = pVideoSignalInfo->TotalSize;

#if 1
    pVideoSignalInfo->VSyncFreq.Numerator = 148500000;
    pVideoSignalInfo->VSyncFreq.Denominator = 2475000;
    pVideoSignalInfo->HSyncFreq.Numerator = 67500;
    pVideoSignalInfo->HSyncFreq.Denominator = 1;
    pVideoSignalInfo->PixelRate = 148500000;
#else

    pVideoSignalInfo->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVideoSignalInfo->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
#endif
    pVideoSignalInfo->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

NTSTATUS VioGpuVidPN::AddSingleTargetMode(VIOGPU_MODE_SNAPSHOT *pSnapshot,
                                          _In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                          D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                          _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                          D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    UNREFERENCED_PARAMETER(pVidPnPinnedSourceModeInfo);
    UNREFERENCED_PARAMETER(SourceId);

    if (pSnapshot == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    D3DKMDT_VIDPN_TARGET_MODE *pVidPnTargetModeInfo = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    for (UINT ModeIndex = 0; ModeIndex < pSnapshot->ModeCount; ++ModeIndex)
    {
        PVIDEO_MODE_INFORMATION pModeInfo = &pSnapshot->Modes[ModeIndex];
        pVidPnTargetModeInfo = NULL;
        Status = pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(hVidPnTargetModeSet, &pVidPnTargetModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = %llu",
                      Status,
                      LONG_PTR(hVidPnTargetModeSet)));
            return Status;
        }
        BuildVideoSignalInfo(&pVidPnTargetModeInfo->VideoSignalInfo, pModeInfo);

        pVidPnTargetModeInfo->Preference = D3DKMDT_MP_NOTPREFERRED; // TODO: another logic for prefferred mode. Maybe
                                                                    // the pinned source mode

        Status = pVidPnTargetModeSetInterface->pfnAddMode(hVidPnTargetModeSet, pVidPnTargetModeInfo);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, pVidPnTargetModeInfo = "
                          "%p\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet),
                          pVidPnTargetModeInfo));
            }

            Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnTargetModeInfo);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuVidPN::AddSingleMonitorMode(VIOGPU_MODE_SNAPSHOT *pSnapshot,
                                           _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    D3DKMDT_MONITOR_SOURCE_MODE *pMonitorSourceMode = NULL;
    PVIDEO_MODE_INFORMATION pVbeModeInfo = NULL;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pSnapshot == NULL || pSnapshot->CurrentModeIndex >= pSnapshot->ModeCount)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                          &pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                  Status,
                  LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
        return Status;
    }

    pVbeModeInfo = &pSnapshot->Modes[pSnapshot->CurrentModeIndex];

    BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

    pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
    pMonitorSourceMode->Preference = D3DKMDT_MP_PREFERRED;
    pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                      "0x%p\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                      pMonitorSourceMode));
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        NTSTATUS TempStatus = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                         pMonitorSourceMode);
        UNREFERENCED_PARAMETER(TempStatus);
        NT_ASSERT(NT_SUCCESS(TempStatus));
        return Status;
    }

    for (UINT Idx = 0; Idx < pSnapshot->ModeCount; ++Idx)
    {
        pVbeModeInfo = &pSnapshot->Modes[Idx];

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                              &pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnCreateNewModeInfo failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet)));
            return Status;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s: add pref mode, dimensions %ux%u, taken from DxgkCbAcquirePostDisplayOwnership at StartDevice\n",
                  __FUNCTION__,
                  pVbeModeInfo->VisScreenWidth,
                  pVbeModeInfo->VisScreenHeight));

        BuildVideoSignalInfo(&pMonitorSourceMode->VideoSignalInfo, pVbeModeInfo);

        pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
        pMonitorSourceMode->Preference = D3DKMDT_MP_NOTPREFERRED;
        pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
        pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;

        Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                    pMonitorSourceMode);
        if (!NT_SUCCESS(Status))
        {
            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAddMode failed with Status = 0x%X, hMonitorSourceModeSet = 0x%llu, pMonitorSourceMode = "
                          "0x%p\n",
                          Status,
                          LONG_PTR(pRecommendMonitorModes->hMonitorSourceModeSet),
                          pMonitorSourceMode));
            }

            Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet,
                                                                                                pMonitorSourceMode);
            NT_ASSERT(NT_SUCCESS(Status));
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuVidPN::EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();

    // One snapshot for the whole enumeration so every mode reported below comes from a single
    // generation, even if the config work thread publishes a new one mid-call.
    VIOGPU_MODE_SNAPSHOT *pSnapshot = AcquireModes();
    if (pSnapshot == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    NTSTATUS Status = EnumVidPnCofuncModalityInternal(pEnumCofuncModality, pSnapshot);
    ReleaseModes(pSnapshot);
    return Status;
}

NTSTATUS VioGpuVidPN::EnumVidPnCofuncModalityInternal(
    _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality, VIOGPU_MODE_SNAPSHOT *pSnapshot)
{
    PAGED_CODE();

    // DbgBreakPoint();

    VIOGPU_ASSERT(pEnumCofuncModality != NULL);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet = 0;
    D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE *pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *pVidPnPresentPathTemp = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo = NULL;
    CONST D3DKMDT_VIDPN_TARGET_MODE *pVidPnPinnedTargetModeInfo = NULL;

    NTSTATUS Status = m_pDxgkInterface->DxgkCbQueryVidPnInterface(pEnumCofuncModality->hConstrainingVidPn,
                                                                  DXGK_VIDPN_INTERFACE_VERSION_V1,
                                                                  &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("DxgkCbQueryVidPnInterface failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnInterface->pfnGetTopology(pEnumCofuncModality->hConstrainingVidPn,
                                             &hVidPnTopology,
                                             &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnGetTopology failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                  Status,
                  LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
        return Status;
    }

    Status = pVidPnTopologyInterface->pfnAcquireFirstPathInfo(hVidPnTopology, &pVidPnPresentPath);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("pfnAcquireFirstPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                  Status,
                  LONG_PTR(hVidPnTopology)));
        return Status;
    }

    while (Status != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                          pVidPnPresentPath->VidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId = "
                      "0x%llu\n",
                      Status,
                      LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                      LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
            break;
        }

        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet,
                                                                        &pVidPnPinnedSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu\n",
                      Status,
                      LONG_PTR(hVidPnSourceModeSet)));
            break;
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId)))
        {
            if (pVidPnPinnedSourceModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnSourceId,
                                                                    &hVidPnSourceModeSet,
                                                                    &pVidPnSourceModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "SourceId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId)));
                    break;
                }

                {
                    Status = AddSingleSourceMode(pSnapshot,
                                                 pVidPnSourceModeSetInterface,
                                                 hVidPnSourceModeSet,
                                                 pVidPnPresentPath->VidPnSourceId);
                }

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleSourceMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnSourceId,
                                                                 hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, SourceId "
                              "= 0x%llu, hVidPnSourceModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnSourceId),
                              LONG_PTR(hVidPnSourceModeSet)));
                    break;
                }
                hVidPnSourceModeSet = 0;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            Status = pVidPnInterface->pfnAcquireTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              pVidPnPresentPath->VidPnTargetId,
                                                              &hVidPnTargetModeSet,
                                                              &pVidPnTargetModeSetInterface);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquireTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId = "
                          "0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                break;
            }

            Status = pVidPnTargetModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnTargetModeSet,
                                                                            &pVidPnPinnedTargetModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnAcquirePinnedModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTargetModeSet)));
                break;
            }

            if (pVidPnPinnedTargetModeInfo == NULL)
            {
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;

                Status = pVidPnInterface->pfnCreateNewTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnTargetId,
                                                                    &hVidPnTargetModeSet,
                                                                    &pVidPnTargetModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnCreateNewTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "TargetId = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId)));
                    break;
                }

                Status = AddSingleTargetMode(pSnapshot,
                                             pVidPnTargetModeSetInterface,
                                             hVidPnTargetModeSet,
                                             pVidPnPinnedSourceModeInfo,
                                             pVidPnPresentPath->VidPnSourceId);

                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("AddSingleTargetMode failed with Status = 0x%X, hFunctionalVidPn = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn)));
                    break;
                }

                Status = pVidPnInterface->pfnAssignTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                 pVidPnPresentPath->VidPnTargetId,
                                                                 hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnAssignTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, TargetId "
                              "= 0x%llu, hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(pVidPnPresentPath->VidPnTargetId),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
            else
            {
                Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet,
                                                                          pVidPnPinnedTargetModeInfo);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnTargetModeSet = 0x%llu, "
                              "pVidPnPinnedTargetModeInfo = %p\n",
                              Status,
                              LONG_PTR(hVidPnTargetModeSet),
                              pVidPnPinnedTargetModeInfo));
                    break;
                }
                pVidPnPinnedTargetModeInfo = NULL;

                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                  hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pfnReleaseTargetModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                              "hVidPnTargetModeSet = 0x%llu\n",
                              Status,
                              LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                              LONG_PTR(hVidPnTargetModeSet)));
                    break;
                }
                hVidPnTargetModeSet = 0;
            }
        }

        if (pVidPnPinnedSourceModeInfo != NULL)
        {
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseModeInfo failed with Status = 0x%X, hVidPnSourceModeSet = 0x%llu, "
                          "pVidPnPinnedSourceModeInfo = %p\n",
                          Status,
                          LONG_PTR(hVidPnSourceModeSet),
                          pVidPnPinnedSourceModeInfo));
                break;
            }
            pVidPnPinnedSourceModeInfo = NULL;
        }

        if (hVidPnSourceModeSet != 0)
        {
            Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnReleaseSourceModeSet failed with Status = 0x%X, hConstrainingVidPn = 0x%llu, "
                          "hVidPnSourceModeSet = 0x%llu\n",
                          Status,
                          LONG_PTR(pEnumCofuncModality->hConstrainingVidPn),
                          LONG_PTR(hVidPnSourceModeSet)));
                break;
            }
            hVidPnSourceModeSet = 0;
        }

        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;
        BOOLEAN SupportFieldsModified = FALSE;

        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_SCALING) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            {
                RtlZeroMemory(&(LocalVidPnPresentPath.ContentTransformation.ScalingSupport),
                              sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT));
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Centered = 1;
                SupportFieldsModified = TRUE;
            }
        }

        if (!((pEnumCofuncModality->EnumPivotType != D3DKMDT_EPT_ROTATION) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            if (pVidPnPresentPath->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            {
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate180 = 0;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate270 = 0;
                SupportFieldsModified = TRUE;
            }
        }

        if (SupportFieldsModified)
        {
            Status = pVidPnTopologyInterface->pfnUpdatePathSupportInfo(hVidPnTopology, &LocalVidPnPresentPath);
            if (!NT_SUCCESS(Status))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("pfnUpdatePathSupportInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu\n",
                          Status,
                          LONG_PTR(hVidPnTopology)));
                break;
            }
        }

        pVidPnPresentPathTemp = pVidPnPresentPath;
        Status = pVidPnTopologyInterface->pfnAcquireNextPathInfo(hVidPnTopology,
                                                                 pVidPnPresentPathTemp,
                                                                 &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnAcquireNextPathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, "
                      "pVidPnPresentPathTemp = %p\n",
                      Status,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            break;
        }

        NTSTATUS TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        if (!NT_SUCCESS(TempStatus))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("pfnReleasePathInfo failed with Status = 0x%X, hVidPnTopology = 0x%llu, pVidPnPresentPathTemp = "
                      "%p\n",
                      TempStatus,
                      LONG_PTR(hVidPnTopology),
                      pVidPnPresentPathTemp));
            Status = TempStatus;
            break;
        }
        pVidPnPresentPathTemp = NULL;
    }

    if (Status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = STATUS_SUCCESS;
    }

    NTSTATUS TempStatus = STATUS_NOT_FOUND;

    if ((pVidPnSourceModeSetInterface != NULL) && (pVidPnPinnedSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTargetModeSetInterface != NULL) && (pVidPnPinnedTargetModeInfo != NULL))
    {
        TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPath != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPathTemp != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnSourceModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnSourceModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnTargetModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              hVidPnTargetModeSet);
        VIOGPU_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    VIOGPU_ASSERT_CHK(TempStatus == STATUS_NOT_FOUND || Status != STATUS_SUCCESS);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuVidPN::SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VIOGPU_ASSERT(pSetVidPnSourceVisibility != NULL);
    VIOGPU_ASSERT((pSetVidPnSourceVisibility->VidPnSourceId < MAX_VIEWS) ||
                  (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL));

    UINT StartVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? 0
                                                                                          : pSetVidPnSourceVisibility->VidPnSourceId;
    UINT MaxVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? MAX_VIEWS
                                                                                        : pSetVidPnSourceVisibility->VidPnSourceId + 1;

    for (UINT SourceId = StartVidPnSourceId; SourceId < MaxVidPnSourceId; ++SourceId)
    {
        if (pSetVidPnSourceVisibility->Visible)
        {
            m_CurrentModes[SourceId].Flags.FullscreenPresent = TRUE;
        }
        else
        {
            BlackOutScreen(&m_CurrentModes[SourceId]);
        }

        m_CurrentModes[SourceId].Flags.SourceNotVisible = !(pSetVidPnSourceVisibility->Visible);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

VOID VioGpuVidPN::BlackOutScreen(CURRENT_MODE *pCurrentMod)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));

    if (pCurrentMod->Flags.FrameBufferIsActive)
    {
        UINT ScreenHeight = pCurrentMod->DispInfo.Height;
        UINT ScreenPitch = pCurrentMod->DispInfo.Pitch;
        BYTE *pDst = (BYTE *)pCurrentMod->FrameBuffer.Ptr;

        UINT resid = 0;

        if (pDst)
        {
            RtlZeroMemory(pDst, (ULONGLONG)ScreenHeight * ScreenPitch);
        }

        // FIXME!!! rotation

        resid = m_pFrameBuf->GetId();

        m_pAdapter->ctrlQueue.SetScanout(0,
                                         resid,
                                         pCurrentMod->DispInfo.Width,
                                         pCurrentMod->DispInfo.Height,
                                         0,
                                         0);
        m_pAdapter->ctrlQueue.ResFlush(resid, pCurrentMod->DispInfo.Width, pCurrentMod->DispInfo.Height, 0, 0);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuVidPN::GetDisplayInfo(VIOGPU_MODE_SNAPSHOT *pSnapshot)
{
    PAGED_CODE();

    if (pSnapshot == NULL)
    {
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf = NULL;
    ULONG xres = 0;
    ULONG yres = 0;

    for (UINT32 i = 0; i < m_pAdapter->m_u32NumScanouts; i++)
    {
        if (m_pAdapter->ctrlQueue.AskDisplayInfo(&vbuf))
        {
            m_pAdapter->ctrlQueue.GetDisplayInfo(vbuf, i, &xres, &yres);
            m_pAdapter->ctrlQueue.ReleaseBuffer(vbuf);
            if (xres && yres)
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("---> %s (%dx%d)\n", __FUNCTION__, xres, yres));
                SetCustomDisplay(pSnapshot, (USHORT)xres, (USHORT)yres);
            }
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

// Append (xres,yres) to a caller-owned bounded table unless it is already there.
// `cnt` is the number of valid entries; it never exceeds Capacity.
BOOLEAN VioGpuVidPN::UpdateModes(VIOGPU_DISP_MODE *pModes, int Capacity, int &cnt, USHORT xres, USHORT yres)
{
    if (pModes == NULL || Capacity <= 0 || cnt < 0)
    {
        return FALSE;
    }
    if (xres == 0 || yres == 0)
    {
        return FALSE;
    }
    if ((xres < MIN_WIDTH_SIZE) || (yres < MIN_HEIGHT_SIZE))
    {
        return FALSE;
    }

    for (int idx = 0; idx < cnt; idx++)
    {
        if ((pModes[idx].XResolution == xres) && (pModes[idx].YResolution == yres))
        {
            return FALSE;
        }
    }

    if (cnt >= Capacity)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s: mode table full at %d entries, dropping %ux%u\n", __FUNCTION__, Capacity, xres, yres));
        return FALSE;
    }

    pModes[cnt].XResolution = xres;
    pModes[cnt].YResolution = yres;
    cnt++;
    return TRUE;
}

void VioGpuVidPN::FixEdid(UCHAR *pEdid)
{
    PAGED_CODE();

    if (pEdid == NULL)
    {
        return;
    }

    UCHAR Sum = 0;
    PUCHAR buf = pEdid;
    PEDID_DATA_V1 pdata = (PEDID_DATA_V1)buf;
    pdata->MaximumHorizontalImageSize[0] = 0;
    pdata->MaximumVerticallImageSize[0] = 0;
    pdata->ExtensionFlag[0] = 0;
    pdata->Checksum[0] = 0;
    for (ULONG i = 0; i < EDID_V1_BLOCK_SIZE; i++)
    {
        Sum += buf[i];
    }
    pdata->Checksum[0] = -Sum;
}

PBYTE VioGpuVidPN::GetCTA861Data(const UCHAR *pEdid)
{
    PAGED_CODE();
    if (pEdid != NULL)
    {
        PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)pEdid;
        if (edid_data->ExtensionFlag[0] > 0)
        {
            PEDID_CTA_861 cta_data = (PEDID_CTA_861)(pEdid + EDID_V1_BLOCK_SIZE);
            if (cta_data->ExtentionTag[0] >= 2 && cta_data->Revision[0] >= 3)
            {
                return (PBYTE)cta_data;
            }
        }
    }
    return NULL;
}

// Fetch each scanout's EDID into a caller-owned array of EdidCapacity blocks.  The host-supplied
// scanout count is NOT trusted as a bound: it comes from device config, so the loop is clamped to
// the caller's capacity rather than overrunning the caller's storage.
BOOLEAN VioGpuVidPN::GetEdids(UCHAR (*Edids)[EDID_RAW_BLOCK_SIZE], UINT32 EdidCapacity, BOOLEAN *pHasEdid)
{
    if (Edids == NULL || pHasEdid == NULL || EdidCapacity == 0)
    {
        return FALSE;
    }
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (m_pAdapter->m_u32NumScanouts > EdidCapacity)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s: host reports %u scanouts but only %u EDID slots exist; ignoring the rest\n",
                  __FUNCTION__,
                  m_pAdapter->m_u32NumScanouts,
                  EdidCapacity));
    }

    UINT32 count = (m_pAdapter->m_u32NumScanouts < EdidCapacity) ? m_pAdapter->m_u32NumScanouts : EdidCapacity;
    PGPU_VBUFFER vbuf = NULL;

    for (UINT32 i = 0; i < count; i++)
    {
        vbuf = NULL;
        if (m_pAdapter->ctrlQueue.AskEdidInfo(&vbuf, i) && m_pAdapter->ctrlQueue.GetEdidInfo(vbuf, i, Edids[i]))
        {
            *pHasEdid = TRUE;
        }
        if (vbuf)
        {
            m_pAdapter->ctrlQueue.ReleaseBuffer(vbuf);
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return TRUE;
}

int VioGpuVidPN::AddEdidModes(const UCHAR *Edid, VIOGPU_DISP_MODE *pModes, int Capacity)
{
    PAGED_CODE();

    if (Edid == NULL || pModes == NULL || Capacity <= 0)
    {
        return -1;
    }

    PEDID_DATA_V1 edid_data = (PEDID_DATA_V1)Edid;
    ESTABLISHED_TIMINGS_1_2 est_timing_1_2 = edid_data->EstablishedTimings;
    MANUFACTURER_TIMINGS manufact_timing = edid_data->ManufacturerTimings;
    int modecount = 0;

    UpdateModes(pModes, Capacity, modecount, MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE);
    UpdateModes(pModes, Capacity, modecount, NOM_WIDTH_SIZE, NOM_HEIGHT_SIZE);
    if (est_timing_1_2.Timing_800x600_60 || est_timing_1_2.Timing_800x600_56 || est_timing_1_2.Timing_800x600_75 ||
        est_timing_1_2.Timing_800x600_72)
    {
        UpdateModes(pModes, Capacity, modecount, 800, 600);
    }
    if (est_timing_1_2.Timing_720x400_88 || est_timing_1_2.Timing_720x400_70)
    {
        UpdateModes(pModes, Capacity, modecount, 720, 400);
    }
    if (est_timing_1_2.Timing_832x624_75)
    {
        UpdateModes(pModes, Capacity, modecount, 832, 624);
    }
    if (est_timing_1_2.Timing_1280x1024_75)
    {
        UpdateModes(pModes, Capacity, modecount, 1280, 1024);
    }
    if (manufact_timing.Timing_1152x870_75)
    {
        UpdateModes(pModes, Capacity, modecount, 1152, 870);
    }
    PSTANDARD_TIMING_DESCRIPTOR standard_timing = edid_data->StandardTimings;
    for (int i = 0; i < 8; i++, standard_timing++)
    {
        VIOGPU_DISP_MODE mode{0};
        if (GetStandardTimingResolution(standard_timing, &mode))
        {
            UpdateModes(pModes, Capacity, modecount, mode.XResolution, mode.YResolution);
        }
    }
    if (edid_data->Revision[0] == 4)
    {
        PEDID_DETAILED_DESCRIPTOR detailed_desc = edid_data->EDIDDetailedTimings;
        for (int i = 0; i < 4; i++, detailed_desc++)
        {
            if (detailed_desc->PixelClock == 0)
            {
                PEDID_DISPLAY_DESCRIPTOR disp = (PEDID_DISPLAY_DESCRIPTOR)detailed_desc;
                if (disp->Tag == 0xF7 && disp->Reserved1 == 0)
                {
                    PESTABLISHED_TIMINGS_3 est_timing_3 = (PESTABLISHED_TIMINGS_3)disp->Data;
                    if (est_timing_3->Timing_640x350_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 640, 350);
                    }
                    if (est_timing_3->Timing_640x400_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 640, 400);
                    }
                    if (est_timing_3->Timing_640x480_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 640, 480);
                    }
                    if (est_timing_3->Timing_720x400_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 720, 400);
                    }
                    if (est_timing_3->Timing_800x600_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 800, 600);
                    }
                    if (est_timing_3->Timing_848x480_60)
                    {
                        UpdateModes(pModes, Capacity, modecount, 848, 480);
                    }
                    if (est_timing_3->Timing_1024x768_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1024, 768);
                    }
                    if (est_timing_3->Timing_1152x864_75)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1152, 864);
                    }
                    if (est_timing_3->Timing_1280x768_60 || est_timing_3->Timing_1280x768_60_RB ||
                        est_timing_3->Timing_1280x768_75 || est_timing_3->Timing_1280x768_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1280, 768);
                    }
                    if (est_timing_3->Timing_1280x960_60 || est_timing_3->Timing_1280x960_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1280, 960);
                    }
                    if (est_timing_3->Timing_1280x1024_60 || est_timing_3->Timing_1280x1024_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1280, 1024);
                    }
                    if (est_timing_3->Timing_1360x768_60)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1360, 768);
                    }
                    if (est_timing_3->Timing_1400x1050_60 || est_timing_3->Timing_1400x1050_60_RB ||
                        est_timing_3->Timing_1400x1050_75 || est_timing_3->Timing_1400x1050_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1400, 1050);
                    }
                    if (est_timing_3->Timing_1440x900_60 || est_timing_3->Timing_1440x900_60_RB ||
                        est_timing_3->Timing_1440x900_75 || est_timing_3->Timing_1440x900_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1440, 900);
                    }
                    if (est_timing_3->Timing_1600x1200_60 || est_timing_3->Timing_1600x1200_65 ||
                        est_timing_3->Timing_1600x1200_70 || est_timing_3->Timing_1600x1200_75 ||
                        est_timing_3->Timing_1600x1200_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1600, 1200);
                    }
                    if (est_timing_3->Timing_1680x1050_60 || est_timing_3->Timing_1680x1050_60_RB ||
                        est_timing_3->Timing_1680x1050_75 || est_timing_3->Timing_1680x1050_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1680, 1050);
                    }
                    if (est_timing_3->Timing_1792x1344_60 || est_timing_3->Timing_1792x1344_75)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1792, 1344);
                    }
                    if (est_timing_3->Timing_1856x1392_60 || est_timing_3->Timing_1856x1392_75)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1856, 1392);
                    }
                    if (est_timing_3->Timing_1920x1200_60 || est_timing_3->Timing_1920x1200_60_RB ||
                        est_timing_3->Timing_1920x1200_75 || est_timing_3->Timing_1920x1200_85)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1920, 1200);
                    }
                    if (est_timing_3->Timing_1920x1440_60 || est_timing_3->Timing_1920x1440_75)
                    {
                        UpdateModes(pModes, Capacity, modecount, 1920, 1440);
                    }
                }
            }
        }
    }
    PEDID_CTA_861 cta_data = (PEDID_CTA_861)GetCTA861Data(Edid);
    if (cta_data && cta_data->DTDBegin[0] > 4)
    {
        int vics = (cta_data->DTDBegin[0] - 1) - 4;
        for (int idx = 0; idx < vics; idx++)
        {
            VIOGPU_DISP_MODE mode{0};
            USHORT vic_num = cta_data->Data[idx];
            if (GetVICResolution(vic_num, &mode))
            {
                UpdateModes(pModes, Capacity, modecount, mode.XResolution, mode.YResolution);
            }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return modecount;
}

void VioGpuVidPN::SetVideoModeInfo(PVIDEO_MODE_INFORMATION pMode, UINT Idx, PVIOGPU_DISP_MODE pModeInfo)
{
    PAGED_CODE();

    if (pMode == NULL || pModeInfo == NULL)
    {
        return;
    }
    pMode->Length = sizeof(VIDEO_MODE_INFORMATION);
    pMode->ModeIndex = Idx;
    pMode->VisScreenWidth = pModeInfo->XResolution;
    pMode->VisScreenHeight = pModeInfo->YResolution;
    pMode->ScreenStride = (pModeInfo->XResolution * 4 + 3) & ~0x3;
}

// Record the size the SPICE client currently wants into the trailing table entry of the snapshot
// being built.  A zero dimension means "no scanout", which must never become a 0x0 mode.
void VioGpuVidPN::SetCustomDisplay(VIOGPU_MODE_SNAPSHOT *pSnapshot, _In_ USHORT xres, _In_ USHORT yres)
{
    PAGED_CODE();

    if (pSnapshot == NULL || pSnapshot->CustomModeIndex >= pSnapshot->ModeCount || xres == 0 || yres == 0)
    {
        return;
    }

    VIOGPU_DISP_MODE tmpModeInfo = {0};

    if (xres < MIN_WIDTH_SIZE || yres < MIN_HEIGHT_SIZE)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s: (%dx%d) less than (%dx%d)\n", __FUNCTION__, xres, yres, MIN_WIDTH_SIZE, MIN_HEIGHT_SIZE));
    }
    tmpModeInfo.XResolution = m_pAdapter->IsFlexResolution() ? xres : max(MIN_WIDTH_SIZE, xres);
    tmpModeInfo.YResolution = m_pAdapter->IsFlexResolution() ? yres : max(MIN_HEIGHT_SIZE, yres);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s - %d (%dx%d)\n",
              __FUNCTION__,
              pSnapshot->CustomModeIndex,
              tmpModeInfo.XResolution,
              tmpModeInfo.YResolution));

    SetVideoModeInfo(&pSnapshot->Modes[pSnapshot->CustomModeIndex], pSnapshot->CustomModeIndex, &tmpModeInfo);
}

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

VOID VioGpuVidPN::StartVsyncTimer(void)
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (!m_timerRes)
    {
        m_timerRes = ExSetTimerResolution(10000, TRUE);
    }

    LARGE_INTEGER now;
    LARGE_INTEGER due;
    KeQuerySystemTime(&now);
    next_vsync_time.QuadPart = now.QuadPart + kVsyncPeriod100ns;
    due.QuadPart = -kVsyncPeriod100ns;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_vsyncTimerLock, &oldIrql);
    InterlockedExchange(&m_shouldFlipStop, 0);
    KeSetTimerEx(&m_vsyncNotifyTimer, due, 0, &m_vsyncNotifyDpc);
    KeReleaseSpinLock(&m_vsyncTimerLock, oldIrql);
}

VOID VioGpuVidPN::StopVsyncTimer(void)
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_vsyncTimerLock, &oldIrql);
    InterlockedExchange(&m_shouldFlipStop, 1);
    KeCancelTimer(&m_vsyncNotifyTimer);
    KeReleaseSpinLock(&m_vsyncTimerLock, oldIrql);

    KeFlushQueuedDpcs();

    if (m_timerRes)
    {
        ExSetTimerResolution(m_timerRes, FALSE);
        m_timerRes = 0;
    }
}

void VioGpuVidPN::Flip()
{
    if (InterlockedExchange(&m_shouldFlip, 0))
    {
        if (m_sourceRes != NULL)
        {
            m_sourceRes->FlushToScreen(0);
        }
        else
        {
            m_pAdapter->ctrlQueue.SetScanout(0, 0, 0, 0, 0, 0);
        }
    }
}

BOOLEAN VioGpuVidPN::QueueSourceAddress(PHYSICAL_ADDRESS address)
{
    LONG head = InterlockedCompareExchange(&m_sourceAddressQueueHead, 0, 0);
    LONG tail = InterlockedCompareExchange(&m_sourceAddressQueueTail, 0, 0);
    if ((tail - head) >= kSourceAddressQueueSize)
    {
        return FALSE;
    }

    LONG slot = tail & (kSourceAddressQueueSize - 1);
    m_sourceAddressQueue[slot] = address;
    m_lastQueuedSourceAddress = address;
    KeMemoryBarrier();

    InterlockedExchange(&m_sourceAddressQueueTail, tail + 1);

    return TRUE;
}

BOOLEAN VioGpuVidPN::DequeueSourceAddress(PHYSICAL_ADDRESS *address)
{
    LONG head = InterlockedCompareExchange(&m_sourceAddressQueueHead, 0, 0);
    LONG tail = InterlockedCompareExchange(&m_sourceAddressQueueTail, 0, 0);
    if (head == tail)
    {
        *address = m_sourceAddress;
        return TRUE;
    }

    LONG slot = head & (kSourceAddressQueueSize-1);
    *address = m_sourceAddressQueue[slot];
    KeMemoryBarrier();
    InterlockedCompareExchange(&m_sourceAddressQueueHead, head + 1, head);
    return TRUE;
}

void VioGpuVidPN::VsyncNotifyTimerDpc(KDPC *dpc, PVOID deferredContext, PVOID systemArg1, PVOID systemArg2)
{
    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(systemArg1);
    UNREFERENCED_PARAMETER(systemArg2);

    VioGpuVidPN *vidpn = reinterpret_cast<VioGpuVidPN *>(deferredContext);
    if (!vidpn || InterlockedCompareExchange(&vidpn->m_shouldFlipStop, 0, 0) != 0)
    {
        return;
    }

    LARGE_INTEGER now;
    LARGE_INTEGER next;
    KeQuerySystemTime(&now);

    next.QuadPart = vidpn->next_vsync_time.QuadPart + kVsyncPeriod100ns;
    if (next.QuadPart < now.QuadPart)
    {
        next.QuadPart = now.QuadPart + kVsyncPeriod100ns;
    }
    else if (next.QuadPart > (now.QuadPart + kVsyncMaxLead100ns))
    {
        next.QuadPart = now.QuadPart + kVsyncPeriod100ns;
    }
    vidpn->next_vsync_time = next;

    LARGE_INTEGER due;
    due.QuadPart = next.QuadPart - now.QuadPart;
    if (due.QuadPart <= 0 || due.QuadPart > kVsyncMaxLead100ns)
    {
        due.QuadPart = kVsyncPeriod100ns;
    }
    due.QuadPart = -due.QuadPart;

    vidpn->Flip();
    InterlockedExchange(&vidpn->m_vsync, 1);
    // submit a NOP to trigger the ISR generate CRTC_VSYNC
    vidpn->m_pAdapter->ctrlQueue.SubmitNop(NULL, NULL);

    KeAcquireSpinLockAtDpcLevel(&vidpn->m_vsyncTimerLock);
    if (InterlockedCompareExchange(&vidpn->m_shouldFlipStop, 0, 0) == 0)
    {
        KeSetTimerEx(&vidpn->m_vsyncNotifyTimer, due, 0, &vidpn->m_vsyncNotifyDpc);
    }
    KeReleaseSpinLockFromDpcLevel(&vidpn->m_vsyncTimerLock);
}


NTSTATUS VioGpuVidPN::SetVidPnSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress)
{
    m_sourceAddress = pSetVidPnSourceAddress->PrimaryAddress;
    m_sourceRes = reinterpret_cast<VioGpuAllocation *>(pSetVidPnSourceAddress->hAllocation);
    if (!QueueSourceAddress(m_sourceAddress))
    {
        LONG fullCount = InterlockedIncrement(&m_sourceQueueFullCount);
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s source queue full[%ld] source=0x%llx alloc=%p last_queued=0x%llx\n",
                  __FUNCTION__,
                  fullCount,
                  m_sourceAddress.QuadPart,
                  m_sourceRes,
                  m_lastQueuedSourceAddress.QuadPart));
    }
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s source=0x%llx alloc=%p\n",
              __FUNCTION__,
              m_sourceAddress.QuadPart,
              m_sourceRes));
    InterlockedOr(&m_shouldFlip, 1);

    return STATUS_SUCCESS;
};

D3DDDI_VIDEO_PRESENT_SOURCE_ID VioGpuVidPN::FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                BOOLEAN DefaultToZero)
{
    UNREFERENCED_PARAMETER(TargetId);
    for (UINT SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        if (m_CurrentModes[SourceId].FrameBuffer.Ptr != NULL)
        {
            return SourceId;
        }
    }

    return DefaultToZero ? 0 : D3DDDI_ID_UNINITIALIZED;
}

NTSTATUS VioGpuVidPN::SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                          _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                          _Out_ UINT *pWidth,
                                          _Out_ UINT *pHeight,
                                          _Out_ D3DDDIFORMAT *pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;

    VIOGPU_ASSERT((TargetId < MAX_CHILDREN) || (TargetId == D3DDDI_ID_UNINITIALIZED));

    if (TargetId == D3DDDI_ID_UNINITIALIZED)
    {
        for (UINT SourceIdx = 0; SourceIdx < MAX_VIEWS; ++SourceIdx)
        {
            if (m_CurrentModes[SourceIdx].FrameBuffer.Ptr != NULL)
            {
                m_SystemDisplaySourceId = SourceIdx;
                break;
            }
        }
    }
    else
    {
        m_SystemDisplaySourceId = FindSourceForTarget(TargetId, FALSE);
    }

    if (m_SystemDisplaySourceId == D3DDDI_ID_UNINITIALIZED)
    {
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    if ((m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE90) ||
        (m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE270))
    {
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }
    else
    {
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }

    *pColorFormat = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat;
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s ColorFormat = %d\n",
              __FUNCTION__,
              m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat));

    return STATUS_SUCCESS;
}

VOID VioGpuVidPN::SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                                     _In_ UINT SourceWidth,
                                     _In_ UINT SourceHeight,
                                     _In_ UINT SourceStride,
                                     _In_ INT PositionX,
                                     _In_ INT PositionY)
{
    UNREFERENCED_PARAMETER(pSource);
    UNREFERENCED_PARAMETER(SourceStride);

    RECT Rect;
    Rect.left = PositionX;
    Rect.top = PositionY;
    Rect.right = Rect.left + SourceWidth;
    Rect.bottom = Rect.top + SourceHeight;

    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = m_CurrentModes[m_SystemDisplaySourceId].FrameBuffer.Ptr;
    DstBltInfo.Pitch = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Pitch;
    DstBltInfo.BitsPerPel = BPPFromPixelFormat(m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat);
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = m_CurrentModes[m_SystemDisplaySourceId].Rotation;
    DstBltInfo.Width = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
    DstBltInfo.Height = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;

    BLT_INFO SrcBltInfo;
    SrcBltInfo.pBits = pSource;
    SrcBltInfo.Pitch = SourceStride;
    SrcBltInfo.BitsPerPel = 32;

    SrcBltInfo.Offset.x = -PositionX;
    SrcBltInfo.Offset.y = -PositionY;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    SrcBltInfo.Width = SourceWidth;
    SrcBltInfo.Height = SourceHeight;

    BltBits(&DstBltInfo, &SrcBltInfo, &Rect);
}

#pragma code_seg(pop) // End Non-Paged Code
