// -*- C++ -*-
//*************************************************************************
// DESCRIPTION: Verilator: Configuration Files
//
// Code available from: http://www.veripool.org/verilator
//
// AUTHORS: Wilson Snyder with Paul Wasson, Duane Gabli
//
//*************************************************************************
//
// Copyright 2010-2010 by Wilson Snyder.  This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
//
// Verilator is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//*************************************************************************

#ifndef _V3CONFIG_H_
#define _V3CONFIG_H_ 1
#include "config_build.h"
#include "verilatedos.h"
#include <string>
#include "V3Error.h"

//######################################################################

class V3Config {
public:
    static void addIgnore(V3ErrorCode code, string filename, int min, int max);
    static void applyIgnores(FileLine* filelinep);
};

#endif // Guard