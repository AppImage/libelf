// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>

#include "ldcache.hpp"

using std::string;
using std::vector;

// There are two formats for ld.so.cache. The pre-glibc format 2.2
// listed the number of library entries, followed by the entries
// themselves, followed by a string table holding strings pointed
// to by the library entries. This format is summarized below:
//
//      HEADER_MAGIC_OLD
//      nlibs
//      libs[0]
//      ...
//      libs[nlibs-1]
//      first string\0second string\0...last string\0
//      ^                                           ^
//      start of string table     end of string table
//
// For glibc 2.2 and beyond, a new format was created so that each
// library entry could hold more meta-data about the libraries they
// reference. To preserve backwards compatibility, the new format was
// embedded in the old format inside its string table (simply moving
// all existing strings further down in the string table). This makes
// sense for backwards compatibility because code that could parse the
// old format still works (the offsets for strings pointed to by
// the library entries are just larger now).
//
// However, it adds complications when parsing for the new format
// because the new format header needs to be aligned on an 8 byte
// boundary (potentially pushing the start address of the string table
// down a few bytes). A summary of the new format embedded in the old
// format with annotations on the start address of the string table
// can be seen below:
//
//      HEADER_MAGIC_OLD
//      nlibs
//      libs[0]
//      ...
//      libs[nlibs-1]
//      pad (align for new format)
//      HEADER_MAGIC_NEW    <-- start of string table
//      nlibs
//      len_strings
//      unused    // 20 bytes reserved for extensions
//      libs[0]
//      ...
//      libs[nlibs-1]
//      first string\0second string\0...last string\0
//                                                  ^
//                                end of string table
//
// We currently only support the new format, since glibc 2.2
// was released in late 2000.

namespace ldcache {

    constexpr char HEADER_MAGIC_OLD[] = "ld.so-1.7.0";
    constexpr char HEADER_MAGIC_NEW[] = "glibc-ld.so.cache1.1";

#define IS_ELF  0x00000001


    struct HeaderOld {
        char magic[sizeof(HEADER_MAGIC_OLD) - 1];
        uint32_t libraryCount; // Number of library entries.
    };


    struct EntryOld {
        int32_t flags;  // 0x01 indicates ELF library.
        uint32_t key;   // String table index.
        uint32_t value; // String table index.
    };


    struct HeaderNew {
        char magic[sizeof(HEADER_MAGIC_NEW) - 1];
        uint32_t libraryCount;  // Number of library entries.
        uint32_t stringsLength; // Length of "actual" string table.
        uint32_t unused[5];     // Leave space for future extensions
        // and align to 8 byte boundary.
    };


    struct EntryNew {
        int32_t flags;        // Flags bits determine arch and library type.
        uint32_t key;         // String table index.
        uint32_t value;       // String table index.
        uint32_t osVersion;   // Required OS version.
        uint64_t hwcap;       // Hwcap entry.
    };


// Returns a 'boundary' aligned pointer by rounding up to
// the nearest multiple of 'boundary'.
    static inline const char* align(const char* address, size_t boundary) {
        if ((size_t) address % boundary == 0) {
            return address;
        }
        return (address + boundary) - ((size_t) address % boundary);
    }


    vector<Entry> parse(const string& path) {
        // Read the complete file into a buffer
        std::ifstream in(path);
        std::vector<char> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        const char* data = buffer.data();

        // Grab a pointer to the old format header (for verification of
        // HEADER_MAGIC_OLD later on). Then jump forward to the location of
        // the new format header (it is the only format we support).
        HeaderOld* headerOld = (HeaderOld*) data;
        data += sizeof(HeaderOld);
        if (data >= buffer.data() + buffer.size()) {
            throw Error("Invalid format");
        }

        data += headerOld->libraryCount * sizeof(EntryOld);
        if (data >= buffer.data() + buffer.size()) {
            throw Error("Invalid format");
        }

        // The new format header and all of its library entries are embedded
        // in the old format's string table (the current location of data).
        // However, the header is aligned on an 8 byte boundary, so we
        // need to align 'data' to get it to point to the new header.
        data = align(data, alignof(HeaderNew));
        if (data >= buffer.data() + buffer.size()) {
            throw Error("Invalid format");
        }

        // Construct pointers to all of the important regions in the new
        // format: the header, the libentry array, and the new string table
        // (which starts at the same address as the aligned headerNew pointer).
        HeaderNew* headerNew = (HeaderNew*) data;
        data += sizeof(HeaderNew);
        if (data >= buffer.data() + buffer.size()) {
            throw Error("Invalid format");
        }

        EntryNew* entriesNew = (EntryNew*) data;
        data += headerNew->libraryCount * sizeof(EntryNew);
        if (data >= buffer.data() + buffer.size()) {
            throw Error("Invalid format");
        }

        // The start of the strings table is at the beginning of
        // the new header, per the above format description.
        char* strings = (char*) headerNew;

        // Adjust the pointer to add on the additional size of the strings
        // contained in the string table. At this point, 'data' should
        // point to an address just beyond the end of the file.
        data += headerNew->stringsLength;
        if ((size_t) (data - buffer.data()) != buffer.size()) {
            throw Error("Invalid format");
        }

        // Validate our header magic.
        if (strncmp(headerOld->magic,
                    HEADER_MAGIC_OLD,
                    sizeof(HEADER_MAGIC_OLD) - 1) != 0) {
            throw Error("Invalid format");
        }

        if (strncmp(headerNew->magic,
                    HEADER_MAGIC_NEW,
                    sizeof(HEADER_MAGIC_NEW) - 1) != 0) {
            throw Error("Invalid format");
        }

        // Make sure the very last character in the buffer is a '\0'.
        // This way, no matter what strings we index in the string
        // table, we know they will never run beyond the end of the
        // file buffer when extracting them.
        if (*(data - 1) != '\0') {
            throw Error("Invalid format");
        }

        // Build our vector of ldcache entries.
        vector<Entry> ldcache;
        for (uint32_t i = 0; i < headerNew->libraryCount; i++) {
            if (!(entriesNew[i].flags & IS_ELF)) {
                continue;
            }

            if (strings + entriesNew[i].key >= data) {
                throw Error("Invalid format");
            }

            if (strings + entriesNew[i].value >= data) {
                throw Error("Invalid format");
            }

            Entry entry;
            entry.name = &strings[entriesNew[i].key];
            entry.path = &strings[entriesNew[i].value];
            ldcache.push_back(entry);
        }

        return ldcache;
    }

    Error::Error(const string& what) : runtime_error(what) {}
} // namespace ldcache {
