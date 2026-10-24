/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include "kdeconnectcore_export.h"

namespace WindowsService
{
KDECONNECTCORE_EXPORT bool registeredForCurrentUser();
KDECONNECTCORE_EXPORT bool removalInProgressForCurrentUser();
KDECONNECTCORE_EXPORT int configure(bool install);
}
