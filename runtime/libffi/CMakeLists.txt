################################################################################
# Copyright IBM Corp. and others 2017
#
# This program and the accompanying materials are made available under
# the terms of the Eclipse Public License 2.0 which accompanies this
# distribution and is available at https://www.eclipse.org/legal/epl-2.0/
# or the Apache License, Version 2.0 which accompanies this distribution and
# is available at https://www.apache.org/licenses/LICENSE-2.0.
#
# This Source Code may also be made available under the following
# Secondary Licenses when the conditions for such availability set
# forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
# General Public License, version 2 with the GNU Classpath
# Exception [1] and GNU General Public License, version 2 with the
# OpenJDK Assembly Exception [2].
#
# [1] https://www.gnu.org/software/classpath/license.html
# [2] https://openjdk.org/legal/assembly-exception.html
#
# SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
################################################################################

set(OMR_ENHANCED_WARNINGS OFF)
set(OMR_WARNINGS_AS_ERRORS OFF)

# Work around a bug in MASM for VS2013
if(OMR_TOOLCONFIG STREQUAL "msvc")
	omr_remove_flags(CMAKE_ASM_MASM_FLAGS /W3)
	omr_append_flags(CMAKE_ASM_MASM_FLAGS /W2)
endif()

# Remove -qsourcetype=assembler which doesn't appear to work with OpenXL.
if(CMAKE_C_COMPILER_IS_XLCLANG)
	omr_remove_flags(CMAKE_ASM_FLAGS -qsourcetype=assembler)
endif()

j9vm_add_library(ffi STATIC
	closures.c
	debug.c
	java_raw_api.c
	prep_cif.c
	raw_api.c
	types.c
	tramp.c
)

# We need this because ffi_common.h is in 'include' in the top directory.
# once we fix that this can go away
target_include_directories(ffi PRIVATE ${j9vm_SOURCE_DIR}/include)

target_include_directories(ffi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#platform specific stuff hacked in
if(OMR_ARCH_X86)
	if(NOT (OMR_ENV_DATA64 OR OMR_OS_LINUX OR OMR_OS_WINDOWS))
		message(SEND_ERROR "Only x86-64 is currently supported")
	endif()

	target_include_directories(ffi PUBLIC x86)
	if(OMR_OS_LINUX)
		if(OMR_ENV_DATA64)
			target_include_directories(ffi PUBLIC preconf/xa)
		else()
			target_include_directories(ffi PUBLIC preconf/xi)
		endif()
	elseif(OMR_OS_OSX)
		target_include_directories(ffi PUBLIC preconf/oa)
	elseif(OMR_OS_WINDOWS)
		if(OMR_ENV_DATA64)
			target_include_directories(ffi PUBLIC preconf/wa)
		else()
			target_include_directories(ffi PUBLIC preconf/wi)
		endif()
	else()
		message(SEND_ERROR "Unsupported OS")
	endif()

	if(OMR_ENV_DATA64)
		target_sources(ffi PRIVATE x86/ffiw64.c)
	else()
		target_sources(ffi PRIVATE x86/ffi.c)
	endif()

	if(OMR_OS_LINUX OR OMR_OS_OSX)
		if(OMR_ENV_DATA64)
			target_sources(ffi
				PRIVATE
					x86/ffi64.c
					x86/unix64.S
					x86/win64.S
			)
		else()
			target_sources(ffi PRIVATE x86/win32.S)
		endif()
	endif()

	if(OMR_OS_WINDOWS)

		if(OMR_ENV_DATA64)
			set(source_name win64_intel)
			set(preconf_dir ${CMAKE_CURRENT_SOURCE_DIR}/preconf/wa)
		else()
			set(source_name win32)
			set(preconf_dir ${CMAKE_CURRENT_SOURCE_DIR}/preconf/wi)
		endif()

		# We are assuming we are using an MSVC toolchain
		add_custom_command(
			OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${source_name}.asm
			COMMAND ${CMAKE_C_COMPILER} -nologo -EP -P  -I ${preconf_dir} -I ${j9vm_SOURCE_DIR}/include -I ${CMAKE_CURRENT_SOURCE_DIR}/x86 -Fi${source_name}.asm  ${CMAKE_CURRENT_SOURCE_DIR}/x86/${source_name}.S
			DEPENDS x86/${source_name}.S
		)
		target_sources(ffi PRIVATE ${source_name}.asm)
		set_source_files_properties(${source_name}.asm PROPERTIES LANGUAGE ASM_MASM)
	else()
		target_sources(ffi PRIVATE x86/sysv.S)
	endif()
elseif(OMR_ARCH_POWER)
	if(CMAKE_CXX_COMPILER_ID STREQUAL "XL")
		if(OMR_ENV_DATA64)
			set(CMAKE_ASM_FLAGS "-qalias=noansi -qxflag=LTOL:LTOL0 -qxflag=selinux -qarch=ppc64 -q64")
		else()
			set(CMAKE_ASM_FLAGS "-qalias=noansi -qxflag=LTOL:LTOL0 -qxflag=selinux -qarch=ppc -q32")
		endif()
	endif()
	if(OMR_OS_LINUX)
		target_sources(ffi
			PRIVATE
				powerpc/ffi.c
				powerpc/ppc_closure.S
				powerpc/sysv.S
				powerpc/linux64.S
				powerpc/linux64_closure.S
				powerpc/ffi_linux64.c
				powerpc/ffi_sysv.c
		)
		set_source_files_properties(
			powerpc/ppc_closure.S
			powerpc/sysv.S
			powerpc/linux64.S
			powerpc/linux64_closure.S
			PROPERTIES
			LANGUAGE ASM
		)
		if(OMR_ENV_LITTLE_ENDIAN)
			target_include_directories(ffi PUBLIC preconf/xl64)
		else()
			target_include_directories(ffi PUBLIC preconf/xp64)
		endif()
	elseif(OMR_OS_AIX)
		target_sources(ffi
			PRIVATE
				powerpc/ffi_darwin.c
				powerpc/aix.S
				powerpc/aix_closure.S
		)
		set_source_files_properties(
			powerpc/aix.S
			powerpc/aix_closure.S
			PROPERTIES
			LANGUAGE ASM
		)
		if(OMR_ENV_DATA64)
			target_include_directories(ffi PUBLIC preconf/ap64)
		else()
			target_include_directories(ffi PUBLIC preconf/ap32)
		endif()
	endif()
	target_include_directories(ffi PUBLIC powerpc)
elseif(OMR_ARCH_S390)

	if(OMR_OS_LINUX)
		target_sources(ffi
			PRIVATE
				s390/ffi.c
				s390/sysv.S
		)
		if(OMR_ENV_DATA64)
			target_include_directories(ffi PUBLIC preconf/xz64)
			set(CMAKE_ASM_FLAGS "-z noexecstack -march=z9-109")
		else()
			target_include_directories(ffi PUBLIC preconf/xz31)
			set(CMAKE_ASM_FLAGS "-z noexecstack -mzarch -march=z9-109 -m31")
		endif()
		target_include_directories(ffi PUBLIC s390)
	elseif(OMR_OS_ZOS)
		target_include_directories(ffi PUBLIC z)
		if(OMR_ENV_DATA64)
			target_sources(ffi
				PRIVATE
					z/closure_sysvz64.s
					z/ffi64.c
					z/sysvz64.s
			)
			target_include_directories(ffi PUBLIC preconf/mz64)
		else()
			target_sources(ffi
				PRIVATE
					z/closure_sysvz.s
					z/ffi.c
					z/sysvz.s
			)
			target_include_directories(ffi PUBLIC preconf/mz31)
		endif()
	else()
		message(SEND_ERROR "Unsupported platform")
	endif()
elseif(OMR_ARCH_ARM AND OMR_OS_LINUX)
	target_include_directories(ffi
		PUBLIC
			arm
			preconf/arm
	)
	target_sources(ffi
		PRIVATE
			arm/ffi.c
			arm/sysv.S
			arm/trampoline.S
	)
elseif(OMR_ARCH_AARCH64 AND (OMR_OS_LINUX OR OMR_OS_OSX))
	target_include_directories(ffi
		PUBLIC
			aarch64
			preconf/xr64
	)
	target_sources(ffi
		PRIVATE
			aarch64/ffi.c
			aarch64/sysv.S
	)

elseif(OMR_ARCH_RISCV AND OMR_OS_LINUX)
	target_include_directories(ffi PUBLIC riscv)
	if(OMR_ENV_DATA64)
		target_include_directories(ffi PUBLIC preconf/rv64)
	else()
		message(SEND_ERROR "Unsupported platform")
	endif()
	target_sources(ffi
		PRIVATE
			riscv/ffi.c
			riscv/sysv.S
	)
else()
	message(SEND_ERROR "Unsupported platform")
endif()
