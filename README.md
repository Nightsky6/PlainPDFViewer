# PlainPDFViewer

A lightweight Windows PDF viewer (Win32, C++, PDFium).

- License: AGPL-3.0 (see `LICENSE`)
- Third-party components: see `THIRD_PARTY_NOTICES.md`
- Source: https://github.com/Nightsky6/PlainPDFViewer

## PDF compression

PDF compression uses Ghostscript 10.08.0, licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).

Corresponding source code:  
https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/tag/gs10080

To enable PDF compression, place the appropriate Ghostscript DLL and required Ghostscript resource files next to the application, or install Ghostscript separately.

Official Ghostscript downloads:  
https://ghostscript.com/releases/gsdnld.html

## Build

Requirements: Visual Studio 2022 or later with the "Desktop development with C++" workload.

1. Clone the repository.
2. Download the PDFium Windows binaries from:

   https://github.com/bblanchon/pdfium-binaries/releases

   Use the appropriate package for your target platform:

   - `pdfium-win-x64`
   - `pdfium-win-x86`
   - `pdfium-win-arm64`

   The PDFium version currently used by this project is **151.0.7906.0**.  
   See `third_party/pdfium/VERSION`.

3. Copy `pdfium.dll` to the appropriate location:

   | Platform | DLL location |
   |---|---|
   | x64 | `third_party\pdfium\bin\pdfium.dll` |
   | x86 | `third_party\pdfium\x86\bin\pdfium.dll` |
   | ARM64 | `third_party\pdfium\arm64\bin\pdfium.dll` |

   The PDFium headers and the x64 `lib/pdfium.dll.lib` are already included.

   For x86 and ARM64 builds, also place the matching `pdfium.dll.lib` in:

   - `third_party\pdfium\x86\lib`
   - `third_party\pdfium\arm64\lib`

4. Optional: To build with PDF compression support, place the appropriate Ghostscript DLL in:

   - x64: `third_party\ghostscript\x64\gsdll64.dll`
   - x86: `third_party\ghostscript\x86\gsdll32.dll`

5. Open `PlainPDFViewer.sln` in Visual Studio and build the desired configuration.

## Platform notes

PDF compression is currently available for x64 and x86 builds.

PDF compression is not available in the ARM64 build because this project does not currently provide or bundle an ARM64 Ghostscript DLL.

## Contact and support

- Email: nightskyworks@gmail.com
- Issues: https://github.com/Nightsky6/PlainPDFViewer/issues
