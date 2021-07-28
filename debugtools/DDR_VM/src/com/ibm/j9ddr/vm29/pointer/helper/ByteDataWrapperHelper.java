/*******************************************************************************
 * Copyright (c) 1991, 2021 IBM Corp. and others
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
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/
package com.ibm.j9ddr.vm29.pointer.helper;

import com.ibm.j9ddr.CorruptDataException;
import com.ibm.j9ddr.vm29.pointer.AbstractPointer;
import com.ibm.j9ddr.vm29.pointer.I32Pointer;
import com.ibm.j9ddr.vm29.pointer.PointerPointer;
import com.ibm.j9ddr.vm29.pointer.U8Pointer;
import com.ibm.j9ddr.vm29.pointer.generated.ByteDataWrapperPointer;
import com.ibm.j9ddr.vm29.pointer.generated.J9ShrOffsetPointer;
import com.ibm.j9ddr.vm29.structure.ByteDataWrapper;
import com.ibm.j9ddr.vm29.types.I32;
import com.ibm.j9ddr.vm29.types.U16;
import com.ibm.j9ddr.vm29.types.U32;
import com.ibm.j9ddr.vm29.types.U8;
import com.ibm.j9ddr.vm29.types.Scalar;

import java.lang.reflect.*;

public class ByteDataWrapperHelper {
	// #define BDWITEM(bdw) (((U_8*)(bdw)) - sizeof(ShcItem))

	// #define BDWEXTBLOCK(bdw) J9SHR_READMEM((bdw)->externalBlockOffset)
	public static I32 BDWEXTBLOCK(ByteDataWrapperPointer ptr, boolean isCacheLayered) throws CorruptDataException {
		PointerPointer externalBlockOffset = ptr.externalBlockOffsetEA();
		if (!isCacheLayered) {
			return I32Pointer.cast(externalBlockOffset).at(0);
		} else {
			J9ShrOffsetPointer j9shroffsetPtr = J9ShrOffsetPointer.cast(externalBlockOffset);
			try {
				Scalar offset = (Scalar) j9shroffsetPtr.getClass().getMethod("offset").invoke(j9shroffsetPtr);
				return new I32(offset);
			} catch (NoSuchMethodException e) {
				throw new CorruptDataException("Error getting the offset from J9ShrOffsetPointer, not method offset() found");
			} catch (InvocationTargetException e) {
				Throwable cause = e.getCause();
				if (cause instanceof CorruptDataException) {
					throw (CorruptDataException) cause;
				} else if (cause instanceof RuntimeException) {
					throw (RuntimeException) cause;
				} else {
					throw new CorruptDataException("Error getting the offset from J9ShrOffsetPointer", cause);
				}
			} catch (IllegalAccessException e) {
				throw new RuntimeException("Error getting the offset from J9ShrOffsetPointer");
			}
		}
	}

	// #define BDWLEN(bdw) J9SHR_READMEM((bdw)->dataLength)
	public static U32 BDWLEN(ByteDataWrapperPointer ptr) throws CorruptDataException {
		return new U32(ptr.dataLength());
	}

	// #define BDWTYPE(bdw)  J9SHR_READMEM((bdw)->dataType)
	public static U8 BDWTYPE(ByteDataWrapperPointer ptr) throws CorruptDataException {
		return ptr.dataType();
	}

	public static U8Pointer getDataFromByteDataWrapper(ByteDataWrapperPointer ptr, boolean isCacheLayered) throws CorruptDataException {
		PointerPointer externalBlockOffset = ptr.externalBlockOffsetEA();
		if (!isCacheLayered) {
			I32 externalBlockOffsetI32 = I32Pointer.cast(externalBlockOffset).at(0);
			if (!externalBlockOffsetI32.isZero()) {
				return U8Pointer.cast(ptr).add(externalBlockOffsetI32); 
			}
		} else {
			try {
				J9ShrOffsetPointer j9shrOffset = J9ShrOffsetPointer.cast(externalBlockOffset);
				U8Pointer ret = SharedClassesMetaDataHelper.getAddressFromJ9shrOffset(j9shrOffset);
				if (U8Pointer.NULL != ret) {
					/* return the same as SH_CacheMap::getDataFromByteDataWrapper(const ByteDataWrapper *bdw) */
					return ret;
				}
			} catch (NoClassDefFoundError e) {
				// J9ShrOffset didn't exist in the VM that created this core file
			}
		}
		/* return ((U_8*)(bdw)) + sizeof(ByteDataWrapper) */
		return U8Pointer.cast(ptr).add(ByteDataWrapper.SIZEOF);
	}

	// #define BDWINPRIVATEUSE(bdw) J9SHR_READMEM((bdw)->inPrivateUse)
	public static U8 BDWINPRIVATEUSE(ByteDataWrapperPointer ptr) throws CorruptDataException {
		return ptr.inPrivateUse();
	}

	// #define BDWPRIVATEOWNERID(bdw) J9SHR_READMEM((bdw)->privateOwnerID)
	public static U16 BDWPRIVATEOWNERID(ByteDataWrapperPointer ptr) throws CorruptDataException {
		return ptr.privateOwnerID();
	}

	public static AbstractPointer BDWTOKEN(ByteDataWrapperPointer ptr, boolean isCacheLayered) throws CorruptDataException {
		PointerPointer tokenOffset = ptr.tokenOffsetEA();
		if (!isCacheLayered) {
			I32 tokenOffsetI32 = I32Pointer.cast(tokenOffset).at(0);
			if (!tokenOffsetI32.isZero()) {
				return U8Pointer.cast(ptr).add(tokenOffsetI32);
			}
		} else {
			try {
				J9ShrOffsetPointer j9shrOffset = J9ShrOffsetPointer.cast(tokenOffset);
				return SharedClassesMetaDataHelper.getAddressFromJ9shrOffset(j9shrOffset);
			} catch (NoClassDefFoundError e) {
				// J9ShrOffset didn't exist in the VM that created this core file
				// even though it appears to support a multi-layer cache.
				throw new CorruptDataException(e);
			}
		}
		return U8Pointer.NULL;
	}
}
