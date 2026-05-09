# Code-signing strategy

FirstContact ships native binaries on three platforms. Code-signing is
required on Windows and macOS to clear the OS gatekeepers; Linux doesn't
need it.

## Linux (AppImage)

No code-signing required. AppImages are unsigned by default and run on
any glibc-compatible distribution. Optional refinements:

- **Detached signature** via `gpg --detach-sign FirstContact-*.AppImage`,
  publishing the matching `.AppImage.asc` next to it on the release page.
  Power users who care about provenance can verify; the OS does not
  enforce it.
- **AppImageUpdate (zsync)** delta updates. Independent of signing;
  worth doing once we have a release cadence.

CI builds the AppImage on tagged releases (see `ci.yml::linux-release-appimage`).

## Windows (MSI)

**Required for "no SmartScreen warning"**: an Authenticode certificate.

**Cost reality:**

| Cert type | Source | Cost (2026) | What it gives |
|---|---|---|---|
| OV (Organization Validated) | DigiCert / Sectigo / SSL.com | ~$200/yr | Signed binary; SmartScreen warns until reputation builds (typically ~3000 downloads or ~30 days post-sign) |
| EV (Extended Validation) | DigiCert / Sectigo | ~$300-700/yr | Signed binary; SmartScreen reputation immediate; requires hardware token (USB HSM) |

**Until a cert is in place:**

- Ship unsigned MSIs. Windows shows "Microsoft Defender SmartScreen prevented
  an unrecognised app from starting" with a "More info" → "Run anyway"
  workflow. Document this in the README install section.
- Do NOT use `signtool` with a self-signed cert; it's worse than unsigned
  (users get "Unknown Publisher" warnings AND a fake-looking signature).
- The MSI itself can still be built and shipped via WiX
  (`packaging/windows/installer.wxs`); the unsigned warning is a UX
  friction, not a blocker.

**When the cert lands**, the CI job needs:

1. The `.pfx` (or HSM session for EV) imported into the runner's certificate
   store. GitHub Actions secrets carry the password; the cert itself goes
   into a private repository or encrypted secret blob.
2. A `signtool sign /tr http://timestamp.digicert.com /td sha256 /fd sha256`
   step after `cmake --install`.
3. SHA-256 hashing because SHA-1 is deprecated and SmartScreen rejects it.
4. Timestamp server is not optional — without it, the signature expires
   when the cert expires, breaking already-distributed binaries.

## macOS (DMG)

**Required for "no Gatekeeper warning"**: an Apple Developer ID
certificate AND notarisation.

**Cost reality:**

- **Apple Developer Program**: $99/yr. Required for the cert.
- **Developer ID Application** certificate: free with the Developer Program
  subscription; can issue up to 5 active certs.
- **Notarisation**: free, but Apple's process is mandatory since macOS
  Catalina. Submission via `notarytool`; Apple scans the binary for known
  malware, returns approval (typically <5 min), at which point the binary
  is "stapled" so Gatekeeper trusts it offline.

**Until a cert is in place:**

- Ship unsigned + un-notarised DMGs. macOS Sonoma/Sequoia will refuse to
  open them by default; users must `xattr -cr FirstContact.app` or
  right-click → Open. This is a noticeable friction.
- An ad-hoc signature (`codesign -s -`) does NOT bypass Gatekeeper but
  does satisfy some hardened-runtime checks; harmless if applied.

**When the cert lands**, the CI job needs:

1. The `.p12` cert + private key imported via the
   `apple-actions/import-codesign-certs@v3` action (or manual `security
   import`).
2. `codesign --deep --options runtime --sign "Developer ID Application: …"`
   over the `.app` bundle.
3. `notarytool submit FirstContact.dmg --apple-id … --team-id … --password …
   --wait` followed by `xcrun stapler staple FirstContact.dmg`.
4. The Apple ID app-specific password and Team ID stored as GitHub Actions
   secrets.

## Decision

For Phase 4 v1.0:

- **Linux**: ship signed-by-GPG AppImages. Cost: $0.
- **Windows**: ship UNSIGNED MSIs. Document the SmartScreen friction.
  Defer paid cert until v1.1 once we have user demand justifying the
  recurring spend.
- **macOS**: ship UNSIGNED + un-notarised DMGs OR skip macOS for v1.0.
  The Gatekeeper friction is worse than Windows SmartScreen — users
  literally can't run the app without manual `xattr` intervention.
  Recommend deferring macOS packaging until after Phase 4 v1.0 unless we
  budget the $99/yr Developer ID upfront.

## Open questions to revisit pre-v1.0

- Do we have access to an organisational entity (LLC / S-Corp) that can
  hold the Authenticode OV cert? Personal certs work too but the
  "Publisher" line on the install dialog reads `<your legal name>`,
  which is friction for a privacy-focused app.
- Is the AGPL-3.0 license compatible with Apple's Developer Program
  agreement? (Yes — AGPL doesn't restrict distribution channel; Apple's
  agreement only requires that we own/have rights to distribute the
  binary, which we do.)
- Auto-update channel: do we ship updates via AppImageUpdate / Squirrel.Windows
  / Sparkle? Each requires a working signature — auto-update without
  signing is a malware delivery vector.
