#include "explorer_sort.h"
#include <shlwapi.h>   // StrStrIW
#include <cstdint>

static const wchar_t* kBagMRU_Primary =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\BagMRU";
static const wchar_t* kBagMRU_Fallback =
    L"Software\\Microsoft\\Windows\\Shell\\BagMRU";
static const wchar_t* kSortSubkeyFmt =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\%u\\Shell\\{5C4F28B5-F869-4E84-8E60-F11DB97C5CC7}";
static const wchar_t* kSortSubkeyFmt_Fallback =
    L"Software\\Microsoft\\Windows\\Shell\\Bags\\%u\\Shell\\{5C4F28B5-F869-4E84-8E60-F11DB97C5CC7}";

// The Sort REG_BINARY value is composed of 24-byte chunks (one per sort column),
// each preceding a 20-byte header. But in practice the data is just a leading
// 20-byte header then 24-byte entries. We only need the first entry (primary key).
// Entry format: 16-byte FMTID GUID (binary, little-endian components) +
//               4-byte PID (LE DWORD) + 4-byte direction (1=asc, 0xFF..FF=desc).

struct SortEntry {
    uint32_t pid;
    bool asc;
};

static const uint8_t kSortFmtidBytes[16] = {
    0x30, 0xF1, 0x25, 0xB7, 0xEF, 0x47, 0x1A, 0x10,
    0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC
};

static bool IsSortFmtid(const BYTE* data, DWORD dataLen) {
    // The sort entries are at offset 20 (after the 20-byte header).
    // Each entry is 24 bytes; loop through entries looking for a match.
    if (dataLen < 44) return false; // header(20) + at least one entry(24)
    DWORD offset = 20;
    while (offset + 24 <= dataLen) {
        if (memcmp(data + offset, kSortFmtidBytes, 16) == 0)
            return true;
        offset += 24;
    }
    return false;
}

static bool ParseSort(const BYTE* data, DWORD dataLen, int& column, bool& ascending) {
    if (dataLen < 44) return false;
    DWORD offset = 20;
    while (offset + 24 <= dataLen) {
        if (memcmp(data + offset, kSortFmtidBytes, 16) != 0) {
            offset += 24;
            continue;
        }
        uint32_t pid = *(uint32_t*)(data + offset + 16);
        uint32_t dir = *(uint32_t*)(data + offset + 20);
        switch (pid) {
        case 0x0A: column = 0; break;   // Name
        case 0x0E: column = 1; break;   // DateModified
        case 0x0C: column = 2; break;   // Size
        case 0x0B: column = 3; break;   // Type
        default:   column = 0; break;
        }
        ascending = (dir != 0xFFFFFFFF);
        return true;
    }
    return false;
}

// Walk the BagMRU tree looking for a node whose binary value's last SHITEMID
// contains `folderName`. The binary value at each BagMRU key is a PIDL:
// a chain of SHITEMIDs, each with a WORD cb prefix. The last SHITEMID's data
// encodes the display name as a UTF-16LE string.
static bool SearchBagMRU(HKEY root, const std::wstring& basePath,
                         const std::wstring& folderName,
                         DWORD& slot, int depth) {
    if (depth > 8) return false;
    HKEY hKey;
    if (RegOpenKeyExW(root, basePath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    bool found = false;
    DWORD idx = 0;
    wchar_t subName[16];
    DWORD subNameLen;
    for (;;) {
        subNameLen = _countof(subName);
        LONG lr = RegEnumKeyExW(hKey, idx++, subName, &subNameLen, nullptr, nullptr, nullptr, nullptr);
        if (lr != ERROR_SUCCESS) break;

        HKEY hSub;
        if (RegOpenKeyExW(hKey, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
            continue;

        // Read NodeSlot
        DWORD nodeSlot = 0;
        DWORD slotSize = sizeof(nodeSlot);
        RegQueryValueExW(hSub, L"NodeSlot", nullptr, nullptr, (LPBYTE)&nodeSlot, &slotSize);

        // Read the binary PIDL value (same name as the key)
        BYTE pidlData[1024];
        DWORD pidlLen = sizeof(pidlData);
        if (RegGetValueW(hSub, nullptr, subName, RRF_RT_REG_BINARY, nullptr,
                         pidlData, &pidlLen) == ERROR_SUCCESS && pidlLen >= 8) {

            // Walk SHITEMID chain to find the last one. Each is cb (WORD) + data.
            DWORD pos = 2; // skip the first cb (desktop/root item)
            DWORD lastDataOff = 0, lastDataLen = 0;
            while (pos + 2 <= pidlLen) {
                WORD cb = *(WORD*)(pidlData + pos);
                if (cb < 2 || pos + cb > pidlLen) break;
                lastDataOff = pos + 2;
                lastDataLen = cb - 2;
                pos += cb;
                if (cb == 0) break;
            }

            // Search for folder name as UTF-16LE substring in the last SHITEMID data
            if (lastDataLen >= folderName.size() * 2) {
                // Build a lowercased copy for case-insensitive comparison
                std::wstring itemText((const wchar_t*)(pidlData + lastDataOff),
                                      lastDataLen / 2);
                for (auto& ch : itemText) ch = (wchar_t)towlower(ch);
                std::wstring lowName = folderName;
                for (auto& ch : lowName) ch = (wchar_t)towlower(ch);

                if (itemText.find(lowName) != std::wstring::npos) {
                    slot = nodeSlot;
                    found = true;
                    RegCloseKey(hSub);
                    break;
                }
            }
        }

        RegCloseKey(hSub);
        if (found) break;
    }
    RegCloseKey(hKey);

    if (found) return true;

    // Recurse into sub-nodes
    if (RegOpenKeyExW(root, basePath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    idx = 0;
    while (!found) {
        subNameLen = _countof(subName);
        LONG lr = RegEnumKeyExW(hKey, idx++, subName, &subNameLen, nullptr, nullptr, nullptr, nullptr);
        if (lr != ERROR_SUCCESS) break;
        std::wstring childPath = basePath.empty() ? std::wstring(subName)
                                                 : basePath + L"\\" + subName;
        RegCloseKey(hKey); // must close before recursion to avoid handle leak
        found = SearchBagMRU(root, childPath, folderName, slot, depth + 1);
        if (found) return true;
        if (RegOpenKeyExW(root, basePath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return false;
    }
    RegCloseKey(hKey);
    return false;
}

bool QueryExplorerSortForFolder(const std::wstring& folder, int& column, bool& ascending) {
    std::wstring folderName = PathFindFileNameW(folder.c_str());
    if (folderName.empty()) return false;

    // Prefer the UsrClass.dat-based bag (primary), fall back to the older NTUSER hive
    const wchar_t* paths[] = { kBagMRU_Primary, kBagMRU_Fallback };
    const wchar_t* sortFmts[] = { kSortSubkeyFmt, kSortSubkeyFmt_Fallback };

    for (int tryRoot = 0; tryRoot < 2; ++tryRoot) {
        HKEY hRoot;
        LSTATUS ls = RegOpenKeyExW(HKEY_CURRENT_USER, paths[tryRoot], 0, KEY_READ, &hRoot);
        if (ls != ERROR_SUCCESS) continue;

        DWORD slot = 0;
        if (SearchBagMRU(hRoot, L"", folderName, slot, 0)) {
            RegCloseKey(hRoot);

            wchar_t sortPath[256];
            _snwprintf_s(sortPath, _countof(sortPath), _TRUNCATE, sortFmts[tryRoot], slot);
            BYTE sortData[256];
            DWORD sortLen = sizeof(sortData);
            if (RegGetValueW(HKEY_CURRENT_USER, sortPath, L"Sort",
                             RRF_RT_REG_BINARY, nullptr, sortData, &sortLen) == ERROR_SUCCESS) {
                if (ParseSort(sortData, sortLen, column, ascending))
                    return true;
            }
            return false; // bag found but no usable Sort → don't try fallback
        }
        RegCloseKey(hRoot);
    }
    return false;
}
