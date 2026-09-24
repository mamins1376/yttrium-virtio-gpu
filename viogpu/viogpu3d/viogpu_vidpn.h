/*
 * Copyright 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 * 
 */

#pragma once

#include "helper.h"
#include "viogpu.h"

class VioGpuAdapter;
class VioGpuAllocation;
class VioGpuObj;

typedef struct _CURRENT_MODE
{
    DXGK_DISPLAY_INFORMATION DispInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
    UINT SrcModeWidth;
    UINT SrcModeHeight;
    struct _CURRENT_MODE_FLAGS
    {
        UINT SourceNotVisible : 1;
        UINT FullscreenPresent : 1;
        UINT FrameBufferIsActive : 1;
        UINT DoNotMapOrUnmap : 1;
        UINT IsInternal : 1;
        UINT Unused : 27;
    } Flags;

    PHYSICAL_ADDRESS ZeroedOutStart;
    PHYSICAL_ADDRESS ZeroedOutEnd;

    union {
        VOID *Ptr;
        ULONG64 Force8Bytes;
    } FrameBuffer;
} CURRENT_MODE;

// An immutable, reference-counted snapshot of everything the mode-reporting and monitor-descriptor
// paths read: the display EDIDs, the mode table derived from them, and the indices this driver
// prefers.  It is built entirely off to the side by the config work thread and published by a
// pointer swap, so no dxgkrnl callback ever has to talk to the virtio control queue, and nothing
// the callbacks read can be mutated underneath them.  See VioGpuVidPN::AcquireModes().
//
// Nothing here may be modified after publication, not even the indices: they describe this
// generation's preferred mode, which is exactly the host-requested size.
typedef struct _VIOGPU_MODE_SNAPSHOT
{
    volatile LONG Refs; // publication reference + one per concurrent reader
    ULONG Generation;
    ULONG ModeCount;
    ULONG CurrentModeIndex; // preferred mode for this generation (the host-requested size)
    ULONG CustomModeIndex;  // the trailing entry carrying the host-requested size
    BOOLEAN HasEdid;        // FALSE => Edids is unused and g_gpu_edid is reported instead
    VIDEO_MODE_INFORMATION *Modes;
    USHORT *ModeNumbers;
    UCHAR Edids[MAX_CHILDREN][EDID_RAW_BLOCK_SIZE];
} VIOGPU_MODE_SNAPSHOT;

class VioGpuVidPN
{
  public:
    VioGpuVidPN(VioGpuAdapter *adapter);
    ~VioGpuVidPN();

    NTSTATUS Start(ULONG *pNumberOfViews, ULONG *pNumberOfChildren);
    VOID StartVsyncTimer(void);
    VOID StopVsyncTimer(void);
    NTSTATUS Stop(void);
    NTSTATUS AcquirePostDisplayOwnership();
    void ReleasePostDisplayOwnership(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, DXGK_DISPLAY_INFORMATION *pDisplayInfo);
    void Powerdown();

    NTSTATUS IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const;
    NTSTATUS IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const;

    NTSTATUS CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn);
    NTSTATUS
    UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath);

    NTSTATUS SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode, VIOGPU_MODE_SNAPSHOT *pSnapshot);
    VOID BlackOutScreen(CURRENT_MODE *pCurrentMod);

    // Rebuild the EDID + mode tables from the host and publish them as a new snapshot.  May block
    // on the virtio control queue and must only be called from PASSIVE_LEVEL, off the dxgkrnl
    // callback paths: the config work thread (a display event) and Start().
    NTSTATUS RefreshModeSnapshot(DXGK_DISPLAY_INFORMATION *pDispInfo);
    // Take a reference on the current snapshot.  NULL if none is published yet.
    VIOGPU_MODE_SNAPSHOT *AcquireModes(void);
    void ReleaseModes(VIOGPU_MODE_SNAPSHOT *pSnapshot);

    void CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode);
    void DestroyFrameBufferObj(BOOLEAN bReset);

    BOOLEAN GpuObjectAttach(UINT res_id, VioGpuObj *obj);

    PBYTE GetCTA861Data(const UCHAR *pEdid);
    void SetVideoModeInfo(PVIDEO_MODE_INFORMATION pMode, UINT Idx, PVIOGPU_DISP_MODE pModeInfo);
    BOOLEAN GetDisplayInfo(VIOGPU_MODE_SNAPSHOT *pSnapshot);
    void FixEdid(UCHAR *pEdid);
    BOOLEAN GetEdids(UCHAR (*Edids)[EDID_RAW_BLOCK_SIZE], BOOLEAN *pHasEdid);
    int AddEdidModes(const UCHAR *Edid, VIOGPU_DISP_MODE *pModes, int Capacity);
    BOOLEAN UpdateModes(VIOGPU_DISP_MODE *pModes, int Capacity, int &cnt, USHORT xres, USHORT yres);
    void SetCustomDisplay(VIOGPU_MODE_SNAPSHOT *pSnapshot, USHORT xres, USHORT yres);

    NTSTATUS EscapeCustomResoulution(VIOGPU_DISP_MODE *resolution);

    NTSTATUS IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn);
    NTSTATUS RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn);
    NTSTATUS RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology);
    NTSTATUS RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality);
    NTSTATUS SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility);
    NTSTATUS QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps);

    NTSTATUS SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                 _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                 _Out_ UINT *pWidth,
                                 _Out_ UINT *pHeight,
                                 _Out_ D3DDDIFORMAT *pColorFormat);
    VOID SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                            _In_ UINT SourceWidth,
                            _In_ UINT SourceHeight,
                            _In_ UINT SourceStride,
                            _In_ INT PositionX,
                            _In_ INT PositionY);

    void Flip();
    static void VsyncNotifyTimerDpc(KDPC *dpc, PVOID deferredContext, PVOID systemArg1, PVOID systemArg2);
    PHYSICAL_ADDRESS GetCurrentSourceAddress() const
    {
        return m_sourceAddress;
    }
    BOOLEAN QueueSourceAddress(PHYSICAL_ADDRESS address);
    BOOLEAN DequeueSourceAddress(PHYSICAL_ADDRESS *address);
    
    volatile LONG m_vsync = 0;

    NTSTATUS SetVidPnSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress);

  private:
    NTSTATUS SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                  CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath);
    NTSTATUS AddSingleMonitorMode(VIOGPU_MODE_SNAPSHOT *pSnapshot,
                                  _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS AddSingleSourceMode(VIOGPU_MODE_SNAPSHOT *pSnapshot,
                                 _In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS AddSingleTargetMode(VIOGPU_MODE_SNAPSHOT *pSnapshot,
                                 _In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                 D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    // Same body as the public EnumVidPnCofuncModality, but holding one acquired snapshot so that
    // every mode it reports comes from a single generation.
    NTSTATUS EnumVidPnCofuncModalityInternal(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality,
                                              VIOGPU_MODE_SNAPSHOT *pSnapshot);
    D3DDDI_VIDEO_PRESENT_SOURCE_ID FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero);
    VOID BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo);
    NTSTATUS BuildModeSnapshot(VIOGPU_MODE_SNAPSHOT **ppSnapshot);
    void FreeSnapshot(VIOGPU_MODE_SNAPSHOT *pSnapshot);

    VioGpuAdapter *m_pAdapter;
    DXGKRNL_INTERFACE *m_pDxgkInterface;

    CURRENT_MODE m_CurrentModes[MAX_VIEWS];

    // Published snapshot.  Guarded by m_ModesLock for the pointer swap and the reference count
    // only; the lock is never held while allocating, calling the control queue, or calling into
    // dxgkrnl.  A FAST_MUTEX, not a KSPIN_LOCK: the snapshot is paged, and a spinlock would raise
    // IRQL before the reference count is touched.
    VIOGPU_MODE_SNAPSHOT *m_pModes;
    FAST_MUTEX m_ModesLock;
    ULONG m_ModeGeneration;

    DXGK_DISPLAY_INFORMATION m_SystemDisplayInfo;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID m_SystemDisplaySourceId;

    VioGpuObj *m_pFrameBuf;

    PHYSICAL_ADDRESS m_sourceAddress = {0};
    VioGpuAllocation *m_sourceRes = NULL;
    volatile LONG m_shouldFlip = 0;


    volatile LONG m_shouldFlipStop = 1;
    KSPIN_LOCK m_vsyncTimerLock;
    KTIMER m_vsyncNotifyTimer;
    KDPC m_vsyncNotifyDpc;
    ULONG m_timerRes = 0;
    LARGE_INTEGER next_vsync_time = {0};
    /* kSourceAddressQueueSize must be power of 2 */
    static const LONG kSourceAddressQueueSize = 16;
    PHYSICAL_ADDRESS m_sourceAddressQueue[kSourceAddressQueueSize] = {};
    volatile LONG m_sourceAddressQueueHead = 0;
    volatile LONG m_sourceAddressQueueTail = 0;
    PHYSICAL_ADDRESS m_lastQueuedSourceAddress = {0};
    volatile LONG m_sourceQueueFullCount = 0;
};
