/*******************************************************************************
 * Copyright IBM Corp. and others 1991
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

#include <stdlib.h>
#include "j9.h"
#include "j9consts.h"
#if defined(J9VM_OPT_SNAPSHOTS)
#include "j9port_generated.h"
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
#include "j9protos.h"
#include "vm_internal.h"
#include "ut_j9vm.h"

#define TYPE_CLASS ((UDATA)0)
#define TYPE_PACKAGEID ((UDATA)-1)
#define TYPE_UNICODE ((UDATA)2)

#define ROUNDING_GRANULARITY    4
#define ROUNDED_BYTE_AMOUNT(number)  (((number) + (ROUNDING_GRANULARITY - 1)) & ~(UDATA)(ROUNDING_GRANULARITY - 1))

typedef union KeyHashTableClassEntry {
	UDATA tag;
	J9Class *ramClass;
	J9PackageIDTableEntry packageID;
	J9UTF8 *className;
} KeyHashTableClassEntry;

typedef struct KeyHashTableClassQueryEntry {
	KeyHashTableClassEntry entry;
	U_8 *charData;
	UDATA length;
} KeyHashTableClassQueryEntry;

static UDATA classHashFn(void *key, void *userData);
static UDATA classHashEqualFn(void *leftKey, void *rightKey, void *userData);
static UDATA classHashGetName(KeyHashTableClassEntry *entry, const U_8 **name, UDATA *nameLength);

static UDATA classLocationHashFn(void *key, void *userData);
static UDATA classLocationHashEqualFn(void *leftKey, void *rightKey, void *userData);

static void addLocationGeneratedClass(J9VMThread *vmThread, J9ClassLoader *classLoader, KeyHashTableClassEntry *existingPackageEntry, IDATA entryIndex, I_32 locationType);

static BOOLEAN isMHProxyPackage(J9ROMClass *romClass);

#if defined(J9_EXTENDED_DEBUG)
static void checkClassAlignment(J9Class *clazz, char const *caller)
{
	if (J9_ARE_ANY_BITS_SET((UDATA)clazz, J9_REQUIRED_CLASS_ALIGNMENT - 1)) {
		J9JavaVM *vm = NULL;
		jint nVMs = 0;
		if (JNI_OK == J9_GetCreatedJavaVMs((JavaVM **)&vm, 1, &nVMs)) {
			if (nVMs == 1) {
				PORT_ACCESS_FROM_JAVAVM(vm);
				J9VMThread *currentThread = currentVMThread(vm);
				j9tty_printf(PORTLIB, "\n<%p> %s: Unaligned class value %p\n", currentThread, caller, clazz);
			}
		}
		Assert_VM_unreachable();
	}
}
#else /* J9_EXTENDED_DEBUG */
#define checkClassAlignment(clazz, caller) Assert_VM_false(J9_ARE_ANY_BITS_SET((UDATA)(clazz), J9_REQUIRED_CLASS_ALIGNMENT - 1))
#endif /* J9_EXTENDED_DEBUG */

static UDATA
classHashGetName(KeyHashTableClassEntry *entry, const U_8 **name, UDATA *nameLength)
{
	UDATA type = TYPE_CLASS;
	UDATA tag = entry->tag;

	if (TAG_RAM_CLASS == (tag & MASK_RAM_CLASS)) {
		J9UTF8 *className = J9ROMCLASS_CLASSNAME(entry->ramClass->romClass);

		*name = J9UTF8_DATA(className);
		*nameLength = J9UTF8_LENGTH(className);
	} else if ((TAG_UTF_QUERY == (tag & MASK_QUERY))
		|| (TAG_PACKAGE_UTF_QUERY == (tag & MASK_QUERY))
	) {
		KeyHashTableClassQueryEntry *queryEntry = (KeyHashTableClassQueryEntry *)entry;

		*name = queryEntry->charData;
		*nameLength = queryEntry->length;
	} else if (TAG_UNICODE_QUERY == (tag & MASK_QUERY)) {
		KeyHashTableClassQueryEntry *queryEntry = (KeyHashTableClassQueryEntry *)entry;

		*name = (const U_8 *)queryEntry->charData;
		type = TYPE_UNICODE;
	} else if (J9_ARE_ANY_BITS_SET(tag, MASK_ROM_CLASS)) {
		*name = getPackageName(&entry->packageID, nameLength);
		type = TYPE_PACKAGEID;
	} else {
		Assert_VM_unreachable();
	}

	return type;
}

static UDATA
classHashEqualFn(void *tableNode, void *queryNode, void *userData)
{
	J9JavaVM *javaVM = (J9JavaVM *)userData;
	UDATA tableNodeLength = 0;
	UDATA queryNodeLength = 0;
	const U_8 *tableNodeName = NULL;
	const U_8 *queryNodeName = NULL;
	char buf[ROM_ADDRESS_LENGTH + 1] = {0};
	UDATA tableNodeType = classHashGetName(tableNode, &tableNodeName, &tableNodeLength);
	UDATA queryNodeType = classHashGetName(queryNode, &queryNodeName, &queryNodeLength);
	UDATA tableNodeTag = ((KeyHashTableClassEntry *)tableNode)->tag;
	BOOLEAN isTableNodeHiddenClass = (TYPE_CLASS == tableNodeType)
					&& (TAG_RAM_CLASS == (tableNodeTag & MASK_RAM_CLASS))
					&& J9ROMCLASS_IS_HIDDEN(((KeyHashTableClassEntry *)tableNode)->ramClass->romClass);

	if (isTableNodeHiddenClass) {
		/* Hidden class is keyed on its rom address, not on its name. */
		PORT_ACCESS_FROM_JAVAVM(javaVM);
		j9str_printf(buf, ROM_ADDRESS_LENGTH + 1, ROM_ADDRESS_FORMAT, (UDATA)((KeyHashTableClassEntry *)tableNode)->ramClass->romClass);
		tableNodeName = (const U_8 *)buf;
		tableNodeLength = ROM_ADDRESS_LENGTH;
	}

	if (queryNodeType == TYPE_UNICODE) {
		if (tableNodeType == TYPE_CLASS) {
			j9object_t stringObject = (j9object_t)queryNodeName;
			j9object_t charArray = J9VMJAVALANGSTRING_VALUE_VM(javaVM, stringObject);
			U_32 i = 0;
			U_32 end = J9VMJAVALANGSTRING_LENGTH_VM(javaVM, stringObject);

			BOOLEAN isCompressed = IS_STRING_COMPRESSED_VM(javaVM, stringObject);

			while (i < end) {
				U_16 unicode = 0;
				U_16 utf = 0;
				U_8 c = 0;

				if (isCompressed) {
					unicode = (U_8)J9JAVAARRAYOFBYTE_LOAD_VM(javaVM, charArray, i);
				} else {
					unicode = J9JAVAARRAYOFCHAR_LOAD_VM(javaVM, charArray, i);
				}

				if (tableNodeLength == 0) {
					return FALSE;
				}

				c = *(tableNodeName++);
				if ((c & 0x80) == 0x00) {
					/* one byte encoding */
					utf = (U_16)c;
					tableNodeLength -= 1;
				} else if ((c & 0xE0) == 0xC0) {
					/* two byte encoding */
					utf = ((U_16)c & 0x1F) << 6;
					c = *(tableNodeName++);
					utf += (U_16)c & 0x3F;
					tableNodeLength -= 2;
				} else {
					/* three byte encoding */
					utf = ((U_16)c & 0x0F) << 12;
					c = *(tableNodeName++);
					utf += ((U_16)c & 0x3F) << 6;
					c = *(tableNodeName++);
					utf += (U_16)c & 0x3F;
					tableNodeLength -= 3;
				}

				if (unicode != utf) {
					if ((unicode != '.') || (utf != '/')) {
						return FALSE;
					}
				}

				++i;
			}
			return tableNodeLength == 0;
		}
		return FALSE;
	}

	/*
	 * queryNodeType must equal tableNodeType
	 */
	if (tableNodeType != queryNodeType) {
		return FALSE;
	}

	Assert_VM_true(TAG_PACKAGE_UTF_QUERY != ((KeyHashTableClassEntry *)tableNode)->tag);

	if (TAG_PACKAGE_UTF_QUERY == ((KeyHashTableClassEntry *)queryNode)->tag) {
		if (queryNodeLength < tableNodeLength) {
			char * className = strrchr((char *)tableNodeName, '/');
			return J9UTF8_DATA_EQUALS(tableNodeName, (UDATA)className - (UDATA)tableNodeName, queryNodeName, queryNodeLength);
		}
		return FALSE;
	}
	return J9UTF8_DATA_EQUALS(tableNodeName, tableNodeLength, queryNodeName, queryNodeLength);
}

static UDATA
classHashFn(void *key, void *userData)
{
	J9JavaVM *javaVM = (J9JavaVM *)userData;
	UDATA length = 0;
	const U_8 *name = NULL;
	U_32 hash = 0;
	UDATA type = classHashGetName(key, &name, &length);
	UDATA keyTag = ((KeyHashTableClassEntry *)key)->tag;
	char buf[ROM_ADDRESS_LENGTH + 1] = {0};
	BOOLEAN isTableNodeHiddenClass = (TYPE_CLASS == type)
					&& (TAG_RAM_CLASS == (keyTag & MASK_RAM_CLASS))
					&& J9ROMCLASS_IS_HIDDEN(((KeyHashTableClassEntry *)key)->ramClass->romClass);

	if (isTableNodeHiddenClass) {
		/* for hidden class, do not key on its name, key on its rom address */
		PORT_ACCESS_FROM_JAVAVM(javaVM);
		j9str_printf(buf, ROM_ADDRESS_LENGTH + 1, ROM_ADDRESS_FORMAT, (UDATA)((KeyHashTableClassEntry *)key)->ramClass->romClass);
		name = (const U_8 *)buf;
		length = ROM_ADDRESS_LENGTH;
	}
	if (type == TYPE_UNICODE) {
		j9object_t stringObject = (j9object_t)name;

		hash = J9VMJAVALANGSTRING_HASH_VM(javaVM, stringObject);
		if (0 == hash) {
			j9object_t charArray = J9VMJAVALANGSTRING_VALUE_VM(javaVM, stringObject);
			U_32 i = 0;
			U_32 end = i + J9VMJAVALANGSTRING_LENGTH_VM(javaVM, stringObject);

			if (IS_STRING_COMPRESSED_VM(javaVM, stringObject)) {
				while (i < end) {
					hash = (hash << 5) - hash + (U_8)J9JAVAARRAYOFBYTE_LOAD_VM(javaVM, charArray, i);
					++i;
				}
			} else {
				while (i < end) {
					hash = (hash << 5) - hash + J9JAVAARRAYOFCHAR_LOAD_VM(javaVM, charArray, i);
					++i;
				}
			}

			J9VMJAVALANGSTRING_SET_HASH_VM(javaVM, stringObject, hash);
		}
		type = TYPE_CLASS;
	} else {
		hash = 0;
		while (length != 0) {
			U_16 unicodeChar = 0;
			U_8 c = *(name++);

			if ((c & 0x80) == 0x00) {
				/* one byte encoding */
				unicodeChar = (U_16)c;
				length -= 1;
			} else if ((c & 0xE0) == 0xC0) {
				/* two byte encoding */
				unicodeChar = ((U_16)c & 0x1F) << 6;
				c = *(name++);
				unicodeChar += (U_16)c & 0x3F;
				length -= 2;
			} else {
				/* three byte encoding */
				unicodeChar = ((U_16)c & 0x0F) << 12;
				c = *(name++);
				unicodeChar += ((U_16)c & 0x3F) << 6;
				c = *(name++);
				unicodeChar += (U_16)c & 0x3F;
				length -= 3;
			}

			/* Make the String and internal representations of the class name consistent */
			if ('/' == unicodeChar) {
				unicodeChar = '.';
			}
			hash = (hash << 5) - hash + unicodeChar;
		}
	}

	if (TYPE_PACKAGEID == type) {
		hash = hash ^ (U_32)type;
	}

	return hash;
}

J9HashTable *
hashClassTableNew(J9JavaVM *javaVM, U_32 initialSize)
{
	U_32 flags = J9HASH_TABLE_ALLOW_SIZE_OPTIMIZATION;
	OMRPORT_ACCESS_FROM_J9PORT(javaVM->portLibrary);

	/* If -XX:+FastClassHashTable is enabled, do not allow hash tables to grow automatically */
	if (J9_ARE_ALL_BITS_SET(javaVM->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_FAST_CLASS_HASH_TABLE)) {
		flags |= J9HASH_TABLE_DO_NOT_GROW;
	}

#if defined(J9VM_OPT_SNAPSHOTS)
	if (IS_SNAPSHOTTING_ENABLED(javaVM)) {
		OMRPORTLIB = VMSNAPSHOTIMPL_OMRPORT_FROM_JAVAVM(javaVM);
	}
#endif /* defined(J9VM_OPT_SNAPSHOTS) */

	return hashTableNew(
			OMRPORTLIB,
			J9_GET_CALLSITE(),
			initialSize,
			sizeof(KeyHashTableClassEntry),
			sizeof(char *),
			flags,
			J9MEM_CATEGORY_CLASSES,
			classHashFn,
			classHashEqualFn,
			NULL,
			javaVM);
}

J9Class *
hashClassTableAt(J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength)
{
	J9HashTable *table = classLoader->classHashTable;
	KeyHashTableClassQueryEntry key;
	KeyHashTableClassEntry *result = NULL;

	key.entry.tag = TAG_UTF_QUERY;
	key.charData = className;
	key.length = classNameLength;
	result = hashTableFind(table, &key);
	if (NULL != result) {
		J9Class *clazz = result->ramClass;
		checkClassAlignment(clazz, "hashClassTableAt");
		if (J9ROMCLASS_IS_HIDDEN(clazz->romClass)) {
			return NULL;
		}
		return clazz;
	} else {
		return NULL;
	}
}

BOOLEAN
isAnyClassLoadedFromPackage(J9ClassLoader *classLoader, U_8 *pkgName, UDATA pkgNameLength)
{
	J9HashTable *table = classLoader->classHashTable;
	KeyHashTableClassQueryEntry key;
	KeyHashTableClassEntry *result = NULL;

	key.entry.tag = TAG_PACKAGE_UTF_QUERY;
	key.charData = pkgName;
	key.length = pkgNameLength;
	result = hashTableFind(table, &key);
	if (NULL != result) {
		return TRUE;
	}
	return FALSE;
}

void
hashClassTableFree(J9ClassLoader *classLoader)
{
	J9HashTable *table = classLoader->classHashTable;

	/* Free all the chained hash tables */
	while (NULL != table) {
		J9HashTable *previous = table->previous;
		hashTableFree(table);
		table = previous;
	}

	classLoader->classHashTable = NULL;
}

static KeyHashTableClassEntry *
growClassHashTable(J9JavaVM *vm, J9ClassLoader *classLoader, KeyHashTableClassEntry *newEntry)
{
	KeyHashTableClassEntry *node = NULL;
	/* If -XX:+FastClassHashTable is enabled, attempt to allocate a new, larger hash table, otherwise return failure */
	if (J9_ARE_ALL_BITS_SET(vm->extendedRuntimeFlags, J9_EXTENDED_RUNTIME_FAST_CLASS_HASH_TABLE)) {
		J9HashTable *oldTable = classLoader->classHashTable;
		J9HashTable *newTable = hashTableNew(
					oldTable->portLibrary,
					J9_GET_CALLSITE(),
					oldTable->tableSize + 1,
					sizeof(KeyHashTableClassEntry),
					sizeof(char *),
					J9HASH_TABLE_DO_NOT_GROW | J9HASH_TABLE_ALLOW_SIZE_OPTIMIZATION,
					J9MEM_CATEGORY_CLASSES,
					classHashFn,
					classHashEqualFn,
					NULL,
					vm);
		if (NULL != newTable) {
			J9HashTableState walkState;
			/* Copy all of the data from the old hash table into the new one */
			KeyHashTableClassEntry *oldNode = hashTableStartDo(oldTable,  &walkState);
			while (NULL != oldNode) {
				if (NULL == hashTableAdd(newTable, oldNode)) {
					hashTableFree(newTable);
					return NULL;
				}
				oldNode = hashTableNextDo(&walkState);
			}
			node = hashTableAdd(newTable, newEntry);
			if (NULL == node) {
				hashTableFree(newTable);
				return NULL;
			}
			newTable->previous = oldTable;
			vm->freePreviousClassLoaders = TRUE;
			issueWriteBarrier();
			classLoader->classHashTable = newTable;
		}
	}
	return node;
}

UDATA
hashClassTableAtPut(J9VMThread *vmThread, J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength, J9Class *value)
{
	J9JavaVM *vm = vmThread->javaVM;
	J9HashTable *table = classLoader->classHashTable;
	KeyHashTableClassEntry *node = NULL;
	KeyHashTableClassEntry entry;

	entry.ramClass = value;
	node = hashTableAdd(table, &entry);

	if (NULL == node) {
		if (NULL == growClassHashTable(vm, classLoader, &entry)) {
			return 1;
		}
	}

	/* Issue a GC write barrier when modifying the class hash table */
	vm->memoryManagerFunctions->j9gc_objaccess_postStoreClassToClassLoader(vmThread, classLoader, value);
	return 0;
}

UDATA
hashClassTableDelete(J9ClassLoader *classLoader, U_8 *className, UDATA classNameLength)
{
	J9HashTable *table = classLoader->classHashTable;
	KeyHashTableClassQueryEntry key;

	key.entry.tag = TAG_UTF_QUERY;
	key.charData = className;
	key.length = classNameLength;
	return hashTableRemove(table, &key);
}

UDATA
hashClassTablePackageDelete(J9VMThread *vmThread, J9ClassLoader *classLoader, J9ROMClass *romClass)
{
	if (isMHProxyPackage(romClass)) {
		/* This generated package only exists for one hidden class and should be
		 * removed when its rom class is unloaded. hashClassTable package id's rely
		 * on there being a valid rom class.
		 */
		UDATA result = 0;
		KeyHashTableClassEntry key;
		key.tag = (UDATA)romClass | TAG_ROM_CLASS;
		omrthread_monitor_enter(vmThread->javaVM->classTableMutex);
		result = hashTableRemove(classLoader->classHashTable, &key);
		J9UTF8 *className = J9ROMCLASS_CLASSNAME(romClass);
		Trc_VM_hashClassTablePackageDelete(vmThread, romClass, J9UTF8_LENGTH(className), J9UTF8_DATA(className));
		omrthread_monitor_exit(vmThread->javaVM->classTableMutex);
		return result;
	} else {
		return 0;
	}
}

void
hashClassTableReplace(J9VMThread *vmThread, J9ClassLoader *classLoader, J9Class *originalClass, J9Class *replacementClass)
{
	J9HashTable *table = classLoader->classHashTable;
	KeyHashTableClassEntry *result = NULL;
	KeyHashTableClassQueryEntry original;

	original.entry.ramClass = originalClass;

	result = hashTableFind(table, &original);
	if ((NULL != result) && (result->ramClass == originalClass)) {
		result->ramClass = replacementClass;
		/* Issue a GC write barrier when modifying the class hash table */
		vmThread->javaVM->memoryManagerFunctions->j9gc_objaccess_postStoreClassToClassLoader(vmThread, classLoader, replacementClass);
	}
}

J9Class *
hashClassTableStartDo(J9ClassLoader *classLoader, J9HashTableState *walkState, UDATA flags)
{
	BOOLEAN skipHidden = J9_ARE_ALL_BITS_SET(flags, J9_HASH_TABLE_STATE_FLAG_SKIP_HIDDEN);
	BOOLEAN continueToNext = FALSE;
	KeyHashTableClassEntry *first = hashTableStartDo(classLoader->classHashTable, walkState);

	if (NULL != first) {
		if (TAG_RAM_CLASS != (first->tag & MASK_RAM_CLASS)) {
			/* only report RAM classes */
			continueToNext = TRUE;
		} else {
			J9Class *clazz = first->ramClass;

			if ((skipHidden && J9ROMCLASS_IS_HIDDEN(clazz->romClass))
#if defined(J9VM_OPT_SNAPSHOTS)
				|| J9_ARE_ANY_BITS_SET(clazz->classFlags, J9ClassIsFrozen)
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
			) {
				continueToNext = TRUE;
			} else {
				continueToNext = FALSE;
			}
		}
	} else {
		continueToNext = FALSE;
	}

	while (continueToNext) {
		first = hashTableNextDo(walkState);
		if (NULL != first) {
			if (TAG_RAM_CLASS != (first->tag & MASK_RAM_CLASS)) {
				/* only report RAM classes */
				continueToNext = TRUE;
			} else {
				J9Class *clazz = first->ramClass;

				if ((skipHidden && J9ROMCLASS_IS_HIDDEN(clazz->romClass))
#if defined(J9VM_OPT_SNAPSHOTS)
					|| J9_ARE_ANY_BITS_SET(clazz->classFlags, J9ClassIsFrozen)
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
				) {
					continueToNext = TRUE;
				} else {
					continueToNext = FALSE;
				}
			}
		} else {
			continueToNext = FALSE;
		}
	}
	walkState->flags = flags;
	return (NULL == first) ? NULL : first->ramClass;
}

J9Class *
hashClassTableNextDo(J9HashTableState *walkState)
{
	BOOLEAN skipHidden = J9_ARE_ALL_BITS_SET(walkState->flags, J9_HASH_TABLE_STATE_FLAG_SKIP_HIDDEN);
	BOOLEAN continueToNext = FALSE;
	KeyHashTableClassEntry *next = hashTableNextDo(walkState);

	if (NULL != next) {
		if (TAG_RAM_CLASS != (next->tag & MASK_RAM_CLASS)) {
			/* only report RAM classes */
			continueToNext = TRUE;
		} else {
			J9Class *clazz = next->ramClass;

			if ((skipHidden && J9ROMCLASS_IS_HIDDEN(clazz->romClass))
#if defined(J9VM_OPT_SNAPSHOTS)
				|| J9_ARE_ANY_BITS_SET(clazz->classFlags, J9ClassIsFrozen)
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
			) {
				continueToNext = TRUE;
			} else {
				continueToNext = FALSE;
			}
		}
	} else {
		continueToNext = FALSE;
	}
	/* only report RAM classes */
	while (continueToNext) {
		next = hashTableNextDo(walkState);
		if (NULL != next) {
			if (TAG_RAM_CLASS != (next->tag & MASK_RAM_CLASS)) {
				/* only report RAM classes */
				continueToNext = TRUE;
			} else {
				J9Class *clazz = next->ramClass;

				if ((skipHidden && J9ROMCLASS_IS_HIDDEN(clazz->romClass))
#if defined(J9VM_OPT_SNAPSHOTS)
					|| J9_ARE_ANY_BITS_SET(clazz->classFlags, J9ClassIsFrozen)
#endif /* defined(J9VM_OPT_SNAPSHOTS) */
				) {
					continueToNext = TRUE;
				} else {
					continueToNext = FALSE;
				}
			}
		} else {
			continueToNext = FALSE;
		}
	}

	return (NULL == next) ? NULL : next->ramClass;
}

static BOOLEAN
isMHProxyPackage(J9ROMClass *romClass) {
	const char *mhproxy = "jdk/MHProxy";
	/* Classes that are not strongly tied to the classloader will have
	 * J9AccClassAnonClass set. See java.lang.invoke.MethodHandles
	 */
	return _J9ROMCLASS_J9MODIFIER_IS_SET(romClass, J9AccClassAnonClass)
			&& J9UTF8_LITERAL_EQUALS(J9UTF8_DATA(J9ROMCLASS_CLASSNAME(romClass)), sizeof(mhproxy) - 1, mhproxy);
}

UDATA
hashPkgTableIDFor(J9VMThread *vmThread, J9ClassLoader *classLoader, J9ROMClass *romClass, IDATA entryIndex, I_32 locationType)
{
	KeyHashTableClassEntry key;
	J9JavaVM *javaVM = vmThread->javaVM;
	J9HashTable *table = classLoader->classHashTable;
	UDATA packageNameLength = 0;
	BOOLEAN isSystemClassLoader = (classLoader == javaVM->systemClassLoader);

	key.tag = (UDATA)romClass | TAG_ROM_CLASS;

	if (isSystemClassLoader && (J9ROMCLASS_IS_UNSAFE(romClass) || (LOAD_LOCATION_UNKNOWN == locationType))) {
		key.tag |= TAG_GENERATED_PACKAGE;
	}

	getPackageName(&key.packageID, &packageNameLength);
	if (packageNameLength == 0) {
		/* default package IDs don't go in the table. This prevents them from being returned by JVM_GetSystemPackages */
		return (UDATA)classLoader;
	} else {
		UDATA packageID = 0;
		KeyHashTableClassEntry *result = NULL;
		BOOLEAN peekOnly = (J9_CP_INDEX_PEEK == entryIndex);

#if JAVA_SPEC_VERSION >= 22
		/* OpenJ9 issue 18907 fix rely's on this assertion.
		 * Generated package names for hidden classes have a
		 * one to one relationship with the generated class.
		 */
		if (isMHProxyPackage(romClass)) {
			Assert_VM_true(NULL == hashTableFind(table, &key));
		}
#endif /* JAVA_SPEC_VERSION >= 22 */

		if (peekOnly) {
			result = hashTableFind(table, &key);
			if (NULL == result) {
				/* Not in the table yet, so use this romClass for the packageID as it is the
				 * first occurrence of this package in this class loader.
				 */
				result = &key;
			}
		} else {
			result = hashTableAdd(table, &key);
			if (NULL == result) {
				result = growClassHashTable(javaVM, classLoader, &key);
			}
		}
		if (NULL != result) {
			packageID = result->packageID.taggedROMClass;
			if (isSystemClassLoader && J9_ARE_ALL_BITS_SET(result->tag, TAG_GENERATED_PACKAGE)) {
				if (!peekOnly && J9_ARE_NO_BITS_SET(key.tag, TAG_GENERATED_PACKAGE)) {
					/* Below function removes the TAG_GENERATED_PACKAGE bit from the hash table entry after adding
					 * a location for the generated class.
					 */
					addLocationGeneratedClass(vmThread, classLoader, result, entryIndex, locationType);
				}
				/* Mask out TAG_GENERATED_PACKAGE so it is not included in the packageID stored in the RAM classes */
				packageID &= ~(UDATA)TAG_GENERATED_PACKAGE;
			}
		}
		return packageID;
	}
}

J9PackageIDTableEntry *
hashPkgTableAt(J9ClassLoader *classLoader, J9ROMClass *romClass)
{
	KeyHashTableClassEntry key;
	J9HashTable *table = classLoader->classHashTable;
	UDATA packageNameLength = 0;
	KeyHashTableClassEntry *result = NULL;

	key.tag = (UDATA)romClass | TAG_ROM_CLASS;

	getPackageName(&key.packageID, &packageNameLength);
	if (0 != packageNameLength) {
		result = hashTableFind(table, &key);
	}
	return (NULL != result) ? &result->packageID : NULL;
}

J9PackageIDTableEntry *
hashPkgTableStartDo(J9ClassLoader *classLoader, J9HashTableState *walkState)
{
	KeyHashTableClassEntry *first = hashTableStartDo(classLoader->classHashTable, walkState);

	/* examine only package IDs */
	while ((NULL != first) && (TAG_ROM_CLASS != (first->tag & MASK_ROM_CLASS))) {
		first = hashTableNextDo(walkState);
	}

	return (NULL == first) ? NULL : &first->packageID;
}

J9PackageIDTableEntry *
hashPkgTableNextDo(J9HashTableState *walkState)
{
	KeyHashTableClassEntry *next = hashTableNextDo(walkState);

	/* examine only package IDs */
	while ((NULL != next) && (TAG_ROM_CLASS != (next->tag & MASK_ROM_CLASS))) {
		next = hashTableNextDo(walkState);
	}

	return (next == NULL) ? NULL : &next->packageID;
}

J9Class *
hashClassTableAtString(J9ClassLoader *classLoader, j9object_t stringObject)
{
	J9HashTable *table = classLoader->classHashTable;
	KeyHashTableClassQueryEntry key;
	KeyHashTableClassEntry *result = NULL;

	key.entry.tag = TAG_UNICODE_QUERY;
	key.charData = (U_8 *)stringObject;
	result = hashTableFind(table, &key);
	if (NULL != result) {
		J9Class *clazz = result->ramClass;
		checkClassAlignment(clazz, "hashClassTableAtString");
		if (J9ROMCLASS_IS_HIDDEN(clazz->romClass)) {
			return NULL;
		}
		return clazz;
	}
	return NULL;
}

J9HashTable *
hashClassLocationTableNew(J9JavaVM *javaVM, U_32 initialSize)
{
	U_32 flags = J9HASH_TABLE_ALLOW_SIZE_OPTIMIZATION;
	OMRPORT_ACCESS_FROM_J9PORT(javaVM->portLibrary);

#if defined(J9VM_OPT_SNAPSHOTS)
	if (IS_SNAPSHOTTING_ENABLED(javaVM)) {
		OMRPORTLIB = VMSNAPSHOTIMPL_OMRPORT_FROM_JAVAVM(javaVM);
	}
#endif /* defined(J9VM_OPT_SNAPSHOTS) */

	return hashTableNew(
			OMRPORTLIB,
			J9_GET_CALLSITE(),
			initialSize,
			sizeof(J9ClassLocation),
			sizeof(char *),
			flags,
			J9MEM_CATEGORY_CLASSES,
			classLocationHashFn,
			classLocationHashEqualFn,
			NULL,
			javaVM);
}

static UDATA
classLocationHashFn(void *key, void *userData)
{
	J9ClassLocation *entry = (J9ClassLocation *)key;

	return (UDATA)entry->clazz;
}

static UDATA
classLocationHashEqualFn(void *leftKey, void *rightKey, void *userData)
{
	J9ClassLocation *leftNode = (J9ClassLocation *)leftKey;
	J9ClassLocation *rightNode = (J9ClassLocation *)rightKey;

	return leftNode->clazz == rightNode->clazz;
}

/**
 * @brief If a generated class is loaded from a package, then package information should not be available. If a non-generated
 * class is loaded from the same package at a later point, then package information should be available. In order to make
 * package information available, this function adds a class location for the generated class in the classLocationHashTable,
 * which points at the entryIndex of the non-generated class. Then, it also removes the TAG_GENERATED_PACKAGE from the existing
 * package entry of the generated class.
 * @param *vmThread J9VMThread instance
 * @param *classLoader Classloader for the class
 * @param *existingPackageEntry Package entry for the generated class i.e. ROM classs
 * @param entryIndex classpath index of the non-generated class
 * @param locationType location type of non-generated class
 * @return void
 */
static void
addLocationGeneratedClass(J9VMThread *vmThread, J9ClassLoader *classLoader, KeyHashTableClassEntry *existingPackageEntry, IDATA entryIndex, I_32 locationType)
{
	J9JavaVM *javaVM = vmThread->javaVM;
	J9InternalVMFunctions *funcs = javaVM->internalVMFunctions;
	J9ROMClass *resultROMClass = (J9ROMClass *)(existingPackageEntry->tag & ~(UDATA)(TAG_ROM_CLASS | TAG_GENERATED_PACKAGE));
	J9UTF8 *className = J9ROMCLASS_CLASSNAME(resultROMClass);
	J9Class *clazz = NULL;

	clazz = funcs->hashClassTableAt(classLoader, J9UTF8_DATA(className), J9UTF8_LENGTH(className));
	if (NULL != clazz) {
		J9ClassLocation *classLocation = NULL;
		J9ClassLocation newLocation = {0};

		omrthread_monitor_enter(javaVM->classLoaderModuleAndLocationMutex);
		classLocation = funcs->findClassLocationForClass(vmThread, clazz);

		if (NULL == classLocation) {
			I_32 newLocationType = 0;
			if (LOAD_LOCATION_PATCH_PATH == locationType) {
				newLocationType = LOAD_LOCATION_PATCH_PATH_NON_GENERATED;
			} else if (LOAD_LOCATION_CLASSPATH == locationType) {
				newLocationType = LOAD_LOCATION_CLASSPATH_NON_GENERATED;
			} else if (LOAD_LOCATION_MODULE == locationType) {
				newLocationType = LOAD_LOCATION_MODULE_NON_GENERATED;
			} else {
				Assert_VM_unreachable();
			}

			newLocation.clazz = clazz;
			newLocation.entryIndex = entryIndex;
			newLocation.locationType = newLocationType;

			/* LOAD_LOCATION_UNKNOWN entries (with entryIndex == -1) are not stored in the classLocationHashtable.
			 * So, we don't need to remove a hash table entry before adding the new one.
			 */
			hashTableAdd(classLoader->classLocationHashTable, (void *)&newLocation);
		} else {
			/* In case of multi-threaded class loading, we must avoid adding an entry if it already exists. But, we
			 * should make sure that the existing entry has locationType for non-generated class i.e. (locationType < 0).
			 */
			Assert_VM_true(classLocation->locationType < 0);
		}
		omrthread_monitor_exit(javaVM->classLoaderModuleAndLocationMutex);

		existingPackageEntry->tag &= ~(UDATA)TAG_GENERATED_PACKAGE;
	}
}

J9ClassLocation *
findClassLocationForClass(J9VMThread *currentThread, J9Class *clazz)
{
	J9ClassLocation classLocation = {0};
	J9ClassLocation *targetPtr = NULL;

	if (NULL == clazz->classLoader->classLocationHashTable) {
		return NULL;
	}

	Assert_VM_mustOwnMonitor(currentThread->javaVM->classLoaderModuleAndLocationMutex);

	classLocation.clazz = clazz;
	targetPtr = hashTableFind(clazz->classLoader->classLocationHashTable, (void *)&classLocation);

	return targetPtr;
}

static UDATA
classLoaderPtrHashFn(void *entry, void *userData)
{
	return (UDATA)*(J9ClassLoader **)entry;
}

static UDATA
classLoaderPtrHashEqualFn(void *a, void *b, void *userData)
{
	return *(J9ClassLoader **)a == *(J9ClassLoader **)b;
}

static J9HashTable *
outlivingLoadersTableNew(J9JavaVM *javaVM, U_32 initialSize)
{
	return hashTableNew(
			OMRPORT_FROM_J9PORT(javaVM->portLibrary),
			J9_GET_CALLSITE(),
			initialSize,
			sizeof(J9ClassLoader *),
			sizeof(J9ClassLoader *),
			0,
			J9MEM_CATEGORY_CLASSES,
			classLoaderPtrHashFn,
			classLoaderPtrHashEqualFn,
			NULL,
			NULL);
}

void
addOutlivingLoader(J9VMThread *currentThread, J9ClassLoader *classLoader, J9ClassLoader *outlivingLoader)
{
	J9JavaVM *vm = currentThread->javaVM;

	Assert_VM_false(outlivingLoader == classLoader);
	Assert_VM_mustOwnMonitor(vm->classTableMutex);

	if (J9CLASSLOADER_OUTLIVING_LOADERS_PERMANENT == outlivingLoader->outlivingLoaders) {
		/* Nothing to do. */
	} else if (J9CLASSLOADER_OUTLIVING_LOADERS_PERMANENT == classLoader->outlivingLoaders) {
		markLoaderPermanent(currentThread, outlivingLoader);
	} else if (NULL == classLoader->outlivingLoaders) {
		classLoader->outlivingLoaders = (void *)((UDATA)outlivingLoader | J9CLASSLOADER_OUTLIVING_LOADERS_SINGLE_TAG);
	} else if (J9_ARE_ANY_BITS_SET((UDATA)classLoader->outlivingLoaders, J9CLASSLOADER_OUTLIVING_LOADERS_SINGLE_TAG)) {
		J9ClassLoader *existing = (J9ClassLoader *)((UDATA)classLoader->outlivingLoaders & ~(UDATA)J9CLASSLOADER_OUTLIVING_LOADERS_SINGLE_TAG);
		if (existing != outlivingLoader) {
			J9HashTable *table = outlivingLoadersTableNew(vm, 2);
			if (NULL != table) {
				J9ClassLoader **entry0 = (J9ClassLoader **)hashTableAdd(table, &existing);
				J9ClassLoader **entry1 = (J9ClassLoader **)hashTableAdd(table, &outlivingLoader);
				if ((NULL != entry0) && (NULL != entry1)) {
					classLoader->outlivingLoaders = table;
				} else {
					hashTableFree(table);
				}
			}
		}
	} else {
		hashTableAdd((J9HashTable *)classLoader->outlivingLoaders, &outlivingLoader);
	}
}

void
markLoaderPermanent(J9VMThread *currentThread, J9ClassLoader *classLoader)
{
	J9JavaVM *vm = currentThread->javaVM;
	void *outlivingLoaders = NULL;

	Assert_VM_mustOwnMonitor(vm->classTableMutex);

	outlivingLoaders = classLoader->outlivingLoaders;
	if (J9CLASSLOADER_OUTLIVING_LOADERS_PERMANENT != outlivingLoaders) {
		/* Update now to ensure that recursion terminates in the presence of cycles. */
		classLoader->outlivingLoaders = J9CLASSLOADER_OUTLIVING_LOADERS_PERMANENT;

		if (NULL != vm->jitConfig) {
			vm->jitConfig->jitAddPermanentLoader(currentThread, classLoader);
		}

		/* Any loader previously known to outlive classLoader is also permanent. */
		if (NULL != outlivingLoaders) {
			if (J9_ARE_ANY_BITS_SET((UDATA)outlivingLoaders, J9CLASSLOADER_OUTLIVING_LOADERS_SINGLE_TAG)) {
				J9ClassLoader *outlivingLoader = (J9ClassLoader *)((UDATA)outlivingLoaders & ~(UDATA)J9CLASSLOADER_OUTLIVING_LOADERS_SINGLE_TAG);
				markLoaderPermanent(currentThread, outlivingLoader);
			} else {
				J9HashTable *table = (J9HashTable *)outlivingLoaders;
				J9HashTableState state;
				J9ClassLoader **entry = (J9ClassLoader **)hashTableStartDo(table, &state);
				while (NULL != entry) {
					markLoaderPermanent(currentThread, *entry);
					entry = (J9ClassLoader **)hashTableNextDo(&state);
				}

				hashTableFree(table);
			}
		}
	}
}
