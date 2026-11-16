// SPDX-License-Identifier: 0BSD
#pragma once

// Publish a fully written temporary record while retaining a recoverable old
// copy. If power is lost between renames, readers fall back to path + ".bak".
template<class Filesystem, class Path>
bool replaceRecord(Filesystem& fs, const Path& temporary, const Path& path, const Path& backup) {
  if (fs.exists(path)) {
    if (fs.exists(backup) && !fs.remove(backup)) return false;
    if (!fs.rename(path, backup)) return false;
  }
  if (!fs.rename(temporary, path)) {
    if (!fs.exists(path) && fs.exists(backup)) fs.rename(backup, path);
    return false;
  }
  if (fs.exists(backup)) fs.remove(backup);
  return true;
}

template<class Job>
bool enqueueLatest(const Job& active, Job& pending, const Job& requested, bool replacePending) {
  if (!replacePending && (active.id || pending.id)) return false;
  pending = requested;
  return true;
}
template<class Job>
bool startPending(Job& active, Job& pending, Job& result) {
  if (active.id || !pending.id) return false;
  active = pending; pending = Job{}; result = active; return true;
}

inline const char* savedPassword(const char* current, const char* replacement, bool clear) {
  return clear ? "" : (*replacement ? replacement : current);
}

// A settings group occupies one NVS blob, so there is no partially updated
// group. The caller changes its RAM snapshot only when this returns true.
template<class Preferences, class Record>
bool persistSettings(Preferences& prefs, const char* key, const Record& record) {
  bool ok = prefs.begin("cfg", false);
  if (ok) ok = prefs.putBytes(key, &record, sizeof(record)) == sizeof(record);
  prefs.end();
  return ok;
}
