param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [uint64[]] $Offsets
)

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Sym {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct SYMBOL_INFO {
        public uint  SizeOfStruct;
        public uint  TypeIndex;
        public ulong Reserved0;
        public ulong Reserved1;
        public uint  Index;
        public uint  Size;
        public ulong ModBase;
        public uint  Flags;
        public ulong Value;
        public ulong Address;
        public uint  Register;
        public uint  Scope;
        public uint  Tag;
        public uint  NameLen;
        public uint  MaxNameLen;
        // Name follows inline. We'll allocate manually.
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct IMAGEHLP_LINE64 {
        public uint   SizeOfStruct;
        public IntPtr Key;
        public uint   LineNumber;
        public IntPtr FileName;
        public ulong  Address;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern uint SymSetOptions(uint SymOptions);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern ulong SymLoadModuleEx(IntPtr hProcess, IntPtr hFile, string ImageName, string ModuleName,
        ulong BaseOfDll, uint DllSize, IntPtr Data, uint Flags);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymFromAddr(IntPtr hProcess, ulong Address, out ulong Displacement, IntPtr Symbol);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymGetLineFromAddr64(IntPtr hProcess, ulong Address, out uint Displacement, ref IMAGEHLP_LINE64 Line);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymCleanup(IntPtr hProcess);
}
'@

Add-Type -TypeDefinition $src -ErrorAction Stop

$proc = [System.Diagnostics.Process]::GetCurrentProcess().Handle
[Sym]::SymSetOptions([uint32](0x10 -bor 0x10000)) | Out-Null  # UNDNAME|LOAD_LINES
$searchPath = [System.IO.Path]::GetDirectoryName((Resolve-Path $Exe))
if (-not [Sym]::SymInitialize($proc, $searchPath, $false)) { throw "SymInitialize failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

$base = [Sym]::SymLoadModuleEx($proc, [IntPtr]::Zero, $Exe, $null, [uint64]0, [uint32]0, [IntPtr]::Zero, [uint32]0)
if ($base -eq 0) { throw "SymLoadModuleEx failed: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

$nameMax = 1024
$totalSize = 0x58 + $nameMax  # sizeof(SYMBOL_INFO) ~ 88 + name buffer
$buf = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($totalSize)
try {
    foreach ($off in $Offsets) {
        # zero
        for ($i = 0; $i -lt $totalSize; $i++) { [System.Runtime.InteropServices.Marshal]::WriteByte($buf, $i, 0) }
        # SizeOfStruct = 88, MaxNameLen at offset 84
        [System.Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, 88)
        [System.Runtime.InteropServices.Marshal]::WriteInt32($buf, 84, $nameMax)

        $addr = $base + $off
        $disp = [uint64] 0
        $ok = [Sym]::SymFromAddr($proc, $addr, [ref]$disp, $buf)
        if ($ok) {
            $nameLen = [System.Runtime.InteropServices.Marshal]::ReadInt32($buf, 80)
            $namePtr = [IntPtr]::Add($buf, 88)
            $name = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi($namePtr, [Math]::Min($nameLen, $nameMax - 1))

            $line = New-Object Sym+IMAGEHLP_LINE64
            $line.SizeOfStruct = [uint32][System.Runtime.InteropServices.Marshal]::SizeOf([type][Sym+IMAGEHLP_LINE64])
            $lineDisp = [uint32] 0
            $haveLine = [Sym]::SymGetLineFromAddr64($proc, $addr, [ref]$lineDisp, [ref]$line)
            if ($haveLine) {
                $file = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi($line.FileName)
                "{0,-10} {1} +0x{2:x}    {3}:{4}" -f ("0x{0:x}" -f $off), $name, $disp, $file, $line.LineNumber
            } else {
                "{0,-10} {1} +0x{2:x}" -f ("0x{0:x}" -f $off), $name, $disp
            }
        } else {
            "{0,-10} <unresolved> err={1}" -f ("0x{0:x}" -f $off), [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        }
    }
} finally {
    [System.Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
    [Sym]::SymCleanup($proc) | Out-Null
}
