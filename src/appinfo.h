#ifndef APPINFO_H
#define APPINFO_H

namespace appinfo {

// Keep these in sync with docs/P7 release checklist and lsaero.rc.
inline const char *appName() { return "LocalSend Aero"; }
inline const char *appVersion() { return "0.1.0"; }
inline const char *orgName() { return "LocalSend Aero"; }
inline const char *exeName() { return "lsaero.exe"; }
inline const char *singleInstanceKey() { return "localsend-aero-single-instance"; }

} // namespace appinfo

#endif // APPINFO_H
