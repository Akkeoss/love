# love_libretro — LÖVE as a libretro core.
#
# Included from the top-level CMakeLists.txt when LOVE_LIBRETRO=ON. It is off by
# default, so a plain LÖVE build is bit-for-bit what it was before this port
# existed — that is the non-regression guarantee, enforced by CMake rather than
# by us remembering.
#
#   cmake -B build -DLOVE_LIBRETRO=ON
#   cmake --build build --target love_libretro
#
# The core is one self-contained .so. It links LÖVE's static module libraries
# directly rather than against liblove.so, so there is a single file to drop into
# a frontend's cores directory — which is what RetroArch and Recalbox expect.

add_library(love_libretro SHARED
	libretro_core.cpp
	libretro_boot.cpp
	libretro_input.cpp
	libretro_options.cpp
	libretro_audio.cpp
	libretro_state.h

	# love.cpp is not part of any love_* static library -- it belongs to the
	# liblove target, and it is what defines luaopen_love and the embedded Lua
	# scripts (boot.lua, callbacks.lua, ...). Without it the module libraries link
	# but nothing can open them, so the core has to compile it in too.
	${CMAKE_SOURCE_DIR}/src/modules/love/love.cpp
)

target_include_directories(love_libretro PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include   # libretro.h
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/modules
	${CMAKE_SOURCE_DIR}/src/libraries
)

# Compiled into every LÖVE translation unit, not just ours: the backends in
# src/modules/*/libretro/ are selected by #ifdef on this, so it has to be visible
# when those files are compiled too. The top-level CMakeLists adds it globally
# when LOVE_LIBRETRO is on.
target_compile_definitions(love_libretro PRIVATE LOVE_ENABLE_LIBRETRO)

# The core links LOVE's static archives directly, and must be self-contained:
# liblove.so is NOT an option, despite being the obvious one. It is built with
# hidden visibility and is not standalone -- `ldd -r` on it reports 526
# undefined symbols, which the love executable supplies at link time. dlopen()ing
# something that depends on it therefore fails.
#
# Linking the archives has its own trap: they reference each other in both
# directions (love_common holds the base classes every module derives from, and
# the modules register themselves back). A normal link drops any archive member
# it has not yet seen referenced, which shows up at dlopen() as an undefined
# typeinfo with no hint that a member was pruned. --whole-archive keeps them all.
#
# The 3rd-party archives (LIBLOVE_LIBRARIES) do not need this: nothing depends on
# them being whole, and forcing them in would bloat the core with dead code.
# Expand LIBLOVE_DEPENDENCIES to the STATIC archives behind it.
#
# Most entries are INTERFACE libraries (love_physics is an INTERFACE over
# love_physics_root + love_physics_box2d). WHOLE_ARCHIVE does not see through an
# INTERFACE: it wraps the four entries that happen to be concrete
# (common/data/math/timer) and leaves the archives behind the interfaces to be
# pulled in later, unwrapped -- so their unreferenced members get pruned and
# love::physics::Body's typeinfo goes missing at dlopen time. The link line shows
# it plainly: --whole-archive around four archives, everything else after it.
#
# So resolve the interfaces ourselves and hand WHOLE_ARCHIVE only real archives.
set(LOVE_LIBRETRO_ARCHIVES "")
foreach(dep IN LISTS LIBLOVE_DEPENDENCIES)
	get_target_property(dep_type ${dep} TYPE)
	if(dep_type STREQUAL "INTERFACE_LIBRARY")
		get_target_property(sub ${dep} INTERFACE_LINK_LIBRARIES)
		foreach(s IN LISTS sub)
			if(TARGET ${s})
				list(APPEND LOVE_LIBRETRO_ARCHIVES ${s})
			endif()
		endforeach()
	else()
		list(APPEND LOVE_LIBRETRO_ARCHIVES ${dep})
	endif()
endforeach()
list(REMOVE_DUPLICATES LOVE_LIBRETRO_ARCHIVES)

# The modules register themselves and derive from base classes in love_common, so
# every member has to survive the link even when nothing visibly references it.
# The 3rd-party archives are linked normally: nothing needs them whole, and
# forcing them in would drag dead code into the core.
target_link_libraries(love_libretro PRIVATE
	"$<LINK_LIBRARY:WHOLE_ARCHIVE,${LOVE_LIBRETRO_ARCHIVES}>"
	${LIBLOVE_LIBRARIES}
)

# Note there is deliberately no C/CXX_VISIBILITY_PRESET hidden here, even though
# liblove sets it and it looks like the obvious thing to copy.
#
# Hidden visibility demotes weak symbols -- vtables, typeinfo, inline template
# instantiations -- to local. LOVE's archives share exactly those across module
# boundaries (love::physics::Body's typeinfo, b2ContactFilter's, glad's function
# pointers), so hiding them leaves ~460 unresolved symbols and the core will not
# dlopen at all. liblove gets away with it only because the love executable links
# it statically and resolves them at link time; a core has no such partner.
#
# The version script below is what actually controls the exported surface, and it
# does that job properly: it hides everything except retro_*, without changing how
# symbols resolve inside the object.
set_target_properties(love_libretro PROPERTIES
	PREFIX ""                       # RetroArch wants love_libretro.so, not liblove_libretro.so
	OUTPUT_NAME "love_libretro"
)

if(UNIX AND NOT APPLE AND LOVE_LIBRETRO_VERSION_SCRIPT)
	target_link_options(love_libretro PRIVATE
		"-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T"
	)
endif()
