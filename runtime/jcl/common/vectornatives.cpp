/*******************************************************************************
 * Copyright IBM Corp. and others 2020
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
#include "j9.h"
#include "jclprots.h"
#include "j9protos.h"

/* TODO this will be replaced by a call that can determine
 * the correct size on each platform
 */
#define MAX_VECTOR_REGISTER_SIZE 128

extern "C" {

jint JNICALL
Java_jdk_internal_vm_vector_VectorSupport_registerNatives(JNIEnv *env, jclass clazz)
{
	return 0;
}

jint JNICALL
Java_jdk_internal_vm_vector_VectorSupport_getMaxLaneCount(JNIEnv *env, jclass clazz, jclass elementType)
{
	J9VMThread *currentThread = (J9VMThread *) env;
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	jint laneCount = 0;

	vmFuncs->internalEnterVMFromJNI(currentThread);
	j9object_t classObj = J9_JNI_UNWRAP_REFERENCE(elementType);
	if (NULL == classObj) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, NULL);
	} else {
		J9Class *elementType = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, classObj);

		if (elementType == vm->byteReflectClass) {
			laneCount = MAX_VECTOR_REGISTER_SIZE/8;
		} else if (elementType == vm->shortReflectClass) {
			laneCount = MAX_VECTOR_REGISTER_SIZE/16;
		} else if ((elementType == vm->intReflectClass) || (elementType == vm->floatReflectClass)) {
			laneCount = MAX_VECTOR_REGISTER_SIZE/32;
		} else if ((elementType == vm->longReflectClass) || (elementType == vm->doubleReflectClass)) {
			laneCount = MAX_VECTOR_REGISTER_SIZE/64;
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION, NULL);
		}
	}

	vmFuncs->internalExitVMToJNI(currentThread);


	return laneCount;
}

#if JAVA_SPEC_VERSION >= 25
jstring JNICALL
Java_jdk_internal_vm_vector_VectorSupport_getCPUFeatures(JNIEnv *env, jclass clazz)
{
	char buf[1024];
	OMRPORT_ACCESS_FROM_J9VMTHREAD((J9VMThread *)env);
	OMRProcessorDesc processorDesc;
	buf[0] = '\0';
	if (0 == omrsysinfo_get_processor_description(&processorDesc)) {
		omrsysinfo_get_processor_feature_string(&processorDesc, buf, sizeof(buf));
	}
	return env->NewStringUTF(buf);
}
#endif /* JAVA_SPEC_VERSION >= 25 */

} /* extern "C" */
