// installer: the app installs and uninstalls itself (no scripts: PowerShell/.cmd installers look like malware
// droppers to antivirus heuristics). The download ships it as InstallOpal.exe; it is installed as Opal.exe.
//   - run from anywhere but Program Files: the "Install Opal" dialog (Start menu / desktop shortcut checkboxes), then
//     relaunches itself elevated with --install <dir> [--start-menu] [--desktop]
//   - --install <dir>: (elevated) stops the installed Opal, copies <dir> to %ProgramFiles%\Opal (this exe as Opal.exe),
//     registers the virtual camera DLL (calls its DllRegisterServer), adds/removes the all-users shortcuts
//   - tray menu "Uninstall Opal...": relaunches elevated with --uninstall (also removes the shortcuts)
#pragma once
#include <string>

namespace opal {

std::wstring installDir();
bool runningFromInstallDir();
// handles --install / --uninstall; returns true if this process was such a helper run (exitCode set)
bool handleInstallerArgs(const wchar_t* cmdLine, int& exitCode);
// interactive: offer to install when run from a download folder. Returns true if the caller should exit.
bool offerInstall();
void startUninstall();
// One-time USB setup: Opal's camera functions are composite, and Windows' generic WinUSB binding for its control
// interface registers no device interface an app can open. Writes DeviceInterfaceGUIDs (the Myriad X GUID) for that
// interface and restarts it (elevated helper, --usb-fix <device instance id>). Returns true if it worked.
bool fixUsbInterface(const std::string& instanceId);

}  // namespace opal
