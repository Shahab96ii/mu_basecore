/** @file
SMM MP service implementation

Copyright (c) 2009 - 2023, Intel Corporation. All rights reserved.<BR>
Copyright (c) 2017, AMD Incorporated. All rights reserved.<BR>

SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "PiSmmCpuDxeSmm.h"

#include <Library/MmMemoryProtectionHobLib.h> // MU_CHANGE

//
// Slots for all MTRR( FIXED MTRR + VARIABLE MTRR + MTRR_LIB_IA32_MTRR_DEF_TYPE)
//
MTRR_SETTINGS                gSmiMtrrs;
UINT64                       gPhyMask;
SMM_DISPATCHER_MP_SYNC_DATA  *mSmmMpSyncData = NULL;
UINTN                        mSmmMpSyncDataSize;
SMM_CPU_SEMAPHORES           mSmmCpuSemaphores;
UINTN                        mSemaphoreSize;
SPIN_LOCK                    *mPFLock = NULL;
SMM_CPU_SYNC_MODE            mCpuSmmSyncMode;
BOOLEAN                      mMachineCheckSupported = FALSE;
MM_COMPLETION                mSmmStartupThisApToken;

//
// Processor specified by mPackageFirstThreadIndex[PackageIndex] will do the package-scope register check.
//
UINT32  *mPackageFirstThreadIndex = NULL;

/**
  Performs an atomic compare exchange operation to get semaphore.
  The compare exchange operation must be performed using
  MP safe mechanisms.

  @param      Sem        IN:  32-bit unsigned integer
                         OUT: original integer - 1
  @return     Original integer - 1

**/
UINT32
WaitForSemaphore (
  IN OUT  volatile UINT32  *Sem
  )
{
  UINT32  Value;

  for ( ; ;) {
    Value = *Sem;
    if ((Value != 0) &&
        (InterlockedCompareExchange32 (
           (UINT32 *)Sem,
           Value,
           Value - 1
           ) == Value))
    {
      break;
    }

    CpuPause ();
  }

  return Value - 1;
}

/**
  Performs an atomic compare exchange operation to release semaphore.
  The compare exchange operation must be performed using
  MP safe mechanisms.

  @param      Sem        IN:  32-bit unsigned integer
                         OUT: original integer + 1
  @return     Original integer + 1

**/
UINT32
ReleaseSemaphore (
  IN OUT  volatile UINT32  *Sem
  )
{
  UINT32  Value;

  do {
    Value = *Sem;
  } while (Value + 1 != 0 &&
           InterlockedCompareExchange32 (
             (UINT32 *)Sem,
             Value,
             Value + 1
             ) != Value);

  return Value + 1;
}

/**
  Performs an atomic compare exchange operation to lock semaphore.
  The compare exchange operation must be performed using
  MP safe mechanisms.

  @param      Sem        IN:  32-bit unsigned integer
                         OUT: -1
  @return     Original integer

**/
UINT32
LockdownSemaphore (
  IN OUT  volatile UINT32  *Sem
  )
{
  UINT32  Value;

  do {
    Value = *Sem;
  } while (InterlockedCompareExchange32 (
             (UINT32 *)Sem,
             Value,
             (UINT32)-1
             ) != Value);

  return Value;
}

/**
  Wait all APs to performs an atomic compare exchange operation to release semaphore.

  @param   NumberOfAPs      AP number

**/
VOID
WaitForAllAPs (
  IN      UINTN  NumberOfAPs
  )
{
  UINTN  BspIndex;

  BspIndex = mSmmMpSyncData->BspIndex;
  while (NumberOfAPs-- > 0) {
    WaitForSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);
  }
}

/**
  Performs an atomic compare exchange operation to release semaphore
  for each AP.

**/
VOID
ReleaseAllAPs (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    if (IsPresentAp (Index)) {
      ReleaseSemaphore (mSmmMpSyncData->CpuData[Index].Run);
    }
  }
}

/**
  Check whether the index of CPU perform the package level register
  programming during System Management Mode initialization.

  The index of Processor specified by mPackageFirstThreadIndex[PackageIndex]
  will do the package-scope register programming.

  @param[in] CpuIndex   Processor Index.

  @retval TRUE  Perform the package level register programming.
  @retval FALSE Don't perform the package level register programming.

**/
BOOLEAN
IsPackageFirstThread (
  IN UINTN  CpuIndex
  )
{
  UINT32  PackageIndex;

  PackageIndex =  gSmmCpuPrivate->ProcessorInfo[CpuIndex].Location.Package;

  ASSERT (mPackageFirstThreadIndex != NULL);

  //
  // Set the value of mPackageFirstThreadIndex[PackageIndex].
  // The package-scope register are checked by the first processor (CpuIndex) in Package.
  //
  // If mPackageFirstThreadIndex[PackageIndex] equals to (UINT32)-1, then update
  // to current CpuIndex. If it doesn't equal to (UINT32)-1, don't change it.
  //
  if (mPackageFirstThreadIndex[PackageIndex] == (UINT32)-1) {
    mPackageFirstThreadIndex[PackageIndex] = (UINT32)CpuIndex;
  }

  return (BOOLEAN)(mPackageFirstThreadIndex[PackageIndex] == CpuIndex);
}

/**
  Returns the Number of SMM Delayed & Blocked & Disabled Thread Count.

  @param[in,out] DelayedCount  The Number of SMM Delayed Thread Count.
  @param[in,out] BlockedCount  The Number of SMM Blocked Thread Count.
  @param[in,out] DisabledCount The Number of SMM Disabled Thread Count.

**/
VOID
GetSmmDelayedBlockedDisabledCount (
  IN OUT UINT32  *DelayedCount,
  IN OUT UINT32  *BlockedCount,
  IN OUT UINT32  *DisabledCount
  )
{
  UINTN  Index;

  for (Index = 0; Index < mNumberOfCpus; Index++) {
    if (IsPackageFirstThread (Index)) {
      if (DelayedCount != NULL) {
        *DelayedCount += (UINT32)SmmCpuFeaturesGetSmmRegister (Index, SmmRegSmmDelayed);
      }

      if (BlockedCount != NULL) {
        *BlockedCount += (UINT32)SmmCpuFeaturesGetSmmRegister (Index, SmmRegSmmBlocked);
      }

      if (DisabledCount != NULL) {
        *DisabledCount += (UINT32)SmmCpuFeaturesGetSmmRegister (Index, SmmRegSmmEnable);
      }
    }
  }
}

/**
  Checks if all CPUs (except Blocked & Disabled) have checked in for this SMI run

  @retval   TRUE  if all CPUs the have checked in.
  @retval   FALSE  if at least one Normal AP hasn't checked in.

**/
BOOLEAN
AllCpusInSmmExceptBlockedDisabled (
  VOID
  )
{
  UINT32  BlockedCount;
  UINT32  DisabledCount;

  BlockedCount  = 0;
  DisabledCount = 0;

  //
  // Check to make sure mSmmMpSyncData->Counter is valid and not locked.
  //
  ASSERT (*mSmmMpSyncData->Counter <= mNumberOfCpus);

  //
  // Check whether all CPUs in SMM.
  //
  if (*mSmmMpSyncData->Counter == mNumberOfCpus) {
    return TRUE;
  }

  //
  // Check for the Blocked & Disabled Exceptions Case.
  //
  GetSmmDelayedBlockedDisabledCount (NULL, &BlockedCount, &DisabledCount);

  //
  // *mSmmMpSyncData->Counter might be updated by all APs concurrently. The value
  // can be dynamic changed. If some Aps enter the SMI after the BlockedCount &
  // DisabledCount check, then the *mSmmMpSyncData->Counter will be increased, thus
  // leading the *mSmmMpSyncData->Counter + BlockedCount + DisabledCount > mNumberOfCpus.
  // since the BlockedCount & DisabledCount are local variable, it's ok here only for
  // the checking of all CPUs In Smm.
  //
  if (*mSmmMpSyncData->Counter + BlockedCount + DisabledCount >= mNumberOfCpus) {
    return TRUE;
  }

  return FALSE;
}

/**
  Has OS enabled Lmce in the MSR_IA32_MCG_EXT_CTL

  @retval TRUE     Os enable lmce.
  @retval FALSE    Os not enable lmce.

**/
BOOLEAN
IsLmceOsEnabled (
  VOID
  )
{
  MSR_IA32_MCG_CAP_REGISTER          McgCap;
  MSR_IA32_FEATURE_CONTROL_REGISTER  FeatureCtrl;
  MSR_IA32_MCG_EXT_CTL_REGISTER      McgExtCtrl;

  McgCap.Uint64 = AsmReadMsr64 (MSR_IA32_MCG_CAP);
  if (McgCap.Bits.MCG_LMCE_P == 0) {
    return FALSE;
  }

  FeatureCtrl.Uint64 = AsmReadMsr64 (MSR_IA32_FEATURE_CONTROL);
  if (FeatureCtrl.Bits.LmceOn == 0) {
    return FALSE;
  }

  McgExtCtrl.Uint64 = AsmReadMsr64 (MSR_IA32_MCG_EXT_CTL);
  return (BOOLEAN)(McgExtCtrl.Bits.LMCE_EN == 1);
}

/**
  Return if Local machine check exception signaled.

  Indicates (when set) that a local machine check exception was generated. This indicates that the current machine-check event was
  delivered to only the logical processor.

  @retval TRUE    LMCE was signaled.
  @retval FALSE   LMCE was not signaled.

**/
BOOLEAN
IsLmceSignaled (
  VOID
  )
{
  MSR_IA32_MCG_STATUS_REGISTER  McgStatus;

  McgStatus.Uint64 = AsmReadMsr64 (MSR_IA32_MCG_STATUS);
  return (BOOLEAN)(McgStatus.Bits.LMCE_S == 1);
}

/**
  Given timeout constraint, wait for all APs to arrive, and insure when this function returns, no AP will execute normal mode code before
  entering SMM, except SMI disabled APs.

**/
VOID
SmmWaitForApArrival (
  VOID
  )
{
  UINT64   Timer;
  UINTN    Index;
  BOOLEAN  LmceEn;
  BOOLEAN  LmceSignal;
  UINT32   DelayedCount;
  UINT32   BlockedCount;

  PERF_FUNCTION_BEGIN ();

  DelayedCount = 0;
  BlockedCount = 0;

  ASSERT (*mSmmMpSyncData->Counter <= mNumberOfCpus);

  LmceEn     = FALSE;
  LmceSignal = FALSE;
  if (mMachineCheckSupported) {
    LmceEn     = IsLmceOsEnabled ();
    LmceSignal = IsLmceSignaled ();
  }

  //
  // Platform implementor should choose a timeout value appropriately:
  // - The timeout value should balance the SMM time constrains and the likelihood that delayed CPUs are excluded in the SMM run. Note
  //   the SMI Handlers must ALWAYS take into account the cases that not all APs are available in an SMI run.
  // - The timeout value must, in the case of 2nd timeout, be at least long enough to give time for all APs to receive the SMI IPI
  //   and either enter SMM or buffer the SMI, to insure there is no CPU running normal mode code when SMI handling starts. This will
  //   be TRUE even if a blocked CPU is brought out of the blocked state by a normal mode CPU (before the normal mode CPU received the
  //   SMI IPI), because with a buffered SMI, and CPU will enter SMM immediately after it is brought out of the blocked state.
  // - The timeout value must be longer than longest possible IO operation in the system
  //

  //
  // Sync with APs 1st timeout
  //
  for (Timer = StartSyncTimer ();
       !IsSyncTimerTimeout (Timer) && !(LmceEn && LmceSignal);
       )
  {
    mSmmMpSyncData->AllApArrivedWithException = AllCpusInSmmExceptBlockedDisabled ();
    if (mSmmMpSyncData->AllApArrivedWithException) {
      break;
    }

    CpuPause ();
  }

  //
  // Not all APs have arrived, so we need 2nd round of timeout. IPIs should be sent to ALL none present APs,
  // because:
  // a) Delayed AP may have just come out of the delayed state. Blocked AP may have just been brought out of blocked state by some AP running
  //    normal mode code. These APs need to be guaranteed to have an SMI pending to insure that once they are out of delayed / blocked state, they
  //    enter SMI immediately without executing instructions in normal mode. Note traditional flow requires there are no APs doing normal mode
  //    work while SMI handling is on-going.
  // b) As a consequence of SMI IPI sending, (spurious) SMI may occur after this SMM run.
  // c) ** NOTE **: Use SMI disabling feature VERY CAREFULLY (if at all) for traditional flow, because a processor in SMI-disabled state
  //    will execute normal mode code, which breaks the traditional SMI handlers' assumption that no APs are doing normal
  //    mode work while SMI handling is on-going.
  // d) We don't add code to check SMI disabling status to skip sending IPI to SMI disabled APs, because:
  //    - In traditional flow, SMI disabling is discouraged.
  //    - In relaxed flow, CheckApArrival() will check SMI disabling status before calling this function.
  //    In both cases, adding SMI-disabling checking code increases overhead.
  //
  if (*mSmmMpSyncData->Counter < mNumberOfCpus) {
    //
    // Send SMI IPIs to bring outside processors in
    //
    for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
      if (!(*(mSmmMpSyncData->CpuData[Index].Present)) && (gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId != INVALID_APIC_ID)) {
        SendSmiIpi ((UINT32)gSmmCpuPrivate->ProcessorInfo[Index].ProcessorId);
      }
    }

    //
    // Sync with APs 2nd timeout.
    //
    for (Timer = StartSyncTimer ();
         !IsSyncTimerTimeout (Timer);
         )
    {
      mSmmMpSyncData->AllApArrivedWithException = AllCpusInSmmExceptBlockedDisabled ();
      if (mSmmMpSyncData->AllApArrivedWithException) {
        break;
      }

      CpuPause ();
    }
  }

  if (!mSmmMpSyncData->AllApArrivedWithException) {
    //
    // Check for the Blocked & Delayed Case.
    //
    GetSmmDelayedBlockedDisabledCount (&DelayedCount, &BlockedCount, NULL);
    DEBUG ((DEBUG_INFO, "SmmWaitForApArrival: Delayed AP Count = %d, Blocked AP Count = %d\n", DelayedCount, BlockedCount));
  }

  PERF_FUNCTION_END ();
}

/**
  Replace OS MTRR's with SMI MTRR's.

  @param    CpuIndex             Processor Index

**/
VOID
ReplaceOSMtrrs (
  IN      UINTN  CpuIndex
  )
{
  SmmCpuFeaturesDisableSmrr ();

  //
  // Replace all MTRRs registers
  //
  MtrrSetAllMtrrs (&gSmiMtrrs);
}

/**
  Wheck whether task has been finished by all APs.

  @param       BlockMode   Whether did it in block mode or non-block mode.

  @retval      TRUE        Task has been finished by all APs.
  @retval      FALSE       Task not has been finished by all APs.

**/
BOOLEAN
WaitForAllAPsNotBusy (
  IN BOOLEAN  BlockMode
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    //
    // Ignore BSP and APs which not call in SMM.
    //
    if (!IsPresentAp (Index)) {
      continue;
    }

    if (BlockMode) {
      AcquireSpinLock (mSmmMpSyncData->CpuData[Index].Busy);
      ReleaseSpinLock (mSmmMpSyncData->CpuData[Index].Busy);
    } else {
      if (AcquireSpinLockOrFail (mSmmMpSyncData->CpuData[Index].Busy)) {
        ReleaseSpinLock (mSmmMpSyncData->CpuData[Index].Busy);
      } else {
        return FALSE;
      }
    }
  }

  return TRUE;
}

/**
  Check whether it is an present AP.

  @param   CpuIndex      The AP index which calls this function.

  @retval  TRUE           It's a present AP.
  @retval  TRUE           This is not an AP or it is not present.

**/
BOOLEAN
IsPresentAp (
  IN UINTN  CpuIndex
  )
{
  return ((CpuIndex != gSmmCpuPrivate->SmmCoreEntryContext.CurrentlyExecutingCpu) &&
          *(mSmmMpSyncData->CpuData[CpuIndex].Present));
}

/**
  Clean up the status flags used during executing the procedure.

  @param   CpuIndex      The AP index which calls this function.

**/
VOID
ReleaseToken (
  IN UINTN  CpuIndex
  )
{
  PROCEDURE_TOKEN  *Token;

  Token = mSmmMpSyncData->CpuData[CpuIndex].Token;

  if (InterlockedDecrement (&Token->RunningApCount) == 0) {
    ReleaseSpinLock (Token->SpinLock);
  }

  mSmmMpSyncData->CpuData[CpuIndex].Token = NULL;
}

/**
  Free the tokens in the maintained list.

**/
VOID
ResetTokens (
  VOID
  )
{
  //
  // Reset the FirstFreeToken to the beginning of token list upon exiting SMI.
  //
  gSmmCpuPrivate->FirstFreeToken = GetFirstNode (&gSmmCpuPrivate->TokenList);
}

/**
  SMI handler for BSP.

  @param     CpuIndex         BSP processor Index
  @param     SyncMode         SMM MP sync mode

**/
VOID
BSPHandler (
  IN      UINTN              CpuIndex,
  IN      SMM_CPU_SYNC_MODE  SyncMode
  )
{
  UINTN          Index;
  MTRR_SETTINGS  Mtrrs;
  UINTN          ApCount;
  BOOLEAN        ClearTopLevelSmiResult;
  UINTN          PresentCount;

  ASSERT (CpuIndex == mSmmMpSyncData->BspIndex);
  ApCount = 0;

  PERF_FUNCTION_BEGIN ();

  //
  // Flag BSP's presence
  //
  *mSmmMpSyncData->InsideSmm = TRUE;

  //
  // Initialize Debug Agent to start source level debug in BSP handler
  //
  InitializeDebugAgent (DEBUG_AGENT_INIT_ENTER_SMI, NULL, NULL);

  //
  // Mark this processor's presence
  //
  *(mSmmMpSyncData->CpuData[CpuIndex].Present) = TRUE;

  //
  // Clear platform top level SMI status bit before calling SMI handlers. If
  // we cleared it after SMI handlers are run, we would miss the SMI that
  // occurs after SMI handlers are done and before SMI status bit is cleared.
  //
  ClearTopLevelSmiResult = ClearTopLevelSmiStatus ();
  ASSERT (ClearTopLevelSmiResult == TRUE);

  //
  // Set running processor index
  //
  gSmmCpuPrivate->SmmCoreEntryContext.CurrentlyExecutingCpu = CpuIndex;

  //
  // If Traditional Sync Mode or need to configure MTRRs: gather all available APs.
  //
  if ((SyncMode == SmmCpuSyncModeTradition) || SmmCpuFeaturesNeedConfigureMtrrs ()) {
    //
    // Wait for APs to arrive
    //
    SmmWaitForApArrival ();

    //
    // Lock the counter down and retrieve the number of APs
    //
    *mSmmMpSyncData->AllCpusInSync = TRUE;
    ApCount                        = LockdownSemaphore (mSmmMpSyncData->Counter) - 1;

    //
    // Wait for all APs to get ready for programming MTRRs
    //
    WaitForAllAPs (ApCount);

    if (SmmCpuFeaturesNeedConfigureMtrrs ()) {
      //
      // Signal all APs it's time for backup MTRRs
      //
      ReleaseAllAPs ();

      //
      // WaitForSemaphore() may wait for ever if an AP happens to enter SMM at
      // exactly this point. Please make sure PcdCpuSmmMaxSyncLoops has been set
      // to a large enough value to avoid this situation.
      // Note: For HT capable CPUs, threads within a core share the same set of MTRRs.
      // We do the backup first and then set MTRR to avoid race condition for threads
      // in the same core.
      //
      MtrrGetAllMtrrs (&Mtrrs);

      //
      // Wait for all APs to complete their MTRR saving
      //
      WaitForAllAPs (ApCount);

      //
      // Let all processors program SMM MTRRs together
      //
      ReleaseAllAPs ();

      //
      // WaitForSemaphore() may wait for ever if an AP happens to enter SMM at
      // exactly this point. Please make sure PcdCpuSmmMaxSyncLoops has been set
      // to a large enough value to avoid this situation.
      //
      ReplaceOSMtrrs (CpuIndex);

      //
      // Wait for all APs to complete their MTRR programming
      //
      WaitForAllAPs (ApCount);
    }
  }

  //
  // The BUSY lock is initialized to Acquired state
  //
  AcquireSpinLock (mSmmMpSyncData->CpuData[CpuIndex].Busy);

  //
  // Perform the pre tasks
  //
  PerformPreTasks ();

  //
  // Invoke SMM Foundation EntryPoint with the processor information context.
  //
  gSmmCpuPrivate->SmmCoreEntry (&gSmmCpuPrivate->SmmCoreEntryContext);

  //
  // Make sure all APs have completed their pending none-block tasks
  //
  WaitForAllAPsNotBusy (TRUE);

  //
  // Perform the remaining tasks
  //
  PerformRemainingTasks ();

  //
  // If Relaxed-AP Sync Mode: gather all available APs after BSP SMM handlers are done, and
  // make those APs to exit SMI synchronously. APs which arrive later will be excluded and
  // will run through freely.
  //
  if ((SyncMode != SmmCpuSyncModeTradition) && !SmmCpuFeaturesNeedConfigureMtrrs ()) {
    //
    // Lock the counter down and retrieve the number of APs
    //
    *mSmmMpSyncData->AllCpusInSync = TRUE;
    ApCount                        = LockdownSemaphore (mSmmMpSyncData->Counter) - 1;
    //
    // Make sure all APs have their Present flag set
    //
    while (TRUE) {
      PresentCount = 0;
      for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
        if (*(mSmmMpSyncData->CpuData[Index].Present)) {
          PresentCount++;
        }
      }

      if (PresentCount > ApCount) {
        break;
      }
    }
  }

  //
  // Notify all APs to exit
  //
  *mSmmMpSyncData->InsideSmm = FALSE;
  ReleaseAllAPs ();

  //
  // Wait for all APs to complete their pending tasks
  //
  WaitForAllAPs (ApCount);

  if (SmmCpuFeaturesNeedConfigureMtrrs ()) {
    //
    // Signal APs to restore MTRRs
    //
    ReleaseAllAPs ();

    //
    // Restore OS MTRRs
    //
    SmmCpuFeaturesReenableSmrr ();
    MtrrSetAllMtrrs (&Mtrrs);

    //
    // Wait for all APs to complete MTRR programming
    //
    WaitForAllAPs (ApCount);
  }

  //
  // Stop source level debug in BSP handler, the code below will not be
  // debugged.
  //
  InitializeDebugAgent (DEBUG_AGENT_INIT_EXIT_SMI, NULL, NULL);

  //
  // Signal APs to Reset states/semaphore for this processor
  //
  ReleaseAllAPs ();

  //
  // Perform pending operations for hot-plug
  //
  SmmCpuUpdate ();

  //
  // Clear the Present flag of BSP
  //
  *(mSmmMpSyncData->CpuData[CpuIndex].Present) = FALSE;

  //
  // Gather APs to exit SMM synchronously. Note the Present flag is cleared by now but
  // WaitForAllAps does not depend on the Present flag.
  //
  WaitForAllAPs (ApCount);

  //
  // At this point, all APs should have exited from APHandler().
  // Migrate the SMM MP performance logging to standard SMM performance logging.
  // Any SMM MP performance logging after this point will be migrated in next SMI.
  //
  PERF_CODE (
    MigrateMpPerf (gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus, CpuIndex);
    );

  //
  // Reset the tokens buffer.
  //
  ResetTokens ();

  //
  // Reset BspIndex to -1, meaning BSP has not been elected.
  //
  if (FeaturePcdGet (PcdCpuSmmEnableBspElection)) {
    mSmmMpSyncData->BspIndex = (UINT32)-1;
  }

  //
  // Allow APs to check in from this point on
  //
  *mSmmMpSyncData->Counter                  = 0;
  *mSmmMpSyncData->AllCpusInSync            = FALSE;
  mSmmMpSyncData->AllApArrivedWithException = FALSE;

  PERF_FUNCTION_END ();
}

/**
  SMI handler for AP.

  @param     CpuIndex         AP processor Index.
  @param     ValidSmi         Indicates that current SMI is a valid SMI or not.
  @param     SyncMode         SMM MP sync mode.

**/
VOID
APHandler (
  IN      UINTN              CpuIndex,
  IN      BOOLEAN            ValidSmi,
  IN      SMM_CPU_SYNC_MODE  SyncMode
  )
{
  UINT64         Timer;
  UINTN          BspIndex;
  MTRR_SETTINGS  Mtrrs;
  EFI_STATUS     ProcedureStatus;

  //
  // Timeout BSP
  //
  for (Timer = StartSyncTimer ();
       !IsSyncTimerTimeout (Timer) &&
       !(*mSmmMpSyncData->InsideSmm);
       )
  {
    CpuPause ();
  }

  if (!(*mSmmMpSyncData->InsideSmm)) {
    //
    // BSP timeout in the first round
    //
    if (mSmmMpSyncData->BspIndex != -1) {
      //
      // BSP Index is known
      // Existing AP is in SMI now but BSP not in, so, try bring BSP in SMM.
      //
      BspIndex = mSmmMpSyncData->BspIndex;
      ASSERT (CpuIndex != BspIndex);

      //
      // Send SMI IPI to bring BSP in
      //
      SendSmiIpi ((UINT32)gSmmCpuPrivate->ProcessorInfo[BspIndex].ProcessorId);

      //
      // Now clock BSP for the 2nd time
      //
      for (Timer = StartSyncTimer ();
           !IsSyncTimerTimeout (Timer) &&
           !(*mSmmMpSyncData->InsideSmm);
           )
      {
        CpuPause ();
      }

      if (!(*mSmmMpSyncData->InsideSmm)) {
        //
        // Give up since BSP is unable to enter SMM
        // and signal the completion of this AP
        // Reduce the mSmmMpSyncData->Counter!
        //
        WaitForSemaphore (mSmmMpSyncData->Counter);
        return;
      }
    } else {
      //
      // Don't know BSP index. Give up without sending IPI to BSP.
      // Reduce the mSmmMpSyncData->Counter!
      //
      WaitForSemaphore (mSmmMpSyncData->Counter);
      return;
    }
  }

  //
  // BSP is available
  //
  BspIndex = mSmmMpSyncData->BspIndex;
  ASSERT (CpuIndex != BspIndex);

  //
  // Mark this processor's presence
  //
  *(mSmmMpSyncData->CpuData[CpuIndex].Present) = TRUE;

  if ((SyncMode == SmmCpuSyncModeTradition) || SmmCpuFeaturesNeedConfigureMtrrs ()) {
    //
    // Notify BSP of arrival at this point
    //
    ReleaseSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);
  }

  if (SmmCpuFeaturesNeedConfigureMtrrs ()) {
    //
    // Wait for the signal from BSP to backup MTRRs
    //
    WaitForSemaphore (mSmmMpSyncData->CpuData[CpuIndex].Run);

    //
    // Backup OS MTRRs
    //
    MtrrGetAllMtrrs (&Mtrrs);

    //
    // Signal BSP the completion of this AP
    //
    ReleaseSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);

    //
    // Wait for BSP's signal to program MTRRs
    //
    WaitForSemaphore (mSmmMpSyncData->CpuData[CpuIndex].Run);

    //
    // Replace OS MTRRs with SMI MTRRs
    //
    ReplaceOSMtrrs (CpuIndex);

    //
    // Signal BSP the completion of this AP
    //
    ReleaseSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);
  }

  while (TRUE) {
    //
    // Wait for something to happen
    //
    WaitForSemaphore (mSmmMpSyncData->CpuData[CpuIndex].Run);

    //
    // Check if BSP wants to exit SMM
    //
    if (!(*mSmmMpSyncData->InsideSmm)) {
      break;
    }

    //
    // BUSY should be acquired by SmmStartupThisAp()
    //
    ASSERT (
      !AcquireSpinLockOrFail (mSmmMpSyncData->CpuData[CpuIndex].Busy)
      );

    //
    // Invoke the scheduled procedure
    //
    ProcedureStatus = (*mSmmMpSyncData->CpuData[CpuIndex].Procedure)(
         (VOID *)mSmmMpSyncData->CpuData[CpuIndex].Parameter
  );
    if (mSmmMpSyncData->CpuData[CpuIndex].Status != NULL) {
      *mSmmMpSyncData->CpuData[CpuIndex].Status = ProcedureStatus;
    }

    if (mSmmMpSyncData->CpuData[CpuIndex].Token != NULL) {
      ReleaseToken (CpuIndex);
    }

    //
    // Release BUSY
    //
    ReleaseSpinLock (mSmmMpSyncData->CpuData[CpuIndex].Busy);
  }

  if (SmmCpuFeaturesNeedConfigureMtrrs ()) {
    //
    // Notify BSP the readiness of this AP to program MTRRs
    //
    ReleaseSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);

    //
    // Wait for the signal from BSP to program MTRRs
    //
    WaitForSemaphore (mSmmMpSyncData->CpuData[CpuIndex].Run);

    //
    // Restore OS MTRRs
    //
    SmmCpuFeaturesReenableSmrr ();
    MtrrSetAllMtrrs (&Mtrrs);
  }

  //
  // Notify BSP the readiness of this AP to Reset states/semaphore for this processor
  //
  ReleaseSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);

  //
  // Wait for the signal from BSP to Reset states/semaphore for this processor
  //
  WaitForSemaphore (mSmmMpSyncData->CpuData[CpuIndex].Run);

  //
  // Reset states/semaphore for this processor
  //
  *(mSmmMpSyncData->CpuData[CpuIndex].Present) = FALSE;

  //
  // Notify BSP the readiness of this AP to exit SMM
  //
  ReleaseSemaphore (mSmmMpSyncData->CpuData[BspIndex].Run);
}

/**
  // MU_CHANGE START Update to use memory protection settings HOB
  if (gMmMps.NullPointerDetectionPolicy) {
    // MU_CHANGE END
  Checks whether the input token is the current used token.

  @param[in]  Token      This parameter describes the token that was passed into DispatchProcedure or
                         BroadcastProcedure.

  @retval TRUE           The input token is the current used token.
  @retval FALSE          The input token is not the current used token.
**/
BOOLEAN
IsTokenInUse (
  IN SPIN_LOCK  *Token
  )
{
  LIST_ENTRY       *Link;
  PROCEDURE_TOKEN  *ProcToken;

  if (Token == NULL) {
    return FALSE;
  }

  Link = GetFirstNode (&gSmmCpuPrivate->TokenList);
  //
  // Only search used tokens.
  //
  while (Link != gSmmCpuPrivate->FirstFreeToken) {
    ProcToken = PROCEDURE_TOKEN_FROM_LINK (Link);

    if (ProcToken->SpinLock == Token) {
      return TRUE;
    }

    Link = GetNextNode (&gSmmCpuPrivate->TokenList, Link);
  }

  return FALSE;
}

/**
  Allocate buffer for the SPIN_LOCK and PROCEDURE_TOKEN.

  @return First token of the token buffer.
**/
LIST_ENTRY *
AllocateTokenBuffer (
  VOID
  )
{
  UINTN            SpinLockSize;
  UINT32           TokenCountPerChunk;
  UINTN            Index;
  SPIN_LOCK        *SpinLock;
  UINT8            *SpinLockBuffer;
  PROCEDURE_TOKEN  *ProcTokens;

  SpinLockSize = GetSpinLockProperties ();

  TokenCountPerChunk = FixedPcdGet32 (PcdCpuSmmMpTokenCountPerChunk);
  ASSERT (TokenCountPerChunk != 0);
  if (TokenCountPerChunk == 0) {
    DEBUG ((DEBUG_ERROR, "PcdCpuSmmMpTokenCountPerChunk should not be Zero!\n"));
    CpuDeadLoop ();
  }

  DEBUG ((DEBUG_INFO, "CpuSmm: SpinLock Size = 0x%x, PcdCpuSmmMpTokenCountPerChunk = 0x%x\n", SpinLockSize, TokenCountPerChunk));

  //
  // Separate the Spin_lock and Proc_token because the alignment requires by Spin_Lock.
  //
  SpinLockBuffer = AllocatePool (SpinLockSize * TokenCountPerChunk);
  // MU_CHANGE [BEGIN] - CodeQL change
  if (SpinLockBuffer == NULL) {
    ASSERT (SpinLockBuffer != NULL);
    return NULL;
  }

  // MU_CHANGE [END] - CodeQL change

  ProcTokens = AllocatePool (sizeof (PROCEDURE_TOKEN) * TokenCountPerChunk);
  // MU_CHANGE [BEGIN] - CodeQL change
  if (ProcTokens == NULL) {
    ASSERT (ProcTokens != NULL);
    FreePool (SpinLockBuffer);
    return NULL;
  }

  // MU_CHANGE [END] - CodeQL change

  for (Index = 0; Index < TokenCountPerChunk; Index++) {
    SpinLock = (SPIN_LOCK *)(SpinLockBuffer + SpinLockSize * Index);
    InitializeSpinLock (SpinLock);

    ProcTokens[Index].Signature      = PROCEDURE_TOKEN_SIGNATURE;
    ProcTokens[Index].SpinLock       = SpinLock;
    ProcTokens[Index].RunningApCount = 0;

    InsertTailList (&gSmmCpuPrivate->TokenList, &ProcTokens[Index].Link);
  }

  return &ProcTokens[0].Link;
}

/**
  Get the free token.

  If no free token, allocate new tokens then return the free one.

  @param RunningApsCount    The Running Aps count for this token.

  @retval    return the first free PROCEDURE_TOKEN.

**/
PROCEDURE_TOKEN *
GetFreeToken (
  IN UINT32  RunningApsCount
  )
{
  PROCEDURE_TOKEN  *NewToken;

  //
  // If FirstFreeToken meets the end of token list, enlarge the token list.
  // Set FirstFreeToken to the first free token.
  //
  if (gSmmCpuPrivate->FirstFreeToken == &gSmmCpuPrivate->TokenList) {
    gSmmCpuPrivate->FirstFreeToken = AllocateTokenBuffer ();
  }

  NewToken                       = PROCEDURE_TOKEN_FROM_LINK (gSmmCpuPrivate->FirstFreeToken);
  gSmmCpuPrivate->FirstFreeToken = GetNextNode (&gSmmCpuPrivate->TokenList, gSmmCpuPrivate->FirstFreeToken);

  NewToken->RunningApCount = RunningApsCount;
  AcquireSpinLock (NewToken->SpinLock);

  return NewToken;
}

/**
  Checks status of specified AP.

  This function checks whether the specified AP has finished the task assigned
  by StartupThisAP(), and whether timeout expires.

  @param[in]  Token             This parameter describes the token that was passed into DispatchProcedure or
                                BroadcastProcedure.

  @retval EFI_SUCCESS           Specified AP has finished task assigned by StartupThisAPs().
  @retval EFI_NOT_READY         Specified AP has not finished task and timeout has not expired.
**/
EFI_STATUS
IsApReady (
  IN SPIN_LOCK  *Token
  )
{
  if (AcquireSpinLockOrFail (Token)) {
    ReleaseSpinLock (Token);
    return EFI_SUCCESS;
  }

  return EFI_NOT_READY;
}

/**
  Schedule a procedure to run on the specified CPU.

  @param[in]       Procedure                The address of the procedure to run
  @param[in]       CpuIndex                 Target CPU Index
  @param[in,out]   ProcArguments            The parameter to pass to the procedure
  @param[in]       Token                    This is an optional parameter that allows the caller to execute the
                                            procedure in a blocking or non-blocking fashion. If it is NULL the
                                            call is blocking, and the call will not return until the AP has
                                            completed the procedure. If the token is not NULL, the call will
                                            return immediately. The caller can check whether the procedure has
                                            completed with CheckOnProcedure or WaitForProcedure.
  @param[in]       TimeoutInMicroseconds    Indicates the time limit in microseconds for the APs to finish
                                            execution of Procedure, either for blocking or non-blocking mode.
                                            Zero means infinity. If the timeout expires before all APs return
                                            from Procedure, then Procedure on the failed APs is terminated. If
                                            the timeout expires in blocking mode, the call returns EFI_TIMEOUT.
                                            If the timeout expires in non-blocking mode, the timeout determined
                                            can be through CheckOnProcedure or WaitForProcedure.
                                            Note that timeout support is optional. Whether an implementation
                                            supports this feature can be determined via the Attributes data
                                            member.
  @param[in,out]   CpuStatus                This optional pointer may be used to get the status code returned
                                            by Procedure when it completes execution on the target AP, or with
                                            EFI_TIMEOUT if the Procedure fails to complete within the optional
                                            timeout. The implementation will update this variable with
                                            EFI_NOT_READY prior to starting Procedure on the target AP.

  @retval EFI_INVALID_PARAMETER    CpuNumber not valid
  @retval EFI_INVALID_PARAMETER    CpuNumber specifying BSP
  @retval EFI_INVALID_PARAMETER    The AP specified by CpuNumber did not enter SMM
  @retval EFI_INVALID_PARAMETER    The AP specified by CpuNumber is busy
  @retval EFI_SUCCESS              The procedure has been successfully scheduled

**/
EFI_STATUS
InternalSmmStartupThisAp (
  IN      EFI_AP_PROCEDURE2  Procedure,
  IN      UINTN              CpuIndex,
  IN OUT  VOID               *ProcArguments OPTIONAL,
  IN      MM_COMPLETION      *Token,
  IN      UINTN              TimeoutInMicroseconds,
  IN OUT  EFI_STATUS         *CpuStatus
  )
{
  PROCEDURE_TOKEN  *ProcToken;

  if (CpuIndex >= gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus) {
    DEBUG ((DEBUG_ERROR, "CpuIndex(%d) >= gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus(%d)\n", CpuIndex, gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus));
    return EFI_INVALID_PARAMETER;
  }

  if (CpuIndex == gSmmCpuPrivate->SmmCoreEntryContext.CurrentlyExecutingCpu) {
    DEBUG ((DEBUG_ERROR, "CpuIndex(%d) == gSmmCpuPrivate->SmmCoreEntryContext.CurrentlyExecutingCpu\n", CpuIndex));
    return EFI_INVALID_PARAMETER;
  }

  if (gSmmCpuPrivate->ProcessorInfo[CpuIndex].ProcessorId == INVALID_APIC_ID) {
    return EFI_INVALID_PARAMETER;
  }

  if (!(*(mSmmMpSyncData->CpuData[CpuIndex].Present))) {
    if (mSmmMpSyncData->EffectiveSyncMode == SmmCpuSyncModeTradition) {
      DEBUG ((DEBUG_ERROR, "!mSmmMpSyncData->CpuData[%d].Present\n", CpuIndex));
    }

    return EFI_INVALID_PARAMETER;
  }

  if (gSmmCpuPrivate->Operation[CpuIndex] == SmmCpuRemove) {
    if (!FeaturePcdGet (PcdCpuHotPlugSupport)) {
      DEBUG ((DEBUG_ERROR, "gSmmCpuPrivate->Operation[%d] == SmmCpuRemove\n", CpuIndex));
    }

    return EFI_INVALID_PARAMETER;
  }

  if ((TimeoutInMicroseconds != 0) && ((mSmmMp.Attributes & EFI_MM_MP_TIMEOUT_SUPPORTED) == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Procedure == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  AcquireSpinLock (mSmmMpSyncData->CpuData[CpuIndex].Busy);

  mSmmMpSyncData->CpuData[CpuIndex].Procedure = Procedure;
  mSmmMpSyncData->CpuData[CpuIndex].Parameter = ProcArguments;
  if (Token != NULL) {
    if (Token != &mSmmStartupThisApToken) {
      //
      // When Token points to mSmmStartupThisApToken, this routine is called
      // from SmmStartupThisAp() in non-blocking mode (PcdCpuSmmBlockStartupThisAp == FALSE).
      //
      // In this case, caller wants to startup AP procedure in non-blocking
      // mode and cannot get the completion status from the Token because there
      // is no way to return the Token to caller from SmmStartupThisAp().
      // Caller needs to use its implementation specific way to query the completion status.
      //
      // There is no need to allocate a token for such case so the 3 overheads
      // can be avoided:
      // 1. Call AllocateTokenBuffer() when there is no free token.
      // 2. Get a free token from the token buffer.
      // 3. Call ReleaseToken() in APHandler().
      //
      ProcToken                               = GetFreeToken (1);
      mSmmMpSyncData->CpuData[CpuIndex].Token = ProcToken;
      *Token                                  = (MM_COMPLETION)ProcToken->SpinLock;
    }
  }

  mSmmMpSyncData->CpuData[CpuIndex].Status = CpuStatus;
  if (mSmmMpSyncData->CpuData[CpuIndex].Status != NULL) {
    *mSmmMpSyncData->CpuData[CpuIndex].Status = EFI_NOT_READY;
  }

  ReleaseSemaphore (mSmmMpSyncData->CpuData[CpuIndex].Run);

  if (Token == NULL) {
    AcquireSpinLock (mSmmMpSyncData->CpuData[CpuIndex].Busy);
    ReleaseSpinLock (mSmmMpSyncData->CpuData[CpuIndex].Busy);
  }

  return EFI_SUCCESS;
}

/**
  Worker function to execute a caller provided function on all enabled APs.

  @param[in]     Procedure               A pointer to the function to be run on
                                         enabled APs of the system.
  @param[in]     TimeoutInMicroseconds   Indicates the time limit in microseconds for
                                         APs to return from Procedure, either for
                                         blocking or non-blocking mode.
  @param[in,out] ProcedureArguments      The parameter passed into Procedure for
                                         all APs.
  @param[in,out] Token                   This is an optional parameter that allows the caller to execute the
                                         procedure in a blocking or non-blocking fashion. If it is NULL the
                                         call is blocking, and the call will not return until the AP has
                                         completed the procedure. If the token is not NULL, the call will
                                         return immediately. The caller can check whether the procedure has
                                         completed with CheckOnProcedure or WaitForProcedure.
  @param[in,out] CPUStatus               This optional pointer may be used to get the status code returned
                                         by Procedure when it completes execution on the target AP, or with
                                         EFI_TIMEOUT if the Procedure fails to complete within the optional
                                         timeout. The implementation will update this variable with
                                         EFI_NOT_READY prior to starting Procedure on the target AP.


  @retval EFI_SUCCESS             In blocking mode, all APs have finished before
                                  the timeout expired.
  @retval EFI_SUCCESS             In non-blocking mode, function has been dispatched
                                  to all enabled APs.
  @retval others                  Failed to Startup all APs.

**/
EFI_STATUS
InternalSmmStartupAllAPs (
  IN       EFI_AP_PROCEDURE2  Procedure,
  IN       UINTN              TimeoutInMicroseconds,
  IN OUT   VOID               *ProcedureArguments OPTIONAL,
  IN OUT   MM_COMPLETION      *Token,
  IN OUT   EFI_STATUS         *CPUStatus
  )
{
  UINTN            Index;
  UINTN            CpuCount;
  PROCEDURE_TOKEN  *ProcToken;

  if ((TimeoutInMicroseconds != 0) && ((mSmmMp.Attributes & EFI_MM_MP_TIMEOUT_SUPPORTED) == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Procedure == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CpuCount = 0;
  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    if (IsPresentAp (Index)) {
      CpuCount++;

      if (gSmmCpuPrivate->Operation[Index] == SmmCpuRemove) {
        return EFI_INVALID_PARAMETER;
      }

      if (!AcquireSpinLockOrFail (mSmmMpSyncData->CpuData[Index].Busy)) {
        return EFI_NOT_READY;
      }

      ReleaseSpinLock (mSmmMpSyncData->CpuData[Index].Busy);
    }
  }

  if (CpuCount == 0) {
    return EFI_NOT_STARTED;
  }

  if (Token != NULL) {
    ProcToken = GetFreeToken ((UINT32)mMaxNumberOfCpus);
    *Token    = (MM_COMPLETION)ProcToken->SpinLock;
  } else {
    ProcToken = NULL;
  }

  //
  // Make sure all BUSY should be acquired.
  //
  // Because former code already check mSmmMpSyncData->CpuData[***].Busy for each AP.
  // Here code always use AcquireSpinLock instead of AcquireSpinLockOrFail for not
  // block mode.
  //
  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    if (IsPresentAp (Index)) {
      AcquireSpinLock (mSmmMpSyncData->CpuData[Index].Busy);
    }
  }

  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    if (IsPresentAp (Index)) {
      mSmmMpSyncData->CpuData[Index].Procedure = (EFI_AP_PROCEDURE2)Procedure;
      mSmmMpSyncData->CpuData[Index].Parameter = ProcedureArguments;
      if (ProcToken != NULL) {
        mSmmMpSyncData->CpuData[Index].Token = ProcToken;
      }

      if (CPUStatus != NULL) {
        mSmmMpSyncData->CpuData[Index].Status = &CPUStatus[Index];
        if (mSmmMpSyncData->CpuData[Index].Status != NULL) {
          *mSmmMpSyncData->CpuData[Index].Status = EFI_NOT_READY;
        }
      }
    } else {
      //
      // PI spec requirement:
      // For every excluded processor, the array entry must contain a value of EFI_NOT_STARTED.
      //
      if (CPUStatus != NULL) {
        CPUStatus[Index] = EFI_NOT_STARTED;
      }

      //
      // Decrease the count to mark this processor(AP or BSP) as finished.
      //
      if (ProcToken != NULL) {
        WaitForSemaphore (&ProcToken->RunningApCount);
      }
    }
  }

  ReleaseAllAPs ();

  if (Token == NULL) {
    //
    // Make sure all APs have completed their tasks.
    //
    WaitForAllAPsNotBusy (TRUE);
  }

  return EFI_SUCCESS;
}

/**
  ISO C99 6.5.2.2 "Function calls", paragraph 9:
  If the function is defined with a type that is not compatible with
  the type (of the expression) pointed to by the expression that
  denotes the called function, the behavior is undefined.

  So add below wrapper function to convert between EFI_AP_PROCEDURE
  and EFI_AP_PROCEDURE2.

  Wrapper for Procedures.

  @param[in]  Buffer              Pointer to PROCEDURE_WRAPPER buffer.

**/
EFI_STATUS
EFIAPI
ProcedureWrapper (
  IN     VOID  *Buffer
  )
{
  PROCEDURE_WRAPPER  *Wrapper;

  Wrapper = Buffer;
  Wrapper->Procedure (Wrapper->ProcedureArgument);

  return EFI_SUCCESS;
}

/**
  Schedule a procedure to run on the specified CPU in blocking mode.

  @param[in]       Procedure                The address of the procedure to run
  @param[in]       CpuIndex                 Target CPU Index
  @param[in, out]  ProcArguments            The parameter to pass to the procedure

  @retval EFI_INVALID_PARAMETER    CpuNumber not valid
  @retval EFI_INVALID_PARAMETER    CpuNumber specifying BSP
  @retval EFI_INVALID_PARAMETER    The AP specified by CpuNumber did not enter SMM
  @retval EFI_INVALID_PARAMETER    The AP specified by CpuNumber is busy
  @retval EFI_SUCCESS              The procedure has been successfully scheduled

**/
EFI_STATUS
EFIAPI
SmmBlockingStartupThisAp (
  IN      EFI_AP_PROCEDURE  Procedure,
  IN      UINTN             CpuIndex,
  IN OUT  VOID              *ProcArguments OPTIONAL
  )
{
  PROCEDURE_WRAPPER  Wrapper;

  Wrapper.Procedure         = Procedure;
  Wrapper.ProcedureArgument = ProcArguments;

  //
  // Use wrapper function to convert EFI_AP_PROCEDURE to EFI_AP_PROCEDURE2.
  //
  return InternalSmmStartupThisAp (ProcedureWrapper, CpuIndex, &Wrapper, NULL, 0, NULL);
}

/**
  Schedule a procedure to run on the specified CPU.

  @param  Procedure                The address of the procedure to run
  @param  CpuIndex                 Target CPU Index
  @param  ProcArguments            The parameter to pass to the procedure

  @retval EFI_INVALID_PARAMETER    CpuNumber not valid
  @retval EFI_INVALID_PARAMETER    CpuNumber specifying BSP
  @retval EFI_INVALID_PARAMETER    The AP specified by CpuNumber did not enter SMM
  @retval EFI_INVALID_PARAMETER    The AP specified by CpuNumber is busy
  @retval EFI_SUCCESS              The procedure has been successfully scheduled

**/
EFI_STATUS
EFIAPI
SmmStartupThisAp (
  IN      EFI_AP_PROCEDURE  Procedure,
  IN      UINTN             CpuIndex,
  IN OUT  VOID              *ProcArguments OPTIONAL
  )
{
  gSmmCpuPrivate->ApWrapperFunc[CpuIndex].Procedure         = Procedure;
  gSmmCpuPrivate->ApWrapperFunc[CpuIndex].ProcedureArgument = ProcArguments;

  //
  // Use wrapper function to convert EFI_AP_PROCEDURE to EFI_AP_PROCEDURE2.
  //
  return InternalSmmStartupThisAp (
           ProcedureWrapper,
           CpuIndex,
           &gSmmCpuPrivate->ApWrapperFunc[CpuIndex],
           FeaturePcdGet (PcdCpuSmmBlockStartupThisAp) ? NULL : &mSmmStartupThisApToken,
           0,
           NULL
           );
}

/**
  This function sets DR6 & DR7 according to SMM save state, before running SMM C code.
  They are useful when you want to enable hardware breakpoints in SMM without entry SMM mode.

  NOTE: It might not be appreciated in runtime since it might
        conflict with OS debugging facilities. Turn them off in RELEASE.

  @param    CpuIndex              CPU Index

**/
VOID
EFIAPI
CpuSmmDebugEntry (
  IN UINTN  CpuIndex
  )
{
  SMRAM_SAVE_STATE_MAP  *CpuSaveState;

  if (FeaturePcdGet (PcdCpuSmmDebug)) {
    ASSERT (CpuIndex < mMaxNumberOfCpus);
    CpuSaveState = (SMRAM_SAVE_STATE_MAP *)gSmmCpuPrivate->CpuSaveState[CpuIndex];
    if (mSmmSaveStateRegisterLma == EFI_SMM_SAVE_STATE_REGISTER_LMA_32BIT) {
      AsmWriteDr6 (CpuSaveState->x86._DR6);
      AsmWriteDr7 (CpuSaveState->x86._DR7);
    } else {
      AsmWriteDr6 ((UINTN)CpuSaveState->x64._DR6);
      AsmWriteDr7 ((UINTN)CpuSaveState->x64._DR7);
    }
  }
}

/**
  This function restores DR6 & DR7 to SMM save state.

  NOTE: It might not be appreciated in runtime since it might
        conflict with OS debugging facilities. Turn them off in RELEASE.

  @param    CpuIndex              CPU Index

**/
VOID
EFIAPI
CpuSmmDebugExit (
  IN UINTN  CpuIndex
  )
{
  SMRAM_SAVE_STATE_MAP  *CpuSaveState;

  if (FeaturePcdGet (PcdCpuSmmDebug)) {
    ASSERT (CpuIndex < mMaxNumberOfCpus);
    CpuSaveState = (SMRAM_SAVE_STATE_MAP *)gSmmCpuPrivate->CpuSaveState[CpuIndex];
    if (mSmmSaveStateRegisterLma == EFI_SMM_SAVE_STATE_REGISTER_LMA_32BIT) {
      CpuSaveState->x86._DR7 = (UINT32)AsmReadDr7 ();
      CpuSaveState->x86._DR6 = (UINT32)AsmReadDr6 ();
    } else {
      CpuSaveState->x64._DR7 = AsmReadDr7 ();
      CpuSaveState->x64._DR6 = AsmReadDr6 ();
    }
  }
}

/**
  C function for SMI entry, each processor comes here upon SMI trigger.

  @param    CpuIndex              CPU Index

**/
VOID
EFIAPI
SmiRendezvous (
  IN      UINTN  CpuIndex
  )
{
  EFI_STATUS  Status;
  BOOLEAN     ValidSmi;
  BOOLEAN     IsBsp;
  BOOLEAN     BspInProgress;
  UINTN       Index;
  UINTN       Cr2;

  ASSERT (CpuIndex < mMaxNumberOfCpus);

  if (mSmmRelocated) {
    ASSERT (mSmmInitialized != NULL);
  }

  //
  // Save Cr2 because Page Fault exception in SMM may override its value,
  // when using on-demand paging for above 4G memory.
  //
  Cr2 = 0;
  SaveCr2 (&Cr2);

  if (mSmmRelocated && !mSmmInitialized[CpuIndex]) {
    //
    // Perform SmmInitHandler for CpuIndex
    //
    SmmInitHandler ();

    //
    // Restore Cr2
    //
    RestoreCr2 (Cr2);

    //
    // Mark the first SMI init for CpuIndex has been done so as to avoid the reentry.
    //
    mSmmInitialized[CpuIndex] = TRUE;

    return;
  }

  //
  // Call the user register Startup function first.
  //
  if (mSmmMpSyncData->StartupProcedure != NULL) {
    mSmmMpSyncData->StartupProcedure (mSmmMpSyncData->StartupProcArgs);
  }

  //
  // Perform CPU specific entry hooks
  //
  PERF_CODE (
    MpPerfBegin (CpuIndex, SMM_MP_PERF_PROCEDURE_ID (SmmRendezvousEntry));
    );
  SmmCpuFeaturesRendezvousEntry (CpuIndex);
  PERF_CODE (
    MpPerfEnd (CpuIndex, SMM_MP_PERF_PROCEDURE_ID (SmmRendezvousEntry));
    );

  //
  // Determine if this is a valid SMI
  //
  PERF_CODE (
    MpPerfBegin (CpuIndex, SMM_MP_PERF_PROCEDURE_ID (PlatformValidSmi));
    );
  ValidSmi = PlatformValidSmi ();
  PERF_CODE (
    MpPerfEnd (CpuIndex, SMM_MP_PERF_PROCEDURE_ID (PlatformValidSmi));
    );

  //
  // Determine if BSP has been already in progress. Note this must be checked after
  // ValidSmi because BSP may clear a valid SMI source after checking in.
  //
  BspInProgress = *mSmmMpSyncData->InsideSmm;

  if (!BspInProgress && !ValidSmi) {
    //
    // If we reach here, it means when we sampled the ValidSmi flag, SMI status had not
    // been cleared by BSP in a new SMI run (so we have a truly invalid SMI), or SMI
    // status had been cleared by BSP and an existing SMI run has almost ended. (Note
    // we sampled ValidSmi flag BEFORE judging BSP-in-progress status.) In both cases, there
    // is nothing we need to do.
    //
    goto Exit;
  } else {
    //
    // Signal presence of this processor
    // mSmmMpSyncData->Counter is increased here!
    // "ReleaseSemaphore (mSmmMpSyncData->Counter) == 0" means BSP has already ended the synchronization.
    //
    if (ReleaseSemaphore (mSmmMpSyncData->Counter) == 0) {
      //
      // BSP has already ended the synchronization, so QUIT!!!
      // Existing AP is too late now to enter SMI since BSP has already ended the synchronization!!!
      //

      //
      // Wait for BSP's signal to finish SMI
      //
      while (*mSmmMpSyncData->AllCpusInSync) {
        CpuPause ();
      }

      goto Exit;
    } else {
      //
      // The BUSY lock is initialized to Released state.
      // This needs to be done early enough to be ready for BSP's SmmStartupThisAp() call.
      // E.g., with Relaxed AP flow, SmmStartupThisAp() may be called immediately
      // after AP's present flag is detected.
      //
      InitializeSpinLock (mSmmMpSyncData->CpuData[CpuIndex].Busy);
    }

    if (FeaturePcdGet (PcdCpuSmmProfileEnable)) {
      ActivateSmmProfile (CpuIndex);
    }

    if (BspInProgress) {
      //
      // BSP has been elected. Follow AP path, regardless of ValidSmi flag
      // as BSP may have cleared the SMI status
      //
      APHandler (CpuIndex, ValidSmi, mSmmMpSyncData->EffectiveSyncMode);
    } else {
      //
      // We have a valid SMI
      //

      //
      // Elect BSP
      //
      IsBsp = FALSE;
      if (FeaturePcdGet (PcdCpuSmmEnableBspElection)) {
        if (!mSmmMpSyncData->SwitchBsp || mSmmMpSyncData->CandidateBsp[CpuIndex]) {
          //
          // Call platform hook to do BSP election
          //
          Status = PlatformSmmBspElection (&IsBsp);
          if (EFI_SUCCESS == Status) {
            //
            // Platform hook determines successfully
            //
            if (IsBsp) {
              mSmmMpSyncData->BspIndex = (UINT32)CpuIndex;
            }
          } else {
            //
            // Platform hook fails to determine, use default BSP election method
            //
            InterlockedCompareExchange32 (
              (UINT32 *)&mSmmMpSyncData->BspIndex,
              (UINT32)-1,
              (UINT32)CpuIndex
              );
          }
        }
      }

      //
      // "mSmmMpSyncData->BspIndex == CpuIndex" means this is the BSP
      //
      if (mSmmMpSyncData->BspIndex == CpuIndex) {
        //
        // Clear last request for SwitchBsp.
        //
        if (mSmmMpSyncData->SwitchBsp) {
          mSmmMpSyncData->SwitchBsp = FALSE;
          for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
            mSmmMpSyncData->CandidateBsp[Index] = FALSE;
          }
        }

        if (FeaturePcdGet (PcdCpuSmmProfileEnable)) {
          SmmProfileRecordSmiNum ();
        }

        //
        // BSP Handler is always called with a ValidSmi == TRUE
        //
        BSPHandler (CpuIndex, mSmmMpSyncData->EffectiveSyncMode);
      } else {
        APHandler (CpuIndex, ValidSmi, mSmmMpSyncData->EffectiveSyncMode);
      }
    }

    ASSERT (*mSmmMpSyncData->CpuData[CpuIndex].Run == 0);

    //
    // Wait for BSP's signal to exit SMI
    //
    while (*mSmmMpSyncData->AllCpusInSync) {
      CpuPause ();
    }
  }

Exit:
  //
  // Note: SmmRendezvousExit perf-logging entry is the only one that will be
  //       migrated to standard perf-logging database in next SMI by BSPHandler().
  //       Hence, the number of SmmRendezvousEntry entries will be larger than
  //       the number of SmmRendezvousExit entries. Delta equals to the number
  //       of CPU threads.
  //
  PERF_CODE (
    MpPerfBegin (CpuIndex, SMM_MP_PERF_PROCEDURE_ID (SmmRendezvousExit));
    );
  SmmCpuFeaturesRendezvousExit (CpuIndex);
  PERF_CODE (
    MpPerfEnd (CpuIndex, SMM_MP_PERF_PROCEDURE_ID (SmmRendezvousExit));
    );

  //
  // Restore Cr2
  //
  RestoreCr2 (Cr2);
}

/**
  Initialize PackageBsp Info. Processor specified by mPackageFirstThreadIndex[PackageIndex]
  will do the package-scope register programming. Set default CpuIndex to (UINT32)-1, which
  means not specified yet.

**/
VOID
InitPackageFirstThreadIndexInfo (
  VOID
  )
{
  UINT32  Index;
  UINT32  PackageId;
  UINT32  PackageCount;

  PackageId    = 0;
  PackageCount = 0;

  //
  // Count the number of package, set to max PackageId + 1
  //
  for (Index = 0; Index < mNumberOfCpus; Index++) {
    if (PackageId < gSmmCpuPrivate->ProcessorInfo[Index].Location.Package) {
      PackageId = gSmmCpuPrivate->ProcessorInfo[Index].Location.Package;
    }
  }

  PackageCount = PackageId + 1;

  mPackageFirstThreadIndex = (UINT32 *)AllocatePool (sizeof (UINT32) * PackageCount);
  ASSERT (mPackageFirstThreadIndex != NULL);
  if (mPackageFirstThreadIndex == NULL) {
    return;
  }

  //
  // Set default CpuIndex to (UINT32)-1, which means not specified yet.
  //
  SetMem32 (mPackageFirstThreadIndex, sizeof (UINT32) * PackageCount, (UINT32)-1);
}

/**
  Allocate buffer for SpinLock and Wrapper function buffer.

**/
VOID
InitializeDataForMmMp (
  VOID
  )
{
  gSmmCpuPrivate->ApWrapperFunc = AllocatePool (sizeof (PROCEDURE_WRAPPER) * gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus);
  ASSERT (gSmmCpuPrivate->ApWrapperFunc != NULL);

  InitializeListHead (&gSmmCpuPrivate->TokenList);

  gSmmCpuPrivate->FirstFreeToken = AllocateTokenBuffer ();
}

/**
  Allocate buffer for all semaphores and spin locks.

**/
VOID
InitializeSmmCpuSemaphores (
  VOID
  )
{
  UINTN  ProcessorCount;
  UINTN  TotalSize;
  UINTN  GlobalSemaphoresSize;
  UINTN  CpuSemaphoresSize;
  UINTN  SemaphoreSize;
  UINTN  Pages;
  UINTN  *SemaphoreBlock;
  UINTN  SemaphoreAddr;

  SemaphoreSize        = GetSpinLockProperties ();
  ProcessorCount       = gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus;
  GlobalSemaphoresSize = (sizeof (SMM_CPU_SEMAPHORE_GLOBAL) / sizeof (VOID *)) * SemaphoreSize;
  CpuSemaphoresSize    = (sizeof (SMM_CPU_SEMAPHORE_CPU) / sizeof (VOID *)) * ProcessorCount * SemaphoreSize;
  TotalSize            = GlobalSemaphoresSize + CpuSemaphoresSize;
  DEBUG ((DEBUG_INFO, "One Semaphore Size    = 0x%x\n", SemaphoreSize));
  DEBUG ((DEBUG_INFO, "Total Semaphores Size = 0x%x\n", TotalSize));
  Pages          = EFI_SIZE_TO_PAGES (TotalSize);
  SemaphoreBlock = AllocatePages (Pages);
  // MU_CHANGE [BEGIN] - CodeQL change
  if (SemaphoreBlock == NULL) {
    ASSERT (SemaphoreBlock != NULL);
    return;
  }

  // MU_CHANGE [END] - CodeQL change
  ZeroMem (SemaphoreBlock, TotalSize);

  SemaphoreAddr                                   = (UINTN)SemaphoreBlock;
  mSmmCpuSemaphores.SemaphoreGlobal.Counter       = (UINT32 *)SemaphoreAddr;
  SemaphoreAddr                                  += SemaphoreSize;
  mSmmCpuSemaphores.SemaphoreGlobal.InsideSmm     = (BOOLEAN *)SemaphoreAddr;
  SemaphoreAddr                                  += SemaphoreSize;
  mSmmCpuSemaphores.SemaphoreGlobal.AllCpusInSync = (BOOLEAN *)SemaphoreAddr;
  SemaphoreAddr                                  += SemaphoreSize;
  mSmmCpuSemaphores.SemaphoreGlobal.PFLock        = (SPIN_LOCK *)SemaphoreAddr;
  SemaphoreAddr                                  += SemaphoreSize;
  mSmmCpuSemaphores.SemaphoreGlobal.CodeAccessCheckLock
                 = (SPIN_LOCK *)SemaphoreAddr;
  SemaphoreAddr += SemaphoreSize;

  SemaphoreAddr                          = (UINTN)SemaphoreBlock + GlobalSemaphoresSize;
  mSmmCpuSemaphores.SemaphoreCpu.Busy    = (SPIN_LOCK *)SemaphoreAddr;
  SemaphoreAddr                         += ProcessorCount * SemaphoreSize;
  mSmmCpuSemaphores.SemaphoreCpu.Run     = (UINT32 *)SemaphoreAddr;
  SemaphoreAddr                         += ProcessorCount * SemaphoreSize;
  mSmmCpuSemaphores.SemaphoreCpu.Present = (BOOLEAN *)SemaphoreAddr;

  mPFLock                       = mSmmCpuSemaphores.SemaphoreGlobal.PFLock;
  mConfigSmmCodeAccessCheckLock = mSmmCpuSemaphores.SemaphoreGlobal.CodeAccessCheckLock;

  mSemaphoreSize = SemaphoreSize;
}

/**
  Initialize un-cacheable data.

**/
VOID
EFIAPI
InitializeMpSyncData (
  VOID
  )
{
  UINTN  CpuIndex;

  if (mSmmMpSyncData != NULL) {
    //
    // mSmmMpSyncDataSize includes one structure of SMM_DISPATCHER_MP_SYNC_DATA, one
    // CpuData array of SMM_CPU_DATA_BLOCK and one CandidateBsp array of BOOLEAN.
    //
    ZeroMem (mSmmMpSyncData, mSmmMpSyncDataSize);
    mSmmMpSyncData->CpuData      = (SMM_CPU_DATA_BLOCK *)((UINT8 *)mSmmMpSyncData + sizeof (SMM_DISPATCHER_MP_SYNC_DATA));
    mSmmMpSyncData->CandidateBsp = (BOOLEAN *)(mSmmMpSyncData->CpuData + gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus);
    if (FeaturePcdGet (PcdCpuSmmEnableBspElection)) {
      //
      // Enable BSP election by setting BspIndex to -1
      //
      mSmmMpSyncData->BspIndex = (UINT32)-1;
    }

    mSmmMpSyncData->EffectiveSyncMode = mCpuSmmSyncMode;

    mSmmMpSyncData->Counter       = mSmmCpuSemaphores.SemaphoreGlobal.Counter;
    mSmmMpSyncData->InsideSmm     = mSmmCpuSemaphores.SemaphoreGlobal.InsideSmm;
    mSmmMpSyncData->AllCpusInSync = mSmmCpuSemaphores.SemaphoreGlobal.AllCpusInSync;
    ASSERT (
      mSmmMpSyncData->Counter != NULL && mSmmMpSyncData->InsideSmm != NULL &&
      mSmmMpSyncData->AllCpusInSync != NULL
      );
    *mSmmMpSyncData->Counter       = 0;
    *mSmmMpSyncData->InsideSmm     = FALSE;
    *mSmmMpSyncData->AllCpusInSync = FALSE;

    mSmmMpSyncData->AllApArrivedWithException = FALSE;

    for (CpuIndex = 0; CpuIndex < gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus; CpuIndex++) {
      mSmmMpSyncData->CpuData[CpuIndex].Busy =
        (SPIN_LOCK *)((UINTN)mSmmCpuSemaphores.SemaphoreCpu.Busy + mSemaphoreSize * CpuIndex);
      mSmmMpSyncData->CpuData[CpuIndex].Run =
        (UINT32 *)((UINTN)mSmmCpuSemaphores.SemaphoreCpu.Run + mSemaphoreSize * CpuIndex);
      mSmmMpSyncData->CpuData[CpuIndex].Present =
        (BOOLEAN *)((UINTN)mSmmCpuSemaphores.SemaphoreCpu.Present + mSemaphoreSize * CpuIndex);
      *(mSmmMpSyncData->CpuData[CpuIndex].Busy)    = 0;
      *(mSmmMpSyncData->CpuData[CpuIndex].Run)     = 0;
      *(mSmmMpSyncData->CpuData[CpuIndex].Present) = FALSE;
    }
  }
}

/**
  Initialize global data for MP synchronization.

  @param Stacks             Base address of SMI stack buffer for all processors.
  @param StackSize          Stack size for each processor in SMM.
  @param ShadowStackSize    Shadow Stack size for each processor in SMM.

**/
UINT32
InitializeMpServiceData (
  IN VOID   *Stacks,
  IN UINTN  StackSize,
  IN UINTN  ShadowStackSize
  )
{
  UINT32                          Cr3;
  UINTN                           Index;
  UINT8                           *GdtTssTables;
  UINTN                           GdtTableStepSize;
  CPUID_VERSION_INFO_EDX          RegEdx;
  UINT32                          MaxExtendedFunction;
  CPUID_VIR_PHY_ADDRESS_SIZE_EAX  VirPhyAddressSize;

  //
  // Determine if this CPU supports machine check
  //
  AsmCpuid (CPUID_VERSION_INFO, NULL, NULL, NULL, &RegEdx.Uint32);
  mMachineCheckSupported = (BOOLEAN)(RegEdx.Bits.MCA == 1);

  //
  // Allocate memory for all locks and semaphores
  //
  InitializeSmmCpuSemaphores ();

  //
  // Initialize mSmmMpSyncData
  //
  mSmmMpSyncDataSize = sizeof (SMM_DISPATCHER_MP_SYNC_DATA) +
                       (sizeof (SMM_CPU_DATA_BLOCK) + sizeof (BOOLEAN)) * gSmmCpuPrivate->SmmCoreEntryContext.NumberOfCpus;
  mSmmMpSyncData = (SMM_DISPATCHER_MP_SYNC_DATA *)AllocatePages (EFI_SIZE_TO_PAGES (mSmmMpSyncDataSize));
  ASSERT (mSmmMpSyncData != NULL);
  mCpuSmmSyncMode = (SMM_CPU_SYNC_MODE)PcdGet8 (PcdCpuSmmSyncMode);
  InitializeMpSyncData ();

  //
  // Initialize physical address mask
  // NOTE: Physical memory above virtual address limit is not supported !!!
  //
  AsmCpuid (CPUID_EXTENDED_FUNCTION, &MaxExtendedFunction, NULL, NULL, NULL);
  if (MaxExtendedFunction >= CPUID_VIR_PHY_ADDRESS_SIZE) {
    AsmCpuid (CPUID_VIR_PHY_ADDRESS_SIZE, &VirPhyAddressSize.Uint32, NULL, NULL, NULL);
  } else {
    VirPhyAddressSize.Bits.PhysicalAddressBits = 36;
  }

  gPhyMask = LShiftU64 (1, VirPhyAddressSize.Bits.PhysicalAddressBits) - 1;
  //
  // Clear the low 12 bits
  //
  gPhyMask &= 0xfffffffffffff000ULL;

  //
  // Create page tables
  //
  Cr3 = SmmInitPageTable ();

  GdtTssTables = InitGdt (Cr3, &GdtTableStepSize);

  //
  // Install SMI handler for each CPU
  //
  for (Index = 0; Index < mMaxNumberOfCpus; Index++) {
    InstallSmiHandler (
      Index,
      (UINT32)mCpuHotPlugData.SmBase[Index],
      (VOID *)((UINTN)Stacks + (StackSize + ShadowStackSize) * Index),
      StackSize,
      (UINTN)(GdtTssTables + GdtTableStepSize * Index),
      gcSmiGdtr.Limit + 1,
      gcSmiIdtr.Base,
      gcSmiIdtr.Limit + 1,
      Cr3
      );
  }

  //
  // Record current MTRR settings
  //
  ZeroMem (&gSmiMtrrs, sizeof (gSmiMtrrs));
  MtrrGetAllMtrrs (&gSmiMtrrs);

  return Cr3;
}

/**

  Register the SMM Foundation entry point.

  @param          This              Pointer to EFI_SMM_CONFIGURATION_PROTOCOL instance
  @param          SmmEntryPoint     SMM Foundation EntryPoint

  @retval         EFI_SUCCESS       Successfully to register SMM foundation entry point

**/
EFI_STATUS
EFIAPI
RegisterSmmEntry (
  IN CONST EFI_SMM_CONFIGURATION_PROTOCOL  *This,
  IN EFI_SMM_ENTRY_POINT                   SmmEntryPoint
  )
{
  //
  // Record SMM Foundation EntryPoint, later invoke it on SMI entry vector.
  //
  gSmmCpuPrivate->SmmCoreEntry = SmmEntryPoint;
  return EFI_SUCCESS;
}

/**

  Register the SMM Foundation entry point.

  @param[in]      Procedure            A pointer to the code stream to be run on the designated target AP
                                       of the system. Type EFI_AP_PROCEDURE is defined below in Volume 2
                                       with the related definitions of
                                       EFI_MP_SERVICES_PROTOCOL.StartupAllAPs.
                                       If caller may pass a value of NULL to deregister any existing
                                       startup procedure.
  @param[in,out]  ProcedureArguments   Allows the caller to pass a list of parameters to the code that is
                                       run by the AP. It is an optional common mailbox between APs and
                                       the caller to share information

  @retval EFI_SUCCESS                  The Procedure has been set successfully.
  @retval EFI_INVALID_PARAMETER        The Procedure is NULL but ProcedureArguments not NULL.

**/
EFI_STATUS
RegisterStartupProcedure (
  IN     EFI_AP_PROCEDURE  Procedure,
  IN OUT VOID              *ProcedureArguments OPTIONAL
  )
{
  if ((Procedure == NULL) && (ProcedureArguments != NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (mSmmMpSyncData == NULL) {
    return EFI_NOT_READY;
  }

  mSmmMpSyncData->StartupProcedure = Procedure;
  mSmmMpSyncData->StartupProcArgs  = ProcedureArguments;

  return EFI_SUCCESS;
}
