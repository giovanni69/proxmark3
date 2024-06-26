//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Low frequency EM4x50 commands
//-----------------------------------------------------------------------------

#ifndef CMDLFEM4X50_H__
#define CMDLFEM4X50_H__

#include "em4x50.h"

int CmdLFEM4X50(const char *Cmd);

int read_em4x50_uid(void);
bool detect_4x50_block(void);
int em4x50_read(em4x50_data_t *etd, em4x50_word_t *out);

#endif
