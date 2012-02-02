/* Copyright (C) 2009 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"
#include "Font.h"

#include "lib/res/graphics/unifont.h"

#include "ps/ConfigDB.h"
#include "ps/Filesystem.h"
#include "ps/CLogger.h"

#include <map>
#include <string>

const wchar_t* DefaultFont = L"sans-10";

CFont::CFont(const CStrW& name)
{
	h = unifont_load(g_VFS, name);

	// Found it
	if (h > 0)
		return;

	// Not found as a font -- give up and use the default.
	LOGERROR(L"Failed to find font '%ls'", name.c_str());
	h = unifont_load(g_VFS, DefaultFont);
	// Assume this worked
}

CFont::~CFont()
{
	unifont_unload(h);
}

void CFont::Bind(size_t unit)
{
	unifont_bind(h, unit);
}

bool CFont::HasRGB()
{
	return unifont_has_rgb(h);
}

int CFont::GetLineSpacing()
{
	return unifont_linespacing(h);
}

int CFont::GetHeight()
{
	return unifont_height(h);
}

int CFont::GetCharacterWidth(wchar_t c)
{
	return unifont_character_width(h, c);
}

void CFont::CalculateStringSize(const CStrW& string, int& width, int& height)
{
	unifont_stringsize(h, string.c_str(), width, height);
}
