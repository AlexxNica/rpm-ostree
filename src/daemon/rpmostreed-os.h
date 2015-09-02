/*
* Copyright (C) 2015 Red Hat, Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#pragma once

#include "rpmostreed-types.h"

#define RPMOSTREED_TYPE_OS   (rpmostreed_os_get_type ())
#define RPMOSTREED_OS(o)     (G_TYPE_CHECK_INSTANCE_CAST ((o), RPMOSTREED_TYPE_OS, RpmostreedOS))
#define RPMOSTREED_IS_OS(o)  (G_TYPE_CHECK_INSTANCE_TYPE ((o), RPMOSTREED_TYPE_OS))

GType             rpmostreed_os_get_type           (void) G_GNUC_CONST;
RPMOSTreeOS *     rpmostreed_os_new                (OstreeSysroot *sysroot,
                                                    const char *name,
                                                    RpmostreedTransactionMonitor *monitor);