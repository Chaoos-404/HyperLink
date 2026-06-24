# WinGet Manifest

This directory contains the Windows Package Manager manifests for Hyperlink.

The manifest identifier is:

```text
Chaoos404.Hyperlink
```

To test locally on Windows:

```powershell
winget install --manifest .\packaging\winget\Manifests\c\Chaoos404\Hyperlink\0.1.1
```

To publish publicly, submit these files to the `microsoft/winget-pkgs` repository under
the same `Manifests/c/Chaoos404/Hyperlink/0.1.1/` path. After Microsoft accepts the PR,
users can install with:

```powershell
winget install Chaoos404.Hyperlink
```
