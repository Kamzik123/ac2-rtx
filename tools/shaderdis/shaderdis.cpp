// shaderdis - offline D3D9 shader disassembler for AC2 extracted MaterialTemplate blobs.
//
// The extracted *.dxbc files are RAW D3D9 bytecode (no container): the first
// dword is 0xFFFE0300 (vs_3_0) or 0xFFFF0300 (ps_3_0), followed by a comment
// token holding "CTAB" (the constant table). That is byte-identical to what the
// game hands to CreateVertexShader, so the same D3DX calls work on them.
//
// Must be built as x86: it LoadLibrary's the 32-bit d3dx9_42.dll.
//
//   usage: shaderdis <input_dir> <output_dir>
//
// Emits <output_dir>\<name>.asm (constant table as header comments + disasm)
// and <output_dir>\index.txt with one summary line per shader:
//   <name> | <ver> | dcl:<inputs> | const:<name>@<reg>x<count>,...

#include <windows.h>
#include <d3dx9.h>

#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

using PFN_Dis = HRESULT(WINAPI*)(const DWORD*, BOOL, LPCSTR, LPD3DXBUFFER*);
using PFN_CT = HRESULT(WINAPI*)(const DWORD*, LPD3DXCONSTANTTABLE*);

static PFN_Dis p_dis = nullptr;
static PFN_CT  p_ct = nullptr;

static bool load_d3dx()
{
    // A 32-bit process gets SysWOW64 automatically via filesystem redirection.
    const char* names[] = { "d3dx9_42.dll", "d3dx9_43.dll", "d3dx9_41.dll", "d3dx9_40.dll" };
    for (const char* n : names)
    {
        HMODULE m = LoadLibraryA(n);
        if (!m) continue;
        p_dis = reinterpret_cast<PFN_Dis>(GetProcAddress(m, "D3DXDisassembleShader"));
        p_ct = reinterpret_cast<PFN_CT>(GetProcAddress(m, "D3DXGetShaderConstantTable"));
        if (p_dis && p_ct) { printf("using %s\n", n); return true; }
    }
    return false;
}

static const char* reg_set_prefix(D3DXREGISTER_SET s)
{
    switch (s)
    {
    case D3DXRS_BOOL:    return "b";
    case D3DXRS_INT4:    return "i";
    case D3DXRS_FLOAT4:  return "c";
    case D3DXRS_SAMPLER: return "s";
    default:             return "?";
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: shaderdis <input_dir> <output_dir>\n");
        return 1;
    }
    if (!load_d3dx())
    {
        printf("ERROR: could not load a d3dx9 dll with the required exports\n");
        return 2;
    }

    const fs::path in_dir = argv[1];
    const fs::path out_dir = argv[2];

    std::error_code ec;
    fs::create_directories(out_dir, ec);

    std::ofstream index(out_dir / "index.txt");

    int ok = 0, bad = 0;

    for (const auto& e : fs::directory_iterator(in_dir, ec))
    {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".dxbc") continue;

        // read whole file
        std::ifstream f(e.path(), std::ios::binary);
        if (!f) { bad++; continue; }
        std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (buf.size() < 8) { bad++; continue; }

        const DWORD* code = reinterpret_cast<const DWORD*>(buf.data());

        // sanity: must start with a shader version token
        const DWORD v = code[0];
        const bool is_vs = (v & 0xFFFF0000) == 0xFFFE0000;
        const bool is_ps = (v & 0xFFFF0000) == 0xFFFF0000;
        if (!is_vs && !is_ps) { bad++; continue; }

        char ver[32];
        sprintf_s(ver, "%s_%u_%u", is_vs ? "vs" : "ps", (v >> 8) & 0xFF, v & 0xFF);

        const std::string stem = e.path().stem().string();
        std::ostringstream head;
        head << "; " << e.path().filename().string() << "\n";
        head << "; version: " << ver << "\n";
        head << "; bytes  : " << buf.size() << "\n;\n";

        std::ostringstream consts; // for index.txt

        LPD3DXCONSTANTTABLE ct = nullptr;
        if (SUCCEEDED(p_ct(code, &ct)) && ct)
        {
            D3DXCONSTANTTABLE_DESC d{};
            if (SUCCEEDED(ct->GetDesc(&d)))
            {
                head << "; ---- constant table (" << d.Constants << ") ----\n";
                for (UINT i = 0; i < d.Constants; ++i)
                {
                    D3DXHANDLE h = ct->GetConstant(nullptr, i);
                    if (!h) continue;
                    D3DXCONSTANT_DESC cd{};
                    UINT n = 1;
                    if (SUCCEEDED(ct->GetConstantDesc(h, &cd, &n)) && cd.Name)
                    {
                        const char* p = reg_set_prefix(cd.RegisterSet);
                        head << ";   " << p << cd.RegisterIndex
                             << " .. " << p << (cd.RegisterIndex + cd.RegisterCount - 1)
                             << "  (" << cd.RegisterCount << ")  " << cd.Name << "\n";
                        consts << cd.Name << "@" << p << cd.RegisterIndex
                               << "x" << cd.RegisterCount << ",";
                    }
                }
                head << ";\n";
            }
            ct->Release();
        }

        std::string asm_text;
        LPD3DXBUFFER b = nullptr;
        if (SUCCEEDED(p_dis(code, FALSE, nullptr, &b)) && b)
        {
            asm_text.assign(static_cast<const char*>(b->GetBufferPointer()), b->GetBufferSize());
            b->Release();
        }

        {
            std::ofstream o(out_dir / (stem + ".asm"));
            o << head.str() << asm_text;
        }

        // collect dcl_ input declarations for the index
        std::ostringstream dcls;
        {
            std::istringstream is(asm_text);
            std::string line;
            while (std::getline(is, line))
            {
                const auto p = line.find("dcl_");
                if (p == std::string::npos) continue;
                if (line.find(" v") == std::string::npos) continue; // inputs only
                std::string t = line.substr(p);
                while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
                dcls << t << ";";
            }
        }

        index << stem << " | " << ver
              << " | dcl:" << dcls.str()
              << " | const:" << consts.str() << "\n";
        ok++;
    }

    printf("disassembled: %d   skipped: %d\n", ok, bad);
    return 0;
}
