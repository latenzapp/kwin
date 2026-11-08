/*
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/*
 * Environment-variable helpers for the WebRTC backend.
 *
 * Boolean variables are read by value, not presence: launchers often export
 * a variable *empty* -- `export VAR=${VAR:-}` in a shell wrapper, a bare key
 * under docker-compose's `environment:` list, systemd's `Environment=VAR=` --
 * and that must read as off.
 *
 * envFlag() checks the value, so an empty string means "off" like every
 * other falsy spelling.
 */

#include <cstdlib>
#include <cstring>
#include <strings.h> // strcasecmp

namespace KWin::NWebRTCPipeline {

// True only if the variable is set to something that actually reads as on.
// Unset, empty, "0", "false", "no", "off" (any case) are all false.
inline bool envFlag(const char *name)
{
    const char *v = std::getenv(name);
    if (!v || v[0] == '\0') return false;
    if (std::strcmp(v, "0") == 0) return false;
    if (strcasecmp(v, "false") == 0) return false;
    if (strcasecmp(v, "no") == 0) return false;
    if (strcasecmp(v, "off") == 0) return false;
    return true;
}

// Non-empty value of `name`, or nullptr. Use instead of std::getenv() when
// the value is then parsed (atof/atoi), so an empty export doesn't parse as
// a deliberate 0.
inline const char *envValue(const char *name)
{
    const char *v = std::getenv(name);
    return (v && v[0] != '\0') ? v : nullptr;
}

} // namespace KWin::NWebRTCPipeline
