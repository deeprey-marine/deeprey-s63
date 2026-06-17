# S63 charts on the MFD — design

## Purpose

S63 is the IHO standard for encrypted electronic navigational charts. Deeprey distributes S63 cells through o-charts. The plugin should make activating S63 and installing charts on the MFD feel as simple as using an app — and it must never ask the user to type cryptographic strings, copy permit files into the right folder, or interpret SSE error codes.

Activation is **on demand and online**: the user taps a button, the MFD contacts the o-charts chart-shop server, and the device activates itself. Only after activation does the user license individual cells (from a dealer/VAR) and bring them onto the MFD on a USB stick. The model is analogous to an ECDIS: the user permit is bound to this device and cannot be reused on another OpenCPN instance.

This document covers two repos:

- `s63_pi` — the OpenCPN plugin (cryptography, chart rendering, `DpS63API` headless layer)
- `deeprey-gui` — the MFD UI, specifically the `Charts → S63 charts` sub-panel

## What the user sees

Two states of the S63 sub-panel. All other surfaces (chart options, basemap, o-charts oeSENC) are unchanged.

### State 1 — Empty (no S63 charts installed)

The layout walks the user through two steps: activate S63 on this device (online, one tap), then license cells and import them from a USB stick.

```
┌──────────────────────────────────────────────────────────┐
│  S63 encrypted charts                                    │
│  Official ENCs with cryptographic protection.            │
│                                                          │
│  ─── 1. Activate S63 on this device ───                  │
│                                                          │
│  Connect to the internet, then tap:                      │
│  [ Activate S63 ]                                        │
│                                                          │
│  ✓ Activated                                             │
│  Device ID: A1B2-C3D4-E5F6-7890                          │
│                                                          │
│  ─── 2. Add your charts ───                              │
│                                                          │
│  License the S63 cells you need from your dealer, copy   │
│  them onto a USB stick, then insert it and tap:          │
│  [ Import from USB ]                                     │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### State 2 — Charts installed

```
┌──────────────────────────────────────────────────────────┐
│  S63 charts                              [+ Add charts]  │
│  12 installed · 2 expiring soon                          │
│  Device ID: A1B2-C3D4-E5F6-7890                          │
│                                                          │
│  GB502106                                                │
│  o-charts · Expires in 12 days                      ⚠    │
│                                                          │
│  NZ500001                                                │
│  o-charts · Valid until 2027-03-15                  ✓    │
│                                                          │
│  FR201478                                                │
│  o-charts · Expired                                 ✗    │
└──────────────────────────────────────────────────────────┘
```

**Per-row content** (all fields populated from `DpS63CellInfo` today):

- Line 1: cell ID (`cellName`)
- Line 2: vendor badge (`producer` / Data Server ID) · status text (computed from `status` + `expiryDate`)
- Trailing pill: ⚠ Expiring soon (≤30 days, via `DpS63Strings::ExpiringSoon`), ✓ Valid, or ✗ Expired

**Sort order**: Expiring → Valid → Expired. Rows within a group sorted by cell name.

**Header**:

- Counter "{N} installed · {M} expiring soon" computed by `DpS63ChartsPanel` from the cell list.
- Device-ID row exposes the device handle (for support tickets and for distinguishing two MFDs on the same boat).

**Per-row action**: tap row → confirmation dialog *"Remove {cellName}?"* with Remove / Cancel. Remove calls `DpS63API::RemoveCell`. No detail sheet in MVP.

**`+ Add charts`**: triggers the USB import flow. Re-importing a `PERMIT.TXT` containing an updated permit for an installed cell replaces the existing `.os63` — this is the renewal and update path.

Certificate management is handled automatically during import. Import diagnostics are written to `/home/opencpn/.opencpn/opencpn.log`. `userpermit` and `installpermit` are device-internal and never surface in the main UI.

## Device identity

The **Device ID** is the MFD's fleet serial — `MFD-YY-NNNNNN-C`, assigned by the enrollment server and written by `deeprey-provision` to `/state/identity/serial` on first boot (see `deeprey-fleet` / `deeprey-system-config`). This is the identifier agreed with o-charts: it is what S63 activation sends and what the UI shows. The user never types, scans, or copies any cryptographic string.

`DpS63Identity::GetDeviceId()` reads `/state/identity/serial`. On a dev workstation (no STATE partition) it falls back to an FPR-derived handle so the UI still shows a stable identifier: on first `s63_pi::Init()` it invokes `OCPNsenc -w -o <identity-dir>` to write a fingerprint file, takes SHA-1 of its contents (first 16 hex chars, formatted `XXXX-XXXX-XXXX-XXXX`), and caches it via a `provisioned.json` sentinel. That handle is a fallback only — on real hardware the fleet serial always wins.

`DpS63Panel::EvaluateMode()` routes between `Empty` and `Charts` based on whether any `.os63` files exist. `DpS63EmptyPanel` renders the **Empty state** described above and reflects activation status (Activated / Not activated yet) from `DpS63API::GetPermitStatus().hasUserpermit`.

## Online activation — the user handles no crypto at all

Activation is a single button (`Activate S63`, Step 1 of the empty state) backed by `DpS63API::RequestActivation`. The whole exchange runs inside the plugin; the user only needs an internet connection.

1. **Build the request.** `OCPNsenc -j -i "<device-serial>" -o <identity-dir>` produces the activation request from the device's secure module (TPM) and prints the path of the request file; `<device-serial>` is the MFD fleet serial from `/state/identity/serial`. The file contents are the JSON body to send.
2. **Contact the shop.** POST that body to `<shop-base>/shop/en/module/ocpermits/request` with `Content-Type: application/json` (libcurl, 30 s timeout). `<shop-base>` is the `ShopBaseUrl` config key, defaulting to `https://test.o-charts.org`; switch it to `https://o-charts.org` for production. The server replies:

   ```json
   { "up": "4E3E8548A87BE0BB6FD976E23147", "ip": "3946FE3D", "t": "5418accb..." }
   ```

3. **Validate.** `OCPNsenc -k -u <up> -e <ip>` confirms the returned permits are valid *for this device*. An `ERROR` line (e.g. "Install permit invalid for this machine") fails the activation.
4. **Persist.** `up` → `g_userpermit`, `ip` → `g_installpermit`, `t` → `g_activation_token`, written to the plugin config. `NotifyStateChanged()` refreshes the panel to "✓ Activated".

The user permit is bound to this MFD's secure module — it cannot be reused on another OpenCPN instance. On the o-charts side the issued UP / InstallPermit / FPR are recorded against the Deeprey account (`S-63MFD@deeprey.com`) and invoiced monthly per activated device.

`RequestActivation` reports each stage through a progress callback and a single plain-language result (`DpS63ActivationResult`): success, no network, server error, bad response, validation failure, or request-build failure.

## Licensing and importing cells

Activation grants the device its permits; individual cells are licensed separately. The user licenses the S63 cells they want at a dealer/VAR, copies them onto a USB stick, and imports them on the MFD. The same flow installs updates to already-licensed cells.

USB import is **button-triggered**, matching the pattern used elsewhere in `deeprey-gui` (Chart Manager, Routes, Layers, Tracks, Bathymetry, Screenshots).

1. User taps `Import from USB` (empty state Step 2) or `+ Add charts` (charts state). The handler calls `DetectUSBMountPoint()` (in `src/utils/DpGUIDialogs.cpp`), which shells out to `/usr/bin/deeprey-detect-usb` (system tool in `deeprey-system-config`). That tool finds a removable drive and mounts it at the fixed path `/media/deeprey-usb` (tries vfat → exfat → ext4).
2. If no drive is found, show a toast: *"Insert a USB stick first."* — `DpToast`, non-modal, matching the existing S63 panel pattern.
3. The `DpS63API` layer scans the mount for `PERMIT.TXT`, `ENC_ROOT/`, and loose `.PUB` certificates. (A bundle may also carry a `USERPERMIT.TXT` / `INSTALLPERMIT.TXT` / `KEYS.TXT` pair; if present it is picked up, but the device's online activation is the normal source of those permits.)
4. New `.PUB` certificates are imported automatically.
5. Cell permits are validated against the device's permits via `OCPNsenc -d -p <permit> -u <userpermit> -e <installpermit>` and stored as `.os63` metadata files.
6. Each encrypted cell is authenticated against the matching publisher certificate and decrypted with `OCPNsenc -n -i <cell> -o <senc> -u <userpermit> -e <installpermit> -p <cellpermit> -z <pluginpath>`, producing an eSENC.
7. The user sees a progress indicator during the import and a plain-language summary at the end. SSE codes (6, 8, 9, 15, 24, 26, …) are translated into sentences for the user; the raw codes go to `/home/opencpn/.opencpn/opencpn.log` for support to read.

eSENC files are always precomputed during import; no prompt. The button label is `Import from USB` in the empty state and `+ Add charts` in the charts state. Progress feedback uses `wxProgressDialog` with stage messages, matching Chart Manager's import UX.

## Component changes

| Component | Repo | Description |
| --- | --- | --- |
| `s63_pi::Init()` | `s63_pi` | Generate the fingerprint on first run, compute the device ID, write the `provisioned.json` sentinel. Subsequent boots are a no-op. |
| `DpS63API::RequestActivation` | `s63_pi` (deeprey-api) | On-demand online activation: build the request (`OCPNsenc -j`), POST to the chart-shop server (libcurl), parse `{up, ip, t}`, validate (`OCPNsenc -k`), persist permits. Reports staged progress and a `DpS63ActivationResult`. |
| `DpS63API` (import) | `s63_pi` (deeprey-api) | `GetDeviceIdString()`, `GetPermitStatus()`, `ImportFromUsb(path, progress, complete)` (single-call orchestration of cert + permit + cell import), `GetInstalledCells()`, `RemoveCell()`. |
| Config keys | `s63_pi` | `ShopBaseUrl` (default `https://test.o-charts.org`) and `ActivationToken` (the `t` value) persisted in the plugin config alongside `Userpermit` / `Installpermit`. |
| `DpS63Panel::EvaluateMode()` | `deeprey-gui` | Two modes only: `Empty` (no `.os63` files exist) and `Charts` (at least one exists). |
| `DpS63EmptyPanel` | `deeprey-gui` | Renders the two-step Empty state: activate online, then import licensed cells from USB. Shows activation status + device ID. No permit-entry inputs. |
| `DpS63ChartsPanel` | `deeprey-gui` | Header summary (`N installed · M expiring soon`) + device-ID row + `+ Add charts` button. Sort cells: Expiring → Valid → Expired, then by name. Tap row → confirm-and-remove dialog. |
| `DpS63CellCard` | `deeprey-gui` | Render as: cell ID, `{producer} · {status text}` line, trailing pill (✓ Valid / ⚠ Expiring soon / ✗ Expired). Tap surface to fire the row-remove signal. |
| `DpS63AdvancedPanel` | `deeprey-gui` | Support-only expert controls: certificate import/removal and manual chart removal. No activation-key or fingerprint controls — activation is online-only. |

## Out of scope for MVP

- Wireless transfer from phone to MFD (local web upload, cloud relay)
- Direct in-app browsing / purchase of individual S63 cells (cells are licensed at a dealer and arrive on USB)
- Auto-renewal nudges
- Location-aware vendor suggestions
- A vendor catalog of any kind in the chart settings UI

## Testing

- First-boot identity generation writes the fingerprint and `provisioned.json` sentinel; the device ID handle is stable across reboots.
- `Activate S63` on a networked device builds a request, reaches the shop, installs the returned permits, and flips the panel to "✓ Activated". `GetPermitStatus().hasUserpermit` is true afterward. (Full happy path requires the device TPM; the workstation has none.)
- Activation surfaces clear non-cryptographic messages for no-network, server-error, and validation-failure cases.
- Empty state appears when zero `.os63` files exist; Charts state appears otherwise.
- An o-charts cell bundle imports through the USB flow and produces rows with the correct vendor badge.
- Expired permits surface as `Expired` pills with a clear non-cryptographic message.
