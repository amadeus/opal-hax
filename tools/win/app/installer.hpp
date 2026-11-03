// installer: Opal.exe installs and uninstalls itself (no scripts: PowerShell/.cmd installers look like malware
// droppers to antivirus heuristics).
//   - run from anywhere but Program Files: asks "Install Opal?", relaunches itself elevated with --install <dir>
//   - --install <dir>: (elevated) stops the installed Opal, copies <dir> to %ProgramFiles%\Opal, registers the
//     virtual camera DLL (calls its DllRegisterServer)
//   - tray menu "Uninstall Opal...": relaunches elevated with --uninstall
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
