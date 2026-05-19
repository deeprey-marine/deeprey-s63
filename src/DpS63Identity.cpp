/******************************************************************************
 *
 * Project:  OpenCPN / Deeprey
 * Purpose:  S63 device-identity store implementation
 *
 ***************************************************************************
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************
 */

#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/textfile.h>

#include "DpS63Identity.h"
#include "dsa/sha1.h"
#include "s63_pi.h"

extern wxString g_CommonDataDir;

namespace DpS63 {

namespace {

wxString s_cachedDeviceId;

// SHA-1 over the bytes of filePath. Returns 20 raw bytes in digest[]; false if
// the file could not be read.
bool Sha1OfFile(const wxString& filePath, uint8_t digest[SHA1HashSize]) {
    wxFFile f(filePath, "rb");
    if (!f.IsOpened()) return false;

    SHA1Context ctx;
    if (SHA1Reset(&ctx) != shaSuccess) return false;

    uint8_t buf[8192];
    size_t n;
    while ((n = f.Read(buf, sizeof(buf))) > 0) {
        if (SHA1Input(&ctx, buf, static_cast<unsigned int>(n)) != shaSuccess)
            return false;
    }
    return SHA1Result(&ctx, digest) == shaSuccess;
}

// "XXXX-XXXX-XXXX-XXXX" from the first 8 bytes (16 hex chars) of the digest.
wxString FormatDeviceIdHandle(const uint8_t digest[SHA1HashSize]) {
    return wxString::Format(
        "%02X%02X-%02X%02X-%02X%02X-%02X%02X",
        digest[0], digest[1], digest[2], digest[3],
        digest[4], digest[5], digest[6], digest[7]);
}

// Locate the FPR path printed by "OCPNsenc -w -o <dir>". The utility prints
// a line of the form "FPR file: <path>" -- match on "FPR" and take the
// substring after the first colon.
wxString ParseFprPathFromOutput(const wxArrayString& output) {
    for (const wxString& line : output) {
        if (line.Upper().Find("ERROR") != wxNOT_FOUND) return wxString();
        if (line.Upper().Find("FPR") != wxNOT_FOUND) {
            wxString p = line.AfterFirst(':');
            p.Trim().Trim(false);
            if (wxFileExists(p)) return p;
        }
    }
    return wxString();
}

bool LoadCachedDeviceId() {
    wxString idPath = DpS63Identity::GetIdentityDir() + "DEVICE_ID.txt";
    if (!wxFileExists(idPath)) return false;
    wxFFile f(idPath, "rb");
    if (!f.IsOpened()) return false;
    wxString contents;
    if (!f.ReadAll(&contents)) return false;
    contents.Trim().Trim(false);
    if (contents.IsEmpty()) return false;
    s_cachedDeviceId = contents;
    return true;
}

bool WriteDeviceIdTxt(const wxString& handle) {
    wxFFile f(DpS63Identity::GetIdentityDir() + "DEVICE_ID.txt", "wb");
    if (!f.IsOpened()) return false;
    return f.Write(handle + "\n");
}

bool WriteProvisionedSentinel(const wxString& handle) {
    wxString json;
    json << "{\n";
    json << "  \"deviceId\": \"" << handle << "\",\n";
    json << "  \"createdAtUtc\": \"" << wxDateTime::Now().ToUTC().FormatISOCombined() << "Z\"\n";
    json << "}\n";
    wxFFile f(DpS63Identity::GetIdentityDir() + "provisioned.json", "wb");
    if (!f.IsOpened()) return false;
    return f.Write(json);
}

bool ProvisionNow() {
    wxString dir = DpS63Identity::GetIdentityDir();
    if (!wxFileName::DirExists(dir)) {
        if (!wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL)) {
            wxLogMessage("s63_pi: identity dir create failed: " + dir);
            return false;
        }
    }

    wxString cmd = " -w -o \"" + dir + "\"";
    wxArrayString out = exec_SENCutil_sync(cmd, false);
    wxString fprPath = ParseFprPathFromOutput(out);
    if (fprPath.IsEmpty()) {
        wxLogMessage("s63_pi: OCPNsenc -w -o produced no FPR file");
        return false;
    }

    // Normalize to the canonical name so the export and re-read paths don't
    // have to scan the directory.
    wxString canonical = DpS63Identity::GetFingerprintPath();
    if (fprPath != canonical) {
        if (wxFileExists(canonical)) ::wxRemoveFile(canonical);
        if (!::wxRenameFile(fprPath, canonical)) {
            wxLogMessage("s63_pi: failed to rename " + fprPath + " -> " + canonical);
            return false;
        }
    }

    uint8_t digest[SHA1HashSize];
    if (!Sha1OfFile(canonical, digest)) {
        wxLogMessage("s63_pi: SHA-1 over fingerprint failed");
        return false;
    }
    wxString handle = FormatDeviceIdHandle(digest);

    if (!WriteDeviceIdTxt(handle)) {
        wxLogMessage("s63_pi: DEVICE_ID.txt write failed");
        return false;
    }
    if (!WriteProvisionedSentinel(handle)) {
        wxLogMessage("s63_pi: provisioned.json write failed");
        return false;
    }

    s_cachedDeviceId = handle;
    wxLogMessage("s63_pi: identity provisioned, device id = " + handle);
    return true;
}

}  // namespace

wxString DpS63Identity::GetIdentityDir() {
    wxString dir = g_CommonDataDir;
    if (!dir.IsEmpty() && dir.Last() != wxFileName::GetPathSeparator())
        dir += wxFileName::GetPathSeparator();
    dir += "identity";
    dir += wxFileName::GetPathSeparator();
    return dir;
}

wxString DpS63Identity::GetFingerprintPath() {
    return GetIdentityDir() + "fingerprint.fpr";
}

wxString DpS63Identity::GetDeviceId() {
    if (s_cachedDeviceId.IsEmpty()) LoadCachedDeviceId();
    return s_cachedDeviceId;
}

bool DpS63Identity::EnsureProvisioned() {
    if (!s_cachedDeviceId.IsEmpty() && wxFileExists(GetFingerprintPath()))
        return true;

    wxString sentinel = GetIdentityDir() + "provisioned.json";
    if (wxFileExists(sentinel) && wxFileExists(GetFingerprintPath())) {
        if (LoadCachedDeviceId()) return true;
        // Sentinel present but DEVICE_ID.txt unreadable -- fall through and
        // re-run provisioning to rebuild a usable store.
    }

    return ProvisionNow();
}

}  // namespace DpS63
