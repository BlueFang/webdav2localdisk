; webdav2localdisk installer — Inno Setup script
; Builds a signed (optional) installer that:
;   1. Installs the WinFsp redistributable (bundled).
;   2. Installs webdav2localdisk.exe.
;   3. Installs Visual C++ runtime.
; Usage: iscc installer/webdav2localdisk.iss
; Run only after the Release build has been published to ./Release/.

#define AppName      "webdav2localdisk"
#define AppVersion   "0.1.0"
#define AppPublisher "BlueFang"
#define AppURL       "https://github.com/BlueFang/webdav2localdisk"

[Setup]
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppSourceURL={#AppURL}
DefaultDirName={pf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..\out\installer
OutputBaseFilename=webdav2localdisk-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\webdav2localdisk.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
; WinFsp redistributable. Download from https://winfsp.dev/ and place
; in installer\deps\winfsp-<ver>.msi before building.
Source: "deps\winfsp-*.msi"; DestDir: "{tmp}"; Flags: deleteafterinstall

Source: "..\build\Release\webdav2localdisk.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README_zh.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\PRINCIPLE.md"; DestDir: "{app}\docs"; Flags: ignoreversion

; VC++ redist. Use vcredist_x64.exe matching the MSVC used at build time.
Source: "deps\vcredist_x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Run]
Filename: "msiexec.exe"; Parameters: "/i {tmp}\winfsp-*.msi /qn"; \
  StatusMsg: "Installing WinFsp redistributable..."; Check: NeedsWinFsp
Filename: "{tmp}\vcredist_x64.exe"; Parameters: "/install /quiet /norestart"; \
  StatusMsg: "Installing Visual C++ runtime..."

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\webdav2localdisk.exe"
Name: "{group}\Readme (EN)"; Filename: "{app}\README.md"
Name: "{group}\Readme (中文)"; Filename: "{app}\README_zh.md"
Name: "{group}\Uninstall"; Filename: "{uninstallexe}"

[Code]
function NeedsWinFsp(): Boolean;
var
  ResultCode: Integer;
  RegVal: String;
begin
  Result := True;
  try
    if Exec('sc.exe', 'query WinFsp.Launcher', '', SW_HIDE,
            ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
      Result := False;
  except
  end;
  if Result and
     RegQueryStringValue(HKLM32, 'SOFTWARE\WinFsp', 'InstallDir', RegVal) then
    Result := False;
end;
