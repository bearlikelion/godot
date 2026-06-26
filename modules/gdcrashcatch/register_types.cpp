/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                          GDCrashCatch module                           */
/*  Graceful crash capture and deferred upload for the Godot Engine.      */
/*  SPDX-License-Identifier: MIT                                           */
/**************************************************************************/

#include "register_types.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

#include "crash_catch.h"
#include "crash_catch_project_settings.h"

static CrashCatch *crash_catch_ptr = nullptr;

void initialize_gdcrashcatch_module(ModuleInitializationLevel p_level) {
	// SCENE level: OS, ProjectSettings, ScriptServer and the main loop machinery
	// are all available, which the report/upload paths depend on.
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(CrashCatch);

	CrashCatchProjectSettings::register_settings();

	crash_catch_ptr = memnew(CrashCatch);
	Engine::get_singleton()->add_singleton(Engine::Singleton("CrashCatch", CrashCatch::get_singleton()));

	// Do not install handlers or touch reports inside the editor; only in a
	// running game/instance.
	if (!Engine::get_singleton()->is_editor_hint()) {
		CrashCatch::get_singleton()->initialize();
	}
}

void uninitialize_gdcrashcatch_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	if (crash_catch_ptr) {
		crash_catch_ptr->shutdown();
	}
	Engine::get_singleton()->remove_singleton("CrashCatch");
	if (crash_catch_ptr) {
		memdelete(crash_catch_ptr);
		crash_catch_ptr = nullptr;
	}
}
