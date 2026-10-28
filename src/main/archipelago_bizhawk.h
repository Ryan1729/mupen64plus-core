/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - archipelago_bizhawk.h                                               *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2026 Ryan1729                                           *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef __ARCHIPELAGO_BIZHAWK_H__
#define __ARCHIPELAGO_BIZHAWK_H__

#define ARCHIPELAGO_BIZHAWK_CORE_VERSION 1

#ifdef M64P_ARCHIPELAGO_BIZHAWK

m64p_error archipelago_bizhawk_start();
void archipelago_bizhawk_stop();

#else

static osal_inline m64p_error archipelago_bizhawk_start()
{
    return M64ERR_INCOMPATIBLE;
}

static osal_inline void archipelago_bizhawk_stop() {
    return;
}

#endif

#endif
