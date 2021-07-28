/*******************************************************************************
 * Copyright (c) 2019, 2021 IBM Corp. and others
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
import com.ibm.j9ddr.vm29.pointer.generated.J9ShrOffsetPointer;
import com.ibm.j9ddr.vm29.tools.ddrinteractive.commands.ShrCCommand;
import com.ibm.j9ddr.vm29.pointer.U8Pointer;
import com.ibm.j9ddr.vm29.types.UDATA;
import com.ibm.j9ddr.vm29.types.I32;
import com.ibm.j9ddr.vm29.types.IDATA;
import com.ibm.j9ddr.vm29.types.Scalar;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

public class SharedClassesMetaDataHelper {
	private static U8Pointer[] cacheBases;
	private static U8Pointer[] cacheEnds;
	public static void setCacheBases(U8Pointer[] baseAddresses) {
		if (null == cacheBases) {
			cacheBases = baseAddresses;
		}
	}
	public static void setCacheEnds(U8Pointer[] endAddresses) {
		if (null == cacheEnds) {
			cacheEnds = endAddresses;
		}
	}
	public static int getCacheLayerFromJ9shrOffset(J9ShrOffsetPointer j9shrOffset) throws CorruptDataException {
		int layer;
		try {
			UDATA layerUDATA = (UDATA) j9shrOffset.getClass().getMethod("cacheLayer").invoke(j9shrOffset);
			layer = layerUDATA.intValue();
		} catch (NoSuchMethodException e) {
			layer = 0;
		} catch (InvocationTargetException e) {
			Throwable cause = e.getCause();
			if (cause instanceof NoSuchFieldError) {
				layer = 0;
			} else if (cause instanceof CorruptDataException) {
				throw (CorruptDataException) cause;
			} else if (cause instanceof RuntimeException) {
				throw (RuntimeException) cause;
			} else {
				throw new CorruptDataException("Error getting the cache layer from J9ShrOffsetPointer", cause);
			}
		} catch (IllegalAccessException e) {
			throw new RuntimeException("Error getting the cache layer from J9ShrOffsetPointer");
		}
		return layer;
	}

	public static U8Pointer getAddressFromJ9shrOffset(J9ShrOffsetPointer j9shrOffset) throws CorruptDataException {
		Scalar offsetValue;
		try {
			offsetValue = (Scalar) j9shrOffset.getClass().getMethod("offset").invoke(j9shrOffset);
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
			throw new RuntimeException("Error getting the cache layer from J9ShrOffsetPointer");
		}
		IDATA offset = new IDATA(offsetValue);
		if (!ShrCCommand.isResizableCache()) {
			if (offset.isZero()) {
				return U8Pointer.NULL;
			}
		}
		int layer = getCacheLayerFromJ9shrOffset(j9shrOffset);
		if (offset.gte(new IDATA(0))) {
			return cacheBases[layer].add(offset);
		} else {
			return cacheEnds[layer].add(offset);
		}
	}
}
