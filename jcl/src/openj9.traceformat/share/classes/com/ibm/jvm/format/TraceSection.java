/*[INCLUDE-IF JAVA_SPEC_VERSION >= 8]*/
/*
 * Copyright IBM Corp. and others 2000
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
 */
package com.ibm.jvm.format;

import java.io.IOException;
import java.math.BigInteger;

/**
 * Trace section of a file header.
 *
 * @author Tim Preece
 */
public class TraceSection {

	private                String    eyecatcher_string;
	private                int       length;
	private                int       version;
	private                int       modification;
	private                BigInteger startPlatform;
	private                BigInteger startSystem;
	private                int       type;
	private                int       generations;
	private                int       pointerSize                   = 4;

	public TraceSection (TraceFile traceFile, int start ) throws IOException
	{
		// Version 1.1
		traceFile.seek((long)start);
		eyecatcher_string  =  Util.convertAndCheckEyecatcher(traceFile.readI());
		length        = traceFile.readI();
		version       = traceFile.readI();
		modification  = traceFile.readI();
		startPlatform = traceFile.readBigInteger(8);
		startSystem   = traceFile.readBigInteger(8);
		type          = traceFile.readI();
		generations   = traceFile.readI();
		pointerSize   = traceFile.readI();

		Util.setProperty("POINTER_SIZE", String.valueOf(pointerSize));
		Message.setPointerSize();
		TraceFormat.setStartSystem(startSystem);
		TraceFormat.setStartPlatform(startPlatform);
		TraceFormat.setGenerations(generations);

		Util.Debug.println("TraceSection: eyecatcher:          " + eyecatcher_string);
		Util.Debug.println("TraceSection: length:              " + length);
		Util.Debug.println("TraceSection: version:             " + version);
		Util.Debug.println("TraceSection: modification:        " + modification);
		Util.Debug.println("TraceSection: startPlatform:       " + startPlatform);
		Util.Debug.println("TraceSection: startSystem:         " + startSystem);
		Util.Debug.println("TraceSection: type:                " + type); //  0=internal 1=external
		Util.Debug.println("TraceSection: generations:         " + generations);
		Util.Debug.println("TraceSection: pointerSize:         " + pointerSize);
	}

	/** returns the type of trace ( INTERNAL or EXTERNAL )
	 *
	 * @return  an int
	 */
	final protected int getTraceType()
	{
		return type;
	}

}
