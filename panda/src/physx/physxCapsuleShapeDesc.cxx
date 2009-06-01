// Filename: physxCapsuleShapeDesc.cxx
// Created by:  pratt (Apr 7, 2006)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) Carnegie Mellon University.  All rights reserved.
//
// All use of this software is subject to the terms of the revised BSD
// license.  You should have received a copy of this license along
// with this source code in a file named "LICENSE."
//
////////////////////////////////////////////////////////////////////

#ifdef HAVE_PHYSX

#include "physxCapsuleShapeDesc.h"



////////////////////////////////////////////////////////////////////
//     Function : PhysxCapsuleShapeDesc
//       Access : Published
//  Description :
////////////////////////////////////////////////////////////////////
PhysxCapsuleShapeDesc::
PhysxCapsuleShapeDesc()
: PhysxShapeDesc( &nCapsuleShapeDesc ) {

}

#endif // HAVE_PHYSX

