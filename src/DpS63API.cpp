/******************************************************************************
 *
 * Project:  OpenCPN / Deeprey
 * Purpose:  Deeprey S63 API implementation
 *
 * Implements DpS63::DpS63API by delegating to the s63_pi plugin. deeprey-gui
 * receives the DpS63API pointer via SendPluginMessage("S63_API_TO_DP_GUI", ...)
 * and drives S63 chart management through this surface.
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

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/textfile.h>
#include <wx/tokenzr.h>

#include <string>

#include <curl/curl.h>

#include "s63_pi.h"
#include "DpS63API.h"
#include "DpS63Identity.h"
#include "jsonreader.h"
#include "jsonval.h"
#include "jsonwriter.h"

//  Globals owned by s63_pi.cpp.
extern wxString g_userpermit;
extern wxString g_installpermit;
extern wxString g_fpr_file;
extern wxString g_SENCdir;
extern wxString g_shop_base_url;
extern wxString g_activation_token;

namespace {

// libcurl write callback: append the received bytes to a std::string.
size_t CurlAppend(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// POST jsonBody to url with "Content-Type: application/json". Fills response
// with the body and httpCode with the status. Returns true only on a 2xx
// response; on transport failure httpCode is left 0 and netErr describes it.
bool HttpPostJson(const wxString& url, const wxString& jsonBody,
                  wxString& response, long& httpCode, wxString& netErr) {
    httpCode = 0;
    CURL* curl = curl_easy_init();
    if (!curl) {
        netErr = "curl init failed";
        return false;
    }

    std::string body(jsonBody.utf8_str());
    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, static_cast<const char*>(url.utf8_str()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlAppend);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "deeprey-s63");

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    response = wxString::FromUTF8(resp.c_str());

    if (rc != CURLE_OK) {
        netErr = wxString::FromUTF8(curl_easy_strerror(rc));
        wxLogMessage(_T("DpS63 HTTP %s -> transport error rc=%d (%s)"),
                     url.c_str(), (int)rc, netErr.c_str());
        return false;
    }
    if (httpCode < 200 || httpCode >= 300) {
        wxLogMessage(_T("DpS63 HTTP %s -> status %ld"), url.c_str(), httpCode);
        return false;
    }
    return true;
}

}  // namespace

namespace DpS63 {

DpS63API::DpS63API(s63_pi* plugin) : m_plugin(plugin) {}

DpS63API::~DpS63API() {}

// ---------------------------------------------------------------------------
//  Storage locations
// ---------------------------------------------------------------------------

wxString DpS63API::GetChartDir() {
    return m_plugin ? m_plugin->GetPermitDir() : wxString();
}

wxString DpS63API::GetCertificateDir() {
    return m_plugin ? m_plugin->GetCertificateDir() : wxString();
}

wxString DpS63API::GetSencDir() {
    return g_SENCdir;
}

// ---------------------------------------------------------------------------
//  Installed cell enumeration
// ---------------------------------------------------------------------------

// Parse a single .os63 permit file into a DpS63CellInfo. Mirrors the cellpermit
// parsing in OCPNPermitList::BuildList(): the permit string carries the cell
// name (chars 0-7) and the ENC expiry date (chars 8-15, %Y%m%d); the line tail
// after ':' is comma-separated permit,serviceLevel,edition,dataServerID.
static bool ParseOs63File(const wxString& path, DpS63CellInfo& out) {
    wxTextFile file(path);
    if (!file.Open()) return false;

    bool found = false;
    for (wxString line = file.GetFirstLine(); !file.Eof();
         line = file.GetNextLine()) {
        if (!line.StartsWith(_T("cellpermit"))) continue;

        wxString permit_string = line.Mid(11);
        out.cellName = permit_string.Mid(0, 8);

        wxString sdate = permit_string.Mid(8, 8);
        out.expiryDate.ParseFormat(sdate, _T("%Y%m%d"));

        wxStringTokenizer tkz(line.AfterFirst(':'), _T(","));
        tkz.GetNextToken();                       // permit
        tkz.GetNextToken();                       // service level
        out.edition = tkz.GetNextToken();         // edition
        out.producer = tkz.GetNextToken();        // data server ID

        out.permitFile = path;
        out.sizeBytes = wxFileName::GetSize(path).GetValue();

        if (out.expiryDate.IsValid() && out.expiryDate < wxDateTime::Now())
            out.status = DpS63CellStatus::PERMIT_EXPIRED;
        else
            out.status = DpS63CellStatus::INSTALLED;

        found = true;
        break;
    }
    return found;
}

std::vector<DpS63CellInfo> DpS63API::GetInstalledCells() {
    std::vector<DpS63CellInfo> cells;
    wxString dir = GetChartDir();
    if (dir.IsEmpty() || !wxDir::Exists(dir)) return cells;

    wxArrayString files;
    wxDir::GetAllFiles(dir, &files, _T("*.os63"));
    for (const wxString& f : files) {
        DpS63CellInfo info;
        if (ParseOs63File(f, info)) cells.push_back(info);
    }
    return cells;
}

DpS63CellInfo DpS63API::GetCellInfo(const wxString& cellName) {
    for (const DpS63CellInfo& c : GetInstalledCells()) {
        if (c.cellName.IsSameAs(cellName, false)) return c;
    }
    DpS63CellInfo empty;
    empty.cellName = cellName;
    empty.status = DpS63CellStatus::NOT_INSTALLED;
    return empty;
}

bool DpS63API::RemoveCell(const wxString& cellName) {
    DpS63CellInfo info = GetCellInfo(cellName);
    if (info.permitFile.IsEmpty() || !wxFileExists(info.permitFile))
        return false;

    RemoveChartFromDBInPlace(info.permitFile);
    bool ok = ::wxRemoveFile(info.permitFile);
    if (ok) NotifyStateChanged();
    return ok;
}

// ---------------------------------------------------------------------------
//  Certificate management
// ---------------------------------------------------------------------------

std::vector<DpS63CertificateInfo> DpS63API::GetCertificates() {
    std::vector<DpS63CertificateInfo> certs;
    wxString dir = GetCertificateDir();
    if (dir.IsEmpty() || !wxDir::Exists(dir)) return certs;

    wxArrayString files;
    wxDir::GetAllFiles(dir, &files, _T("*.PUB"));
    for (const wxString& f : files) {
        DpS63CertificateInfo info;
        wxFileName fn(f);
        info.name = fn.GetFullName();
        info.path = f;
        info.isDefaultIHO = info.name.IsSameAs(_T("IHO.PUB"), false);
        certs.push_back(info);
    }
    return certs;
}

bool DpS63API::ImportCertificate(const wxString& pubFilePath) {
    if (!m_plugin || !wxFileExists(pubFilePath)) return false;

    m_plugin->m_apiCertFileOverride = pubFilePath;
    int rv = m_plugin->ImportCert();
    m_plugin->m_apiCertFileOverride.Clear();

    if (rv == 0) NotifyStateChanged();
    return rv == 0;
}

bool DpS63API::RemoveCertificate(const wxString& certName) {
    wxString dir = GetCertificateDir();
    if (dir.IsEmpty()) return false;

    wxString path = dir + wxFileName::GetPathSeparator() + certName;
    if (!wxFileExists(path)) return false;

    bool ok = ::wxRemoveFile(path);
    if (ok) NotifyStateChanged();
    return ok;
}

// ---------------------------------------------------------------------------
//  Permit / hardware identity
// ---------------------------------------------------------------------------

DpS63PermitStatus DpS63API::GetPermitStatus() {
    DpS63PermitStatus status;
    status.userpermit = g_userpermit;
    status.installpermit = g_installpermit;
    status.fingerprint = g_fpr_file;
    status.hasUserpermit =
        g_userpermit.Len() && !g_userpermit.IsSameAs(_T("X"));
    status.hasInstallpermit =
        g_installpermit.Len() && !g_installpermit.IsSameAs(_T("Y"));
    return status;
}

bool DpS63API::SetUserpermit(const wxString& userpermit) {
    if (!m_plugin) return false;
    g_userpermit = userpermit;
    m_plugin->SaveConfig();
    NotifyStateChanged();
    return true;
}

bool DpS63API::SetInstallpermit(const wxString& installpermit) {
    if (!m_plugin) return false;
    g_installpermit = installpermit;
    m_plugin->SaveConfig();
    NotifyStateChanged();
    return true;
}

// Scan OCPNsenc output for an "ERROR" marker (the convention every utility
// command follows) -- true if any line reports an error.
static bool SencOutputHasError(const wxArrayString& out) {
    for (const wxString& line : out)
        if (line.Upper().Find(_T("ERROR")) != wxNOT_FOUND) return true;
    return false;
}

// Run OCPNsenc -x -i <input> (v2.01+), which prints md5(input + salt) using
// OCPNsenc's embedded server salt. Returns the lowercase 32-char hex digest, or
// an empty string if the tool produced none. The md5 salt is a server secret
// baked into OCPNsenc, so the open-source plugin uses this to build the
// /confirm request token and to verify shop response tokens -- it cannot
// compute any of these itself.
static wxString SencComputeToken(const wxString& input) {
    wxString cmd = _T(" -x -i \"") + input + _T("\"");
    wxArrayString out = exec_SENCutil_sync(cmd, false);
    for (const wxString& line : out) {
        wxString t = line;
        t.Trim().Trim(false);
        if (t.Len() != 32) continue;
        bool hex = true;
        for (size_t i = 0; i < t.Len(); ++i)
            if (!wxIsxdigit(t[i])) { hex = false; break; }
        if (hex) return t.Lower();
    }
    return wxString();
}

// Read a string member from a wxJSON object. The bundled wxJSON (src/wxJSON)
// has HasMember(), Get() and GetMemberNames() stubbed out -- their map-lookup
// bodies are commented, so they always report "not present" even when the
// member exists (Size() and operator[] still work). Member access therefore has
// to go through operator[], which reads the internal map directly; a missing key
// auto-inserts an invalid value that IsString() correctly rejects.
static wxString JsonStr(wxJSONValue& obj, const wxChar* key) {
    wxJSONValue& v = obj[key];
    return v.IsString() ? v.AsString() : wxString();
}

void DpS63API::RequestActivation(ActivationProgressCallback onProgress,
                                 ActivationCompleteCallback onComplete) {
    auto stage = [&](int pct, const wxString& msg) {
        if (onProgress) onProgress(pct, msg);
    };
    auto done = [&](DpS63ActivationResult r, const wxString& msg) {
        if (onComplete) onComplete(r, msg);
    };

    if (!m_plugin) {
        done(DpS63ActivationResult::UNKNOWN_ERROR,
             _("The S63 plugin is not available."));
        return;
    }

    //  1. Build the activation request from the device's secure module (TPM).
    //     OCPNsenc -j (v2.00+) writes the complete JSON request body
    //     ({dev, fpr, fprn, t}) directly to the -o output file, with the md5
    //     token computed using OCPNsenc's embedded salt.
    stage(10, _("Preparing activation request..."));

    wxString deviceId = DpS63Identity::GetDeviceId();
    if (deviceId.IsEmpty()) {
        DpS63Identity::EnsureProvisioned();
        deviceId = DpS63Identity::GetDeviceId();
    }
    if (deviceId.IsEmpty()) deviceId = _T("MFD");

    wxString outDir = DpS63Identity::GetIdentityDir();
    if (!wxFileName::DirExists(outDir))
        wxFileName::Mkdir(outDir, 0755, wxPATH_MKDIR_FULL);
    wxString reqPath = outDir + _T("activation_request.json");
    if (wxFileExists(reqPath)) ::wxRemoveFile(reqPath);

    wxString cmd = _T(" -j -i \"") + deviceId + _T("\" -o \"") + reqPath + _T("\"");
    exec_SENCutil_sync(cmd, false);

    //  OCPNsenc's exit status is unreliable here (it logs TPM TCTI lines even on
    //  success), so the reliable success signal is the request file being
    //  written with JSON content.
    wxString requestJson;
    if (wxFileExists(reqPath)) {
        wxFFile f(reqPath, "rb");
        if (f.IsOpened()) f.ReadAll(&requestJson);
        ::wxRemoveFile(reqPath);
    }
    requestJson.Trim().Trim(false);
    if (!requestJson.StartsWith(_T("{"))) {
        done(DpS63ActivationResult::FPR_FAILED,
             _("Could not prepare the activation request. The secure module "
               "may be unavailable on this device."));
        return;
    }

    //  2. POST the request to the o-charts chart-shop server.
    stage(40, _("Contacting the chart shop..."));

    wxString base = g_shop_base_url;
    if (base.IsEmpty()) base = _T("https://test.o-charts.org");
    while (!base.IsEmpty() && base.Last() == '/') base.RemoveLast();
    wxString url = base + _T("/shop/en/module/ocpermits/request");

    wxString response;
    long httpCode = 0;
    wxString netErr;
    if (!HttpPostJson(url, requestJson, response, httpCode, netErr)) {
        if (httpCode == 0) {
            done(DpS63ActivationResult::NO_NETWORK,
                 _("Could not reach the chart shop. Check the internet "
                   "connection and try again."));
        } else {
            done(DpS63ActivationResult::SERVER_ERROR,
                 wxString::Format(
                     _("The chart shop reported a problem (status %ld)."),
                     httpCode));
        }
        return;
    }

    //  3. Parse the returned { "up", "ip", "t" }.
    stage(70, _("Reading activation keys..."));

    wxJSONValue root;
    wxJSONReader reader;
    if (reader.Parse(response, &root) != 0 || !root.IsObject()) {
        done(DpS63ActivationResult::BAD_RESPONSE,
             _("The chart shop response could not be read."));
        return;
    }
    wxString up = JsonStr(root, _T("up"));
    wxString ip = JsonStr(root, _T("ip"));
    wxString token = JsonStr(root, _T("t"));
    up.Trim().Trim(false);
    ip.Trim().Trim(false);
    token.Trim().Trim(false);
    if (up.IsEmpty() || ip.IsEmpty()) {
        done(DpS63ActivationResult::BAD_RESPONSE,
             _("The chart shop did not return valid activation keys."));
        return;
    }

    //  Verify the response token: t = md5(ip + dev + salt). The cryptographic
    //  gate is the OCPNsenc -k validation below (it binds the install permit to
    //  this device's TPM); this token check is an authenticity guard on the shop
    //  response, so a mismatch is logged rather than fatal.
    if (!token.IsEmpty()) {
        wxString expected = SencComputeToken(ip + deviceId);
        if (!expected.IsEmpty() && !expected.IsSameAs(token, false)) {
            wxLogMessage(_T("DpS63: /request response token mismatch ")
                         _T("(got %s, expected %s)"),
                         token.c_str(), expected.c_str());
        }
    }

    //  4. Validate the returned permits against this device (OCPNsenc -k).
    stage(85, _("Validating activation..."));

    wxString vcmd = _T(" -k -u ") + up + _T(" -e ") + ip;
    wxArrayString vout = exec_SENCutil_sync(vcmd, false);
    if (SencOutputHasError(vout)) {
        done(DpS63ActivationResult::VALIDATION_FAILED,
             _("The activation keys could not be validated for this device."));
        return;
    }

    //  5. Persist the activation. The user/install permits become the active
    //     credentials used by every cell-permit and cell-decrypt operation.
    stage(90, _("Finishing..."));
    g_userpermit = up;
    g_installpermit = ip;
    g_activation_token = token;
    m_plugin->SaveConfig();
    NotifyStateChanged();

    //  6. Confirm the activation. POST { dev, t = md5(dev + salt) } to /confirm,
    //     which records the operation for billing. It is idempotent, so a
    //     failure here is non-fatal: the device is already activated and a later
    //     re-activation will record it. The permits above are what make the
    //     charts usable, so we never fail the activation on a confirm hiccup.
    stage(95, _("Confirming activation..."));

    wxString confirmTok = SencComputeToken(deviceId);
    if (confirmTok.IsEmpty()) {
        wxLogMessage(_T("DpS63: could not build /confirm token; ")
                     _T("skipping confirm (activation still valid)."));
    } else {
        //  Build the body by hand: the bundled wxJSONWriter (src/wxJSON) is
        //  stubbed and serializes objects to an empty string, which the shop
        //  rejects with HTTP 400 "Invalid JSON. Syntax error". deviceId is a
        //  fleet serial and confirmTok a hex digest, but escape \ and " for
        //  correctness.
        auto jsonEscape = [](const wxString& s) {
            wxString out;
            for (size_t i = 0; i < s.Len(); ++i) {
                if (s[i] == '\\' || s[i] == '"') out += '\\';
                out += s[i];
            }
            return out;
        };
        wxString cbody = wxString::Format(
            _T("{\"dev\":\"%s\",\"t\":\"%s\"}"),
            jsonEscape(deviceId), jsonEscape(confirmTok));

        wxString curl = base + _T("/shop/en/module/ocpermits/confirm");
        wxString cresp;
        long cCode = 0;
        wxString cErr;
        if (!HttpPostJson(curl, cbody, cresp, cCode, cErr)) {
            wxLogMessage(_T("DpS63: /confirm failed (status %ld, %s); ")
                         _T("activation still valid."),
                         cCode, cErr.c_str());
        } else {
            //  Response { datetime, t = md5(datetime + salt) } -- verify the
            //  token; mismatch is logged, not fatal (see step 3 rationale).
            wxJSONValue croot;
            wxJSONReader creader;
            wxString datetime = (creader.Parse(cresp, &croot) == 0 &&
                                 croot.IsObject())
                                    ? JsonStr(croot, _T("datetime"))
                                    : wxString();
            if (!datetime.IsEmpty()) {
                wxString ctok = JsonStr(croot, _T("t"));
                datetime.Trim().Trim(false);
                ctok.Trim().Trim(false);
                if (!ctok.IsEmpty()) {
                    wxString expected = SencComputeToken(datetime);
                    if (!expected.IsEmpty() &&
                        !expected.IsSameAs(ctok, false)) {
                        wxLogMessage(_T("DpS63: /confirm response token ")
                                     _T("mismatch (got %s, expected %s)"),
                                     ctok.c_str(), expected.c_str());
                    }
                }
                wxLogMessage(_T("DpS63: activation confirmed at %s"),
                             datetime.c_str());
            } else {
                wxLogMessage(_T("DpS63: /confirm response unreadable; ")
                             _T("activation still valid."));
            }
        }
    }

    stage(100, _("Done."));
    done(DpS63ActivationResult::SUCCESS, _("S63 charts activated."));
}

wxString DpS63API::GetDeviceIdString() const {
    return DpS63Identity::GetDeviceId();
}

void DpS63API::ExportActivationFileToUsb(const wxString& usbMountPath,
                                         ExportCompleteCallback onComplete) {
    auto fail = [&](const wxString& msg) {
        if (onComplete) onComplete(false, msg);
    };

    if (usbMountPath.IsEmpty() || !wxDir::Exists(usbMountPath)) {
        fail(_("USB drive not found."));
        return;
    }

    //  Identity may not have been provisioned at plugin init (e.g. OCPNsenc
    //  unavailable). Retry now -- if it still fails, surface a clear error.
    if (!DpS63Identity::EnsureProvisioned()) {
        fail(_("Could not generate the activation file. Check that OCPNsenc is installed."));
        return;
    }

    wxString fprSrc = DpS63Identity::GetFingerprintPath();
    wxString handle = DpS63Identity::GetDeviceId();
    if (!wxFileExists(fprSrc) || handle.IsEmpty()) {
        fail(_("Activation file is not available."));
        return;
    }

    wxString dest = usbMountPath;
    if (dest.Last() != wxFileName::GetPathSeparator())
        dest += wxFileName::GetPathSeparator();

    wxString fprDest = dest + _T("fingerprint.fpr");
    if (!::wxCopyFile(fprSrc, fprDest, true /*overwrite*/)) {
        fail(_("Could not write to the USB drive."));
        return;
    }

    wxString idDest = dest + _T("DEVICE_ID.txt");
    wxTextFile idTxt(idDest);
    if (idTxt.Exists()) ::wxRemoveFile(idDest);
    if (!idTxt.Create()) {
        fail(_("Could not write DEVICE_ID.txt to the USB drive."));
        return;
    }
    idTxt.AddLine(handle);
    idTxt.Write();
    idTxt.Close();

    if (onComplete)
        onComplete(true, _("Saved activation file to USB."));
}

wxString DpS63API::CreateFingerprintFile(const wxString& targetDir) {
    wxString dir = targetDir;
    if (dir.IsEmpty()) return wxString();
    if (dir.Last() != wxFileName::GetPathSeparator())
        dir += wxFileName::GetPathSeparator();

    //  Invoke OCPNsenc to write a fingerprint (XFPR) file. Same command the
    //  native OnNewFPRClick handler uses, minus the confirmation dialogs.
    wxString cmd = _T(" -w -o ") + wxString('\"') + dir + wxString('\"');
    wxArrayString result = exec_SENCutil_sync(cmd, false);

    wxString fpr_file;
    bool err = false;
    for (const wxString& line : result) {
        if (line.Upper().Find(_T("ERROR")) != wxNOT_FOUND) {
            err = true;
            break;
        }
        if (line.Upper().Find(_T("FPR")) != wxNOT_FOUND)
            fpr_file = line.AfterFirst(':').Trim().Trim(false);
    }

    if (err || fpr_file.IsEmpty()) return wxString();

    g_fpr_file = fpr_file;
    if (m_plugin) {
        m_plugin->Set_FPR();
        m_plugin->SaveConfig();
    }
    NotifyStateChanged();
    return fpr_file;
}

// ---------------------------------------------------------------------------
//  Import (offline, file-based)
// ---------------------------------------------------------------------------

void DpS63API::ImportPermitFile(const wxString& permitFilePath,
                                CompleteCallback onComplete) {
    if (!m_plugin || !wxFileExists(permitFilePath)) {
        if (onComplete)
            onComplete(DpS63ImportResult::BAD_PERMIT_FILE,
                       _("Permit file not found"));
        return;
    }

    m_plugin->m_apiPermitFileOverride = permitFilePath;
    int rv = m_plugin->ImportCellPermits();
    m_plugin->m_apiPermitFileOverride.Clear();

    DpS63ImportResult result =
        (rv == 0) ? DpS63ImportResult::SUCCESS : DpS63ImportResult::NO_USERPERMIT;
    NotifyStateChanged();
    if (onComplete)
        onComplete(result, rv == 0 ? _("Permits imported")
                                   : _("Userpermit/Installpermit required"));
}

void DpS63API::ImportCells(const wxString& cellSourceDir,
                           ProgressCallback onProgress,
                           CompleteCallback onComplete) {
    (void)onProgress;  // s63_pi drives its own wxProgressDialog during import
    if (!m_plugin || !wxDir::Exists(cellSourceDir)) {
        if (onComplete)
            onComplete(DpS63ImportResult::UNKNOWN_ERROR,
                       _("Cell source directory not found"));
        return;
    }

    m_plugin->m_apiEncRootOverride = cellSourceDir;
    int rv = m_plugin->ImportCells();
    m_plugin->m_apiEncRootOverride.Clear();

    DpS63ImportResult result =
        (rv == 0) ? DpS63ImportResult::SUCCESS : DpS63ImportResult::SENC_BUILD_FAILED;
    NotifyStateChanged();
    if (onComplete)
        onComplete(result, rv == 0 ? _("Cells imported")
                                   : _("Cell import failed"));
}

// ---------------------------------------------------------------------------
//  USB bundle import (single-call orchestration)
// ---------------------------------------------------------------------------

namespace {

// Case-insensitive search for fileName directly under dir. Returns absolute
// path or empty string. (S63 bundles use upper-case names per spec, but USB
// filesystems may have lower-cased them.)
wxString FindFileAt(const wxString& dir, const wxString& fileName) {
    wxDir d(dir);
    if (!d.IsOpened()) return wxString();
    wxString hit;
    if (d.GetFirst(&hit, wxEmptyString, wxDIR_FILES)) {
        do {
            if (hit.IsSameAs(fileName, false))
                return dir + wxFileName::GetPathSeparator() + hit;
        } while (d.GetNext(&hit));
    }
    return wxString();
}

wxString FindDirAt(const wxString& dir, const wxString& dirName) {
    wxDir d(dir);
    if (!d.IsOpened()) return wxString();
    wxString hit;
    if (d.GetFirst(&hit, wxEmptyString, wxDIR_DIRS)) {
        do {
            if (hit.IsSameAs(dirName, false))
                return dir + wxFileName::GetPathSeparator() + hit;
        } while (d.GetNext(&hit));
    }
    return wxString();
}

// Read the first non-empty trimmed line of a text file. Empty string if not
// readable.
wxString ReadFirstNonEmptyLine(const wxString& path) {
    wxTextFile f(path);
    if (!f.Open()) return wxString();
    for (wxString line = f.GetFirstLine(); !f.Eof();
         line = f.GetNextLine()) {
        line.Trim().Trim(false);
        if (!line.IsEmpty()) return line;
    }
    // Last line (the loop above stops before reading the final line if file
    // has no trailing newline).
    wxString last = f.GetLastLine();
    last.Trim().Trim(false);
    return last;
}

// Discover a userpermit + installpermit pair on the USB. Tries (in order)
// sibling USERPERMIT.TXT / INSTALLPERMIT.TXT files, then a KEYS.TXT with two
// lines. Returns true if both values were found.
bool ScanUsbForActivationKeys(const wxString& usbDir,
                              wxString& outUser, wxString& outInstall) {
    outUser.Clear();
    outInstall.Clear();

    wxString userPath = FindFileAt(usbDir, "USERPERMIT.TXT");
    wxString instPath = FindFileAt(usbDir, "INSTALLPERMIT.TXT");
    if (!userPath.IsEmpty() && !instPath.IsEmpty()) {
        outUser = ReadFirstNonEmptyLine(userPath);
        outInstall = ReadFirstNonEmptyLine(instPath);
        return !outUser.IsEmpty() && !outInstall.IsEmpty();
    }

    wxString keysPath = FindFileAt(usbDir, "KEYS.TXT");
    if (!keysPath.IsEmpty()) {
        wxTextFile f(keysPath);
        if (f.Open()) {
            wxArrayString nonEmpty;
            for (wxString line = f.GetFirstLine(); !f.Eof();
                 line = f.GetNextLine()) {
                line.Trim().Trim(false);
                if (!line.IsEmpty()) nonEmpty.Add(line);
            }
            wxString tail = f.GetLastLine();
            tail.Trim().Trim(false);
            if (!tail.IsEmpty()) nonEmpty.Add(tail);
            if (nonEmpty.GetCount() >= 2) {
                outUser = nonEmpty[0];
                outInstall = nonEmpty[1];
                return true;
            }
        }
    }
    return false;
}

// Count cell-permit lines under :ENC sections in a PERMIT.TXT. Used to size
// the post-import diff.
std::vector<wxString> ListCellNamesInPermitFile(const wxString& permitFile) {
    std::vector<wxString> names;
    wxTextFile f(permitFile);
    if (!f.Open()) return names;
    bool inEnc = false;
    for (wxString line = f.GetFirstLine(); !f.Eof();
         line = f.GetNextLine()) {
        if (line.StartsWith(":ENC")) { inEnc = true; continue; }
        if (line.StartsWith(":"))    { inEnc = false; continue; }
        if (!inEnc) continue;
        wxString s = line; s.Trim().Trim(false);
        if (s.Len() >= 8) names.push_back(s.Mid(0, 8));
    }
    return names;
}

}  // namespace

void DpS63API::ImportFromUsb(const wxString& usbMountPath,
                             ImportProgressCallback onProgress,
                             ImportCompleteCallback onComplete) {
    auto report = [&](DpS63ImportResult r, const wxString& summary,
                      int added, int updated, int failed) {
        if (onComplete) onComplete(r, summary, added, updated, failed);
    };
    auto stage = [&](int pct, const wxString& msg) {
        if (onProgress) onProgress(pct, msg);
    };

    if (!m_plugin || usbMountPath.IsEmpty() || !wxDir::Exists(usbMountPath)) {
        report(DpS63ImportResult::UNKNOWN_ERROR,
               _("USB drive not found."), 0, 0, 0);
        return;
    }

    stage(2, _("Scanning USB..."));

    wxString permitFile = FindFileAt(usbMountPath, "PERMIT.TXT");
    wxString encRoot    = FindDirAt(usbMountPath, "ENC_ROOT");
    if (permitFile.IsEmpty() || encRoot.IsEmpty()) {
        report(DpS63ImportResult::BAD_PERMIT_FILE,
               _("USB does not contain a valid S63 chart bundle "
                 "(PERMIT.TXT and ENC_ROOT/ are required)."),
               0, 0, 0);
        return;
    }

    //  1. Pick up activation keys from the bundle if a fresh pair is offered.
    //     This is the path that lets the user move between chart authorities
    //     without ever typing a hex string.
    stage(8, _("Reading activation keys..."));
    {
        wxString user, inst;
        if (ScanUsbForActivationKeys(usbMountPath, user, inst)) {
            if (user != g_userpermit)    SetUserpermit(user);
            if (inst != g_installpermit) SetInstallpermit(inst);
        }
    }

    //  2. Import any loose .PUB certificates shipped alongside the bundle so
    //     cell signatures can authenticate. Failures here are logged but not
    //     fatal -- the IHO default certificate covers most bundles.
    stage(15, _("Importing certificates..."));
    {
        wxArrayString pubs;
        wxDir::GetAllFiles(usbMountPath, &pubs, "*.PUB", wxDIR_FILES);
        for (const wxString& pub : pubs) ImportCertificate(pub);
    }

    //  Count cells present before this import so we can compute added vs
    //  updated after the permit-write step completes.
    std::vector<wxString> cellsBefore;
    for (const DpS63CellInfo& c : GetInstalledCells())
        cellsBefore.push_back(c.cellName);

    //  3. Validate and persist permits. The plugin's ImportCellPermits writes
    //     .os63 metadata under <ChartDir>/<DataServerID>/.
    stage(30, _("Validating permits..."));
    m_plugin->m_apiPermitFileOverride = permitFile;
    int permitRv = m_plugin->ImportCellPermits();
    m_plugin->m_apiPermitFileOverride.Clear();
    if (permitRv != 0) {
        NotifyStateChanged();
        report(DpS63ImportResult::NO_USERPERMIT,
               _("Activation keys are missing or invalid. "
                 "Make sure the USB carries the keys for this device."),
               0, 0, 0);
        return;
    }

    //  4. Decrypt cells. OCPNsenc is invoked per cell base + update; the
    //     plugin handles authentication against publisher certificates.
    stage(60, _("Decrypting cells..."));
    m_plugin->m_apiEncRootOverride = encRoot;
    int cellRv = m_plugin->ImportCells();
    m_plugin->m_apiEncRootOverride.Clear();

    NotifyStateChanged();
    stage(100, _("Finishing..."));

    //  Diff before/after to break the result down into added / updated and
    //  classify any cell permits in PERMIT.TXT that did not land as failed.
    std::vector<wxString> permitCells = ListCellNamesInPermitFile(permitFile);
    std::vector<wxString> cellsAfter;
    for (const DpS63CellInfo& c : GetInstalledCells())
        cellsAfter.push_back(c.cellName);

    auto contains = [](const std::vector<wxString>& v, const wxString& s) {
        for (const auto& x : v) if (x.IsSameAs(s, false)) return true;
        return false;
    };

    int added = 0, updated = 0, failed = 0;
    for (const wxString& name : permitCells) {
        bool wasBefore = contains(cellsBefore, name);
        bool isAfter   = contains(cellsAfter, name);
        if (isAfter && !wasBefore) ++added;
        else if (isAfter && wasBefore) ++updated;
        else ++failed;
    }

    DpS63ImportResult result;
    wxString summary;
    if (cellRv != 0 || failed > 0) {
        result = (added + updated > 0)
                     ? DpS63ImportResult::PARTIAL
                     : DpS63ImportResult::SENC_BUILD_FAILED;
        summary = wxString::Format(
            _("Imported %d new, updated %d. %d failed -- see log for details."),
            added, updated, failed);
    } else {
        result = DpS63ImportResult::SUCCESS;
        if (added > 0 && updated > 0)
            summary = wxString::Format(
                _("Imported %d new charts, updated %d."), added, updated);
        else if (added > 0)
            summary = wxString::Format(_("Imported %d new charts."), added);
        else if (updated > 0)
            summary = wxString::Format(_("Updated %d charts."), updated);
        else
            summary = _("No new charts to import.");
    }
    report(result, summary, added, updated, failed);
}

// ---------------------------------------------------------------------------
//  State-change notification
// ---------------------------------------------------------------------------

uint64_t DpS63API::AddStateChangedCallback(std::function<void()> callback) {
    uint64_t id = m_nextCallbackId++;
    m_stateCallbacks[id] = std::move(callback);
    return id;
}

void DpS63API::RemoveStateChangedCallback(uint64_t callbackId) {
    m_stateCallbacks.erase(callbackId);
}

void DpS63API::NotifyStateChanged() {
    for (auto& kv : m_stateCallbacks) {
        if (kv.second) kv.second();
    }
}

}  // namespace DpS63
