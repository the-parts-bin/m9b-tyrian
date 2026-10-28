/*
* OpenTyrian: A modern cross-platform port of Tyrian
* Copyright (C) The OpenTyrian Development Team
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include "logging.h"

SDL_PRINTF_VARARG_FUNC(1)
void logFatal(SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_CRITICAL, fmt, ap);

	char buffer[4096];
	SDL_vsnprintf(buffer, sizeof(buffer), fmt, ap);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", buffer, NULL);

	va_end(ap);
}
