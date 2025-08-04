/*
 * Copyright 2012 Jose Fonseca
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include "libdwarf_private.h"
#include "dwarf_pe.h"

#include <assert.h>
#include <stdlib.h>

#include <windows.h>

#include <string>
#include <vector>

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"  // for Dwarf_Debug_s

#include "outdbg.h"
#include "paths.h"


typedef struct {
    HANDLE hFileMapping;
    SIZE_T nFileSize;
    union {
        PBYTE lpFileBase;
        PIMAGE_DOS_HEADER pDosHeader;
    };
    PIMAGE_NT_HEADERS pNtHeaders;
    PIMAGE_SECTION_HEADER Sections;
    PIMAGE_SYMBOL pSymbolTable;
    PSTR pStringTable;
} pe_access_object_t;


static int
pe_get_section_info(void *obj,
                    Dwarf_Unsigned section_index,
                    Dwarf_Obj_Access_Section_a *return_section,
                    int *error)
{
    pe_access_object_t *pe_obj = (pe_access_object_t *)obj;

    return_section->as_addr = 0;
    if (section_index == 0) {
        /* Non-elf object formats must honor elf convention that pSection index
         * is always empty. */
        return_section->as_size = 0;
        return_section->as_name = "";
    } else {
        PIMAGE_SECTION_HEADER pSection = pe_obj->Sections + section_index - 1;
        if (pSection->Misc.VirtualSize < pSection->SizeOfRawData) {
            return_section->as_size = pSection->Misc.VirtualSize;
        } else {
            return_section->as_size = pSection->SizeOfRawData;
        }
        return_section->as_name = (const char *)pSection->Name;
        if (return_section->as_name[0] == '/') {
            return_section->as_name = &pe_obj->pStringTable[atoi(&return_section->as_name[1])];
        }
    }
    return_section->as_link = 0;
    return_section->as_info = 0;
    return_section->as_addralign = 0;
    return_section->as_entrysize = 0;

    return DW_DLV_OK;
}


static Dwarf_Small
pe_get_byte_order(void *obj)
{
    return DW_END_little;
}


static Dwarf_Small
pe_get_length_pointer_size(void *obj)
{
    pe_access_object_t *pe_obj = (pe_access_object_t *)obj;
    PIMAGE_OPTIONAL_HEADER pOptionalHeader = &pe_obj->pNtHeaders->OptionalHeader;

    switch (pOptionalHeader->Magic) {
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        return 4;
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        return 8;
    default:
        return 0;
    }
}


static Dwarf_Unsigned
pe_get_filesize(void* obj)
{
    pe_access_object_t *pe_obj = (pe_access_object_t *)obj;
    return pe_obj->nFileSize;
}


static Dwarf_Unsigned
pe_get_section_count(void *obj)
{
    pe_access_object_t *pe_obj = (pe_access_object_t *)obj;
    PIMAGE_FILE_HEADER pFileHeader = &pe_obj->pNtHeaders->FileHeader;
    return pFileHeader->NumberOfSections + 1;
}


static int
pe_load_section(void *obj, Dwarf_Unsigned section_index, Dwarf_Small **return_data, int *error)
{
    pe_access_object_t *pe_obj = (pe_access_object_t *)obj;
    if (section_index == 0) {
        return DW_DLV_NO_ENTRY;
    } else {
        PIMAGE_SECTION_HEADER pSection = pe_obj->Sections + section_index - 1;
        *return_data = pe_obj->lpFileBase + pSection->PointerToRawData;
        return DW_DLV_OK;
    }
}


static const Dwarf_Obj_Access_Methods_a pe_methods = {
    pe_get_section_info,
    pe_get_byte_order,
    pe_get_length_pointer_size,
    pe_get_length_pointer_size,
    pe_get_filesize,
    pe_get_section_count,
    pe_load_section,
    NULL  // om_relocate_a_section
};


int
dwarf_pe_init(HANDLE hFile,
              const char *image,
              Dwarf_Handler errhand,
              Dwarf_Ptr errarg,
              Dwarf_Debug *ret_dbg,
              Dwarf_Error *error)
{
    int res = DW_DLV_ERROR;
    pe_access_object_t *pe_obj;
    DWORD dwFileSizeHi;
    DWORD dwFileSizeLo;
    Dwarf_Unsigned section_count;

    /* Initialize the internal struct */
    pe_obj = (pe_access_object_t *)calloc(1, sizeof *pe_obj);
    if (!pe_obj) {
        goto no_internals;
    }

    pe_obj->hFileMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!pe_obj->hFileMapping) {
        goto no_file_mapping;
    }

    pe_obj->lpFileBase = (PBYTE)MapViewOfFile(pe_obj->hFileMapping, FILE_MAP_READ, 0, 0, 0);
    if (!pe_obj->lpFileBase) {
        goto no_view_of_file;
    }

    dwFileSizeHi = 0;
    dwFileSizeLo = GetFileSize(hFile, &dwFileSizeHi);
    pe_obj->nFileSize = dwFileSizeLo;
#ifdef _WIN64
    pe_obj->nFileSize |= (SIZE_T)dwFileSizeHi << 32;
#else
    assert(dwFileSizeHi == 0);
#endif

    pe_obj->pNtHeaders = (PIMAGE_NT_HEADERS)(pe_obj->lpFileBase + pe_obj->pDosHeader->e_lfanew);
    pe_obj->Sections = (PIMAGE_SECTION_HEADER)((PBYTE)pe_obj->pNtHeaders + sizeof(DWORD) +
                                               sizeof(IMAGE_FILE_HEADER) +
                                               pe_obj->pNtHeaders->FileHeader.SizeOfOptionalHeader);
    pe_obj->pSymbolTable =
        (PIMAGE_SYMBOL)(pe_obj->lpFileBase + pe_obj->pNtHeaders->FileHeader.PointerToSymbolTable);
    if (pe_obj->pNtHeaders->FileHeader.PointerToSymbolTable +
            pe_obj->pNtHeaders->FileHeader.NumberOfSymbols * sizeof pe_obj->pSymbolTable[0] >
        pe_obj->nFileSize) {
        OutputDebug("MGWHELP: %s - symbol table extends beyond image size\n", image);
        goto no_intfc;
    }

    pe_obj->pStringTable =
        (PSTR)&pe_obj->pSymbolTable[pe_obj->pNtHeaders->FileHeader.NumberOfSymbols];

    // https://sourceware.org/gdb/onlinedocs/gdb/Separate-Debug-Files.html
    section_count = pe_get_section_count(pe_obj);
    for (Dwarf_Unsigned section_index = 0; section_index < section_count; ++section_index) {
        Dwarf_Obj_Access_Section_a doas;
        memset(&doas, 0, sizeof doas);
        int err = 0;
        pe_get_section_info(pe_obj, section_index, &doas, &err);
        if (!doas.as_size) {
            continue;
        }

        if (strcmp(doas.as_name, ".gnu_debuglink") == 0) {
            Dwarf_Small *data;
            if (DW_DLV_OK != pe_load_section(pe_obj, section_index, &data, &err)) {
                continue;
            }
            const char *debuglink = (const char *)data;

            std::vector<std::string> debugSearchDirs;

            // Search on the image directory
            const char *pImageSep = getSeparator(image);
            std::string imageDir;
            if (pImageSep) {
                imageDir.append(image, pImageSep);
            }
            debugSearchDirs.emplace_back(imageDir);

            // Then search on a .debug subdirectory
            imageDir.append(".debug\\");
            debugSearchDirs.emplace_back(imageDir);

            HANDLE hFile = INVALID_HANDLE_VALUE;
            for (auto const &debugSearchDir : debugSearchDirs) {
                std::string debugImage(debugSearchDir);
                debugImage.append(debuglink);
                const char *debugImageStr = debugImage.c_str();
                hFile = CreateFileA(debugImageStr, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
                if (hFile == INVALID_HANDLE_VALUE) {
                    OutputDebug("MGWHELP: %s - not found\n", debugImageStr);
                } else {
                    res = dwarf_pe_init(hFile, debugImageStr, errhand, errarg, ret_dbg, error);
                    CloseHandle(hFile);
                    break;
                }
            }

            break;
        }
    }

    if (res != DW_DLV_OK) {
        Dwarf_Obj_Access_Interface_a *intfc;

        /* Initialize the interface struct */
        intfc = (Dwarf_Obj_Access_Interface_a *)calloc(1, sizeof *intfc);
        if (!intfc) {
            goto no_intfc;
        }
        intfc->ai_object = pe_obj;
        intfc->ai_methods = &pe_methods;

        res = dwarf_object_init_b(intfc, errhand, errarg, DW_GROUPNUMBER_ANY, ret_dbg, error);
        if (res == DW_DLV_OK) {
            return res;
        }

        // Warn if no symbols found, yet this looks like a PE file generated by
        // MinGW.
        // See also http://reverseengineering.stackexchange.com/a/1826
        PIMAGE_OPTIONAL_HEADER pOptionalHeader;
        pOptionalHeader = &pe_obj->pNtHeaders->OptionalHeader;
        if (pOptionalHeader->MajorLinkerVersion == 2 && pOptionalHeader->MinorLinkerVersion >= 21) {
            OutputDebug("MGWHELP: %s - no dwarf symbols\n", image);
        }

        free(intfc);
    }

no_intfc:
    UnmapViewOfFile(pe_obj->lpFileBase);
no_view_of_file:
    CloseHandle(pe_obj->hFileMapping);
no_file_mapping:
    free(pe_obj);
no_internals:
    return res;
}


int
dwarf_pe_finish(Dwarf_Debug dbg, Dwarf_Error *error)
{
    Dwarf_Obj_Access_Interface_a *intfc = dbg->de_obj_file;
    pe_access_object_t *pe_obj = (pe_access_object_t *)intfc->ai_object;
    free(intfc);
    UnmapViewOfFile(pe_obj->lpFileBase);
    CloseHandle(pe_obj->hFileMapping);
    free(pe_obj);
    *error = nullptr;
    return dwarf_object_finish(dbg);
}
