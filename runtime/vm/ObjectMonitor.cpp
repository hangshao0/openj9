/*******************************************************************************
 * Copyright IBM Corp. and others 2001
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "j9protos.h"
#include "j9consts.h"
#include "j9port.h"
#include "vm_internal.h"
#include "ut_j9vm.h"
#include "thrtypes.h"
#include "stackwalk.h"
#include "objhelp.h"
#include "omrcfg.h"
#include "VMHelpers.hpp"
#include "AtomicSupport.hpp"
#include "ObjectMonitor.hpp"
#include "VMAccess.hpp"

extern "C" {

/* SET_IGNORE_ENTER to avoid reporting enter/exit JLM events for internal artifacts which are not representative of the hold time for the lock */
#if defined(OMR_THR_JLM)
#define SET_IGNORE_ENTER(monitor) ((J9ThreadMonitor*)monitor)->flags |= J9THREAD_MONITOR_IGNORE_ENTER
#define CLEAR_IGNORE_ENTER(monitor) ((J9ThreadMonitor*)monitor)->flags &= ~(UDATA)J9THREAD_MONITOR_IGNORE_ENTER
#else /* OMR_THR_JLM */
#define SET_IGNORE_ENTER(monitor)
#define CLEAR_IGNORE_ENTER(monitor)
#endif /* OMR_THR_JLM */

#define J9VM_SAMPLE_TIMESTAMP_FREQUENCY 1024

static bool
spinOnFlatLock(J9VMThread *currentThread, j9objectmonitor_t volatile *lwEA, j9object_t object);

static bool
spinOnTryEnter(J9VMThread *currentThread, J9ObjectMonitor *objectMonitor, j9objectmonitor_t volatile *lwEA, j9object_t object);

void
monitorExitWriteBarrier()
{
	VM_AtomicSupport::writeBarrier();
}

void
incrementCancelCounter(J9Class *clazz)
{
	VM_ObjectMonitor::incrementCancelCounter(clazz);
}

/*
 * Preserve the blockingEnterObject of the current thread in a special stack frame.
 * If there is no special stack frame already on stack, build one first.
 *
 * @param[in] currentThread the current J9VMThread
 *
 * @return true if a new stack frame was built, false if not
 */
static bool
saveBlockingEnterObject(J9VMThread *currentThread)
{
	bool frameBuilt = false;
	U_8 *pc = currentThread->pc;
	if (!IS_SPECIAL_FRAME_PC(pc)) {
		frameBuilt = true;
		J9SFSpecialFrame *frame = ((J9SFSpecialFrame*)currentThread->sp) - 1;
		frame->specialFrameFlags = 0;
		frame->savedCP = currentThread->literals;
		frame->savedPC = pc;
		frame->savedA0 = (UDATA*)((UDATA)currentThread->arg0EA | J9SF_A0_INVISIBLE_TAG);
		currentThread->literals = NULL;
		currentThread->sp = (UDATA*)frame;
		currentThread->pc = (U_8*)J9SF_FRAME_TYPE_GENERIC_SPECIAL;
		currentThread->arg0EA = (UDATA*)&(frame->savedA0);
	}
	PUSH_OBJECT_IN_SPECIAL_FRAME(currentThread, J9VMTHREAD_BLOCKINGENTEROBJECT(currentThread, currentThread));
	return frameBuilt;
}

/*
 * Restore the blockingEnterObject of the current thread from a special stack frame.
 * Optionally collapse the special stack frame.
 *
 * @param[in] currentThread the current J9VMThread
 * @param[in] collapseFrame true to collapse the special stack frame, false not to
 *
 * @return the restored object
 */
static j9object_t
restoreBlockingEnterObject(J9VMThread *currentThread, bool collapseFrame)
{
	j9object_t object = POP_OBJECT_IN_SPECIAL_FRAME(currentThread);
	J9VMTHREAD_SET_BLOCKINGENTEROBJECT(currentThread, currentThread, object);
	if (collapseFrame) {
		J9SFSpecialFrame *frame = (J9SFSpecialFrame*)currentThread->sp;
		currentThread->literals = frame->savedCP;
		currentThread->pc = frame->savedPC;
		currentThread->arg0EA = (UDATA*)(((UDATA)frame->savedA0) & ~(UDATA)J9SF_A0_INVISIBLE_TAG);
		currentThread->sp = (UDATA*)(frame + 1);
	}
	return object;
}

/*
 * Enter the monitor for the specified object, blocking if necessary.
 *
 * Before blocking, this helper will:
 * 	- report the enter event
 * 	- release VM access
 * After blocking, it will:
 * 	- handle suspend requests
 * 	- report an entered event
 * 	- reacquire VM access
 *
 * @param[in] currentThread the current J9VMThread
 *
 * @return the object
 */
UDATA
objectMonitorEnterBlocking(J9VMThread *currentThread)
{
	Trc_VM_objectMonitorEnterBlocking_Entry(currentThread);
	UDATA result = 0;
	j9object_t object = J9VMTHREAD_BLOCKINGENTEROBJECT(currentThread, currentThread);
	J9Class *ramClass = J9OBJECT_CLAZZ(currentThread, object);
	J9JavaVM *vm = currentThread->javaVM;
	PORT_ACCESS_FROM_JAVAVM(vm);
	I_64 startTicks = 0;
	bool print = false;
	j9object_t object1 = object;
	{
		J9Class * objClass = J9OBJECT_CLAZZ(currentThread, object);
		J9UTF8* currentClassName = J9ROMCLASS_CLASSNAME(objClass->romClass);
		if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(currentClassName), J9UTF8_LENGTH(currentClassName), "java/util/concurrent/ConcurrentHashMap$Node", 43)
		) {
			print = true;
		}

	}

	Assert_VM_true(vm->internalVMFunctions->currentVMThread(vm) == currentThread);

	if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_MONITOR_CONTENDED_ENTERED)) {
		startTicks = j9time_nano_time();
	}
	/* Throughout this function, note that inlineGetLockAddress cannot run into out of memory case because
	 * an entry in monitor table will have been created by the earlier call in objectMonitorEnterNonBlocking.
	 */
#if defined(J9VM_THR_LOCK_RESERVATION)
	{
		j9objectmonitor_t volatile *lwEA = VM_ObjectMonitor::inlineGetLockAddress(currentThread, object);
		while (OBJECT_HEADER_LOCK_RESERVED == (J9_LOAD_LOCKWORD(currentThread, lwEA) & (OBJECT_HEADER_LOCK_RESERVED + OBJECT_HEADER_LOCK_INFLATED))) {
			Trc_VM_objectMonitorEnterBlocking_reservedOnEntry(currentThread);
			cancelLockReservation(currentThread);
			/* calculate the new lock word, since the object may have moved */
			object = J9VMTHREAD_BLOCKINGENTEROBJECT(currentThread, currentThread);
			lwEA = VM_ObjectMonitor::inlineGetLockAddress(currentThread, object);
			if (VM_ObjectMonitor::inlineFastInitAndEnterMonitor(currentThread, lwEA)) {
				if (print) {
					Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 51, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
				}
				goto success;
			}
		}
	}
#endif /* J9VM_THR_LOCK_RESERVATION */
	{
		J9ObjectMonitor *objectMonitor = monitorTableAt(currentThread, object);
		/* Table entry was created by the nonblocking case, so this peek cannot fail */
		Assert_VM_notNull(objectMonitor);
		object = NULL; // for safety, since object may be moved by the GC at various points after this
		/* Ensure object monitor isn't deflated while we block */
		omrthread_monitor_t monitor = objectMonitor->monitor;
		J9VMThread *previousOwner = NULL;
		omrthread_t previousOMRThreadOwner = ((J9ThreadMonitor *)monitor)->owner;
		VM_AtomicSupport::add(&monitor->pinCount, 1);
		/* Initialize our wait time to 1ms. Increase it as we have to wait more and more
		 * using the sequence 1, 4, 16, 64 and then 64 thereafter.
		 */
		IDATA waitTime = 1;

		if ((NULL != previousOMRThreadOwner) && !IS_J9_OBJECT_MONITOR_OWNER_DETACHED(previousOMRThreadOwner)) {
			previousOwner = getVMThreadFromOMRThread(vm, previousOMRThreadOwner);
		}

		if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_MONITOR_CONTENDED_ENTER)) {
			bool frameBuilt = saveBlockingEnterObject(currentThread);
			VM_VMAccess::setPublicFlags(currentThread, J9_PUBLIC_FLAGS_THREAD_BLOCKED);
			ALWAYS_TRIGGER_J9HOOK_VM_MONITOR_CONTENDED_ENTER(vm->hookInterface, currentThread, monitor);
			restoreBlockingEnterObject(currentThread, frameBuilt);
		}
		omrthread_t const osThread = currentThread->osThread;
		/* Update j.l.management info */
		currentThread->mgmtBlockedCount += 1;
		if (J9_ARE_ALL_BITS_SET(currentThread->publicFlags, J9_PUBLIC_FLAGS_THREAD_BLOCKED)) {
			internalReleaseVMAccess(currentThread);
			goto releasedAccess;
		}
restart:
		internalReleaseVMAccessSetStatus(currentThread, J9_PUBLIC_FLAGS_THREAD_BLOCKED);
releasedAccess:
		omrthread_monitor_enter_using_threadId(monitor, osThread);
#if defined(J9VM_THR_SMART_DEFLATION)
		/* Update the anti-deflation vote because we had to block */
		objectMonitor->antiDeflationCount += 1;
#endif /* J9VM_THR_SMART_DEFLATION */
		for (;;) {
			/* if the INFLATED bit is set, then we own the object and can safely block in acquireVMAccess */
			if (J9_ARE_ANY_BITS_SET(((J9ThreadMonitor*)monitor)->flags, J9THREAD_MONITOR_INFLATED)) {
				Trc_VM_objectMonitorEnterBlocking_alreadyInflated(currentThread);
				if (print) {
					j9objectmonitor_t volatile *lwEA = VM_ObjectMonitor::inlineGetLockAddress(currentThread, object1);
					Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 30, object1, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
				}
				internalAcquireVMAccess(currentThread);
#if JAVA_SPEC_VERSION >= 19
				currentThread->ownedMonitorCount += 1;
#endif /* JAVA_SPEC_VERSION >= 19 */
				goto done;
			}
			if (0 != internalTryAcquireVMAccess(currentThread)) {
				Trc_VM_objectMonitorEnterBlocking_tryAcquireVMAccessFailed(currentThread);
				SET_IGNORE_ENTER(monitor);
				omrthread_monitor_exit_using_threadId(monitor, osThread);
				internalAcquireVMAccess(currentThread);
				goto restart;
			}
			/* In a Concurrent GC where monitor object can *move* in a middle of GC cycle,
			 * we need a proper barrier to get an up-to-date location of the monitor object */
			j9objectmonitor_t volatile *lwEA = VM_ObjectMonitor::inlineGetLockAddress(currentThread, J9WEAKROOT_OBJECT_LOAD(currentThread, &((J9ThreadMonitor*)monitor)->userData));
			j9objectmonitor_t lockInLoop = J9_LOAD_LOCKWORD(currentThread, lwEA);
			/* Change lockword from Learning to Flat if in Learning state. */
			while (OBJECT_HEADER_LOCK_LEARNING == (lockInLoop & (OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_INFLATED))) {
				j9objectmonitor_t newFlatLock;
				if (0 == J9_FLATLOCK_COUNT(lockInLoop)) {
					/* Lock is currently unlocked so convert to Flat-Unlocked which is all 0s. */
					newFlatLock = (j9objectmonitor_t)(UDATA)0;
				} else {
					/* Clears the Learning bit to convert the lock to Flat state.
					 * First clears the bits under OBJECT_HEADER_LOCK_RECURSION_MASK then fills in the new RC.
					 * This is done to reclaim the bits used for the learning counter that are no longer used under Flat.
					 * For the same logical recursion count, the value encoded in the RC bits is one lower under
					 * Flat compared to Learning so the new RC field is one lower than before the conversion to flat. */
					newFlatLock = (lockInLoop & ~(j9objectmonitor_t)(OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_RECURSION_MASK)) |
					              ((J9_FLATLOCK_COUNT(lockInLoop) - 1) << OBJECT_HEADER_LOCK_V2_RECURSION_OFFSET);
				}

				j9objectmonitor_t const oldValue = lockInLoop;
				lockInLoop = VM_ObjectMonitor::compareAndSwapLockword(currentThread, lwEA, lockInLoop, newFlatLock);
				if (oldValue == lockInLoop) {
					/* Transition from Learning to Flat occurred so the Cancel Counter in the object's J9Class is incremented by 1. */
					VM_ObjectMonitor::incrementCancelCounter(ramClass);
					break;
				}
			}
			lockInLoop = J9_LOAD_LOCKWORD(currentThread, lwEA);
#if defined(J9VM_THR_LOCK_RESERVATION)
			/* has the lock become reserved? */
			/* TODO: Use lockInLoop instead of another read? */
			if (OBJECT_HEADER_LOCK_RESERVED == (J9_LOAD_LOCKWORD(currentThread, lwEA) & (OBJECT_HEADER_LOCK_RESERVED + OBJECT_HEADER_LOCK_INFLATED))) {
				Trc_VM_objectMonitorEnterBlocking_reservedInLoop(currentThread);
				SET_IGNORE_ENTER(monitor);
				omrthread_monitor_exit_using_threadId(monitor, osThread);
				cancelLockReservation(currentThread);
				goto restart;
			}
#endif /* J9VM_THR_LOCK_RESERVATION */
			/* Ensure FLC bit is set */
			if (J9_ARE_NO_BITS_SET(lockInLoop, OBJECT_HEADER_LOCK_FLC)) {
				while (0 != lockInLoop) {
					j9objectmonitor_t const flcBitSet = lockInLoop | OBJECT_HEADER_LOCK_FLC;
					j9objectmonitor_t const oldValue = lockInLoop;
					if (print) {
						Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 20, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
					}
					lockInLoop = VM_ObjectMonitor::compareAndSwapLockword(currentThread, lwEA, lockInLoop, flcBitSet);
					if (oldValue == lockInLoop) {
						if (print) {
							Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 21, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
						}
						break;
					} else {
						if (print) {
							Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 22, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
						}
					}
				}
			}
			if (VM_ObjectMonitor::inlineFastInitAndEnterMonitor(currentThread, lwEA, true)) {
				/* this thread now owns the thin lock - release the inflated lock */
				Trc_VM_objectMonitorEnterBlocking_gotFlatLockInLoop(currentThread);
				SET_IGNORE_ENTER(monitor);
				omrthread_monitor_exit_using_threadId(monitor, osThread);
				/* this path is fairly unlikely, and there is a good chance that
				 * other threads are behind us in the queue. Set the FLC bit to
				 * make sure that this thread signals them when it is done.
				 */
				j9objectmonitor_t lock = J9_LOAD_LOCKWORD(currentThread, lwEA);
				lock |= OBJECT_HEADER_LOCK_FLC;
				J9_STORE_LOCKWORD(currentThread, lwEA, lock);
				if (print) {
					Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 31, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
				}
				goto done;
			}
			internalReleaseVMAccessSetStatus(currentThread, J9_PUBLIC_FLAGS_THREAD_BLOCKED);
			SET_IGNORE_ENTER(monitor);
			omrthread_monitor_wait_timed(monitor, (I_64)waitTime, 0);
			CLEAR_IGNORE_ENTER(monitor);
			/* increase wait time */
			if (64 != waitTime) {
				waitTime <<= 2;
			}
		}
done:
		clearEventFlag(currentThread, J9_PUBLIC_FLAGS_THREAD_BLOCKED);
		/* Clear the SUPPRESS_CONTENDED_EXITS bit in the monitor saying that CONTENDED EXIT can be sent again */
		((J9ThreadMonitor*)monitor)->flags &= ~(UDATA)J9THREAD_MONITOR_SUPPRESS_CONTENDED_EXIT;
		VM_AtomicSupport::subtract(&monitor->pinCount, 1);
		if (J9_EVENT_IS_HOOKED(vm->hookInterface, J9HOOK_VM_MONITOR_CONTENDED_ENTERED)) {
			bool frameBuilt = saveBlockingEnterObject(currentThread);
			ALWAYS_TRIGGER_J9HOOK_VM_MONITOR_CONTENDED_ENTERED(vm->hookInterface, currentThread, monitor, startTicks, ramClass, previousOwner);
			restoreBlockingEnterObject(currentThread, frameBuilt);
		}
	}
success:
	if (print) {
		j9objectmonitor_t volatile *lwEA = VM_ObjectMonitor::inlineGetLockAddress(currentThread, object1);
		Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 32, object1, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
	}
	result = (IDATA)(UDATA)J9VMTHREAD_BLOCKINGENTEROBJECT(currentThread, currentThread);
	J9VMTHREAD_SET_BLOCKINGENTEROBJECT(currentThread, currentThread, NULL);
	Trc_VM_objectMonitorEnterBlocking_Exit(currentThread, result);
	return result;
}

/*
 * Enter the monitor for the specified object without blocking.  If blocking
 * would be required, store the object in the blockingEnterObject field of
 * the current thread.
 *
 * @param[in] currentThread the current J9VMThread
 * @param[in] object the object for which to enter the monitor
 *
 * @return
 * 	object on success
 * 	1 if blocking is necessary
 * 	0 if out of memory
 */
UDATA
objectMonitorEnterNonBlocking(J9VMThread *currentThread, j9object_t object)
{
	UDATA result = (UDATA)object;
	J9JavaVM *vm = currentThread->javaVM;
	j9objectmonitor_t volatile *lwEA = VM_ObjectMonitor::inlineGetLockAddress(currentThread, object);
#if JAVA_SPEC_VERSION >= 16
	J9Class * objClass = J9OBJECT_CLAZZ(currentThread, object);
#endif /* JAVA_SPEC_VERSION >= 16 */
#if defined(J9VM_OPT_CRIU_SUPPORT)
	BOOLEAN retry = FALSE;
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */

	bool print = false;

	{
		J9UTF8* currentClassName = J9ROMCLASS_CLASSNAME(objClass->romClass);
		if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(currentClassName), J9UTF8_LENGTH(currentClassName), "java/util/concurrent/ConcurrentHashMap$Node", 43)
		) {
			print = true;
		}
	}

#if defined(J9VM_OPT_VALHALLA_VALUE_TYPES)
	if (J9_IS_J9CLASS_VALUETYPE(objClass)) {
		result = J9_OBJECT_MONITOR_VALUE_TYPE_IMSE;
		goto done;
	}
#endif /* J9VM_OPT_VALHALLA_VALUE_TYPES */
#if JAVA_SPEC_VERSION >= 16
	if (J9_IS_J9CLASS_VALUEBASED(objClass)) {
		U_32 runtimeFlags2 = vm->extendedRuntimeFlags2;
		if (J9_ARE_ALL_BITS_SET(runtimeFlags2, J9_EXTENDED_RUNTIME2_VALUE_BASED_EXCEPTION)) {
			result = J9_OBJECT_MONITOR_VALUE_TYPE_IMSE;
			goto done;
		} else if (J9_ARE_ALL_BITS_SET(runtimeFlags2, J9_EXTENDED_RUNTIME2_VALUE_BASED_WARNING)) {
			PORT_ACCESS_FROM_VMC(currentThread);
			const J9UTF8* className = J9ROMCLASS_CLASSNAME(J9OBJECT_CLAZZ(currentThread, object)->romClass);
			j9nls_printf(PORTLIB, J9NLS_WARNING, J9NLS_VM_ERROR_BYTECODE_OBJECTREF_CANNOT_BE_VALUE_BASED, J9UTF8_LENGTH(className), J9UTF8_DATA(className));
		}
	}
#endif /* JAVA_SPEC_VERSION >= 16 */

	Assert_VM_true(vm->internalVMFunctions->currentVMThread(vm) == currentThread);

restart:
	if (NULL == lwEA) {
		/* out of memory */
		result = J9_OBJECT_MONITOR_OOM;
		goto done;
	} else {
		/* check for a recursive flat lock */
		j9objectmonitor_t const lock = J9_LOAD_LOCKWORD(currentThread, lwEA);

		if (print) {
			Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 0, object, currentThread->ownedMonitorCount, lock);
		}

		/* Global Lock Reservation is currently only supported on Power. Learning bit should be clear on other platforms if Inflated bit is not set. */
#if !(defined(AIXPPC) || defined(LINUXPPC))
		Assert_VM_false(OBJECT_HEADER_LOCK_LEARNING == (lock & (OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_INFLATED)));
#endif /* !(defined(AIXPPC) || defined(LINUXPPC)) */

		/* try incrementing first to ensure that we won't overflow the recursion counter */
		j9objectmonitor_t incremented = lock + ((lock & OBJECT_HEADER_LOCK_LEARNING) ? OBJECT_HEADER_LOCK_LEARNING_FIRST_RECURSION_BIT : OBJECT_HEADER_LOCK_FIRST_RECURSION_BIT);
		if (J9_FLATLOCK_OWNER(incremented) == currentThread) {
			/*
			 * Lock can potentially be Flat, Reserved or Learning.
			 * In all three cases it is confirmed that the RC count will not overflow after being incremented and so the lock will not inflate.
			 * In the Flat case, this is also guaranteed to be a nested lock.
			 * In the Reserved and Learning case, this can be either a non-nested or nested lock.
			 */
			if (0 == (lock & OBJECT_HEADER_LOCK_LEARNING)) {
				/* Flat and Reserved state case */
				J9_STORE_LOCKWORD(currentThread, lwEA, incremented);
			} else {
				/*
				 * Learning state case
				 * The Learning Counter is only used in the Learning state.
				 * It starts at 0 the first time an object is locked. If the same thread locks the object again, the counter is incremented.
				 * When the Learning Counter reaches reservedTransitionThreshold, the object transitions from Learning to Reserved.
				 * reservedTransitionThreshold is being checked against a 2 bit value so values of 4 or higher will prevent transitions to Reserved.
				 */
				U_32 reservedTransitionThreshold = vm->reservedTransitionThreshold;
				bool reservedTransition = false;
				if (((lock & OBJECT_HEADER_LOCK_LEARNING_LC_MASK) >> OBJECT_HEADER_LOCK_LEARNING_LC_OFFSET) >= reservedTransitionThreshold) {
					/*
					 * Clears the Learning bit and sets the Reserved bit to change the state to Reserved.
					 * Clears the bits under OBJECT_HEADER_LOCK_RECURSION_MASK then fills in the new RC.
					 * This is done to reclaim the bits used for the Learning Counter that are no longer used under Reserved.
					 */
					incremented = (incremented & ~(j9objectmonitor_t)(OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_RECURSION_MASK)) |
					              (J9_FLATLOCK_COUNT(incremented) << OBJECT_HEADER_LOCK_V2_RECURSION_OFFSET) |
					              OBJECT_HEADER_LOCK_RESERVED;
					reservedTransition = true;
				} else {
					/* LC field overflow check. Don't increment if it will overflow the counter. */
					if ((lock & OBJECT_HEADER_LOCK_LEARNING_LC_MASK) != OBJECT_HEADER_LOCK_LEARNING_LC_MASK) {
						incremented = incremented + OBJECT_HEADER_LOCK_LEARNING_FIRST_LC_BIT;
					}
				}

				if (lock == VM_ObjectMonitor::compareAndSwapLockword(currentThread, lwEA, lock, incremented, false)) {
					if (0 == J9_FLATLOCK_COUNT(lock)) {
						VM_AtomicSupport::readBarrier();
					}
					if (reservedTransition) {
						/* Transition from Learning to Reserved occurred so the Reserved Counter in the object's J9Class is incremented by 1. */
						VM_ObjectMonitor::incrementReservedCounter(J9OBJECT_CLAZZ(currentThread, object));
					}
				} else {
					/*
					 * Another thread has attempted to get the lock and atomically changed the state to Flat.
					 * Start over and read the new lockword. The lock can not go back to Learning so this will
					 * only happen once.
					 */
					goto restart;
				}
			}
#if JAVA_SPEC_VERSION >= 19
			currentThread->ownedMonitorCount += 1;
#endif /* JAVA_SPEC_VERSION >= 19 */
			/* no barrier is required in the recursive case */
		} else {
			/* check to see if object is unlocked (JIT did not do initial inline sequence due to insufficient static type info) */
			if (J9_ARE_NO_BITS_SET(lock, ~(j9objectmonitor_t)(OBJECT_HEADER_LOCK_RESERVED | OBJECT_HEADER_LOCK_LEARNING)) &&
				VM_ObjectMonitor::inlineFastInitAndEnterMonitor(currentThread, lwEA, false, lock)
			) {
				/* compare and swap succeeded - barrier already performed */
				if (lock == OBJECT_HEADER_LOCK_RESERVED) {
					/* Object in New-AutoReserve was locked so the Reserved Counter in the object's J9Class is incremented by 1. */
					VM_ObjectMonitor::incrementReservedCounter(J9OBJECT_CLAZZ(currentThread, object));
				}
			} else {
				J9ObjectMonitor *objectMonitor = NULL;
				/* check if the monitor is flat */
				if (!J9_LOCK_IS_INFLATED(lock)) {
					/* lock is flat, am I the owner? */
					if (J9_FLATLOCK_OWNER(lock) == currentThread) {
						/* Convert Learning to Flat instead Inflated for dealing with RC overflow.*/
						if (OBJECT_HEADER_LOCK_LEARNING == (lock & (OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_INFLATED))) {
							j9objectmonitor_t newFlatLock;
							if (0 == J9_FLATLOCK_COUNT(lock)) {
								/* Lock is currently unlocked so convert to Flat unlocked which is all 0s. */
								newFlatLock = (j9objectmonitor_t)(UDATA)0;
							} else {
								/*
								 * Clears the Learning bit to convert the lock to Flat state.
								 * First clears the bits under OBJECT_HEADER_LOCK_RECURSION_MASK then fills in the new RC.
								 * This is done to reclaim the bits used for the learning counter that are no longer used under Flat.
								 * For the same logical recursion count, the value encoded in the RC bits is one lower under
								 * Flat compared to Learning so the new RC field is one lower than before the conversion to flat.
								 */
								newFlatLock = (lock & ~(j9objectmonitor_t)(OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_RECURSION_MASK)) |
								              ((J9_FLATLOCK_COUNT(lock) - 1) << OBJECT_HEADER_LOCK_V2_RECURSION_OFFSET);
							}

							/* If CAS fails, some other thread changed the lock from Learning to Flat so the code needs to start over anyways. */
							if (lock == VM_ObjectMonitor::compareAndSwapLockword(currentThread, lwEA, lock, newFlatLock)) {
								/* Transition from Learning to Flat occurred so the Cancel Counter in the object's J9Class is incremented by 1. */
								VM_ObjectMonitor::incrementCancelCounter(J9OBJECT_CLAZZ(currentThread, object));
							}
							goto restart;
						}

						/* recursive flat lock - count is full */
						objectMonitor = objectMonitorInflate(currentThread, object, lock);
						if (NULL == objectMonitor) {
							/* out of memory */
							result = J9_OBJECT_MONITOR_OOM;
							goto done;
						}
					} else {
						/* lock is flat, but contended, spin on it for a while */
						if (spinOnFlatLock(currentThread, lwEA, object)) {
							goto done;
						}
						/* Preemptively create the object monitor but do not assign it.
						 * This makes it impossible to fail in objectMonitorEnterBlocking.
						 * This only affects reserved locks, as the non-reserved path in
						 * the blocking code immediately calls monitorTableAt.
						 */
						if (NULL == monitorTableAt(currentThread, object)) {
							/* out of memory */
							result = J9_OBJECT_MONITOR_OOM;
							goto done;
						}
						goto wouldBlock;
					}
				} else {
					/* the monitor is already inflated */
					objectMonitor = J9_INFLLOCK_OBJECT_MONITOR(lock);
				}
				if (!spinOnTryEnter(currentThread, objectMonitor, lwEA, object)) {
					goto wouldBlock;
				}
			}
		}
	}
	goto done;
wouldBlock:
	/* unable to get thin lock by spinning - follow blocking path */
	J9VMTHREAD_SET_BLOCKINGENTEROBJECT(currentThread, currentThread, object);
#if defined(J9VM_OPT_CRIU_SUPPORT)
	if (J9_THROW_BLOCKING_EXCEPTION_IN_SINGLE_THREAD_MODE(vm)) {
		if (OBJECT_HEADER_LOCK_RESERVED == (J9_LOAD_LOCKWORD(currentThread, lwEA) & (OBJECT_HEADER_LOCK_RESERVED + OBJECT_HEADER_LOCK_INFLATED)) && !retry) {
			cancelLockReservation(currentThread);
			retry = TRUE;
			goto restart;
		}
		result = J9_OBJECT_MONITOR_CRIU_SINGLE_THREAD_MODE_THROW;
	} else
#endif /* defined(J9VM_OPT_CRIU_SUPPORT) */
	{
		result = J9_OBJECT_MONITOR_BLOCKING;
	}
done:
	if (result == (UDATA)object) {
		//J9UTF8* currentClassName = J9ROMCLASS_CLASSNAME(objClass->romClass);
		//if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(currentClassName), J9UTF8_LENGTH(currentClassName), "java/util/concurrent/ConcurrentHashMap$Node", 43)) {
		if (print) {
			Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 1, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
		}
	} else {
		if (print) {
			Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 2, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
		}
	}
	return result;
}

/**
 * Spin on a flat lock
 *
 * @param currentThread[in] the current J9VMThread
 * @param lwEA[in] the location of the lockword
 * @param object[in] the object to which the lockword belongs to
 *
 * @returns	true if lock acquired, false if failed to acquire lock
 */
static bool
spinOnFlatLock(J9VMThread *currentThread, j9objectmonitor_t volatile *lwEA, j9object_t object)
{
	bool rc = false;
	bool nestedPath = true;
	J9JavaVM *vm = currentThread->javaVM;
	UDATA spinCount2 = vm->thrMaxSpins2BeforeBlocking;
	UDATA yieldCount = vm->thrMaxYieldsBeforeBlocking;
	UDATA const nestedSpinning = vm->thrNestedSpinning;

	bool print = false;
	{
		J9Class * objClass = J9OBJECT_CLAZZ(currentThread, object);
		J9UTF8* currentClassName = J9ROMCLASS_CLASSNAME(objClass->romClass);
		if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(currentClassName), J9UTF8_LENGTH(currentClassName), "java/util/concurrent/ConcurrentHashMap$Node", 43)
		) {
			print = true;
		}
	}

#if defined(J9VM_INTERP_CUSTOM_SPIN_OPTIONS)
	J9Class *ramClass = J9OBJECT_CLAZZ(currentThread, object);
	J9VMCustomSpinOptions *option = ramClass->customSpinOption;
	UDATA spinCount1 = vm->thrMaxSpins1BeforeBlocking;

	/* Use custom spin options if provided */
	if (NULL != option) {
		const J9ObjectMonitorCustomSpinOptions *const j9monitorOptions = &option->j9monitorOptions;
		spinCount1 = j9monitorOptions->thrMaxSpins1BeforeBlocking;
		spinCount2 = j9monitorOptions->thrMaxSpins2BeforeBlocking;
		yieldCount = j9monitorOptions->thrMaxYieldsBeforeBlocking;

		Trc_VM_MonitorEnterNonBlocking_CustomSpinOption_Set1(option->className,
				object,
				spinCount1,
				spinCount2,
				yieldCount);
	}
#else /* J9VM_INTERP_CUSTOM_SPIN_OPTIONS */
	UDATA const spinCount1 = vm->thrMaxSpins1BeforeBlocking;
#endif /* J9VM_INTERP_CUSTOM_SPIN_OPTIONS */

	j9objectmonitor_t bits = OBJECT_HEADER_LOCK_FLC + OBJECT_HEADER_LOCK_INFLATED;
#if defined(J9VM_THR_LOCK_RESERVATION)
	bits += OBJECT_HEADER_LOCK_RESERVED;
#endif

	for (UDATA _yieldCount = yieldCount; _yieldCount > 0; _yieldCount--) {
		for (UDATA _spinCount2 = spinCount2; _spinCount2 > 0; _spinCount2--) {
			/* try to take the flat monitor by swapping the currentThread in */
			if (VM_ObjectMonitor::inlineFastInitAndEnterMonitor(currentThread, lwEA, true)) {
				/* compare and swap succeeded - barrier already performed */
				if (print) {
					Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 61, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
				}
				rc = true;
				goto done;
			}
			/* do not spin if the FLC, inflated or reserved bits are already set */
			j9objectmonitor_t const lock = J9_LOAD_LOCKWORD(currentThread, lwEA);
			if (J9_ARE_NO_BITS_SET(lock, bits) && J9_ARE_NO_BITS_SET(currentThread->publicFlags, J9_PUBLIC_FLAGS_HALT_THREAD_EXCLUSIVE)) {
				/* If the Learning bit is set, need to handle Learning state. */
				if (0 != (lock & OBJECT_HEADER_LOCK_LEARNING)) {
					/* Check if RC is 0, if so it is possible to just atomically lock the object now. */
					if (0 == J9_FLATLOCK_COUNT(lock)) {
						if (lock == VM_ObjectMonitor::compareAndSwapLockword(currentThread, lwEA, lock, (j9objectmonitor_t)(UDATA)currentThread, false)) {
							/* compare and swap succeeded */
							VM_AtomicSupport::readBarrier();
#if JAVA_SPEC_VERSION >= 19
							currentThread->ownedMonitorCount += 1;
#endif /* JAVA_SPEC_VERSION >= 19 */
							if (print) {
								Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 62, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
							}
							rc = true;

							/* Transition from Learning to Flat occurred so the Cancel Counter in the object's J9Class is incremented by 1. */
							VM_ObjectMonitor::incrementCancelCounter(J9OBJECT_CLAZZ(currentThread, object));
							goto done;
						}
					} else {
						/*
						 * Attempt to atomically change the lock for Learning to Flat. Try again next iteration if this doesn't work.
						 * Clears the Learning bit to convert the lock to Flat state.
						 * Clears the bits under OBJECT_HEADER_LOCK_RECURSION_MASK then fills in the new RC.
						 * This is done to reclaim the bits used for the learning counter that are no longer used under Flat.
						 * For the same logical recursion count, the value encoded in the RC bits is one lower under
						 * Flat compared to Learning so the new RC field is one lower than before the conversion to flat.
						 */
						j9objectmonitor_t const newLock = (lock & ~(j9objectmonitor_t)(OBJECT_HEADER_LOCK_LEARNING | OBJECT_HEADER_LOCK_RECURSION_MASK)) |
						                                  ((J9_FLATLOCK_COUNT(lock) - 1) << OBJECT_HEADER_LOCK_V2_RECURSION_OFFSET);

						if (lock == VM_ObjectMonitor::compareAndSwapLockword(currentThread, lwEA, lock, newLock, false)) {
							/* Transition from Learning to Flat occurred so the Cancel Counter in the object's J9Class is incremented by 1. */
							VM_ObjectMonitor::incrementCancelCounter(J9OBJECT_CLAZZ(currentThread, object));
						}
					}
				}

				if (nestedPath) {
					VM_AtomicSupport::yieldCPU();
					VM_AtomicSupport::dropSMTThreadPriority();
					for (UDATA _spinCount1 = spinCount1; _spinCount1 > 0; _spinCount1--) {
						VM_AtomicSupport::nop();
					} /* end tight loop */
					VM_AtomicSupport::restoreSMTThreadPriority();
				}
			} else {
				goto done;
			}
		}
		if ((0 == nestedSpinning) && nestedPath) {
			//only execute second inner for loop once and do not execute third inner for loop
			spinCount2 = 1;
			nestedPath = false;
		}
#if defined(OMR_THR_YIELD_ALG)
		omrthread_yield_new(yieldCount - _yieldCount);
#else /* OMR_THR_YIELD_ALG */
		omrthread_yield();
#endif /* OMR_THR_YIELD_ALG */
	}

done:
	return rc;
}

/**
 * Spin on try enter
 *
 * @param currentThread[in] the current J9VMThread
 * @param objectMonitor[in] object monitor which is being acquired
 * @param lwEA[in] the location of the lockword
 *
 * @returns	true if lock acquired, false if failed to acquire lock
 */
static bool
spinOnTryEnter(J9VMThread *currentThread, J9ObjectMonitor *objectMonitor, j9objectmonitor_t volatile *lwEA, j9object_t object)
{
	J9JavaVM *vm = currentThread->javaVM;
	omrthread_t const osThread = currentThread->osThread;
	omrthread_monitor_t monitor = objectMonitor->monitor;
	omrthread_library_t const lib = osThread->library;

	bool nestedPath = true;
	bool rc = false;
	IDATA rc_tryEnterUsingThreadID = 0;

	UDATA tryEnterSpinCount2 = vm->thrMaxTryEnterSpins2BeforeBlocking;
	UDATA tryEnterYieldCount = vm->thrMaxTryEnterYieldsBeforeBlocking;
	UDATA const tryEnterNestedSpinning = vm->thrTryEnterNestedSpinning;

	bool print = false;
	{
		J9Class * objClass = J9OBJECT_CLAZZ(currentThread, object);
		J9UTF8* currentClassName = J9ROMCLASS_CLASSNAME(objClass->romClass);
		if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(currentClassName), J9UTF8_LENGTH(currentClassName), "java/util/concurrent/ConcurrentHashMap$Node", 43)
		) {
			print = true;
		}
	}

#if defined(J9VM_INTERP_CUSTOM_SPIN_OPTIONS)
	J9Class *ramClass = J9OBJECT_CLAZZ(currentThread, object);
	J9VMCustomSpinOptions *option = ramClass->customSpinOption;
	UDATA tryEnterSpinCount1 = vm->thrMaxTryEnterSpins1BeforeBlocking;

	if (NULL != option) {
		const J9ObjectMonitorCustomSpinOptions *const j9monitorOptions = &option->j9monitorOptions;
		tryEnterSpinCount1 = j9monitorOptions->thrMaxTryEnterSpins1BeforeBlocking;
		tryEnterSpinCount2 = j9monitorOptions->thrMaxTryEnterSpins2BeforeBlocking;
		tryEnterYieldCount = j9monitorOptions->thrMaxTryEnterYieldsBeforeBlocking;

		Trc_VM_MonitorEnterNonBlocking_CustomSpinOption_Set2(option->className,
				object,
				tryEnterSpinCount1,
				tryEnterSpinCount2,
				tryEnterYieldCount);
	}
#else /* J9VM_INTERP_CUSTOM_SPIN_OPTIONS */
	UDATA const tryEnterSpinCount1 = vm->thrMaxTryEnterSpins1BeforeBlocking;
#endif /* J9VM_INTERP_CUSTOM_SPIN_OPTIONS */

#if defined(OMR_THR_JLM)
	/* Initialize JLM */
	J9ThreadMonitorTracing *tracing = NULL;
	if (J9_ARE_ALL_BITS_SET(lib->flags, J9THREAD_LIB_FLAG_JLM_ENABLED)) {
		tracing = ((J9ThreadMonitor*)monitor)->tracing;
	}
#endif /* OMR_THR_JLM */

#if defined(OMR_THR_THREE_TIER_LOCKING) && defined(OMR_THR_SPIN_WAKE_CONTROL)
	bool tryEnterSpin = true;
	if (OMRTHREAD_IGNORE_SPIN_THREAD_BOUND != lib->maxSpinThreads) {
		if (monitor->spinThreads < lib->maxSpinThreads) {
			VM_AtomicSupport::add(&monitor->spinThreads, 1);
		} else {
			tryEnterSpinCount1 = 1;
			tryEnterSpinCount2 = 1;
			tryEnterYieldCount = 1;
			tryEnterSpin = false;
		}
	}
#endif /* defined(OMR_THR_THREE_TIER_LOCKING) && defined(OMR_THR_SPIN_WAKE_CONTROL) */

	/* Need to store the original value of tryEnterSpinCount2 since it gets overridden during non-nested spinning */
	UDATA tryEnterSpinCount2Init = tryEnterSpinCount2;

	UDATA _tryEnterYieldCount = tryEnterYieldCount;
	UDATA _tryEnterSpinCount2 = tryEnterSpinCount2;

	/* we have the monitor object from the lock word so prime the cache with the monitor so we do not later look it up from the monitor table */
	cacheObjectMonitorForLookup(vm, currentThread, objectMonitor);

	for (; _tryEnterYieldCount > 0; _tryEnterYieldCount--) {
		for (_tryEnterSpinCount2 = tryEnterSpinCount2; _tryEnterSpinCount2 > 0; _tryEnterSpinCount2--) {
			rc_tryEnterUsingThreadID = omrthread_monitor_try_enter_using_threadId(monitor, osThread);
			if (0 == rc_tryEnterUsingThreadID) {
#if defined(J9VM_THR_SMART_DEFLATION)
				/* Update the monitor's pro deflation vote because we got in without blocking */
				if (VM_AtomicSupport::sampleTimestamp(J9VM_SAMPLE_TIMESTAMP_FREQUENCY)) {
					objectMonitor->proDeflationCount += 1;
				}
#endif /* J9VM_THR_SMART_DEFLATION */
				if (J9_LOCK_IS_INFLATED(J9_LOAD_LOCKWORD(currentThread, lwEA))) {
					/* try_enter succeeded - monitor is inflated */
					rc = true;
#if JAVA_SPEC_VERSION >= 19
					currentThread->ownedMonitorCount += 1;
					if (print) {
						Trc_VM_MonitorEnterNonBlocking_Entered(currentThread, 63, object, currentThread->ownedMonitorCount, J9_LOAD_LOCKWORD(currentThread, lwEA));
					}
#endif /* JAVA_SPEC_VERSION >= 19 */
				} else {
					/* try_enter succeeded - monitor is not inflated - would block */
					SET_IGNORE_ENTER(monitor);
					omrthread_monitor_exit_using_threadId(monitor, osThread);
				}
				goto update_jlm;
			}
#if defined(OMR_THR_ADAPTIVE_SPIN)
			/* check if spinning has been disabled, if so go directly to the blocking path */
			if (J9_ARE_ALL_BITS_SET(monitor->flags, J9THREAD_MONITOR_DISABLE_SPINNING)) {
				goto update_jlm;
			}
#endif /* OMR_THR_ADAPTIVE_SPIN */
			if (J9_ARE_ALL_BITS_SET(currentThread->publicFlags, J9_PUBLIC_FLAGS_HALT_THREAD_EXCLUSIVE)) {
				goto update_jlm;
			}
			if (nestedPath) {
				VM_AtomicSupport::yieldCPU();
				for (UDATA _tryEnterSpinCount1 = tryEnterSpinCount1; _tryEnterSpinCount1 > 0; _tryEnterSpinCount1--) {
					VM_AtomicSupport::nop();
				} /* end tight loop */
			}
		}
		if ((0 == tryEnterNestedSpinning) && nestedPath) {
			tryEnterSpinCount2 = 1;
			nestedPath = false;
		}
#if defined(OMR_THR_YIELD_ALG)
		omrthread_yield_new(tryEnterYieldCount - _tryEnterYieldCount);
#else /* OMR_THR_YIELD_ALG */
		omrthread_yield();
#endif /* OMR_THR_YIELD_ALG */
	}

update_jlm:
#if defined(OMR_THR_JLM)
	if (NULL != tracing) {
		/* Add JLM counts atomically:
		 * JLM: let m=tryEnterYieldCount, n=tryEnterSpinCount2
		 * JLM: let i=_tryEnterSpinCount2, j=_tryEnterYieldCount
		 *
		 * JLM: after partial set of spins
		 * JLM: with non-nested spinning -> yield_count += (m-j), spin2_count += (m-j)+(n-i+1)
		 * JLM: with nested spinning -> yield_count += m, spin2_count += m+n
		 *
		 * JLM: after complete set of spins
		 * JLM: with non-nested spinning -> yield_count += (m-j), spin2_count += ((m-j)*n)+(n-i+1)
		 * JLM: with nested spinning -> yield_count += m, spin2_count += m*n
		 */
		UDATA yieldCount = tryEnterYieldCount - _tryEnterYieldCount;
		UDATA spin2Count = yieldCount;
		if (1 == tryEnterNestedSpinning) {
			spin2Count *= tryEnterSpinCount2Init;
			if (0 != _tryEnterYieldCount) {
				/* acquired lock while spinning - partial set of spins performed */
				spin2Count += (tryEnterSpinCount2Init - _tryEnterSpinCount2 + 1);
			}
		} else {
			/* In non-nested case, tryEnterSpinCount2 gets set to 1 once tryEnterSpinCount2 is completed */
			if (1 != tryEnterSpinCount2) {
				spin2Count += (tryEnterSpinCount2Init - _tryEnterSpinCount2 + 1);
			} else {
				spin2Count += tryEnterSpinCount2Init;
			}
		}
		VM_AtomicSupport::add(&tracing->yield_count, yieldCount);
		VM_AtomicSupport::add(&tracing->spin2_count, spin2Count);
	}
#endif /* OMR_THR_JLM */

#if defined(OMR_THR_THREE_TIER_LOCKING) && defined(OMR_THR_SPIN_WAKE_CONTROL)
	if (tryEnterSpin && (OMRTHREAD_IGNORE_SPIN_THREAD_BOUND != lib->maxSpinThreads)) {
		VM_AtomicSupport::subtract(&monitor->spinThreads, 1);
	}
#endif /* defined(OMR_THR_THREE_TIER_LOCKING) && defined(OMR_THR_SPIN_WAKE_CONTROL) */

	return rc;
}

} /* extern "C" */
