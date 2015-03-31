/*
 * Copyright 2013-2014 Jose Fonseca
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


#include "tap.h"

#include <stdlib.h>

#include <windows.h>
#include <dbghelp.h>


static void
checkSym(HANDLE hProcess,
         const void *symbol,
         const char *szSymbolName)
{
    bool ok;

    DWORD64 dwAddr = (DWORD64)(UINT_PTR)symbol;

    // Test SymFromAddr
    DWORD64 Displacement = 0;
    struct {
        SYMBOL_INFO Symbol;
        CHAR Name[256];
    } s;
    memset(&s, 0, sizeof s);
    s.Symbol.SizeOfStruct = sizeof s.Symbol;
    s.Symbol.MaxNameLen = sizeof s.Symbol.Name + sizeof s.Name;
    ok = SymFromAddr(hProcess, dwAddr, &Displacement, &s.Symbol);
    test_line(ok, "SymFromAddr(&%s)", szSymbolName);
    if (!ok) {
        test_diagnostic_last_error();
    } else {
        ok = strcmp(s.Symbol.Name, szSymbolName) == 0;
        test_line(ok, "SymFromAddr(&%s).Name", szSymbolName);
        if (!ok) {
            test_diagnostic("Name = \"%s\" != \"%s\"",
                            s.Symbol.Name, szSymbolName);
        }
    }
}


static void
checkSymLine(HANDLE hProcess,
             const void *symbol,
             const char *szSymbolName,
             const char *szFileName,
             DWORD dwLineNumber)
{
    bool ok;

    DWORD64 dwAddr = (DWORD64)(UINT_PTR)symbol;

    checkSym(hProcess, symbol, szSymbolName);

    // Test SymGetLineFromAddr64
    DWORD dwDisplacement;
    IMAGEHLP_LINE64 Line;
    ZeroMemory(&Line, sizeof Line);
    Line.SizeOfStruct = sizeof Line;
    ok = SymGetLineFromAddr64(hProcess, dwAddr, &dwDisplacement, &Line);
    test_line(ok, "SymGetLineFromAddr64(&%s)", szSymbolName);
    if (!ok) {
        test_diagnostic_last_error();
    } else {
        ok = strcmp(Line.FileName, szFileName) == 0;
        test_line(ok, "SymGetLineFromAddr64(&%s).FileName", szSymbolName);
        if (!ok) {
            test_diagnostic("FileName = \"%s\" != \"%s\"",
                            Line.FileName, szFileName);
        }
        ok = Line.LineNumber == dwLineNumber;
        test_line(ok, "SymGetLineFromAddr64(&%s).LineNumber", szSymbolName);
        if (Line.LineNumber != dwLineNumber) {
            test_diagnostic("LineNumber = %lu != %lu",
                            Line.LineNumber, dwLineNumber);
        }
    }
}


static void
checkCaller(HANDLE hProcess,
            const char *szSymbolName,
            const char *szFileName,
            DWORD dwLineNumber)
{
    void *addr = __builtin_return_address(0);
    checkSymLine(hProcess, addr, szSymbolName, szFileName, dwLineNumber);
}


static void
checkExport(HANDLE hProcess,
            const char *szModuleName,
            const char *szSymbolName)
{
    HMODULE hModule = GetModuleHandleA(szModuleName);
    const void *pvSymbol = GetProcAddress(hModule, szSymbolName);
    checkSym(hProcess, pvSymbol, szSymbolName);
}



static const DWORD foo_line = __LINE__; static int foo(int a, int b) {
    return a * b;
}


static void dummy(void)
{
    getenv("USERPROFILE");
}


int
main()
{
    HMODULE hProcess = GetCurrentProcess();
    bool ok;

    ok = SymInitialize(hProcess, "", TRUE);
    test_line(ok, "SymInitialize()");
    if (!ok) {
        test_diagnostic_last_error();
    } {
        checkSymLine(hProcess, &foo, "foo", __FILE__, foo_line);

        checkCaller(hProcess, "main", __FILE__, __LINE__); dummy();

        // Test DbgHelp fallback
        checkExport(hProcess, "kernel32", "Sleep");

        ok = SymCleanup(hProcess);
        test_line(ok, "SymCleanup()");
        if (!ok) {
            test_diagnostic_last_error();
        }
    }

    test_exit();
}