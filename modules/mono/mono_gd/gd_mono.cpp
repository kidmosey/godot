/*************************************************************************/
/*  gd_mono.cpp                                                          */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "gd_mono.h"

#include "core/config/project_settings.h"
#include "core/debugger/engine_debugger.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#include "../csharp_script.h"
#include "../glue/runtime_interop.h"
#include "../godotsharp_dirs.h"
#include "../utils/path_utils.h"
#include "gd_mono_cache.h"

#ifdef TOOLS_ENABLED
#include <nethost.h>
#endif

#include <coreclr_delegates.h>
#include <hostfxr.h>
#ifdef UNIX_ENABLED
#include <dlfcn.h>
#endif

// TODO mobile
#if 0
#ifdef ANDROID_ENABLED
#include "support/android_support.h"
#elif defined(IOS_ENABLED)
#include "support/ios_support.h"
#endif
#endif

GDMono *GDMono::singleton = nullptr;

namespace {
hostfxr_initialize_for_dotnet_command_line_fn hostfxr_initialize_for_dotnet_command_line = nullptr;
hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;

#ifdef _WIN32
static_assert(sizeof(char_t) == sizeof(char16_t));
using HostFxrCharString = Char16String;
#define HOSTFXR_STR(m_str) L##m_str
#else
static_assert(sizeof(char_t) == sizeof(char));
using HostFxrCharString = CharString;
#define HOSTFXR_STR(m_str) m_str
#endif

HostFxrCharString str_to_hostfxr(const String &p_str) {
#ifdef _WIN32
	return p_str.utf16();
#else
	return p_str.utf8();
#endif
}

#ifdef TOOLS_ENABLED
String str_from_hostfxr(const char_t *p_buffer) {
#ifdef _WIN32
	return String::utf16((const char16_t *)p_buffer);
#else
	return String::utf8((const char *)p_buffer);
#endif
}
#endif

const char_t *get_data(const HostFxrCharString &p_char_str) {
	return (const char_t *)p_char_str.get_data();
}

#ifdef TOOLS_ENABLED
String find_hostfxr(size_t p_known_buffer_size, get_hostfxr_parameters *p_get_hostfxr_params) {
	// Pre-allocate a large buffer for the path to hostfxr
	Vector<char_t> buffer;
	buffer.resize(p_known_buffer_size);

	int rc = get_hostfxr_path(buffer.ptrw(), &p_known_buffer_size, p_get_hostfxr_params);

	ERR_FAIL_COND_V_MSG(rc != 0, String(), "get_hostfxr_path failed with code: " + itos(rc));

	return str_from_hostfxr(buffer.ptr());
}
#endif

String find_hostfxr() {
#ifdef TOOLS_ENABLED
	const int CoreHostLibMissingFailure = 0x80008083;
	const int HostApiBufferTooSmall = 0x80008098;

	size_t buffer_size = 0;
	int rc = get_hostfxr_path(nullptr, &buffer_size, nullptr);

	if (rc == HostApiBufferTooSmall) {
		return find_hostfxr(buffer_size, nullptr);
	}

	if (rc == CoreHostLibMissingFailure) {
		// Apparently `get_hostfxr_path` doesn't look for dotnet in `PATH`? (I suppose it needs the
		// `DOTNET_ROOT` environment variable). If it fails, we try to find the dotnet executable
		// in `PATH` ourselves and pass its location as `dotnet_root` to `get_hostfxr_path`.
		String dotnet_exe = path::find_executable("dotnet");

		if (!dotnet_exe.is_empty()) {
			// The file found in PATH may be a symlink
			dotnet_exe = path::abspath(path::realpath(dotnet_exe));

			// TODO:
			// Sometimes, the symlink may not point to the dotnet executable in the dotnet root.
			// That's the case with snaps. The snap install should have been found with the
			// previous `get_hostfxr_path`, but it would still be better to do this properly
			// and use something like `dotnet --list-sdks/runtimes` to find the actual location.
			// This way we could also check if the proper sdk or runtime is installed. This would
			// allow us to fail gracefully and show some helpful information in the editor.

			HostFxrCharString dotnet_root = str_to_hostfxr(dotnet_exe.get_base_dir());

			get_hostfxr_parameters get_hostfxr_parameters = {
				sizeof(get_hostfxr_parameters),
				nullptr,
				get_data(dotnet_root)
			};

			buffer_size = 0;
			rc = get_hostfxr_path(nullptr, &buffer_size, &get_hostfxr_parameters);
			if (rc == HostApiBufferTooSmall) {
				return find_hostfxr(buffer_size, &get_hostfxr_parameters);
			}
		}
	}

	if (rc == CoreHostLibMissingFailure) {
		ERR_PRINT(String() + ".NET: One of the dependent libraries is missing. " +
				"Typically when the `hostfxr`, `hostpolicy` or `coreclr` dynamic " +
				"libraries are not present in the expected locations.");
	}

	return String();
#else

#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("hostfxr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libhostfxr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libhostfxr.so");
#else
#error "Platform not supported (yet?)"
#endif

	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}

	return String();

#endif
}

bool load_hostfxr(void *&r_hostfxr_dll_handle) {
	String hostfxr_path = find_hostfxr();

	if (hostfxr_path.is_empty()) {
		return false;
	}

	print_verbose("Found hostfxr: " + hostfxr_path);

	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, r_hostfxr_dll_handle);

	if (err != OK) {
		return false;
	}

	void *lib = r_hostfxr_dll_handle;

	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_dotnet_command_line", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_dotnet_command_line = (hostfxr_initialize_for_dotnet_command_line_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_close = (hostfxr_close_fn)symbol;

	return (hostfxr_initialize_for_runtime_config &&
			hostfxr_get_runtime_delegate &&
			hostfxr_close);
}

#ifdef TOOLS_ENABLED
load_assembly_and_get_function_pointer_fn initialize_hostfxr_for_config(const char_t *p_config_path) {
	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(p_config_path, nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_runtime_config failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;

	rc = hostfxr_get_runtime_delegate(cxt,
			hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);

	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#else
load_assembly_and_get_function_pointer_fn initialize_hostfxr_self_contained(
		const char_t *p_main_assembly_path) {
	hostfxr_handle cxt = nullptr;

	List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();

	List<HostFxrCharString> argv_store;
	Vector<const char_t *> argv;
	argv.resize(cmdline_args.size() + 1);

	argv.write[0] = p_main_assembly_path;

	int i = 1;
	for (const String &E : cmdline_args) {
		HostFxrCharString &stored = argv_store.push_back(str_to_hostfxr(E))->get();
		argv.write[i] = get_data(stored);
		i++;
	}

	int rc = hostfxr_initialize_for_dotnet_command_line(argv.size(), argv.ptrw(), nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_dotnet_command_line failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;

	rc = hostfxr_get_runtime_delegate(cxt,
			hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);

	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#endif

#ifdef TOOLS_ENABLED
using godot_plugins_initialize_fn = bool (*)(void *, bool, gdmono::PluginCallbacks *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#else
using godot_plugins_initialize_fn = bool (*)(void *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#endif

#ifdef TOOLS_ENABLED
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	HostFxrCharString godot_plugins_path = str_to_hostfxr(
			GodotSharpDirs::get_api_assemblies_dir().path_join("GodotPlugins.dll"));

	HostFxrCharString config_path = str_to_hostfxr(
			GodotSharpDirs::get_api_assemblies_dir().path_join("GodotPlugins.runtimeconfig.json"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_for_config(get_data(config_path));
	ERR_FAIL_NULL_V(load_assembly_and_get_function_pointer, nullptr);

	r_runtime_initialized = true;

	print_verbose(".NET: hostfxr initialized");

	int rc = load_assembly_and_get_function_pointer(get_data(godot_plugins_path),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("InitializeFromEngine"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#else
static String get_assembly_name() {
	String assembly_name = ProjectSettings::get_singleton()->get_setting("dotnet/project/assembly_name");

	if (assembly_name.is_empty()) {
		assembly_name = ProjectSettings::get_singleton()->get_safe_project_name();
	}

	return assembly_name;
}

godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	String assembly_name = get_assembly_name();

	HostFxrCharString assembly_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir()
															 .path_join(assembly_name + ".dll"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_self_contained(get_data(assembly_path));
	ERR_FAIL_NULL_V(load_assembly_and_get_function_pointer, nullptr);

	r_runtime_initialized = true;

	print_verbose(".NET: hostfxr initialized");

	int rc = load_assembly_and_get_function_pointer(get_data(assembly_path),
			get_data(str_to_hostfxr("GodotPlugins.Game.Main, " + assembly_name)),
			HOSTFXR_STR("InitializeFromGameProject"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}

godot_plugins_initialize_fn try_load_native_aot_library(void *&r_aot_dll_handle) {
	String assembly_name = get_assembly_name();

#if defined(WINDOWS_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dll");
#elif defined(MACOS_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dylib");
#elif defined(UNIX_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".so");
#else
#error "Platform not supported (yet?)"
#endif

	if (FileAccess::exists(native_aot_so_path)) {
		Error err = OS::get_singleton()->open_dynamic_library(native_aot_so_path, r_aot_dll_handle);

		if (err != OK) {
			return nullptr;
		}

		void *lib = r_aot_dll_handle;

		void *symbol = nullptr;

		err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "godotsharp_game_main_init", symbol);
		ERR_FAIL_COND_V(err != OK, nullptr);
		return (godot_plugins_initialize_fn)symbol;
	}

	return nullptr;
}
#endif

} // namespace

static bool _on_core_api_assembly_loaded() {
	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	bool debug;
#ifdef DEBUG_ENABLED
	debug = true;
#else
	debug = false;
#endif

	GDMonoCache::managed_callbacks.GD_OnCoreApiAssemblyLoaded(debug);

	return true;
}

void GDMono::initialize() {
	print_verbose(".NET: Initializing module...");

	_init_godot_api_hashes();

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	if (!load_hostfxr(hostfxr_dll_handle)) {
#if !defined(TOOLS_ENABLED)
		godot_plugins_initialize = try_load_native_aot_library(hostfxr_dll_handle);

		if (godot_plugins_initialize != nullptr) {
			is_native_aot = true;
		} else {
			ERR_FAIL_MSG(".NET: Failed to load hostfxr");
		}
#else
		ERR_FAIL_MSG(".NET: Failed to load hostfxr");
#endif
	}

	if (!is_native_aot) {
		godot_plugins_initialize = initialize_hostfxr_and_godot_plugins(runtime_initialized);
		ERR_FAIL_NULL(godot_plugins_initialize);
	}

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_runtime_interop_funcs(interop_funcs_size);

	GDMonoCache::ManagedCallbacks managed_callbacks{};

	void *godot_dll_handle = nullptr;

#if defined(UNIX_ENABLED) && !defined(MACOS_ENABLED) && !defined(IOS_ENABLED)
	// Managed code can access it on its own on other platforms
	godot_dll_handle = dlopen(nullptr, RTLD_NOW);
#endif

#ifdef TOOLS_ENABLED
	gdmono::PluginCallbacks plugin_callbacks_res;
	bool init_ok = godot_plugins_initialize(godot_dll_handle,
			Engine::get_singleton()->is_editor_hint(),
			&plugin_callbacks_res, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: GodotPlugins initialization failed");

	plugin_callbacks = plugin_callbacks_res;
#else
	bool init_ok = godot_plugins_initialize(godot_dll_handle, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: GodotPlugins initialization failed");
#endif

	GDMonoCache::update_godot_api_cache(managed_callbacks);

	print_verbose(".NET: GodotPlugins initialized");

	_on_core_api_assembly_loaded();
}

#ifdef TOOLS_ENABLED
void GDMono::initialize_load_assemblies() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}

	// Load the project's main assembly. This doesn't necessarily need to succeed.
	// The game may not be using .NET at all, or if the project does use .NET and
	// we're running in the editor, it may just happen to be it wasn't built yet.
	if (!_load_project_assembly()) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			print_error(".NET: Failed to load project assembly");
		}
	}
}
#endif

void GDMono::_init_godot_api_hashes() {
#ifdef DEBUG_METHODS_ENABLED
	get_api_core_hash();

#ifdef TOOLS_ENABLED
	get_api_editor_hash();
#endif // TOOLS_ENABLED
#endif // DEBUG_METHODS_ENABLED
}

#ifdef TOOLS_ENABLED
bool GDMono::_load_project_assembly() {
	String assembly_name = ProjectSettings::get_singleton()->get_setting("dotnet/project/assembly_name");

	if (assembly_name.is_empty()) {
		assembly_name = ProjectSettings::get_singleton()->get_safe_project_name();
	}

	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir()
								   .path_join(assembly_name + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

	if (!FileAccess::exists(assembly_path)) {
		return false;
	}

	String loaded_assembly_path;
	bool success = plugin_callbacks.LoadProjectAssemblyCallback(assembly_path.utf16(), &loaded_assembly_path);

	if (success) {
		project_assembly_path = loaded_assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(loaded_assembly_path);
	}

	return success;
}
#endif

#ifdef GD_MONO_HOT_RELOAD
Error GDMono::reload_project_assemblies() {
	ERR_FAIL_COND_V(!runtime_initialized, ERR_BUG);

	finalizing_scripts_domain = true;

	CSharpLanguage::get_singleton()->_on_scripts_domain_about_to_unload();

	if (!get_plugin_callbacks().UnloadProjectPluginCallback()) {
		ERR_FAIL_V_MSG(Error::FAILED, ".NET: Failed to unload assemblies.");
	}

	finalizing_scripts_domain = false;

	// Load the project's main assembly. Here, during hot-reloading, we do
	// consider failing to load the project's main assembly to be an error.
	if (!_load_project_assembly()) {
		print_error(".NET: Failed to load project assembly.");
		return ERR_CANT_OPEN;
	}

	return OK;
}
#endif

GDMono::GDMono() {
	singleton = this;

	runtime_initialized = false;
	finalizing_scripts_domain = false;

	api_core_hash = 0;
#ifdef TOOLS_ENABLED
	api_editor_hash = 0;
#endif
}

GDMono::~GDMono() {
	finalizing_scripts_domain = true;

	if (is_runtime_initialized()) {
		if (GDMonoCache::godot_api_cache_updated) {
			GDMonoCache::managed_callbacks.DisposablesTracker_OnGodotShuttingDown();
		}
	}

	if (hostfxr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(hostfxr_dll_handle);
	}

	finalizing_scripts_domain = false;
	runtime_initialized = false;

#if defined(ANDROID_ENABLED)
	gdmono::android::support::cleanup();
#endif

	singleton = nullptr;
}

namespace mono_bind {

GodotSharp *GodotSharp::singleton = nullptr;

bool GodotSharp::_is_runtime_initialized() {
	return GDMono::get_singleton() != nullptr && GDMono::get_singleton()->is_runtime_initialized();
}

void GodotSharp::_reload_assemblies(bool p_soft_reload) {
#ifdef GD_MONO_HOT_RELOAD
	CRASH_COND(CSharpLanguage::get_singleton() == nullptr);
	// This method may be called more than once with `call_deferred`, so we need to check
	// again if reloading is needed to avoid reloading multiple times unnecessarily.
	if (CSharpLanguage::get_singleton()->is_assembly_reloading_needed()) {
		CSharpLanguage::get_singleton()->reload_assemblies(p_soft_reload);
	}
#endif
}

void GodotSharp::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_runtime_initialized"), &GodotSharp::_is_runtime_initialized);
	ClassDB::bind_method(D_METHOD("_reload_assemblies"), &GodotSharp::_reload_assemblies);
}

GodotSharp::GodotSharp() {
	singleton = this;
}

GodotSharp::~GodotSharp() {
	singleton = nullptr;
}

} // namespace mono_bind
