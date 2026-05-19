/******************************************************************************
 *
 * Project:  OpenCPN / Deeprey
 * Purpose:  S63 device-identity store
 *
 * Owns the on-disk identity store under <g_CommonDataDir>/identity/:
 *
 *     fingerprint.fpr   activation file written by "OCPNsenc -w -o"
 *     DEVICE_ID.txt     single-line, human-readable handle
 *     provisioned.json  sentinel so subsequent boots are a no-op
 *
 * Provisioning runs once on first plugin Init. The device ID is the first
 * 16 hex characters of SHA-1 over the activation file's bytes, formatted
 * "XXXX-XXXX-XXXX-XXXX".
 *
 ***************************************************************************
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************
 */

#pragma once

#include <wx/string.h>

namespace DpS63 {

class DpS63Identity {
public:
    // Run provisioning if it hasn't yet succeeded. Idempotent: cheap on
    // subsequent calls. Returns true if the identity store is in a usable
    // state after the call (fingerprint.fpr + device ID both available).
    static bool EnsureProvisioned();

    // Cached device handle ("XXXX-XXXX-XXXX-XXXX"). Empty if provisioning
    // has not yet run successfully on this device.
    static wxString GetDeviceId();

    // Absolute path to the activation file in the identity store. Valid
    // only after EnsureProvisioned() has returned true.
    static wxString GetFingerprintPath();

    // Identity-store directory: <g_CommonDataDir>/identity/ with trailing
    // separator. Created if missing.
    static wxString GetIdentityDir();
};

}  // namespace DpS63
