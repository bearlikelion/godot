/**************************************************************************/
/*  register_types.h                                                      */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#ifndef GDCRASHCATCH_REGISTER_TYPES_H
#define GDCRASHCATCH_REGISTER_TYPES_H

#include "modules/register_module_types.h"

void initialize_gdcrashcatch_module(ModuleInitializationLevel p_level);
void uninitialize_gdcrashcatch_module(ModuleInitializationLevel p_level);

#endif // GDCRASHCATCH_REGISTER_TYPES_H
